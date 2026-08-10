#!/usr/bin/env python3
"""Generate a deterministic action-split TLAPS induction proof.

The theorem is not weakened: the generated module proves the existing
AssignmentFenceCore.CoreInvariant from the existing Spec.  Every action named
by Next receives an explicit preservation lemma, and stuttering is handled
separately under UNCHANGED vars.
"""

from __future__ import annotations

import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
MODEL = HERE / "AssignmentFenceCore.tla"
PROOF = HERE / "AssignmentFenceCoreProof.tla"

HEADER_RE = re.compile(
    r"(?m)^([A-Za-z_][A-Za-z0-9_]*)"
    r"(?:\(([^=\n]*)\))?\s*=="
)
IDENT_RE = re.compile(r"\b[A-Za-z_][A-Za-z0-9_]*\b")


def parse_definitions(text: str) -> tuple[dict[str, str], dict[str, tuple[str, ...]]]:
    matches = list(HEADER_RE.finditer(text))
    definitions: dict[str, str] = {}
    parameters: dict[str, tuple[str, ...]] = {}
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        name = match.group(1)
        definitions[name] = text[match.end():end]
        raw = match.group(2) or ""
        parameters[name] = tuple(
            item.strip() for item in raw.split(",") if item.strip()
        )
    return definitions, parameters


def closure(targets: list[str], definitions: dict[str, str]) -> list[str]:
    seen: set[str] = set()
    pending = list(targets)
    while pending:
        name = pending.pop()
        if name in seen or name not in definitions:
            continue
        seen.add(name)
        for dependency in IDENT_RE.findall(definitions[name]):
            if dependency in definitions and dependency not in seen:
                pending.append(dependency)
    return sorted(seen)


def action_names(definitions: dict[str, str]) -> list[str]:
    direct = [
        name
        for name in IDENT_RE.findall(definitions["Next"])
        if name in definitions and "'" in definitions[name]
    ]
    result = sorted(dict.fromkeys(direct))
    if not result:
        raise SystemExit("Next contains no primed action definitions")
    return result


def theorem_name(action: str) -> str:
    return f"{action}PreservesCoreInvariant"


def action_application(action: str, parameters: tuple[str, ...]) -> str:
    if not parameters:
        return action
    return f"{action}({', '.join(parameters)})"


def generated_text() -> str:
    model = MODEL.read_text(encoding="utf-8")
    definitions, parameters = parse_definitions(model)
    required = {"Init", "Next", "Spec", "vars", "CoreInvariant"}
    missing = required - definitions.keys()
    if missing:
        raise SystemExit(f"proof generator missing definitions: {sorted(missing)}")

    actions = action_names(definitions)
    init_defs = closure(["Init", "CoreInvariant"], definitions)
    invariant_defs = closure(["CoreInvariant"], definitions)

    lines = [
        "-------------------- MODULE AssignmentFenceCoreProof --------------------",
        "(***************************************************************************",
        "Generated action-split proof for the existing unbounded logical core.",
        "Regenerate with generate_assignment_fence_core_proof_v2.py.",
        "***************************************************************************)",
        "EXTENDS AssignmentFenceCore",
        "",
        "THEOREM InitImpliesCoreInvariant ==",
        "    Init => CoreInvariant",
        "BY SMT DEF " + ", ".join(init_defs),
        "",
    ]

    action_lemmas: list[str] = []
    for action in actions:
        lemma = theorem_name(action)
        action_lemmas.append(lemma)
        params = parameters.get(action, ())
        proposition = (
            f"CoreInvariant /\\ {action_application(action, params)}"
            " => CoreInvariant'"
        )
        if params:
            proposition = f"\\A {', '.join(params)} : ({proposition})"
        definitions_for_action = closure(
            [action, "CoreInvariant"], definitions
        )
        lines.extend(
            [
                f"THEOREM {lemma} ==",
                f"    {proposition}",
                "BY SMT DEF " + ", ".join(definitions_for_action),
                "",
            ]
        )

    stutter_defs = sorted(set(invariant_defs + ["vars"]))
    lines.extend(
        [
            "THEOREM StutterPreservesCoreInvariant ==",
            "    CoreInvariant /\\ UNCHANGED vars => CoreInvariant'",
            "BY SMT DEF " + ", ".join(stutter_defs),
            "",
            "THEOREM NextPreservesCoreInvariant ==",
            "    CoreInvariant /\\ [Next]_vars => CoreInvariant'",
            "BY " + ", ".join(action_lemmas + ["StutterPreservesCoreInvariant"]),
            "   SMT DEF Next, vars",
            "",
            "THEOREM SpecImpliesCoreInvariant ==",
            "    Spec => []CoreInvariant",
            "<1>1. Init => CoreInvariant",
            "      BY InitImpliesCoreInvariant",
            "<1>2. CoreInvariant /\\ [Next]_vars => CoreInvariant'",
            "      BY NextPreservesCoreInvariant",
            "<1> QED",
            "      BY <1>1, <1>2, PTL DEF Spec",
            "",
            "=============================================================================",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> None:
    text = generated_text()
    if "THEOREM SpecImpliesCoreInvariant" not in text:
        raise SystemExit("generated proof lacks final theorem")
    PROOF.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    main()
