#!/usr/bin/env python3
"""Reject ambiguous Boolean precedence in transition conjuncts.

TLA+ equality binds more tightly than ``/\\`` and ``\\/``. Consequently:

    /\\ seen' = seen \\/ event

is not one assignment with a Boolean RHS; it is a top-level disjunction whose
right branch leaves ``seen'`` unconstrained. Likewise, a guard written as:

    /\\ claimMade \\/ alreadyStarted
    /\\ phase' = ...

can let the disjunct escape the surrounding conjunct list and bypass the state
updates. Ordinary Boolean RHS expressions and guard disjunctions must therefore
be parenthesized.

This linter examines every root ``*.tla`` module. It rejects:

* a primed assignment whose ordinary RHS contains a top-level ``/\\`` or
  ``\\/``; and
* a conjunction bullet whose ordinary guard contains a top-level ``\\/``.

Structured RHS/guard forms beginning with IF, CASE, LET, quantifiers, or CHOOSE
are parsed by their own TLA+ grammar and are exempt from this deliberately
narrow check. TLA+ line comments and nested block comments are removed before
conjuncts are recognized.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

sys.dont_write_bytecode = True


@dataclass(frozen=True)
class Violation:
    path: Path
    line: int
    variable: str
    operator: str
    rhs: str
    kind: str = "assignment"


_ASSIGNMENT_RE = re.compile(
    r"^(?P<indent>\s*)/\\\s+"
    r"(?P<variable>[A-Za-z_][A-Za-z0-9_]*)'\s*=\s*(?P<rhs>.*)$"
)
_CONJUNCT_RE = re.compile(r"^(?P<indent>\s*)/\\\s+(?P<rhs>.*)$")
_STRUCTURED_PREFIXES = (
    "IF ",
    "CASE ",
    "LET ",
    "\\A ",
    "\\E ",
    "CHOOSE ",
)


def _strip_block_comments(text: str) -> str:
    """Blank nested ``(* ... *)`` comments while preserving line numbers."""
    output: list[str] = []
    index = 0
    depth = 0
    in_string = False
    escaped = False
    while index < len(text):
        if depth > 0:
            if text.startswith("(*", index):
                depth += 1
                output.extend("  ")
                index += 2
                continue
            if text.startswith("*)", index):
                depth -= 1
                output.extend("  ")
                index += 2
                continue
            char = text[index]
            output.append("\n" if char == "\n" else " ")
            index += 1
            continue

        if not in_string and text.startswith("(*", index):
            depth = 1
            output.extend("  ")
            index += 2
            continue

        char = text[index]
        output.append(char)
        if in_string:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
        elif char == '"':
            in_string = True
        index += 1

    if depth != 0:
        raise RuntimeError("unterminated TLA+ block comment")
    return "".join(output)


def _without_line_comments(text: str) -> str:
    """Remove TLA+ line comments while preserving quoted strings."""
    output: list[str] = []
    index = 0
    in_string = False
    escaped = False
    while index < len(text):
        if not in_string and text.startswith("\\*", index):
            while index < len(text) and text[index] != "\n":
                index += 1
            continue
        char = text[index]
        output.append(char)
        if in_string:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
        elif char == '"':
            in_string = True
        index += 1
    return "".join(output)


def _top_level_boolean_operator(rhs: str) -> str | None:
    text = _without_line_comments(rhs).strip()
    if not text or text.startswith(_STRUCTURED_PREFIXES):
        return None

    paren = bracket = brace = tuple_depth = 0
    in_string = False
    escaped = False
    index = 0
    while index < len(text):
        if in_string:
            char = text[index]
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
            index += 1
            continue

        if text[index] == '"':
            in_string = True
            index += 1
            continue
        if text.startswith("<<", index):
            tuple_depth += 1
            index += 2
            continue
        if text.startswith(">>", index) and tuple_depth > 0:
            tuple_depth -= 1
            index += 2
            continue

        char = text[index]
        if char == "(":
            paren += 1
        elif char == ")":
            paren = max(0, paren - 1)
        elif char == "[":
            bracket += 1
        elif char == "]":
            bracket = max(0, bracket - 1)
        elif char == "{":
            brace += 1
        elif char == "}":
            brace = max(0, brace - 1)
        elif paren == bracket == brace == tuple_depth == 0:
            if text.startswith("/\\", index):
                return "/\\"
            if text.startswith("\\/", index):
                return "\\/"
        index += 1
    return None


def _collect_rhs(
    lines: list[str], index: int, indent: int, first_fragment: str
) -> tuple[str, int]:
    fragments = [first_fragment]
    cursor = index + 1
    while cursor < len(lines):
        line = lines[cursor]
        if not line.strip():
            fragments.append(line)
            cursor += 1
            continue
        indentation = len(line) - len(line.lstrip())
        if indentation <= indent:
            break
        fragments.append(line.strip())
        cursor += 1
    return "\n".join(fragments).strip(), cursor


def lint_text(path: Path, text: str) -> list[Violation]:
    lines = _strip_block_comments(text).splitlines()
    violations: list[Violation] = []
    index = 0
    while index < len(lines):
        assignment = _ASSIGNMENT_RE.match(lines[index])
        if assignment is not None:
            indent = len(assignment.group("indent"))
            rhs, cursor = _collect_rhs(
                lines, index, indent, assignment.group("rhs")
            )
            operator = _top_level_boolean_operator(rhs)
            if operator is not None:
                violations.append(
                    Violation(
                        path=path,
                        line=index + 1,
                        variable=assignment.group("variable"),
                        operator=operator,
                        rhs=rhs,
                        kind="assignment",
                    )
                )
            index = max(index + 1, cursor)
            continue

        conjunct = _CONJUNCT_RE.match(lines[index])
        if conjunct is not None:
            indent = len(conjunct.group("indent"))
            rhs, cursor = _collect_rhs(
                lines, index, indent, conjunct.group("rhs")
            )
            operator = _top_level_boolean_operator(rhs)
            if operator == "\\/":
                violations.append(
                    Violation(
                        path=path,
                        line=index + 1,
                        variable="<guard>",
                        operator=operator,
                        rhs=rhs,
                        kind="guard",
                    )
                )
            index = max(index + 1, cursor)
            continue

        index += 1
    return violations


def lint_directory(directory: Path) -> list[Violation]:
    violations: list[Violation] = []
    for path in sorted(directory.glob("*.tla")):
        try:
            text = path.read_text(encoding="utf-8")
        except OSError as exc:
            raise RuntimeError(f"cannot read {path}: {exc}") from exc
        violations.extend(lint_text(path, text))
    return violations


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "directory",
        nargs="?",
        type=Path,
        default=Path(__file__).resolve().parent,
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    violations = lint_directory(args.directory)
    if violations:
        for item in violations:
            compact = " ".join(item.rhs.split())
            print(
                f"{item.path}:{item.line}: {item.kind} conjunct "
                f"{item.variable} has top-level {item.operator}: {compact}",
                file=sys.stderr,
            )
        return 1
    print(f"checked {len(list(args.directory.glob('*.tla')))} TLA+ modules")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
