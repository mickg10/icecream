#!/usr/bin/env python3
"""Mutation tests for check_formal_task_workflows_readonly.py."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

import check_formal_task_workflows_readonly as guard


READ_ONLY = """name: read-only
on:
  workflow_dispatch:
permissions:
  contents: read
jobs:
  check:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
        with:
          persist-credentials: false
      - run: echo ok
"""


class FormalTaskWorkflowGuardTests(unittest.TestCase):
    def make_repo(self, temporary: str) -> Path:
        root = Path(temporary)
        workflows = root / ".github" / "workflows"
        workflows.mkdir(parents=True)
        formal = root / "formal"
        formal.mkdir()
        (workflows / "assignment-fence-read-only.yml").write_text(
            READ_ONLY, encoding="utf-8"
        )
        return root

    def write_freeze(self, root: Path) -> None:
        (root / "formal" / "FINAL_FORMAL_SOURCE_FREEZE.json").write_text(
            json.dumps(
                {
                    "schema": 1,
                    "source_parent": "1" * 40,
                    "removed_write_workflows": [
                        ".github/workflows/assignment-fence-fix-once.yml"
                    ],
                    "files": {
                        "formal/AssignmentFenceCore.tla": "a" * 64
                    },
                }
            )
            + "\n",
            encoding="utf-8",
        )

    def test_read_only_workflow_passes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_repo(temporary)
            report = guard.check(root, require_freeze=False)
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["workflow_count"], 1)

    def test_contents_write_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_repo(temporary)
            path = root / ".github" / "workflows" / "f2s-write.yml"
            path.write_text(
                READ_ONLY.replace("contents: read", "contents: write"),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(guard.GuardError, "contents-write"):
                guard.check(root, require_freeze=False)

    def test_git_push_is_rejected_without_contents_write(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_repo(temporary)
            path = root / ".github" / "workflows" / "cross-branch-push.yml"
            path.write_text(
                READ_ONLY.replace("- run: echo ok", "- run: git push origin HEAD:branch"),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(guard.GuardError, "git-push"):
                guard.check(root, require_freeze=False)

    def test_git_commit_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_repo(temporary)
            path = root / ".github" / "workflows" / "assignment-fence-commit.yml"
            path.write_text(
                READ_ONLY.replace("- run: echo ok", "- run: git commit -m generated"),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(guard.GuardError, "git-commit"):
                guard.check(root, require_freeze=False)

    def test_explicit_persisted_credentials_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_repo(temporary)
            path = root / ".github" / "workflows" / "assignment-fence-creds.yml"
            path.write_text(
                READ_ONLY.replace(
                    "persist-credentials: false",
                    "persist-credentials: true",
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                guard.GuardError, "persist-credentials-true"
            ):
                guard.check(root, require_freeze=False)

    def test_freeze_is_required_in_final_mode(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_repo(temporary)
            with self.assertRaisesRegex(guard.GuardError, "freeze"):
                guard.check(root, require_freeze=True)
            self.write_freeze(root)
            report = guard.check(root, require_freeze=True)
        self.assertEqual(report["freeze"]["file_count"], 1)
        self.assertEqual(report["freeze"]["removed_workflow_count"], 1)

    def test_unrelated_workflows_are_outside_the_task_guard(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_repo(temporary)
            path = root / ".github" / "workflows" / "unrelated-release.yml"
            path.write_text(
                READ_ONLY.replace("contents: read", "contents: write"),
                encoding="utf-8",
            )
            report = guard.check(root, require_freeze=False)
        self.assertEqual(report["workflow_count"], 1)


if __name__ == "__main__":
    unittest.main()
