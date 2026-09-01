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
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence

try:
    import zstandard as zstd
except ImportError:  # pragma: no cover - exercised on minimal hosts
    zstd = None


SCHEMA = "icecream-s8-method-matrix-simulator-v1"
OCCURRENCE_SCHEMA = "icecream-s8-method-occurrence-v1"
SUMMARY_SCHEMA = "icecream-s8-method-matrix-summary-v1"
METHODS = ("RAW_II", "ZSTD_TU", "ZSTD_ROUTE", "ZSTD_COHORT", "ZSTD_GLOBAL")
READY_METHODS = frozenset(("RAW_II", "ZSTD_TU", "ZSTD_ROUTE"))
TOPOLOGY_IDS = ("C1F1/100000", "C1F20/40")
DEFAULT_HISTORY_BYTES = 128 << 20


class MatrixError(ValueError):
    """Malformed topology, occurrence, or method authority."""


class NotReady(MatrixError):
    """A method was requested without its independent authority."""


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _canonical(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":"),
                       ensure_ascii=True, allow_nan=False) + "\n").encode("ascii")


def _stamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def _authority_file(path: Path, finding: str) -> dict[str, object]:
    result: dict[str, object] = {"path": str(path), "finding": finding}
    try:
        raw = path.read_bytes()
    except OSError:
        result["available"] = False
        result["sha256"] = None
        return result
    result.update({"available": True, "bytes": len(raw), "sha256": _sha256(raw)})
    return result


def method_authority(method: str) -> dict[str, object]:
    """Return the method's explicit implementation/status authority."""
    if method not in METHODS:
        raise MatrixError(f"unknown method: {method}")
    root = Path(__file__).resolve().parents[1]
    product = root / "cache" / "p50_zstd.cpp"
    planner = root / "farmharness" / "s4_version_transition_planner.py"
    if method == "RAW_II":
        return {"status": "READY", "kind": "whole-legacy-control",
                "authority": [_authority_file(product,
                    "RAW_II is a raw framed application-wire control; no zstd state")],
                "contract": "raw bytes in both directions are measured by the control witness"}
    if method == "ZSTD_TU":
        return {"status": "READY", "kind": "product-profile",
                "authority": [_authority_file(product,
                    "ZstdTuCodec resets session and parameters for each independent TU")],
                "contract": "independent level-3 frame/context per TU"}
    if method == "ZSTD_ROUTE":
        return {"status": "READY", "kind": "product-profile",
                "authority": [_authority_file(product,
                    "ZstdRouteCodec resets a fresh level-3 frame and refPrefixes committed raw history")],
                "contract": "one bounded-prefix frame per TU; commit advances relationship state"}
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
            (1, 1, 1, 100000) if self.topology_id == "C1F1/100000"
            else (20, 2, 40, None))
        f_guids = self.f_store_guids or tuple(f"F{index}" for index in range(expected_f))
        object.__setattr__(self, "f_store_guids", tuple(f_guids))
        if len(f_guids) != expected_f or len(set(f_guids)) != expected_f:
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
                   slot: int = 0) -> dict[str, object]:
        if type(ordinal) is not int or ordinal < 0:
            raise MatrixError("ordinal must be a non-negative integer")
        guid = f_store_guid or self.f_store_guids[ordinal % self.relationship_count]
        key = self.relationship_for(guid, slot)
        return {"ordinal": ordinal, "f_store_guid": guid, "slot": slot,
                "relationship_key": list(key),
                "relationship_index": self.f_store_guids.index(guid)}


@dataclass(frozen=True)
class Occurrence:
    ordinal: int
    raw: bytes
    f_store_guid: str | None = None
    slot: int = 0
    route_id: str | None = None
    history_nonce: int = 1
    rel_seq: int | None = None
    commit: bool = True
    reset_before: bool = False

    def __post_init__(self) -> None:
        if type(self.ordinal) is not int or self.ordinal < 0:
            raise MatrixError("occurrence ordinal must be non-negative")
        if not isinstance(self.raw, bytes) or not self.raw:
            raise MatrixError("occurrence raw payload must be non-empty bytes")
        if type(self.slot) is not int or self.slot < 0:
            raise MatrixError("occurrence slot must be non-negative")
        if type(self.history_nonce) is not int or self.history_nonce <= 0:
            raise MatrixError("history nonce must be positive")
        if self.rel_seq is not None and (type(self.rel_seq) is not int or self.rel_seq < 0):
            raise MatrixError("REL_SEQ must be a non-negative integer")

    @classmethod
    def from_path(cls, ordinal: int, path: Path, **kwargs: object) -> "Occurrence":
        info = path.lstat()
        if not path.is_file() or path.is_symlink() or info.st_nlink != 1:
            raise MatrixError(f"source is not an authenticated private regular file: {path}")
        return cls(ordinal, path.read_bytes(), **kwargs)


@dataclass
class _RelationshipState:
    key: tuple[str, str]
    history: bytes = b""
    next_rel_seq: int = 0
    last_route_id: str | None = None
    history_nonce: int = 1
    reset_count: int = 0


def assign_relationships(topology: MatrixTopology, occurrences: Sequence[Occurrence]) -> list[dict[str, object]]:
    """Deterministically map ordered TUs to C/F relationships."""
    assignments = []
    for occurrence in occurrences:
        assignments.append(topology.assignment(occurrence.ordinal,
                                                occurrence.f_store_guid,
                                                occurrence.slot))
    return assignments


def repeat_full_state_contract(method: str) -> dict[str, object]:
    """State permitted to survive full-1 into repeat-full."""
    if method == "ZSTD_ROUTE":
        return {"survives": True, "fields": ["committed_raw_prefix", "history_nonce",
                                               "next_rel_seq", "route_identity"]}
    if method == "ZSTD_COHORT":
        return {"survives": True, "fields": ["cohort_dictionary_identity",
                                               "committed_relationship_continuation"]}
    return {"survives": False, "fields": []}


class MethodMatrixSimulator:
    """Run one ordered occurrence stream over explicit method states."""

    def __init__(self, topology: MatrixTopology, *, methods: Iterable[str] = METHODS,
                 max_history_bytes: int = DEFAULT_HISTORY_BYTES,
                 cohort_dictionary: bytes | None = None,
                 cohort_authority: Mapping[str, object] | None = None) -> None:
        if type(max_history_bytes) is not int or max_history_bytes <= 0:
            raise MatrixError("max_history_bytes must be positive")
        self.topology = topology
        self.methods = tuple(methods)
        if not self.methods or any(method not in METHODS for method in self.methods):
            raise MatrixError("methods contain an unknown or empty method")
        self.max_history_bytes = max_history_bytes
        self.cohort_dictionary = cohort_dictionary
        self.cohort_authority = dict(cohort_authority or {})
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

    def _encode(self, method: str, raw: bytes, state: _RelationshipState) -> tuple[bytes, int, int]:
        authority = self.authority[method]
        if authority["status"] == "NOT_IMPLEMENTED":
            raise NotReady("ZSTD_GLOBAL is NOT_IMPLEMENTED and is not aliased")
        if authority["status"] == "NOT_READY":
            raise NotReady(str(authority.get("reason", "method authority unavailable")))
        start_cpu, start_wall = time.process_time_ns(), time.perf_counter_ns()
        if method == "RAW_II":
            encoded = raw
        else:
            if zstd is None:
                raise NotReady("libzstd Python binding is unavailable")
            dictionary: Any = None
            if method == "ZSTD_ROUTE":
                if state.history:
                    dictionary = zstd.ZstdCompressionDict(
                        state.history, dict_type=zstd.DICT_TYPE_RAWCONTENT)
            elif method == "ZSTD_COHORT":
                # The base dictionary is immutable.  The per-relationship
                # continuation is a separate suffix used only by this relation.
                material = self.cohort_dictionary
                assert material is not None
                if state.history:
                    material += state.history
                dictionary = zstd.ZstdCompressionDict(
                    material, dict_type=zstd.DICT_TYPE_RAWCONTENT)
            encoded = zstd.ZstdCompressor(level=3, dict_data=dictionary).compress(raw)
        return encoded, time.process_time_ns() - start_cpu, time.perf_counter_ns() - start_wall

    def run(self, occurrences: Sequence[Occurrence], *, output_root: Path | None = None,
            timestamp: str | None = None, repeat_full: bool = False,
            prior_state: Mapping[str, object] | None = None) -> Path | dict[str, object]:
        if repeat_full and prior_state is None:
            raise MatrixError("repeat-full requires explicit full-1 state")
        assignments = assign_relationships(self.topology, occurrences)
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
                    history = value.get("committed_raw_prefix", b"")
                    if isinstance(history, str):
                        history = bytes.fromhex(history)
                    if not isinstance(history, bytes) or len(history) > self.max_history_bytes:
                        raise MatrixError("repeat-full committed prefix exceeds bound")
                    states_by_method[method][key].history = history
                    states_by_method[method][key].next_rel_seq = int(value.get("next_rel_seq", 0))

        if output_root is None:
            return self._run_memory(occurrences, assignments, states_by_method)
        experiment = output_root.absolute() / (timestamp or _stamp())
        if experiment.exists():
            raise MatrixError(f"experiment already exists: {experiment}")
        experiment.mkdir(parents=True)
        (experiment / "bytes").mkdir()
        rows: list[dict[str, object]] = []
        for occurrence, assignment in zip(occurrences, assignments):
            for method in self.methods:
                rows.append(self._run_occurrence(experiment, occurrence, assignment,
                                                 method, states_by_method[method][tuple(assignment["relationship_key"])]))
        manifest = {"schema": SCHEMA, "experiment": experiment.name,
                    "topology": self._topology_record(), "methods": list(self.methods),
                    "authority": self.authority, "repeat_full": repeat_full,
                    "repeat_full_state_contract": {
                        method: repeat_full_state_contract(method) for method in self.methods},
                    "relationship_count": self.topology.relationship_count,
                    "capacity_is_concurrency": True}
        (experiment / "manifest.json").write_bytes(_canonical(manifest))
        with (experiment / "occurrences.jsonl").open("wb") as stream:
            for row in rows:
                stream.write(_canonical(row))
        summary = self._summary(rows, experiment, states_by_method)
        (experiment / "summary.json").write_bytes(_canonical(summary))
        return experiment

    def _run_memory(self, occurrences: Sequence[Occurrence], assignments: Sequence[dict[str, object]],
                    states_by_method: dict[str, dict[tuple[str, str], _RelationshipState]]) -> dict[str, object]:
        rows: list[dict[str, object]] = []
        for occurrence, assignment in zip(occurrences, assignments):
            for method in self.methods:
                key = tuple(assignment["relationship_key"])
                rows.append(self._run_occurrence(
                    None, occurrence, assignment, method, states_by_method[method][key]))
        return {"schema": SUMMARY_SCHEMA, "status": self._status(rows),
                "topology": self._topology_record(), "rows": rows,
                "relationship_count": self.topology.relationship_count,
                "method_status": {method: self.authority[method]["status"] for method in self.methods}}

    def _run_occurrence(self, experiment: Path | None, occurrence: Occurrence,
                        assignment: Mapping[str, object], method: str,
                        state: _RelationshipState) -> dict[str, object]:
        key = tuple(assignment["relationship_key"])  # type: ignore[arg-type]
        pre = _sha256(_canonical({"history": state.history.hex(), "next_rel_seq": state.next_rel_seq,
                                  "route_id": state.last_route_id, "nonce": state.history_nonce}))
        raw_path = encoded_path = None
        raw_sha = _sha256(occurrence.raw)
        if experiment is not None:
            method_dir = experiment / "bytes" / method
            method_dir.mkdir(exist_ok=True)
            raw_path = method_dir / f"raw-{occurrence.ordinal:06d}.bin"
            raw_path.write_bytes(occurrence.raw)
        row: dict[str, object] = {"schema": OCCURRENCE_SCHEMA, "method": method,
            "topology": self.topology.topology_id, "ordinal": occurrence.ordinal,
            "relationship_key": list(key), "relationship_index": assignment["relationship_index"],
            "slot": occurrence.slot, "raw_bytes": len(occurrence.raw), "raw_sha256": raw_sha,
            "raw_path": str(raw_path.relative_to(experiment)) if raw_path and experiment else None,
            "status": self.authority[method]["status"], "committed": False,
            "encoded_bytes": None, "encoded_sha256": None, "encoded_path": None,
            "codec_cpu_ns": None, "codec_wall_ns": None, "transition": "not_run",
            "pre_state_digest": pre, "post_state_digest": pre,
            "authority": self.authority[method]}
        if self.authority[method]["status"] not in {"READY"}:
            return row
        # Preparation is tentative until the terminal commit witness arrives.
        # Encode against a copy so a rejected candidate cannot rebind a route,
        # consume a reset nonce, or advance REL_SEQ.
        encode_state = _RelationshipState(key, state.history, state.next_rel_seq,
                                          state.last_route_id, state.history_nonce,
                                          state.reset_count)
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
        encoded, cpu_ns, wall_ns = self._encode(method, occurrence.raw, encode_state)
        if experiment is not None:
            encoded_path = experiment / "bytes" / method / f"encoded-{occurrence.ordinal:06d}.bin"
            encoded_path.write_bytes(encoded)
        row.update({"encoded_bytes": len(encoded), "encoded_sha256": _sha256(encoded),
                    "encoded_path": str(encoded_path.relative_to(experiment))
                    if encoded_path and experiment else None,
                    "codec_cpu_ns": cpu_ns, "codec_wall_ns": wall_ns})
        if occurrence.commit:
            if method in {"ZSTD_ROUTE", "ZSTD_COHORT"}:
                encode_state.history = (encode_state.history + occurrence.raw)[-self.max_history_bytes:]
                encode_state.next_rel_seq += 1
                state.history = encode_state.history
                state.next_rel_seq = encode_state.next_rel_seq
                state.reset_count = encode_state.reset_count
                state.history_nonce = encode_state.history_nonce
                state.last_route_id = encode_state.last_route_id
                row["transition"] = "committed_relationship_advance"
            elif method == "ZSTD_TU":
                row["transition"] = "committed_tu_reset"
            else:
                row["transition"] = "committed_raw_control"
            row["committed"] = True
        else:
            row["transition"] = "tentative_not_committed"
        post = _sha256(_canonical({"history": state.history.hex(), "next_rel_seq": state.next_rel_seq,
                                   "route_id": state.last_route_id, "nonce": state.history_nonce}))
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
        return "COMPLETED" if all(row["status"] == "READY" for row in rows) else "PARTIAL_NOT_READY"

    def _summary(self, rows: Sequence[Mapping[str, object]], experiment: Path,
                 states_by_method: Mapping[str, Mapping[tuple[str, str], _RelationshipState]]) -> dict[str, object]:
        relationships = {
            method: {"|".join(key): {"next_rel_seq": state.next_rel_seq,
                "committed_raw_prefix": state.history.hex(),
                "history_nonce": state.history_nonce,
                "route_identity": state.last_route_id,
                "committed_raw_prefix_bytes": len(state.history),
                "reset_count": state.reset_count}
                for key, state in states.items()}
            for method, states in states_by_method.items()
        }
        return {"schema": SUMMARY_SCHEMA, "status": self._status(rows),
                "experiment": str(experiment), "topology": self._topology_record(),
                "relationship_count": self.topology.relationship_count,
                "method_status": {method: self.authority[method]["status"] for method in self.methods},
                "occurrence_rows": len(rows),
                "committed_rows": sum(bool(row["committed"]) for row in rows),
                "relationships": relationships}


def firefox_occurrences(trace: Path, *, count: int = 100) -> list[Occurrence]:
    """Load only authenticated existing Firefox preprocessed inputs from TSV."""
    result: list[Occurrence] = []
    with trace.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream, delimiter="\t")
        for row in reader:
            if len(result) >= count:
                break
            raw_path = Path(row.get("ii_relative", ""))
            if not raw_path.is_absolute():
                raw_path = Path("/") / raw_path
            try:
                occurrence = Occurrence.from_path(int(row["logical"]), raw_path)
            except (KeyError, OSError, MatrixError):
                return []
            expected = int(row.get("raw_bytes", occurrence.raw.__len__()))
            if len(occurrence.raw) != expected:
                return []
            result.append(occurrence)
    return result


def write_not_ready_canary(output_root: Path, trace: Path, *, topology: MatrixTopology,
                           count: int, reason: str) -> Path:
    """Write an immutable readiness experiment when Firefox inputs are absent."""
    experiment = output_root.absolute() / f"{_stamp()}-{topology.topology_id.split('/')[0]}"
    experiment.mkdir(parents=True, exist_ok=False)
    (experiment / "occurrences.jsonl").write_bytes(b"")
    authority = {method: method_authority(method) for method in METHODS}
    manifest = {"schema": SCHEMA, "status": "NOT_READY", "topology": {
        "id": topology.topology_id, "relationship_count": topology.relationship_count,
        "slots_per_f": topology.slots_per_f, "global_slots": topology.global_slots},
        "methods": list(METHODS), "input_authority": {"trace": str(trace),
        "trace_exists": trace.is_file(), "requested_tus": count}, "authority": authority,
        "reason": reason}
    (experiment / "manifest.json").write_bytes(_canonical(manifest))
    (experiment / "summary.json").write_bytes(_canonical({
        "schema": SUMMARY_SCHEMA, "status": "NOT_READY", "experiment": str(experiment),
        "reason": reason, "method_status": {method: authority[method]["status"] for method in METHODS},
        "relationship_count": topology.relationship_count, "occurrence_rows": 0}))
    return experiment


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--firefox-trace", type=Path)
    parser.add_argument("--output-root", type=Path, default=Path("experiments"))
    parser.add_argument("--count", type=int, default=100)
    args = parser.parse_args(argv)
    if args.firefox_trace is None:
        parser.error("--firefox-trace is required")
    for topology_id in TOPOLOGY_IDS:
        topology = MatrixTopology.from_id(topology_id)
        occurrences = firefox_occurrences(args.firefox_trace, count=args.count)
        if not occurrences:
            path = write_not_ready_canary(args.output_root, args.firefox_trace,
                                          topology=topology, count=args.count,
                                          reason="MISSING_AUTHORITY: authenticated Firefox .ii inputs are unavailable")
            print(path)
            continue
        path = MethodMatrixSimulator(topology).run(occurrences, output_root=args.output_root)
        assert isinstance(path, Path)
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
