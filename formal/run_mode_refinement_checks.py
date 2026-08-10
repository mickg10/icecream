#!/usr/bin/env python3
"""Run the authoritative strict/pipelined refinement matrix via runner v4.

The wrapper runs the exact domain static gate first, then adapts to the
proofless generation-4 runner's own CLI. It refuses a substituted manifest,
in-checkout artifacts, or required domain/tool inputs that cannot be mapped.
Formal verdict interpretation remains entirely in runner v4.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
RUNNER = HERE / "run_tlc_only_checks_v4.py"
CHECKER = HERE / "mode_refinement_static_check.py"
AUTHORITATIVE_MANIFEST = HERE / "mode-refinement-checks-v1.json"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    result.add_argument("--repo", required=True, type=Path)
    result.add_argument("--manifest", required=True, type=Path)
    result.add_argument("--artifacts", required=True, type=Path)
    result.add_argument("--expected-git-sha", required=True)
    result.add_argument("--stable-jar", required=True, type=Path)
    result.add_argument("--stable-sha256", required=True)
    result.add_argument("--differential-jar", required=True, type=Path)
    result.add_argument("--differential-sha256", required=True)
    return result


def available_options(help_text: str) -> set[str]:
    return set(re.findall(r"(?<!\w)--[a-z0-9][a-z0-9-]*", help_text))


def required_options(help_text: str) -> set[str]:
    usage_lines: list[str] = []
    collecting = False
    for line in help_text.splitlines():
        if line.startswith("usage:"):
            collecting = True
        if collecting:
            if not line.strip() and usage_lines:
                break
            usage_lines.append(line)
    usage = " ".join(usage_lines)
    return set(re.findall(r"(?<!\[)(--[a-z0-9][a-z0-9-]*)", usage))


def add_supported(
    command: list[str],
    available: set[str],
    candidates: tuple[str, ...],
    value: str,
) -> str | None:
    for candidate in candidates:
        if candidate in available:
            command.extend([candidate, value])
            return candidate
    return None


def require_supported(
    command: list[str],
    available: set[str],
    candidates: tuple[str, ...],
    value: str,
    purpose: str,
) -> str:
    selected = add_supported(command, available, candidates, value)
    if selected is None:
        raise SystemExit(
            f"generation-4 runner exposes no option for {purpose}: "
            + ", ".join(candidates)
        )
    return selected


def run_static(repo: Path, manifest: Path) -> int:
    completed = subprocess.run(
        [
            sys.executable,
            str(CHECKER),
            "--manifest",
            str(manifest),
            "--repo",
            str(repo),
            "--formal-dir",
            "formal",
        ],
        cwd=repo,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    print(completed.stdout, end="")
    return completed.returncode


def main() -> int:
    args = parser().parse_args()
    repo = args.repo.resolve()
    manifest = args.manifest.resolve()
    artifacts = args.artifacts.resolve()

    if manifest != AUTHORITATIVE_MANIFEST.resolve():
        raise SystemExit(
            f"only {AUTHORITATIVE_MANIFEST.resolve()} is authoritative"
        )
    try:
        artifacts.relative_to(repo)
    except ValueError:
        pass
    else:
        raise SystemExit("artifact directory must be outside the checkout")

    static_rc = run_static(repo, manifest)
    if static_rc != 0:
        return static_rc

    help_process = subprocess.run(
        [sys.executable, str(RUNNER), "--help"],
        cwd=repo,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=True,
    )
    help_text = help_process.stdout
    options = available_options(help_text)

    command = [sys.executable, str(RUNNER)]
    supplied: set[str] = set()
    required_mappings = (
        (("--manifest",), str(manifest), "manifest"),
        (("--repo", "--repository"), str(repo), "repository"),
        (("--artifacts", "--artifact-dir", "--artifacts-dir"), str(artifacts), "artifact directory"),
        (("--expected-git-sha", "--expected-revision", "--expected-sha"), args.expected_git_sha, "exact git revision"),
        (("--stable-jar", "--stable-tlc-jar"), str(args.stable_jar.resolve()), "stable TLC jar"),
        (("--stable-sha256", "--stable-tlc-sha256"), args.stable_sha256, "stable TLC digest"),
        (("--differential-jar", "--differential-tlc-jar"), str(args.differential_jar.resolve()), "differential TLC jar"),
        (("--differential-sha256", "--differential-tlc-sha256"), args.differential_sha256, "differential TLC digest"),
    )
    for candidates, value, purpose in required_mappings:
        supplied.add(
            require_supported(command, options, candidates, value, purpose)
        )

    optional_mappings = (
        (("--static-checker", "--static-checker-path", "--checker", "--checker-path", "--static-checker-script"), str(CHECKER.resolve())),
        (("--static-checker-sha256", "--checker-sha256", "--static-checker-hash"), sha256(CHECKER.resolve())),
        (("--manifest-sha256", "--manifest-hash"), sha256(manifest)),
        (("--formal-dir",), str(HERE)),
        (("--domain", "--domain-name", "--suite-name"), "mode-refinement"),
        (("--expected-check-count", "--check-count"), "9"),
    )
    for candidates, value in optional_mappings:
        selected = add_supported(command, options, candidates, value)
        if selected:
            supplied.add(selected)

    checker_arguments = (
        "--manifest",
        str(manifest),
        "--repo",
        str(repo),
        "--formal-dir",
        "formal",
    )
    for checker_argument_option in (
        "--static-checker-arg",
        "--checker-arg",
        "--static-arg",
    ):
        if checker_argument_option in options:
            for argument in checker_arguments:
                command.extend([checker_argument_option, argument])
            supplied.add(checker_argument_option)
            break

    only = os.environ.get("MODE_REFINEMENT_ONLY")
    if only:
        require("--only" in options, "runner does not expose --only")
        command.extend(["--only", only])
        supplied.add("--only")

    unmapped_required = sorted(
        option
        for option in required_options(help_text)
        if option in options and option not in supplied
    )
    relevant = (
        "manifest",
        "repo",
        "revision",
        "sha",
        "git",
        "artifact",
        "stable",
        "differential",
        "jar",
        "checker",
        "static",
        "formal-dir",
        "domain",
    )
    relevant_unmapped = [
        option
        for option in unmapped_required
        if any(token in option for token in relevant)
    ]
    if relevant_unmapped:
        raise SystemExit(
            "unmapped required generation-4 options: "
            + ", ".join(relevant_unmapped)
        )

    print("Mode refinement runner command:")
    print(shlex.join(command))
    return subprocess.run(command, cwd=repo).returncode


if __name__ == "__main__":
    raise SystemExit(main())
