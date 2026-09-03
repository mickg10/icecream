#!/usr/bin/env python3
"""Read-only reporter for verified S8 method-matrix experiments.

The reporter is deliberately not an execution engine.  It invokes the
simulator's verifier, reads the verified JSONL/schema, and writes a separate
descriptive report.  In particular, RAW_II is a byte-count control only and
cannot acquire product wire or timing witnesses here.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import stat
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Mapping, Sequence

try:  # Package import when used by a caller in farmharness.
    from . import s8_method_matrix_simulator as simulator
except ImportError:  # Direct script/PYTHONPATH invocation.
    import s8_method_matrix_simulator as simulator


REPORT_SCHEMA = "icecream-s8-method-matrix-report-v1"
UTC_SECOND = re.compile(r"^[0-9]{8}T[0-9]{6}Z$")
TOPOLOGIES = ("C1F1/100000", "C1F20/40")
DEPTHS = ("100", "200", "full-1", "state-carrying-full-2")
METHODS = tuple(simulator.METHODS)
CORE_METHODS = frozenset(simulator.CORE_METHODS)
STATEFUL_METHODS = frozenset(simulator.STATEFUL_METHODS)
METHOD_ORDER = {name: index for index, name in enumerate(METHODS)}
DEPTH_ORDER = {name: index for index, name in enumerate(DEPTHS)}
EXPECTED_TOPOLOGY = {
    "C1F1/100000": {"relationship_count": 1, "slots_per_f": 100000,
                    "global_slots": 100000},
    "C1F20/40": {"relationship_count": 20, "slots_per_f": 2,
                 "global_slots": 40},
}
ROW_STATUSES = frozenset(("READY", "NOT_READY", "NOT_IMPLEMENTED"))
PRODUCER_IDENTITY_SCHEMA = simulator.PRODUCER_IDENTITY_SCHEMA


class ReportError(ValueError):
    """Input or output does not satisfy the report contract."""


def _valid_run_identity(value: object, *, topology: str | None = None,
                        depth: str | None = None) -> bool:
    if not isinstance(value, Mapping) or set(value) != {"timestamp", "topology", "depth", "pass"}:
        return False
    timestamp = value.get("timestamp")
    if (not isinstance(timestamp, str) or UTC_SECOND.fullmatch(timestamp) is None or
            not isinstance(value.get("topology"), str) or
            not isinstance(value.get("depth"), str) or
            not isinstance(value.get("pass"), str) or
            not re.fullmatch(r"[A-Za-z0-9_.-]+", value["pass"])):
        return False
    try:
        datetime.strptime(timestamp, "%Y%m%dT%H%M%SZ")
    except ValueError:
        return False
    return ((topology is None or value["topology"] == topology) and
            (depth is None or value["depth"] == depth))


def _valid_digest(value: object) -> bool:
    return (isinstance(value, str) and value != "0" * 32 and
            re.fullmatch(r"[0-9a-f]{32}", value) is not None)


def _route_prefix(state: Mapping[str, Any]) -> tuple[int, str] | None:
    descriptor = state.get("committed_raw_prefix_descriptor")
    if (not isinstance(descriptor, Mapping) or
            set(descriptor) != {"schema", "bytes", "digest128"} or
            descriptor.get("schema") != simulator.PREFIX_DESCRIPTOR_SCHEMA):
        return None
    bytes_value = descriptor.get("bytes")
    digest = descriptor.get("digest128")
    if (type(bytes_value) is not int or not 0 <= bytes_value <= (1 << simulator.MAX_HISTORY_WINDOW_LOG) or
            not _valid_digest(digest)):
        return None
    return bytes_value, str(digest)


def _transaction_prefix(transaction: Mapping[str, Any], *, before: bool) -> tuple[int, str] | None:
    descriptor_name = ("committed_raw_prefix_before_descriptor" if before
                       else "committed_raw_prefix_descriptor")
    descriptor = transaction.get(descriptor_name)
    if (not isinstance(descriptor, Mapping) or
            set(descriptor) != {"schema", "bytes", "digest128"} or
            descriptor.get("schema") != simulator.PREFIX_DESCRIPTOR_SCHEMA):
        return None
    bytes_value = descriptor.get("bytes")
    digest = descriptor.get("digest128")
    if (type(bytes_value) is not int or not 0 <= bytes_value <= (1 << simulator.MAX_HISTORY_WINDOW_LOG) or
            not _valid_digest(digest)):
        return None
    return bytes_value, str(digest)


def _sha256(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def _private_bytes(path: Path, label: str) -> bytes:
    """Read one regular, non-followed file and detect replacement while open."""
    try:
        fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    except OSError as exc:
        raise ReportError(f"{label}:missing:{path}") from exc
    try:
        before = os.fstat(fd)
        if not stat.S_ISREG(before.st_mode) or before.st_nlink != 1:
            raise ReportError(f"{label}:not_private_regular:{path}")
        chunks: list[bytes] = []
        while True:
            block = os.read(fd, 1 << 20)
            if not block:
                break
            chunks.append(block)
        after = os.fstat(fd)
    finally:
        os.close(fd)
    raw = b"".join(chunks)
    if (len(raw) != before.st_size or after.st_dev != before.st_dev or
            after.st_ino != before.st_ino or after.st_nlink != before.st_nlink or
            after.st_size != before.st_size):
        raise ReportError(f"{label}:identity_changed:{path}")
    return raw


def _json(raw: bytes, label: str) -> Any:
    def reject_duplicate(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise ReportError(f"{label}:duplicate_key:{key}")
            result[key] = value
        return result

    try:
        return json.loads(raw.decode("utf-8"), object_pairs_hook=reject_duplicate,
                          parse_constant=lambda value: (_ for _ in ()).throw(
                              ReportError(f"{label}:nonfinite:{value}")))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ReportError(f"{label}:invalid_json") from exc


def _int_or_none(value: Any, field: str, *, nonnegative: bool = True) -> int | None:
    if value is None:
        return None
    if type(value) is not int or (nonnegative and value < 0):
        raise ReportError(f"row:{field}:invalid_integer")
    return value


def _ratio(raw: int | None, encoded: int | None) -> tuple[float | None, float | None, str | None]:
    if raw is None:
        return None, None, "raw_bytes_unavailable"
    if raw == 0:
        return None, None, "zero_raw_denominator"
    if encoded is None:
        return None, None, "encoded_bytes_unavailable"
    ratio = encoded / raw
    return ratio, 1.0 - ratio, None


def _stable_stamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def _new_output_dir(root: Path) -> Path:
    root = root.absolute()
    if root.exists() and (root.is_symlink() or not root.is_dir()):
        raise ReportError("output_root:is_not_directory")
    root.mkdir(parents=True, exist_ok=True)
    base = root / ("method-matrix-" + _stable_stamp())
    candidate = base
    suffix = 1
    while candidate.exists():
        candidate = root / f"{base.name}-r{suffix:02d}"
        suffix += 1
    candidate.mkdir()
    return candidate


def _topology_record(manifest: Mapping[str, Any], topology: str) -> tuple[int, int]:
    record = manifest.get("topology")
    expected = EXPECTED_TOPOLOGY[topology]
    if not isinstance(record, Mapping) or record.get("id") != topology:
        raise ReportError("manifest:topology_identity_invalid")
    for key, value in expected.items():
        if record.get(key) != value:
            raise ReportError(f"manifest:topology_{key}_invalid")
    return int(expected["relationship_count"]), int(expected["global_slots"])


def _not_proven(reason: str) -> dict[str, Any]:
    return {"status": "NOT_PROVEN", "predecessor_bound": False,
            "relationship_state_present": False, "relationship_state_methods": [],
            "reason": reason}


def _valid_producer_identity(value: object) -> bool:
    """Validate the compact identity emitted by the native simulator seam."""
    if not isinstance(value, Mapping) or set(value) != {
            "schema", "source", "p50sim_binary", "build_receipt"}:
        return False
    if value.get("schema") != PRODUCER_IDENTITY_SCHEMA:
        return False
    source = value.get("source")
    binary = value.get("p50sim_binary")
    receipt = value.get("build_receipt")
    return (
        isinstance(source, Mapping) and set(source) == {"head", "tree"} and
        isinstance(source.get("head"), str) and
        re.fullmatch(r"[0-9a-f]{40}", source["head"]) is not None and
        isinstance(source.get("tree"), str) and
        re.fullmatch(r"[0-9a-f]{40}", source["tree"]) is not None and
        isinstance(binary, Mapping) and set(binary) == {"path", "bytes", "sha256"} and
        isinstance(binary.get("path"), str) and bool(binary["path"]) and
        type(binary.get("bytes")) is int and binary["bytes"] > 0 and
        isinstance(binary.get("sha256"), str) and
        re.fullmatch(r"[0-9a-f]{64}", binary["sha256"]) is not None and
        isinstance(receipt, Mapping) and
        set(receipt) == {"schema", "path", "bytes", "sha256"} and
        receipt.get("schema") == "icecream-p50sim-build-v1" and
        isinstance(receipt.get("path"), str) and bool(receipt["path"]) and
        type(receipt.get("bytes")) is int and receipt["bytes"] > 0 and
        isinstance(receipt.get("sha256"), str) and
        re.fullmatch(r"[0-9a-f]{64}", receipt["sha256"]) is not None)


def _producer_identities(manifest: Mapping[str, Any]) -> dict[str, dict[str, Any]] | None:
    value = manifest.get("producer_identity")
    if not isinstance(value, Mapping):
        return None
    result: dict[str, dict[str, Any]] = {}
    for method, identity in value.items():
        if method not in METHODS or not _valid_producer_identity(identity):
            return None
        result[str(method)] = dict(identity)
    return result


def _full2_marker(manifest: Mapping[str, Any], summary: Mapping[str, Any],
                  rows: Sequence[Mapping[str, Any]], topology: str, depth: str,
                  ) -> dict[str, Any]:
    if depth != "state-carrying-full-2":
        return {"status": "NOT_APPLICABLE", "predecessor_bound": False,
                "relationship_state_present": False}
    predecessor = manifest.get("predecessor_input_authority")
    if manifest.get("repeat_full") is not True or not isinstance(predecessor, Mapping):
        return _not_proven("repeat_full_predecessor_declaration_missing")
    predecessor_path = predecessor.get("experiment")
    predecessor_digest = predecessor.get("manifest_sha256")
    predecessor_identity = predecessor.get("run_identity")
    selected_inputs = predecessor.get("selected_inputs")
    predecessor_assignment = predecessor.get("assignment")
    if (not isinstance(predecessor_path, str) or not predecessor_path or
            not isinstance(predecessor_digest, str) or
            not re.fullmatch(r"[0-9a-f]{64}", predecessor_digest) or
            not isinstance(predecessor_identity, Mapping) or
            not isinstance(selected_inputs, list) or
            not isinstance(predecessor_assignment, Mapping)):
        return _not_proven("predecessor_identity_binding_missing")
    try:
        predecessor_bundle = simulator.verify_experiment(Path(predecessor_path))
    except Exception:
        return _not_proven("predecessor_experiment_not_verified")
    if not isinstance(predecessor_bundle, Mapping):
        return _not_proven("predecessor_verified_bundle_invalid")
    predecessor_manifest = predecessor_bundle.get("manifest")
    predecessor_summary = predecessor_bundle.get("summary")
    predecessor_facts = predecessor_bundle.get("manifest_facts")
    if (not isinstance(predecessor_manifest, Mapping) or
            not isinstance(predecessor_summary, Mapping) or
            not isinstance(predecessor_facts, Mapping) or
            predecessor_facts.get("sha256") != predecessor_digest or
            predecessor_bundle.get("experiment") != predecessor_path):
        return _not_proven("predecessor_manifest_digest_mismatch")
    predecessor_run_identity = predecessor_manifest.get("run_identity")
    predecessor_topology = predecessor_manifest.get("topology")
    predecessor_summary_topology = predecessor_summary.get("topology")
    if (not isinstance(predecessor_topology, Mapping) or
            not isinstance(predecessor_summary_topology, Mapping) or
            predecessor_manifest.get("schema") != simulator.SCHEMA or
            predecessor_manifest.get("experiment") != Path(predecessor_path).name or
            predecessor_manifest.get("repeat_full") is not False or
            predecessor_topology.get("id") != topology or
            predecessor_run_identity != dict(predecessor_identity) or
            not _valid_run_identity(predecessor_run_identity, topology=topology,
                                    depth="full-1") or
            predecessor_summary.get("schema") != simulator.SUMMARY_SCHEMA or
            predecessor_summary_topology.get("id") != topology):
        return _not_proven("predecessor_run_identity_mismatch")
    current_producers = _producer_identities(manifest)
    predecessor_producers = _producer_identities(predecessor_manifest)
    if current_producers is None or predecessor_producers is None:
        return _not_proven("producer_identity_binding_missing")
    current_authority = manifest.get("authority")
    predecessor_authority = predecessor_manifest.get("authority")
    if not isinstance(current_authority, Mapping) or not isinstance(predecessor_authority, Mapping):
        return _not_proven("producer_identity_internal_authority_missing")
    for producer_method in STATEFUL_METHODS:
        current_producer = current_producers.get(producer_method)
        predecessor_producer = predecessor_producers.get(producer_method)
        if current_producer is None or predecessor_producer is None:
            return _not_proven(f"{producer_method.lower()}_producer_identity_missing")
        # Native method authority retains the same compact identity.  When
        # present, compare it as an internal consistency check so a manifest
        # cannot claim a producer that disagrees with its method authority.
        for authority, producer, label in (
                (current_authority, current_producer, "current"),
                (predecessor_authority, predecessor_producer, "predecessor")):
            method_authority = authority.get(producer_method)
            if (not isinstance(method_authority, Mapping) or
                    not _valid_producer_identity(method_authority.get("producer_identity")) or
                    method_authority.get("producer_identity") != producer):
                return _not_proven(
                    f"{producer_method.lower()}_{label}_producer_identity_internal_mismatch")
        current_binary = current_producer["p50sim_binary"]
        predecessor_binary = predecessor_producer["p50sim_binary"]
        if (current_binary["sha256"] != predecessor_binary["sha256"] or
                current_binary["bytes"] != predecessor_binary["bytes"]):
            return _not_proven(f"{producer_method.lower()}_p50sim_binary_mismatch")
    predecessor_input_authority = predecessor_manifest.get("input_authority")
    predecessor_inputs = (predecessor_input_authority.get("selected_inputs", [])
                          if isinstance(predecessor_input_authority, Mapping) else None)
    predecessor_authority = predecessor_manifest.get("assignment_authority")
    if (not isinstance(predecessor_inputs, list) or
            selected_inputs != predecessor_inputs or predecessor_assignment != predecessor_authority):
        return _not_proven("predecessor_input_assignment_mismatch")
    current_authority = manifest.get("assignment_authority")
    if not isinstance(current_authority, Mapping) or current_authority.get("topology") != topology:
        return _not_proven("current_assignment_authority_missing")
    relationships = summary.get("relationships", {})
    predecessor_relationships = predecessor_summary.get("relationships", {})
    if not isinstance(relationships, Mapping) or not isinstance(predecessor_relationships, Mapping):
        return _not_proven("relationship_state_missing")
    expected_keys = {"|".join(key) for key in simulator.MatrixTopology.from_id(topology).relationship_keys}
    ordered_rows: dict[tuple[str, tuple[str, str]], list[Mapping[str, Any]]] = {}
    for row in rows:
        key = row.get("relationship_key")
        if isinstance(key, list) and len(key) == 2 and all(isinstance(part, str) for part in key):
            method = row.get("method")
            if method in ("ZSTD_ROUTE", "P29", "GRZ_RESIDUAL"):
                ordered_rows.setdefault((method, (key[0], key[1])), []).append(row)
    for method in ("ZSTD_ROUTE", "P29", "GRZ_RESIDUAL"):
        current_state = relationships.get(method)
        prior_state = predecessor_relationships.get(method)
        if (not isinstance(current_state, Mapping) or not isinstance(prior_state, Mapping) or
                set(current_state) != expected_keys or set(prior_state) != expected_keys):
            return _not_proven(f"{method.lower()}_relationship_state_incomplete")
        # C-wide TU continuation is a single method authority, not a
        # relationship-local counter.  Every relationship's observation must
        # bind to the same predecessor/current authority marker.
        c_authorities = summary.get("c_authorities", {})
        predecessor_c_authorities = predecessor_summary.get("c_authorities", {})
        current_c = c_authorities.get(method) if isinstance(c_authorities, Mapping) else None
        prior_c = (predecessor_c_authorities.get(method)
                   if isinstance(predecessor_c_authorities, Mapping) else None)
        if (not isinstance(prior_c, Mapping) or type(prior_c.get("native_next_tu_seq")) is not int or
                prior_c["native_next_tu_seq"] <= 0 or
                not isinstance(current_c, Mapping) or type(current_c.get("native_next_tu_seq")) is not int or
                current_c["native_next_tu_seq"] <= prior_c["native_next_tu_seq"] or
                current_c["native_next_tu_seq"] != prior_c["native_next_tu_seq"] + len(
                    [row for row in rows if row.get("method") == method])):
            return _not_proven(f"{method.lower()}_c_wide_tu_authority_missing")
        method_rows = [row for row in rows if row.get("method") == method]
        for index, current_row in enumerate(method_rows):
            expected_tu = prior_c["native_next_tu_seq"] + index
            if (current_row.get("native_tu_seq") != expected_tu or
                    current_row.get("native_next_tu_seq") != expected_tu + 1):
                return _not_proven(f"{method.lower()}_global_tu_sequence_invalid")
        for key in expected_keys:
            before = prior_state[key]
            after = current_state[key]
            expected_route_identity = key.replace("|", "->")
            if (not isinstance(before, Mapping) or not isinstance(after, Mapping) or
                    type(before.get("native_last_tu_seq")) is not int or
                    type(before.get("native_next_rel_seq")) is not int or
                    before["native_next_rel_seq"] <= 0 or
                    not _valid_digest(before.get("native_state_digest")) or
                    type(before.get("history_nonce")) is not int or
                    before["history_nonce"] <= 0 or
                    not isinstance(before.get("route_identity"), str) or
                    not before["route_identity"] or
                    before["route_identity"] != expected_route_identity or
                    type(after.get("native_last_tu_seq")) is not int or
                    type(after.get("native_next_rel_seq")) is not int or
                    after["native_next_rel_seq"] <= 0 or
                    not _valid_digest(after.get("native_state_digest")) or
                    type(after.get("history_nonce")) is not int or
                    after["history_nonce"] <= 0 or
                    not isinstance(after.get("route_identity"), str) or
                    not after["route_identity"] or
                    after["route_identity"] != expected_route_identity or
                    after["history_nonce"] != before["history_nonce"] or
                    after["route_identity"] != before["route_identity"] or
                    after["native_last_tu_seq"] < before["native_last_tu_seq"]):
                return _not_proven(f"{method.lower()}_native_state_marker_missing")
            before_prefix = after_prefix = None
            if method == "ZSTD_ROUTE":
                before_prefix = _route_prefix(before)
                after_prefix = _route_prefix(after)
                if before_prefix is None or after_prefix is None:
                    return _not_proven("zstd_route_prefix_marker_missing")
            current_rows = ordered_rows.get((method, tuple(key.split("|", 1))), [])
            if not current_rows:
                return _not_proven(f"{method.lower()}_successor_marker_mismatch")
            ordinals = [row.get("ordinal") for row in current_rows]
            sequences = [row.get("native_tu_seq") for row in current_rows]
            if (any(type(ordinal) is not int for ordinal in ordinals) or
                    any(type(sequence) is not int or sequence < 0 for sequence in sequences) or
                    any(left >= right for left, right in zip(ordinals, ordinals[1:])) or
                    any(left >= right for left, right in zip(sequences, sequences[1:]))):
                return _not_proven(f"{method.lower()}_current_row_order_invalid")
            row = current_rows[0]
            last_row = current_rows[-1]
            if (row.get("native_next_rel_seq") != before["native_next_rel_seq"] + 1 or
                    row.get("native_state_before_digest") != before["native_state_digest"]):
                return _not_proven(f"{method.lower()}_successor_marker_mismatch")
            prior_state_digest = before["native_state_digest"]
            prior_prefix = before_prefix
            for row_index, current_row in enumerate(current_rows):
                transaction = current_row.get("product_transaction")
                previous_next_rel = (current_rows[row_index - 1].get("native_next_rel_seq")
                                     if row_index else before.get("native_next_rel_seq"))
                native_next_rel = current_row.get("native_next_rel_seq")
                rel_seq = current_row.get("rel_seq")
                transaction_tu = (transaction.get("tu_seq")
                                  if isinstance(transaction, Mapping) else None)
                transaction_rel = (transaction.get("rel_seq")
                                   if isinstance(transaction, Mapping) else None)
                transaction_next_rel = (transaction.get("native_next_rel_seq")
                                        if isinstance(transaction, Mapping) else None)
                if (type(previous_next_rel) is not int or
                        type(native_next_rel) is not int or
                        type(rel_seq) is not int or
                        native_next_rel != previous_next_rel + 1 or
                        native_next_rel != rel_seq + 1 or
                        current_row.get("committed") is not True or
                        not isinstance(transaction, Mapping) or
                        transaction.get("committed") is not True or
                        type(transaction_tu) is not int or
                        transaction_tu != current_row.get("native_tu_seq") or
                        type(transaction_rel) is not int or
                        transaction_rel != rel_seq or
                        type(transaction_next_rel) is not int or
                        transaction_next_rel != native_next_rel or
                        current_row.get("native_state_before_digest") != prior_state_digest or
                        transaction.get("state_before_digest") != prior_state_digest or
                        not _valid_digest(transaction.get("state_digest")) or
                        transaction.get("state_digest") != current_row.get("native_state_digest") or
                        transaction.get("history_nonce") != before["history_nonce"] or
                        transaction.get("route_identity") != expected_route_identity):
                    return _not_proven(f"{method.lower()}_current_transaction_continuity_mismatch")
                if method == "ZSTD_ROUTE":
                    transaction_before = _transaction_prefix(transaction, before=True)
                    transaction_after = _transaction_prefix(transaction, before=False)
                    if transaction_before is None or transaction_after is None or transaction_before != prior_prefix:
                        return _not_proven("zstd_route_prefix_successor_mismatch")
                    prior_prefix = transaction_after
                prior_state_digest = transaction["state_digest"]
            last_transaction = current_rows[-1]["product_transaction"]
            if (after["native_last_tu_seq"] != current_rows[-1]["native_tu_seq"] or
                    after["native_next_rel_seq"] != current_rows[-1]["native_next_rel_seq"] or
                    after["native_state_digest"] != last_transaction["state_digest"]):
                return _not_proven(f"{method.lower()}_final_state_marker_mismatch")
            if method == "ZSTD_ROUTE" and (
                    last_transaction.get("committed_raw_prefix_descriptor") !=
                    after.get("committed_raw_prefix_descriptor")):
                return _not_proven("zstd_route_final_prefix_mismatch")
    return {"status": "CONTINUOUS", "predecessor_bound": True,
            "relationship_state_present": True,
            "relationship_state_methods": ["ZSTD_ROUTE", "P29", "GRZ_RESIDUAL"],
            "predecessor_experiment": predecessor_path,
            "predecessor_manifest_sha256": predecessor_digest,
            "compatibility_basis": "bit_identical_p50sim",
            "producer_identity": {"predecessor": predecessor_producers,
                                   "current": current_producers}}


def _validate_rows(rows: Sequence[Any], manifest: Mapping[str, Any], topology: str,
                  experiment: Path) -> dict[str, list[Mapping[str, Any]]]:
    grouped: dict[str, list[Mapping[str, Any]]] = {method: [] for method in METHODS}
    seen: set[tuple[Any, str]] = set()
    for row in rows:
        if not isinstance(row, Mapping):
            raise ReportError("occurrences:row_not_object")
        method = row.get("method")
        if method not in METHODS:
            raise ReportError("occurrences:unknown_method")
        if row.get("schema") != simulator.OCCURRENCE_SCHEMA:
            raise ReportError("occurrences:row_schema_invalid")
        if row.get("topology") != topology:
            raise ReportError("occurrences:topology_mismatch")
        if type(row.get("ordinal")) is not int or row["ordinal"] < 0:
            raise ReportError("occurrences:ordinal_invalid")
        key = (row.get("ordinal"), str(method))
        if key in seen:
            raise ReportError("occurrences:duplicate_ordinal_method")
        seen.add(key)
        for field in ("raw_bytes", "encoded_bytes", "codec_cpu_ns", "codec_wall_ns"):
            _int_or_none(row.get(field), field)
        raw_bytes = row.get("raw_bytes")
        if type(raw_bytes) is not int or raw_bytes < 0:
            raise ReportError("row:raw_bytes:invalid_integer")
        wire = row.get("wire_witnessed")
        if type(wire) is not bool:
            raise ReportError("row:wire_witnessed:invalid_boolean")
        if row.get("status") not in ROW_STATUSES:
            raise ReportError("row:status_invalid")
        if wire and not isinstance(row.get("product_transaction"), Mapping):
            raise ReportError("row:wire_witnessed_without_product_transaction")
        transaction = row.get("product_transaction")
        if transaction is not None and not isinstance(transaction, Mapping):
            raise ReportError("row:product_transaction_invalid")
        if isinstance(transaction, Mapping):
            for field in ("c_to_f_bytes", "f_to_c_bytes", "simulator_execution_ns"):
                _int_or_none(transaction.get(field), field)
            if ("prepare_ns" in transaction and
                    _int_or_none(transaction.get("prepare_ns"), "prepare_ns") is None):
                raise ReportError("row:prepare_ns:invalid_integer")
        if method == "RAW_II":
            if any(row.get(field) is not None for field in
                   ("encoded_bytes", "codec_cpu_ns", "codec_wall_ns")):
                raise ReportError("RAW_II:payload_or_time_witness_present")
            if wire:
                raise ReportError("RAW_II:wire_witness_forbidden")
        grouped[str(method)].append(row)
    return grouped


def _validate_summary_totals(summary: Mapping[str, Any],
                             grouped: Mapping[str, Sequence[Mapping[str, Any]]]) -> None:
    """Cross-check semantic totals; a descriptor hash alone cannot fix bad JSON."""
    totals = summary.get("totals", {})
    if not isinstance(totals, Mapping):
        # NOT_READY canaries intentionally have no occurrence totals.
        if any(grouped.values()):
            raise ReportError("summary:totals_missing")
        return
    for method, rows in grouped.items():
        if not rows or method not in totals:
            continue
        total = totals[method]
        if not isinstance(total, Mapping):
            raise ReportError(f"summary:totals_{method}_invalid")
        raw = sum(int(row["raw_bytes"]) for row in rows)
        if total.get("raw_bytes") != raw:
            raise ReportError(f"summary:raw_total_mismatch:{method}")
        encoded_values = [row.get("encoded_bytes") for row in rows]
        if all(value is not None for value in encoded_values):
            if total.get("encoded_bytes") != sum(int(value) for value in encoded_values):
                raise ReportError(f"summary:encoded_total_mismatch:{method}")
        elif total.get("encoded_bytes") not in (None, 0):
            raise ReportError(f"summary:encoded_unavailable_mismatch:{method}")
        wire = bool(rows) and all(bool(row.get("wire_witnessed")) and
                                  isinstance(row.get("product_transaction"), Mapping)
                                  for row in rows)
        if bool(total.get("wire_witnessed")) != wire:
            raise ReportError(f"summary:wire_witness_mismatch:{method}")
        if wire:
            transactions = [row["product_transaction"] for row in rows]
            prepare_present = ["prepare_ns" in tx for tx in transactions]
            if any(prepare_present) and not all(prepare_present):
                raise ReportError(f"summary:prepare_ns_mixed_availability:{method}")
            prepare_ns = (sum(int(tx["prepare_ns"]) for tx in transactions)
                          if transactions and all(prepare_present) else None)
            execution_ns = sum(int(tx.get("simulator_execution_ns", 0))
                               for tx in transactions)
            expected = {
                "c_to_f_bytes": sum(int(tx.get("c_to_f_bytes", 0)) for tx in transactions),
                "f_to_c_bytes": sum(int(tx.get("f_to_c_bytes", 0)) for tx in transactions),
                "prepare_ns": prepare_ns,
                "execution_ns": execution_ns,
                "codec_ns": (prepare_ns + execution_ns
                             if prepare_ns is not None else None),
            }
            for field, value in expected.items():
                if total.get(field) != value:
                    raise ReportError(f"summary:{field}_mismatch:{method}")


def _method_result(experiment: Path, manifest: Mapping[str, Any], summary: Mapping[str, Any],
                   grouped: Mapping[str, Sequence[Mapping[str, Any]]], method: str,
                   topology: str, depth: str, pass_id: str, timestamp: str,
                   marker: Mapping[str, Any], manifest_sha256: str | None = None) -> dict[str, Any]:
    rows = list(grouped.get(method, ()))
    status_map = summary.get("method_status", {})
    status = status_map.get(method) if isinstance(status_map, Mapping) else None
    requested = method in set(manifest.get("methods", ()))
    reason: str | None = None
    if not requested:
        status, reason = "UNAVAILABLE", "method_not_requested_in_verified_experiment"
    elif not rows:
        # An authority declaration alone is not a measurement row.  Keep the
        # experiment method explicitly unavailable rather than upgrading a
        # NOT_READY canary's RAW authority to a witnessed result.
        status = "UNAVAILABLE"
        authority = manifest.get("authority", {})
        detail = authority.get(method, {}) if isinstance(authority, Mapping) else {}
        reason = (str(detail.get("reason")) if isinstance(detail, Mapping) and detail.get("reason")
                  else "no_verified_occurrence_rows")
    else:
        status = str(status or rows[0].get("status") or "UNKNOWN")
        row_statuses = {str(row.get("status")) for row in rows}
        if len(row_statuses) != 1 or status not in row_statuses:
            raise ReportError(f"method:{method}:status_summary_mismatch")
        if status != "READY":
            reasons = [str(row.get("reason")) for row in rows if row.get("reason")]
            reason = reasons[0] if reasons else "method_not_ready"

    raw = sum(int(row["raw_bytes"]) for row in rows) if rows else None
    encoded_values = [row.get("encoded_bytes") for row in rows]
    if any(value is None for value in encoded_values):
        encoded = None
    else:
        encoded = sum(int(value) for value in encoded_values) if rows else None
    transactions = [row.get("product_transaction") for row in rows
                    if isinstance(row.get("product_transaction"), Mapping)]
    wire = bool(rows) and len(transactions) == len(rows) and all(
        bool(row.get("wire_witnessed")) for row in rows)
    if wire:
        c_to_f = sum(_int_or_none(tx.get("c_to_f_bytes"), "c_to_f_bytes") or 0
                     for tx in transactions)
        f_to_c = sum(_int_or_none(tx.get("f_to_c_bytes"), "f_to_c_bytes") or 0
                     for tx in transactions)
        execution = sum(_int_or_none(tx.get("simulator_execution_ns"), "execution_ns") or 0
                        for tx in transactions)
        prepare_values = [_int_or_none(tx.get("prepare_ns"), "prepare_ns")
                          for tx in transactions]
        prepare = (sum(value for value in prepare_values if value is not None)
                   if prepare_values and all(value is not None for value in prepare_values)
                   else None)
        codec = prepare + execution if prepare is not None else None
    else:
        c_to_f = f_to_c = prepare = execution = codec = None
        if reason is None and method != "RAW_II":
            reason = "product_transaction_wire_witness_unavailable"
    ratio, reduction, ratio_reason = _ratio(raw, encoded)
    return {
        "schema": REPORT_SCHEMA,
        "source_experiment": str(experiment),
        "source_manifest_sha256": manifest_sha256,
        "topology": topology,
        "relationship_count": EXPECTED_TOPOLOGY[topology]["relationship_count"],
        "capacity": EXPECTED_TOPOLOGY[topology]["global_slots"],
        "depth": depth, "pass": pass_id, "run_timestamp": timestamp,
        "run_identity": {"timestamp": timestamp, "topology": topology,
                         "depth": depth, "pass": pass_id},
        "method": method, "classification": "core" if method in CORE_METHODS else "optional",
        "status": status, "reason": reason,
        "raw_bytes": raw, "encoded_bytes": encoded,
        "c_to_f_bytes": c_to_f, "f_to_c_bytes": f_to_c,
        "prepare_ns": prepare, "execution_ns": execution,
        "codec_ns": codec,
        "wire_witnessed": wire, "compression_ratio": ratio,
        "byte_reduction_fraction": reduction, "ratio_reason": ratio_reason,
        "full2_continuity": dict(marker),
    }


def _load_experiment(path: Path) -> tuple[dict[str, Any], list[dict[str, Any]], dict[str, Any],
                                          dict[str, Any], tuple[str, str, str], str,
                                          dict[str, Any]]:
    path = path.absolute()
    try:
        root_info = path.lstat()
    except OSError as exc:
        raise ReportError(f"experiment:missing:{path}") from exc
    if stat.S_ISLNK(root_info.st_mode) or not stat.S_ISDIR(root_info.st_mode):
        raise ReportError("experiment:not_private_directory")
    try:
        verified = simulator.verify_experiment(path)
    except Exception as exc:
        raise ReportError(f"experiment_verification_failed:{path}:{exc}") from exc
    if not isinstance(verified, Mapping):
        raise ReportError("experiment_verification_failed:verified_bundle_invalid")
    manifest = verified.get("manifest")
    summary = verified.get("summary")
    occurrences = verified.get("rows_data")
    manifest_facts = verified.get("manifest_facts")
    if (not isinstance(manifest, Mapping) or not isinstance(summary, Mapping) or
            not isinstance(occurrences, list) or not isinstance(manifest_facts, Mapping) or
            not isinstance(manifest_facts.get("sha256"), str)):
        raise ReportError("experiment_verification_failed:verified_bundle_incomplete")
    if not isinstance(manifest, Mapping) or manifest.get("schema") != simulator.SCHEMA:
        raise ReportError("manifest:schema_invalid")
    if not isinstance(summary, Mapping) or summary.get("schema") != simulator.SUMMARY_SCHEMA:
        raise ReportError("summary:schema_invalid")
    identity = manifest.get("run_identity")
    if not isinstance(identity, Mapping):
        raise ReportError("manifest:run_identity_missing")
    if not _valid_run_identity(identity):
        raise ReportError("manifest:run_identity_keys_invalid")
    timestamp = identity.get("timestamp")
    topology, depth, pass_id = (identity.get("topology"), identity.get("depth"), identity.get("pass"))
    if (not isinstance(topology, str) or topology not in TOPOLOGIES or
            not isinstance(depth, str) or depth not in DEPTHS or
            not isinstance(pass_id, str) or not re.fullmatch(r"[A-Za-z0-9_.-]+", pass_id)):
        raise ReportError("manifest:run_identity_dimension_invalid")
    _topology_record(manifest, str(topology))
    summary_topology = summary.get("topology")
    expected_topology = EXPECTED_TOPOLOGY[str(topology)]
    canary_without_rows = (summary.get("status") == "NOT_READY" and
                           summary.get("occurrence_rows") == 0 and
                           summary_topology is None)
    if (not canary_without_rows and
            (not isinstance(summary_topology, Mapping) or summary_topology.get("id") != topology or
             summary.get("relationship_count") != expected_topology["relationship_count"])):
        raise ReportError("summary:topology_binding_invalid")
    # Older NOT_READY canaries did not include the optional experiment name;
    # their verifier-bound directory and run_identity still bind the source.
    if manifest.get("experiment", path.name) != path.name or summary.get("experiment") != str(path):
        raise ReportError("experiment:identity_binding_invalid")
    methods = manifest.get("methods")
    if not isinstance(methods, list) or len(set(methods)) != len(methods) or any(m not in METHODS for m in methods):
        raise ReportError("manifest:methods_invalid")
    method_status = summary.get("method_status")
    if not isinstance(method_status, Mapping) or any(method not in method_status for method in methods):
        raise ReportError("summary:method_status_missing")
    if summary.get("occurrence_rows") != len(occurrences):
        raise ReportError("summary:occurrence_count_mismatch")
    grouped = _validate_rows(occurrences, manifest, str(topology), path)
    _validate_summary_totals(summary, grouped)
    marker = _full2_marker(manifest, summary, [dict(row) for row in occurrences],
                           str(topology), str(depth))
    return (dict(manifest), [dict(row) for row in occurrences], dict(summary), grouped,
            (str(topology), str(depth), pass_id), str(manifest_facts["sha256"]), marker)


def build_report(experiments: Sequence[Path], output_root: Path) -> Path:
    if not experiments:
        raise ReportError("at_least_one_experiment_required")
    records: list[dict[str, Any]] = []
    seen_keys: set[tuple[str, str, str]] = set()
    sources: list[dict[str, Any]] = []
    for path in experiments:
        manifest, _occurrences, summary, grouped, identity, manifest_sha256, marker = _load_experiment(path)
        if identity in seen_keys:
            raise ReportError("duplicate_experiment_key:" + "/".join(identity))
        seen_keys.add(identity)
        topology, depth, pass_id = identity
        sources.append({"experiment": str(path.absolute()), "manifest_sha256": manifest_sha256,
                        "run_identity": {"topology": topology, "depth": depth, "pass": pass_id},
                        "producer_identity": manifest.get("producer_identity", {})})
        records.extend(_method_result(path.absolute(), manifest, summary, grouped, method,
                                      topology, depth, pass_id, str(manifest["run_identity"]["timestamp"]),
                                      marker, manifest_sha256) for method in METHODS)
    records.sort(key=lambda row: (row["topology"], DEPTH_ORDER[row["depth"]], row["pass"],
                                  METHOD_ORDER[row["method"]]))
    covered = {(row["topology"], row["depth"]) for row in records}
    required = {(topology, depth) for topology in TOPOLOGIES for depth in DEPTHS}
    missing = [{"topology": topology, "depth": depth} for topology, depth in sorted(
        required - covered, key=lambda item: (TOPOLOGIES.index(item[0]), DEPTH_ORDER[item[1]]))]
    complete_experiments = True
    for source in sources:
        source_rows = [row for row in records
                       if row["source_experiment"] == source["experiment"]]
        core_rows = [row for row in source_rows if row["method"] in CORE_METHODS]
        core_ready = (
            {row["method"] for row in source_rows} >= CORE_METHODS and
            all(row["status"] == "READY" and
                (row["method"] == "RAW_II" or row["wire_witnessed"])
                for row in core_rows))
        depth = source["run_identity"]["depth"]
        continuity_ready = (
            depth != "state-carrying-full-2" or
            all(row.get("full2_continuity", {}).get("status") == "CONTINUOUS"
                for row in core_rows))
        if not core_ready or not continuity_ready:
            complete_experiments = False
    missing_core_evidence = [
        {"experiment": source["experiment"], "method": row["method"],
         "reason": ("method_not_ready" if row["status"] != "READY" else
                    "full2_continuity_not_proven"
                    if source["run_identity"]["depth"] == "state-carrying-full-2" and
                    row.get("full2_continuity", {}).get("status") != "CONTINUOUS" else
                    "product_transaction_wire_witness_unavailable")}
        for source in sources for row in records
        if row["source_experiment"] == source["experiment"] and row["method"] in CORE_METHODS
        and (row["status"] != "READY" or
             (row["method"] != "RAW_II" and not row["wire_witnessed"]) or
             (source["run_identity"]["depth"] == "state-carrying-full-2" and
              row.get("full2_continuity", {}).get("status") != "CONTINUOUS"))]
    matrix_status = "COMPLETE" if not missing and complete_experiments else "INCOMPLETE_REQUESTED_MATRIX"
    summary = {
        "schema": REPORT_SCHEMA, "status": matrix_status,
        "complete_requested_matrix": matrix_status == "COMPLETE",
        "requested_dimensions": {"topologies": list(TOPOLOGIES), "depths": list(DEPTHS),
                                 "methods": list(METHODS)},
        "required_core_methods": sorted(CORE_METHODS, key=METHOD_ORDER.get),
        "optional_methods": [m for m in METHODS if m not in CORE_METHODS],
        "source_experiments": sources, "experiment_keys": [list(key) for key in sorted(seen_keys)],
        "missing_dimensions": missing,
        "missing_core_evidence": missing_core_evidence,
        "optional_unavailable_is_not_core_failure": True,
        "rows": len(records),
        "loss_curve": {"kind": "transparent_tabular_points_by_depth", "fitted": False,
                       "points": [{"topology": row["topology"], "depth": row["depth"],
                                   "pass": row["pass"], "method": row["method"],
                                   "raw_bytes": row["raw_bytes"], "encoded_bytes": row["encoded_bytes"],
                                   "byte_reduction_fraction": row["byte_reduction_fraction"]}
                                  for row in records]},
        "results": records,
    }
    output = _new_output_dir(output_root)
    results_raw = b"".join((json.dumps(row, sort_keys=True, separators=(",", ":"),
                                      ensure_ascii=True, allow_nan=False) + "\n").encode("utf-8")
                            for row in records)
    (output / "results.jsonl").write_bytes(results_raw)
    (output / "summary.json").write_text(json.dumps(summary, sort_keys=True, indent=2,
                                                       ensure_ascii=True, allow_nan=False) + "\n")
    fields = list(records[0].keys()) if records else []
    with (output / "matrix.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="raise")
        writer.writeheader()
        for row in records:
            writer.writerow({key: (json.dumps(value, sort_keys=True, separators=(",", ":"))
                               if isinstance(value, (dict, list)) else value)
                             for key, value in row.items()})
    lines = ["# S8 method matrix report", "", f"Status: **{matrix_status}**", "",
             "The loss curve is a transparent tabular point set by depth; no fitted model is used.", "",
             "| topology | depth | pass | method | status | raw | encoded | C→F | F→C | prepare ns | exchange ns | codec ns | ratio | reduction | reason |",
             "|---|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|"]
    for row in records:
        values = [row.get(key) for key in ("topology", "depth", "pass", "method", "status",
                                            "raw_bytes", "encoded_bytes", "c_to_f_bytes",
                                            "f_to_c_bytes", "prepare_ns", "execution_ns",
                                            "codec_ns", "compression_ratio",
                                            "byte_reduction_fraction", "reason")]
        lines.append("| " + " | ".join("" if value is None else str(value).replace("|", "\\|")
                                         for value in values) + " |")
    (output / "table.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return output


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("experiments", nargs="+", type=Path)
    parser.add_argument("--output-root", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        output = build_report(args.experiments, args.output_root)
    except ReportError as exc:
        print(f"REJECT: {exc}", file=sys.stderr)
        return 2
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
