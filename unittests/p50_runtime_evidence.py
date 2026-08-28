#!/usr/bin/env python3
"""Collect authenticated P50 runtime evidence into one experiment artifact.

The cache service owns the READY v2 frame.  This collector only copies that
frame and the existing completion-flow observations; it does not infer cache
identity, transaction identity, or timing statistics from logs.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any, Mapping


SCHEMA = "icecream-p50-runtime-evidence-v1"
READY_KEYS = (
    "generation", "attempt", "F_STORE_GENERATION", "DERIVATION_VERSION",
    "pid", "C_STORE_GUID", "F_STORE_GUID", "PATH", "DIGEST", "DEV", "INO",
)
READY_HEX_FIELDS = {"C_STORE_GUID", "F_STORE_GUID", "DIGEST"}
READY_PREFIX = re.compile(r"READY v2 ")
READY_FIELDS = frozenset({
    "record", "source", "control_generation", "control_attempt",
    "f_store_generation", "derivation_version", "pid", "c_store_guid",
    "f_store_guid", "path", "socket_digest", "device", "inode",
})


def parse_ready_lines(text: str) -> list[dict[str, Any]]:
    """Return only complete, fixed-order READY v2 frames."""
    rows: list[dict[str, Any]] = []
    for line in text.splitlines():
        marker = READY_PREFIX.search(line)
        if marker is None:
            continue
        tokens = line[marker.start():].strip().split()
        if len(tokens) != len(READY_KEYS) + 2 or tokens[:2] != ["READY", "v2"]:
            continue
        values: dict[str, str] = {}
        valid = True
        for token, key in zip(tokens[2:], READY_KEYS):
            prefix = key + "="
            if not token.startswith(prefix) or not token[len(prefix):]:
                valid = False
                break
            values[key] = token[len(prefix):]
        if not valid:
            continue
        numbers: dict[str, int] = {}
        for key in set(READY_KEYS) - READY_HEX_FIELDS - {"PATH"}:
            value = values[key]
            if not value.isdecimal() or int(value) <= 0:
                valid = False
                break
            numbers[key] = int(value)
        if not valid or numbers["DERIVATION_VERSION"] != 1:
            continue
        for key in READY_HEX_FIELDS:
            value = values[key]
            if len(value) != 32 or any(char not in "0123456789abcdefABCDEF"
                                       for char in value):
                valid = False
                break
        if not valid or any(char.isspace() for char in values["PATH"]):
            continue
        rows.append({
            "record": "f-store-ready",
            "source": "cache-service-ready-v2",
            "control_generation": numbers["generation"],
            "control_attempt": numbers["attempt"],
            "f_store_generation": numbers["F_STORE_GENERATION"],
            "derivation_version": numbers["DERIVATION_VERSION"],
            "pid": numbers["pid"],
            "c_store_guid": values["C_STORE_GUID"].lower(),
            "f_store_guid": values["F_STORE_GUID"].lower(),
            "path": values["PATH"],
            "socket_digest": values["DIGEST"].lower(),
            "device": numbers["DEV"],
            "inode": numbers["INO"],
        })
    return rows


def _file_evidence(path: Path) -> dict[str, Any]:
    raw = path.read_bytes()
    return {
        "path": path.name,
        "bytes": len(raw),
        "sha256": hashlib.sha256(raw).hexdigest(),
    }


def _valid_ready_row(row: Any) -> bool:
    if not isinstance(row, Mapping) or set(row) != READY_FIELDS:
        return False
    if row.get("record") != "f-store-ready" or row.get("source") != "cache-service-ready-v2":
        return False
    for field in ("control_generation", "control_attempt", "f_store_generation",
                  "derivation_version", "pid", "device", "inode"):
        if type(row.get(field)) is not int or row[field] <= 0:
            return False
    if row.get("derivation_version") != 1:
        return False
    for field in ("c_store_guid", "f_store_guid", "socket_digest"):
        value = row.get(field)
        if (not isinstance(value, str) or len(value) != 32 or
                any(char not in "0123456789abcdef" for char in value)):
            return False
    path = row.get("path")
    return (isinstance(path, str) and bool(path) and
            not any(char.isspace() for char in path))


def _lines(path: Path, prefix: str | None = None) -> list[str]:
    if not path.exists():
        return []
    result = path.read_text(encoding="utf-8", errors="replace").splitlines()
    if prefix is not None:
        result = [line for line in result if line.startswith(prefix)]
    return result


def verify_runtime_artifact(document: Mapping[str, Any]) -> dict[str, Any]:
    """Validate the runtime envelope and optionally invoke the full verifier.

    Runtime completion evidence is not a statistical document.  A complete
    statistical document may be attached by a producer under ``statistics``;
    if it is absent, the result remains an explicit HOLD with no fabricated
    observations.
    """
    issues: list[str] = []
    if document.get("schema") != SCHEMA:
        issues.append("runtime_schema_invalid")
    for field in ("experiment_id", "run_id"):
        if not isinstance(document.get(field), str) or not document[field]:
            issues.append(field + "_missing")
    ready = document.get("f_store_ready")
    if not isinstance(ready, list) or not ready:
        issues.append("f_store_ready_missing")
    else:
        if any(not _valid_ready_row(row) for row in ready):
            issues.append("f_store_ready_row_invalid")
        generations = {row.get("f_store_generation") for row in ready
                       if _valid_ready_row(row)}
        if len(generations) != 1:
            issues.append("f_store_generation_ambiguous")
    runtime = document.get("runtime")
    if not isinstance(runtime, Mapping) or not runtime.get("lifecycle"):
        issues.append("runtime_lifecycle_missing")
    identity_status = document.get("identity_status")
    if not isinstance(identity_status, list):
        issues.append("identity_status_missing")
    else:
        fields = {row.get("field") for row in identity_status
                  if isinstance(row, Mapping)}
        if fields != {"c_guid", "tu_seq"}:
            issues.append("identity_status_fields_incomplete")
        for row in identity_status:
            if (not isinstance(row, Mapping) or row.get("status") != "HOLD" or
                    not isinstance(row.get("reason"), str) or not row["reason"]):
                issues.append("identity_status_not_hold")

    statistics = document.get("statistics")
    verifier_result: dict[str, Any] | None = None
    if statistics is not None:
        if not isinstance(statistics, Mapping):
            issues.append("statistics_document_invalid")
        else:
            preregistration = statistics.get("preregistration")
            evidence = statistics.get("evidence")
            expected_digest = statistics.get("preregistration_sha256")
            if (not isinstance(preregistration, Mapping) or
                    not isinstance(evidence, Mapping) or
                    not isinstance(expected_digest, str)):
                issues.append("statistics_preregistration_missing")
            else:
                try:
                    try:
                        from capability.distribution.s5_statistics import verify_preregistered
                    except ImportError:
                        root = str(Path(__file__).resolve().parents[1])
                        if root not in sys.path:
                            sys.path.insert(0, root)
                        from capability.distribution.s5_statistics import verify_preregistered
                    verifier_result = verify_preregistered(
                        preregistration, evidence,
                        expected_digest=expected_digest,
                    )
                    if verifier_result.get("valid") is not True:
                        issues.extend("statistics:" + str(issue)
                                      for issue in verifier_result.get("issues", []))
                except (ImportError, OSError, TypeError, ValueError) as error:
                    issues.append("statistics_verifier_unavailable:" + type(error).__name__)
    else:
        issues.append("statistics_document_missing")

    if issues:
        status = "HOLD"
    elif verifier_result is not None:
        status = str(verifier_result.get("decision", "HOLD"))
    else:
        status = "HOLD"
    result: dict[str, Any] = {
        "status": status,
        "issues": sorted(set(issues)),
        "verifier": "s5-statistics-python-v10" if statistics is not None else None,
    }
    if verifier_result is not None:
        result["statistics_result"] = verifier_result
    return result


def collect(args: argparse.Namespace) -> dict[str, Any]:
    ready_path = Path(args.ready)
    lifecycle_path = Path(args.lifecycle)
    source_paths = [ready_path, lifecycle_path]
    if args.worker_log:
        source_paths.append(Path(args.worker_log))
    source_paths = [path for path in source_paths if path.exists()]
    ready = parse_ready_lines(ready_path.read_text(encoding="utf-8", errors="replace")
                              if ready_path.exists() else "")
    lifecycle = _lines(lifecycle_path, "P50_LIFECYCLE ")
    settlements = _lines(Path(args.worker_log), "P50 input settlement ") \
        if args.worker_log else []
    return {
        "schema": SCHEMA,
        "experiment_id": args.experiment_id,
        "run_id": args.run_id,
        "f_store_ready": ready,
        "runtime": {
            "lifecycle": lifecycle,
            "settlements": settlements,
        },
        "source_files": [_file_evidence(path) for path in source_paths],
        "identity_status": [
            {"field": "c_guid", "status": "HOLD",
             "reason": "completion artifact does not publish a C_GUID witness"},
            {"field": "tu_seq", "status": "HOLD",
             "reason": "completion artifact does not publish a TU_SEQ witness"},
        ],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--experiment-id", required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--ready", required=True, type=Path)
    parser.add_argument("--lifecycle", required=True, type=Path)
    parser.add_argument("--worker-log", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    document = collect(args)
    document["verification"] = verify_runtime_artifact(document)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    encoded = json.dumps(document, ensure_ascii=True, sort_keys=True,
                         separators=(",", ":")) + "\n"
    temporary = args.output.with_name(args.output.name + ".tmp")
    temporary.write_text(encoded, encoding="utf-8")
    temporary.replace(args.output)
    print(json.dumps(document["verification"], sort_keys=True,
                     separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
