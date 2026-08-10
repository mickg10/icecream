#!/usr/bin/env python3
"""Reject any repository-writing workflow in the formal task closure."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

PATTERNS = (
    "assignment-fence*.yml",
    "f2s*.yml",
    "cross-branch*.yml",
)

WRITE_PERMISSION_RE = re.compile(r"(?m)^\s*contents:\s*write\s*$")
PUSH_RE = re.compile(r"(?m)^\s*git\s+push\b")
COMMIT_RE = re.compile(r"(?m)^\s*git\s+commit\b")
CHECKOUT_WRITE_RE = re.compile(r"persist-credentials:\s*true")


class GuardError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise GuardError(message)


def task_workflows(root: Path) -> list[Path]:
    workflows = root / ".github" / "workflows"
    result: set[Path] = set()
    for pattern in PATTERNS:
        result.update(workflows.glob(pattern))
    return sorted(path for path in result if path.is_file())


def check(root: Path, require_freeze: bool) -> dict:
    paths = task_workflows(root)
    require(paths, "no formal-task workflows found")
    violations: list[dict[str, str]] = []
    for path in paths:
        text = path.read_text(encoding="utf-8")
        relative = str(path.relative_to(root))
        for name, pattern in (
            ("contents-write", WRITE_PERMISSION_RE),
            ("git-push", PUSH_RE),
            ("git-commit", COMMIT_RE),
        ):
            if pattern.search(text):
                violations.append({"path": relative, "kind": name})
        # Read-only checkout should not explicitly opt into writable persisted
        # credentials. The checkout action's default is intentionally not
        # inferred here; a final task workflow must state false if it sets it.
        if "persist-credentials:" in text and CHECKOUT_WRITE_RE.search(text):
            violations.append(
                {"path": relative, "kind": "persist-credentials-true"}
            )
    require(not violations, f"write-capable task workflows remain: {violations}")

    freeze = root / "formal" / "FINAL_FORMAL_SOURCE_FREEZE.json"
    freeze_report = None
    if require_freeze:
        require(freeze.is_file(), "FINAL_FORMAL_SOURCE_FREEZE.json is missing")
        document = json.loads(freeze.read_text(encoding="utf-8"))
        require(document.get("schema") == 1, "freeze schema mismatch")
        files = document.get("files")
        removed = document.get("removed_write_workflows")
        require(isinstance(files, dict) and files, "freeze file closure missing")
        require(
            isinstance(removed, list) and removed,
            "freeze removed-workflow inventory missing",
        )
        freeze_report = {
            "source_parent": document.get("source_parent"),
            "file_count": len(files),
            "removed_workflow_count": len(removed),
        }

    return {
        "schema": 1,
        "status": "PASS",
        "workflow_count": len(paths),
        "workflows": [str(path.relative_to(root)) for path in paths],
        "freeze": freeze_report,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", default=".", type=Path)
    parser.add_argument("--require-freeze", action="store_true")
    args = parser.parse_args()
    try:
        report = check(args.repo.resolve(), args.require_freeze)
    except (GuardError, OSError, json.JSONDecodeError) as error:
        print(f"formal task workflow guard rejected: {error}")
        return 1
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
