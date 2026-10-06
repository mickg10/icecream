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

Usage
-----
    analyze.py <trace_dir> [--job EPOCH:WIREID] [--summary] [--top-gaps N]
               [--offsets offsets.json] [--tu-seq SEQ] [--limit N]

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
                        offsets: Dict[str, int]) -> None:
    epoch, wire_id = job_key
    ordered = sorted(recs, key=lambda r: corrected_ts(r, offsets))
    t0 = corrected_ts(ordered[0], offsets)
    print(f"\n=== job epoch={epoch} wire_id={wire_id} "
          f"({len(ordered)} stage records) ===")
    prev_ts = t0
    for rec in ordered:
        ts = corrected_ts(rec, offsets)
        delta = ts - prev_ts
        kv_str = f" {rec.kv}" if rec.kv else ""
        print(f"  +{fmt_ms(ts - t0):>12}  (Δ{fmt_ms(delta):>10})  "
              f"{rec.component:<12} {rec.stage:<28} "
              f"host={rec.host} pid={rec.pid} tid={rec.tid}"
              f" logical_job={rec.logical_job} attempt={rec.attempt}{kv_str}")
        prev_ts = ts
    print(f"  total span: {fmt_ms(corrected_ts(ordered[-1], offsets) - t0)}")


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

    if args.job is not None:
        recs = jobs.get(args.job)
        if not recs:
            print(f"error: no records for job {args.job}", file=sys.stderr)
            return 1
        print_job_timeline(args.job, recs, offsets)
        if args.top_gaps:
            print_top_gaps(args.job, recs, offsets, args.top_gaps)
    elif args.summary:
        summarize(jobs, offsets)
    else:
        for job_key, recs in sorted(jobs.items()):
            print_job_timeline(job_key, recs, offsets)
            if args.top_gaps:
                print_top_gaps(job_key, recs, offsets, args.top_gaps)

    if not args.no_uncorrelated:
        print_uncorrelated(records, args.tu_seq, args.limit)

    return 0


if __name__ == "__main__":
    sys.exit(main())
