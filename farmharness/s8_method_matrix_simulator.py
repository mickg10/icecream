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
READY_METHODS = frozenset(("RAW_II", "ZSTD_TU", "ZSTD_ROUTE"))
TOPOLOGY_IDS = ("C1F1/100000", "C1F20/40")
DEFAULT_HISTORY_BYTES = 128 << 20
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
    finally:
        os.close(fd)
    if len(raw) != info.st_size:
        raise MatrixError(f"{label}:size_changed:{path}")
    return raw, {"path": str(path), "bytes": len(raw), "sha256": _sha256(raw)}


def _authenticated_assignment(topology_id: str, count: int) -> dict[str, object]:
    path = AUTH_ASSIGNMENT_PATHS[topology_id]
    assignment_raw, facts = _private_bytes(path, "assignment")
    if facts["sha256"] != AUTH_ASSIGNMENT_SHA256[topology_id]:
        raise MatrixError("assignment:authenticated_digest_mismatch")
    rows = list(csv.DictReader(assignment_raw.decode("utf-8").splitlines(), delimiter="\t"))
    if not rows or len(rows) < count:
        raise MatrixError("assignment:too_short")
    selected: list[dict[str, object]] = []
    expected_relations = 1 if topology_id == "C1F1/100000" else 20
    for ordinal, row in enumerate(rows[:count]):
        try:
            if int(row["dispatch_order"]) != ordinal or \
                    ("tu_seq" in row and int(row["tu_seq"]) != ordinal):
                raise MatrixError("assignment:dispatch_order_invalid")
            relation = int(row["worker"])
            # C1F20's two dispatch lanes are authenticated in compiler_slot;
            # C1F1's retained map has one relationship and no lane column.
            slot = int(row.get("compiler_slot", row.get("slot", "0")))
        except (KeyError, ValueError) as exc:
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
        selected.append({"ordinal": ordinal, "global_slot": global_slot,
                         "f_relationship": expected_relation, "per_f_slot": expected_slot,
                         "dispatch_order": ordinal, "authority_worker": relation,
                         "authority_slot": slot, "authority_build": int(row.get("build", 0)),
                         "authority_logical": int(row.get("logical", ordinal))})
    return {"path": facts["path"], "bytes": facts["bytes"], "sha256": facts["sha256"],
            "schema": "root-matrix-v4-assignment-tsv-v1", "topology": topology_id,
            "rows": selected, "selected_count": count}


def _native_batch(occurrences: Sequence[Occurrence], topology: MatrixTopology,
                  assignment: Mapping[str, object], method: str) -> dict[int, dict[str, object]]:
    """Run the actual endpoint transaction for every selected TU."""
    root = Path(__file__).resolve().parents[1]
    binary = root / "cache" / "sim" / ".p50sim.bin"
    if not binary.is_file():
        raise NotReady("MISSING_AUTHORITY: native p50sim binary")
    rows = assignment.get("rows")
    if not isinstance(rows, list) or len(rows) != len(occurrences):
        raise MatrixError("assignment authority/stream length mismatch")
    with tempfile.TemporaryDirectory(prefix="s8-product-batch-") as scratch:
        scratch_path = Path(scratch)
        manifest = scratch_path / "inputs.manifest"
        mapping = scratch_path / "assignments.map"
        output = scratch_path / "output.jsonl"
        manifest.write_text("".join((occurrence.source_path or "") + "\n"
                                     for occurrence in occurrences), encoding="utf-8")
        mapping.write_text("cardinality=" + str(topology.relationship_count) + "\n" +
                           "".join(str(int(item["f_relationship"])) + "\n" for item in rows),
                           encoding="ascii")
        environment = dict(os.environ)
        environment["ICECC_P50_PROFILE"] = method
        completed = subprocess.run(
            [str(binary), "--batch-manifest", str(manifest), "--batch-assignment-map", str(mapping),
             "--batch-output", str(output), "--batch-allow-repeated-inputs", "1"],
            env=environment, check=False,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=600)
        if completed.returncode != 0:
            raise NotReady("native product transaction failed: " +
                           completed.stderr.decode("utf-8", "replace")[:300])
        result: dict[int, dict[str, object]] = {}
        for line in output.read_text(encoding="utf-8").splitlines():
            item = json.loads(line)
            ordinal = int(item["tu_index"])
            if item.get("committed") is not True or item.get("raw_bytes") != occurrences[ordinal].byte_count():
                raise MatrixError("native product commit/input binding invalid")
            result[ordinal] = item
        if set(result) != set(range(len(occurrences))):
            raise MatrixError("native product batch is incomplete or reordered")
        return result


def _native_receipt(root: Path) -> tuple[dict[str, object] | None, dict[str, object]]:
    receipt_path = root / "cache" / "sim" / ".p50sim-build.json"
    binary = root / "cache" / "sim" / ".p50sim.bin"
    receipt_facts = _authority_file(receipt_path, "native p50sim build receipt")
    try:
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        binary_facts = _private_digest(binary, "native p50sim binary")
        head = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"],
                                       text=True, timeout=10).strip()
        tree = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD^{tree}"],
                                       text=True, timeout=10).strip()
    except (OSError, json.JSONDecodeError, MatrixError):
        return None, receipt_facts
    if (receipt.get("schema") != "icecream-p50sim-build-v1" or
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
            "head": head, "tree": tree, "archive_sha256": AUTH_LIBBSC_ARCHIVE_SHA256,
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
                "contract": "raw bytes in both directions are measured by the control witness"}
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
            if item.get("ordinal") != ordinal or item.get("dispatch_order") != ordinal:
                raise MatrixError("assignment dispatch order changed")
        else:
            if f_store_guid is None:
                raise MatrixError("authenticated assignment authority is required")
            guid = f_store_guid
        key = self.relationship_for(guid, slot)
        return {"ordinal": ordinal, "f_store_guid": guid, "slot": slot,
                "relationship_key": list(key),
                "relationship_index": self.f_store_guids.index(guid),
                "global_slot": (self.f_store_guids.index(guid) * int(self.slots_per_f) + slot)}


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
                   source_sha256=digest, source_path=str(path), **kwargs)

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
    history: bytes = b""
    next_rel_seq: int = 0
    last_route_id: str | None = None
    history_nonce: int = 1
    reset_count: int = 0
    pending: bool = False


def assign_relationships(topology: MatrixTopology, occurrences: Sequence[Occurrence],
                        authority: Mapping[str, object] | None = None) -> list[dict[str, object]]:
    """Deterministically map ordered TUs to C/F relationships."""
    assignments = []
    for occurrence in occurrences:
        assignments.append(topology.assignment(occurrence.ordinal,
                                                occurrence.f_store_guid,
                                                occurrence.slot, authority))
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
                 cohort_authority: Mapping[str, object] | None = None,
                 assignment_authority: Mapping[str, object] | None = None) -> None:
        if type(max_history_bytes) is not int or max_history_bytes <= 0:
            raise MatrixError("max_history_bytes must be positive")
        self.topology = topology
        self.methods = tuple(methods)
        if not self.methods or any(method not in METHODS for method in self.methods):
            raise MatrixError("methods contain an unknown or empty method")
        self.max_history_bytes = max_history_bytes
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
            receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise NotReady("MISSING_AUTHORITY: native p50sim binary/receipt") from exc
        if (stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode) or
                info.st_nlink != 1 or not os.access(binary, os.X_OK) or
                receipt.get("schema") != "icecream-p50sim-build-v1" or
                receipt.get("binary", {}).get("path") != str(binary) or
                receipt.get("binary", {}).get("sha256") != _sha256(binary.read_bytes())):
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
                encoded = output_file.read_bytes()
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
            return raw, 0, 0
        if method in {"ZSTD_TU", "ZSTD_ROUTE"}:
            return self._native_codec(method, raw, state)
        raise NotReady("cohort codec has no accepted native authority")

    def run(self, occurrences: Sequence[Occurrence], *, output_root: Path | None = None,
            timestamp: str | None = None, repeat_full: bool = False,
            prior_state: Mapping[str, object] | None = None) -> Path | dict[str, object]:
        if repeat_full and prior_state is None:
            raise MatrixError("repeat-full requires explicit full-1 state")
        if self.assignment_authority.get("status") == "UNBOUND":
            self.assignment_authority = _authenticated_assignment(
                self.topology.topology_id, len(occurrences))
        if (self.assignment_authority.get("topology") != self.topology.topology_id or
                int(self.assignment_authority.get("selected_count", len(occurrences))) < len(occurrences)):
            raise MatrixError("assignment authority does not cover stream")
        assignments = assign_relationships(self.topology, occurrences, self.assignment_authority)
        if occurrences and all(occurrence.source_path for occurrence in occurrences):
            for method in self.methods:
                if method in {"ZSTD_TU", "P29", "GRZ_RESIDUAL", "ZSTD_ROUTE"} and \
                        self.authority[method]["status"] == "READY":
                    self._native_rows[method] = _native_batch(
                        occurrences, self.topology, self.assignment_authority, method)
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
                    "artifacts": descriptors,
                    "row_order": [{"ordinal": row["ordinal"], "method": row["method"]}
                                  for row in rows]}
        (experiment / "manifest.json").write_bytes(_canonical(manifest))
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
        raw = occurrence.read_raw()
        raw_sha = occurrence.source_sha256 or _sha256(raw)
        # Raw corpus payloads remain at their authenticated immutable source;
        # experiment evidence retains source path/size/hash descriptors only.
        if experiment is not None:
            (experiment / "bytes" / method).mkdir(exist_ok=True)
        row: dict[str, object] = {"schema": OCCURRENCE_SCHEMA, "method": method,
            "topology": self.topology.topology_id, "ordinal": occurrence.ordinal,
            "relationship_key": list(key), "relationship_index": assignment["relationship_index"],
            "slot": assignment["slot"], "global_slot": assignment["global_slot"],
            "authority_worker": self.assignment_authority.get("rows", [])[occurrence.ordinal].get("authority_worker")
                if isinstance(self.assignment_authority.get("rows"), list) and occurrence.ordinal < len(self.assignment_authority.get("rows", [])) else None,
            "raw_bytes": len(raw), "raw_sha256": raw_sha,
            "source_relative": occurrence.source_relative, "source_sha256": occurrence.source_sha256,
            "raw_path": str(raw_path.relative_to(experiment)) if raw_path and experiment else None,
            "status": self.authority[method]["status"], "committed": False,
            "encoded_bytes": None, "encoded_sha256": None, "encoded_path": None,
            "codec_cpu_ns": None, "codec_wall_ns": None, "transition": "not_run",
            "pre_state_digest": pre, "post_state_digest": pre,
            "authority": self.authority[method]}
        if self.authority[method]["status"] not in {"READY"}:
            return row
        if state.pending:
            raise MatrixError("pending preparation blocks until explicit release")
        # Preparation is tentative until the terminal commit witness arrives.
        # Encode against a copy so a rejected candidate cannot rebind a route,
        # consume a reset nonce, or advance REL_SEQ.
        encode_state = _RelationshipState(key, state.history, state.next_rel_seq,
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
        else:
            encoded, cpu_ns, wall_ns = self._encode(method, raw, encode_state)
        if experiment is not None and product is None:
            encoded_path = experiment / "bytes" / method / f"encoded-{occurrence.ordinal:06d}.bin"
            encoded_path.write_bytes(encoded)
        row.update({"encoded_bytes": (int(product["encoded_source_bytes"]) if product is not None else len(encoded)),
                    "encoded_sha256": (_sha256(encoded) if product is None else None),
                    "encoded_path": str(encoded_path.relative_to(experiment))
                    if encoded_path and experiment else None,
                    "codec_cpu_ns": cpu_ns, "codec_wall_ns": wall_ns})
        if method in self._native_rows and occurrence.ordinal in self._native_rows[method]:
            row["product_transaction"] = self._native_rows[method][occurrence.ordinal]
        if occurrence.commit:
            if method in {"ZSTD_ROUTE", "ZSTD_COHORT"}:
                encode_state.history = (encode_state.history + raw)[-self.max_history_bytes:]
                encode_state.next_rel_seq += 1
                state.history = encode_state.history
                state.next_rel_seq = encode_state.next_rel_seq
                state.reset_count = encode_state.reset_count
                state.history_nonce = encode_state.history_nonce
                state.last_route_id = encode_state.last_route_id
                state.pending = False
                row["transition"] = "committed_relationship_advance"
            elif method == "ZSTD_TU":
                row["transition"] = "committed_tu_reset"
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
        totals = {}
        for method in self.methods:
            selected = [row for row in rows if row.get("method") == method]
            totals[method] = {
                "raw_bytes": sum(int(row.get("raw_bytes") or 0) for row in selected),
                "encoded_bytes": sum(int(row.get("encoded_bytes") or 0) for row in selected),
                "c_to_f_bytes": sum(int(row.get("product_transaction", {}).get("c_to_f_bytes", 0))
                                     for row in selected),
                "f_to_c_bytes": sum(int(row.get("product_transaction", {}).get("f_to_c_bytes", 0))
                                     for row in selected),
                "execution_ns": sum(int(row.get("product_transaction", {}).get("simulator_execution_ns", 0))
                                     for row in selected),
                "state_digests": [row.get("product_transaction", {}).get("state_digest") for row in selected
                                  if row.get("product_transaction")],
            }
        return {"schema": SUMMARY_SCHEMA, "status": self._status(rows),
                "experiment": str(experiment), "topology": self._topology_record(),
                "relationship_count": self.topology.relationship_count,
                "method_status": {method: self.authority[method]["status"] for method in self.methods},
                "occurrence_rows": len(rows),
                "committed_rows": sum(bool(row["committed"]) for row in rows),
                "totals": totals,
                "relationships": relationships}


def verify_experiment(experiment: Path) -> dict[str, object]:
    """Verify canonical descriptors, row order, and byte artifacts fail closed."""
    try:
        manifest = json.loads((experiment / "manifest.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise MatrixError("verifier:manifest_unavailable") from exc
    if manifest.get("schema") != SCHEMA or not isinstance(manifest.get("artifacts"), dict):
        raise MatrixError("verifier:manifest_schema_invalid")
    actual_names = {str(p.relative_to(experiment)) for p in experiment.rglob("*")
                    if p.is_file() and p.name != "manifest.json"}
    declared = set(manifest["artifacts"])
    if actual_names != declared:
        raise MatrixError("verifier:artifact_added_or_deleted")
    for name, descriptor in manifest["artifacts"].items():
        path = experiment / name
        if (not isinstance(descriptor, Mapping) or descriptor.get("bytes") != path.stat().st_size or
                descriptor.get("sha256") != _sha256(path.read_bytes())):
            raise MatrixError("verifier:artifact_mutated:" + name)
    input_authority = manifest.get("input_authority")
    if isinstance(input_authority, Mapping):
        root = Path(str(input_authority.get("corpus_root", AUTH_CORPUS_ROOT))).resolve()
        selected = input_authority.get("selected_inputs", [])
        if not isinstance(selected, list):
            raise MatrixError("verifier:input_authority_invalid")
        for item in selected:
            if not isinstance(item, Mapping) or not isinstance(item.get("source_relative"), str):
                raise MatrixError("verifier:input_descriptor_invalid")
            path = root / str(item["source_relative"])
            facts = _private_digest(path, "verifier_input")
            if facts["sha256"] != item.get("sha256") or facts["bytes"] != item.get("bytes"):
                raise MatrixError("verifier:external_input_mutated")
    try:
        rows = [json.loads(line) for line in
                (experiment / "occurrences.jsonl").read_text(encoding="utf-8").splitlines()]
    except (OSError, json.JSONDecodeError) as exc:
        raise MatrixError("verifier:occurrences_invalid") from exc
    order = [{"ordinal": row.get("ordinal"), "method": row.get("method")} for row in rows]
    if order != manifest.get("row_order"):
        raise MatrixError("verifier:row_reordered_or_changed")
    for row in rows:
        for field in ("raw_path", "encoded_path"):
            name = row.get(field)
            digest_field = "raw_sha256" if field == "raw_path" else "encoded_sha256"
            if name is not None:
                descriptor = manifest["artifacts"].get(name)
                if descriptor is None or descriptor.get("sha256") != row.get(digest_field):
                    raise MatrixError("verifier:row_artifact_binding_invalid")
    return {"status": "PASS", "experiment": str(experiment), "rows": len(rows),
            "artifacts": len(declared)}


def firefox_occurrences(trace: Path, *, count: int = 100,
                        corpus_root: Path = AUTH_CORPUS_ROOT,
                        corpus_manifest: Path = AUTH_CORPUS_MANIFEST) -> list[Occurrence]:
    """Load Firefox inputs only after trace, corpus, and every file authenticate."""
    if type(count) is not int or count <= 0:
        raise MatrixError("requested count must be positive")
    trace_facts = _private_digest(trace, "trace")
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
    trace_raw, _ = _private_bytes(trace, "trace")
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
                                     source_logical=logical))
    except csv.Error as exc:
        raise MatrixError("trace:invalid_tsv") from exc
    if not trace_occurrences:
        raise MatrixError("trace:requested_rows_unavailable")
    # Join the authenticated global dispatch authority to trace logical rows.
    # This supports build boundaries and repeat-full without treating a
    # logical reset as a new dispatch identity.
    authority = _authenticated_assignment("C1F1/100000", count)
    result: list[Occurrence] = []
    for item in authority["rows"]:  # type: ignore[union-attr]
        logical = int(item["authority_logical"])
        if logical >= len(trace_occurrences):
            raise MatrixError("assignment:trace_logical_out_of_range")
        base = trace_occurrences[logical]
        result.append(Occurrence(ordinal=int(item["ordinal"]), raw=base.raw,
                                 source_relative=base.source_relative,
                                 source_sha256=base.source_sha256,
                                 source_path=base.source_path,
                                 source_build=int(item["authority_build"]),
                                 source_logical=logical))
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
    artifacts = {}
    for path in (experiment / "occurrences.jsonl", experiment / "summary.json"):
        raw = path.read_bytes()
        artifacts[path.name] = {"bytes": len(raw), "sha256": _sha256(raw)}
    manifest["artifacts"] = artifacts
    manifest["row_order"] = []
    (experiment / "manifest.json").write_bytes(_canonical(manifest))
    return experiment


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--firefox-trace", type=Path)
    parser.add_argument("--output-root", type=Path, default=Path("experiments"))
    parser.add_argument("--count", type=int, default=100)
    parser.add_argument("--depth", choices=("100", "200", "full-1", "state-carrying-full-2"))
    args = parser.parse_args(argv)
    if args.firefox_trace is None:
        parser.error("--firefox-trace is required")
    if args.depth == "state-carrying-full-2":
        parser.error("state-carrying-full-2 requires an authenticated full-1 predecessor plan")
    count = {None: args.count, "100": 100, "200": 200, "full-1": 2498}[args.depth]
    for topology_id in TOPOLOGY_IDS:
        topology = MatrixTopology.from_id(topology_id)
        try:
            occurrences = firefox_occurrences(args.firefox_trace, count=count)
            reason = ""
        except MatrixError as exc:
            occurrences = []
            reason = str(exc)
        if not occurrences:
            path = write_not_ready_canary(args.output_root, args.firefox_trace,
                                          topology=topology, count=count,
                                          reason="MISSING_AUTHORITY: " + (reason or
                                              "authenticated Firefox .ii inputs are unavailable"))
            print(path)
            continue
        path = MethodMatrixSimulator(topology).run(occurrences, output_root=args.output_root)
        assert isinstance(path, Path)
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
