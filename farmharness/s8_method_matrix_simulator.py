#!/usr/bin/env python3
"""Bounded method-matrix simulator with the P50 relationship contract.

This module is intentionally small and deterministic.  It is the simulator
boundary used by matrix experiments; it does not pretend that a missing
product implementation is a measurement.  In particular, a worker's slot
capacity is a scheduling limit, while cache relationship state is keyed by
the authoritative C/F pair.

The route codec mirrors the local product authority in ``cache/p50_zstd.cpp``:
one level-3 frame is made for every TU, with a bounded committed raw prefix
used as the frame prefix.  A failed/tentative occurrence never advances that
prefix.  Cohort and global are kept separate from route and fail closed unless
their own authority is supplied.

The command-line runner measures the five core methods by default.  Optional
methods can be requested explicitly with ``--methods``; the selection is
ordered, duplicate-free, and is retained in NOT_READY canaries.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import stat
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence

SCHEMA = "icecream-s8-method-matrix-simulator-v1"
OCCURRENCE_SCHEMA = "icecream-s8-method-occurrence-v1"
SUMMARY_SCHEMA = "icecream-s8-method-matrix-summary-v1"
METHODS = ("RAW_II", "ZSTD_TU", "P29", "GRZ_RESIDUAL", "ZSTD_ROUTE",
           "ZSTD_COHORT", "ZSTD_GLOBAL")
DEFAULT_CLI_METHODS = ("RAW_II", "ZSTD_TU", "P29", "GRZ_RESIDUAL", "ZSTD_ROUTE")
READY_METHODS = frozenset(("RAW_II", "ZSTD_TU", "ZSTD_ROUTE"))
CORE_METHODS = frozenset(("RAW_II", "ZSTD_TU", "P29", "GRZ_RESIDUAL", "ZSTD_ROUTE"))
STATEFUL_METHODS = frozenset(("ZSTD_ROUTE", "P29", "GRZ_RESIDUAL"))
TOPOLOGY_IDS = ("C1F1/100000", "C1F20/40")
DEFAULT_HISTORY_BYTES = 128 << 20
MAX_HISTORY_WINDOW_LOG = 27
NATIVE_BATCH_TIMEOUT_FLOOR_SECONDS = 600
NATIVE_BATCH_SECONDS_PER_TRANSACTION = 10
PREFIX_DESCRIPTOR_SCHEMA = "icecream-s8-route-prefix-descriptor-v1"
AUTH_TRACE_SHA256 = "9fa7124f63212ccfd78f139dc05dcc5b2737ef868dc3e6878e64f609e079e960"
AUTH_CORPUS_MANIFEST_SHA256 = "cbca563b6efe3255382db61b490ba948f3d2cdc0d95b793317f7d0134ec6f3fa"
AUTH_ASSIGNMENT_SHA256 = {
    "C1F1/100000": "4c9e969acb321f37f451b1c14f589b0192ecbbed827da6a6531b1ee238bd2950",
    "C1F20/40": "674f4c0bad8abf3b7dfdf595218a60e8d8ff83553287996d28be0c66f9e4dfe6",
}
AUTH_CORPUS_ROOT = Path("/tanksmall/scratch/ictmp/corpus18")
AUTH_CORPUS_MANIFEST = AUTH_CORPUS_ROOT / "manifest.txt"
AUTH_ASSIGNMENT_ROOT = Path("/tanksmall/scratch/ictmp/root-matrix-v4-corrected")
AUTH_ASSIGNMENT_PATHS = {
    "C1F1/100000": AUTH_ASSIGNMENT_ROOT / "assignments-1c1f-1000000.tsv",
    "C1F20/40": AUTH_ASSIGNMENT_ROOT / "assignments-1c20f-40total.tsv",
}
AUTH_P50_ZSTD_SHA256 = "a367316c1ee51ebe2559eacae8d4ab0eab938565a03e408ebbeb0df07a6c9bec"
AUTH_LIBBSC_HEAD = "baffa62c70b6ebbecc9af14ce550e965ea247680"
AUTH_LIBBSC_TREE = "1e35539f6c626639f66b9a83a84bc21b2cd84418"
AUTH_LIBBSC_ARCHIVE_SHA256 = "39edf31118aa546a0439a08e730a7bcab522f376fa9a715fc17a3add4c760ccf"
AUTH_LIBBSC_LIBRARY_SHA256 = "b7446c05a46405cd85eb4ca4d2615350c29e7b9d3ce758e528b37a622c944879"
AUTH_LIBBSC_HEADER_SHA256 = "27dfa3fc8f383aff80e5fb3bcb79efd5d73918784b55fa1f46f8b373c9c965de"


class MatrixError(ValueError):
    """Malformed topology, occurrence, or method authority."""


class NotReady(MatrixError):
    """A method was requested without its independent authority."""


def _native_batch_timeout_seconds(transaction_count: int) -> int:
    """Return the deterministic native timeout for an authenticated batch."""
    if type(transaction_count) is not int or transaction_count < 0:
        raise MatrixError("native batch transaction count invalid")
    return max(NATIVE_BATCH_TIMEOUT_FLOOR_SECONDS,
               NATIVE_BATCH_SECONDS_PER_TRANSACTION * transaction_count)


def _validate_method_selection(methods: Iterable[str]) -> tuple[str, ...]:
    selected = tuple(methods)
    if not selected or any(not isinstance(method, str) or method not in METHODS
                           for method in selected):
        raise MatrixError("methods must contain one or more known method names")
    if len(selected) != len(set(selected)):
        raise MatrixError("methods must not contain duplicates")
    return selected


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _digest128(data: bytes) -> str:
    try:
        import xxhash
    except ImportError as exc:
        raise MatrixError("native digest authority unavailable: xxhash") from exc
    return xxhash.xxh3_128_hexdigest(data)


def _digest128_text(value: object) -> bool:
    return (isinstance(value, str) and value != "0" * 32 and
            bool(re.fullmatch(r"[0-9a-f]{32}", value)))


def _prefix_descriptor(history: bytes | bytearray) -> dict[str, object]:
    """Return fixed-size evidence for a bounded route prefix."""
    return {"schema": PREFIX_DESCRIPTOR_SCHEMA, "bytes": len(history),
            "digest128": _digest128(history)}


def _valid_prefix_descriptor(value: object, *, limit: int = 1 << MAX_HISTORY_WINDOW_LOG) -> bool:
    return (isinstance(value, Mapping) and set(value) == {"schema", "bytes", "digest128"} and
            value.get("schema") == PREFIX_DESCRIPTOR_SCHEMA and type(value.get("bytes")) is int and
            0 <= int(value["bytes"]) <= limit and _digest128_text(value.get("digest128")))


def _validate_native_prefix_keys(item: Mapping[str, object], method: str) -> None:
    prefix_keys = {key for key in item
                   if isinstance(key, str) and key.startswith("committed_raw_prefix")}
    allowed = {"committed_raw_prefix_before_descriptor", "committed_raw_prefix_descriptor"}
    if method == "ZSTD_ROUTE":
        if prefix_keys != allowed:
            raise MatrixError("native product output route descriptor keys invalid")
    elif prefix_keys:
        raise MatrixError("native product output prefix body forbidden")


def _state_digest(history: bytes | bytearray, next_rel_seq: int,
                  route_id: str | None, nonce: int) -> str:
    return _sha256(_canonical({"prefix": _prefix_descriptor(history),
                               "next_rel_seq": next_rel_seq,
                               "route_id": route_id, "nonce": nonce}))


def _append_bounded_history(history: bytes | bytearray, raw: bytes, limit: int
                            ) -> bytes | bytearray:
    """Append without materializing the old history plus the new payload."""
    if isinstance(history, bytearray):
        _append_bounded_history_in_place(history, raw, limit)
        return history
    if len(raw) >= limit:
        return raw[-limit:]
    keep = max(0, limit - len(raw))
    return history[-keep:] + raw


def _append_bounded_history_in_place(history: bytearray, raw: bytes, limit: int) -> None:
    """Append to verifier state without ever growing beyond the suffix bound."""
    if len(raw) >= limit:
        history.clear()
        history.extend(raw[-limit:])
        return
    keep = max(0, limit - len(raw))
    if keep == 0:
        history.clear()
    elif len(history) > keep:
        del history[:-keep]
    history.extend(raw)


def _canonical(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":"),
                       ensure_ascii=True, allow_nan=False) + "\n").encode("ascii")


def _strict_pairs(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise MatrixError("json:duplicate_key:" + key)
        result[key] = value
    return result


def _reject_json_constant(value: str) -> object:
    raise MatrixError("json:nonfinite:" + value)


def _strict_json(raw: bytes, label: str) -> object:
    try:
        return json.loads(raw.decode("utf-8"), object_pairs_hook=_strict_pairs,
                          parse_constant=_reject_json_constant)
    except (UnicodeDecodeError, json.JSONDecodeError, MatrixError) as exc:
        if isinstance(exc, MatrixError):
            raise
        raise MatrixError(label + ":invalid_json") from exc


def _private_experiment_file(experiment: Path, relative: str, label: str) -> Path:
    if not isinstance(relative, str) or not relative or Path(relative).is_absolute():
        raise MatrixError(label + ":path_invalid")
    parts = Path(relative).parts
    if any(part in ("", ".", "..") for part in parts):
        raise MatrixError(label + ":path_invalid")
    root = experiment.resolve(strict=True)
    path = experiment / relative
    try:
        if path.resolve(strict=True).relative_to(root) != Path(relative):
            raise MatrixError(label + ":path_outside_experiment")
    except ValueError as exc:
        raise MatrixError(label + ":path_outside_experiment") from exc
    return path


def _experiment_files(experiment: Path) -> dict[str, tuple[bytes, dict[str, object]]]:
    try:
        info = experiment.lstat()
    except OSError as exc:
        raise MatrixError("verifier:experiment_unavailable") from exc
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode):
        raise MatrixError("verifier:experiment_not_private_directory")
    files: dict[str, tuple[bytes, dict[str, object]]] = {}
    for path in experiment.rglob("*"):
        if path == experiment / "manifest.json":
            continue
        try:
            child = path.lstat()
        except OSError as exc:
            raise MatrixError("verifier:artifact_unavailable") from exc
        if stat.S_ISLNK(child.st_mode):
            raise MatrixError("verifier:artifact_symlink")
        if stat.S_ISDIR(child.st_mode):
            continue
        if not stat.S_ISREG(child.st_mode) or child.st_nlink != 1:
            raise MatrixError("verifier:artifact_not_private_regular")
        relative = str(path.relative_to(experiment))
        files[relative] = _private_bytes(path, "verifier_artifact")
    return files


def _stamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def _authority_file(path: Path, finding: str) -> dict[str, object]:
    result: dict[str, object] = {"path": str(path), "finding": finding}
    try:
        _raw, facts = _private_bytes(path, finding)
    except (OSError, MatrixError):
        result["available"] = False
        result["sha256"] = None
        return result
    result.update({"available": True, "bytes": facts["bytes"], "sha256": facts["sha256"]})
    return result


def _private_digest(path: Path, label: str) -> dict[str, object]:
    return {key: value for key, value in _private_bytes(path, label)[1].items()
            if key in ("path", "bytes", "sha256")}


def _private_bytes(path: Path, label: str) -> tuple[bytes, dict[str, object]]:
    try:
        fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    except OSError as exc:
        raise MatrixError(f"{label}:missing:{path}") from exc
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
            raise MatrixError(f"{label}:not_private_regular:{path}")
        chunks = []
        while True:
            chunk = os.read(fd, 1 << 20)
            if not chunk:
                break
            chunks.append(chunk)
        raw = b"".join(chunks)
        after = os.fstat(fd)
    finally:
        os.close(fd)
    if (len(raw) != info.st_size or after.st_dev != info.st_dev or
            after.st_ino != info.st_ino or after.st_nlink != info.st_nlink or
            after.st_size != info.st_size):
        raise MatrixError(f"{label}:identity_changed:{path}")
    return raw, {"path": str(path), "bytes": len(raw), "sha256": _sha256(raw)}


def _stream_route_prefixes(selected: object, assignment: object, root: Path,
                           *, predecessor_selected: object | None = None,
                           predecessor_assignment: object | None = None,
                           max_history_bytes: int = DEFAULT_HISTORY_BYTES,
                           label: str = "route_input") -> tuple[
                               dict[int, dict[str, object]],
                               dict[str, object], list[dict[str, object]]]:
    """Authenticate inputs once while reconstructing fixed-size route evidence."""
    def build_entries(stream: object, stream_assignment: object, stream_label: str
                      ) -> tuple[dict[int, list[tuple[int, Mapping[str, object], Mapping[str, object]]]], int]:
        if not isinstance(stream, list) or not isinstance(stream_assignment, Mapping):
            raise MatrixError(stream_label + ":authority_missing")
        assignment_rows = stream_assignment.get("rows")
        if not isinstance(assignment_rows, list) or len(assignment_rows) != len(stream):
            raise MatrixError(stream_label + ":assignment_length_invalid")
        topology_id = stream_assignment.get("topology")
        if topology_id not in TOPOLOGY_IDS:
            raise MatrixError(stream_label + ":topology_invalid")
        relationship_count = 1 if topology_id == "C1F1/100000" else 20
        if assignment_rows:
            try:
                start = int(assignment_rows[0]["dispatch_order"])
            except (KeyError, TypeError, ValueError) as exc:
                raise MatrixError(stream_label + ":assignment_row_invalid") from exc
            expected_assignment = _authenticated_assignment(topology_id, len(stream), start=start)
            if any(stream_assignment.get(field) != expected_assignment.get(field)
                   for field in ("path", "bytes", "sha256", "rows", "selected_count",
                                 "authority_total_rows")):
                raise MatrixError(stream_label + ":assignment_not_authenticated")
        entries: dict[int, list[tuple[int, Mapping[str, object], Mapping[str, object]]]] = {
            relation: [] for relation in range(relationship_count)}
        for index, (item, assignment_row) in enumerate(zip(stream, assignment_rows)):
            if (not isinstance(item, Mapping) or not isinstance(assignment_row, Mapping) or
                    type(item.get("ordinal")) is not int or
                    item.get("ordinal") != assignment_row.get("dispatch_order")):
                raise MatrixError(stream_label + ":identity_invalid")
            relative = item.get("source_relative")
            if (not isinstance(relative, str) or Path(relative).is_absolute() or
                    any(part in ("", ".", "..") for part in Path(relative).parts)):
                raise MatrixError(stream_label + ":input_descriptor_invalid")
            try:
                relation = int(assignment_row["f_relationship"])
            except (KeyError, TypeError, ValueError) as exc:
                raise MatrixError(stream_label + ":assignment_row_invalid") from exc
            if relation < 0 or relation >= relationship_count:
                raise MatrixError(stream_label + ":assignment_row_invalid")
            entries[relation].append((index, item, assignment_row))
        return entries, relationship_count

    if type(max_history_bytes) is not int or max_history_bytes <= 0:
        raise MatrixError(label + ":history_bound_invalid")
    entries, relationship_count = build_entries(selected, assignment, label)
    predecessor_entries: dict[int, list[tuple[int, Mapping[str, object], Mapping[str, object]]]] = {}
    if predecessor_selected is not None:
        predecessor_entries, predecessor_count = build_entries(
            predecessor_selected, predecessor_assignment, label + "_predecessor")
        if predecessor_count != relationship_count:
            raise MatrixError(label + ":predecessor_topology_invalid")
    observations: dict[int, dict[str, object]] = {}
    final: dict[str, object] = {}
    facts_by_index: dict[int, dict[str, object]] = {}
    effective_limit = min(max_history_bytes, 1 << MAX_HISTORY_WINDOW_LOG)
    for relation, relation_entries in entries.items():
        relationship_key = "C0|F" + str(relation)
        history = bytearray()
        before_descriptor = _prefix_descriptor(history)
        for is_current, stream_entries in ((False, predecessor_entries.get(relation, [])),
                                           (True, relation_entries)):
            for index, item, _assignment_row in stream_entries:
                relative = str(item["source_relative"])
                path = root / relative
                try:
                    if path.resolve(strict=True).relative_to(root) != Path(relative):
                        raise MatrixError(label + ":path_outside_root")
                except (OSError, ValueError) as exc:
                    raise MatrixError(label + ":path_outside_root") from exc
                raw, file_facts = _private_bytes(path, label + "_input")
                if (file_facts["sha256"] != item.get("sha256") or
                        file_facts["bytes"] != item.get("bytes")):
                    raise MatrixError(label + ":input_mutated")
                before = before_descriptor
                _append_bounded_history_in_place(history, raw, effective_limit)
                after = _prefix_descriptor(history)
                before_descriptor = after
                if is_current:
                    observations[item["ordinal"]] = {"relationship_key": ["C0", "F" + str(relation)],
                                           "before": before, "after": after}
                    facts_by_index[index] = {
                        "ordinal": item.get("ordinal"), "build": item.get("build"),
                        "logical": item.get("logical"), "source_relative": relative,
                        "bytes": file_facts["bytes"], "sha256": file_facts["sha256"],
                        "source_path": str(path), "source_digest128": _digest128(raw)}
        final[relationship_key] = _prefix_descriptor(history)
    facts = [facts_by_index[index] for index in range(len(selected))]
    return observations, final, facts


def _authenticated_assignment(topology_id: str, count: int, *, start: int = 0) -> dict[str, object]:
    if type(start) is not int or start < 0:
        raise MatrixError("assignment:start_invalid")
    path = AUTH_ASSIGNMENT_PATHS[topology_id]
    assignment_raw, facts = _private_bytes(path, "assignment")
    if facts["sha256"] != AUTH_ASSIGNMENT_SHA256[topology_id]:
        raise MatrixError("assignment:authenticated_digest_mismatch")
    rows = list(csv.DictReader(assignment_raw.decode("utf-8").splitlines(), delimiter="\t"))
    if not rows or len(rows) < start + count:
        raise MatrixError("assignment:too_short")
    selected: list[dict[str, object]] = []
    expected_relations = 1 if topology_id == "C1F1/100000" else 20
    for ordinal, row in enumerate(rows[start:start + count]):
        try:
            dispatch_order = int(row["dispatch_order"])
            authority_tu_seq = int(row.get("tu_seq", dispatch_order))
            if dispatch_order != start + ordinal or authority_tu_seq != dispatch_order:
                raise MatrixError("assignment:dispatch_order_invalid")
            relation = int(row["worker"])
            # C1F20's two dispatch lanes are authenticated in compiler_slot;
            # C1F1's retained map has one relationship and no lane column.
            slot = int(row.get("compiler_slot", row.get("slot", "0")))
        except (KeyError, TypeError, ValueError) as exc:
            raise MatrixError("assignment:row_invalid") from exc
        if not 0 <= relation < expected_relations:
            raise MatrixError("assignment:relationship_invalid")
        # The retained dispatch sequence is the authority.  Decode its global
        # slot with the product contract: C1F20 relation=global_slot//2 and
        # per-F slot=global_slot%2; C1F1 has one relationship and 100000 slots.
        global_slot = relation * (2 if expected_relations == 20 else 100000) + slot
        expected_relation = global_slot // 2 if expected_relations == 20 else 0
        expected_slot = global_slot % 2 if expected_relations == 20 else global_slot
        if expected_relation >= expected_relations:
            raise MatrixError("assignment:global_slot_outside_relationships")
        try:
            authority_rel_seq = (int(row["rel_seq"]) if row.get("rel_seq") is not None
                                 else None)
        except (TypeError, ValueError) as exc:
            raise MatrixError("assignment:relationship_sequence_invalid") from exc
        if authority_rel_seq is not None and authority_rel_seq < 0:
            raise MatrixError("assignment:relationship_sequence_invalid")
        selected.append({"ordinal": ordinal, "global_slot": global_slot,
                         "f_relationship": expected_relation, "per_f_slot": expected_slot,
                         "dispatch_order": dispatch_order, "authority_worker": relation,
                         "authority_slot": slot, "authority_build": int(row.get("build", 0)),
            "authority_logical": int(row.get("logical", ordinal)),
                         "authority_dispatch_order": dispatch_order,
                         "authority_tu_seq": authority_tu_seq,
                         "authority_rel_seq": authority_rel_seq})
    return {"path": facts["path"], "bytes": facts["bytes"], "sha256": facts["sha256"],
            "schema": "root-matrix-v4-assignment-tsv-v1", "topology": topology_id,
            "rows": selected, "selected_count": count, "authority_total_rows": len(rows),
            "authority_capacity_slots": (100000 if topology_id == "C1F1/100000" else 40)}


def _native_batch(occurrences: Sequence[Occurrence], topology: MatrixTopology,
                  assignment: Mapping[str, object], method: str,
                  *, predecessor_occurrences: Sequence[Occurrence] = (),
                  predecessor_assignment: Mapping[str, object] | None = None
                  ) -> dict[int, dict[str, object]]:
    """Run the actual endpoint transaction for every selected TU."""
    root = Path(__file__).resolve().parents[1]
    binary = root / "cache" / "sim" / ".p50sim.bin"
    if not binary.is_file():
        raise NotReady("MISSING_AUTHORITY: native p50sim binary")
    rows = assignment.get("rows")
    if not isinstance(rows, list) or len(rows) != len(occurrences):
        raise MatrixError("assignment authority/stream length mismatch")
    if predecessor_occurrences and (predecessor_assignment is None or
                                    not predecessor_assignment.get("rows")):
        raise MatrixError("repeat-full predecessor assignment authority is missing")
    predecessor_rows = (predecessor_assignment or {}).get("rows", [])
    if not isinstance(predecessor_rows, list) or len(predecessor_rows) != len(predecessor_occurrences):
        raise MatrixError("repeat-full predecessor assignment/stream length mismatch")
    transaction_count = len(occurrences) + len(predecessor_occurrences)
    timeout_seconds = _native_batch_timeout_seconds(transaction_count)
    with tempfile.TemporaryDirectory(prefix="s8-product-batch-") as scratch:
        scratch_path = Path(scratch)
        manifest = scratch_path / "inputs.manifest"
        mapping = scratch_path / "assignments.map"
        manifest_2 = scratch_path / "inputs-2.manifest"
        mapping_2 = scratch_path / "assignments-2.map"
        output = scratch_path / "output.jsonl"
        def write_segment(manifest_path: Path, mapping_path: Path,
                          segment_occurrences: Sequence[Occurrence],
                          segment_rows: list[object]) -> None:
            if any(not occurrence.source_path for occurrence in segment_occurrences):
                raise MatrixError("native batch requires authenticated source paths")
            manifest_path.write_text("".join(str(occurrence.source_path) + "\n"
                                               for occurrence in segment_occurrences),
                                     encoding="utf-8")
            mapping_path.write_text("cardinality=" + str(topology.relationship_count) + "\n" +
                                    "".join(str(int(item["f_relationship"])) + "\n"
                                            for item in segment_rows), encoding="ascii")
        write_segment(manifest, mapping, occurrences, rows)
        command = [str(binary), "--batch-manifest", str(manifest),
                   "--batch-assignment-map", str(mapping)]
        if predecessor_occurrences:
            write_segment(manifest_2, mapping_2, predecessor_occurrences, predecessor_rows)
            # The native endpoint processes its first manifest before its
            # second manifest; pass full-1 first so relationship state carries
            # into the measured full-2 segment.
            command = [str(binary), "--batch-manifest", str(manifest_2),
                       "--batch-assignment-map", str(mapping_2),
                       "--batch-manifest-2", str(manifest),
                       "--batch-assignment-map-2", str(mapping)]
        command += ["--batch-output", str(output), "--batch-allow-repeated-inputs", "1"]
        environment = dict(os.environ)
        environment["ICECC_P50_PROFILE"] = method
        try:
            completed = subprocess.run(
                command,
                env=environment, check=False,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                timeout=timeout_seconds)
        except subprocess.TimeoutExpired:
            raise NotReady(
                "native product batch timed out: "
                f"method={method} transactions={transaction_count} "
                f"timeout={timeout_seconds}s"
            ) from None
        if completed.returncode != 0:
            raise NotReady("native product transaction failed: " +
                           completed.stderr.decode("utf-8", "replace")[:300])
        result: dict[int, dict[str, object]] = {}
        predecessor_result: dict[int, dict[str, object]] = {}
        seen: set[tuple[str, int]] = set()
        # TU_SEQ is allocated by the C-wide product authority, so it is one
        # contiguous sequence over the interleaved C1F20 dispatch stream.
        # REL_SEQ remains relationship-local and is checked independently
        # against the authenticated assignment rows below.
        global_next_tu_seq = 0
        relation_next_rel_seq: dict[int, int] = {}
        relation_last_state: dict[int, str] = {}
        expected_profile = "GRZ_RESIDUAL" if method == "GRZ_RESIDUAL" else method
        stateful_method = method in STATEFUL_METHODS
        def validate_item(item: object, segment: str, index: int,
                          segment_occurrences: Sequence[Occurrence],
                          segment_rows: Sequence[object]) -> None:
            if not isinstance(item, Mapping) or item.get("schema") != "icecream-p50sim-batch-v1":
                raise MatrixError("native product output schema invalid")
            _validate_native_prefix_keys(item, method)
            marker = (segment, index)
            if marker in seen:
                raise MatrixError("native product output duplicate ordinal")
            seen.add(marker)
            if index < 0 or index >= len(segment_occurrences):
                raise MatrixError("native product output ordinal invalid")
            row = segment_rows[index]
            if not isinstance(row, Mapping):
                raise MatrixError("native product assignment row invalid")
            try:
                relation = int(row["f_relationship"])
            except (KeyError, TypeError, ValueError) as exc:
                raise MatrixError("native product assignment row invalid") from exc
            expected_relation_id = f"c1f{topology.relationship_count}-r{relation:02d}"
            if (type(item.get("tu_index")) is not int or
                    type(item.get("tu_seq")) is not int or
                    type(item.get("raw_bytes")) is not int or
                    item.get("segment") != segment or item.get("tu_index") != index or
                    item.get("profile") != expected_profile or
                    item.get("relationship_id") != expected_relation_id or
                    item.get("committed") is not True):
                raise MatrixError("native product output identity binding invalid")
            occurrence = segment_occurrences[index]
            raw_digest = occurrence.source_digest128
            if raw_digest is None:
                raw_digest = _digest128(occurrence.read_raw())
            if item.get("raw_bytes") != occurrence.byte_count() or item.get("raw_digest") != raw_digest:
                raise MatrixError("native product output raw binding invalid")
            digest_fields = ("state_before_digest", "state_digest", "transaction_digest")
            if any(not _digest128_text(item.get(field)) for field in digest_fields):
                raise MatrixError("native product output digest invalid")
            if method == "ZSTD_ROUTE" and (
                    not _valid_prefix_descriptor(item.get("committed_raw_prefix_before_descriptor")) or
                    not _valid_prefix_descriptor(item.get("committed_raw_prefix_descriptor"))):
                raise MatrixError("native product output route descriptor invalid")
            nonlocal global_next_tu_seq
            if item.get("tu_seq") != global_next_tu_seq:
                raise MatrixError("native product output global TU sequence invalid")
            authority_rel_seq = row.get("authority_rel_seq")
            if authority_rel_seq is not None and (
                    type(authority_rel_seq) is not int or authority_rel_seq < 0):
                raise MatrixError("native product assignment REL sequence invalid")
            if topology.relationship_count > 1 and authority_rel_seq is None:
                raise MatrixError("native product assignment REL sequence is missing")
            expected_rel_seq = (int(authority_rel_seq) if authority_rel_seq is not None
                                else relation_next_rel_seq.get(relation, 0))
            if expected_rel_seq != relation_next_rel_seq.get(relation, 0):
                raise MatrixError("native product relationship REL sequence invalid")
            # The product emits REL_SEQ independently of its C-wide TU_SEQ.
            # Do not reconstruct it from TU_SEQ; omission is not continuity.
            if (type(item.get("rel_seq")) is not int or
                    item.get("rel_seq") != expected_rel_seq):
                raise MatrixError("native product output relationship REL sequence invalid")
            before = str(item["state_before_digest"])
            if stateful_method and relation in relation_last_state and before != relation_last_state[relation]:
                raise MatrixError("native product output state continuity invalid")
            relation_next_rel_seq[relation] = expected_rel_seq + 1
            if stateful_method:
                relation_last_state[relation] = str(item["state_digest"])
            # These are in-memory continuity annotations.  They are consumed
            # by row/state assembly but never exposed as product evidence.
            item["_native_rel_seq"] = int(item["rel_seq"])  # type: ignore[index]
            item["_native_next_rel_seq"] = expected_rel_seq + 1  # type: ignore[index]
            item["_native_next_tu_seq"] = global_next_tu_seq + 1  # type: ignore[index]
            global_next_tu_seq += 1
        expected_segment = "full-2" if predecessor_occurrences else "full-1"
        output_raw, _output_facts = _private_bytes(output, "native_batch_output")
        try:
            output_lines = output_raw.decode("utf-8").splitlines()
        except UnicodeDecodeError as exc:
            raise MatrixError("native product output is not UTF-8") from exc
        for line in output_lines:
            item = _strict_json(line.encode("utf-8"), "native_batch_output")
            if not isinstance(item, Mapping):
                raise MatrixError("native product output schema invalid")
            segment = item.get("segment")
            if segment not in {"full-1", "full-2"}:
                raise MatrixError("native product output segment invalid")
            try:
                ordinal = int(item["tu_index"])
            except (KeyError, TypeError, ValueError) as exc:
                raise MatrixError("native product output ordinal invalid") from exc
            segment_occurrences = (predecessor_occurrences if segment == "full-1" and
                                   predecessor_occurrences else occurrences)
            segment_rows = predecessor_rows if segment == "full-1" and predecessor_occurrences else rows
            validate_item(item, segment, ordinal, segment_occurrences, segment_rows)
            if segment == expected_segment:
                result[ordinal] = item
            else:
                predecessor_result[ordinal] = item
        if predecessor_occurrences and set(predecessor_result) != set(range(len(predecessor_occurrences))):
            raise MatrixError("native product predecessor batch is incomplete or reordered")
        if set(result) != set(range(len(occurrences))):
            raise MatrixError("native product batch is incomplete or reordered")
        # The native endpoint numbers each manifest segment from zero.  The
        # experiment row retains the authenticated global dispatch identity,
        # so re-key the measured segment by occurrence ordinal at this seam.
        return {occurrences[index].ordinal: item for index, item in result.items()}


def _native_receipt(root: Path) -> tuple[dict[str, object] | None, dict[str, object]]:
    receipt_path = root / "cache" / "sim" / ".p50sim-build.json"
    binary = root / "cache" / "sim" / ".p50sim.bin"
    try:
        receipt_raw, receipt_facts_full = _private_bytes(receipt_path, "native p50sim build receipt")
    except MatrixError:
        return None, {"path": str(receipt_path), "available": False, "sha256": None}
    receipt_facts = {key: receipt_facts_full[key] for key in ("path", "bytes", "sha256")}
    try:
        receipt = _strict_json(receipt_raw, "native_receipt")
        if not isinstance(receipt, Mapping):
            raise MatrixError("native_receipt:schema_invalid")
        binary_facts = _private_digest(binary, "native p50sim binary")
        head = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"],
                                       text=True, timeout=10).strip()
        tree = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD^{tree}"],
                                       text=True, timeout=10).strip()
    except (OSError, json.JSONDecodeError, MatrixError):
        return None, receipt_facts
    declared_inputs = receipt.get("inputs", {})
    input_paths = {"p50sim_source": root / "cache" / "sim" / "p50sim.cpp",
                   "config_h": root / "config.h",
                   "cache_makefile": root / "cache" / "Makefile",
                   "services_makefile": root / "services" / "Makefile"}
    inputs_match = isinstance(declared_inputs, Mapping)
    for name, path in input_paths.items():
        if not inputs_match:
            break
        try:
            current = _private_digest(path, "native receipt input")
        except MatrixError:
            inputs_match = False
            break
        declared = declared_inputs.get(name)
        inputs_match = isinstance(declared, Mapping) and all(
            declared.get(field) == current.get(field) for field in ("path", "bytes", "sha256"))
    if (receipt.get("schema") != "icecream-p50sim-build-v1" or not inputs_match or
            receipt.get("source", {}).get("head") != head or
            receipt.get("source", {}).get("tree") != tree or
            receipt.get("binary", {}).get("path") != str(binary) or
            receipt.get("binary", {}).get("sha256") != binary_facts["sha256"] or
            receipt.get("binary", {}).get("bytes") != binary_facts["bytes"]):
        return None, receipt_facts
    return receipt, receipt_facts


def _libbsc_authority(root: Path) -> dict[str, object]:
    source = Path("/tanksmall/scratch/ictmp/libbsc-issue16")
    header = source / "libbsc" / "libbsc.h"
    library = source / "build-gcc2" / "libbsc.a"
    provenance = root / "vendor" / "libbsc" / "PROVENANCE"
    manifest = root / "vendor" / "libbsc" / "SOURCE-MANIFEST.sha256"
    try:
        head = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
        tree = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD^{tree}"], text=True).strip()
        header_facts = _private_digest(header, "libbsc_header")
        library_facts = _private_digest(library, "libbsc_library")
        provenance_facts = _private_digest(provenance, "libbsc_provenance")
        manifest_facts = _private_digest(manifest, "libbsc_manifest")
    except (OSError, subprocess.SubprocessError, MatrixError):
        return {"status": "NOT_READY", "reason": "MISSING_AUTHORITY: libbsc source/build facts"}
    status = (head == AUTH_LIBBSC_HEAD and tree == AUTH_LIBBSC_TREE and
              header_facts["sha256"] == AUTH_LIBBSC_HEADER_SHA256 and
              library_facts["sha256"] == AUTH_LIBBSC_LIBRARY_SHA256)
    return {"status": "READY" if status else "NOT_READY", "source_root": str(source),
            "head": head, "tree": tree,
            "archive": {"sha256": AUTH_LIBBSC_ARCHIVE_SHA256, "materialized": False,
                        "reproducible_from": "git archive --format=tar --prefix=libbsc-baffa62c70b6ebbecc9af14ce550e965ea247680/ "
                                             + AUTH_LIBBSC_HEAD,
                        "finding": "archive byte is not retained; SHA is provenance-only"},
            "archive_sha256": AUTH_LIBBSC_ARCHIVE_SHA256,
            "header": header_facts, "library": library_facts,
            "provenance": provenance_facts, "source_manifest": manifest_facts,
            "reason": None if status else "MISSING_AUTHORITY: libbsc hash mismatch"}


def method_authority(method: str) -> dict[str, object]:
    """Return the method's explicit implementation/status authority."""
    if method not in METHODS:
        raise MatrixError(f"unknown method: {method}")
    root = Path(__file__).resolve().parents[1]
    product = root / "cache" / "p50_zstd.cpp"
    product_facts = _authority_file(product, "authenticated product p50_zstd source")
    planner = root / "farmharness" / "s4_version_transition_planner.py"
    native_binary = root / "cache" / "sim" / ".p50sim.bin"
    receipt, receipt_facts = _native_receipt(root)
    libbsc = _libbsc_authority(root)
    if method == "RAW_II":
        return {"status": "READY", "kind": "whole-legacy-control",
                "authority": [_authority_file(product,
                    "RAW_II is a raw framed application-wire control; no zstd state")],
                "contract": "raw byte-size control only; endpoint wire/time is not witnessed"}
    if method == "ZSTD_TU":
        status = "READY" if product_facts.get("sha256") == AUTH_P50_ZSTD_SHA256 and receipt is not None else "NOT_READY"
        return {"status": status, "kind": "product-profile",
                "authority": [product_facts,
                    _authority_file(root / "cache" / "sim" / ".p50sim-build.json",
                    "native product codec receipt"),
                    _authority_file(native_binary, "native product codec binary"),
                    _authority_file(product,
                    "ZstdTuCodec resets session and parameters for each independent TU")],
                "contract": "independent level-3 frame/context per TU",
                "reason": None if status == "READY" else "MISSING_AUTHORITY: exact native receipt/binary/source"}
    if method == "ZSTD_ROUTE":
        status = "READY" if product_facts.get("sha256") == AUTH_P50_ZSTD_SHA256 and receipt is not None else "NOT_READY"
        return {"status": status, "kind": "product-profile",
                "authority": [product_facts,
                    _authority_file(root / "cache" / "sim" / ".p50sim-build.json",
                    "native product codec receipt"),
                    _authority_file(native_binary, "native product codec binary"),
                    _authority_file(product,
                    "ZstdRouteCodec resets a fresh level-3 frame and refPrefixes committed raw history")],
                "contract": "one bounded-prefix frame per TU; commit advances relationship state",
                "reason": None if status == "READY" else "MISSING_AUTHORITY: exact native receipt/binary/source"}
    if method in {"P29", "GRZ_RESIDUAL"}:
        ready = receipt is not None and (method != "GRZ_RESIDUAL" or
                                         receipt.get("configuration", {}).get("with_libbsc") == 1) and \
                (method != "GRZ_RESIDUAL" or libbsc.get("status") == "READY")
        return {"status": "READY" if ready else "NOT_READY", "kind": "product-profile",
                "authority": [product_facts,
                    receipt_facts, _authority_file(native_binary, "native product profile binary"),
                    *([libbsc] if method == "GRZ_RESIDUAL" else [])],
                "contract": "native p50sim profile batch; no Python approximation",
                "reason": None if ready else ("MISSING_AUTHORITY: authenticated libbsc build receipt"
                    if method == "GRZ_RESIDUAL" else "MISSING_AUTHORITY: native p50sim receipt")}
    if method == "ZSTD_COHORT":
        return {"status": "NOT_READY", "kind": "unaccepted-extra",
                "authority": [_authority_file(planner,
                    "OPTIONAL_UNACCEPTED_METHOD_EXTRAS marks cohort absent from the current profile registry")],
                "reason": "MISSING_AUTHORITY: exact immutable cohort dictionary construction is unspecified"}
    return {"status": "NOT_IMPLEMENTED", "kind": "unaccepted-extra",
            "authority": [_authority_file(planner,
                "OPTIONAL_UNACCEPTED_METHOD_EXTRAS marks global absent; global resource state is not a codec")],
            "reason": "MISSING_IMPLEMENTATION: no authoritative centralized dispatch-order codec"}


@dataclass(frozen=True)
class MatrixTopology:
    """Physical topology; ``slots_per_f`` never changes relationship count."""

    topology_id: str
    c_store_guid: str = "C0"
    f_store_guids: tuple[str, ...] = ()
    slots_per_f: int | None = None
    global_slots: int | None = None
    stream_capacity_tus: int | None = None

    def __post_init__(self) -> None:
        if self.topology_id not in TOPOLOGY_IDS:
            raise MatrixError(f"unsupported topology: {self.topology_id}")
        expected_f, expected_slots, expected_global, expected_capacity = (
            (1, 100000, 100000, 100000) if self.topology_id == "C1F1/100000"
            else (20, 2, 40, None))
        f_guids = self.f_store_guids or tuple(f"F{index}" for index in range(expected_f))
        object.__setattr__(self, "f_store_guids", tuple(f_guids))
        if (len(f_guids) != expected_f or len(set(f_guids)) != expected_f or
                any(not isinstance(value, str) or not value or "|" in value for value in f_guids)):
            raise MatrixError("topology must declare one unique F GUID per relationship")
        slots = expected_slots if self.slots_per_f is None else self.slots_per_f
        global_slots = expected_global if self.global_slots is None else self.global_slots
        if type(slots) is not int or slots <= 0 or type(global_slots) is not int or global_slots <= 0:
            raise MatrixError("topology capacities must be positive integers")
        if global_slots != expected_f * slots:
            raise MatrixError("global slots must equal F count multiplied by slots_per_f")
        object.__setattr__(self, "slots_per_f", slots)
        object.__setattr__(self, "global_slots", global_slots)
        if self.stream_capacity_tus is None:
            object.__setattr__(self, "stream_capacity_tus", expected_capacity)

    @classmethod
    def from_id(cls, topology_id: str, **kwargs: object) -> "MatrixTopology":
        return cls(topology_id, **kwargs)

    @property
    def relationship_count(self) -> int:
        return len(self.f_store_guids)

    @property
    def relationship_keys(self) -> tuple[tuple[str, str], ...]:
        return tuple((self.c_store_guid, guid) for guid in self.f_store_guids)

    def relationship_for(self, f_store_guid: str, slot: int = 0) -> tuple[str, str]:
        if f_store_guid not in self.f_store_guids:
            raise MatrixError(f"unknown F GUID: {f_store_guid}")
        if type(slot) is not int or not 0 <= slot < int(self.slots_per_f):
            raise MatrixError("slot is outside the F concurrency capacity")
        # Deliberately omit slot: capacity is concurrency, not state cardinality.
        return (self.c_store_guid, f_store_guid)

    def assignment(self, ordinal: int, f_store_guid: str | None = None,
                   slot: int = 0, authority: Mapping[str, object] | None = None) -> dict[str, object]:
        if type(ordinal) is not int or ordinal < 0:
            raise MatrixError("ordinal must be a non-negative integer")
        if authority is not None:
            rows = authority.get("rows")
            if not isinstance(rows, list) or ordinal >= len(rows) or not isinstance(rows[ordinal], Mapping):
                raise MatrixError("assignment authority does not cover ordinal")
            item = rows[ordinal]
            relation = int(item["f_relationship"])
            slot = int(item["per_f_slot"])
            if relation >= len(self.f_store_guids):
                raise MatrixError("assignment relationship outside topology")
            guid = self.f_store_guids[relation]
            if item.get("ordinal") != ordinal:
                raise MatrixError("assignment dispatch order changed")
        else:
            if f_store_guid is None:
                raise MatrixError("authenticated assignment authority is required")
            guid = f_store_guid
        key = self.relationship_for(guid, slot)
        return {"ordinal": ordinal, "f_store_guid": guid, "slot": slot,
                "relationship_key": list(key),
                "relationship_index": self.f_store_guids.index(guid),
                "global_slot": (self.f_store_guids.index(guid) * int(self.slots_per_f) + slot),
                "authority_worker": item.get("authority_worker") if authority is not None else None,
                "authority_slot": item.get("authority_slot") if authority is not None else None,
                "authority_build": item.get("authority_build") if authority is not None else None,
                "authority_logical": item.get("authority_logical") if authority is not None else None,
                "dispatch_order": item.get("dispatch_order") if authority is not None else None,
                "authority_dispatch_order": item.get("authority_dispatch_order")
                if authority is not None else None,
                "authority_tu_seq": item.get("authority_tu_seq")
                if authority is not None else None,
                "authority_rel_seq": item.get("authority_rel_seq")
                if authority is not None else None}


@dataclass(frozen=True)
class Occurrence:
    ordinal: int
    raw: bytes | None
    f_store_guid: str | None = None
    slot: int = 0
    route_id: str | None = None
    history_nonce: int = 1
    rel_seq: int | None = None
    commit: bool = True
    reset_before: bool = False
    release: bool = False
    source_relative: str | None = None
    source_sha256: str | None = None
    source_path: str | None = None
    source_build: int | None = None
    source_logical: int | None = None
    source_digest128: str | None = None

    def __post_init__(self) -> None:
        if type(self.ordinal) is not int or self.ordinal < 0:
            raise MatrixError("occurrence ordinal must be non-negative")
        if self.raw is not None and (not isinstance(self.raw, bytes) or not self.raw):
            raise MatrixError("occurrence raw payload must be non-empty bytes")
        if self.raw is None and not self.source_path:
            raise MatrixError("lazy occurrence requires an authenticated source path")
        if type(self.slot) is not int or self.slot < 0:
            raise MatrixError("occurrence slot must be non-negative")
        if type(self.history_nonce) is not int or self.history_nonce <= 0:
            raise MatrixError("history nonce must be positive")
        if self.rel_seq is not None and (type(self.rel_seq) is not int or self.rel_seq < 0):
            raise MatrixError("REL_SEQ must be a non-negative integer")
        if self.source_digest128 is not None and not _digest128_text(self.source_digest128):
            raise MatrixError("source digest128 is malformed")

    @classmethod
    def from_path(cls, ordinal: int, path: Path, **kwargs: object) -> "Occurrence":
        raw, facts = _private_bytes(path, "occurrence")
        expected_size = kwargs.pop("expected_size", None)
        expected_sha256 = kwargs.pop("expected_sha256", None)
        if expected_size is not None and len(raw) != expected_size:
            raise MatrixError("authenticated input size changed")
        digest = str(facts["sha256"])
        if expected_sha256 is not None and digest != expected_sha256:
            raise MatrixError("authenticated input digest changed")
        return cls(ordinal, raw, source_relative=kwargs.pop("source_relative", None),
                   source_sha256=digest, source_path=str(path),
                   source_digest128=_digest128(raw), **kwargs)

    def byte_count(self) -> int:
        if self.raw is not None:
            return len(self.raw)
        return int(os.stat(self.source_path, follow_symlinks=False).st_size)  # type: ignore[arg-type]

    def read_raw(self) -> bytes:
        if self.raw is not None:
            return self.raw
        raw, facts = _private_bytes(Path(self.source_path), "occurrence")  # type: ignore[arg-type]
        if facts["sha256"] != self.source_sha256:
            raise MatrixError("authenticated input changed")
        return raw


@dataclass
class _RelationshipState:
    key: tuple[str, str]
    history: bytes | bytearray = b""
    next_rel_seq: int = 0
    last_route_id: str | None = None
    history_nonce: int = 1
    reset_count: int = 0
    pending: bool = False
    native_last_tu_seq: int | None = None
    native_next_rel_seq: int | None = None
    native_state_digest: str | None = None
    prefix_descriptor: dict[str, object] | None = None


def assign_relationships(topology: MatrixTopology, occurrences: Sequence[Occurrence],
                        authority: Mapping[str, object] | None = None) -> list[dict[str, object]]:
    """Deterministically map ordered TUs to C/F relationships."""
    assignments = []
    for stream_index, occurrence in enumerate(occurrences):
        # Assignment ordinal is local to this authenticated segment.  The
        # occurrence retains the global dispatch/tu_seq identity separately.
        assignments.append(topology.assignment(stream_index,
                                                occurrence.f_store_guid,
                                                occurrence.slot, authority))
    return assignments


def repeat_full_state_contract(method: str) -> dict[str, object]:
    """State permitted to survive full-1 into repeat-full."""
    if method in STATEFUL_METHODS:
        fields = ["c_authorities.native_next_tu_seq", "native_last_tu_seq",
                  "native_next_rel_seq", "native_state_digest",
                  "history_nonce", "route_identity"]
        if method == "ZSTD_ROUTE":
            fields.append("committed_raw_prefix_descriptor")
        return {"survives": True, "fields": fields}
    if method == "ZSTD_COHORT":
        return {"survives": True, "fields": ["cohort_dictionary_identity",
                                               "committed_relationship_continuation"]}
    return {"survives": False, "fields": []}


class MethodMatrixSimulator:
    """Run one ordered occurrence stream over explicit method states."""

    def __init__(self, topology: MatrixTopology, *, methods: Iterable[str] = METHODS,
                 max_history_bytes: int = DEFAULT_HISTORY_BYTES,
                 cohort_dictionary: bytes | None = None,
                 cohort_authority: Mapping[str, object] | None = None,
                 assignment_authority: Mapping[str, object] | None = None) -> None:
        if type(max_history_bytes) is not int or max_history_bytes <= 0:
            raise MatrixError("max_history_bytes must be positive")
        self.topology = topology
        self.methods = tuple(methods)
        if not self.methods or any(method not in METHODS for method in self.methods):
            raise MatrixError("methods contain an unknown or empty method")
        self.max_history_bytes = min(max_history_bytes, 1 << MAX_HISTORY_WINDOW_LOG)
        self.cohort_dictionary = cohort_dictionary
        self.cohort_authority = dict(cohort_authority or {})
        self.assignment_authority = dict(assignment_authority or {
            "status": "UNBOUND", "topology": topology.topology_id})
        self._native_rows: dict[str, dict[int, dict[str, object]]] = {}
        if cohort_dictionary is not None:
            if not cohort_dictionary or self.cohort_authority.get("status") != "READY":
                raise NotReady("ZSTD_COHORT requires an independently authenticated dictionary authority")
            declared = str(self.cohort_authority.get("sha256", "")).lower()
            construction = self.cohort_authority.get("construction")
            if declared != _sha256(cohort_dictionary) or not isinstance(construction, str) or not construction:
                raise NotReady("ZSTD_COHORT dictionary authority digest/construction is incomplete")
        self.authority = {method: method_authority(method) for method in self.methods}
        if "ZSTD_COHORT" in self.methods and cohort_dictionary is not None:
            self.authority["ZSTD_COHORT"] = {
                "status": "READY", "kind": "authenticated-cohort",
                "authority": [self.cohort_authority],
                "contract": "immutable shared dictionary plus independent relationship continuation",
            }

    def _native_codec(self, method: str, raw: bytes, state: _RelationshipState) -> tuple[bytes, int, int]:
        root = Path(__file__).resolve().parents[1]
        binary = root / "cache" / "sim" / ".p50sim.bin"
        receipt_path = binary.parent / ".p50sim-build.json"
        try:
            info = binary.lstat()
            receipt_raw, _receipt_facts = _private_bytes(receipt_path, "native p50sim build receipt")
            receipt = _strict_json(receipt_raw, "native_receipt")
            if not isinstance(receipt, Mapping):
                raise MatrixError("native_receipt:schema_invalid")
        except (OSError, json.JSONDecodeError, MatrixError) as exc:
            raise NotReady("MISSING_AUTHORITY: native p50sim binary/receipt") from exc
        if (stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode) or
                info.st_nlink != 1 or not os.access(binary, os.X_OK) or
                receipt.get("schema") != "icecream-p50sim-build-v1" or
                receipt.get("binary", {}).get("path") != str(binary) or
                receipt.get("binary", {}).get("sha256") != _private_digest(
                    binary, "native p50sim binary")["sha256"]):
            raise NotReady("MISSING_AUTHORITY: native p50sim receipt is stale")
        prefix_file = None
        input_file = None
        output_file = None
        try:
            with tempfile.TemporaryDirectory(prefix="s8-native-codec-") as scratch:
                input_file = Path(scratch) / "input.bin"
                output_file = Path(scratch) / "encoded.bin"
                input_file.write_bytes(raw)
                args = [str(binary), "--codec-method", method, "--input", str(input_file),
                        "--codec-output", str(output_file), "--codec-rel-seq",
                        str(state.next_rel_seq), "--codec-tu-seq", str(state.next_rel_seq)]
                if method == "ZSTD_ROUTE" and state.history:
                    prefix_file = Path(scratch) / "prefix.bin"
                    prefix_file.write_bytes(state.history)
                    args += ["--codec-prefix", str(prefix_file)]
                started_cpu, started_wall = time.process_time_ns(), time.perf_counter_ns()
                completed = subprocess.run(args, check=False, stdout=subprocess.PIPE,
                                           stderr=subprocess.PIPE, timeout=120)
                if completed.returncode != 0 or not output_file.is_file():
                    raise NotReady("native product codec rejected authenticated input: " +
                                   completed.stderr.decode("utf-8", "replace")[:200])
                encoded, _encoded_facts = _private_bytes(output_file, "native codec output")
                return encoded, time.process_time_ns() - started_cpu, time.perf_counter_ns() - started_wall
        except (OSError, subprocess.SubprocessError) as exc:
            raise NotReady("native product codec unavailable") from exc

    def _encode(self, method: str, raw: bytes, state: _RelationshipState) -> tuple[bytes, int, int]:
        authority = self.authority[method]
        if authority["status"] == "NOT_IMPLEMENTED":
            raise NotReady("ZSTD_GLOBAL is NOT_IMPLEMENTED and is not aliased")
        if authority["status"] == "NOT_READY":
            raise NotReady(str(authority.get("reason", "method authority unavailable")))
        if method == "RAW_II":
            return b"", 0, 0
        if method in {"ZSTD_TU", "ZSTD_ROUTE"}:
            return self._native_codec(method, raw, state)
        raise NotReady("cohort codec has no accepted native authority")

    def run(self, occurrences: Sequence[Occurrence], *, output_root: Path | None = None,
            timestamp: str | None = None, repeat_full: bool = False,
            prior_state: Mapping[str, object] | None = None,
            predecessor_occurrences: Sequence[Occurrence] = (),
            predecessor_assignment_authority: Mapping[str, object] | None = None,
            depth: str | None = None, pass_id: str | None = None
            ) -> Path | dict[str, object]:
        if repeat_full and prior_state is None:
            raise MatrixError("repeat-full requires explicit full-1 state")
        if repeat_full and (not predecessor_occurrences or
                            predecessor_assignment_authority is None):
            raise MatrixError("repeat-full requires authenticated full-1 inputs and assignment")
        if self.assignment_authority.get("status") == "UNBOUND":
            start = occurrences[0].ordinal if occurrences else 0
            self.assignment_authority = _authenticated_assignment(
                self.topology.topology_id, len(occurrences), start=start)
        if (self.assignment_authority.get("topology") != self.topology.topology_id or
                int(self.assignment_authority.get("selected_count", len(occurrences))) < len(occurrences)):
            raise MatrixError("assignment authority does not cover stream")
        assignments = assign_relationships(self.topology, occurrences, self.assignment_authority)
        if occurrences and all(occurrence.source_path for occurrence in occurrences):
            for method in self.methods:
                if method in {"ZSTD_TU", "P29", "GRZ_RESIDUAL", "ZSTD_ROUTE"} and \
                        self.authority[method]["status"] == "READY":
                    self._native_rows[method] = _native_batch(
                        occurrences, self.topology, self.assignment_authority, method,
                        predecessor_occurrences=predecessor_occurrences if repeat_full else (),
                        predecessor_assignment=predecessor_assignment_authority if repeat_full else None)
        # Relationship state is method-local.  Sharing this map across methods
        # would make a route prefix leak into a cohort/TU arm and falsely turn
        # distinct methods into aliases.
        states_by_method = {
            method: {key: _RelationshipState(key) for key in self.topology.relationship_keys}
            for method in self.methods
        }
        if repeat_full:
            allowed = {method for method in self.methods if repeat_full_state_contract(method)["survives"]}
            carried = prior_state.get("relationships", {})
            if not isinstance(carried, Mapping):
                raise MatrixError("repeat-full prior relationship state is malformed")
            carried_c_authorities = prior_state.get("c_authorities", {})
            if not isinstance(carried_c_authorities, Mapping):
                raise MatrixError("repeat-full prior C authority state is missing")
            for method in allowed:
                # State is method-specific; only the declared surviving route
                # continuation is loaded.  TU/RAW state is intentionally absent.
                values = carried.get(method, {})
                if not isinstance(values, Mapping):
                    raise MatrixError("repeat-full method state is malformed")
                for key_text, value in values.items():
                    key = tuple(str(key_text).split("|", 1))
                    if key not in states_by_method[method] or not isinstance(value, Mapping):
                        raise MatrixError("repeat-full relationship identity is invalid")
                    # Native full2 owns route continuation inside one p50sim
                    # process; Python never carries its prefix payload.  The
                    # bounded in-memory hook exists only for synthetic tests.
                    history = (value.get("_runtime_history", b"")
                               if not (method == "ZSTD_ROUTE" and
                                       self._native_rows.get(method)) else b"")
                    if (not isinstance(history, (bytes, bytearray)) or
                            len(history) > self.max_history_bytes):
                        raise MatrixError("repeat-full committed prefix exceeds bound")
                    states_by_method[method][key].history = history
                    if method == "ZSTD_ROUTE":
                        descriptor = value.get("committed_raw_prefix_descriptor")
                        if _valid_prefix_descriptor(descriptor):
                            states_by_method[method][key].prefix_descriptor = dict(descriptor)
                    states_by_method[method][key].next_rel_seq = int(value.get("next_rel_seq", 0))
                    if method in STATEFUL_METHODS:
                        native_seq = value.get("native_last_tu_seq")
                        native_next_rel = value.get("native_next_rel_seq")
                        native_digest = value.get("native_state_digest")
                        if native_seq is not None:
                            if (type(native_seq) is not int or type(native_next_rel) is not int or
                                    native_next_rel <= 0 or
                                    not _digest128_text(native_digest)):
                                raise MatrixError("repeat-full native relationship state is malformed")
                            states_by_method[method][key].native_last_tu_seq = native_seq
                            states_by_method[method][key].native_next_rel_seq = native_next_rel
                            states_by_method[method][key].native_state_digest = str(native_digest)

            # The native endpoint is the authority for a product continuation,
            # but the retained full-1 summary must bind that continuation too.
            for method in STATEFUL_METHODS & set(self.methods):
                prior_relationships = carried.get(method, {})
                if not isinstance(prior_relationships, Mapping):
                    raise MatrixError("repeat-full native predecessor state is missing")
                c_authority = carried_c_authorities.get(method)
                first_product = (self._native_rows.get(method, {}).get(occurrences[0].ordinal)
                                 if occurrences else None)
                if first_product is not None:
                    if (not isinstance(c_authority, Mapping) or
                            type(c_authority.get("native_next_tu_seq")) is not int or
                            c_authority["native_next_tu_seq"] <= 0 or
                            first_product.get("tu_seq") != c_authority["native_next_tu_seq"]):
                        raise MatrixError("repeat-full native C-wide TU successor is invalid")
                checked: set[tuple[str, str]] = set()
                for occurrence, assignment in zip(occurrences, assignments):
                    key = tuple(assignment["relationship_key"])
                    if key in checked:
                        continue
                    product = self._native_rows.get(method, {}).get(occurrence.ordinal)
                    if product is None:
                        continue
                    checked.add(key)
                    prior = prior_relationships.get("|".join(key))
                    if (isinstance(prior, Mapping) and
                            prior.get("native_state_digest") is None):
                        # Legacy in-memory controls may provide only the
                        # Python route state; persisted full-1 experiments are
                        # required to carry native fields by the predecessor
                        # verifier above.
                        continue
                    if (not isinstance(prior, Mapping) or
                            prior.get("native_next_rel_seq") != product.get("_native_rel_seq") or
                            prior.get("native_state_digest") != product.get("state_before_digest")):
                        raise MatrixError("repeat-full native predecessor successor is invalid")

        if output_root is None:
            return self._run_memory(occurrences, assignments, states_by_method)
        depth_label = depth or f"count-{len(occurrences)}"
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", depth_label):
            raise MatrixError("experiment depth identity is malformed")
        pass_label = pass_id or "pass-1"
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", pass_label):
            raise MatrixError("experiment pass identity is malformed")
        run_timestamp = timestamp or _stamp()
        run_label = run_timestamp + "-" + self.topology.topology_id.replace("/", "-") + \
            "-" + depth_label + "-" + pass_label
        experiment = output_root.absolute() / run_label
        suffix = 1
        while experiment.exists():
            experiment = output_root.absolute() / f"{run_label}-r{suffix:02d}"
            suffix += 1
        experiment.mkdir(parents=True)
        (experiment / "bytes").mkdir()
        rows: list[dict[str, object]] = []
        for occurrence, assignment in zip(occurrences, assignments):
            raw_cache = occurrence.raw if occurrence.source_path is None else None
            for method in self.methods:
                if method != "RAW_II" and raw_cache is None:
                    raw_cache = occurrence.read_raw()
                rows.append(self._run_occurrence(experiment, occurrence, assignment,
                                                 method, states_by_method[method][tuple(assignment["relationship_key"])],
                                                 raw_override=raw_cache))
        with (experiment / "occurrences.jsonl").open("wb") as stream:
            for row in rows:
                stream.write(_canonical(row))
        summary = self._summary(rows, experiment, states_by_method)
        (experiment / "summary.json").write_bytes(_canonical(summary))
        descriptors = {}
        for path in sorted(p for p in experiment.rglob("*") if p.is_file() and p.name != "manifest.json"):
            raw = path.read_bytes()
            descriptors[str(path.relative_to(experiment))] = {
                "bytes": len(raw), "sha256": _sha256(raw)}
        manifest = {"schema": SCHEMA, "experiment": experiment.name,
                    "topology": self._topology_record(), "methods": list(self.methods),
                    "authority": self.authority, "repeat_full": repeat_full,
                    "repeat_full_state_contract": {
                        method: repeat_full_state_contract(method) for method in self.methods},
                    "relationship_count": self.topology.relationship_count,
                    "capacity_is_concurrency": True,
                    "assignment_authority": self.assignment_authority,
                    "input_authority": {"trace_sha256": AUTH_TRACE_SHA256,
                                        "corpus_manifest_sha256": AUTH_CORPUS_MANIFEST_SHA256,
                                        "corpus_root": str(AUTH_CORPUS_ROOT),
                                        "selected_inputs": [{"ordinal": o.ordinal,
                                            "build": o.source_build, "logical": o.source_logical,
                                            "source_relative": o.source_relative,
                                            "bytes": o.byte_count(), "sha256": o.source_sha256}
                                            for o in occurrences]},
                    "run_identity": {"timestamp": run_timestamp,
                                     "topology": self.topology.topology_id,
                                     "depth": depth_label, "pass": pass_label},
                    "predecessor_input_authority": ({"selected_inputs": [
                        {"ordinal": o.ordinal, "build": o.source_build,
                         "logical": o.source_logical, "source_relative": o.source_relative,
                         "bytes": o.byte_count(), "sha256": o.source_sha256}
                        for o in predecessor_occurrences],
                        "assignment": predecessor_assignment_authority}
                        if predecessor_occurrences else None),
                    "artifacts": descriptors,
                    "row_order": [{"ordinal": row["ordinal"], "method": row["method"]}
                                  for row in rows]}
        (experiment / "manifest.json").write_bytes(_canonical(manifest))
        return experiment

    def _run_memory(self, occurrences: Sequence[Occurrence], assignments: Sequence[dict[str, object]],
                    states_by_method: dict[str, dict[tuple[str, str], _RelationshipState]]) -> dict[str, object]:
        rows: list[dict[str, object]] = []
        for occurrence, assignment in zip(occurrences, assignments):
            raw_cache = occurrence.raw if occurrence.source_path is None else None
            for method in self.methods:
                key = tuple(assignment["relationship_key"])
                if method != "RAW_II" and raw_cache is None:
                    raw_cache = occurrence.read_raw()
                rows.append(self._run_occurrence(
                    None, occurrence, assignment, method, states_by_method[method][key],
                    raw_override=raw_cache))
        return {"schema": SUMMARY_SCHEMA, "status": self._status(rows),
                "core_completion": self._completion(rows, CORE_METHODS),
                "optional_completion": self._completion(rows, set(self.methods) - CORE_METHODS),
                "topology": self._topology_record(), "rows": rows,
                "relationship_count": self.topology.relationship_count,
                "method_status": {method: self.authority[method]["status"] for method in self.methods}}

    def _run_occurrence(self, experiment: Path | None, occurrence: Occurrence,
                        assignment: Mapping[str, object], method: str,
                        state: _RelationshipState,
                        raw_override: bytes | None = None) -> dict[str, object]:
        key = tuple(assignment["relationship_key"])  # type: ignore[arg-type]
        pre = _state_digest(state.history, state.next_rel_seq,
                            state.last_route_id, state.history_nonce)
        raw_path = encoded_path = None
        raw = (None if method == "RAW_II" and occurrence.source_path else
               raw_override if raw_override is not None else occurrence.read_raw())
        raw_bytes = occurrence.byte_count()
        raw_sha = occurrence.source_sha256 or _sha256(raw or b"")
        # Raw corpus payloads remain at their authenticated immutable source;
        # experiment evidence retains source path/size/hash descriptors only.
        if experiment is not None and method != "RAW_II":
            (experiment / "bytes" / method).mkdir(exist_ok=True)
        row: dict[str, object] = {"schema": OCCURRENCE_SCHEMA, "method": method,
            "topology": self.topology.topology_id, "ordinal": occurrence.ordinal,
            "relationship_key": list(key), "relationship_index": assignment["relationship_index"],
            "slot": assignment["slot"], "global_slot": assignment["global_slot"],
            "authority_worker": assignment.get("authority_worker"),
            "authority_slot": assignment.get("authority_slot"),
            "authority_build": assignment.get("authority_build"),
            "authority_logical": assignment.get("authority_logical"),
            "authority_dispatch_order": assignment.get("dispatch_order"),
            "raw_bytes": raw_bytes, "raw_sha256": raw_sha,
            "source_relative": occurrence.source_relative, "source_sha256": occurrence.source_sha256,
            "raw_path": str(raw_path.relative_to(experiment)) if raw_path and experiment else None,
            "status": self.authority[method]["status"], "committed": False,
            "measurement_scope": ("raw_bytes_only_no_wire_witness" if method == "RAW_II"
                                   else "native_codec_witnessed"),
            "wire_witnessed": False,
            "encoded_bytes": None, "encoded_sha256": None, "encoded_path": None,
            "codec_cpu_ns": None, "codec_wall_ns": None, "transition": "not_run",
            "native_tu_seq": None, "native_next_tu_seq": None,
            "native_next_rel_seq": None,
            "native_state_before_digest": None, "native_state_digest": None,
            "native_transaction_digest": None,
            "pre_state_digest": pre, "post_state_digest": pre,
            "authority": self.authority[method]}
        if self.authority[method]["status"] not in {"READY"}:
            return row
        if state.pending:
            raise MatrixError("pending preparation blocks until explicit release")
        # Preparation is tentative until the terminal commit witness arrives.
        # Encode against a copy so a rejected candidate cannot rebind a route,
        # consume a reset nonce, or advance REL_SEQ.
        history_copy = (bytearray(state.history) if isinstance(state.history, bytearray)
                        and not occurrence.commit else state.history)
        encode_state = _RelationshipState(key, history_copy, state.next_rel_seq,
                                          state.last_route_id, state.history_nonce,
                                          state.reset_count, state.pending)
        if method in {"ZSTD_ROUTE", "ZSTD_COHORT"}:
            route_id = occurrence.route_id or f"{key[0]}->{key[1]}"
            if occurrence.reset_before:
                if occurrence.history_nonce <= state.history_nonce:
                    raise MatrixError("route reset must advance history nonce")
                encode_state.history = b""
                encode_state.next_rel_seq = 0
                encode_state.reset_count += 1
                encode_state.history_nonce = occurrence.history_nonce
                encode_state.last_route_id = route_id
            elif state.last_route_id is not None and (route_id != state.last_route_id or
                                                       occurrence.history_nonce != state.history_nonce):
                raise MatrixError("route rebind requires an explicit advancing reset")
            else:
                encode_state.last_route_id, encode_state.history_nonce = route_id, occurrence.history_nonce
            if occurrence.rel_seq is not None and occurrence.rel_seq != encode_state.next_rel_seq:
                raise MatrixError(
                    f"route order expected REL_SEQ {encode_state.next_rel_seq}, got {occurrence.rel_seq}"
                )
        product = self._native_rows.get(method, {}).get(occurrence.ordinal)
        if method in {"P29", "GRZ_RESIDUAL"} and product is None:
            row["status"] = "NOT_READY"
            row["reason"] = "native batch requires authenticated source-path inputs"
            row["authority"] = self.authority[method]
            return row
        if product is not None:
            # Batch endpoint output is the authoritative full-stream
            # measurement.  Do not launch a second native process per TU or
            # materialize 75GB of Firefox payloads; bounded codec mode remains
            # available for focused parity probes.
            encoded = b""
            cpu_ns = int(product["simulator_execution_ns"])
            wall_ns = cpu_ns
            row["measurement_scope"] = "native_endpoint_transaction"
            row["wire_witnessed"] = True
            native_tu_seq = int(product["tu_seq"])
            native_rel_seq = product.get("_native_rel_seq")
            if native_rel_seq is None:
                # Directly injected bounded canaries predate the batch
                # annotation; use only relationship-local Python state as a
                # compatibility fallback, never the global TU_SEQ.
                native_rel_seq = state.native_next_rel_seq
                if native_rel_seq is None:
                    native_rel_seq = state.next_rel_seq
            if type(native_rel_seq) is not int or native_rel_seq < 0:
                raise MatrixError("native product relationship REL sequence is missing")
            native_next_tu_seq = product.get("_native_next_tu_seq", native_tu_seq + 1)
            if type(native_next_tu_seq) is not int or native_next_tu_seq != native_tu_seq + 1:
                raise MatrixError("native product global TU continuation is invalid")
            native_next_rel_seq = product.get("_native_next_rel_seq", native_rel_seq + 1)
            if type(native_next_rel_seq) is not int or native_next_rel_seq != native_rel_seq + 1:
                raise MatrixError("native product relationship REL continuation is invalid")
            row.update({"native_tu_seq": native_tu_seq,
                        "native_next_tu_seq": native_next_tu_seq,
                        "native_next_rel_seq": native_next_rel_seq,
                        "native_state_before_digest": product["state_before_digest"],
                        "native_state_digest": product["state_digest"],
                        "native_transaction_digest": product["transaction_digest"]})
            transaction = {key: value for key, value in product.items()
                           if not str(key).startswith("_")}
            transaction.update({"native_next_tu_seq": native_next_tu_seq,
                                "native_next_rel_seq": native_next_rel_seq,
                                "history_nonce": occurrence.history_nonce,
                                "route_identity": f"{key[0]}->{key[1]}"})
            native_route_descriptor = (method == "ZSTD_ROUTE" and
                                       product.get("profile") == "ZSTD_ROUTE")
            if native_route_descriptor:
                if (not _valid_prefix_descriptor(product.get("committed_raw_prefix_before_descriptor")) or
                        not _valid_prefix_descriptor(product.get("committed_raw_prefix_descriptor"))):
                    raise MatrixError("native route descriptor is missing or malformed")
                transaction.update({
                    "committed_raw_prefix_before_descriptor":
                        product["committed_raw_prefix_before_descriptor"],
                    "committed_raw_prefix_descriptor": product["committed_raw_prefix_descriptor"]})
            elif method == "ZSTD_ROUTE":
                # The transaction's ordinary prefix fields are overwritten
                # with post-commit state below.  Preserve the authenticated
                # pre-state separately so repeat-full can bind the first
                # current row to the predecessor without rereading payloads.
                prefix_before = state.history
                transaction.update({"committed_raw_prefix_before_descriptor":
                                    _prefix_descriptor(prefix_before),
                                    "committed_raw_prefix_descriptor":
                                    _prefix_descriptor(prefix_before)})
            row["product_transaction"] = transaction
            if method == "ZSTD_ROUTE" and native_route_descriptor:
                row["pre_state_digest"] = product["state_before_digest"]
                state.prefix_descriptor = dict(product["committed_raw_prefix_descriptor"])
            state.native_last_tu_seq = native_tu_seq
            state.native_next_rel_seq = native_next_rel_seq
            state.native_state_digest = str(product["state_digest"])
            if method in STATEFUL_METHODS:
                state.last_route_id = f"{key[0]}->{key[1]}"
                state.history_nonce = occurrence.history_nonce
                if method != "ZSTD_ROUTE":
                    state.next_rel_seq = native_next_rel_seq
        else:
            encoded, cpu_ns, wall_ns = self._encode(method, raw, encode_state)
        if experiment is not None and product is None and method != "RAW_II":
            encoded_path = experiment / "bytes" / method / f"encoded-{occurrence.ordinal:06d}.bin"
            encoded_path.write_bytes(encoded)
        row.update({"encoded_bytes": (None if method == "RAW_II" else
                                       int(product["encoded_source_bytes"]) if product is not None else len(encoded)),
                    "encoded_sha256": (None if method == "RAW_II" else
                                        _sha256(encoded) if product is None else None),
                    "encoded_path": str(encoded_path.relative_to(experiment))
                    if encoded_path and experiment else None,
                    "codec_cpu_ns": (None if method == "RAW_II" else cpu_ns),
                    "codec_wall_ns": (None if method == "RAW_II" else wall_ns)})
        if occurrence.commit:
            if method in {"ZSTD_ROUTE", "ZSTD_COHORT"}:
                native_route_descriptor = (method == "ZSTD_ROUTE" and product is not None and
                                           product.get("profile") == "ZSTD_ROUTE")
                if not native_route_descriptor:
                    encode_state.history = _append_bounded_history(
                        encode_state.history, raw, self.max_history_bytes)
                encode_state.next_rel_seq += 1
                if not native_route_descriptor:
                    state.history = encode_state.history
                state.next_rel_seq = encode_state.next_rel_seq
                state.reset_count = encode_state.reset_count
                state.history_nonce = encode_state.history_nonce
                state.last_route_id = encode_state.last_route_id
                state.pending = False
                row["transition"] = "committed_relationship_advance"
            elif method == "ZSTD_TU":
                row["transition"] = "committed_tu_reset"
            elif method in {"P29", "GRZ_RESIDUAL"}:
                row["transition"] = "committed_native_profile"
            else:
                row["transition"] = "committed_raw_control"
            state.pending = False
            row["committed"] = True
        else:
            row["transition"] = "tentative_not_committed"
            if not occurrence.release:
                state.pending = True
            else:
                state.pending = False
                row["transition"] = "tentative_discarded_explicit_release"
        if product is not None:
            transaction = row["product_transaction"]
            if isinstance(transaction, dict):
                transaction["committed_state_digest"] = state.native_state_digest
                if method == "ZSTD_ROUTE" and product.get("profile") != "ZSTD_ROUTE":
                    transaction["committed_raw_prefix_descriptor"] = _prefix_descriptor(state.history)
        post = _state_digest(state.history, state.next_rel_seq,
                             state.last_route_id, state.history_nonce)
        if product is not None and method == "ZSTD_ROUTE" and product.get("profile") == "ZSTD_ROUTE":
            post = str(product["state_digest"])
        row["post_state_digest"] = post
        return row

    def _topology_record(self) -> dict[str, object]:
        return {"id": self.topology.topology_id, "c_store_guid": self.topology.c_store_guid,
                "f_store_guids": list(self.topology.f_store_guids),
                "relationship_count": self.topology.relationship_count,
                "slots_per_f": self.topology.slots_per_f,
                "global_slots": self.topology.global_slots,
                "stream_capacity_tus": self.topology.stream_capacity_tus,
                "capacity_semantics": "concurrency_not_state_cardinality"}

    @staticmethod
    def _status(rows: Sequence[Mapping[str, object]]) -> str:
        core = [row for row in rows if row.get("method") in CORE_METHODS]
        optional = [row for row in rows if row.get("method") not in CORE_METHODS]
        present = {str(row.get("method")) for row in core}
        if present != CORE_METHODS or not all(row["status"] == "READY" for row in core):
            return "PARTIAL_NOT_READY"
        if optional and not all(row["status"] == "READY" for row in optional):
            return "CORE_COMPLETED_OPTIONALS_UNAVAILABLE"
        return "COMPLETED"

    @staticmethod
    def _completion(rows: Sequence[Mapping[str, object]], methods: set[str] | frozenset[str],
                    required: set[str] | frozenset[str] | None = None) -> dict[str, object]:
        requested = set(methods)
        required_methods = set(required or methods)
        selected = [row for row in rows if row.get("method") in requested]
        present = {str(row.get("method")) for row in selected}
        missing = sorted(required_methods - present)
        missing_core = sorted(CORE_METHODS - present) if requested & CORE_METHODS else []
        if not selected:
            return {"status": "NOT_REQUESTED", "methods": [],
                    "missing_core_methods": missing_core}
        ready = all(row.get("status") == "READY" for row in selected)
        status = ("COMPLETED" if ready and not missing else
                  "INCOMPLETE_REQUESTED_SUBSET" if missing else "NOT_READY")
        return {"status": status, "methods": sorted(present),
                "missing_core_methods": missing_core,
                "unready_methods": sorted({str(row.get("method")) for row in selected
                                            if row.get("status") != "READY"})}

    def _summary(self, rows: Sequence[Mapping[str, object]], experiment: Path,
                 states_by_method: Mapping[str, Mapping[tuple[str, str], _RelationshipState]]) -> dict[str, object]:
        relationships = {
            method: {"|".join(key): {"next_rel_seq": state.next_rel_seq,
                "committed_raw_prefix_descriptor": (
                    dict(state.prefix_descriptor) if method == "ZSTD_ROUTE" and
                    state.prefix_descriptor is not None else _prefix_descriptor(state.history)),
                "history_nonce": state.history_nonce,
                "route_identity": state.last_route_id,
                "reset_count": state.reset_count,
                "native_last_tu_seq": state.native_last_tu_seq,
                "native_next_rel_seq": state.native_next_rel_seq,
                "native_state_digest": state.native_state_digest}
                for key, state in states.items()}
            for method, states in states_by_method.items()
        }
        totals = {}
        c_authorities = {}
        for method in self.methods:
            selected = [row for row in rows if row.get("method") == method]
            native_tu = [row.get("native_tu_seq") for row in selected
                         if type(row.get("native_tu_seq")) is int]
            c_authorities[method] = {
                "native_next_tu_seq": (max(native_tu) + 1 if native_tu else None)}
            totals[method] = {
                "raw_bytes": sum(int(row.get("raw_bytes") or 0) for row in selected),
                "encoded_bytes": (None if method == "RAW_II" else
                                  sum(int(row.get("encoded_bytes") or 0) for row in selected)),
                "execution_ns": (None if method == "RAW_II" else
                                  sum(int(row.get("product_transaction", {}).get("simulator_execution_ns", 0))
                                      for row in selected)),
                "wire_witnessed": bool(selected) and all(
                    bool(row.get("wire_witnessed")) for row in selected),
                "c_to_f_bytes": (None if method == "RAW_II" else
                                  sum(int(row.get("product_transaction", {}).get("c_to_f_bytes", 0))
                                      for row in selected)),
                "f_to_c_bytes": (None if method == "RAW_II" else
                                  sum(int(row.get("product_transaction", {}).get("f_to_c_bytes", 0))
                                      for row in selected)),
                "state_digests": [row.get("product_transaction", {}).get("state_digest") for row in selected
                                  if row.get("product_transaction")],
            }
        return {"schema": SUMMARY_SCHEMA, "status": self._status(rows),
                "core_completion": self._completion(rows, CORE_METHODS),
                "optional_completion": self._completion(rows, set(self.methods) - CORE_METHODS),
                "experiment": str(experiment), "topology": self._topology_record(),
                "relationship_count": self.topology.relationship_count,
                "method_status": {method: self.authority[method]["status"] for method in self.methods},
                "c_authorities": c_authorities,
                "occurrence_rows": len(rows),
                "committed_rows": sum(bool(row["committed"]) for row in rows),
                "totals": totals,
                "relationships": relationships}


def verify_experiment(experiment: Path) -> dict[str, object]:
    """Verify canonical descriptors, row order, and byte artifacts fail closed."""
    try:
        manifest_raw, _manifest_facts = _private_bytes(experiment / "manifest.json",
                                                       "verifier_manifest")
        manifest = _strict_json(manifest_raw, "verifier_manifest")
    except (OSError, MatrixError) as exc:
        if isinstance(exc, MatrixError):
            raise
        raise MatrixError("verifier:manifest_unavailable") from exc
    if not isinstance(manifest, Mapping) or manifest.get("schema") != SCHEMA or \
            not isinstance(manifest.get("artifacts"), dict):
        raise MatrixError("verifier:manifest_schema_invalid")
    files = _experiment_files(experiment)
    files.pop("manifest.json", None)
    actual_names = set(files)
    declared = set(manifest["artifacts"])
    if actual_names != declared:
        raise MatrixError("verifier:artifact_added_or_deleted")
    for name, descriptor in manifest["artifacts"].items():
        if (not isinstance(descriptor, Mapping) or not isinstance(name, str) or
                name not in files or descriptor.get("bytes") != files[name][1]["bytes"] or
                descriptor.get("sha256") != files[name][1]["sha256"]):
            raise MatrixError("verifier:artifact_mutated:" + name)
        _private_experiment_file(experiment, name, "verifier_artifact")
    if "summary.json" not in files or "occurrences.jsonl" not in files:
        raise MatrixError("verifier:required_artifact_missing")
    summary = _strict_json(files["summary.json"][0], "verifier_summary")
    if not isinstance(summary, Mapping) or summary.get("schema") != SUMMARY_SCHEMA:
        raise MatrixError("verifier:summary_schema_invalid")
    def has_prefix_body(value: object) -> bool:
        if isinstance(value, Mapping):
            return any((isinstance(key, str) and key.startswith("committed_raw_prefix") and
                        key not in {"committed_raw_prefix_before_descriptor",
                                    "committed_raw_prefix_descriptor"}) or has_prefix_body(item)
                       for key, item in value.items())
        if isinstance(value, list):
            return any(has_prefix_body(item) for item in value)
        return False
    if has_prefix_body(manifest) or has_prefix_body(summary):
        raise MatrixError("verifier:route_prefix_body_forbidden")
    input_authority = manifest.get("input_authority")
    input_facts: list[dict[str, object]] = []
    route_observations: dict[int, dict[str, object]] = {}
    route_final_histories: dict[str, object] = {}
    if isinstance(input_authority, Mapping):
        root_path = Path(str(input_authority.get("corpus_root", AUTH_CORPUS_ROOT)))
        try:
            root_info = root_path.lstat()
            root = root_path.resolve(strict=True)
        except OSError as exc:
            raise MatrixError("verifier:input_root_unavailable") from exc
        if stat.S_ISLNK(root_info.st_mode) or not stat.S_ISDIR(root_info.st_mode):
            raise MatrixError("verifier:input_root_invalid")
        selected = input_authority.get("selected_inputs", [])
        if not isinstance(selected, list):
            raise MatrixError("verifier:input_authority_invalid")
        assignment = manifest.get("assignment_authority")
        predecessor_selected: object | None = None
        predecessor_assignment: object | None = None
        predecessor_input_authority = manifest.get("predecessor_input_authority")
        if manifest.get("repeat_full") is True:
            if (not isinstance(predecessor_input_authority, Mapping) or
                    not isinstance(predecessor_input_authority.get("selected_inputs"), list) or
                    not isinstance(predecessor_input_authority.get("assignment"), Mapping)):
                raise MatrixError("verifier:predecessor_route_authority_missing")
            predecessor_selected = predecessor_input_authority["selected_inputs"]
            predecessor_assignment = predecessor_input_authority["assignment"]
            if predecessor_selected:
                if not all(isinstance(item, Mapping) and item.get("source_relative") is not None
                           for item in predecessor_selected):
                    raise MatrixError("verifier:predecessor_route_inputs_missing")
        if selected and all(isinstance(item, Mapping) and
                            item.get("source_relative") is not None for item in selected):
            route_observations, route_final_histories, input_facts = _stream_route_prefixes(
                selected, assignment, root,
                predecessor_selected=(predecessor_selected if isinstance(predecessor_selected, list)
                                      and predecessor_selected else None),
                predecessor_assignment=predecessor_assignment,
                label="verifier_route")
        else:
            for item in selected:
                if not isinstance(item, Mapping):
                    raise MatrixError("verifier:input_descriptor_invalid")
                # In-memory synthetic controls intentionally have no external
                # source descriptor; they are not predecessor candidates.
                if item.get("source_relative") is None:
                    if item.get("sha256") is not None:
                        raise MatrixError("verifier:input_descriptor_invalid")
                    continue
                raise MatrixError("verifier:input_descriptor_invalid")
    try:
        rows = [_strict_json(line.encode("utf-8"), "verifier_occurrence") for line in
                files["occurrences.jsonl"][0].decode("utf-8").splitlines()]
    except (UnicodeDecodeError, MatrixError) as exc:
        if isinstance(exc, MatrixError):
            raise
        raise MatrixError("verifier:occurrences_invalid") from exc
    if any(not isinstance(row, Mapping) for row in rows):
        raise MatrixError("verifier:occurrences_invalid")
    if has_prefix_body(rows):
        raise MatrixError("verifier:route_prefix_body_forbidden")
    order = [{"ordinal": row.get("ordinal"), "method": row.get("method")} for row in rows]
    if order != manifest.get("row_order"):
        raise MatrixError("verifier:row_reordered_or_changed")
    for row in rows:
        for field in ("raw_path", "encoded_path"):
            name = row.get(field)
            digest_field = "raw_sha256" if field == "raw_path" else "encoded_sha256"
            if name is not None:
                if not isinstance(name, str) or name not in files:
                    raise MatrixError("verifier:row_artifact_binding_invalid")
                descriptor = manifest["artifacts"].get(name)
                if (descriptor is None or descriptor.get("sha256") != row.get(digest_field) or
                        files[name][1]["sha256"] != row.get(digest_field)):
                    raise MatrixError("verifier:row_artifact_binding_invalid")
    route_rows = [row for row in rows if row.get("method") == "ZSTD_ROUTE"]
    if route_observations and route_rows:
        seen_route_ordinals: set[int] = set()
        for row in route_rows:
            ordinal = row.get("ordinal")
            if type(ordinal) is not int or ordinal in seen_route_ordinals or ordinal not in route_observations:
                raise MatrixError("verifier:route_prefix_row_identity_invalid")
            seen_route_ordinals.add(ordinal)
            expected = route_observations[ordinal]
            if row.get("relationship_key") != expected["relationship_key"]:
                raise MatrixError("verifier:route_prefix_relationship_invalid")
            transaction = row.get("product_transaction")
            if not isinstance(transaction, Mapping):
                raise MatrixError("verifier:route_prefix_descriptor_missing")
            for suffix, descriptor_name in (("_before", "before"), ("", "after")):
                field = ("committed_raw_prefix_before_descriptor" if suffix else
                         "committed_raw_prefix_descriptor")
                transaction_descriptor = transaction.get(field)
                descriptor = expected[descriptor_name]
                if transaction_descriptor != descriptor:
                    raise MatrixError("verifier:route_prefix_descriptor_mismatch")
        if seen_route_ordinals != set(route_observations):
            raise MatrixError("verifier:route_prefix_rows_incomplete")
        relationships = summary.get("relationships")
        route_summary = relationships.get("ZSTD_ROUTE") if isinstance(relationships, Mapping) else None
        if not isinstance(route_summary, Mapping):
            raise MatrixError("verifier:route_prefix_summary_missing")
        for relationship_key, history in route_final_histories.items():
            state = route_summary.get(relationship_key)
            descriptor = (history if isinstance(history, Mapping)
                          else _prefix_descriptor(history))
            if (not isinstance(state, Mapping) or
                    state.get("committed_raw_prefix_descriptor") != descriptor):
                raise MatrixError("verifier:route_prefix_summary_mismatch")
    return {"status": "PASS", "experiment": str(experiment), "rows": len(rows),
            "artifacts": len(declared), "manifest": dict(manifest),
            "manifest_facts": dict(_manifest_facts),
            "summary": dict(summary), "rows_data": list(rows),
            "artifact_facts": {name: facts for name, (_raw, facts) in files.items()},
            "input_facts": input_facts}


def firefox_occurrences(trace: Path, *, topology_id: str | None = None,
                        topology: MatrixTopology | str | None = None,
                        count: int = 100, dispatch_start: int = 0,
                        corpus_root: Path = AUTH_CORPUS_ROOT,
                        corpus_manifest: Path = AUTH_CORPUS_MANIFEST) -> list[Occurrence]:
    """Load Firefox inputs only after trace, corpus, and every file authenticate."""
    if topology is not None:
        selected_topology = (topology.topology_id if isinstance(topology, MatrixTopology)
                             else str(topology))
        if topology_id is not None and topology_id != selected_topology:
            raise MatrixError("topology identity arguments disagree")
        topology_id = selected_topology
    if topology_id not in TOPOLOGY_IDS:
        raise MatrixError("firefox occurrence loader requires an explicit topology authority")
    if type(count) is not int or count <= 0 or type(dispatch_start) is not int or dispatch_start < 0:
        raise MatrixError("requested count must be positive")
    trace_raw, trace_facts_full = _private_bytes(trace, "trace")
    trace_facts = {key: trace_facts_full[key] for key in ("path", "bytes", "sha256")}
    if trace_facts["sha256"] != AUTH_TRACE_SHA256:
        raise MatrixError("trace:authenticated_digest_mismatch")
    manifest_raw, manifest_facts = _private_bytes(corpus_manifest, "corpus_manifest")
    if manifest_facts["sha256"] != AUTH_CORPUS_MANIFEST_SHA256:
        raise MatrixError("corpus_manifest:authenticated_digest_mismatch")
    root = corpus_root.resolve()
    root_info = corpus_root.lstat()
    if stat.S_ISLNK(root_info.st_mode) or not stat.S_ISDIR(root_info.st_mode):
        raise MatrixError("corpus_root:not_private_directory")
    manifest_paths: dict[str, dict[str, object]] = {}
    for line in manifest_raw.decode("utf-8").splitlines():
        if not line or line != line.strip():
            raise MatrixError("corpus_manifest:line_format_invalid")
        candidate = Path(line)
        if not candidate.is_absolute():
            candidate = root / candidate
        resolved = candidate.resolve()
        try:
            relative = str(resolved.relative_to(root))
        except ValueError as exc:
            raise MatrixError("corpus_manifest:path_outside_root") from exc
        if relative in manifest_paths:
            raise MatrixError("corpus_manifest:duplicate_path")
        # Membership is authenticated by the retained manifest digest; hash
        # and size are checked for each selected input below (bounded canary).
        manifest_paths[relative] = {"path": str(resolved)}
    trace_occurrences: list[Occurrence] = []
    reader = csv.DictReader(trace_raw.decode("utf-8").splitlines(), delimiter="\t")
    try:
        for row in reader:
            if len(trace_occurrences) >= min(count, 2498):
                break
            relative = row.get("ii_relative", "")
            if not relative or Path(relative).is_absolute() or relative.startswith("../"):
                raise MatrixError("trace:ii_relative_invalid")
            raw_path = root / relative
            try:
                logical = int(row["logical"])
                expected = int(row["raw_bytes"])
            except (KeyError, ValueError) as exc:
                raise MatrixError("trace:row_invalid") from exc
            if logical != len(trace_occurrences) or relative not in manifest_paths:
                raise MatrixError("trace:input_not_bound_to_corpus_manifest")
            raw, facts = _private_bytes(raw_path, "corpus_input")
            if len(raw) != expected:
                raise MatrixError("trace:raw_bytes_mismatch")
            trace_occurrences.append(Occurrence(ordinal=logical, raw=None,
                                     source_relative=relative, source_sha256=str(facts["sha256"]),
                                     source_path=str(raw_path),
                                     source_logical=logical,
                                     source_digest128=_digest128(raw)))
    except csv.Error as exc:
        raise MatrixError("trace:invalid_tsv") from exc
    if not trace_occurrences:
        raise MatrixError("trace:requested_rows_unavailable")
    # Join the authenticated global dispatch authority to trace logical rows.
    # This supports build boundaries and repeat-full without treating a
    # logical reset as a new dispatch identity.
    authority = _authenticated_assignment(topology_id, count, start=dispatch_start)
    result: list[Occurrence] = []
    for item in authority["rows"]:  # type: ignore[union-attr]
        logical = int(item["authority_logical"])
        if logical >= len(trace_occurrences):
            raise MatrixError("assignment:trace_logical_out_of_range")
        base = trace_occurrences[logical]
        result.append(Occurrence(ordinal=int(item["dispatch_order"]), raw=base.raw,
                                 source_relative=base.source_relative,
                                 source_sha256=base.source_sha256,
                                 source_path=base.source_path,
                                 source_build=int(item["authority_build"]),
                                 source_logical=logical,
                                 source_digest128=base.source_digest128))
    return result


def write_not_ready_canary(output_root: Path, trace: Path, *, topology: MatrixTopology,
                           count: int, reason: str, depth: str | None = None,
                           pass_id: str | None = None,
                           methods: Iterable[str] = METHODS) -> Path:
    """Write an immutable readiness experiment when Firefox inputs are absent."""
    run_timestamp = _stamp()
    depth_label = depth or f"count-{count}"
    pass_label = pass_id or "not-ready"
    run_label = run_timestamp + "-" + topology.topology_id.replace("/", "-") + \
        "-" + depth_label + "-" + pass_label
    experiment = output_root.absolute() / run_label
    suffix = 1
    while experiment.exists():
        experiment = output_root.absolute() / f"{run_label}-r{suffix:02d}"
        suffix += 1
    experiment.mkdir(parents=True, exist_ok=False)
    (experiment / "occurrences.jsonl").write_bytes(b"")
    selected_methods = tuple(methods)
    _validate_method_selection(selected_methods)
    authority = {method: method_authority(method) for method in selected_methods}
    manifest = {"schema": SCHEMA, "status": "NOT_READY", "topology": {
        "id": topology.topology_id, "relationship_count": topology.relationship_count,
        "slots_per_f": topology.slots_per_f, "global_slots": topology.global_slots},
        "methods": list(selected_methods), "input_authority": {"trace": str(trace),
        "trace_exists": trace.is_file(), "requested_tus": count}, "authority": authority,
        "reason": reason, "run_identity": {"timestamp": run_timestamp,
        "topology": topology.topology_id, "depth": depth_label, "pass": pass_label}}
    (experiment / "manifest.json").write_bytes(_canonical(manifest))
    (experiment / "summary.json").write_bytes(_canonical({
        "schema": SUMMARY_SCHEMA, "status": "NOT_READY", "experiment": str(experiment),
        "reason": reason, "method_status": {method: authority[method]["status"] for method in selected_methods},
        "relationship_count": topology.relationship_count, "occurrence_rows": 0}))
    artifacts = {}
    for path in (experiment / "occurrences.jsonl", experiment / "summary.json"):
        raw = path.read_bytes()
        artifacts[path.name] = {"bytes": len(raw), "sha256": _sha256(raw)}
    manifest["artifacts"] = artifacts
    manifest["row_order"] = []
    (experiment / "manifest.json").write_bytes(_canonical(manifest))
    return experiment


def _trace_input_sequence(trace: Path, count: int) -> list[dict[str, object]]:
    """Read authenticated trace/corpus membership without payload reads."""
    trace_raw, trace_facts = _private_bytes(trace, "trace")
    if trace_facts["sha256"] != AUTH_TRACE_SHA256:
        raise MatrixError("trace:authenticated_digest_mismatch")
    manifest_raw, manifest_facts = _private_bytes(AUTH_CORPUS_MANIFEST, "corpus_manifest")
    if manifest_facts["sha256"] != AUTH_CORPUS_MANIFEST_SHA256:
        raise MatrixError("corpus_manifest:authenticated_digest_mismatch")
    root = AUTH_CORPUS_ROOT.resolve(strict=True)
    members: set[str] = set()
    for line in manifest_raw.decode("utf-8").splitlines():
        if not line or line != line.strip():
            raise MatrixError("corpus_manifest:line_format_invalid")
        candidate = Path(line)
        if not candidate.is_absolute():
            candidate = root / candidate
        try:
            relative = str(candidate.resolve(strict=True).relative_to(root))
        except (OSError, ValueError) as exc:
            raise MatrixError("corpus_manifest:path_outside_root") from exc
        members.add(relative)
    rows: list[dict[str, object]] = []
    try:
        reader = csv.DictReader(trace_raw.decode("utf-8").splitlines(), delimiter="\t")
        for row in reader:
            if len(rows) >= count:
                break
            relative = row.get("ii_relative", "")
            if (not relative or Path(relative).is_absolute() or
                    any(part in ("", ".", "..") for part in Path(relative).parts) or
                    relative not in members):
                raise MatrixError("trace:input_not_bound_to_corpus_manifest")
            try:
                logical = int(row["logical"])
                expected = int(row["raw_bytes"])
            except (KeyError, ValueError) as exc:
                raise MatrixError("trace:row_invalid") from exc
            if logical != len(rows) or expected <= 0:
                raise MatrixError("trace:row_invalid")
            rows.append({"logical": logical, "source_relative": relative,
                         "bytes": expected})
    except UnicodeDecodeError as exc:
        raise MatrixError("trace:invalid_tsv") from exc
    if len(rows) != count:
        raise MatrixError("trace:requested_rows_unavailable")
    return rows


def _authenticated_predecessor_plan(
        path: Path, topology_id: str, *, trace: Path | None = None
        ) -> tuple[dict[str, object], dict[str, object], list[Occurrence]]:
    """Load one strictly verified full-1 state/plan and lazy inputs."""
    if not path.is_dir():
        raise MatrixError("repeat-full predecessor experiment is unavailable")
    verified = verify_experiment(path)
    manifest = verified.get("manifest")
    summary = verified.get("summary")
    if not isinstance(manifest, Mapping) or not isinstance(summary, Mapping):
        raise MatrixError("repeat-full predecessor manifest/summary is unavailable")
    run_identity = manifest.get("run_identity")
    if (manifest.get("schema") != SCHEMA or manifest.get("experiment") != path.name or
            summary.get("schema") != SUMMARY_SCHEMA or
            manifest.get("topology", {}).get("id") != topology_id or
            manifest.get("repeat_full") is not False or
            not isinstance(run_identity, Mapping) or
            run_identity.get("topology") != topology_id or
            run_identity.get("depth") != "full-1" or
            run_identity.get("pass") not in {"pass-1", "full-1"} or
            not isinstance(run_identity.get("timestamp"), str)):
        raise MatrixError("repeat-full predecessor is not an authenticated full-1 experiment")
    rows = verified.get("rows_data")
    methods = manifest.get("methods", [])
    artifacts = manifest.get("artifacts")
    if (not isinstance(artifacts, Mapping) or
            not isinstance(artifacts.get("summary.json"), Mapping) or
            not isinstance(artifacts.get("occurrences.jsonl"), Mapping) or
            not isinstance(methods, list) or any(not isinstance(method, str) for method in methods) or
            len(methods) != len(set(methods)) or
            not isinstance(rows, list) or int(summary.get("occurrence_rows", 0)) != len(rows) or
            len(rows) != 2498 * len(methods) or not manifest.get("row_order") or
            not set(methods).issuperset(STATEFUL_METHODS)):
        raise MatrixError("repeat-full predecessor artifacts/state are incomplete")
    expected_relationships = {"|".join(key) for key in MatrixTopology.from_id(topology_id).relationship_keys}
    relationships_all = summary.get("relationships", {})
    if not isinstance(relationships_all, Mapping):
        raise MatrixError("repeat-full predecessor relationship state is incomplete")
    c_authorities = summary.get("c_authorities", {})
    if not isinstance(c_authorities, Mapping):
        raise MatrixError("repeat-full predecessor C-wide TU authority is incomplete")
    for method in STATEFUL_METHODS:
        authority_state = c_authorities.get(method)
        if (not isinstance(authority_state, Mapping) or
                type(authority_state.get("native_next_tu_seq")) is not int or
                authority_state["native_next_tu_seq"] <= 0):
            raise MatrixError("repeat-full predecessor C-wide TU authority is incomplete")
    for method in STATEFUL_METHODS:
        relationships = relationships_all.get(method, {})
        if (not isinstance(relationships, Mapping) or
                set(relationships) != expected_relationships):
            raise MatrixError("repeat-full predecessor relationship state is incomplete")
        authority_next = c_authorities[method]["native_next_tu_seq"]
        relationship_last_tus: list[int] = []
        for value in relationships.values():
            if (not isinstance(value, Mapping) or
                    type(value.get("native_last_tu_seq")) is not int or
                    type(value.get("native_next_rel_seq")) is not int or
                    value["native_next_rel_seq"] <= 0 or
                    not _digest128_text(value.get("native_state_digest")) or
                    type(value.get("history_nonce")) is not int or value["history_nonce"] <= 0 or
                    not isinstance(value.get("route_identity"), str) or not value["route_identity"]):
                raise MatrixError("repeat-full predecessor native state is incomplete")
            relationship_last_tus.append(int(value["native_last_tu_seq"]))
            if method == "ZSTD_ROUTE":
                descriptor = value.get("committed_raw_prefix_descriptor")
                if not _valid_prefix_descriptor(descriptor):
                    raise MatrixError("repeat-full predecessor route prefix is incomplete")
        if max(relationship_last_tus, default=-1) != authority_next - 1:
            raise MatrixError("repeat-full predecessor C-wide TU authority is inconsistent")
    if (summary.get("experiment") != str(path) or
            summary.get("topology", {}).get("id") != topology_id or
            summary.get("status") not in {"COMPLETED", "CORE_COMPLETED_OPTIONALS_UNAVAILABLE"} or
            int(summary.get("relationship_count", 0)) != len(expected_relationships)):
        raise MatrixError("repeat-full predecessor summary binding is invalid")
    authority = manifest.get("assignment_authority")
    if not isinstance(authority, Mapping) or authority.get("topology") != topology_id:
        raise MatrixError("repeat-full predecessor assignment authority is missing")
    expected_assignment = _authenticated_assignment(topology_id, 2498, start=0)
    if (authority.get("path") != expected_assignment.get("path") or
            authority.get("bytes") != expected_assignment.get("bytes") or
            authority.get("sha256") != expected_assignment.get("sha256") or
            authority.get("rows") != expected_assignment.get("rows") or
            authority.get("selected_count") != 2498 or
            authority.get("authority_total_rows") != expected_assignment.get("authority_total_rows")):
        raise MatrixError("repeat-full predecessor assignment authority changed")
    input_authority = manifest.get("input_authority")
    selected_inputs = input_authority.get("selected_inputs", []) if isinstance(input_authority, Mapping) else []
    if (not isinstance(input_authority, Mapping) or
            input_authority.get("trace_sha256") != AUTH_TRACE_SHA256 or
            input_authority.get("corpus_manifest_sha256") != AUTH_CORPUS_MANIFEST_SHA256 or
            len(selected_inputs) != 2498 or trace is None):
        raise MatrixError("repeat-full predecessor input authority is incomplete")
    trace_rows = _trace_input_sequence(trace, 2498)
    facts = verified.get("input_facts")
    if not isinstance(facts, list) or len(facts) != 2498:
        raise MatrixError("repeat-full predecessor input facts are incomplete")
    occurrences: list[Occurrence] = []
    for index, (item, fact, assignment_row) in enumerate(zip(
            selected_inputs, facts, expected_assignment["rows"])):
        logical = int(assignment_row["authority_logical"])
        trace_item = trace_rows[logical]
        expected = {"ordinal": index, "build": int(assignment_row["authority_build"]),
                    "logical": logical, "source_relative": trace_item["source_relative"],
                    "bytes": trace_item["bytes"], "sha256": fact["sha256"]}
        if (item != expected or fact.get("source_relative") != expected["source_relative"] or
                fact.get("bytes") != expected["bytes"]):
            raise MatrixError("repeat-full predecessor input sequence is not authenticated")
        occurrences.append(Occurrence(index, None, source_relative=str(fact["source_relative"]),
                                      source_sha256=str(fact["sha256"]),
                                      source_path=str(fact["source_path"]),
                                      source_build=int(assignment_row["authority_build"]),
                                      source_logical=logical,
                                      source_digest128=str(fact["source_digest128"])))
    summary_relationships = summary.get("relationships", {})
    prior_state = dict(summary)
    prior_relationships = summary_relationships if isinstance(summary_relationships, Mapping) else {}
    prior_state["relationships"] = {
        method: {key: dict(value) if isinstance(value, Mapping) else value
                 for key, value in states.items()}
        for method, states in prior_relationships.items()
        if isinstance(states, Mapping)
    }
    return prior_state, dict(authority), occurrences


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--firefox-trace", type=Path)
    parser.add_argument("--output-root", type=Path, default=Path("experiments"))
    parser.add_argument("--count", type=int, default=100)
    parser.add_argument("--depth", choices=("100", "200", "full-1", "state-carrying-full-2"))
    parser.add_argument("--methods", nargs="+", default=list(DEFAULT_CLI_METHODS),
                        metavar="METHOD")
    parser.add_argument("--full-1-experiment-c1f1", type=Path)
    parser.add_argument("--full-1-experiment-c1f20", type=Path)
    args = parser.parse_args(argv)
    if args.firefox_trace is None:
        parser.error("--firefox-trace is required")
    try:
        methods = _validate_method_selection(args.methods)
    except MatrixError as exc:
        parser.error(str(exc))
    if args.depth == "state-carrying-full-2" and not (
            args.full_1_experiment_c1f1 and args.full_1_experiment_c1f20):
        parser.error("state-carrying-full-2 requires topology-specific predecessor experiments")
    count = {None: args.count, "100": 100, "200": 200, "full-1": 2498,
             "state-carrying-full-2": 2498}[args.depth]
    for topology_id in TOPOLOGY_IDS:
        topology = MatrixTopology.from_id(topology_id)
        try:
            dispatch_start = 2498 if args.depth == "state-carrying-full-2" else 0
            occurrences = firefox_occurrences(args.firefox_trace, topology_id=topology_id,
                                               count=count,
                                               dispatch_start=dispatch_start)
            if args.depth == "state-carrying-full-2":
                predecessor_path = {"C1F1/100000": args.full_1_experiment_c1f1,
                                    "C1F20/40": args.full_1_experiment_c1f20}[topology_id]
                prior_state, predecessor_authority, predecessor = _authenticated_predecessor_plan(
                    predecessor_path, topology_id, trace=args.firefox_trace)
                current_authority = _authenticated_assignment(
                    topology_id, count, start=dispatch_start)
                path = MethodMatrixSimulator(
                    topology, methods=methods, assignment_authority=current_authority).run(
                        occurrences, output_root=args.output_root, repeat_full=True,
                        prior_state=prior_state, predecessor_occurrences=predecessor,
                        predecessor_assignment_authority=predecessor_authority,
                        depth=args.depth)
                assert isinstance(path, Path)
                print(path)
                continue
            reason = ""
        except MatrixError as exc:
            occurrences = []
            reason = str(exc)
        if not occurrences:
            path = write_not_ready_canary(args.output_root, args.firefox_trace,
                                          topology=topology, count=count,
                                          reason="MISSING_AUTHORITY: " + (reason or
                                              "authenticated Firefox .ii inputs are unavailable"),
                                          depth=args.depth or f"count-{count}", methods=methods)
            print(path)
            continue
        path = MethodMatrixSimulator(topology, methods=methods).run(
            occurrences, output_root=args.output_root,
            depth=args.depth or f"count-{count}")
        assert isinstance(path, Path)
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
