#!/usr/bin/env python3
"""Run the authoritative F-to-S quotient matrix through runner v4.

The wrapper performs the exact domain static check first, then discovers the
proofless generation-4 runner interface from its own help. It supplies every
recognized manifest/checker/tool digest and refuses required domain/tool
options it cannot map. Formal verdict interpretation remains entirely in the
generation-4 runner.
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
CHECKER = HERE / "f2s_quotient_static_check.py"
AUTHORITATIVE_MANIFEST = HERE / "f2s-quotient-checks-v1.json"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--expected-git-sha", required=True)
    parser.add_argument("--stable-jar", required=True, type=Path)
    parser.add_argument("--stable-sha256", required=True)
    parser.add_argument("--differential-jar", required=True, type=Path)
    parser.add_argument("--differential-sha256", required=True)
    return parser


def available_options(help_text: str) -> set[str]:
    return set(re.findall(r"(?<!\w)--[a-z0-9][a-z0-9-]*", help_text))


def required_options(help_text: str) -> set[str]:
    """Extract unbracketed options from argparse's wrapped usage block."""
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
    return set(
        re.findall(r"(?<!\[)(--[a-z0-9][a-z0-9-]*)", usage)
    )


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
    chosen = add_supported(command, available, candidates, value)
    if chosen is None:
        raise SystemExit(
            f"generation-4 runner exposes no option for {purpose}: "
            + ", ".join(candidates)
        )
    return chosen


def run_static_check(repo: Path, manifest: Path) -> None:
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
    if completed.returncode != 0:
        raise SystemExit(completed.returncode)


def main() -> int:
    args = build_parser().parse_args()
    repo = args.repo.resolve()
    manifest = args.manifest.resolve()
    artifacts = args.artifacts.resolve()
    checker = CHECKER.resolve()

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

    run_static_check(repo, manifest)

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

    mappings = (
        (("--manifest",), str(manifest), "manifest"),
        (("--repo", "--repository"), str(repo), "repository"),
        (("--artifacts", "--artifact-dir", "--artifacts-dir"), str(artifacts), "artifact directory"),
        (("--expected-git-sha", "--expected-revision", "--expected-sha"), args.expected_git_sha, "exact git revision"),
        (("--stable-jar", "--stable-tlc-jar"), str(args.stable_jar.resolve()), "stable TLC jar"),
        (("--stable-sha256", "--stable-tlc-sha256"), args.stable_sha256, "stable TLC digest"),
        (("--differential-jar", "--differential-tlc-jar"), str(args.differential_jar.resolve()), "differential TLC jar"),
        (("--differential-sha256", "--differential-tlc-sha256"), args.differential_sha256, "differential TLC digest"),
    )
    for candidates, value, purpose in mappings:
        supplied.add(
            require_supported(command, options, candidates, value, purpose)
        )

    optional_mappings = (
        (("--static-checker", "--static-checker-path", "--checker", "--checker-path", "--static-checker-script"), str(checker)),
        (("--static-checker-sha256", "--checker-sha256", "--static-checker-hash"), sha256(checker)),
        (("--manifest-sha256", "--manifest-hash"), sha256(manifest)),
        (("--formal-dir",), str(HERE)),
    )
    for candidates, value in optional_mappings:
        chosen = add_supported(command, options, candidates, value)
        if chosen:
            supplied.add(chosen)

    # F2S_RUNNER_V4_DOMAIN_ARGS: supply domain metadata and repeated
    # static-checker argv only when the generation-4 runner exposes them.
    checker_arguments = [
        "--manifest",
        str(manifest),
        "--repo",
        str(repo),
        "--formal-dir",
        "formal",
    ]
    for option in (
        "--static-checker-arg",
        "--checker-arg",
        "--static-arg",
    ):
        if option in options:
            for argument in checker_arguments:
                command.extend([option, argument])
            supplied.add(option)
            break

    for candidates, value in (
        (("--domain", "--domain-name", "--suite-name"), "f2s-quotient"),
        (("--expected-check-count", "--check-count"), "3"),
    ):
        chosen = add_supported(command, options, candidates, value)
        if chosen:
            supplied.add(chosen)

    only = os.environ.get("F2S_QUOTIENT_ONLY")
    if only and "--only" in options:
        command.extend(["--only", only])
        supplied.add("--only")

    unmapped = sorted(
        option
        for option in required_options(help_text)
        if option in options and option not in supplied
    )
    relevant_tokens = (
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
    )
    relevant_unmapped = [
        option
        for option in unmapped
        if any(token in option for token in relevant_tokens)
    ]
    if relevant_unmapped:
        raise SystemExit(
            "unmapped required generation-4 options: "
            + ", ".join(relevant_unmapped)
        )

    print("F2S quotient runner command:")
    print(shlex.join(command))
    completed = subprocess.run(command, cwd=repo)
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
