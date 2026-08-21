#!/usr/bin/env python3
"""Fail-closed static contract for f2s-quotient-checks-v1.json."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

sys.dont_write_bytecode = True

EXPECTED_ROWS = (
    (
        "f2s-quotient-fixed",
        "F2SQuotientSimulation.cfg",
        "pass",
        "SimulationInvariant",
        None,
    ),
    (
        "f2s-quotient-two-frame-witness",
        "F2SQuotientTwoFrameWitness.cfg",
        "counterexample",
        "NoTwoFrameQueue",
        "trace-manifests/F2SQuotientTwoFrameWitness.manifest.template.json",
    ),
    (
        "f2s-quotient-index-two-mutant",
        "F2SQuotientBypassMutant.cfg",
        "counterexample",
        "NoBypassObserved",
        "trace-manifests/F2SQuotientBypassMutant.manifest.template.json",
    ),
)

SPEC_RE = re.compile(r"^SPECIFICATION\s+(\w+)\s*$", re.M)
CHECK_RE = re.compile(r"^(INVARIANT|PROPERTY)\s+(\w+)\s*$", re.M)
DEADLOCK_RE = re.compile(r"^CHECK_DEADLOCK\s+(TRUE|FALSE)\s*$", re.M)
REDUCTION_RE = re.compile(
    r"^(CONSTRAINT|ACTION_CONSTRAINT|SYMMETRY|VIEW)\b", re.M
)
OPERATOR_RE = re.compile(r"(?m)^([A-Za-z_][A-Za-z0-9_]*)\s*==")


class StaticError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise StaticError(message)


def load(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def check_manifest(manifest: Path, formal: Path) -> dict[str, Any]:
    document = load(manifest)
    require(isinstance(document, dict), "manifest must be an object")
    require(document.get("schema") == 1, "manifest schema must be 1")
    require(document.get("proofs") == [], "quotient matrix must have no proof rows")
    rows = document.get("checks")
    require(isinstance(rows, list), "checks must be an array")
    require(len(rows) == len(EXPECTED_ROWS), "exactly three rows required")

    module_path = formal / "F2SQuotientSimulation.tla"
    require(module_path.is_file(), f"missing {module_path}")
    module_text = module_path.read_text(encoding="utf-8")
    operators = set(OPERATOR_RE.findall(module_text))

    report_rows: list[dict[str, Any]] = []
    for row, expected in zip(rows, EXPECTED_ROWS, strict=True):
        check_id, config_name, verdict, property_name, trace_name = expected
        require(isinstance(row, dict), f"{check_id}: row must be an object")
        require(row.get("id") == check_id, f"row order/id mismatch: {row.get('id')}")
        require(row.get("module") == "F2SQuotientSimulation", f"{check_id}: module mismatch")
        require(row.get("config") == config_name, f"{check_id}: config mismatch")
        require(row.get("kind") == "safety", f"{check_id}: safety kind required")
        require(row.get("workers") == 1, f"{check_id}: one worker required")
        require(row.get("expected") == verdict, f"{check_id}: verdict mismatch")
        require(row.get("property") == property_name, f"{check_id}: property mismatch")
        require(row.get("toolchains") == ["stable", "differential"], f"{check_id}: toolchain order mismatch")
        require(property_name in operators, f"{check_id}: property operator absent")

        config_path = formal / config_name
        require(config_path.is_file(), f"{check_id}: missing config")
        config_text = config_path.read_text(encoding="utf-8")
        require(SPEC_RE.findall(config_text) == ["Spec"], f"{check_id}: exactly SPECIFICATION Spec required")
        checks = CHECK_RE.findall(config_text)
        require(("INVARIANT", property_name) in checks, f"{check_id}: named invariant not checked directly")
        if verdict == "counterexample":
            require(checks == [("INVARIANT", property_name)], f"{check_id}: expected counterexample must check only its named invariant")
        require(DEADLOCK_RE.findall(config_text) == ["FALSE"], f"{check_id}: CHECK_DEADLOCK FALSE required")
        require(REDUCTION_RE.search(config_text) is None, f"{check_id}: state-space reductions forbidden")

        if trace_name is None:
            require("trace_manifest" not in row, f"{check_id}: passing row cannot carry a trace manifest")
        else:
            require(row.get("trace_manifest") == trace_name, f"{check_id}: trace path mismatch")
            trace_path = formal / trace_name
            require(trace_path.is_file(), f"{check_id}: missing trace manifest")
            trace = load(trace_path)
            require(trace.get("schema") == 1, f"{check_id}: trace schema mismatch")
            require(trace.get("property") == property_name, f"{check_id}: trace property mismatch")
            require(trace.get("expected_result") == "counterexample", f"{check_id}: trace verdict mismatch")
            metadata = trace.get("metadata")
            require(isinstance(metadata, dict), f"{check_id}: trace metadata required")
            require(metadata.get("module") == row["module"], f"{check_id}: trace module mismatch")
            require(metadata.get("config") == config_name, f"{check_id}: trace config mismatch")
            events = trace.get("events")
            require(isinstance(events, list) and events, f"{check_id}: nonempty events required")
            names = [event.get("name") for event in events if isinstance(event, dict)]
            require(len(names) == len(events) and all(isinstance(name, str) and name for name in names), f"{check_id}: every event needs a name")
            required = trace.get("required_subsequence")
            require(isinstance(required, list) and required, f"{check_id}: required subsequence missing")
            require(set(required) <= set(names), f"{check_id}: subsequence references unknown events")
            final_all = trace.get("final_all")
            require(isinstance(final_all, list) and final_all, f"{check_id}: final predicates required")
            harness = trace.get("harness_steps")
            require(isinstance(harness, list) and harness, f"{check_id}: harness anchors required")

        report_rows.append(
            {
                "id": check_id,
                "config": config_name,
                "expected": verdict,
                "property": property_name,
            }
        )

    forbidden_model_fragments = (
        "VIEW ",
        "SYMMETRY ",
        "CONSTRAINT ",
        "ACTION_CONSTRAINT ",
    )
    for fragment in forbidden_model_fragments:
        require(fragment not in module_text, f"model contains forbidden reduction fragment {fragment!r}")

    return {
        "schema": 1,
        "status": "PASS",
        "manifest": str(manifest),
        "check_count": len(report_rows),
        "checks": report_rows,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--repo", default=".", type=Path)
    parser.add_argument("--formal-dir", default="formal", type=Path)
    args = parser.parse_args()
    repo = args.repo.resolve()
    formal = (repo / args.formal_dir).resolve()
    authoritative = formal / "f2s-quotient-checks-v1.json"
    manifest = args.manifest.resolve()
    try:
        require(manifest == authoritative, f"only {authoritative} is authoritative")
        report = check_manifest(manifest, formal)
    except (StaticError, OSError, json.JSONDecodeError) as error:
        print(f"F2S quotient static check failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
