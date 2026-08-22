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
import inspect
import json
import textwrap
from collections.abc import Iterator, Sequence
from pathlib import Path
from typing import Any

import run_scenario as _accepted


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
            return list(iter(self))[index]
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
            start, stop, step = index.indices(len(self))
            if start == 1 and stop == max(1, len(self) - 1) and step == 1:
                return _TimelineRows(self)
            return list(iter(self))[index]
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
    """Compile the accepted validator after verifying its sole storage rewrite."""
    source = textwrap.dedent(inspect.getsource(_accepted.validate_experiment_jsonl))
    tree = ast.parse(source)
    function = tree.body[0]
    if not isinstance(function, (ast.FunctionDef, ast.AsyncFunctionDef)):
        raise RuntimeError("accepted validator source has an unexpected shape")
    replacements = 0
    for statement in function.body:
        if (
            isinstance(statement, ast.Assign)
            and len(statement.targets) == 1
            and isinstance(statement.targets[0], ast.Name)
            and statement.targets[0].id == "rows"
        ):
            statement.value = ast.Call(
                func=ast.Name(id="_StreamingRows", ctx=ast.Load()),
                args=[ast.Name(id="path", ctx=ast.Load())],
                keywords=[],
            )
            replacements += 1
    if replacements != 1:
        raise RuntimeError(
            "accepted validator no longer has exactly one eager rows assignment"
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
