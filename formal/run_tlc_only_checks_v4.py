#!/usr/bin/env python3
"""Run a proofless formal manifest with the pinned generation-4 TLC path.

This module is deliberately narrower than ``run_formal_checks_v4.py``.  It is
for prerequisite and compatibility matrices whose manifest contains no TLAPS
proof rows.  Such a run must not require, probe, or accept placeholder TLAPM
and backend inventory.

The underlying TLC, trace normalization, manifest discrimination, clean-tree,
positive-RSS, and semantic differential checks are exactly the generation-4
implementation.  This file changes orchestration only.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import sys
from pathlib import Path
from typing import Any, Mapping, Sequence

sys.dont_write_bytecode = True

import run_formal_checks_v4 as v4

v2 = v4.v2


class TlcOnlyContractError(v2.FormalRunError):
    """A proofless-wrapper contract was violated."""


def _inside(path: Path, directory: Path) -> bool:
    try:
        path.relative_to(directory)
        return True
    except ValueError:
        return False


def _build_parser(description: str) -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument(
        "--repo",
        type=Path,
        default=Path(__file__).resolve().parents[1],
    )
    parser.add_argument("--formal-dir", type=Path)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--expected-git-sha", required=True)
    parser.add_argument("--java", default="java")
    parser.add_argument("--stable-jar", type=Path, required=True)
    parser.add_argument("--stable-sha256", required=True)
    parser.add_argument("--differential-jar", type=Path, required=True)
    parser.add_argument("--differential-sha256", required=True)
    parser.add_argument(
        "--time-bin",
        type=Path,
        default=Path("/usr/bin/time"),
    )
    parser.add_argument(
        "--only",
        action="append",
        default=[],
        metavar="REGEX",
        help="run only check ids matching a full regular expression",
    )
    return parser


def _run_static_check(
    *,
    script: Path,
    manifest: Path,
    repo: Path,
    formal_dir: Path,
    artifacts: Path,
) -> dict[str, Any]:
    if not script.is_file():
        raise TlcOnlyContractError(f"missing static-check script: {script}")
    environment = dict(os.environ)
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    result = v2.run_capture(
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
        formal_dir,
        timeout=300,
        env=environment,
    )
    log_path = artifacts / "preflight" / f"{script.name}.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text(result["output"], encoding="utf-8")
    if result["returncode"] != 0:
        raise TlcOnlyContractError(
            f"static preflight failed: {script.name}; see {log_path}"
        )
    return {
        "script": script.name,
        "script_sha256": v2.sha256_file(script),
        "command": result["command"],
        "returncode": result["returncode"],
        "elapsed_seconds": result["elapsed_seconds"],
        "log_sha256": v2.sha256_file(log_path),
    }


def _validate_proofless_manifest(document: Any) -> dict[str, Any]:
    manifest = v2.validate_manifest(document)
    proofs = manifest.get("proofs", [])
    if proofs != []:
        raise TlcOnlyContractError(
            "TLC-only orchestration requires an explicit empty proofs array"
        )
    for check in manifest["checks"]:
        if check.get("toolchains", ["stable", "differential"]) != [
            "stable",
            "differential",
        ]:
            raise TlcOnlyContractError(
                f"{check['id']}: TLC-only acceptance requires stable then "
                "differential toolchains"
            )
    return manifest


def _selected_checks(
    checks: Sequence[Mapping[str, Any]], patterns: Sequence[str]
) -> list[Mapping[str, Any]]:
    if not patterns:
        return list(checks)
    selected = [
        check
        for check in checks
        if any(re.fullmatch(pattern, str(check["id"])) for pattern in patterns)
    ]
    if not selected:
        raise TlcOnlyContractError(
            f"--only patterns selected no checks: {list(patterns)}"
        )
    return selected


def _write_failure(artifacts: Path | None, message: str) -> None:
    if artifacts is None or not artifacts.exists():
        return
    try:
        v2.write_json(
            artifacts / "failure.json",
            {
                "schema": v2.SCHEMA_VERSION,
                "status": "FAIL",
                "finished_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
                "error": message,
            },
        )
    except v2.FormalRunError:
        pass


def run_manifest(
    argv: Sequence[str] | None,
    *,
    required_manifest_name: str,
    static_check_name: str,
    description: str,
) -> int:
    """Run one exact proofless manifest and reject manifest substitution."""
    args = _build_parser(description).parse_args(argv)
    artifacts: Path | None = None
    try:
        repo = args.repo.resolve()
        formal_dir = (args.formal_dir or repo / "formal").resolve()
        manifest_path = args.manifest.resolve()
        expected_manifest = (formal_dir / required_manifest_name).resolve()
        static_check = (formal_dir / static_check_name).resolve()
        artifacts = args.artifacts.resolve()

        if manifest_path != expected_manifest:
            raise TlcOnlyContractError(
                f"this wrapper accepts only {expected_manifest}; got {manifest_path}"
            )

        # Identity and cleanliness are established before the external artifact
        # directory is created and before any repository module is imported by
        # a subprocess.
        git = v2.git_identity(repo, args.expected_git_sha)
        if _inside(artifacts, repo):
            raise TlcOnlyContractError(
                f"artifact directory must be outside the checkout: {artifacts}"
            )
        if artifacts.exists() and any(artifacts.iterdir()):
            raise TlcOnlyContractError(
                f"artifact directory must be absent or empty: {artifacts}"
            )
        artifacts.mkdir(parents=True, exist_ok=True)

        if not args.time_bin.is_file():
            raise TlcOnlyContractError(
                f"GNU time is required for positive peak-RSS evidence: "
                f"{args.time_bin}"
            )

        manifest = _validate_proofless_manifest(v2.load_json(manifest_path))
        static_result = _run_static_check(
            script=static_check,
            manifest=manifest_path,
            repo=repo,
            formal_dir=formal_dir,
            artifacts=artifacts,
        )
        self_tests = v4.run_python_self_tests(formal_dir, artifacts)
        git_after_tests = v2.git_identity(repo, args.expected_git_sha)

        stable_sha = v2.exact_sha(
            args.stable_jar.resolve(), args.stable_sha256, "stable TLC"
        )
        differential_sha = v2.exact_sha(
            args.differential_jar.resolve(),
            args.differential_sha256,
            "differential TLC",
        )
        if stable_sha == differential_sha:
            raise TlcOnlyContractError(
                "stable and differential TLC jars must have different hashes"
            )
        stable = v2.preflight_toolchain(
            name="stable",
            jar=args.stable_jar.resolve(),
            sha256=stable_sha,
            java=args.java,
            repo=repo,
        )
        differential = v2.preflight_toolchain(
            name="differential",
            jar=args.differential_jar.resolve(),
            sha256=differential_sha,
            java=args.java,
            repo=repo,
        )
        java_version = v2.run_capture([args.java, "-version"], repo)
        if java_version["returncode"] != 0:
            raise TlcOnlyContractError("Java version command failed")

        run_metadata = {
            "schema": v2.SCHEMA_VERSION,
            "mode": "tlc-only",
            "started_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
            "git": git_after_tests,
            "manifest": {
                "path": str(manifest_path),
                "sha256": v2.sha256_file(manifest_path),
                "required_name": required_manifest_name,
                "proof_count": 0,
            },
            "static_preflight": static_result,
            "self_tests": self_tests,
            "java": java_version,
            "toolchains": {
                "stable": {
                    "path": str(stable.jar),
                    "sha256": stable.sha256,
                    "help_command": stable.help_command,
                    "help_returncode": stable.help_returncode,
                    "help_output": stable.help_output,
                    "supports_dump_trace": stable.supports_dump_trace,
                    "banner": stable.banner,
                },
                "differential": {
                    "path": str(differential.jar),
                    "sha256": differential.sha256,
                    "help_command": differential.help_command,
                    "help_returncode": differential.help_returncode,
                    "help_output": differential.help_output,
                    "supports_dump_trace": differential.supports_dump_trace,
                    "banner": differential.banner,
                },
            },
            "tlaps": {
                "required": False,
                "reason": "the exact manifest contains zero proof rows",
            },
        }
        v2.write_json(artifacts / "run-metadata.json", run_metadata)

        selected = _selected_checks(manifest["checks"], args.only)
        toolchains = {"stable": stable, "differential": differential}
        results: list[dict[str, Any]] = []
        for check in selected:
            for name in ("stable", "differential"):
                results.append(
                    v2.run_tlc_check(
                        check=check,
                        toolchain=toolchains[name],
                        java=args.java,
                        formal_dir=formal_dir,
                        artifacts=artifacts,
                        time_bin=args.time_bin.resolve(),
                    )
                )

        comparisons = v4.differential_compare(results)
        summary = {
            "schema": v2.SCHEMA_VERSION,
            "status": "PASS",
            "mode": "tlc-only",
            "finished_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
            "git": git_after_tests,
            "selected_check_count": len(selected),
            "tlc_result_count": len(results),
            "comparison_count": len(comparisons),
            "tlc_results": results,
            "toolchain_comparisons": comparisons,
            "tlaps_results": [],
        }
        v2.write_json(artifacts / "summary.json", summary)
        print(json.dumps(summary, indent=2, sort_keys=True))
        return 0
    except v2.FormalRunError as exc:
        _write_failure(artifacts, str(exc))
        print(f"formal acceptance failed: {exc}", file=sys.stderr)
        return 1


__all__ = ["TlcOnlyContractError", "run_manifest"]
