#!/usr/bin/env python3
"""Canonical proofless wrapper for mixed-version compatibility checks."""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

from run_tlc_only_checks_v4 import run_manifest


def main(argv: list[str] | None = None) -> int:
    return run_manifest(
        argv,
        required_manifest_name="compatibility-formal-checks-v1.json",
        static_check_name="compatibility_static_check.py",
        description=(
            "Run the exact mixed-version compatibility TLC matrix on both "
            "pinned toolchains without requiring irrelevant TLAPS inventory."
        ),
    )


if __name__ == "__main__":
    raise SystemExit(main())
