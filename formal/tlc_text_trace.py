#!/usr/bin/env python3
"""Convert a TLC text counterexample into trace_to_harness JSON input.

TLA+ Tools 1.7.4 prints complete counterexample states to the text log but does
not support the newer ``-dumpTrace json`` option.  This module parses that
retained text deterministically.  It supports the value forms emitted by the
finite models in this directory: booleans, integers, strings, model values,
sequences, sets, records/functions, and ``:>``/``@@`` function constructors.

The converter is fail-closed: every state must contain assignments, delimiters
must balance, input must be fully consumed, and duplicate variables are an
error.  It does not infer actions; trace_to_harness.py classifies adjacent
state records with a declarative manifest after conversion.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Sequence


class TLCTextTraceError(RuntimeError):
    """The retained TLC text is not a complete parseable state trace."""


@dataclass(frozen=True)
class ParsedTextTrace:
    states: list[dict[str, Any]]
    state_ordinals: list[int]
    lasso: dict[str, Any] | None


_TOKEN_END = set(",]}>)\r\n\t ")
_STATE_RE = re.compile(r"^State\s+([0-9]+)\s*:\s*(.*)$")
_ASSIGN_RE = re.compile(r"^\s*/\\\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$")
_BACK_RE = re.compile(r"\bBack\s+to\s+state\s+([0-9]+)\b", re.IGNORECASE)
_STUTTER_RE = re.compile(r"\bStuttering\b", re.IGNORECASE)


class _ValueParser:
    def __init__(self, text: str) -> None:
        self.text = text
        self.pos = 0

    def error(self, message: str) -> TLCTextTraceError:
        left = max(0, self.pos - 30)
        right = min(len(self.text), self.pos + 50)
        return TLCTextTraceError(
            f"{message} at offset {self.pos}: {self.text[left:right]!r}"
        )

    def skip_ws(self) -> None:
        while self.pos < len(self.text) and self.text[self.pos].isspace():
            self.pos += 1

    def starts(self, token: str) -> bool:
        return self.text.startswith(token, self.pos)

    def consume(self, token: str) -> None:
        self.skip_ws()
        if not self.starts(token):
            raise self.error(f"expected {token!r}")
        self.pos += len(token)

    def parse(self) -> Any:
        value = self.parse_value()
        self.skip_ws()
        if self.pos != len(self.text):
            raise self.error("trailing input after TLA+ value")
        return value

    def parse_value(self) -> Any:
        self.skip_ws()
        if self.pos >= len(self.text):
            raise self.error("expected a TLA+ value")
        if self.starts("<<"):
            return self.parse_sequence()
        char = self.text[self.pos]
        if char == '"':
            return self.parse_string()
        if char == "{":
            return self.parse_set()
        if char == "[":
            return self.parse_bracket_map()
        if char == "(":
            return self.parse_parenthesized()
        if char == "-" or char.isdigit():
            return self.parse_number_or_bare()
        return self.parse_bare()

    def parse_string(self) -> str:
        start = self.pos
        self.pos += 1
        escaped = False
        while self.pos < len(self.text):
            char = self.text[self.pos]
            self.pos += 1
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                literal = self.text[start:self.pos]
                try:
                    return json.loads(literal)
                except json.JSONDecodeError as exc:
                    raise self.error(f"invalid TLC string literal: {exc}") from exc
        raise self.error("unterminated string literal")

    def parse_number_or_bare(self) -> Any:
        start = self.pos
        if self.text[self.pos] == "-":
            self.pos += 1
        digits = self.pos
        while self.pos < len(self.text) and self.text[self.pos].isdigit():
            self.pos += 1
        if self.pos > digits and (
            self.pos == len(self.text) or self.text[self.pos] in _TOKEN_END
        ):
            return int(self.text[start:self.pos])
        self.pos = start
        return self.parse_bare()

    def parse_bare(self) -> Any:
        start = self.pos
        while self.pos < len(self.text):
            if self.starts("@@") or self.starts(":>") or self.starts("|->"):
                break
            char = self.text[self.pos]
            if char in _TOKEN_END:
                break
            self.pos += 1
        if self.pos == start:
            raise self.error("empty model value")
        token = self.text[start:self.pos]
        if token == "TRUE":
            return True
        if token == "FALSE":
            return False
        return token

    def parse_sequence(self) -> list[Any]:
        self.consume("<<")
        result: list[Any] = []
        self.skip_ws()
        if self.starts(">>"):
            self.pos += 2
            return result
        while True:
            result.append(self.parse_value())
            self.skip_ws()
            if self.starts(">>"):
                self.pos += 2
                return result
            self.consume(",")

    def parse_set(self) -> list[Any]:
        self.consume("{")
        result: list[Any] = []
        self.skip_ws()
        if self.starts("}"):
            self.pos += 1
            return result
        while True:
            result.append(self.parse_value())
            self.skip_ws()
            if self.starts("}"):
                self.pos += 1
                return result
            self.consume(",")

    @staticmethod
    def key_string(key: Any) -> str:
        if isinstance(key, bool):
            return "TRUE" if key else "FALSE"
        if isinstance(key, (str, int)):
            return str(key)
        return json.dumps(key, sort_keys=True, separators=(",", ":"))

    def parse_bracket_map(self) -> dict[str, Any]:
        self.consume("[")
        result: dict[str, Any] = {}
        self.skip_ws()
        if self.starts("]"):
            self.pos += 1
            return result
        while True:
            key = self.parse_value()
            self.consume("|->")
            value = self.parse_value()
            skey = self.key_string(key)
            if skey in result:
                raise self.error(f"duplicate function/record key {skey!r}")
            result[skey] = value
            self.skip_ws()
            if self.starts("]"):
                self.pos += 1
                return result
            self.consume(",")

    def parse_parenthesized(self) -> Any:
        self.consume("(")
        self.skip_ws()
        if self.starts(")"):
            self.pos += 1
            return []

        first = self.parse_value()
        self.skip_ws()
        if self.starts(":>"):
            result: dict[str, Any] = {}
            key = first
            while True:
                self.consume(":>")
                value = self.parse_value()
                skey = self.key_string(key)
                if skey in result:
                    raise self.error(f"duplicate :> function key {skey!r}")
                result[skey] = value
                self.skip_ws()
                if self.starts(")"):
                    self.pos += 1
                    return result
                self.consume("@@")
                key = self.parse_value()
        self.consume(")")
        return first


def parse_tla_value(text: str) -> Any:
    return _ValueParser(text).parse()


def _flush_assignment(
    state: dict[str, Any], variable: str | None, fragments: list[str], ordinal: int
) -> None:
    if variable is None:
        return
    if variable in state:
        raise TLCTextTraceError(f"state {ordinal}: duplicate variable {variable!r}")
    value_text = " ".join(fragment.strip() for fragment in fragments).strip()
    if not value_text:
        raise TLCTextTraceError(f"state {ordinal}: variable {variable!r} has no value")
    try:
        state[variable] = parse_tla_value(value_text)
    except TLCTextTraceError as exc:
        raise TLCTextTraceError(
            f"state {ordinal}, variable {variable}: {exc}"
        ) from exc


def _parse_state_block(ordinal: int, lines: list[str]) -> dict[str, Any]:
    state: dict[str, Any] = {}
    variable: str | None = None
    fragments: list[str] = []
    for line in lines:
        match = _ASSIGN_RE.match(line)
        if match:
            _flush_assignment(state, variable, fragments, ordinal)
            variable = match.group(1)
            fragments = [match.group(2)]
        elif variable is not None:
            stripped = line.strip()
            if stripped and not stripped.startswith("Error:"):
                fragments.append(stripped)
    _flush_assignment(state, variable, fragments, ordinal)
    if not state:
        raise TLCTextTraceError(f"state {ordinal}: no /\\ variable assignments found")
    return state


def parse_tlc_text_trace(text: str) -> ParsedTextTrace:
    states: list[dict[str, Any]] = []
    ordinals: list[int] = []
    current_ordinal: int | None = None
    current_lines: list[str] = []
    lasso: dict[str, Any] | None = None

    def flush() -> None:
        nonlocal current_ordinal, current_lines
        if current_ordinal is not None:
            states.append(_parse_state_block(current_ordinal, current_lines))
            ordinals.append(current_ordinal)
        current_ordinal = None
        current_lines = []

    for line in text.splitlines():
        match = _STATE_RE.match(line)
        if match:
            flush()
            ordinal = int(match.group(1))
            suffix = match.group(2).strip()
            back = _BACK_RE.search(suffix)
            if back:
                lasso = {
                    "kind": "back-edge",
                    "source_state_ordinal": ordinal,
                    "target_state_ordinal": int(back.group(1)),
                }
                continue
            if _STUTTER_RE.search(suffix):
                lasso = {"kind": "stuttering", "state_ordinal": ordinal}
                continue
            current_ordinal = ordinal
            current_lines = []
            continue
        if current_ordinal is not None:
            current_lines.append(line)
    flush()

    if not states:
        raise TLCTextTraceError("no TLC counterexample states found in text log")
    if len(states) < 2:
        raise TLCTextTraceError("counterexample text contains fewer than two states")
    if ordinals != sorted(ordinals) or len(ordinals) != len(set(ordinals)):
        raise TLCTextTraceError(f"state ordinals are not strictly increasing: {ordinals}")
    return ParsedTextTrace(states=states, state_ordinals=ordinals, lasso=lasso)


def convert_file(source: Path, destination: Path) -> ParsedTextTrace:
    try:
        text = source.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        raise TLCTextTraceError(f"cannot read {source}: {exc}") from exc
    parsed = parse_tlc_text_trace(text)
    try:
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(
            json.dumps(parsed.states, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    except OSError as exc:
        raise TLCTextTraceError(f"cannot write {destination}: {exc}") from exc
    return parsed


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True, help="retained TLC text log")
    parser.add_argument("--output", type=Path, required=True, help="JSON state array")
    parser.add_argument(
        "--metadata-output",
        type=Path,
        help="optional JSON file containing state ordinals and lasso metadata",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        parsed = convert_file(args.input, args.output)
        if args.metadata_output:
            args.metadata_output.parent.mkdir(parents=True, exist_ok=True)
            args.metadata_output.write_text(
                json.dumps(
                    {
                        "state_ordinals": parsed.state_ordinals,
                        "lasso": parsed.lasso,
                    },
                    indent=2,
                    sort_keys=True,
                )
                + "\n",
                encoding="utf-8",
            )
        return 0
    except TLCTextTraceError as exc:
        print(f"TLC text-trace conversion failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
