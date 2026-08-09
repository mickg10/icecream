#!/usr/bin/env python3
"""Canonical proof-bearing wrapper for the assignment-fence acceptance gate."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path
from typing import Sequence

sys.dont_write_bytecode = True

import run_formal_checks_v4 as v4

REQUIRED_MANIFEST = "assignment-fence-formal-checks-v1.json"
STATIC_CHECK = "assignment_fence_static_check.py"


def main(argv: Sequence[str] | None = None) -> int:
    arguments = list(argv) if argv is not None else sys.argv[1:]
    try:
        parsed = v4.v2.build_parser().parse_args(arguments)
        repo = parsed.repo.resolve()
        formal_dir = (parsed.formal_dir or repo / "formal").resolve()
        manifest = parsed.manifest.resolve()
        expected = (formal_dir / REQUIRED_MANIFEST).resolve()
        if manifest != expected:
            raise v4.v2.FormalRunError(f"only {expected} is authoritative")
        if parsed.skip_proofs:
            raise v4.v2.FormalRunError(
                "the proof-bearing assignment-fence gate forbids --skip-proofs"
            )
        script = formal_dir / STATIC_CHECK
        result = subprocess.run(
            [
                sys.executable,
                str(script),
                "--manifest",
                str(manifest),
                "--repo",
                str(repo),
                "--formal-dir",
                str(formal_dir),
            ],
            cwd=formal_dir,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=300,
            check=False,
            env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
        )
        if result.returncode != 0:
            raise v4.v2.FormalRunError(
                f"assignment-fence static preflight failed:\n{result.stdout}"
            )
        print(result.stdout, end="")
    except (v4.v2.FormalRunError, OSError, subprocess.SubprocessError) as exc:
        print(f"formal acceptance failed: {exc}", file=sys.stderr)
        return 1
    return v4.main(arguments)


if __name__ == "__main__":
    raise SystemExit(main())
