#!/usr/bin/env python3
"""Fail-closed static contract for mode-refinement-checks-v1.json."""

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
        "mode-refinement-strict-fixed",
        "ModeRefinementStrict.cfg",
        "safety",
        "pass",
        "RefinementSafety",
        None,
        "StrictEnforcing",
        None,
    ),
    (
        "mode-refinement-pipelined-fixed",
        "ModeRefinementPipelined.cfg",
        "safety",
        "pass",
        "RefinementSafety",
        None,
        "PipelinedEnforcing",
        None,
    ),
    (
        "mode-refinement-pending-stutter-witness",
        "ModeRefinementPendingStutterWitness.cfg",
        "safety",
        "counterexample",
        "NoPendingStutterSeen",
        "trace-manifests/ModeRefinementPendingStutterWitness.manifest.template.json",
        "PipelinedEnforcing",
        None,
    ),
    (
        "mode-refinement-revoke-witness",
        "ModeRefinementRevokeWitness.cfg",
        "safety",
        "counterexample",
        "NoRevocationSeen",
        "trace-manifests/ModeRefinementRevokeWitness.manifest.template.json",
        "PipelinedEnforcing",
        None,
    ),
    (
        "mode-refinement-pending-authorizes-mutant",
        "ModeRefinementPendingAuthorizesMutant.cfg",
        "safety",
        "counterexample",
        "PendingDoesNotAuthorize",
        "trace-manifests/ModeRefinementPendingAuthorizesMutant.manifest.template.json",
        "PipelinedEnforcing",
        "MutantPendingAuthorizes",
    ),
    (
        "mode-refinement-start-before-grant-mutant",
        "ModeRefinementStartBeforeGrantMutant.cfg",
        "safety",
        "counterexample",
        "StartRequiresAuthorization",
        "trace-manifests/ModeRefinementStartBeforeGrantMutant.manifest.template.json",
        "PipelinedEnforcing",
        "MutantStartBeforeGrant",
    ),
    (
        "mode-refinement-drop-revoke-mutant",
        "ModeRefinementDropRevokeMutant.cfg",
        "safety",
        "counterexample",
        "AbstractionRelation",
        "trace-manifests/ModeRefinementDropRevokeMutant.manifest.template.json",
        "PipelinedEnforcing",
        "MutantDropRevoke",
    ),
    (
        "mode-refinement-unbounded-pending-mutant",
        "ModeRefinementUnboundedPendingMutant.cfg",
        "safety",
        "counterexample",
        "PendingBound",
        "trace-manifests/ModeRefinementUnboundedPendingMutant.manifest.template.json",
        "PipelinedEnforcing",
        "MutantUnboundedPending",
    ),
    (
        "mode-refinement-pending-liveness",
        "ModeRefinementPendingLiveness.cfg",
        "liveness",
        "pass",
        "PendingEventuallyResolved",
        None,
        "PipelinedEnforcing",
        None,
    ),
)

MUTANTS = (
    "MutantPendingAuthorizes",
    "MutantStartBeforeGrant",
    "MutantDropRevoke",
    "MutantUnboundedPending",
)

IDENTIFIER = r"[A-Za-z_][A-Za-z0-9_]*"
OPERATOR_RE = re.compile(
    rf"(?m)^\s*({IDENTIFIER})(?:\([^=\n]*\))?\s*=="
)
SPEC_RE = re.compile(rf"(?m)^\s*SPECIFICATION\s+({IDENTIFIER})\s*$")
CHECK_RE = re.compile(
    rf"(?m)^\s*(INVARIANT|PROPERTY)\s+({IDENTIFIER})\s*$"
)
DEADLOCK_RE = re.compile(r"(?m)^\s*CHECK_DEADLOCK\s+(TRUE|FALSE)\s*$")
ASSIGNMENT_RE = re.compile(
    rf"(?m)^\s*({IDENTIFIER})\s*=\s*([^\n#]+?)\s*$"
)
REDUCTION_RE = re.compile(
    r"(?m)^\s*(CONSTRAINT|ACTION_CONSTRAINT|SYMMETRY|VIEW)\b"
)


class StaticError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise StaticError(message)


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def constant_names(text: str) -> set[str]:
    lines = text.splitlines()
    result: set[str] = set()
    index = 0
    stop = re.compile(
        r"^(ASSUME|VARIABLES?|EXTENDS|LOCAL|THEOREM|LEMMA|INSTANCE)\b"
    )
    while index < len(lines):
        match = re.match(r"^\s*CONSTANTS?\b(.*)$", lines[index])
        if match is None:
            index += 1
            continue
        fragments = [match.group(1)]
        cursor = index + 1
        while cursor < len(lines):
            stripped = lines[cursor].strip()
            if not stripped or stop.match(stripped):
                break
            if "==" in stripped or stripped.startswith(("/\\", "\\/")):
                break
            fragments.append(stripped)
            cursor += 1
        result.update(re.findall(IDENTIFIER, " ".join(fragments)))
        index = max(index + 1, cursor)
    return result


def config_assignments(text: str) -> dict[str, str]:
    return {
        name: value.strip()
        for name, value in ASSIGNMENT_RE.findall(text)
    }


def validate_trace(
    formal: Path,
    row: dict[str, Any],
    relative: str,
) -> dict[str, Any]:
    context = str(row["id"])
    path = formal / relative
    require(path.is_file(), f"{context}: missing trace {path}")
    trace = load_json(path)
    require(isinstance(trace, dict), f"{context}: trace must be an object")
    require(trace.get("schema") == 1, f"{context}: trace schema mismatch")
    require(
        trace.get("expected_result") == "counterexample",
        f"{context}: trace verdict mismatch",
    )
    require(trace.get("property") == row["property"], f"{context}: trace property mismatch")
    minimum = trace.get("min_states")
    require(
        isinstance(minimum, int)
        and not isinstance(minimum, bool)
        and minimum >= 2,
        f"{context}: min_states must be at least 2",
    )
    events = trace.get("events")
    require(isinstance(events, list) and events, f"{context}: nonempty events required")
    names: list[str] = []
    for event in events:
        require(isinstance(event, dict), f"{context}: event must be an object")
        name = event.get("name")
        require(isinstance(name, str) and name, f"{context}: event name required")
        predicates = event.get("all")
        require(
            isinstance(predicates, list) and predicates,
            f"{context}:{name}: nonempty predicates required",
        )
        for predicate in predicates:
            require(
                isinstance(predicate, dict)
                and isinstance(predicate.get("path"), str)
                and predicate["path"],
                f"{context}:{name}: every predicate needs a path",
            )
        names.append(name)
    require(len(names) == len(set(names)), f"{context}: duplicate event names")

    required = trace.get("required_subsequence")
    require(
        isinstance(required, list) and required,
        f"{context}: required_subsequence missing",
    )
    require(set(required) <= set(names), f"{context}: subsequence references unknown event")
    final_all = trace.get("final_all")
    require(
        isinstance(final_all, list) and final_all,
        f"{context}: nonempty final_all required",
    )
    harness = trace.get("harness_steps")
    require(
        isinstance(harness, list) and harness,
        f"{context}: nonempty harness_steps required",
    )
    for step in harness:
        require(isinstance(step, dict), f"{context}: harness step must be an object")
        require(step.get("after") in names, f"{context}: harness references unknown event")
        require(
            isinstance(step.get("emit"), str) and step["emit"],
            f"{context}: harness emit required",
        )
        require(
            isinstance(step.get("actor"), str) and step["actor"],
            f"{context}: harness actor required",
        )

    metadata = trace.get("metadata")
    require(isinstance(metadata, dict), f"{context}: metadata required")
    require(metadata.get("module") == row["module"], f"{context}: trace module mismatch")
    require(metadata.get("config") == row["config"], f"{context}: trace config mismatch")
    return {
        "scenario": trace.get("scenario"),
        "event_count": len(events),
        "harness_count": len(harness),
    }


def check_document(document: Any, formal: Path) -> dict[str, Any]:
    require(isinstance(document, dict), "manifest must be an object")
    require(document.get("schema") == 1, "manifest schema must be 1")
    require(document.get("proofs") == [], "refinement matrix must have no proof rows")
    rows = document.get("checks")
    require(isinstance(rows, list), "checks must be an array")
    require(len(rows) == len(EXPECTED_ROWS), "exactly nine rows required")

    module_path = formal / "AssignmentFenceModeRefinement.tla"
    require(module_path.is_file(), f"missing {module_path}")
    module_text = module_path.read_text(encoding="utf-8")
    operators = set(OPERATOR_RE.findall(module_text))
    constants = constant_names(module_text)
    expected_constants = {
        "StrictMode",
        "PipelinedMode",
        "Mode",
        "MaxPending",
        *MUTANTS,
    }
    require(constants == expected_constants, f"model constants drifted: {sorted(constants)}")

    for marker in (
        "AbsOf ==",
        "AbstractionRelation == abstractState = AbsOf",
        "StartRequiresAuthorization == started => authorizationSeen",
        "PendingDoesNotAuthorize ==",
        "PendingBound == pendingCount <= MaxPending",
        "PendingEventuallyResolved ==",
        "FairSpec == Spec /\\ WF_vars(ConsumePrepare)",
        "CapInc(n) == IF n < MaxPending THEN n + 1 ELSE n",
        "rejectedClaims \\in 0..MaxPending",
    ):
        require(marker in module_text, f"model marker absent: {marker}")
    for forbidden in (
        "SF_vars(",
        "rejectedClaims + 1",
        "rejectedClaims \\in Nat",
        "VIEW ",
        "SYMMETRY ",
        "CONSTRAINT ",
        "ACTION_CONSTRAINT ",
    ):
        require(forbidden not in module_text, f"forbidden model fragment: {forbidden}")

    report_rows: list[dict[str, Any]] = []
    for row, expected_row in zip(rows, EXPECTED_ROWS, strict=True):
        (
            check_id,
            config_name,
            kind,
            verdict,
            property_name,
            trace_name,
            mode,
            enabled_mutant,
        ) = expected_row
        require(isinstance(row, dict), f"{check_id}: row must be an object")
        require(row.get("id") == check_id, f"row order/id mismatch: {row.get('id')}")
        require(
            row.get("module") == "AssignmentFenceModeRefinement",
            f"{check_id}: module mismatch",
        )
        require(row.get("config") == config_name, f"{check_id}: config mismatch")
        require(row.get("kind") == kind, f"{check_id}: kind mismatch")
        require(row.get("expected") == verdict, f"{check_id}: verdict mismatch")
        require(row.get("property") == property_name, f"{check_id}: property mismatch")
        require(row.get("workers") == 1, f"{check_id}: one worker required")
        require(
            row.get("toolchains") == ["stable", "differential"],
            f"{check_id}: stable/differential order required",
        )
        require(property_name in operators, f"{check_id}: property operator absent")
        timeout = row.get("timeout_seconds")
        require(
            isinstance(timeout, int)
            and not isinstance(timeout, bool)
            and timeout > 0,
            f"{check_id}: positive timeout required",
        )

        config_path = formal / config_name
        require(config_path.is_file(), f"{check_id}: missing config")
        config_text = config_path.read_text(encoding="utf-8")
        require(REDUCTION_RE.search(config_text) is None, f"{check_id}: reductions forbidden")
        expected_spec = "FairSpec" if kind == "liveness" else "Spec"
        require(SPEC_RE.findall(config_text) == [expected_spec], f"{check_id}: specification mismatch")
        expected_direct = "PROPERTY" if kind == "liveness" else "INVARIANT"
        direct = CHECK_RE.findall(config_text)
        require(
            (expected_direct, property_name) in direct,
            f"{check_id}: property not checked directly",
        )
        if verdict == "counterexample":
            require(
                direct == [(expected_direct, property_name)],
                f"{check_id}: counterexample must check only named property",
            )
        require(DEADLOCK_RE.findall(config_text) == ["FALSE"], f"{check_id}: CHECK_DEADLOCK FALSE required")

        assigned = config_assignments(config_text)
        require(set(assigned) == constants, f"{check_id}: constant assignment set mismatch")
        require(assigned["StrictMode"] == '"StrictEnforcing"', f"{check_id}: StrictMode drifted")
        require(assigned["PipelinedMode"] == '"PipelinedEnforcing"', f"{check_id}: PipelinedMode drifted")
        require(assigned["Mode"] == f'"{mode}"', f"{check_id}: Mode mismatch")
        require(assigned["MaxPending"] == "2", f"{check_id}: MaxPending must be 2")
        for mutant in MUTANTS:
            expected_value = "TRUE" if mutant == enabled_mutant else "FALSE"
            require(
                assigned[mutant] == expected_value,
                f"{check_id}: {mutant} must be {expected_value}",
            )

        trace_report = None
        if trace_name is None:
            require("trace_manifest" not in row, f"{check_id}: unexpected trace")
        else:
            require(row.get("trace_manifest") == trace_name, f"{check_id}: trace path mismatch")
            trace_report = validate_trace(formal, row, trace_name)

        if kind == "liveness":
            assumptions = row.get("assumptions")
            require(isinstance(assumptions, list) and assumptions, f"{check_id}: fairness assumption required")
            joined = " ".join(assumptions).lower()
            require("weak fairness" in joined, f"{check_id}: weak fairness must be stated")
            require("consumeprepare" in joined, f"{check_id}: fair action must be named")

        report_rows.append(
            {
                "id": check_id,
                "config": config_name,
                "kind": kind,
                "expected": verdict,
                "property": property_name,
                "mode": mode,
                "enabled_mutant": enabled_mutant,
                "trace": trace_report,
            }
        )

    return {
        "schema": 1,
        "status": "PASS",
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
    authoritative = formal / "mode-refinement-checks-v1.json"
    manifest = args.manifest.resolve()
    try:
        require(manifest == authoritative, f"only {authoritative} is authoritative")
        report = check_document(load_json(manifest), formal)
    except (StaticError, OSError, json.JSONDecodeError) as error:
        print(f"mode refinement static check failed: {error}", file=sys.stderr)
        return 1
    report["manifest"] = str(manifest)
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
