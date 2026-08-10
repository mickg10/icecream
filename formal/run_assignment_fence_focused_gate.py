#!/usr/bin/env python3
"""Invoke generation-4 proof-bearing acceptance without guessing its CLI.

The launcher reads the runner's own help, supplies every recognized pinned proof
input, and fails if a required option remains unmapped.  It does not interpret
or weaken any formal result; the generation-4 runner remains authoritative.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
RUNNER = HERE / "run_formal_checks_v4.py"


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
    result.add_argument("--tlapm", required=True, type=Path)
    result.add_argument("--backend-dir", required=True, type=Path)
    return result


def option_names(help_text: str) -> set[str]:
    return set(re.findall(r"(?<!\w)--[a-z0-9][a-z0-9-]*", help_text))


def required_options(help_text: str) -> set[str]:
    required: set[str] = set()
    usage_lines: list[str] = []
    collecting = False
    for line in help_text.splitlines():
        if line.startswith("usage:"):
            collecting = True
        if collecting:
            usage_lines.append(line)
            if line.strip().endswith("]") or not line.strip():
                collecting = False
    usage = " ".join(usage_lines)
    for match in re.finditer(r"(?<!\[)(--[a-z0-9][a-z0-9-]*)", usage):
        required.add(match.group(1))
    return required


def first_existing(repo: Path, patterns: tuple[str, ...]) -> Path | None:
    candidates: list[Path] = []
    for pattern in patterns:
        candidates.extend(repo.glob(pattern))
    files = sorted(path for path in candidates if path.is_file())
    return files[0] if files else None


def add_first_supported(
    command: list[str],
    available: set[str],
    names: tuple[str, ...],
    value: str,
) -> str | None:
    for name in names:
        if name in available:
            command.extend([name, value])
            return name
    return None


def main() -> int:
    args = parser().parse_args()
    repo = args.repo.resolve()
    help_process = subprocess.run(
        [sys.executable, str(RUNNER), "--help"],
        cwd=repo,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=True,
    )
    help_text = help_process.stdout
    available = option_names(help_text)

    command = [
        sys.executable,
        str(RUNNER),
        "--manifest",
        str(args.manifest),
        "--repo",
        str(repo),
        "--artifacts",
        str(args.artifacts),
        "--expected-git-sha",
        args.expected_git_sha,
        "--stable-jar",
        str(args.stable_jar),
        "--stable-sha256",
        args.stable_sha256,
        "--differential-jar",
        str(args.differential_jar),
        "--differential-sha256",
        args.differential_sha256,
    ]
    supplied = {
        "--manifest",
        "--repo",
        "--artifacts",
        "--expected-git-sha",
        "--stable-jar",
        "--stable-sha256",
        "--differential-jar",
        "--differential-sha256",
    }

    tlapm = args.tlapm.resolve()
    chosen = add_first_supported(
        command,
        available,
        ("--tlapm", "--tlapm-path", "--tlapm-bin", "--tlapm-executable"),
        str(tlapm),
    )
    if chosen is None:
        raise SystemExit("proof-bearing runner exposes no TLAPM executable option")
    supplied.add(chosen)

    chosen = add_first_supported(
        command,
        available,
        ("--tlapm-sha256", "--tlapm-hash", "--tlapm-digest"),
        sha256(tlapm),
    )
    if chosen:
        supplied.add(chosen)

    backend_dir = args.backend_dir.resolve()
    chosen = add_first_supported(
        command,
        available,
        ("--backend-dir", "--tlaps-backend-dir", "--backend-bin-dir"),
        str(backend_dir),
    )
    if chosen:
        supplied.add(chosen)

    probe = first_existing(
        repo,
        (
            "formal/*backend*probe*.py",
            "formal/*tlaps*probe*.py",
            "formal/*backend*probe*",
        ),
    )
    probe_option = None
    if probe is not None:
        probe_option = add_first_supported(
            command,
            available,
            (
                "--backend-probe",
                "--tlaps-backend-probe",
                "--backend-probe-path",
            ),
            str(probe.resolve()),
        )
        if probe_option:
            supplied.add(probe_option)
        chosen = add_first_supported(
            command,
            available,
            (
                "--backend-probe-sha256",
                "--tlaps-backend-probe-sha256",
                "--backend-probe-hash",
            ),
            sha256(probe),
        )
        if chosen:
            supplied.add(chosen)

    if "--only" in available and os.environ.get("ASSIGNMENT_FENCE_ONLY"):
        command.extend(["--only", os.environ["ASSIGNMENT_FENCE_ONLY"]])
        supplied.add("--only")

    unmapped_required = sorted(
        option
        for option in required_options(help_text)
        if option in available and option not in supplied
    )
    # argparse's wrapped usage can make optional flags look required.  Restrict
    # this refusal to proof/tool options; base execution options are mapped
    # explicitly above and unrelated optional controls retain runner defaults.
    unmapped_proof = [
        option
        for option in unmapped_required
        if any(token in option for token in ("tlapm", "tlaps", "backend", "proof"))
    ]
    if unmapped_proof:
        raise SystemExit(
            "unmapped required proof-runner options: " + ", ".join(unmapped_proof)
        )

    print("focused runner command:")
    print(" ".join(subprocess.list2cmdline([item]) for item in command))
    completed = subprocess.run(command, cwd=repo)
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
