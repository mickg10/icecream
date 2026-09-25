#!/usr/bin/env python3
"""Reject skipped, failed, or incomplete opt-in gate logs."""
from __future__ import annotations

import sys
from pathlib import Path


def main(argv: list[str]) -> int:
    if len(argv) != 5:
        print("usage: gate-result.py EXIT LOG PASS_MARKER EXPECTED_COUNT", file=sys.stderr)
        return 2
    try:
        status = int(argv[1], 10)
        expected = int(argv[4], 10)
        if status < 0 or expected < 1:
            raise ValueError
        log = Path(argv[2]).read_text(encoding="utf-8", errors="replace")
    except (OSError, ValueError) as exc:
        print(f"FAIL: invalid gate result input: {exc}", file=sys.stderr)
        return 2
    if status == 77:
        print("FAIL: gate returned skip status 77; skips are not qualification", file=sys.stderr)
        return 1
    if status != 0:
        print(f"FAIL: gate exited {status}", file=sys.stderr)
        return status if status < 126 else 1
    if "SKIP:" in log:
        print("FAIL: gate log contains a skip marker", file=sys.stderr)
        return 1
    count = log.count(argv[3])
    if count != expected:
        print(f"FAIL: expected {expected} pass markers, found {count}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
