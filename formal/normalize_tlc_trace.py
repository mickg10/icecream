#!/usr/bin/env python3
"""
Normalize TLC counterexample output into the canonical JSON state-array format
consumed by trace_to_harness.py.

Supported inputs:
* modern ``-dumpTrace json`` output, including the real TLC wrapper
  ``{"counterexample": {"state": [[ordinal, state], ...]}, "vars": [...]}``;
* the historical flat-array/wrapper fixtures;
* the ordinary textual TLC error trace emitted by TLA+ Tools 1.7.4.

The text parser is deliberately a small fail-closed parser for TLC values used
by this repository's finite models: strings, integers, booleans, model values,
sets, tuples/sequences, records, and finite functions. It does not evaluate
TLA+ expressions.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Sequence

sys.dont_write_bytecode = True


class TraceNormalizationError(RuntimeError):
    """Input is not a complete supported TLC counterexample trace."""


class _NeedMoreInput(TraceNormalizationError):
    """A TLC value ended while a closing token/value was still required."""


@dataclass(frozen=True)
class NormalizedTrace:
    states: list[dict[str, Any]]
    source_format: str
    source_sha256: str


def _sha256(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def _read(path: Path) -> bytes:
    try:
        return path.read_bytes()
    except OSError as exc:
        raise TraceNormalizationError(f"cannot read {path}: {exc}") from exc


def _state_pairs(value: Any) -> list[dict[str, Any]] | None:
    if not isinstance(value, list):
        return None
    pairs: list[tuple[int, dict[str, Any]]] = []
    for entry in value:
        if (
            not isinstance(entry, list)
            or len(entry) != 2
            or isinstance(entry[0], bool)
            or not isinstance(entry[0], int)
            or not isinstance(entry[1], dict)
        ):
            return None
        pairs.append((entry[0], entry[1]))
    if not pairs:
        return []
    ordinals = [ordinal for ordinal, _ in pairs]
    if len(ordinals) != len(set(ordinals)):
        raise TraceNormalizationError("TLC JSON contains duplicate state ordinals")
    pairs.sort(key=lambda item: item[0])
    expected = list(range(pairs[0][0], pairs[0][0] + len(pairs)))
    if [ordinal for ordinal, _ in pairs] != expected:
        raise TraceNormalizationError("TLC JSON state ordinals are not contiguous")
    return [state for _, state in pairs]


def normalize_json_document(document: Any) -> list[dict[str, Any]]:
    """Normalize supported TLC/fixture JSON wrappers into a state array."""
    candidate = document
    if isinstance(candidate, dict) and "counterexample" in candidate:
        counterexample = candidate["counterexample"]
        if isinstance(counterexample, dict):
            for key in ("state", "states", "trace"):
                if key in counterexample:
                    candidate = counterexample[key]
                    break
            else:
                candidate = counterexample
        else:
            candidate = counterexample
    elif isinstance(candidate, dict):
        for key in ("states", "state", "trace"):
            if key in candidate:
                candidate = candidate[key]
                break

    pairs = _state_pairs(candidate)
    states = pairs if pairs is not None else candidate
    if not isinstance(states, list):
        raise TraceNormalizationError("normalized JSON trace is not an array")
    if not states:
        raise TraceNormalizationError("trace contains zero states")
    result: list[dict[str, Any]] = []
    for index, state in enumerate(states, start=1):
        if not isinstance(state, dict):
            raise TraceNormalizationError(f"state {index} is not a JSON object")
        result.append(state)
    return result


_TOKEN_RE = re.compile(
    r"""
    \s*
    (?:
        (?P<lseq><<)
      | (?P<rseq>>> )
      | (?P<mapsto>\|->)
      | (?P<funsto>:>)
      | (?P<merge>@@)
      | (?P<string>"(?:\\.|[^"\\])*")
      | (?P<number>-?[0-9]+)
      | (?P<identifier>[A-Za-z_$][A-Za-z0-9_$!]*)
      | (?P<punct>[\[\]\{\}\(\),])
      | (?P<other>\S)
    )
    """.replace(">> ", ">>"),
    re.VERBOSE,
)


@dataclass(frozen=True)
class _Token:
    kind: str
    text: str
    offset: int


def _tokenize(text: str) -> list[_Token]:
    tokens: list[_Token] = []
    cursor = 0
    while cursor < len(text):
        match = _TOKEN_RE.match(text, cursor)
        if not match:
            if text[cursor:].strip() == "":
                break
            raise TraceNormalizationError(
                f"cannot tokenize TLC value at offset {cursor}: "
                f"{text[cursor:cursor + 40]!r}"
            )
        cursor = match.end()
        kind = match.lastgroup
        assert kind is not None
        token_text = match.group(kind)
        if kind == "other":
            raise TraceNormalizationError(
                f"unsupported TLC token {token_text!r} at offset {match.start(kind)}"
            )
        tokens.append(_Token(kind, token_text, match.start(kind)))
    return tokens


def _json_key(value: Any) -> str:
    if isinstance(value, bool):
        return "TRUE" if value else "FALSE"
    if isinstance(value, (str, int)):
        return str(value)
    raise TraceNormalizationError(f"finite-function key is not scalar: {value!r}")


class _ValueParser:
    def __init__(self, text: str):
        self.tokens = _tokenize(text)
        self.index = 0

    def _peek(self, kind: str | None = None, text: str | None = None) -> bool:
        if self.index >= len(self.tokens):
            return False
        token = self.tokens[self.index]
        return (kind is None or token.kind == kind) and (
            text is None or token.text == text
        )

    def _take(self, kind: str | None = None, text: str | None = None) -> _Token:
        if self.index >= len(self.tokens):
            raise _NeedMoreInput("TLC value ended before it was complete")
        token = self.tokens[self.index]
        if kind is not None and token.kind != kind:
            raise TraceNormalizationError(
                f"expected token kind {kind}, got {token.kind} at offset {token.offset}"
            )
        if text is not None and token.text != text:
            raise TraceNormalizationError(
                f"expected {text!r}, got {token.text!r} at offset {token.offset}"
            )
        self.index += 1
        return token

    def parse_complete(self) -> Any:
        value = self.parse_value()
        if self.index != len(self.tokens):
            token = self.tokens[self.index]
            raise TraceNormalizationError(
                f"trailing TLC value token {token.text!r} at offset {token.offset}"
            )
        return value

    def parse_value(self) -> Any:
        if self.index >= len(self.tokens):
            raise _NeedMoreInput("TLC value is empty or incomplete")
        token = self.tokens[self.index]
        if token.kind == "string":
            self.index += 1
            try:
                return json.loads(token.text)
            except json.JSONDecodeError as exc:
                raise TraceNormalizationError(
                    f"invalid TLC string literal at offset {token.offset}: {exc}"
                ) from exc
        if token.kind == "number":
            self.index += 1
            return int(token.text)
        if token.kind == "identifier":
            self.index += 1
            if token.text == "TRUE":
                return True
            if token.text == "FALSE":
                return False
            return token.text
        if token.kind == "lseq":
            return self._parse_sequence()
        if token.kind == "punct" and token.text == "{":
            return self._parse_set()
        if token.kind == "punct" and token.text == "[":
            return self._parse_record()
        if token.kind == "punct" and token.text == "(":
            return self._parse_parenthesized()
        raise TraceNormalizationError(
            f"unsupported TLC value token {token.text!r} at offset {token.offset}"
        )

    def _parse_sequence(self) -> list[Any]:
        self._take("lseq")
        values: list[Any] = []
        if self._peek("rseq"):
            self._take("rseq")
            return values
        while True:
            values.append(self.parse_value())
            if self._peek("punct", ","):
                self._take("punct", ",")
                continue
            self._take("rseq")
            return values

    def _parse_set(self) -> list[Any]:
        self._take("punct", "{")
        values: list[Any] = []
        if self._peek("punct", "}"):
            self._take("punct", "}")
            return values
        while True:
            values.append(self.parse_value())
            if self._peek("punct", ","):
                self._take("punct", ",")
                continue
            self._take("punct", "}")
            return values

    def _parse_record(self) -> dict[str, Any]:
        self._take("punct", "[")
        result: dict[str, Any] = {}
        if self._peek("punct", "]"):
            self._take("punct", "]")
            return result
        while True:
            key = _json_key(self.parse_value())
            self._take("mapsto")
            if key in result:
                raise TraceNormalizationError(f"duplicate record/function key {key!r}")
            result[key] = self.parse_value()
            if self._peek("punct", ","):
                self._take("punct", ",")
                continue
            self._take("punct", "]")
            return result

    def _parse_parenthesized(self) -> Any:
        self._take("punct", "(")
        if self._peek("punct", ")"):
            self._take("punct", ")")
            return {}
        first = self.parse_value()
        if self._peek("funsto"):
            result: dict[str, Any] = {}
            while True:
                key = _json_key(first)
                self._take("funsto")
                if key in result:
                    raise TraceNormalizationError(
                        f"duplicate finite-function key {key!r}"
                    )
                result[key] = self.parse_value()
                if not self._peek("merge"):
                    break
                self._take("merge")
                first = self.parse_value()
            self._take("punct", ")")
            return result
        self._take("punct", ")")
        return first


def parse_tlc_value(text: str) -> Any:
    return _ValueParser(text).parse_complete()


_STATE_HEADER_RE = re.compile(
    r"(?m)^State\s+([0-9]+)\s*:\s*(?!Back\s+to\s+state\b|Stuttering\b).*$"
)
_ASSIGNMENT_RE = re.compile(
    r"^\s*/\\\s+([A-Za-z_$][A-Za-z0-9_$]*)\s*=\s*(.*)$"
)


def _state_blocks(text: str) -> list[tuple[int, str]]:
    headers = list(_STATE_HEADER_RE.finditer(text))
    blocks: list[tuple[int, str]] = []
    for index, header in enumerate(headers):
        start = header.end()
        end = headers[index + 1].start() if index + 1 < len(headers) else len(text)
        blocks.append((int(header.group(1)), text[start:end]))
    return blocks


def _parse_state_body(ordinal: int, body: str) -> dict[str, Any]:
    lines = body.splitlines()
    state: dict[str, Any] = {}
    index = 0
    while index < len(lines):
        match = _ASSIGNMENT_RE.match(lines[index])
        if not match:
            index += 1
            continue
        name = match.group(1)
        pieces = [match.group(2)]
        index += 1
        last_error: Exception | None = None
        while True:
            joined = "\n".join(pieces).strip()
            try:
                value = parse_tlc_value(joined)
                break
            except TraceNormalizationError as exc:
                last_error = exc
            if index >= len(lines):
                raise TraceNormalizationError(
                    f"state {ordinal} variable {name}: incomplete TLC value: "
                    f"{last_error}"
                ) from last_error
            if _ASSIGNMENT_RE.match(lines[index]):
                raise TraceNormalizationError(
                    f"state {ordinal} variable {name}: value ended before next variable"
                ) from last_error
            if lines[index] and not lines[index][0].isspace():
                raise TraceNormalizationError(
                    f"state {ordinal} variable {name}: unsupported/incomplete value "
                    f"before footer line {lines[index]!r}: {last_error}"
                ) from last_error
            pieces.append(lines[index])
            index += 1

        if name in state:
            raise TraceNormalizationError(
                f"state {ordinal} contains duplicate variable {name!r}"
            )
        state[name] = value

    if not state:
        raise TraceNormalizationError(
            f"state {ordinal} contains no '/\\ variable = value' assignments"
        )
    return state


def normalize_tlc_text(text: str) -> list[dict[str, Any]]:
    blocks = _state_blocks(text)
    if not blocks:
        raise TraceNormalizationError("TLC text contains no counterexample states")
    ordinals = [ordinal for ordinal, _ in blocks]
    if len(ordinals) != len(set(ordinals)):
        raise TraceNormalizationError("TLC text contains duplicate state ordinals")
    if ordinals != sorted(ordinals):
        raise TraceNormalizationError("TLC text state ordinals are not increasing")
    return [_parse_state_body(ordinal, body) for ordinal, body in blocks]


def normalize_trace(path: Path) -> NormalizedTrace:
    raw = _read(path)
    stripped = raw.lstrip()
    if stripped.startswith((b"{", b"[")):
        try:
            document = json.loads(raw)
        except json.JSONDecodeError as exc:
            raise TraceNormalizationError(
                f"{path}: invalid JSON at line {exc.lineno}, column "
                f"{exc.colno}: {exc.msg}"
            ) from exc
        states = normalize_json_document(document)
        source_format = "tlc-json"
    else:
        text = raw.decode("utf-8", errors="replace")
        states = normalize_tlc_text(text)
        source_format = "tlc-text"
    return NormalizedTrace(states, source_format, _sha256(raw))


def write_normalized(trace: NormalizedTrace, output: Path) -> None:
    try:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(
            json.dumps(trace.states, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    except OSError as exc:
        raise TraceNormalizationError(f"cannot write {output}: {exc}") from exc


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--metadata",
        type=Path,
        help="optional JSON metadata output with source format/hash/state count",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    try:
        trace = normalize_trace(args.input)
        write_normalized(trace, args.output)
        if args.metadata:
            args.metadata.parent.mkdir(parents=True, exist_ok=True)
            args.metadata.write_text(
                json.dumps(
                    {
                        "source": str(args.input),
                        "source_format": trace.source_format,
                        "source_sha256": trace.source_sha256,
                        "state_count": len(trace.states),
                        "normalized": str(args.output),
                    },
                    indent=2,
                    sort_keys=True,
                )
                + "\n",
                encoding="utf-8",
            )
        return 0
    except TraceNormalizationError as exc:
        print(f"trace normalization failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
