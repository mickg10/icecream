#!/usr/bin/env python3
"""Generate exact TLAPS proof candidates without changing the theorem.

Candidates differ only in proof decomposition/backend tactic.  Every candidate
proves the existing `Spec => []CoreInvariant` statement from the existing
AssignmentFenceCore module.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
MODEL = HERE / "AssignmentFenceCore.tla"

HEADER_RE = re.compile(
    r"(?m)^([A-Za-z_][A-Za-z0-9_]*)"
    r"(?:\(([^=\n]*)\))?\s*=="
)
IDENT_RE = re.compile(r"\b[A-Za-z_][A-Za-z0-9_]*\b")
CONJUNCT_RE = re.compile(r"/\\\s*([A-Za-z_][A-Za-z0-9_]*)\b")


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
    return sorted(
        dict.fromkeys(
            name
            for name in IDENT_RE.findall(definitions["Next"])
            if name in definitions and "'" in definitions[name]
        )
    )


def invariant_names(definitions: dict[str, str]) -> list[str]:
    result = [
        name
        for name in CONJUNCT_RE.findall(definitions["CoreInvariant"])
        if name in definitions
    ]
    result = list(dict.fromkeys(result))
    if not result:
        raise SystemExit("CoreInvariant has no named conjuncts")
    return result


def sanitized(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_]", "_", name)


def application(action: str, parameters: tuple[str, ...]) -> str:
    if not parameters:
        return action
    return f"{action}({', '.join(parameters)})"


def quantified(parameters: tuple[str, ...], proposition: str) -> str:
    if not parameters:
        return proposition
    return f"\\A {', '.join(parameters)} : ({proposition})"


def leaf_tactic(mode: str) -> str:
    if mode == "conjunct-smt":
        return "SMT"
    if mode == "conjunct-zenon":
        return "Zenon"
    if mode == "conjunct-smt-zenon":
        return "SMT, Zenon"
    raise ValueError(mode)


def module_name(mode: str) -> str:
    return "AssignmentFenceCoreProof" + "".join(
        part.capitalize() for part in mode.split("-")
    )


def generate_conjunct_candidate(mode: str) -> str:
    text = MODEL.read_text(encoding="utf-8")
    definitions, parameters = parse_definitions(text)
    actions = action_names(definitions)
    invariants = invariant_names(definitions)
    if not actions:
        raise SystemExit("Next has no actions")

    name = module_name(mode)
    tactic = leaf_tactic(mode)
    lines = [
        f"---------------- MODULE {name} ----------------",
        "(***************************************************************************",
        "Generated exact action-by-invariant-conjunct induction candidate.",
        f"Leaf tactic: {tactic}.",
        "***************************************************************************)",
        "EXTENDS AssignmentFenceCore",
        "",
        "THEOREM InitImpliesCoreInvariant ==",
        "    Init => CoreInvariant",
        f"BY {tactic} DEF " + ", ".join(
            closure(["Init", "CoreInvariant"], definitions)
        ),
        "",
    ]

    action_lemmas: list[str] = []
    invariant_assumption = " /\\ ".join(invariants)
    for action in actions:
        params = parameters.get(action, ())
        action_expression = application(action, params)
        leaf_lemmas: list[str] = []
        for invariant in invariants:
            lemma = f"{sanitized(action)}{sanitized(invariant)}Preserved"
            leaf_lemmas.append(lemma)
            proposition = quantified(
                params,
                f"({invariant_assumption}) /\\ {action_expression}"
                f" => {invariant}'",
            )
            leaf_defs = closure(
                [action, invariant, *invariants], definitions
            )
            lines.extend(
                [
                    f"THEOREM {lemma} ==",
                    f"    {proposition}",
                    f"BY {tactic} DEF " + ", ".join(leaf_defs),
                    "",
                ]
            )

        action_lemma = f"{sanitized(action)}PreservesCoreInvariant"
        action_lemmas.append(action_lemma)
        proposition = quantified(
            params,
            f"CoreInvariant /\\ {action_expression} => CoreInvariant'",
        )
        lines.extend(
            [
                f"THEOREM {action_lemma} ==",
                f"    {proposition}",
                "BY " + ", ".join(leaf_lemmas) + " DEF CoreInvariant",
                "",
            ]
        )

    lines.extend(
        [
            "THEOREM StutterPreservesCoreInvariant ==",
            "    CoreInvariant /\\ UNCHANGED vars => CoreInvariant'",
            f"BY {tactic} DEF " + ", ".join(
                closure(["CoreInvariant", "vars"], definitions)
            ),
            "",
            "THEOREM NextPreservesCoreInvariant ==",
            "    CoreInvariant /\\ [Next]_vars => CoreInvariant'",
            "BY " + ", ".join(
                action_lemmas + ["StutterPreservesCoreInvariant"]
            ) + f", {tactic} DEF Next, vars",
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
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--mode",
        required=True,
        choices=("conjunct-smt", "conjunct-zenon", "conjunct-smt-zenon"),
    )
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.write_text(
        generate_conjunct_candidate(args.mode), encoding="utf-8"
    )


if __name__ == "__main__":
    main()
