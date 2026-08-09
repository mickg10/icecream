#!/usr/bin/env python3
"""Apply the seven intended compatibility parenthesizations exactly once.

This is a temporary branch-repair helper. It is line based rather than
substring based, so an already-correct parenthesized target cannot also match
its source. Every mapping must have exactly one source or exactly one target.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Sequence

sys.dont_write_bytecode = True

LINE_REPLACEMENTS = (
    (
        "/\\ seenLegacy' = seenLegacy \\/ expectedPolicy = \"Legacy\"",
        "/\\ seenLegacy' = (seenLegacy \\/ expectedPolicy = \"Legacy\")",
    ),
    (
        "/\\ seenFenced' = seenFenced \\/ expectedPolicy = \"FencedLegacy\"",
        "/\\ seenFenced' = (seenFenced \\/ expectedPolicy = \"FencedLegacy\")",
    ),
    (
        "/\\ seenToken' = seenToken \\/ expectedPolicy = \"Token\"",
        "/\\ seenToken' = (seenToken \\/ expectedPolicy = \"Token\")",
    ),
    (
        "legacyRestartAmbiguous \\/ policy = \"Legacy\"",
        "(legacyRestartAmbiguous \\/ policy = \"Legacy\")",
    ),
    (
        "fencedRestartAmbiguous \\/ policy = \"FencedLegacy\"",
        "(fencedRestartAmbiguous \\/ policy = \"FencedLegacy\")",
    ),
    (
        "NoMixedLegacyToken == ~(seenLegacy /\\ seenToken)",
        "NoMixedLegacyToken == ~((seenLegacy /\\ seenToken))",
    ),
    (
        "NoMixedFencedToken == ~(seenFenced /\\ seenToken)",
        "NoMixedFencedToken == ~((seenFenced /\\ seenToken))",
    ),
)


class RepairError(RuntimeError):
    """The generated source no longer matches the reviewed repair contract."""


def apply(path: Path) -> bool:
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines()
    changed = False
    for source, target in LINE_REPLACEMENTS:
        source_indices = [
            index for index, line in enumerate(lines) if line.strip() == source
        ]
        target_indices = [
            index for index, line in enumerate(lines) if line.strip() == target
        ]
        if len(source_indices) == 1 and len(target_indices) == 0:
            index = source_indices[0]
            indent = lines[index][: len(lines[index]) - len(lines[index].lstrip())]
            lines[index] = indent + target
            changed = True
        elif len(source_indices) == 0 and len(target_indices) == 1:
            continue
        else:
            raise RepairError(
                f"unexpected cardinality for {source!r}: "
                f"source={len(source_indices)}, target={len(target_indices)}"
            )
    if changed:
        trailing_newline = "\n" if text.endswith("\n") else ""
        path.write_text("\n".join(lines) + trailing_newline, encoding="utf-8")
    return changed


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "path",
        nargs="?",
        type=Path,
        default=Path(__file__).with_name("MixedVersionCompatibility.tla"),
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        changed = apply(args.path)
        print("changed" if changed else "already-correct")
        return 0
    except (OSError, RepairError) as exc:
        print(f"compatibility precedence repair failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
