#!/usr/bin/env python3
"""Fail-closed static contract for compatibility-formal-checks-v1.json.

This checker runs before either TLC jar. It validates the exact 31-row matrix,
local TLA+ dependency closure, complete constant instantiation, fixed topology
coverage, deliberate old-client restart limitations, one-premise mutants,
discriminating trace manifests, and the deterministic byte-level codec tests.
It does not claim a TLC or production-code compatibility result.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Any, Mapping, Sequence

sys.dont_write_bytecode = True

REQUIRED_MANIFEST = "compatibility-formal-checks-v1.json"
STANDARD_MODULES = {
    "Bags",
    "FiniteSets",
    "Functions",
    "Integers",
    "Json",
    "Naturals",
    "Randomization",
    "RealTime",
    "Reals",
    "Sequences",
    "TLC",
    "TLAPS",
    "Toolbox",
}
FIXED_IDS = [
    "compat-old-f-old-c",
    "compat-new-f-old-c",
    "compat-new-f-new-c",
    "compat-old-f-new-c",
    "compat-mixed-f-mixed-c",
    "compat-new-f-mixed-c",
]
WITNESS_IDS = [
    "compat-legacy-oldf-oldc-witness",
    "compat-fenced-newf-oldc-witness",
    "compat-token-newf-newc-witness",
    "compat-legacy-oldf-newc-witness",
    "compat-cancel-before-delivery-witness",
    "compat-fenced-revoke-witness",
    "compat-token-revoke-witness",
    "compat-legacy-restart-limitation-witness",
    "compat-fenced-restart-limitation-witness",
    "compat-token-restart-reject-witness",
    "compat-worker-loss-witness",
    "compat-submitter-loss-witness",
    "compat-detached-completion-witness",
    "compat-mixed-legacy-token-witness",
    "compat-mixed-fenced-token-witness",
    "compat-old-projection-completion-witness",
]
MUTANT_IDS = [
    "compat-prepare-old-worker-mutant",
    "compat-token-old-client-mutant",
    "compat-global-capability-mutant",
    "compat-late-claim-mutant",
    "compat-accept-stale-token-mutant",
    "compat-wait-ready-old-worker-mutant",
    "compat-partial-old-frame-mutant",
    "compat-new-terminal-old-peer-mutant",
    "compat-policy-after-dispatch-mutant",
]
EXPECTED_IDS = FIXED_IDS + WITNESS_IDS + MUTANT_IDS
EXPECTED_TOPOLOGIES = {
    "compat-old-f-old-c": "S'FC",
    "compat-new-f-old-c": "S'F'C",
    "compat-new-f-new-c": "S'F'C'",
    "compat-old-f-new-c": "S'FC'",
    "compat-mixed-f-mixed-c": "S'F[F']C[C']",
    "compat-new-f-mixed-c": "S'F'[CC']",
}
MUTANT_CONSTANTS = {
    "MutantPrepareOldWorker",
    "MutantTokenOldClient",
    "MutantGlobalCapability",
    "MutantDefaultAllowUnknown",
    "MutantAcceptStaleToken",
    "MutantWaitReadyOldWorker",
    "MutantPartialOldFrame",
    "MutantNewTerminalOldPeer",
    "MutantChoosePolicyAfterDispatch",
}
LIMITATION_IDS = {
    "compat-legacy-restart-limitation-witness",
    "compat-fenced-restart-limitation-witness",
}
IDENTIFIER_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
MODULE_HEADER_RE = re.compile(
    r"^-+\s*MODULE\s+([A-Za-z_][A-Za-z0-9_]*)\s*-+\s*$",
    re.MULTILINE,
)
EXTENDS_RE = re.compile(r"^\s*EXTENDS\s+([^\r\n]+)$", re.MULTILINE)
DIRECT_CHECK_RE = re.compile(
    r"^\s*(INVARIANT|PROPERTY)\s+([A-Za-z_][A-Za-z0-9_]*)\s*$",
    re.MULTILINE,
)
FORBIDDEN_CFG_RE = re.compile(
    r"^\s*(CONSTRAINT|ACTION_CONSTRAINT|SYMMETRY|VIEW)\b", re.MULTILINE
)
CONFIG_ASSIGNMENT_RE = re.compile(
    r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*(=|<-)\s*(.+?)\s*$",
    re.MULTILINE,
)


class CompatibilityStaticError(RuntimeError):
    """The compatibility formal input is not executable acceptance input."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CompatibilityStaticError(message)


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        raise CompatibilityStaticError(f"cannot read {path}: {exc}") from exc


def read_json(path: Path) -> Any:
    try:
        return json.loads(read_text(path))
    except json.JSONDecodeError as exc:
        raise CompatibilityStaticError(
            f"{path}: invalid JSON at line {exc.lineno}, column "
            f"{exc.colno}: {exc.msg}"
        ) from exc


def strip_tla_comments(text: str) -> str:
    output: list[str] = []
    index = 0
    depth = 0
    while index < len(text):
        if text.startswith("(*", index):
            depth += 1
            index += 2
            continue
        if depth and text.startswith("*)", index):
            depth -= 1
            index += 2
            continue
        if depth == 0:
            output.append(text[index])
        elif text[index] == "\n":
            output.append("\n")
        index += 1
    require(depth == 0, "unterminated TLA+ block comment")
    return "".join(output)


def local_extensions(module_path: Path) -> list[str]:
    text = read_text(module_path)
    header = MODULE_HEADER_RE.search(text)
    require(header is not None, f"{module_path}: missing TLA+ module header")
    require(
        header.group(1) == module_path.stem,
        f"{module_path}: module name {header.group(1)!r} mismatches filename",
    )
    names: list[str] = []
    for match in EXTENDS_RE.finditer(text):
        for raw in match.group(1).split(","):
            name = raw.strip()
            require(
                bool(IDENTIFIER_RE.fullmatch(name)),
                f"{module_path}: malformed EXTENDS entry {name!r}",
            )
            if name not in STANDARD_MODULES:
                names.append(name)
    return names


def module_closure(formal_dir: Path, root: str) -> list[Path]:
    pending = [root]
    seen: set[str] = set()
    ordered: list[Path] = []
    while pending:
        name = pending.pop()
        if name in seen:
            continue
        seen.add(name)
        path = formal_dir / f"{name}.tla"
        require(path.is_file(), f"missing local TLA+ module {path}")
        ordered.append(path)
        pending.extend(reversed(local_extensions(path)))
    return ordered


def declared_constants(module_path: Path) -> set[str]:
    text = strip_tla_comments(read_text(module_path))
    result: set[str] = set()
    lines = text.splitlines()
    index = 0
    stop_words = {
        "ASSUME",
        "VARIABLE",
        "VARIABLES",
        "RECURSIVE",
        "INSTANCE",
        "LOCAL",
        "THEOREM",
        "LEMMA",
        "COROLLARY",
    }
    while index < len(lines):
        match = re.match(r"^\s*CONSTANTS?\s+(.*)$", lines[index])
        if not match:
            index += 1
            continue
        fragments = [match.group(1)]
        index += 1
        while index < len(lines):
            stripped = lines[index].strip()
            if not stripped:
                break
            first = stripped.split(None, 1)[0]
            if first in stop_words or "==" in stripped:
                break
            fragments.append(stripped)
            index += 1
        declaration = " ".join(fragments)
        result.update(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", declaration))
    return result


def config_assignments(config_text: str) -> dict[str, tuple[str, str]]:
    in_constants = False
    result: dict[str, tuple[str, str]] = {}
    for line in config_text.splitlines():
        stripped = line.strip()
        if stripped == "CONSTANTS":
            in_constants = True
            continue
        if in_constants and (
            not stripped
            or re.match(
                r"^(INVARIANT|PROPERTY|CHECK_DEADLOCK|SPECIFICATION|INIT|NEXT)\b",
                stripped,
            )
        ):
            if stripped:
                in_constants = False
            else:
                continue
        if in_constants:
            match = re.match(
                r"^([A-Za-z_][A-Za-z0-9_]*)\s*(=|<-)\s*(.+?)\s*$",
                stripped,
            )
            require(match is not None, f"malformed CONSTANTS row: {line!r}")
            name, operator, value = match.groups()
            require(name not in result, f"duplicate constant assignment {name!r}")
            result[name] = (operator, value)
    return result


def validate_config(
    formal_dir: Path,
    check: Mapping[str, Any],
) -> tuple[list[Path], dict[str, tuple[str, str]]]:
    config_path = formal_dir / check["config"]
    require(config_path.is_file(), f"missing config {config_path}")
    text = read_text(config_path)
    specifications = re.findall(
        r"^\s*SPECIFICATION\s+([A-Za-z_][A-Za-z0-9_]*)\s*$",
        text,
        re.MULTILINE,
    )
    require(
        specifications == ["Spec"],
        f"{config_path}: exactly SPECIFICATION Spec is required",
    )
    deadlocks = re.findall(
        r"^\s*CHECK_DEADLOCK\s+(TRUE|FALSE)\s*$", text, re.MULTILINE
    )
    require(
        len(deadlocks) == 1,
        f"{config_path}: CHECK_DEADLOCK must appear exactly once",
    )
    forbidden = FORBIDDEN_CFG_RE.search(text)
    require(
        forbidden is None,
        f"{config_path}: {forbidden.group(1) if forbidden else ''} is forbidden",
    )
    direct = [(kind, name) for kind, name in DIRECT_CHECK_RE.findall(text)]
    require(
        direct == [("INVARIANT", check["property"])],
        f"{config_path}: must check exactly INVARIANT {check['property']}; "
        f"found {direct}",
    )

    closure = module_closure(formal_dir, check["module"])
    required_constants: set[str] = set()
    for module_path in closure:
        required_constants.update(declared_constants(module_path))
    assignments = config_assignments(text)
    supplied = set(assignments)
    require(
        not required_constants - supplied,
        f"{config_path}: missing constants {sorted(required_constants - supplied)}",
    )
    require(
        not supplied - required_constants,
        f"{config_path}: unknown constants {sorted(supplied - required_constants)}",
    )
    return closure, assignments


def validate_condition(condition: Any, context: str) -> None:
    require(isinstance(condition, dict), f"{context}: condition must be an object")
    require(
        isinstance(condition.get("path"), str) and bool(condition["path"]),
        f"{context}: nonempty path is required",
    )
    require(len(condition) > 1, f"{context}: condition is unconstrained")


def validate_trace_manifest(
    formal_dir: Path,
    check: Mapping[str, Any],
) -> None:
    relative = check.get("trace_manifest")
    require(
        isinstance(relative, str) and bool(relative),
        f"{check['id']}: counterexample row needs trace_manifest",
    )
    path = formal_dir / relative
    require(path.is_file(), f"{check['id']}: missing trace manifest {path}")
    document = read_json(path)
    require(isinstance(document, dict), f"{path}: root must be an object")
    require(document.get("schema") == 1, f"{path}: schema must be 1")
    require(
        document.get("property") == check["property"],
        f"{path}: property does not match {check['property']!r}",
    )
    require(
        document.get("expected_result", "counterexample") == "counterexample",
        f"{path}: expected_result must be counterexample",
    )
    events = document.get("events")
    require(isinstance(events, list) and bool(events), f"{path}: events must be nonempty")
    names: list[str] = []
    for event_index, event in enumerate(events):
        require(isinstance(event, dict), f"{path}: event {event_index} is not an object")
        name = event.get("name")
        require(isinstance(name, str) and bool(name), f"{path}: event has no name")
        names.append(name)
        groups = [event.get(key, []) for key in ("all", "any", "none")]
        require(
            any(isinstance(group, list) and bool(group) for group in groups),
            f"{path}: event {name!r} has no conditions",
        )
        for group_name, group in zip(("all", "any", "none"), groups):
            require(isinstance(group, list), f"{path}: {group_name} must be a list")
            for condition_index, condition in enumerate(group):
                validate_condition(
                    condition,
                    f"{path}: {name}.{group_name}[{condition_index}]",
                )
    require(len(names) == len(set(names)), f"{path}: duplicate event names")
    required = document.get("required_subsequence")
    require(
        isinstance(required, list) and bool(required),
        f"{path}: required_subsequence must be nonempty",
    )
    require(
        all(isinstance(name, str) and name in names for name in required),
        f"{path}: required_subsequence references an unknown event",
    )
    finals = document.get("final_all")
    require(isinstance(finals, list) and bool(finals), f"{path}: final_all must be nonempty")
    for index, condition in enumerate(finals):
        validate_condition(condition, f"{path}: final_all[{index}]")
    steps = document.get("harness_steps")
    require(isinstance(steps, list) and bool(steps), f"{path}: harness_steps must be nonempty")
    for index, step in enumerate(steps):
        require(isinstance(step, dict), f"{path}: harness step {index} is not an object")
        require(step.get("after") in names, f"{path}: harness step has unknown event")
        require(
            isinstance(step.get("emit"), str) and bool(step["emit"]),
            f"{path}: harness step {index} has no emitted barrier",
        )
        occurrence = step.get("occurrence", 1)
        require(
            isinstance(occurrence, int) and occurrence >= 1,
            f"{path}: harness step occurrence must be >= 1",
        )
    metadata = document.get("metadata")
    require(isinstance(metadata, dict), f"{path}: metadata must be an object")
    require(
        metadata.get("module") == check["module"],
        f"{path}: metadata module does not match {check['module']!r}",
    )
    configs = metadata.get("configs")
    require(
        isinstance(configs, list)
        and all(isinstance(item, str) for item in configs)
        and check["config"] in configs,
        f"{path}: metadata configs do not include {check['config']!r}",
    )


def true_mutants(assignments: Mapping[str, tuple[str, str]]) -> set[str]:
    result: set[str] = set()
    for name in MUTANT_CONSTANTS:
        require(name in assignments, f"config omitted mutant switch {name}")
        operator, value = assignments[name]
        require(operator == "=", f"mutant switch {name} must use '='")
        require(value in {"TRUE", "FALSE"}, f"mutant switch {name} is not boolean")
        if value == "TRUE":
            result.add(name)
    return result


def validate_check(
    formal_dir: Path,
    check: Any,
    *,
    fixed: bool,
    witness: bool,
    mutant: bool,
) -> dict[str, Any]:
    require(isinstance(check, dict), "matrix row is not an object")
    for key in ("id", "module", "config", "kind", "expected", "property"):
        require(
            isinstance(check.get(key), str) and bool(check[key]),
            f"matrix row has no nonempty {key!r}",
        )
    require(check["module"] == "MixedVersionCompatibilityChecked", f"{check['id']}: wrong module")
    require(check["kind"] == "safety", f"{check['id']}: compatibility rows are safety checks")
    require(check.get("workers") == 1, f"{check['id']}: workers must be one")
    require(
        check.get("toolchains") == ["stable", "differential"],
        f"{check['id']}: toolchain order must be stable then differential",
    )
    require(
        isinstance(check.get("timeout_seconds"), int)
        and check["timeout_seconds"] > 0,
        f"{check['id']}: positive timeout is required",
    )
    finite = check.get("finite_state")
    require(isinstance(finite, dict), f"{check['id']}: finite_state is required")
    require(
        isinstance(finite.get("argument"), str)
        and len(finite["argument"].strip()) >= 24,
        f"{check['id']}: finite_state.argument is missing or vacuous",
    )

    closure, assignments = validate_config(formal_dir, check)
    enabled = true_mutants(assignments)
    if fixed:
        require(check["expected"] == "pass", f"{check['id']}: fixed row must pass")
        require(
            check["property"] == "CompatibilitySafetyInvariant",
            f"{check['id']}: fixed row must check CompatibilitySafetyInvariant",
        )
        require(not enabled, f"{check['id']}: fixed row enables mutants {sorted(enabled)}")
        require(
            check.get("topology") == EXPECTED_TOPOLOGIES[check["id"]],
            f"{check['id']}: topology label mismatch",
        )
        require("trace_manifest" not in check, f"{check['id']}: passing row has trace manifest")
    elif witness:
        require(check["expected"] == "counterexample", f"{check['id']}: witness must be cex")
        require(not enabled, f"{check['id']}: fixed witness enables mutants {sorted(enabled)}")
        assumptions = check.get("assumptions")
        require(
            isinstance(assumptions, list)
            and bool(assumptions)
            and all(isinstance(item, str) and item.strip() for item in assumptions),
            f"{check['id']}: witness assumption/rationale is required",
        )
        if check["id"] in LIMITATION_IDS:
            joined = " ".join(assumptions).lower()
            require(
                "limitation" in joined or "not exact" in joined,
                f"{check['id']}: old-client restart limitation must be explicit",
            )
        validate_trace_manifest(formal_dir, check)
    elif mutant:
        require(check["expected"] == "counterexample", f"{check['id']}: mutant must be cex")
        require(
            len(enabled) == 1,
            f"{check['id']}: exactly one mutant switch must be TRUE, got {sorted(enabled)}",
        )
        validate_trace_manifest(formal_dir, check)
    else:
        raise CompatibilityStaticError(f"unclassified row {check['id']}")

    return {
        "id": check["id"],
        "config": check["config"],
        "property": check["property"],
        "expected": check["expected"],
        "enabled_mutants": sorted(enabled),
        "module_closure": [path.name for path in closure],
    }


def run_codec_tests(formal_dir: Path) -> dict[str, Any]:
    script = formal_dir / "compatibility_codec_fixture_test.py"
    require(script.is_file(), f"missing codec test {script}")
    environment = dict(os.environ)
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    completed = subprocess.run(
        [sys.executable, str(script)],
        cwd=str(formal_dir),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
        timeout=120,
        env=environment,
    )
    require(
        completed.returncode == 0,
        f"codec fixture tests failed with {completed.returncode}:\n{completed.stdout}",
    )
    require(
        "OK" in completed.stdout,
        f"codec test output lacks unittest success marker:\n{completed.stdout}",
    )
    return {
        "script": script.name,
        "returncode": completed.returncode,
        "output": completed.stdout,
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--formal-dir", type=Path, required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        repo = args.repo.resolve()
        formal_dir = args.formal_dir.resolve()
        manifest_path = args.manifest.resolve()
        require(repo.is_dir(), f"repository directory is absent: {repo}")
        require(formal_dir == repo / "formal", "formal-dir must be <repo>/formal")
        require(
            manifest_path == formal_dir / REQUIRED_MANIFEST,
            f"only {formal_dir / REQUIRED_MANIFEST} is authoritative",
        )
        document = read_json(manifest_path)
        require(isinstance(document, dict), "matrix root must be an object")
        require(document.get("schema") == 1, "matrix schema must be 1")
        require(document.get("proofs") == [], "compatibility matrix proofs must be []")
        checks = document.get("checks")
        require(isinstance(checks, list), "matrix checks must be an array")
        ids = [check.get("id") if isinstance(check, dict) else None for check in checks]
        require(ids == EXPECTED_IDS, f"matrix IDs/order differ from contract: {ids}")
        require(len(ids) == len(set(ids)) == 31, "matrix must have 31 unique rows")
        require(len(FIXED_IDS) == 6 and len(WITNESS_IDS) == 16 and len(MUTANT_IDS) == 9, "internal group contract is wrong")

        results: list[dict[str, Any]] = []
        for check in checks:
            check_id = check["id"]
            results.append(
                validate_check(
                    formal_dir,
                    check,
                    fixed=check_id in FIXED_IDS,
                    witness=check_id in WITNESS_IDS,
                    mutant=check_id in MUTANT_IDS,
                )
            )
        codec_result = run_codec_tests(formal_dir)
        print(
            json.dumps(
                {
                    "schema": 1,
                    "status": "PASS",
                    "manifest": str(manifest_path),
                    "check_count": len(results),
                    "groups": {"fixed": 6, "witness": 16, "mutant": 9},
                    "codec_fixture": codec_result,
                    "checks": results,
                },
                indent=2,
                sort_keys=True,
            )
        )
        return 0
    except (CompatibilityStaticError, subprocess.TimeoutExpired) as exc:
        print(f"compatibility static check failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
