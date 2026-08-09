#!/usr/bin/env python3
"""Static contract tests for every formal acceptance-matrix row.

These checks do not replace SANY or TLC. They reject inexpensive wiring errors
before either pinned toolchain starts:

* missing module/config/proof/trace-manifest files;
* duplicate check IDs across the two acceptance manifests;
* omitted or unknown constant assignments, including inherited local modules;
* missing SPECIFICATION/property operators;
* absent explicit CHECK_DEADLOCK policy;
* expected counterexamples that check more than their one named property; and
* trace manifests whose property, events, harness anchors, or metadata do not
  match the owning matrix row.
"""

from __future__ import annotations

import json
import re
import sys
import unittest
from functools import lru_cache
from pathlib import Path
from typing import Any, Iterable

sys.dont_write_bytecode = True

FORMAL_DIR = Path(__file__).resolve().parent
MATRIX_FILES = (
    FORMAL_DIR / "formal-checks.json",
    FORMAL_DIR / "formal-checks-lifecycle-allocator.json",
)

_IDENTIFIER = r"[A-Za-z_][A-Za-z0-9_]*"
_OPERATOR_RE = re.compile(rf"^\s*({_IDENTIFIER})\s*==", re.MULTILINE)
_EXTENDS_RE = re.compile(r"^\s*EXTENDS\s+(.+)$", re.MULTILINE)
_CONFIG_ASSIGNMENT_RE = re.compile(
    rf"^\s*({_IDENTIFIER})\s*(?:=|<-)\s*", re.MULTILINE
)
_DIRECT_CHECK_RE = re.compile(
    rf"^\s*(INVARIANT|PROPERTY)\s+({_IDENTIFIER})\s*$", re.MULTILINE
)
_SPECIFICATION_RE = re.compile(
    rf"^\s*SPECIFICATION\s+({_IDENTIFIER})\s*$", re.MULTILINE
)
_CHECK_DEADLOCK_RE = re.compile(
    r"^\s*CHECK_DEADLOCK\s+(TRUE|FALSE)\s*$", re.MULTILINE
)


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def module_path(name: str) -> Path:
    return FORMAL_DIR / f"{name}.tla"


def _constant_names(text: str) -> set[str]:
    """Extract names from root CONSTANT/CONSTANTS declaration blocks."""
    lines = text.splitlines()
    names: set[str] = set()
    index = 0
    while index < len(lines):
        match = re.match(r"^\s*CONSTANTS?\b(.*)$", lines[index])
        if match is None:
            index += 1
            continue
        fragments = [match.group(1)]
        cursor = index + 1
        while cursor < len(lines):
            line = lines[cursor]
            stripped = line.strip()
            if not stripped:
                break
            if re.match(
                r"^(ASSUME|VARIABLES?|EXTENDS|LOCAL|THEOREM|LEMMA|INSTANCE)\b",
                stripped,
            ):
                break
            if "==" in stripped or stripped.startswith(("/\\", "\\/")):
                break
            fragments.append(stripped)
            cursor += 1
        declaration = " ".join(fragments)
        names.update(re.findall(_IDENTIFIER, declaration))
        index = max(index + 1, cursor)
    return names


def _local_extends(text: str) -> set[str]:
    result: set[str] = set()
    for match in _EXTENDS_RE.finditer(text):
        for name in re.findall(_IDENTIFIER, match.group(1)):
            if module_path(name).is_file():
                result.add(name)
    return result


@lru_cache(maxsize=None)
def module_closure(name: str) -> tuple[str, ...]:
    path = module_path(name)
    if not path.is_file():
        raise AssertionError(f"missing TLA+ module: {path}")
    text = path.read_text(encoding="utf-8")
    closure = {name}
    for parent in _local_extends(text):
        closure.update(module_closure(parent))
    return tuple(sorted(closure))


def closure_texts(name: str) -> dict[str, str]:
    return {
        module: module_path(module).read_text(encoding="utf-8")
        for module in module_closure(name)
    }


def closure_constants(name: str) -> set[str]:
    constants: set[str] = set()
    for text in closure_texts(name).values():
        constants.update(_constant_names(text))
    return constants


def closure_operators(name: str) -> set[str]:
    operators: set[str] = set()
    for text in closure_texts(name).values():
        operators.update(_OPERATOR_RE.findall(text))
    return operators


def direct_checks(config_text: str) -> list[tuple[str, str]]:
    return [
        (kind, operator)
        for kind, operator in _DIRECT_CHECK_RE.findall(config_text)
    ]


def required_event_names(trace_manifest: dict[str, Any]) -> set[str]:
    events = trace_manifest.get("events")
    if not isinstance(events, list):
        raise AssertionError("trace manifest events must be an array")
    names: list[str] = []
    for event in events:
        if not isinstance(event, dict) or not isinstance(event.get("name"), str):
            raise AssertionError("each trace event requires a string name")
        names.append(event["name"])
    if len(names) != len(set(names)):
        raise AssertionError(f"duplicate trace event names: {names}")
    return set(names)


def iter_matrix_rows() -> Iterable[tuple[Path, dict[str, Any]]]:
    for matrix_path in MATRIX_FILES:
        document = load_json(matrix_path)
        if not isinstance(document, dict) or document.get("schema") != 1:
            raise AssertionError(f"{matrix_path}: unsupported schema")
        checks = document.get("checks")
        if not isinstance(checks, list) or not checks:
            raise AssertionError(f"{matrix_path}: nonempty checks array required")
        for row in checks:
            if not isinstance(row, dict):
                raise AssertionError(f"{matrix_path}: check row must be an object")
            yield matrix_path, row


class ManifestContractTests(unittest.TestCase):
    def test_matrix_check_ids_are_globally_unique(self) -> None:
        seen: dict[str, Path] = {}
        for matrix_path, row in iter_matrix_rows():
            check_id = row.get("id")
            self.assertIsInstance(check_id, str, matrix_path)
            self.assertNotIn(
                check_id,
                seen,
                f"duplicate check id {check_id!r} in {seen.get(check_id)} and {matrix_path}",
            )
            seen[check_id] = matrix_path

    def test_each_check_has_complete_module_config_and_property_contract(self) -> None:
        for matrix_path, row in iter_matrix_rows():
            with self.subTest(matrix=matrix_path.name, check=row.get("id")):
                module = row.get("module")
                config = row.get("config")
                property_name = row.get("property")
                expected = row.get("expected")
                kind = row.get("kind")
                workers = row.get("workers")
                self.assertIsInstance(module, str)
                self.assertIsInstance(config, str)
                self.assertIsInstance(property_name, str)
                self.assertIn(expected, {"pass", "counterexample"})
                self.assertIn(kind, {"safety", "liveness"})
                self.assertIsInstance(workers, int)
                self.assertGreaterEqual(workers, 1)
                if kind == "liveness":
                    self.assertEqual(workers, 1)

                tla_path = module_path(module)
                config_path = FORMAL_DIR / config
                self.assertTrue(tla_path.is_file(), tla_path)
                self.assertTrue(config_path.is_file(), config_path)
                config_text = config_path.read_text(encoding="utf-8")

                specification = _SPECIFICATION_RE.findall(config_text)
                self.assertEqual(
                    len(specification),
                    1,
                    f"{config_path}: exactly one SPECIFICATION required",
                )
                operators = closure_operators(module)
                self.assertIn(
                    specification[0],
                    operators,
                    f"{config_path}: specification operator is absent from {module_closure(module)}",
                )
                self.assertIn(
                    property_name,
                    operators,
                    f"{config_path}: named property is absent from {module_closure(module)}",
                )

                deadlock = _CHECK_DEADLOCK_RE.findall(config_text)
                self.assertEqual(
                    len(deadlock),
                    1,
                    f"{config_path}: exactly one explicit CHECK_DEADLOCK required",
                )

                checked = direct_checks(config_text)
                checked_names = [operator for _, operator in checked]
                self.assertIn(
                    property_name,
                    checked_names,
                    f"{config_path}: manifest property is not checked directly",
                )
                if expected == "counterexample":
                    self.assertEqual(
                        checked,
                        [
                            (
                                "PROPERTY" if kind == "liveness" else "INVARIANT",
                                property_name,
                            )
                        ],
                        f"{config_path}: mutant must check exactly its named property",
                    )

                declared = closure_constants(module)
                assigned = set(_CONFIG_ASSIGNMENT_RE.findall(config_text))
                self.assertEqual(
                    declared - assigned,
                    set(),
                    f"{config_path}: unassigned constants {sorted(declared - assigned)}",
                )
                self.assertEqual(
                    assigned - declared,
                    set(),
                    f"{config_path}: unknown constant assignments {sorted(assigned - declared)}",
                )

    def test_counterexample_trace_manifests_match_owning_rows(self) -> None:
        for matrix_path, row in iter_matrix_rows():
            with self.subTest(matrix=matrix_path.name, check=row.get("id")):
                trace_relative = row.get("trace_manifest")
                if row.get("expected") == "pass":
                    self.assertIsNone(
                        trace_relative,
                        "passing checks must not carry a counterexample manifest",
                    )
                    continue
                self.assertIsInstance(trace_relative, str)
                trace_path = FORMAL_DIR / trace_relative
                self.assertTrue(trace_path.is_file(), trace_path)
                trace = load_json(trace_path)
                self.assertIsInstance(trace, dict)
                self.assertEqual(trace.get("schema"), 1)
                self.assertEqual(trace.get("expected_result"), "counterexample")
                self.assertEqual(trace.get("property"), row.get("property"))
                self.assertIsInstance(trace.get("scenario"), str)
                self.assertTrue(trace.get("scenario"))

                event_names = required_event_names(trace)
                required = trace.get("required_subsequence")
                self.assertIsInstance(required, list)
                self.assertTrue(required)
                self.assertTrue(all(isinstance(item, str) for item in required))
                self.assertEqual(
                    set(required) - event_names,
                    set(),
                    f"{trace_path}: required sequence references unknown events",
                )

                harness = trace.get("harness_steps", [])
                self.assertIsInstance(harness, list)
                for step in harness:
                    self.assertIsInstance(step, dict)
                    self.assertIn(step.get("after"), event_names)
                    self.assertIsInstance(step.get("emit"), str)
                    self.assertTrue(step.get("emit"))

                final_all = trace.get("final_all", [])
                self.assertIsInstance(final_all, list)
                self.assertTrue(
                    final_all or trace.get("require_lasso") is True,
                    f"{trace_path}: counterexample must validate a final predicate or lasso",
                )

                metadata = trace.get("metadata", {})
                self.assertIsInstance(metadata, dict)
                if "module" in metadata:
                    self.assertEqual(metadata["module"], row.get("module"))
                if "config" in metadata:
                    self.assertEqual(metadata["config"], row.get("config"))

    def test_proof_rows_reference_existing_pass_candidates(self) -> None:
        for matrix_path in MATRIX_FILES:
            document = load_json(matrix_path)
            proofs = document.get("proofs", [])
            self.assertIsInstance(proofs, list)
            for proof in proofs:
                with self.subTest(matrix=matrix_path.name, proof=proof.get("id")):
                    self.assertIsInstance(proof, dict)
                    self.assertEqual(proof.get("expected"), "pass")
                    proof_file = proof.get("file")
                    self.assertIsInstance(proof_file, str)
                    self.assertTrue((FORMAL_DIR / proof_file).is_file())


if __name__ == "__main__":
    unittest.main()
