#!/usr/bin/env python3
"""Run the exact assignment-fence TLC phase without claiming the TLAPS gate.

The authoritative manifest remains proof-bearing.  This wrapper retains and
validates its exact proof inventory, executes only the fourteen TLC rows, and
records explicitly that the proof was not run.  Final acceptance must use
``run_assignment_fence_checks_v4.py`` and cannot skip proofs.
"""

from __future__ import annotations

import sys
from typing import Sequence

sys.dont_write_bytecode = True

from run_tlc_only_checks_v4 import run_manifest

PROOFS = [
    {
        "id": "core-tlaps-proof",
        "file": "AssignmentFenceCoreProof.tla",
        "expected": "pass",
        "timeout_seconds": 7200,
    }
]


def main(argv: Sequence[str] | None = None) -> int:
    return run_manifest(
        argv,
        required_manifest_name="assignment-fence-formal-checks-v1.json",
        static_check_name="assignment_fence_static_check.py",
        description=__doc__ or "assignment-fence TLC phase",
        allowed_proofs=PROOFS,
        mode="assignment-fence-tlc-phase",
    )


if __name__ == "__main__":
    raise SystemExit(main())
