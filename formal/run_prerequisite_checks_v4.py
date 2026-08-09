#!/usr/bin/env python3
"""Canonical proofless wrapper for protocol-neutral prerequisite checks."""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

from run_tlc_only_checks_v4 import run_manifest


def main(argv: list[str] | None = None) -> int:
    return run_manifest(
        argv,
        required_manifest_name="prerequisite-formal-checks-v4.json",
        static_check_name="prerequisite_static_check_v2.py",
        description=(
            "Run the exact protocol-neutral prerequisite TLC matrix on both "
            "pinned toolchains without requiring irrelevant TLAPS inventory."
        ),
    )


if __name__ == "__main__":
    raise SystemExit(main())
