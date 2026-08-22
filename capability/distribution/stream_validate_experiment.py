#!/usr/bin/env python3
"""Bounded-row-storage execution of the accepted R4 validator.

The accepted validator in :mod:`run_scenario` is also the simulator source
named by retained R4 evidence, so changing that file would invalidate existing
experiments.  This module compiles that exact validator body with one narrowly
checked substitution: its eager ``rows`` list becomes a two-pass sequence that
retains only the execution and summary records.  Every schema, identity,
replay, accounting, and evidence check remains the accepted implementation.
"""

from __future__ import annotations

import ast
import hashlib
import inspect
import json
import textwrap
from collections.abc import Iterator, Sequence
from pathlib import Path
from typing import Any

import run_scenario as _accepted


# SHA-256 of the dedented source returned for the accepted R4 validator at
# 98e07ae70b34cf5927df20404c3e34a3018ac8bb.  The streaming adapter is a
# deliberately exact rewrite of that function, not a general source-to-source
# transform.  Refuse every source change until this adapter is reviewed with
# the new authoritative validator.
_ACCEPTED_VALIDATOR_SOURCE_SHA256 = (
    "44bfb76eb5d9ff997afd25780207be38c6a9f4ed25e0e7f132e845dbc75b8bb8"
)

_ACCEPTED_ROWS_EXPRESSION = ast.parse(
    "[json.loads(line) for line in path.read_text().splitlines() if line.strip()]",
    mode="eval",
).body


def _same_ast(left: ast.AST, right: ast.AST) -> bool:
    return ast.dump(left, include_attributes=False) == ast.dump(
        right, include_attributes=False
    )


class _TimelineRows(Sequence[dict[str, object]]):
    """Repeatable view of the timeline records between header and summary."""

    def __init__(self, owner: "_StreamingRows") -> None:
        self._owner = owner

    def __len__(self) -> int:
        return max(0, len(self._owner) - 2)

    def __iter__(self) -> Iterator[dict[str, object]]:
        final_index = len(self._owner) - 1
        for index, row in self._owner.iter_rows():
            if 0 < index < final_index:
                yield row

    def __getitem__(self, index: int | slice) -> Any:
        if isinstance(index, slice):
            raise TypeError("accepted validator does not slice timeline rows")
        normalized = index if index >= 0 else len(self) + index
        if not 0 <= normalized < len(self):
            raise IndexError(index)
        for current, row in enumerate(self):
            if current == normalized:
                return row
        raise IndexError(index)


class _StreamingRows(Sequence[dict[str, object]]):
    """JSONL sequence with bounded row storage and repeatable file passes."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self._count = 0
        self._first: dict[str, object] | None = None
        self._last: dict[str, object] | None = None
        for _index, row in self.iter_rows():
            if self._first is None:
                self._first = row
            self._last = row
            self._count += 1

    def iter_rows(self) -> Iterator[tuple[int, dict[str, object]]]:
        logical_index = 0
        with self.path.open() as source:
            for line in source:
                if not line.strip():
                    continue
                row = json.loads(line)
                yield logical_index, row
                logical_index += 1

    def __len__(self) -> int:
        return self._count

    def __iter__(self) -> Iterator[dict[str, object]]:
        for _index, row in self.iter_rows():
            yield row

    def __getitem__(self, index: int | slice) -> Any:
        if isinstance(index, slice):
            if index.start == 1 and index.stop == -1 and index.step is None:
                return _TimelineRows(self)
            raise TypeError("accepted validator requested an unsupported rows slice")
        normalized = index if index >= 0 else len(self) + index
        if not 0 <= normalized < len(self):
            raise IndexError(index)
        if normalized == 0:
            return self._first
        if normalized == len(self) - 1:
            return self._last
        for current, row in self.iter_rows():
            if current == normalized:
                return row
        raise IndexError(index)


def _compile_streaming_validator():
    """Compile the exact accepted validator with its sole reviewed rewrite."""
    source = textwrap.dedent(inspect.getsource(_accepted.validate_experiment_jsonl))
    tree = ast.parse(source)
    if len(tree.body) != 1:
        raise RuntimeError("accepted validator source has an unexpected module shape")
    function = tree.body[0]
    if (
        not isinstance(function, ast.FunctionDef)
        or function.name != "validate_experiment_jsonl"
    ):
        raise RuntimeError("accepted validator source has an unexpected shape")

    rows_assignments = [
        statement
        for statement in function.body
        if isinstance(statement, ast.Assign)
        and len(statement.targets) == 1
        and isinstance(statement.targets[0], ast.Name)
        and statement.targets[0].id == "rows"
    ]
    if len(rows_assignments) != 1 or not _same_ast(
        rows_assignments[0].value, _ACCEPTED_ROWS_EXPRESSION
    ):
        raise RuntimeError(
            "accepted validator no longer has its exact eager rows initialization"
        )
    source_digest = hashlib.sha256(source.encode("utf-8")).hexdigest()
    if source_digest != _ACCEPTED_VALIDATOR_SOURCE_SHA256:
        raise RuntimeError(
            "accepted validator source fingerprint changed; streaming rewrite refused"
        )

    rows_assignments[0].value = ast.Call(
        func=ast.Name(id="_StreamingRows", ctx=ast.Load()),
        args=[ast.Name(id="path", ctx=ast.Load())],
        keywords=[],
    )
    ast.fix_missing_locations(tree)
    namespace = dict(vars(_accepted))
    namespace["_StreamingRows"] = _StreamingRows
    exec(
        compile(tree, inspect.getsourcefile(_accepted) or "run_scenario.py", "exec"),
        namespace,
    )
    return namespace["validate_experiment_jsonl"]


validate_experiment_jsonl = _compile_streaming_validator()
