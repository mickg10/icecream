#!/usr/bin/env python3
"""Reject ambiguous top-level Boolean operators in primed assignments.

TLA+ equality binds more tightly than ``/\\`` and ``\\/``. Consequently:

    /\\ seen' = seen \\/ event

is not one assignment with a Boolean RHS; it is a top-level disjunction whose
right branch leaves ``seen'`` unconstrained. The same defect applies to an
unparenthesized top-level conjunction.

This linter examines primed-assignment conjuncts in every local ``*.tla``
module. If an ordinary expression RHS contains a top-level ``/\\`` or ``\\/``,
it must be parenthesized so the operator is below the assignment. Structured
RHS forms beginning with IF, CASE, or LET are parsed by their own TLA+ grammar
and are exempt from this deliberately narrow check.
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


_ASSIGNMENT_RE = re.compile(
    r"^(?P<indent>\s*)/\\\s+"
    r"(?P<variable>[A-Za-z_][A-Za-z0-9_]*)'\s*=\s*(?P<rhs>.*)$"
)
_STRUCTURED_PREFIXES = ("IF ", "CASE ", "LET ")


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


def lint_text(path: Path, text: str) -> list[Violation]:
    lines = text.splitlines()
    violations: list[Violation] = []
    index = 0
    while index < len(lines):
        match = _ASSIGNMENT_RE.match(lines[index])
        if match is None:
            index += 1
            continue

        assignment_indent = len(match.group("indent"))
        fragments = [match.group("rhs")]
        cursor = index + 1
        while cursor < len(lines):
            line = lines[cursor]
            if not line.strip():
                fragments.append(line)
                cursor += 1
                continue
            indentation = len(line) - len(line.lstrip())
            if indentation <= assignment_indent:
                break
            fragments.append(line.strip())
            cursor += 1

        rhs = "\n".join(fragments).strip()
        operator = _top_level_boolean_operator(rhs)
        if operator is not None:
            violations.append(
                Violation(
                    path=path,
                    line=index + 1,
                    variable=match.group("variable"),
                    operator=operator,
                    rhs=rhs,
                )
            )
        index = max(index + 1, cursor)
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
                f"{item.path}:{item.line}: primed assignment {item.variable}' "
                f"has top-level {item.operator}: {compact}",
                file=sys.stderr,
            )
        return 1
    print(f"checked {len(list(args.directory.glob('*.tla')))} TLA+ modules")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
