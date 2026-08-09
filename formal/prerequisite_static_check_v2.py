#!/usr/bin/env python3
"""Fail-closed static contract for prerequisite-formal-checks-v4.json.

This checker runs before either TLC jar. It validates the exact 24-row matrix,
local TLA+ dependency closure, complete constant instantiation, direct property
selection, finite-state arguments, and every expected-counterexample manifest.
It does not prove the models; it prevents malformed or vacuous execution input
from being mistaken for a model-checking result.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence

sys.dont_write_bytecode = True

REQUIRED_MANIFEST = "prerequisite-formal-checks-v4.json"
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
EXPECTED_IDS = [
    "allocator-fixed",
    "allocator-cross-namespace-mutant",
    "allocator-duplicate-begin-orphan-mutant",
    "allocator-disconnect-leak-mutant",
    "allocator-partial-batch-mutant",
    "lifecycle-authority-fixed",
    "lifecycle-detached-false-terminal-mutant",
    "lifecycle-duplicate-begin-mutant",
    "lifecycle-detached-identity-mutant",
    "lifecycle-started-termination-liveness",
    "lifecycle-started-reachability-witness",
    "usecs-handoff-fixed-all-cuts",
    "usecs-cut-zero-witness",
    "usecs-cut-header-witness",
    "usecs-cut-body-witness",
    "usecs-cut-final-short-witness",
    "usecs-local-alias-mutant",
    "usecs-abort-drop-mutant",
    "fsession-quiescence-fixed",
    "fsession-wrong-child-reap-mutant",
    "fsession-echild-live-group-mutant",
    "fsession-per-child-deadline-mutant",
    "fsession-residue-reset-mutant",
    "fsession-early-advertise-mutant",
]
GROUP_PREFIX_COUNTS = {
    "allocator-": 5,
    "lifecycle-": 6,
    "usecs-": 7,
    "fsession-": 6,
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


class StaticContractError(RuntimeError):
    """The prerequisite formal input is not executable acceptance input."""


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        raise StaticContractError(f"cannot read {path}: {exc}") from exc


def read_json(path: Path) -> Any:
    try:
        return json.loads(read_text(path))
    except json.JSONDecodeError as exc:
        raise StaticContractError(
            f"{path}: invalid JSON at line {exc.lineno}, column "
            f"{exc.colno}: {exc.msg}"
        ) from exc


def require(condition: bool, message: str) -> None:
    if not condition:
        raise StaticContractError(message)


def local_extensions(module_path: Path) -> list[str]:
    text = read_text(module_path)
    header = MODULE_HEADER_RE.search(text)
    require(header is not None, f"{module_path}: missing TLA+ module header")
    require(
        header.group(1) == module_path.stem,
        f"{module_path}: module header {header.group(1)!r} does not match filename",
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


def strip_tla_comments(text: str) -> str:
    # Nested TLA+ comments are uncommon in constant declarations. This scanner
    # nevertheless tracks nesting so an identifier in prose is never treated
    # as a model constant.
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
            line = lines[index]
            stripped = line.strip()
            if not stripped:
                break
            first = stripped.split(None, 1)[0] if stripped else ""
            if first in stop_words or "==" in line:
                break
            if re.match(r"^[A-Za-z_][A-Za-z0-9_]*\s*==", stripped):
                break
            fragments.append(stripped)
            index += 1
        declaration = " ".join(fragments)
        for token in re.findall(r"[A-Za-z_][A-Za-z0-9_]*", declaration):
            if token not in {"CONSTANT", "CONSTANTS"}:
                result.add(token)
    return result


def configured_constants(config_text: str) -> set[str]:
    result: set[str] = set()
    in_constants = False
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
                r"^([A-Za-z_][A-Za-z0-9_]*)\s*(?:=|<-)\s*", stripped
            )
            require(match is not None, f"malformed CONSTANTS row: {line!r}")
            result.add(match.group(1))
    return result


def validate_config(
    *,
    formal_dir: Path,
    module: str,
    config: str,
    expected: str,
    property_name: str,
) -> list[Path]:
    config_path = formal_dir / config
    require(config_path.is_file(), f"missing TLC config {config_path}")
    require(config_path.suffix == ".cfg", f"config is not .cfg: {config_path}")
    text = read_text(config_path)
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
        f"{config_path}: {forbidden.group(1) if forbidden else ''} is forbidden "
        "in the prerequisite acceptance matrix",
    )
    specification = re.findall(
        r"^\s*SPECIFICATION\s+([A-Za-z_][A-Za-z0-9_]*)\s*$",
        text,
        re.MULTILINE,
    )
    require(
        len(specification) == 1,
        f"{config_path}: exactly one SPECIFICATION is required",
    )
    direct = [(kind, name) for kind, name in DIRECT_CHECK_RE.findall(text)]
    direct_names = [name for _, name in direct]
    require(
        property_name in direct_names,
        f"{config_path}: manifest property {property_name!r} is not checked directly; "
        f"found {direct_names}",
    )
    if expected == "counterexample":
        require(
            direct_names == [property_name],
            f"{config_path}: expected counterexample must check exactly "
            f"{property_name!r}, found {direct_names}",
        )

    closure = module_closure(formal_dir, module)
    required_constants: set[str] = set()
    for module_path in closure:
        required_constants.update(declared_constants(module_path))
    supplied = configured_constants(text)
    missing = sorted(required_constants - supplied)
    extra = sorted(supplied - required_constants)
    require(
        not missing,
        f"{config_path}: constants missing for local module closure: {missing}",
    )
    require(
        not extra,
        f"{config_path}: config supplies undeclared constants: {extra}",
    )
    return closure


def validate_condition(condition: Any, context: str) -> None:
    require(isinstance(condition, dict), f"{context}: condition must be an object")
    require(
        isinstance(condition.get("path"), str) and bool(condition["path"]),
        f"{context}: condition requires a nonempty path",
    )
    require(
        len(condition) > 1,
        f"{context}: condition must constrain more than its path",
    )


def validate_trace_manifest(
    *,
    formal_dir: Path,
    relative: str,
    check: Mapping[str, Any],
) -> None:
    path = formal_dir / relative
    require(path.is_file(), f"{check['id']}: missing trace manifest {path}")
    document = read_json(path)
    require(isinstance(document, dict), f"{path}: manifest root must be an object")
    require(document.get("schema") == 1, f"{path}: schema must be 1")
    require(
        document.get("property") == check["property"],
        f"{path}: property does not match matrix row {check['property']!r}",
    )
    require(
        document.get("expected_result", "counterexample") == "counterexample",
        f"{path}: expected_result must be counterexample",
    )
    events = document.get("events")
    require(isinstance(events, list) and bool(events), f"{path}: events must be nonempty")
    names: list[str] = []
    for index, event in enumerate(events):
        require(isinstance(event, dict), f"{path}: event {index} is not an object")
        name = event.get("name")
        require(isinstance(name, str) and bool(name), f"{path}: event {index} has no name")
        names.append(name)
        groups = [event.get(key, []) for key in ("all", "any", "none")]
        require(
            any(isinstance(group, list) and bool(group) for group in groups),
            f"{path}: event {name!r} has no discriminating conditions",
        )
        for key, group in zip(("all", "any", "none"), groups):
            require(isinstance(group, list), f"{path}: event {name!r} {key} is not a list")
            for condition_index, condition in enumerate(group):
                validate_condition(
                    condition,
                    f"{path}: event {name!r} {key}[{condition_index}]",
                )
    require(len(names) == len(set(names)), f"{path}: event names are not unique")

    required = document.get("required_subsequence")
    require(
        isinstance(required, list) and bool(required),
        f"{path}: required_subsequence must be nonempty",
    )
    require(
        all(isinstance(name, str) and name in names for name in required),
        f"{path}: required_subsequence references an unknown event",
    )
    final_all = document.get("final_all")
    require(
        isinstance(final_all, list) and bool(final_all),
        f"{path}: final_all must be nonempty",
    )
    for index, condition in enumerate(final_all):
        validate_condition(condition, f"{path}: final_all[{index}]")

    steps = document.get("harness_steps")
    require(
        isinstance(steps, list) and bool(steps),
        f"{path}: harness_steps must be nonempty",
    )
    for index, step in enumerate(steps):
        require(isinstance(step, dict), f"{path}: harness step {index} is not an object")
        require(
            step.get("after") in names,
            f"{path}: harness step {index} references unknown event {step.get('after')!r}",
        )
        require(
            isinstance(step.get("emit"), str) and bool(step["emit"]),
            f"{path}: harness step {index} has no emitted barrier",
        )
        occurrence = step.get("occurrence", 1)
        require(
            isinstance(occurrence, int) and occurrence >= 1,
            f"{path}: harness step {index} occurrence must be >= 1",
        )
        require(
            required.count(step["after"]) >= occurrence
            or names.count(step["after"]) >= 1,
            f"{path}: harness occurrence is not represented by the required trace",
        )

    metadata = document.get("metadata")
    require(isinstance(metadata, dict), f"{path}: metadata must be an object")
    require(
        metadata.get("module") == check["module"],
        f"{path}: metadata module does not match {check['module']!r}",
    )
    require(
        metadata.get("config") == check["config"],
        f"{path}: metadata config does not match {check['config']!r}",
    )


def validate_check(
    *,
    formal_dir: Path,
    check: Any,
    seen_configs: set[str],
) -> dict[str, Any]:
    require(isinstance(check, dict), "matrix check is not an object")
    required_strings = ("id", "module", "config", "kind", "expected", "property")
    for key in required_strings:
        require(
            isinstance(check.get(key), str) and bool(check[key]),
            f"matrix check has no nonempty {key!r}",
        )
    require(check["config"] not in seen_configs, f"duplicate config row: {check['config']}")
    seen_configs.add(check["config"])
    require(check.get("workers") == 1, f"{check['id']}: workers must be exactly 1")
    require(
        check.get("toolchains") == ["stable", "differential"],
        f"{check['id']}: toolchains must be stable then differential",
    )
    require(check["kind"] in {"safety", "liveness"}, f"{check['id']}: invalid kind")
    require(
        check["expected"] in {"pass", "counterexample"},
        f"{check['id']}: invalid expected result",
    )
    require(
        isinstance(check.get("timeout_seconds"), int)
        and check["timeout_seconds"] > 0,
        f"{check['id']}: positive timeout_seconds is required",
    )
    finite = check.get("finite_state")
    require(isinstance(finite, dict), f"{check['id']}: finite_state object is required")
    require(
        isinstance(finite.get("argument"), str)
        and len(finite["argument"].strip()) >= 24,
        f"{check['id']}: finite_state.argument is missing or vacuous",
    )
    if check["kind"] == "liveness":
        assumptions = check.get("assumptions")
        require(
            isinstance(assumptions, list)
            and bool(assumptions)
            and all(isinstance(item, str) and item.strip() for item in assumptions),
            f"{check['id']}: liveness assumptions must be explicit and nonempty",
        )

    closure = validate_config(
        formal_dir=formal_dir,
        module=check["module"],
        config=check["config"],
        expected=check["expected"],
        property_name=check["property"],
    )
    if check["expected"] == "counterexample":
        relative = check.get("trace_manifest")
        require(
            isinstance(relative, str) and bool(relative),
            f"{check['id']}: counterexample row needs trace_manifest",
        )
        validate_trace_manifest(formal_dir=formal_dir, relative=relative, check=check)
    else:
        require(
            "trace_manifest" not in check,
            f"{check['id']}: passing row must not carry a counterexample manifest",
        )
    return {
        "id": check["id"],
        "module_closure": [path.name for path in closure],
        "config": check["config"],
        "property": check["property"],
        "expected": check["expected"],
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
        require(document.get("proofs") == [], "prerequisite matrix proofs must be []")
        checks = document.get("checks")
        require(isinstance(checks, list), "matrix checks must be an array")
        ids = [check.get("id") if isinstance(check, dict) else None for check in checks]
        require(ids == EXPECTED_IDS, f"matrix IDs/order differ from contract: {ids}")
        require(len(ids) == len(set(ids)) == 24, "matrix must contain 24 unique rows")
        for prefix, expected_count in GROUP_PREFIX_COUNTS.items():
            actual = sum(isinstance(item, str) and item.startswith(prefix) for item in ids)
            require(
                actual == expected_count,
                f"matrix group {prefix!r}: expected {expected_count}, got {actual}",
            )

        seen_configs: set[str] = set()
        results = [
            validate_check(formal_dir=formal_dir, check=check, seen_configs=seen_configs)
            for check in checks
        ]
        print(
            json.dumps(
                {
                    "schema": 1,
                    "status": "PASS",
                    "manifest": str(manifest_path),
                    "check_count": len(results),
                    "groups": GROUP_PREFIX_COUNTS,
                    "checks": results,
                },
                indent=2,
                sort_keys=True,
            )
        )
        return 0
    except StaticContractError as exc:
        print(f"prerequisite static check failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
