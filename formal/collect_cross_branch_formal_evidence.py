#!/usr/bin/env python3
"""Collect and verify immutable prerequisite/compatibility formal evidence.

This script resolves moving branch names once, then validates only the resolved
commits and their exact successful workflow runs/artifacts. It does not infer a
formal result from source publication.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any


class EvidenceError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise EvidenceError(message)


def command(
    repo: Path,
    arguments: list[str],
    *,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        arguments,
        cwd=repo,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=check,
        env=os.environ,
    )


def git(repo: Path, *arguments: str, check: bool = True) -> str:
    return command(repo, ["git", *arguments], check=check).stdout


def gh(arguments: list[str]) -> Any:
    completed = subprocess.run(
        ["gh", "api", *arguments],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
        env=os.environ,
    )
    return json.loads(completed.stdout)


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


@dataclass(frozen=True)
class Domain:
    name: str
    branch: str
    workflow_name: str
    workflow_path: str
    manifest_path: str
    handoff_path: str
    check_count: int
    artifact_prefixes: tuple[str, ...]


DOMAINS = (
    Domain(
        name="prerequisite",
        branch="bigoracle/protocol-prerequisites-formal",
        workflow_name="prerequisite-formal",
        workflow_path=".github/workflows/prerequisite-formal.yml",
        manifest_path="formal/prerequisite-formal-checks-v4.json",
        handoff_path="formal/PREREQUISITES_FINAL_HANDOFF.md",
        check_count=24,
        artifact_prefixes=(
            "prerequisites-allocator-",
            "prerequisites-lifecycle-",
            "prerequisites-usecs-",
            "prerequisites-fsession-",
        ),
    ),
    Domain(
        name="compatibility",
        branch="bigoracle/compatibility-formal",
        workflow_name="compatibility-formal",
        workflow_path=".github/workflows/compatibility-formal.yml",
        manifest_path="formal/compatibility-formal-checks-v1.json",
        handoff_path="formal/COMPATIBILITY_FINAL_HANDOFF.md",
        check_count=31,
        artifact_prefixes=(
            "compatibility-fixed-",
            "compatibility-witness-a-",
            "compatibility-witness-b-",
            "compatibility-mutants-",
        ),
    ),
)


def show(repo: Path, revision: str, path: str) -> str:
    completed = command(
        repo,
        ["git", "show", f"{revision}:{path}"],
        check=False,
    )
    require(
        completed.returncode == 0,
        f"{revision}:{path}: missing: {completed.stderr.strip()}",
    )
    return completed.stdout


def resolve_branch(repo: Path, branch: str) -> str:
    git(
        repo,
        "fetch",
        "--no-tags",
        "origin",
        f"refs/heads/{branch}:refs/remotes/origin/{branch}",
    )
    revision = git(repo, "rev-parse", f"refs/remotes/origin/{branch}").strip()
    require(len(revision) == 40, f"{branch}: invalid resolved revision")
    return revision


def validate_workflow(text: str, domain: Domain) -> None:
    require(
        "contents: read" in text,
        f"{domain.name}: workflow does not grant read-only contents",
    )
    require(
        "contents: write" not in text,
        f"{domain.name}: workflow is write-enabled",
    )
    require(
        "actions/upload-artifact@v4" in text,
        f"{domain.name}: workflow retains no artifact",
    )
    require(
        "workflow_dispatch:" in text,
        f"{domain.name}: workflow has no explicit rerun entry",
    )
    for forbidden in (
        "precedence-repair:",
        "fix-compat-precedence",
        "git push origin",
        "git commit -m",
    ):
        require(
            forbidden not in text,
            f"{domain.name}: workflow still contains repair behavior {forbidden!r}",
        )


def validate_manifest(document: Any, domain: Domain) -> dict[str, Any]:
    require(isinstance(document, dict), f"{domain.name}: manifest must be an object")
    require(document.get("schema") == 1, f"{domain.name}: manifest schema mismatch")
    checks = document.get("checks")
    require(isinstance(checks, list), f"{domain.name}: checks must be an array")
    require(
        len(checks) == domain.check_count,
        f"{domain.name}: expected {domain.check_count} checks, found {len(checks)}",
    )
    require(document.get("proofs") == [], f"{domain.name}: proofless domain gained proof rows")
    ids = [row.get("id") if isinstance(row, dict) else None for row in checks]
    require(all(isinstance(item, str) and item for item in ids), f"{domain.name}: invalid check ID")
    require(len(ids) == len(set(ids)), f"{domain.name}: duplicate check IDs")
    for row in checks:
        require(row.get("workers") == 1, f"{domain.name}:{row.get('id')}: one worker required")
        require(
            row.get("toolchains") == ["stable", "differential"],
            f"{domain.name}:{row.get('id')}: toolchain order mismatch",
        )
        require(
            row.get("expected") in {"pass", "counterexample"},
            f"{domain.name}:{row.get('id')}: invalid expected result",
        )
    if domain.name == "prerequisite":
        segments = (
            (0, 5, "allocator-"),
            (5, 11, "lifecycle-"),
            (11, 18, "usecs-"),
            (18, 24, "fsession-"),
        )
        for start, end, prefix in segments:
            require(
                all(item.startswith(prefix) for item in ids[start:end]),
                f"prerequisite: row segment {start}:{end} is not {prefix}",
            )
    else:
        require(
            all(not item.endswith("-mutant") for item in ids[:22]),
            "compatibility: fixed/witness prefix contains mutant row",
        )
        require(
            all(item.endswith("-mutant") for item in ids[22:]),
            "compatibility: final nine rows are not all mutants",
        )
    return {
        "check_count": len(checks),
        "check_ids_sha256": sha256_text("\n".join(ids) + "\n"),
    }


def successful_run(repository: str, revision: str, domain: Domain) -> dict[str, Any]:
    document = gh(
        [
            "--method",
            "GET",
            f"repos/{repository}/actions/runs",
            "-f",
            f"head_sha={revision}",
            "-f",
            "status=completed",
            "-f",
            "per_page=100",
        ]
    )
    runs = [
        run
        for run in document.get("workflow_runs", [])
        if run.get("head_sha") == revision
        and run.get("name") == domain.workflow_name
        and run.get("status") == "completed"
        and run.get("conclusion") == "success"
    ]
    require(runs, f"{domain.name}: no successful workflow at exact SHA {revision}")
    runs.sort(key=lambda run: (run.get("created_at") or "", int(run["id"])), reverse=True)
    return runs[0]


def validate_jobs(repository: str, run_id: int, domain: Domain) -> list[dict[str, Any]]:
    document = gh(
        ["--method", "GET", f"repos/{repository}/actions/runs/{run_id}/jobs", "-f", "per_page=100"]
    )
    jobs = document.get("jobs", [])
    require(jobs, f"{domain.name}: successful run has no jobs")
    bad = [
        {"id": job.get("id"), "name": job.get("name"), "conclusion": job.get("conclusion")}
        for job in jobs
        if job.get("conclusion") != "success"
    ]
    require(not bad, f"{domain.name}: non-success jobs in successful run: {bad}")
    return [
        {
            "id": job["id"],
            "name": job["name"],
            "conclusion": job["conclusion"],
        }
        for job in jobs
    ]


def validate_artifacts(
    repository: str,
    run_id: int,
    domain: Domain,
) -> list[dict[str, Any]]:
    document = gh(
        ["--method", "GET", f"repos/{repository}/actions/runs/{run_id}/artifacts", "-f", "per_page=100"]
    )
    artifacts = document.get("artifacts", [])
    selected: list[dict[str, Any]] = []
    for prefix in domain.artifact_prefixes:
        matches = [
            artifact
            for artifact in artifacts
            if artifact.get("name", "").startswith(prefix)
            and not artifact.get("expired", False)
        ]
        require(
            len(matches) == 1,
            f"{domain.name}: expected one live artifact with prefix {prefix!r}, "
            f"found {[item.get('name') for item in matches]}",
        )
        artifact = matches[0]
        digest_value = artifact.get("digest")
        require(
            isinstance(digest_value, str) and digest_value.startswith("sha256:"),
            f"{domain.name}:{artifact.get('name')}: SHA-256 artifact digest required",
        )
        selected.append(
            {
                "id": artifact["id"],
                "name": artifact["name"],
                "size_in_bytes": artifact["size_in_bytes"],
                "digest": digest_value,
                "expires_at": artifact.get("expires_at"),
            }
        )
    return selected


def collect(repo: Path, repository: str, domain: Domain) -> dict[str, Any]:
    revision = resolve_branch(repo, domain.branch)
    workflow_text = show(repo, revision, domain.workflow_path)
    validate_workflow(workflow_text, domain)
    manifest_text = show(repo, revision, domain.manifest_path)
    manifest_report = validate_manifest(json.loads(manifest_text), domain)
    handoff_text = show(repo, revision, domain.handoff_path)
    require(
        domain.branch in handoff_text,
        f"{domain.name}: final handoff does not name its branch",
    )
    require(
        str(domain.check_count) in handoff_text,
        f"{domain.name}: final handoff does not state its row count",
    )

    run = successful_run(repository, revision, domain)
    run_id = int(run["id"])
    jobs = validate_jobs(repository, run_id, domain)
    artifacts = validate_artifacts(repository, run_id, domain)
    return {
        "domain": domain.name,
        "branch": domain.branch,
        "revision": revision,
        "workflow": {
            "name": domain.workflow_name,
            "path": domain.workflow_path,
            "sha256": sha256_text(workflow_text),
            "run_id": run_id,
            "html_url": run.get("html_url"),
            "created_at": run.get("created_at"),
            "updated_at": run.get("updated_at"),
            "jobs": jobs,
        },
        "manifest": {
            "path": domain.manifest_path,
            "sha256": sha256_text(manifest_text),
            **manifest_report,
        },
        "handoff": {
            "path": domain.handoff_path,
            "sha256": sha256_text(handoff_text),
        },
        "artifacts": artifacts,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", default=".", type=Path)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    try:
        evidence = [
            collect(args.repo.resolve(), args.repository, domain)
            for domain in DOMAINS
        ]
    except (
        EvidenceError,
        OSError,
        json.JSONDecodeError,
        subprocess.CalledProcessError,
    ) as error:
        print(f"cross-branch formal evidence rejected: {error}")
        return 1

    document = {
        "schema": 1,
        "repository": args.repository,
        "domains": evidence,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(document, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
