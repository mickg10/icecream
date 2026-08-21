#!/usr/bin/env python3
"""Reproduce or check the selected exact AssignmentFenceCore proof.

The strategy file records only proof decomposition/backend selection.  All
strategies prove the same existing theorem from the same existing model.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

import generate_assignment_fence_core_proof_v2 as action_generator
import generate_assignment_fence_core_proof_candidates as candidate_generator

HERE = Path(__file__).resolve().parent
PROOF = HERE / "AssignmentFenceCoreProof.tla"
STRATEGY = HERE / "assignment-fence-proof-strategy.json"


def canonicalize(text: str) -> str:
    result, count = re.subn(
        r"(?m)^-+ MODULE [A-Za-z_][A-Za-z0-9_]* -+$",
        "-------------------- MODULE AssignmentFenceCoreProof --------------------",
        text,
        count=1,
    )
    if count != 1:
        raise SystemExit("proof candidate has no canonicalizable module header")
    return result


def selected_strategy() -> str:
    if not STRATEGY.exists():
        return "action"
    document = json.loads(STRATEGY.read_text(encoding="utf-8"))
    if document.get("schema") != 1:
        raise SystemExit("unsupported proof strategy schema")
    strategy = document.get("strategy")
    allowed = {
        "action",
        "conjunct-smt",
        "conjunct-zenon",
        "conjunct-smt-zenon",
    }
    if strategy not in allowed:
        raise SystemExit(f"unsupported proof strategy: {strategy!r}")
    return strategy


def generated(strategy: str) -> str:
    if strategy == "action":
        return action_generator.generated_text()
    return canonicalize(
        candidate_generator.generate_conjunct_candidate(strategy)
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--output", type=Path, default=PROOF)
    args = parser.parse_args()

    strategy = selected_strategy()
    text = generated(strategy)
    if args.check:
        existing = args.output.read_text(encoding="utf-8")
        if existing != text:
            raise SystemExit(
                f"canonical proof does not reproduce selected strategy {strategy}"
            )
        print(f"PASS: canonical proof reproduces strategy {strategy}")
        return 0
    args.output.write_text(text, encoding="utf-8")
    print(f"wrote {args.output} from strategy {strategy}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
