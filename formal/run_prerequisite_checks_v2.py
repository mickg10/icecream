#!/usr/bin/env python3
"""Authoritative entry point for the protocol-neutral prerequisite matrix.

The domain contract is revision 2; execution uses the shared generation-4
proofless TLC orchestration. The delegated wrapper rejects any manifest other
than prerequisite-formal-checks-v4.json.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

from run_prerequisite_checks_v4 import main


if __name__ == "__main__":
    raise SystemExit(main())
