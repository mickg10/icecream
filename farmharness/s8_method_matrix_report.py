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
TOPOLOGIES = ("C1F1/100000", "C1F20/40")
DEPTHS = ("100", "200", "full-1", "state-carrying-full-2")
METHODS = tuple(simulator.METHODS)
CORE_METHODS = frozenset(simulator.CORE_METHODS)
METHOD_ORDER = {name: index for index, name in enumerate(METHODS)}
DEPTH_ORDER = {name: index for index, name in enumerate(DEPTHS)}
EXPECTED_TOPOLOGY = {
    "C1F1/100000": {"relationship_count": 1, "slots_per_f": 100000,
                    "global_slots": 100000},
    "C1F20/40": {"relationship_count": 20, "slots_per_f": 2,
                 "global_slots": 40},
}
ROW_STATUSES = frozenset(("READY", "NOT_READY", "NOT_IMPLEMENTED"))


class ReportError(ValueError):
    """Input or output does not satisfy the report contract."""


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


def _read_json(path: Path, label: str) -> Any:
    return _json(_private_bytes(path, label), label)


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


def _full2_marker(manifest: Mapping[str, Any], summary: Mapping[str, Any],
                  topology: str, depth: str) -> dict[str, Any]:
    if depth != "state-carrying-full-2":
        return {"status": "NOT_APPLICABLE", "predecessor_bound": False,
                "relationship_state_present": False}
    predecessor = manifest.get("predecessor_input_authority")
    relationships = summary.get("relationships", {})
    route = relationships.get("ZSTD_ROUTE", {}) if isinstance(relationships, Mapping) else {}
    predecessor_assignment = (predecessor.get("assignment")
                              if isinstance(predecessor, Mapping) else None)
    selected_inputs = predecessor.get("selected_inputs") if isinstance(predecessor, Mapping) else None
    bound = (isinstance(predecessor, Mapping) and isinstance(selected_inputs, list) and
             bool(selected_inputs) and isinstance(predecessor_assignment, Mapping) and
             predecessor_assignment.get("topology") == topology)
    state = (isinstance(route, Mapping) and bool(route) and
             all(isinstance(value, Mapping) and type(value.get("next_rel_seq")) is int and
                 value["next_rel_seq"] > 0 for value in route.values()))
    if manifest.get("repeat_full") is not True or not bound or not state:
        raise ReportError("full2:predecessor_continuity_marker_missing")
    return {"status": "CONTINUOUS", "predecessor_bound": True,
            "relationship_state_present": True,
            "relationship_state_methods": ["ZSTD_ROUTE"]}


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
            expected = {
                "c_to_f_bytes": sum(int(tx.get("c_to_f_bytes", 0)) for tx in transactions),
                "f_to_c_bytes": sum(int(tx.get("f_to_c_bytes", 0)) for tx in transactions),
                "execution_ns": sum(int(tx.get("simulator_execution_ns", 0)) for tx in transactions),
            }
            for field, value in expected.items():
                if total.get(field) != value:
                    raise ReportError(f"summary:{field}_mismatch:{method}")


def _method_result(experiment: Path, manifest: Mapping[str, Any], summary: Mapping[str, Any],
                   grouped: Mapping[str, Sequence[Mapping[str, Any]]], method: str,
                   topology: str, depth: str, pass_id: str, timestamp: str,
                   marker: Mapping[str, Any]) -> dict[str, Any]:
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
    else:
        c_to_f = f_to_c = execution = None
        if reason is None and method != "RAW_II":
            reason = "product_transaction_wire_witness_unavailable"
    ratio, reduction, ratio_reason = _ratio(raw, encoded)
    return {
        "schema": REPORT_SCHEMA,
        "source_experiment": str(experiment),
        "source_manifest_sha256": _sha256(_private_bytes(experiment / "manifest.json", "manifest")),
        "topology": topology,
        "relationship_count": EXPECTED_TOPOLOGY[topology]["relationship_count"],
        "capacity": EXPECTED_TOPOLOGY[topology]["global_slots"],
        "depth": depth, "pass": pass_id, "run_timestamp": timestamp,
        "run_identity": {"timestamp": timestamp, "topology": topology,
                         "depth": depth, "pass": pass_id},
        "method": method, "classification": "core" if method in CORE_METHODS else "optional",
        "status": status, "reason": reason,
        "raw_bytes": raw, "encoded_bytes": encoded,
        "c_to_f_bytes": c_to_f, "f_to_c_bytes": f_to_c, "execution_ns": execution,
        "wire_witnessed": wire, "compression_ratio": ratio,
        "byte_reduction_fraction": reduction, "ratio_reason": ratio_reason,
        "full2_continuity": dict(marker),
    }


def _load_experiment(path: Path) -> tuple[dict[str, Any], list[dict[str, Any]], dict[str, Any],
                                          dict[str, Any], tuple[str, str, str]]:
    path = path.absolute()
    try:
        root_info = path.lstat()
    except OSError as exc:
        raise ReportError(f"experiment:missing:{path}") from exc
    if stat.S_ISLNK(root_info.st_mode) or not stat.S_ISDIR(root_info.st_mode):
        raise ReportError("experiment:not_private_directory")
    try:
        simulator.verify_experiment(path)
    except Exception as exc:
        raise ReportError(f"experiment_verification_failed:{path}:{exc}") from exc
    manifest = _read_json(path / "manifest.json", "manifest")
    summary = _read_json(path / "summary.json", "summary")
    if not isinstance(manifest, Mapping) or manifest.get("schema") != simulator.SCHEMA:
        raise ReportError("manifest:schema_invalid")
    if not isinstance(summary, Mapping) or summary.get("schema") != simulator.SUMMARY_SCHEMA:
        raise ReportError("summary:schema_invalid")
    identity = manifest.get("run_identity")
    if not isinstance(identity, Mapping):
        raise ReportError("manifest:run_identity_missing")
    topology, depth, pass_id = (identity.get("topology"), identity.get("depth"), identity.get("pass"))
    if topology not in TOPOLOGIES or depth not in DEPTHS or not isinstance(pass_id, str) or not pass_id:
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
    occurrences = _read_jsonl(path / "occurrences.jsonl", "occurrences")
    if summary.get("occurrence_rows") != len(occurrences):
        raise ReportError("summary:occurrence_count_mismatch")
    grouped = _validate_rows(occurrences, manifest, str(topology), path)
    _validate_summary_totals(summary, grouped)
    marker = _full2_marker(manifest, summary, str(topology), str(depth))
    return dict(manifest), occurrences, dict(summary), grouped, (str(topology), str(depth), pass_id)


def _read_jsonl(path: Path, label: str) -> list[dict[str, Any]]:
    raw = _private_bytes(path, label)
    rows: list[dict[str, Any]] = []
    for index, line in enumerate(raw.splitlines(), 1):
        if not line.strip():
            raise ReportError(f"{label}:blank_line:{index}")
        value = _json(line, f"{label}:{index}")
        if not isinstance(value, Mapping):
            raise ReportError(f"{label}:{index}:row_not_object")
        rows.append(dict(value))
    return rows


def build_report(experiments: Sequence[Path], output_root: Path) -> Path:
    if not experiments:
        raise ReportError("at_least_one_experiment_required")
    records: list[dict[str, Any]] = []
    seen_keys: set[tuple[str, str, str]] = set()
    sources: list[dict[str, Any]] = []
    for path in experiments:
        manifest, _occurrences, summary, grouped, identity = _load_experiment(path)
        if identity in seen_keys:
            raise ReportError("duplicate_experiment_key:" + "/".join(identity))
        seen_keys.add(identity)
        topology, depth, pass_id = identity
        marker = _full2_marker(manifest, summary, topology, depth)
        manifest_sha = _sha256(_private_bytes(path.absolute() / "manifest.json", "manifest"))
        sources.append({"experiment": str(path.absolute()), "manifest_sha256": manifest_sha,
                        "run_identity": {"topology": topology, "depth": depth, "pass": pass_id}})
        records.extend(_method_result(path.absolute(), manifest, summary, grouped, method,
                                      topology, depth, pass_id, str(manifest["run_identity"]["timestamp"]),
                                      marker) for method in METHODS)
    records.sort(key=lambda row: (row["topology"], DEPTH_ORDER[row["depth"]], row["pass"],
                                  METHOD_ORDER[row["method"]]))
    covered = {(row["topology"], row["depth"]) for row in records}
    required = {(topology, depth) for topology in TOPOLOGIES for depth in DEPTHS}
    missing = [{"topology": topology, "depth": depth} for topology, depth in sorted(
        required - covered, key=lambda item: (TOPOLOGIES.index(item[0]), DEPTH_ORDER[item[1]]))]
    complete_experiments = all(
        all(row["status"] == "READY" and
            (row["method"] == "RAW_II" or row["wire_witnessed"])
            for row in records
            if row["source_experiment"] == source["experiment"] and row["method"] in CORE_METHODS)
        and {row["method"] for row in records if row["source_experiment"] == source["experiment"]} >= CORE_METHODS
        for source in sources)
    missing_core_evidence = [
        {"experiment": source["experiment"], "method": row["method"],
         "reason": ("method_not_ready" if row["status"] != "READY"
                    else "product_transaction_wire_witness_unavailable")}
        for source in sources for row in records
        if row["source_experiment"] == source["experiment"] and row["method"] in CORE_METHODS
        and (row["status"] != "READY" or
             (row["method"] != "RAW_II" and not row["wire_witnessed"]))]
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
             "| topology | depth | pass | method | status | raw | encoded | C→F | F→C | execution ns | ratio | reduction | reason |",
             "|---|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---|"]
    for row in records:
        values = [row.get(key) for key in ("topology", "depth", "pass", "method", "status",
                                            "raw_bytes", "encoded_bytes", "c_to_f_bytes",
                                            "f_to_c_bytes", "execution_ns", "compression_ratio",
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
