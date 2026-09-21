#!/usr/bin/env python3
"""Command-line entry point for the S5 statistical contract verifier."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from s5_statistics import ContractError, is_canonical_jsonl, parse_canonical_bytes, verify_preregistered


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Verify an immutable S5 ZSTD_TU evidence document")
    parser.add_argument("evidence", type=Path, help="JSON or JSONL evidence file or evidence document")
    parser.add_argument("--preregistration", type=Path, required=True, help="independent canonical preregistration JSON")
    parser.add_argument("--preregistration-sha256", required=True, help="suite-supplied SHA-256 of complete canonical preregistration file bytes (including final LF)")
    parser.add_argument("--evidence-root", type=Path, required=True, help="descriptor-relative authenticated artifact root")
    parser.add_argument("-o", "--output", type=Path, help="write the deterministic JSON result here")
    args = parser.parse_args(argv)
    try:
        prereg_raw = args.preregistration.read_bytes()
        evidence_raw = args.evidence.read_bytes()
        prereg = parse_canonical_bytes(prereg_raw)
        evidence = parse_canonical_bytes(evidence_raw, jsonl=is_canonical_jsonl(evidence_raw))
        result = verify_preregistered(prereg, evidence,
                                      expected_digest=args.preregistration_sha256,
                                      evidence_root=args.evidence_root)
    except OSError as exc:
        parser.error(str(exc))
    except (ContractError, TypeError, ValueError, UnicodeError):
        result = {"valid": False, "decision": "INCONCLUSIVE", "issues": ["canonical_input_invalid"]}
    output = json.dumps(result, ensure_ascii=True, sort_keys=True, separators=(",", ":")) + "\n"
    if args.output:
        tmp = args.output.with_name(args.output.name + ".tmp")
        with tmp.open("w", encoding="utf-8") as handle:
            handle.write(output)
            handle.flush()
            import os
            os.fsync(handle.fileno())
        tmp.replace(args.output)
    else:
        sys.stdout.write(output)
    # Invalid/incomplete evidence is intentionally non-zero; RED and
    # INCONCLUSIVE are usable decisions on otherwise valid evidence.
    return 0 if result.get("valid") is True else 2


if __name__ == "__main__":
    raise SystemExit(main())
