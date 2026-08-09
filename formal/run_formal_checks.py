#!/usr/bin/env python3
"""Canonical entry point for the fail-closed formal acceptance runner."""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

from run_formal_checks_v4 import main


if __name__ == "__main__":
    raise SystemExit(main())
