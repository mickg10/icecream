#!/usr/bin/env python3
"""Authoritative entry point for the mixed-version compatibility matrix."""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

from run_compatibility_checks_v4 import main


if __name__ == "__main__":
    raise SystemExit(main())
