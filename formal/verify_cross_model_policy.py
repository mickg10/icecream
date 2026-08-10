#!/usr/bin/env python3
"""Verify policy/guarantee consistency across formal evidence branches."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path


class PolicyError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise PolicyError(message)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compact(text: str) -> str:
    return re.sub(r"\s+", " ", text).strip()


def load(path: Path) -> str:
    require(path.is_file(), f"missing {path}")
    return path.read_text(encoding="utf-8")


def verify(consumer: Path, compatibility: Path) -> dict:
    theory_path = consumer / "formal" / "ASSIGNMENT_FENCE_THEORY.md"
    theory = load(theory_path)
    landing_path = consumer / "formal" / "UPSTREAM_LANDING_PLAN.md"
    landing = load(landing_path)
    model_path = compatibility / "formal" / "MixedVersionCompatibility.tla"
    model = load(model_path)
    handoff_path = compatibility / "formal" / "COMPATIBILITY_FINAL_HANDOFF.md"
    handoff = load(handoff_path)
    manifest_path = compatibility / "formal" / "compatibility-formal-checks-v1.json"
    manifest = json.loads(load(manifest_path))

    normalized = compact(model)
    expected_policy = compact(
        '''ExpectedPolicy(c, w) ==
           IF WorkerVersion[w] = OldVersion
           THEN "Legacy"
           ELSE IF ClientVersion[c] = OldVersion
                THEN "FencedLegacy"
                ELSE "Token"'''
    )
    require(expected_policy in normalized, "compatibility ExpectedPolicy table drifted")

    guarantee_fragments = (
        'p = "Legacy" -> "LegacyOnly"',
        'p = "FencedLegacy" -> "EpochScoped"',
        'p = "Token" -> "ExactRestart"',
    )
    for fragment in guarantee_fragments:
        require(fragment in normalized, f"compatibility guarantee drift: {fragment}")
    require(
        compact('IdentityOf(p) == IF p = "Token" THEN "FullToken" ELSE "Wire"')
        in normalized,
        "compatibility identity policy drifted",
    )

    required_properties = (
        "PerAssignmentPolicyCorrect ==",
        "GuaranteeMatchesPolicy ==",
        "ClaimIdentityMatchesPolicy ==",
        "OldWorkerSeesOnlyOldVocabulary ==",
        "OldClientSeesNoTokenField ==",
        "NoGlobalCapabilityLeak ==",
        "TokenRejectsStaleRestartClaim ==",
        "OldTraceRefinement ==",
        "PolicyChosenBeforeDispatch ==",
    )
    for property_name in required_properties:
        require(property_name in model, f"compatibility property absent: {property_name}")

    require(isinstance(manifest, dict) and manifest.get("schema") == 1, "manifest schema mismatch")
    rows = manifest.get("checks")
    require(isinstance(rows, list) and len(rows) == 31, "compatibility matrix must have 31 rows")
    by_id = {row.get("id"): row for row in rows if isinstance(row, dict)}
    required_rows = {
        "compat-old-f-old-c",
        "compat-new-f-old-c",
        "compat-new-f-new-c",
        "compat-old-f-new-c",
        "compat-mixed-f-mixed-c",
        "compat-new-f-mixed-c",
        "compat-legacy-restart-limitation-witness",
        "compat-fenced-restart-limitation-witness",
        "compat-token-restart-reject-witness",
        "compat-token-old-client-mutant",
        "compat-global-capability-mutant",
        "compat-policy-after-dispatch-mutant",
    }
    require(required_rows <= by_id.keys(), f"compatibility rows missing: {sorted(required_rows-by_id.keys())}")
    for check_id in (
        "compat-legacy-restart-limitation-witness",
        "compat-fenced-restart-limitation-witness",
        "compat-token-restart-reject-witness",
    ):
        row = by_id[check_id]
        require(row.get("expected") == "counterexample", f"{check_id}: limitation/witness verdict drifted")
        require(isinstance(row.get("trace_manifest"), str), f"{check_id}: trace witness missing")

    policy_documents = compact(theory + "\n" + landing)
    policy_patterns = (
        r"old\s+F[^.]*Legacy",
        r"new\s+F\s*(?:\+|plus)\s*old\s+C[^.]*FencedLegacy",
        r"new\s+F\s*(?:\+|plus)\s*new\s+C[^.]*Token",
        r"Legacy[^.]{0,200}(?:frozen|old behavior)",
        r"FencedLegacy[^.]{0,240}(?:worker-side|live epoch|epoch-scoped)",
        r"Token[^.]{0,240}(?:stale|prior-epoch|restart)",
        r"PipelinedEnforcing[^.]{0,300}(?:negotiated|new peer|new-S)",
    )
    for pattern in policy_patterns:
        require(
            re.search(pattern, policy_documents, re.IGNORECASE) is not None,
            f"assignment-fence policy documentation drift: {pattern}",
        )

    topology_fragments = (
        "S'FC",
        "S'F'C",
        "S'F'C'",
        "S'FC'",
        "S'F[F']C[C']",
        "S'F'[CC']",
    )
    for topology in topology_fragments:
        require(topology in handoff, f"compatibility handoff topology absent: {topology}")

    codec_path = compatibility / "formal" / "compatibility_codec_fixture.py"
    codec_test_path = compatibility / "formal" / "compatibility_codec_fixture_test.py"
    load(codec_path)
    load(codec_test_path)
    require(
        "not the production Icecream codec" in load(codec_path),
        "generated codec fixture no longer states its non-production boundary",
    )

    return {
        "schema": 1,
        "status": "PASS",
        "policy": {
            "old_f": "Legacy",
            "new_f_old_c": "FencedLegacy",
            "new_f_new_c": "Token",
        },
        "consumer_theory": {
            "path": str(theory_path),
            "sha256": sha256(theory_path),
        },
        "upstream_landing_plan": {
            "path": str(landing_path),
            "sha256": sha256(landing_path),
        },
        "compatibility": {
            "model_sha256": sha256(model_path),
            "manifest_sha256": sha256(manifest_path),
            "handoff_sha256": sha256(handoff_path),
            "codec_sha256": sha256(codec_path),
            "codec_test_sha256": sha256(codec_test_path),
            "row_count": len(rows),
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--consumer-repo", default=".", type=Path)
    parser.add_argument("--compatibility-worktree", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        report = verify(
            args.consumer_repo.resolve(),
            args.compatibility_worktree.resolve(),
        )
    except (PolicyError, OSError, json.JSONDecodeError) as error:
        print(f"cross-model policy rejected: {error}")
        return 1
    rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
