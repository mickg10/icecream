#!/usr/bin/env python3
"""Analyze stage_trace() JSONL output from a P50 (P29V1) remote compile farm.

Reads every "*.jsonl" file produced by services/stage_trace.h (one file per
component/host/pid: "<component>-<host>-<pid>.jsonl") out of a directory
tree, reassembles each wire job's end-to-end timeline, and reports where
time goes between NEW and END.

Correlation model
------------------
Most stages carry a wire-identity correlation key: (assignment_epoch,
wire_job_id), plus logical_job/attempt for retries. These exist at the
scheduler / cdaemon / fdaemon / iceccclient / csidecar layers and are
grouped into one timeline per job below.

Two components stamp a few stages with NO wire identity, by design, because
the correlation key genuinely is not available at that point in the code
without a protocol change (see p50-stagetrace commits perf/st-cache and
perf/st-fstore for the exact call sites and reasoning):

  - fsidecar session_accepted   -- raw socket accept, wire protocol unparsed
  - fsidecar input_committed    -- keyed by (c_store_guid, tu_seq) only
  - fsidecar input_materialized -- keyed by (c_store_guid, tu_seq) only

These are reported separately, grouped by (host, pid) and ordered by time,
rather than being force-fit into a per-job timeline. If a tu_seq happens to
be known out-of-band for a job of interest, use --tu-seq to filter them.

Ninja log join (--ninja-log)
-----------------------------
If a job's output_path_mapped stage_trace event (see fnv1a64_path) is
present, --ninja-log FILE joins ninja's own per-edge timing from a
.ninja_log into the same job's timeline as synthetic "ninja edge_start"/
"edge_end" rows. .ninja_log timestamps are elapsed milliseconds since
ninja's own build-start reference, not absolute time, so this first
estimates one global anchor (median over all matched edges of
corrected(source_prepare_start) - edge.start_ms) and reports how many
edges matched. Only the FNV-1a hash of each edge's output path is ever
read into memory or printed -- e.g. a build producing synthetic objects
"widget.o" and "gadget.o" shows up here only as two opaque hash values,
never those names.

Clock sanity (--check-clocks) and occupancy (--occupancy)
-----------------------------------------------------------
--summary's gap percentiles sort every job's records by corrected
timestamp first, which by construction can never show a negative gap.
--check-clocks instead walks a fixed causal stage order per job (see
CANONICAL_STAGE_ORDER) and flags any cross-host pair that comes out
backwards -- the only way that can happen is a wrong/missing entry in
--offsets for one of the two hosts. --occupancy buckets scheduler
jobbegin_recv/jobdone_recv intervals into a concurrency-over-time
histogram, independent of --job/--summary.

Usage
-----
    analyze.py <trace_dir> [--job EPOCH:WIREID] [--summary] [--top-gaps N]
               [--offsets offsets.json] [--tu-seq SEQ] [--limit N]
               [--ninja-log FILE] [--occupancy] [--occupancy-bucket-ms MS]
               [--check-clocks]

Exit status is always 0 unless the trace directory is unreadable or no
records are found.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import statistics
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from typing import Any, Dict, Iterable, List, Optional, Tuple

UNKNOWN = -1  # mirrors icecc::stagetrace::kUnknown

# Mirrors client/remote.cpp's fnv1a64_path(): same FNV-1a 64-bit constants,
# same 63-bit mask (so the value matches the signed int64_t KV extra the
# client wrote without sign-extension ambiguity). Only ever applied to a
# path read locally from a .ninja_log on this machine -- the hash is what
# travels in stage_trace output and in any printed/joined line below, the
# literal path string never is.
_FNV_OFFSET_BASIS = 1469598103934665603
_FNV_PRIME = 1099511628211
_FNV_MASK64 = 0xFFFFFFFFFFFFFFFF
_SIGN63_MASK = 0x7FFFFFFFFFFFFFFF


def fnv1a64_path(path: str) -> int:
    h = _FNV_OFFSET_BASIS
    for byte in path.encode("utf-8", errors="surrogateescape"):
        h ^= byte
        h = (h * _FNV_PRIME) & _FNV_MASK64
    return h & _SIGN63_MASK


@dataclass
class Record:
    realtime_ns: int
    mono_ns: int
    component: str
    stage: str
    epoch: int
    wire_id: int
    logical_job: int
    attempt: int
    host: str
    pid: int
    tid: int
    kv: Dict[str, Any] = field(default_factory=dict)
    src_file: str = ""

    @property
    def has_wire_identity(self) -> bool:
        return self.epoch != UNKNOWN and self.wire_id != UNKNOWN

    @property
    def job_key(self) -> Tuple[int, int]:
        return (self.epoch, self.wire_id)


def load_records(trace_dir: str) -> List[Record]:
    records: List[Record] = []
    paths = sorted(glob.glob(os.path.join(trace_dir, "**", "*.jsonl"), recursive=True))
    if not paths:
        print(f"warning: no *.jsonl files found under {trace_dir}", file=sys.stderr)
    for path in paths:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            for lineno, line in enumerate(fh, start=1):
                line = line.strip()
                if not line:
                    continue
                try:
                    obj = json.loads(line)
                except json.JSONDecodeError as exc:
                    print(f"warning: {path}:{lineno}: skipping malformed JSON ({exc})",
                          file=sys.stderr)
                    continue
                known = {
                    "realtime_ns", "mono_ns", "component", "stage", "epoch",
                    "wire_id", "logical_job", "attempt", "host", "pid", "tid",
                }
                kv = {k: v for k, v in obj.items() if k not in known}
                try:
                    records.append(Record(
                        realtime_ns=int(obj["realtime_ns"]),
                        mono_ns=int(obj["mono_ns"]),
                        component=str(obj["component"]),
                        stage=str(obj["stage"]),
                        epoch=int(obj.get("epoch", UNKNOWN)),
                        wire_id=int(obj.get("wire_id", UNKNOWN)),
                        logical_job=int(obj.get("logical_job", UNKNOWN)),
                        attempt=int(obj.get("attempt", UNKNOWN)),
                        host=str(obj.get("host", "")),
                        pid=int(obj.get("pid", -1)),
                        tid=int(obj.get("tid", -1)),
                        kv=kv,
                        src_file=path,
                    ))
                except (KeyError, TypeError, ValueError) as exc:
                    print(f"warning: {path}:{lineno}: skipping record missing "
                          f"required field ({exc})", file=sys.stderr)
    return records


def load_offsets(path: Optional[str]) -> Dict[str, int]:
    """Load {host: offset_ns} as produced by offsets.sh.

    offset_ns is added to a host's realtime_ns to align it to the
    reference clock (the host offsets.sh was run from). Hosts absent
    from the map are assumed to already be the reference (offset 0).
    """
    if not path:
        return {}
    with open(path, "r", encoding="utf-8") as fh:
        data = json.load(fh)
    return {str(k): int(v) for k, v in data.items()}


def corrected_ts(rec: Record, offsets: Dict[str, int]) -> int:
    return rec.realtime_ns + offsets.get(rec.host, 0)


# Protocol-level NTP-style clock offset derivation (review item 3).
#
# offsets.sh/load_offsets() above estimate a host's clock skew from an SSH
# round trip -- useful, but a separate measurement from the actual P50
# traffic. The C-side icecream-p50-source-result-v5 trace
# (ICECC_P50_SOURCE_RESULT_TRACE) and the F-side icecream-p50-f-result-v1
# trace (ICECC_P50_F_RESULT_TRACE) each stamp absolute CLOCK_REALTIME wall
# clock samples around the same BEGIN/COMMIT round trip that already
# crosses the wire for every transfer, so the same round trip can be
# treated as its own NTP exchange and used to derive a clock offset without
# any extra probe traffic:
#
#   t1 = c_begin_write_wall_ns   -- C writes BEGIN, timestamped on C's clock
#   t2 = f_begin_recv_wall_ns    -- F receives BEGIN, timestamped on F's clock
#   t3 = wall_clock_ns (F v1)    -- F writes COMMIT, timestamped on F's clock
#   t4 = c_commit_read_wall_ns   -- C reads COMMIT, timestamped on C's clock
#
#   offset = ((t2 - t1) + (t3 - t4)) / 2   -- classic NTP clock offset
#   delay  = (t4 - t1) - (t3 - t2)         -- classic NTP round-trip delay
#   error <= delay / 2
#
# offset is denominated exactly like offsets.sh's output: the amount the F
# host's clock reads ahead of the reference (C1) clock, directly comparable
# to an existing offsets.json entry for the same host. Both traces are
# diagnostic-only and env-gated; neither is required for correct operation,
# and nothing here changes wire bytes or behavior.
#
# t2 is sampled on F immediately after the BEGIN frame read completes, so it
# is F's receipt time for the BEGIN bytes, not F's dispatch time for them.
# In pipelined mode the BEGIN frame can simply sit in F's socket receive
# buffer for a while before F gets around to reading it (F is still busy
# finishing an earlier session's HELLO/STATE exchange) -- that shows up as
# one-way delay, which cancels out of the offset formula and only widens
# the error bound, so the lowest-delay-decile filter in derive_ntp_offsets()
# already handles it without special-casing pipelined mode.
NTP_DISAGREEMENT_WARN_NS = 5_000_000  # 5ms, see review item 3


@dataclass
class SourceResultSample:
    """One parsed icecream-p50-source-result-v5 record (C-side), reduced to
    just the join key and the two wall-clock stamps derive_ntp_offsets()
    needs. A plain Dict[str, int] can't hold c_store_guid (a str) alongside
    the int timestamps without lying to the type checker, so this is a
    dataclass instead."""
    c_store_guid: str
    tu_seq: int
    t1: int  # c_begin_write_wall_ns
    t4: int  # c_commit_read_wall_ns


@dataclass
class FResultSample:
    """One parsed icecream-p50-f-result-v1 record (F-side); see
    SourceResultSample above."""
    c_store_guid: str
    tu_seq: int
    t2: int  # f_begin_recv_wall_ns
    t3: int  # wall_clock_ns, taken at TX_COMMIT write


def load_v5_source_results(path: str) -> List[SourceResultSample]:
    """Load icecream-p50-source-result-v5 records (ICECC_P50_SOURCE_RESULT_TRACE).

    C-side, diagnostic-only. Extracts only the join key and the two
    absolute wall-clock samples needed for derive_ntp_offsets(); never
    touches source bytes or paths.
    """
    out: List[SourceResultSample] = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for lineno, line in enumerate(fh, start=1):
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError as exc:
                print(f"warning: {path}:{lineno}: skipping malformed JSON ({exc})",
                      file=sys.stderr)
                continue
            if obj.get("schema") != "icecream-p50-source-result-v5":
                continue
            try:
                out.append(SourceResultSample(
                    c_store_guid=str(obj["c_store_guid"]),
                    tu_seq=int(obj["tu_seq"]),
                    t1=int(obj.get("c_begin_write_wall_ns", 0)),
                    t4=int(obj.get("c_commit_read_wall_ns", 0)),
                ))
            except (KeyError, TypeError, ValueError) as exc:
                print(f"warning: {path}:{lineno}: skipping v5 record ({exc})",
                      file=sys.stderr)
    return out


def load_v1_f_results(path: str) -> List[FResultSample]:
    """Load icecream-p50-f-result-v1 records (ICECC_P50_F_RESULT_TRACE).

    F-side, diagnostic-only. See load_v5_source_results() above.
    """
    out: List[FResultSample] = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for lineno, line in enumerate(fh, start=1):
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError as exc:
                print(f"warning: {path}:{lineno}: skipping malformed JSON ({exc})",
                      file=sys.stderr)
                continue
            if obj.get("schema") != "icecream-p50-f-result-v1":
                continue
            try:
                out.append(FResultSample(
                    c_store_guid=str(obj["c_store_guid"]),
                    tu_seq=int(obj["tu_seq"]),
                    t2=int(obj.get("f_begin_recv_wall_ns", 0)),
                    t3=int(obj.get("wall_clock_ns", 0)),
                ))
            except (KeyError, TypeError, ValueError) as exc:
                print(f"warning: {path}:{lineno}: skipping f-v1 record ({exc})",
                      file=sys.stderr)
    return out


@dataclass
class NtpOffsetResult:
    offset_ns: float
    error_ns: float
    n_kept: int
    n_total: int


def derive_ntp_offsets(
        c_records: List[SourceResultSample],
        f_records_by_host: Dict[str, List[FResultSample]],
) -> Dict[str, "NtpOffsetResult"]:
    """Derive one NTP-style clock offset per F host from joined C v5/F v1 traces.

    Joins c_records (icecream-p50-source-result-v5) against each host's
    f_records (icecream-p50-f-result-v1) on (c_store_guid, tu_seq) -- the
    only key both schemas carry (c_store_guid is "route" in the review's
    wording). For each joined pair with all four of t1..t4 present and a
    physically plausible (positive) round-trip delay, computes the sample
    offset/delay per the module-level comment above.

    t2 (f_begin_recv_wall_ns) is sampled immediately after the BEGIN frame
    read completes, before it is decoded -- see cache/p50_endpoint.cpp. In
    pipelined mode the BEGIN frame may simply be sitting in F's socket
    receive buffer for a while before F gets to read it (F is still busy
    with an earlier session's HELLO/STATE exchange), which inflates delay
    for that sample without biasing offset -- any one-way delay in either
    direction cancels out of the offset formula and only widens the error
    bound. The lowest-delay-decile filter below naturally favors samples
    without that queuing, so pipelining does not need special-casing here.

    Applies NTP-style clock filtering: keeps only the lowest-delay decile
    of samples per host (at least one), since low-delay samples carry the
    least queueing-induced asymmetry error, then reports the median offset
    and the max per-sample error bound over that kept decile. A host with
    zero usable joined samples is omitted from the result.
    """
    results: Dict[str, NtpOffsetResult] = {}
    for host, f_records in f_records_by_host.items():
        f_index: Dict[Tuple[str, int], FResultSample] = {}
        for rec in f_records:
            f_index.setdefault((rec.c_store_guid, rec.tu_seq), rec)

        samples: List[Tuple[float, float]] = []  # (offset_ns, delay_ns)
        n_total = 0
        for c_rec in c_records:
            f_rec = f_index.get((c_rec.c_store_guid, c_rec.tu_seq))
            if f_rec is None:
                continue
            n_total += 1
            t1, t4 = c_rec.t1, c_rec.t4
            t2, t3 = f_rec.t2, f_rec.t3
            if not (t1 and t2 and t3 and t4):
                continue  # one of the four wall-clock stamps wasn't captured
            delay = (t4 - t1) - (t3 - t2)
            if delay <= 0:
                continue  # not a physically plausible round trip -- drop
            offset = ((t2 - t1) + (t3 - t4)) / 2.0
            samples.append((offset, delay))

        if not samples:
            continue

        samples.sort(key=lambda s: s[1])  # ascending delay
        decile_n = max(1, len(samples) // 10)
        kept = samples[:decile_n]
        offsets_kept = sorted(o for o, _ in kept)
        median_offset = offsets_kept[len(offsets_kept) // 2]
        max_error = max(d / 2.0 for _, d in kept)
        results[host] = NtpOffsetResult(
            offset_ns=median_offset, error_ns=max_error,
            n_kept=len(kept), n_total=n_total)
    return results


def print_ntp_offsets(derived: Dict[str, "NtpOffsetResult"],
                       existing_offsets: Dict[str, int]) -> None:
    if not derived:
        print("\n=== NTP-derived clock offsets: no joined "
              "C-v5/F-v1 samples found ===")
        return
    print("\n=== NTP-derived clock offsets (protocol-level, see "
          "derive_ntp_offsets) -- next to offsets.json ===")
    for host in sorted(derived):
        r = derived[host]
        line = (f"  {host}: offset={r.offset_ns:+.0f}ns "
                f"error<={r.error_ns:.0f}ns "
                f"(n={r.n_kept}/{r.n_total} samples, lowest-delay decile)")
        existing = existing_offsets.get(host)
        if existing is not None:
            disagreement_ns = abs(r.offset_ns - existing)
            line += (f" vs offsets.json={existing:+d}ns "
                      f"diff={disagreement_ns:.0f}ns")
            if disagreement_ns > NTP_DISAGREEMENT_WARN_NS:
                line += " WARNING: disagreement > 5ms"
        print(line)


@dataclass
class NinjaEdge:
    """One .ninja_log data line, path-free by construction.

    start_ms/end_ms are elapsed milliseconds since ninja's own build-start
    reference, as written by ninja -- NOT absolute wall-clock. Use
    compute_ninja_epoch() to place them on the same absolute timeline as
    stage_trace's realtime_ns.

    mtime_ns is .ninja_log v7's restat field: the output file's mtime, in
    epoch nanoseconds, as read by ninja on C1's own clock right after the
    edge finished. It is 0 for older log versions or restat-less edges (see
    parse_ninja_log). Unlike start_ms/end_ms, it is already an absolute
    C1-clock timestamp, so it gives a second, join-free epoch anchor -- see
    compute_ninja_epoch_mtime().
    """
    output_hash: int
    start_ms: int
    end_ms: int
    mtime_ns: int
    cmd_hash: str


def parse_ninja_log(path: str) -> List[NinjaEdge]:
    """Parses a .ninja_log file into path-free NinjaEdge records.

    Only the FNV-1a hash of each edge's output path is kept (see
    fnv1a64_path above) -- the literal path is read to compute the hash
    and then discarded, so no TU/output path ever ends up in anything
    this tool prints or could be copy-pasted elsewhere.
    """
    edges: List[NinjaEdge] = []
    with open(path, "r", encoding="utf-8", errors="surrogateescape") as fh:
        for lineno, line in enumerate(fh, start=1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 5:
                print(f"warning: {path}:{lineno}: skipping malformed ninja_log line",
                      file=sys.stderr)
                continue
            start_ms, end_ms, restat_mtime, out_path, cmd_hash = parts[:5]
            try:
                edges.append(NinjaEdge(
                    output_hash=fnv1a64_path(out_path),
                    start_ms=int(start_ms),
                    end_ms=int(end_ms),
                    mtime_ns=int(restat_mtime),
                    cmd_hash=cmd_hash,
                ))
            except ValueError:
                print(f"warning: {path}:{lineno}: skipping unparsable ninja_log line",
                      file=sys.stderr)
    return edges


def _hash_to_job(jobs: Dict[Tuple[int, int], List[Record]]) -> Dict[int, Tuple[int, int]]:
    out: Dict[int, Tuple[int, int]] = {}
    for job_key, recs in jobs.items():
        for rec in recs:
            if rec.stage == "output_path_mapped" and "output_path_hash" in rec.kv:
                out[int(rec.kv["output_path_hash"])] = job_key
    return out


def compute_ninja_epoch(edges: List[NinjaEdge], jobs: Dict[Tuple[int, int], List[Record]],
                         offsets: Dict[str, int]) -> Optional[int]:
    """Estimates the absolute realtime_ns matching ninja_log's t=0.

    ninja_log timestamps are elapsed ms since ninja's build-start
    reference, not wall-clock. For every ninja edge whose output-path
    hash matches a job's "output_path_mapped" stage_trace event, this
    anchors that edge's start_ms against the job's own
    "iceccclient:source_prepare_start" timestamp (stamped moments after
    ninja forked/exec'd the client for that edge, so the two should be
    only a few ms apart). The median anchor across all matched edges is
    returned, which is robust to any single edge's fork/exec jitter.
    Returns None if no edge could be anchored (e.g. no --ninja-log given,
    or the trace dir predates item (c)'s output_path_mapped event).
    """
    hash_to_job = _hash_to_job(jobs)
    anchors: List[int] = []
    for edge in edges:
        job_key = hash_to_job.get(edge.output_hash)
        if job_key is None:
            continue
        for rec in jobs[job_key]:
            if rec.component == "iceccclient" and rec.stage == "source_prepare_start":
                anchors.append(corrected_ts(rec, offsets) - edge.start_ms * 1_000_000)
                break
    if not anchors:
        return None
    anchors.sort()
    return anchors[len(anchors) // 2]


# Disagreement threshold between the two independent ninja epoch anchors
# (join-based and mtime-based) above which main() flags a warning. 50ms is
# generous relative to filesystem mtime granularity and the join anchor's
# own fork/exec jitter -- it is meant to catch a wrong epoch pick (wrong
# log, wrong host clock), not day-to-day noise.
NINJA_EPOCH_DISAGREEMENT_WARN_NS = 50_000_000


def compute_ninja_epoch_mtime(edges: List[NinjaEdge]) -> Optional[int]:
    """Estimates the absolute realtime_ns matching ninja_log's t=0, using
    only the log file itself -- no stage_trace join required.

    .ninja_log's restat field (NinjaEdge.mtime_ns) is the output file's
    mtime in epoch nanoseconds, stamped on C1's own clock when the edge's
    compiler/linker closed the file -- i.e. already an absolute timestamp.
    Since end_ms is that same instant on ninja's elapsed-ms clock, each
    edge gives an independent epoch estimate `mtime_ns - end_ms * 1e6`.
    The median across all edges with a nonzero mtime is returned. Edges
    with mtime_ns == 0 (older log versions, or restat-less edges that
    never had their output touched) are skipped entirely, per edge --
    they do not count as zero in the median.

    This anchor is intentionally independent of compute_ninja_epoch(): it
    needs no output_path_mapped join and no --offsets, so it also serves
    as a cross-check that the join-based anchor picked the right log file
    and the right host's clock offset. See NINJA_EPOCH_DISAGREEMENT_WARN_NS.
    """
    anchors = [edge.mtime_ns - edge.end_ms * 1_000_000
               for edge in edges if edge.mtime_ns != 0]
    if not anchors:
        return None
    anchors.sort()
    return anchors[len(anchors) // 2]


def group_by_job(records: Iterable[Record]) -> Dict[Tuple[int, int], List[Record]]:
    jobs: Dict[Tuple[int, int], List[Record]] = defaultdict(list)
    for rec in records:
        if rec.has_wire_identity:
            jobs[rec.job_key].append(rec)
    return jobs


def group_uncorrelated(records: Iterable[Record]) -> Dict[str, List[Record]]:
    """Group the fsidecar cache-layer records that have no wire identity.

    Keyed by (component, stage) first for reporting, with each bucket's
    records further carrying host/pid/tu_seq so a reader can manually
    line them up against a job's wall-clock window if needed.
    """
    out: Dict[str, List[Record]] = defaultdict(list)
    for rec in records:
        if not rec.has_wire_identity:
            out[f"{rec.component}:{rec.stage}"].append(rec)
    return out


def fmt_ms(ns: int) -> str:
    return f"{ns / 1e6:.3f}ms"


def print_job_timeline(job_key: Tuple[int, int], recs: List[Record],
                        offsets: Dict[str, int],
                        ninja_edge: Optional[NinjaEdge] = None,
                        ninja_epoch_ns: Optional[int] = None) -> None:
    epoch, wire_id = job_key
    ordered = sorted(recs, key=lambda r: corrected_ts(r, offsets))

    # rows: (abs_ts_ns, label_text) -- real stage_trace records plus, when
    # a matching ninja edge was found, two synthetic rows for ninja's own
    # view of the edge's start/end so the two sources sit on one timeline.
    rows: List[Tuple[int, str]] = []
    for rec in ordered:
        ts = corrected_ts(rec, offsets)
        kv_str = f" {rec.kv}" if rec.kv else ""
        rows.append((ts, f"{rec.component:<12} {rec.stage:<28} "
                          f"host={rec.host} pid={rec.pid} tid={rec.tid}"
                          f" logical_job={rec.logical_job} attempt={rec.attempt}{kv_str}"))
    if ninja_edge is not None and ninja_epoch_ns is not None:
        start_ns = ninja_epoch_ns + ninja_edge.start_ms * 1_000_000
        end_ns = ninja_epoch_ns + ninja_edge.end_ms * 1_000_000
        rows.append((start_ns, f"{'ninja':<12} {'edge_start':<28} "
                                f"(from .ninja_log, epoch-anchored, cmd_hash={ninja_edge.cmd_hash})"))
        rows.append((end_ns, f"{'ninja':<12} {'edge_end':<28} "
                              f"(from .ninja_log, epoch-anchored, cmd_hash={ninja_edge.cmd_hash})"))
    rows.sort(key=lambda r: r[0])

    t0 = rows[0][0]
    print(f"\n=== job epoch={epoch} wire_id={wire_id} "
          f"({len(rows)} rows, {len(ordered)} stage records"
          f"{', +ninja edge' if ninja_edge is not None else ''}) ===")
    prev_ts = t0
    for ts, text in rows:
        delta = ts - prev_ts
        print(f"  +{fmt_ms(ts - t0):>12}  (Δ{fmt_ms(delta):>10})  {text}")
        prev_ts = ts
    print(f"  total span: {fmt_ms(rows[-1][0] - t0)}")


def print_top_gaps(job_key: Tuple[int, int], recs: List[Record],
                    offsets: Dict[str, int], top_n: int) -> None:
    ordered = sorted(recs, key=lambda r: corrected_ts(r, offsets))
    gaps = []
    for a, b in zip(ordered, ordered[1:]):
        delta = corrected_ts(b, offsets) - corrected_ts(a, offsets)
        gaps.append((delta, a, b))
    gaps.sort(key=lambda g: g[0], reverse=True)
    print(f"\n--- top {top_n} gaps for epoch={job_key[0]} wire_id={job_key[1]} ---")
    for delta, a, b in gaps[:top_n]:
        print(f"  {fmt_ms(delta):>10}  {a.component}.{a.stage} -> {b.component}.{b.stage}")


def summarize(jobs: Dict[Tuple[int, int], List[Record]],
              offsets: Dict[str, int]) -> None:
    spans = []
    # Collect, per unique (component,stage) transition seen as consecutive
    # within a job's sorted timeline, the distribution of gap durations.
    transition_gaps: Dict[str, List[int]] = defaultdict(list)
    # Specifically track the scheduler-assign -> JobBegin gap called out
    # in the original stage-trace request, wherever both ends are present.
    assign_to_jobbegin: List[int] = []

    for job_key, recs in jobs.items():
        ordered = sorted(recs, key=lambda r: corrected_ts(r, offsets))
        t0 = corrected_ts(ordered[0], offsets)
        t1 = corrected_ts(ordered[-1], offsets)
        spans.append(t1 - t0)

        assign_ts = None
        jobbegin_ts = None
        for a, b in zip(ordered, ordered[1:]):
            key = f"{a.component}.{a.stage} -> {b.component}.{b.stage}"
            transition_gaps[key].append(corrected_ts(b, offsets) - corrected_ts(a, offsets))
        for rec in ordered:
            if rec.stage in ("assign_prepare_sent", "usecs_sent") and assign_ts is None:
                assign_ts = corrected_ts(rec, offsets)
            if rec.stage == "jobbegin_sent" and rec.component in ("fdaemon", "cdaemon"):
                if assign_ts is not None and jobbegin_ts is None:
                    jobbegin_ts = corrected_ts(rec, offsets)
        if assign_ts is not None and jobbegin_ts is not None and jobbegin_ts >= assign_ts:
            assign_to_jobbegin.append(jobbegin_ts - assign_ts)

    def pct(data: List[int], p: float) -> float:
        if not data:
            return float("nan")
        data = sorted(data)
        k = (len(data) - 1) * p
        f, c = int(k), min(int(k) + 1, len(data) - 1)
        return data[f] + (data[c] - data[f]) * (k - f)

    print(f"\n=== summary over {len(jobs)} jobs ===")
    if spans:
        print(f"NEW->END span (ms): p50={pct(spans,0.5)/1e6:.3f} "
              f"p90={pct(spans,0.9)/1e6:.3f} p99={pct(spans,0.99)/1e6:.3f} "
              f"max={max(spans)/1e6:.3f}")
    if assign_to_jobbegin:
        print(f"scheduler-assign -> JobBegin gap (ms): "
              f"p50={pct(assign_to_jobbegin,0.5)/1e6:.3f} "
              f"p90={pct(assign_to_jobbegin,0.9)/1e6:.3f} "
              f"p99={pct(assign_to_jobbegin,0.99)/1e6:.3f} "
              f"n={len(assign_to_jobbegin)}")
    else:
        print("scheduler-assign -> JobBegin gap: no job had both ends stamped")

    print("\nper-transition gap percentiles (ms), sorted by p50 descending:")
    rows = []
    for key, deltas in transition_gaps.items():
        rows.append((pct(deltas, 0.5), pct(deltas, 0.9), pct(deltas, 0.99),
                     len(deltas), key))
    rows.sort(reverse=True)
    for p50, p90, p99, n, key in rows:
        print(f"  p50={p50/1e6:8.3f} p90={p90/1e6:8.3f} p99={p99/1e6:8.3f} "
              f"n={n:5d}  {key}")


# Canonical causal order of (component, stage) pairs along the P50 remote-
# compile path. Used only by check_clock_sanity() below: unlike every other
# report in this file, which sorts a job's records by corrected_ts (and so
# can never show a negative gap by construction), this walks the stages in
# their known real-world order and flags any pair whose corrected
# timestamps come out backwards. That can only mean the clock-offset
# correction between the two hosts involved is wrong (or missing) --
# genuine reordering of the underlying events is not possible given the
# protocol's own synchronous handoffs.
#
# Only adjacent *present* entries whose hosts differ are ever compared (see
# the loop below), so consecutive same-host entries are always safe to
# insert for extra resolution -- they never add a spurious check. Entries
# are grounded directly in the call sites that emit them:
#   - scheduler's fenced-assignment admission control (assign_prepare_sent
#     -> F's assign_prepare_recv/ready_sent -> assign_ready_recv) must
#     complete, with F signaling READY, before the scheduler is allowed to
#     expose the assignment to C via UseCS.
#   - the ARM/CACHE_SESSION handshake (csidecar's open_armed()) is a
#     synchronous round trip over the ordinary wrapper connection, strictly
#     *after* source_read_done/prepare_source_done and *before*
#     transfer_lock_acquired (this reorders the previous, incorrect
#     placement of source_read_done/prepare_source_done after
#     transfer_lock_acquired).
#   - the remote compile-done cluster (compile_start/compile_result_sent/
#     object_sent on F) brackets the existing iceccclient receive stages.
# fdaemon.child_exit is deliberately omitted: it has no synchronous
# handoff partner on another host (it is F-local cleanup after object_sent
# returns), so pairing it with any C-side stage would encode an empirical
# ordering, not a true causal one, and could false-positive under normal
# network jitter. The three fsidecar stages (session_accepted,
# input_committed, input_materialized) are deliberately excluded too: per
# this file's module docstring they carry no wire identity and cannot be
# keyed to a job_key, so group_uncorrelated() -- not this table -- is where
# they belong.
CANONICAL_STAGE_ORDER: List[Tuple[str, str]] = [
    ("scheduler", "getcs_recv"),
    ("scheduler", "assign_prepare_sent"),
    ("fdaemon", "assign_prepare_recv"),
    ("fdaemon", "ready_sent"),
    ("scheduler", "assign_ready_recv"),
    ("scheduler", "usecs_sent"),
    ("cdaemon", "usecs_recv"),
    ("cdaemon", "usecs_relayed"),
    ("iceccclient", "source_prepare_start"),
    ("iceccclient", "source_prepare_done"),
    ("iceccclient", "connected_to_f"),
    ("csidecar", "transfer_request_received"),
    ("csidecar", "source_read_done"),
    ("csidecar", "prepare_source_done"),
    ("csidecar", "armed_slot_acquired"),
    ("csidecar", "f_connect_done"),
    ("fdaemon", "arm_recv"),
    ("fdaemon", "arm_validated"),
    ("csidecar", "arm_ack_received"),
    ("fdaemon", "cache_session_dispatched"),
    ("csidecar", "cache_session_ready"),
    ("csidecar", "transfer_lock_acquired"),
    ("csidecar", "transfer_lock_released"),
    ("iceccclient", "source_transferred"),
    ("iceccclient", "compile_file_sent"),
    ("fdaemon", "compile_child_forked"),
    ("fdaemon", "jobbegin_sent"),
    ("scheduler", "jobbegin_recv"),
    ("fdaemon", "compile_start"),
    ("fdaemon", "compile_result_sent"),
    ("iceccclient", "compile_result_received"),
    ("iceccclient", "output_path_mapped"),
    ("fdaemon", "object_sent"),
    ("iceccclient", "object_received"),
    ("cdaemon", "jobdone_sent"),
    ("scheduler", "jobdone_recv"),
    ("iceccclient", "job_complete"),
]


def check_clock_sanity(jobs: Dict[Tuple[int, int], List[Record]],
                        offsets: Dict[str, int]) -> List[str]:
    """Flags jobs where two causally-ordered stages, on different hosts,
    have corrected timestamps in the wrong order -- i.e. the clock-offset
    correction (or lack of one, with --offsets omitted) is wrong for at
    least one of the hosts involved. Returns one message per violation
    found; callers decide whether/how to print them.
    """
    warnings: List[str] = []
    for job_key, recs in jobs.items():
        by_stage: Dict[Tuple[str, str], Record] = {}
        for rec in recs:
            key = (rec.component, rec.stage)
            if key not in by_stage:  # first occurrence only (ignore retries here)
                by_stage[key] = rec
        prev: Optional[Tuple[Tuple[str, str], Record]] = None
        for key in CANONICAL_STAGE_ORDER:
            rec = by_stage.get(key)
            if rec is None:
                continue
            if prev is not None:
                prev_key, prev_rec = prev
                if prev_rec.host != rec.host:
                    prev_ts = corrected_ts(prev_rec, offsets)
                    ts = corrected_ts(rec, offsets)
                    if ts < prev_ts:
                        warnings.append(
                            f"job epoch={job_key[0]} wire_id={job_key[1]}: "
                            f"{prev_key[0]}.{prev_key[1]} (host={prev_rec.host}) "
                            f"-> {key[0]}.{key[1]} (host={rec.host}) is "
                            f"{fmt_ms(ts - prev_ts)} backwards -- check offsets.json "
                            f"for these two hosts")
            prev = (key, rec)
    return warnings


def occupancy_timeline(intervals: List[Tuple[int, int]],
                        bucket_ms: int = 500) -> List[Tuple[int, int]]:
    """Buckets a list of (start_ns, end_ns) busy intervals into a
    concurrency-over-time histogram: for each bucket_ms-wide time bucket,
    how many intervals were open at its start. Returns [(bucket_start_ns,
    concurrency), ...]; printing is left to the caller (see
    print_occupancy) so this stays unit-testable without stdout.
    """
    if not intervals:
        return []
    t_min = min(s for s, _ in intervals)
    t_max = max(e for _, e in intervals)
    bucket_ns = bucket_ms * 1_000_000
    n_buckets = max(1, (t_max - t_min) // bucket_ns + 1)
    counts = [0] * n_buckets
    for start, end in intervals:
        b0 = (start - t_min) // bucket_ns
        b1 = (end - t_min) // bucket_ns
        for b in range(max(0, b0), min(n_buckets - 1, b1) + 1):
            counts[b] += 1
    return [(t_min + i * bucket_ns, c) for i, c in enumerate(counts)]


def print_occupancy(jobs: Dict[Tuple[int, int], List[Record]],
                     offsets: Dict[str, int], bucket_ms: int) -> None:
    """Farm occupancy over time, derived from each job's scheduler-visible
    busy interval: jobbegin_recv (assignment accepted) to jobdone_recv
    (result landed). This only covers jobs that actually went remote
    (local/cache-hit ninja edges never reach the scheduler); pass
    --ninja-log for the full-graph view instead/in addition.
    """
    intervals: List[Tuple[int, int]] = []
    for recs in jobs.values():
        begin = end = None
        for rec in recs:
            if rec.component == "scheduler" and rec.stage == "jobbegin_recv":
                begin = corrected_ts(rec, offsets)
            elif rec.component == "scheduler" and rec.stage == "jobdone_recv":
                end = corrected_ts(rec, offsets)
        if begin is not None and end is not None and end >= begin:
            intervals.append((begin, end))
    if not intervals:
        print("\n=== farm occupancy ===\nno job had both jobbegin_recv and "
              "jobdone_recv -- nothing to plot")
        return
    buckets = occupancy_timeline(intervals, bucket_ms)
    peak = max(c for _, c in buckets) if buckets else 0
    print(f"\n=== farm occupancy over {len(intervals)} scheduler-visible jobs "
          f"(bucket={bucket_ms}ms, peak={peak}) ===")
    t0 = buckets[0][0]
    for ts, count in buckets:
        bar = "#" * min(count, 120)
        print(f"  +{fmt_ms(ts - t0):>10}  n={count:4d}  {bar}")


def print_ninja_summary(edges: List[NinjaEdge], jobs: Dict[Tuple[int, int], List[Record]]) -> None:
    """Reports how many .ninja_log edges matched a stage-traced job via the
    output_path_mapped hash, and the distribution of ninja's own
    (end_ms - start_ms) wall time for matched edges -- a sanity check on
    whether stage_trace's own NEW->END span over/under-estimates ninja's
    view of the same edge.
    """
    hash_to_job = _hash_to_job(jobs)
    matched = [e for e in edges if e.output_hash in hash_to_job]
    walls_ms = [e.end_ms - e.start_ms for e in matched]
    print(f"\n=== ninja log join: {len(matched)}/{len(edges)} edges matched a "
          f"stage-traced job via output_path_mapped ===")
    if walls_ms:
        walls_ms.sort()

        def pct(data, p):
            k = (len(data) - 1) * p
            f, c = int(k), min(int(k) + 1, len(data) - 1)
            return data[f] + (data[c] - data[f]) * (k - f)
        print(f"  ninja edge wall time (ms): p50={pct(walls_ms,0.5):.3f} "
              f"p90={pct(walls_ms,0.9):.3f} max={max(walls_ms):.3f}")
    unmatched = len(edges) - len(matched)
    if unmatched:
        print(f"  {unmatched} edges unmatched (local/cache-hit compiles never "
              f"reach iceccclient, or trace dir predates output_path_mapped)")


def print_uncorrelated(records: List[Record], tu_seq_filter: Optional[int],
                        limit: int) -> None:
    buckets = group_uncorrelated(records)
    if not buckets:
        return
    print("\n=== uncorrelated fsidecar/cache-layer events "
          "(no wire identity available at this layer) ===")
    for key, recs in sorted(buckets.items()):
        shown = recs
        if tu_seq_filter is not None:
            shown = [r for r in recs if r.kv.get("tu_seq") == tu_seq_filter]
        shown = sorted(shown, key=lambda r: r.realtime_ns)[:limit]
        print(f"\n  {key}  ({len(recs)} total"
              f"{', filtered to tu_seq=' + str(tu_seq_filter) if tu_seq_filter is not None else ''}"
              f", showing {len(shown)})")
        for rec in shown:
            kv_str = f" {rec.kv}" if rec.kv else ""
            print(f"    host={rec.host} pid={rec.pid} tid={rec.tid}"
                  f" realtime_ns={rec.realtime_ns}{kv_str}")


def parse_job_arg(value: str) -> Tuple[int, int]:
    try:
        epoch_s, wire_s = value.split(":", 1)
        return int(epoch_s), int(wire_s)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            f"--job expects EPOCH:WIREID, got {value!r}") from exc


def parse_host_path_arg(value: str) -> Tuple[str, str]:
    host, _, path = value.partition("=")
    if not host or not path:
        raise argparse.ArgumentTypeError(
            f"--f-result-trace expects HOST=FILE, got {value!r}")
    return host, path


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("trace_dir", help="directory containing *.jsonl stage_trace output")
    parser.add_argument("--job", type=parse_job_arg, default=None,
                         help="print only this job's timeline, as EPOCH:WIREID")
    parser.add_argument("--summary", action="store_true",
                         help="print cross-job percentile summary instead of per-job dumps")
    parser.add_argument("--top-gaps", type=int, default=0, metavar="N",
                         help="also print the top-N largest gaps for each printed job")
    parser.add_argument("--offsets", default=None, metavar="FILE",
                         help="JSON file of {host: offset_ns} from offsets.sh, "
                              "used to align realtime_ns across hosts")
    parser.add_argument("--tu-seq", type=int, default=None,
                         help="filter the uncorrelated fsidecar section to one tu_seq")
    parser.add_argument("--limit", type=int, default=20,
                         help="max records to print per uncorrelated bucket (default 20)")
    parser.add_argument("--no-uncorrelated", action="store_true",
                         help="suppress the uncorrelated fsidecar/cache-layer section")
    parser.add_argument("--ninja-log", default=None, metavar="FILE",
                         help="path to a .ninja_log file; joined to jobs via the "
                              "output_path_mapped hash (see fnv1a64_path) and "
                              "epoch-anchored against source_prepare_start -- "
                              "never prints the paths it reads, only hashes "
                              "and durations")
    parser.add_argument("--occupancy", action="store_true",
                         help="print a farm occupancy timeline derived from "
                              "scheduler jobbegin_recv/jobdone_recv intervals")
    parser.add_argument("--occupancy-bucket-ms", type=int, default=500, metavar="MS",
                         help="bucket width for --occupancy (default 500ms)")
    parser.add_argument("--check-clocks", action="store_true",
                         help="walk each job's canonical causal stage order and "
                              "flag any cross-host pair whose corrected "
                              "timestamps come out backwards (see "
                              "CANONICAL_STAGE_ORDER); unlike --summary this "
                              "can actually detect clock-offset problems")
    parser.add_argument("--c-result-trace", default=None, metavar="FILE",
                         help="path to a C-side ICECC_P50_SOURCE_RESULT_TRACE "
                              "(icecream-p50-source-result-v5) JSONL file; "
                              "combine with one or more --f-result-trace to "
                              "derive protocol-level NTP clock offsets "
                              "(see derive_ntp_offsets)")
    parser.add_argument("--f-result-trace", action="append", default=[],
                         type=parse_host_path_arg, metavar="HOST=FILE",
                         help="HOST=FILE pair for an F-side "
                              "ICECC_P50_F_RESULT_TRACE "
                              "(icecream-p50-f-result-v1) JSONL file; repeat "
                              "once per F host")
    args = parser.parse_args(argv)

    records = load_records(args.trace_dir)
    if not records:
        print("error: no stage_trace records loaded", file=sys.stderr)
        return 1

    offsets = load_offsets(args.offsets)
    jobs = group_by_job(records)
    if not jobs:
        print("warning: no records carried a wire identity "
              "(epoch,wire_id) -- is ICECC_STAGE_TRACE_DIR set tree-wide?",
              file=sys.stderr)

    ninja_edges: List[NinjaEdge] = []
    ninja_epoch_ns: Optional[int] = None
    job_to_edge: Dict[Tuple[int, int], NinjaEdge] = {}
    if args.ninja_log:
        ninja_edges = parse_ninja_log(args.ninja_log)
        ninja_epoch_ns = compute_ninja_epoch(ninja_edges, jobs, offsets)
        if ninja_epoch_ns is None:
            print("warning: --ninja-log given but no edge matched any "
                  "output_path_mapped job -- is item (c) enabled in this trace?",
                  file=sys.stderr)
        else:
            hash_to_job = _hash_to_job(jobs)
            for edge in ninja_edges:
                job_key = hash_to_job.get(edge.output_hash)
                if job_key is not None:
                    job_to_edge[job_key] = edge
        mtime_epoch_ns = compute_ninja_epoch_mtime(ninja_edges)
        print(f"  ninja epoch anchor (join via output_path_mapped): "
              f"{'n/a' if ninja_epoch_ns is None else ninja_epoch_ns}")
        print(f"  ninja epoch anchor (restat mtime, join-free): "
              f"{'n/a' if mtime_epoch_ns is None else mtime_epoch_ns}")
        if ninja_epoch_ns is not None and mtime_epoch_ns is not None:
            disagreement_ns = abs(ninja_epoch_ns - mtime_epoch_ns)
            if disagreement_ns > NINJA_EPOCH_DISAGREEMENT_WARN_NS:
                print(f"  WARNING: the two ninja epoch anchors disagree by "
                      f"{fmt_ms(disagreement_ns)} (> "
                      f"{fmt_ms(NINJA_EPOCH_DISAGREEMENT_WARN_NS)}) -- check "
                      f"that --ninja-log matches this trace dir and host",
                      file=sys.stderr)
        print_ninja_summary(ninja_edges, jobs)

    def ninja_edge_for(job_key: Tuple[int, int]) -> Optional[NinjaEdge]:
        return job_to_edge.get(job_key)

    if args.job is not None:
        recs = jobs.get(args.job)
        if not recs:
            print(f"error: no records for job {args.job}", file=sys.stderr)
            return 1
        print_job_timeline(args.job, recs, offsets,
                            ninja_edge_for(args.job), ninja_epoch_ns)
        if args.top_gaps:
            print_top_gaps(args.job, recs, offsets, args.top_gaps)
    elif args.summary:
        summarize(jobs, offsets)
    else:
        for job_key, recs in sorted(jobs.items()):
            print_job_timeline(job_key, recs, offsets,
                                ninja_edge_for(job_key), ninja_epoch_ns)
            if args.top_gaps:
                print_top_gaps(job_key, recs, offsets, args.top_gaps)

    if args.occupancy:
        print_occupancy(jobs, offsets, args.occupancy_bucket_ms)

    if args.check_clocks:
        warnings = check_clock_sanity(jobs, offsets)
        if warnings:
            print(f"\n=== clock sanity: {len(warnings)} causal-order "
                  f"violation(s) found ===")
            for w in warnings:
                print(f"  WARNING: {w}")
        else:
            print("\n=== clock sanity: no causal-order violations "
                  "found across hosts ===")

    if not args.no_uncorrelated:
        print_uncorrelated(records, args.tu_seq, args.limit)

    if args.c_result_trace and args.f_result_trace:
        c_v5_records = load_v5_source_results(args.c_result_trace)
        f_v1_records_by_host: Dict[str, List[FResultSample]] = {}
        for host, path in args.f_result_trace:
            f_v1_records_by_host.setdefault(host, []).extend(
                load_v1_f_results(path))
        derived_offsets = derive_ntp_offsets(c_v5_records, f_v1_records_by_host)
        print_ntp_offsets(derived_offsets, offsets)
    elif args.f_result_trace and not args.c_result_trace:
        print("warning: --f-result-trace given without --c-result-trace -- "
              "skipping NTP offset derivation", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
