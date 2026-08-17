#!/usr/bin/env python3
"""Capture the exact preprocessing profile for one matrix cell."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
from datetime import datetime, timezone


def run(command: list[str], *, stdin: bytes | None = None) -> dict[str, object]:
    try:
        result = subprocess.run(command, input=stdin, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, check=False)
        return {
            "command": command,
            "returncode": result.returncode,
            "stdout": result.stdout.decode("utf-8", "replace"),
            "stderr": result.stderr.decode("utf-8", "replace"),
        }
    except OSError as error:
        return {"command": command, "returncode": None, "error": str(error),
                "stdout": "", "stderr": ""}


def write_capture(directory: Path, name: str, content: str) -> dict[str, object]:
    path = directory / name
    path.write_text(content, encoding="utf-8")
    encoded = content.encode("utf-8")
    return {"path": f"environment/{name}", "bytes": len(encoded),
            "sha256": hashlib.sha256(encoded).hexdigest()}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--project", required=True)
    parser.add_argument("--profile", required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--image-id", required=True)
    args = parser.parse_args()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    captures = args.output.parent / "environment"
    captures.mkdir(parents=True, exist_ok=True)
    cc = os.environ.get("CC", "cc")
    cxx = os.environ.get("CXX", "c++")

    compiler_version = run([cxx, "--version"])
    target = run([cxx, "-dumpmachine"])
    sysroot = run([cxx, "--print-sysroot"])
    macros = run([cxx, "-dM", "-E", "-x", "c++", "-"], stdin=b"\n")
    includes = run([cxx, "-E", "-v", "-x", "c++", "-"], stdin=b"\n")
    macro_capture = write_capture(captures, "predefined-macros.txt", str(macros["stdout"]))
    include_capture = write_capture(
        captures, "include-search.txt",
        str(includes["stdout"]) + "\n--- stderr ---\n" + str(includes["stderr"]),
    )

    tools = {}
    for tool, version_args in {
        "cmake": ["--version"], "ninja": ["--version"], "make": ["--version"],
        "python3": ["--version"], "conan": ["--version"], "brew": ["--version"],
        "zstd": ["--version"], "tar": ["--version"],
    }.items():
        if shutil.which(tool):
            tools[tool] = run([tool, *version_args])

    git_revision = run(["git", "-C", str(args.source_root), "rev-parse", "HEAD"])
    git_status = run(["git", "-C", str(args.source_root), "status", "--porcelain=v1"])
    commands_path = args.output.parent / "commands.jsonl"
    commands_sha = None
    if commands_path.is_file():
        commands_sha = hashlib.sha256(commands_path.read_bytes()).hexdigest()

    report = {
        "schema": 1,
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "project": args.project,
        "profile": args.profile,
        "container_image_id": args.image_id,
        "source_root_inside_profile": str(args.source_root),
        "build_root_inside_profile": str(args.build_root),
        "cc": cc,
        "cxx": cxx,
        "compiler_version": compiler_version,
        "target": target,
        "sysroot": sysroot,
        "predefined_macros": macro_capture,
        "include_search": include_capture,
        "git_revision": git_revision,
        "git_status": git_status,
        "commands_jsonl_sha256": commands_sha,
        "environment": {key: value for key, value in sorted(os.environ.items())
                        if key.startswith(("CC", "CXX", "CPP", "LD", "AR", "CMAKE_", "CONAN_", "PROFILE_"))},
        "platform": platform.uname()._asdict(),
        "tools": tools,
    }
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
