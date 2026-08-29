#!/usr/bin/env python3
"""Read-only readiness inventory for the predeclared DuckDB and LLVM S8 inputs.

The inventory authenticates source checkouts, retained ``.ii`` manifests, and
one compile-database entry per held-out corpus.  It does not read experiment
results, invoke a compiler, or create an input.  The emitted JSON is a
machine-readable handoff for the existing ``p50compilee2e-run.sh`` contract.
"""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import os
import shlex
import stat
import subprocess
import sys
from pathlib import Path
from typing import Any


SCHEMA = "icecream-s8-heldout-preflight-v1"
DEFAULT_ROOT = Path("/tanksmall/scratch/ictmp")
RUNNER = Path("unittests/p50compilee2e-run.sh")
SOURCE_CONTRACT = Path("unittests/p50compilee2e-source.sh")
CORPORA = {
    "DuckDB": {
        "source_root": Path("build2/duckdb"),
        "corpus_root": Path("corpus3"),
        "compile_db": Path("build2/duckdb/build/compile_commands.json"),
        "representative_name": "test_platform.cpp.ii",
        "corpus_id": "corpus3", "tu": 689,
        "matrix_manifest": Path(
            "cold-four-build-matrix-p25-p29-source-visible/manifests/duckdb/manifest.txt"
        ),
    },
    "LLVM-1238": {
        "source_root": Path("corpus/llvm-project"),
        "corpus_root": Path("corpus"),
        "compile_db": Path("corpus/build/compile_commands.json"),
        "representative_name": "AliasAnalysis.cpp.ii",
        "corpus_id": "corpus", "tu": 1238,
        "matrix_manifest": Path(
            "cold-four-build-matrix-p25-p29-source-visible/manifests/llvm/manifest.txt"
        ),
    },
}
PROFILES = ("ZSTD_TU", "ZSTD_ROUTE", "P29", "GRZ_RESIDUAL")
MAX_FILE_BYTES = 512 * 1024 * 1024


class PreflightError(ValueError):
    """Raised when a required held-out input is unavailable or ambiguous."""


def _sha256(path: Path, label: str, limit: int = MAX_FILE_BYTES) -> dict[str, Any]:
    try:
        info = path.lstat()
    except OSError as exc:
        raise PreflightError(f"{label}:unavailable:{path}") from exc
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
        raise PreflightError(f"{label}:not_private_regular_file:{path}")
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0))
    try:
        before = os.fstat(fd)
        digest = hashlib.sha256()
        size = 0
        while True:
            block = os.read(fd, 1 << 20)
            if not block:
                break
            size += len(block)
            if size > limit:
                raise PreflightError(f"{label}:too_large:{path}")
            digest.update(block)
        after = os.fstat(fd)
        if any(getattr(before, field) != getattr(after, field) for field in
               ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns")):
            raise PreflightError(f"{label}:changed_while_reading:{path}")
        return {"path": str(path), "bytes": size, "sha256": digest.hexdigest()}
    except OSError as exc:
        raise PreflightError(f"{label}:read_failed:{path}") from exc
    finally:
        os.close(fd)


def _json(path: Path, label: str) -> dict[str, Any]:
    facts = _sha256(path, label, 16 * 1024 * 1024)
    try:
        raw = path.read_bytes()
        value = json.loads(
            raw.decode("utf-8"),
            object_pairs_hook=_unique_keys,
            parse_constant=lambda token: (_ for _ in ()).throw(
                PreflightError(f"{label}:non_finite:{path}")
            ),
        )
    except PreflightError:
        raise
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise PreflightError(f"{label}:invalid_json:{path}") from exc
    facts["value"] = value
    return facts


def _unique_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise PreflightError(f"duplicate_json_key:{key}")
        result[key] = value
    return result


def _git_identity(root: Path) -> dict[str, str]:
    def rev(spec: str) -> str:
        try:
            result = subprocess.run(["git", "-C", str(root), "rev-parse", spec],
                                    check=True, capture_output=True, text=True, timeout=10)
        except (OSError, subprocess.SubprocessError) as exc:
            raise PreflightError(f"source_git_identity_unavailable:{root}") from exc
        value = result.stdout.strip()
        if len(value) != 40 or any(char not in "0123456789abcdefABCDEF" for char in value):
            raise PreflightError(f"source_git_identity_invalid:{root}")
        return value.lower()

    try:
        status = subprocess.run(["git", "-C", str(root), "status", "--porcelain",
                                 "--untracked-files=all"], check=True,
                                capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.SubprocessError) as exc:
        raise PreflightError(f"source_git_status_unavailable:{root}") from exc
    if status.stdout:
        raise PreflightError(f"source_git_dirty:{root}")
    return {"commit": rev("HEAD"), "tree": rev("HEAD^{tree}")}


def _paths_from_manifest(path: Path, root: Path, label: str,
                         host_root: Path | None = None,
                         repeat_factor: int | None = None,
                         representative_name: str | None = None) -> dict[str, Any]:
    facts = _sha256(path, label, 32 * 1024 * 1024)
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as exc:
        raise PreflightError(f"{label}:unreadable:{path}") from exc
    if not lines or any(not line or line != line.strip() for line in lines):
        raise PreflightError(f"{label}:line_format_invalid:{path}")
    resolved: list[Path] = []
    host_root = host_root or root
    for line in lines:
        candidate = Path(line)
        if not candidate.is_absolute():
            candidate = root / candidate
        elif line.startswith("/home/ttuser/ictmp/"):
            candidate = host_root / line.removeprefix("/home/ttuser/ictmp/")
        candidate = candidate.resolve()
        try:
            candidate.relative_to(root.resolve())
        except ValueError as exc:
            raise PreflightError(f"{label}:path_outside_corpus:{line}") from exc
        if not candidate.is_file() or candidate.is_symlink():
            raise PreflightError(f"{label}:missing_input:{candidate}")
        resolved.append(candidate)
    counts = Counter(resolved)
    if repeat_factor is None and len(counts) != len(resolved):
        raise PreflightError(f"{label}:duplicate_input")
    if repeat_factor is not None and (repeat_factor <= 0 or
                                      set(counts.values()) != {repeat_factor}):
        raise PreflightError(f"{label}:repeat_factor_invalid")
    representatives = [item for item in counts if representative_name is None or
                       item.name == representative_name]
    if representative_name is not None and len(representatives) != 1:
        raise PreflightError(f"{label}:representative_count={len(representatives)}")
    return {"file": facts, "entries": len(resolved), "unique_entries": len(counts),
            **({"repeat_factor": repeat_factor} if repeat_factor is not None else {}),
            "first": str(resolved[0]), "last": str(resolved[-1]),
            "representative": str(representatives[0] if representative_name else resolved[0])}


def _compile_entry(db: Path, source: Path, source_root: Path, label: str) -> dict[str, Any]:
    facts = _json(db, label)
    entries = facts.pop("value")
    if not isinstance(entries, list) or not entries:
        raise PreflightError(f"{label}:entries_invalid:{db}")
    matches: list[dict[str, Any]] = []
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("file"), str):
            continue
        file_path = Path(entry["file"])
        if not file_path.is_absolute():
            directory = Path(entry.get("directory", str(source_root)))
            if not directory.is_absolute():
                directory = source_root / directory
            file_path = directory / file_path
        if file_path.resolve() != source.resolve():
            continue
        if ("command" in entry) == ("arguments" in entry):
            raise PreflightError(f"{label}:command_arguments_ambiguous:{db}")
        if "arguments" in entry:
            argv = entry["arguments"]
            if not isinstance(argv, list) or not all(isinstance(token, str) for token in argv):
                raise PreflightError(f"{label}:arguments_invalid:{db}")
        else:
            try:
                argv = shlex.split(entry["command"])
            except (TypeError, ValueError) as exc:
                raise PreflightError(f"{label}:command_unparseable:{db}") from exc
        if not argv:
            raise PreflightError(f"{label}:command_empty:{db}")
        directory = Path(entry.get("directory", str(source_root)))
        if not directory.is_absolute():
            directory = source_root / directory
        source_tokens = []
        for token in argv:
            candidate = Path(token)
            if not candidate.is_absolute():
                candidate = directory / candidate
            if candidate.resolve() == source.resolve():
                source_tokens.append(token)
        if len(source_tokens) != 1:
            raise PreflightError(f"{label}:source_token_ambiguous:{db}")
        matches.append({"directory": str(entry.get("directory", source_root)),
                        "argv": argv, "command": shlex.join(argv)})
    if len(matches) != 1:
        raise PreflightError(f"{label}:match_count={len(matches)}:{db}")
    match = matches[0]
    return {"file": facts, "source": str(source), "directory": match["directory"],
            "argv": match["argv"], "command": match["command"],
            "argv_sha256": hashlib.sha256(
                json.dumps(match["argv"], separators=(",", ":"), ensure_ascii=True).encode()
            ).hexdigest()}


def _runner_contract(repo: Path) -> dict[str, Any]:
    runner = repo / RUNNER
    source_contract = repo / SOURCE_CONTRACT
    runner_facts = _sha256(runner, "runner")
    source_facts = _sha256(source_contract, "source_contract")
    try:
        subprocess.run(["sh", "-n", str(runner)], check=True,
                       capture_output=True, timeout=10)
        subprocess.run(["sh", "-n", str(source_contract)], check=True,
                       capture_output=True, timeout=10)
    except (OSError, subprocess.SubprocessError) as exc:
        raise PreflightError("runner_shell_syntax_invalid") from exc
    text = runner.read_text(encoding="utf-8")
    required = ["P29) profile_advertisement=p29", "ZSTD_TU)", "ZSTD_ROUTE)",
                "GRZ|GRZ_RESIDUAL)", "ICECC_P50_C1F1_WARM", "ICECC_P50_C1F1_INPUT",
                "ICECC_P50_C1F1_COMPILE_DB", "ICECC_P50_C1F1_COMPILE_SOURCE"]
    missing = [needle for needle in required if needle not in text]
    if missing:
        raise PreflightError(f"runner_contract_missing:{','.join(missing)}")
    return {
        "path": str(runner), "sha256": runner_facts["sha256"], "bytes": runner_facts["bytes"],
        "source_contract": {"path": str(source_contract), "sha256": source_facts["sha256"],
                            "bytes": source_facts["bytes"]},
        "profiles": list(PROFILES), "warm_values": [0, 1],
        "accepted_without_dependency": ["ZSTD_TU", "ZSTD_ROUTE", "P29"],
        "dependency_required": {"GRZ_RESIDUAL": "ICECC_P50_WITH_LIBBSC product build"},
        "inputs": ["ICECC_P50_C1F1_SOURCE_ROOT", "ICECC_P50_C1F1_SOURCE_RELATIVE",
                   "ICECC_P50_C1F1_COMPILE_DB", "ICECC_P50_C1F1_COMPILE_SOURCE"],
        "contract_check": "PASS",
    }


def preflight(root: Path = DEFAULT_ROOT, repo: Path | None = None) -> dict[str, Any]:
    """Return a deterministic, authenticated held-out readiness manifest."""
    root = root.resolve()
    repo = (repo or Path(__file__).resolve().parents[1]).resolve()
    corpora: dict[str, Any] = {}
    for corpus, config in CORPORA.items():
        source_root = root / config["source_root"]
        corpus_root = root / config["corpus_root"]
        source_manifest = corpus_root / "manifest.txt"
        metadata = _json(corpus_root / "METADATA.json", f"{corpus}.metadata")
        metadata_value = metadata.pop("value")
        if not isinstance(metadata_value, dict):
            raise PreflightError(f"{corpus}:metadata_object_required")
        if metadata_value.get("project") not in {corpus, "LLVM" if corpus == "LLVM-1238" else corpus}:
            raise PreflightError(f"{corpus}:metadata_project_invalid")
        if metadata_value.get("corpus_id") != config["corpus_id"] or metadata_value.get("TU") != config["tu"]:
            raise PreflightError(f"{corpus}:metadata_identity_invalid")
        if metadata_value.get("source_checkouts") != [str(source_root)]:
            raise PreflightError(f"{corpus}:metadata_source_checkout_invalid")
        source = Path(config["source_root"]) / Path(
            "tools/utils/test_platform.cpp" if corpus == "DuckDB" else "llvm/lib/Analysis/AliasAnalysis.cpp"
        )
        source_path = root / source
        retained = _paths_from_manifest(
            source_manifest, corpus_root, f"{corpus}.retained_manifest",
            representative_name=config["representative_name"])
        matrix = _paths_from_manifest(root / config["matrix_manifest"], corpus_root,
                                      f"{corpus}.matrix_manifest", root, repeat_factor=4)
        source_facts = _sha256(source_path, f"{corpus}.source")
        representative = Path(retained["representative"])
        ii_facts = _sha256(representative, f"{corpus}.representative_ii")
        compile_db = root / config["compile_db"]
        compile_entry = _compile_entry(compile_db, source_path, source_root, f"{corpus}.compile_db")
        git_identity = _git_identity(source_root)
        metadata_commit = metadata_value.get("git_commit")
        if not isinstance(metadata_commit, str) or not git_identity["commit"].startswith(metadata_commit.lower()):
            raise PreflightError(f"{corpus}:metadata_git_commit_mismatch")
        corpora[corpus] = {
            "source_root": str(source_root), "corpus_root": str(corpus_root),
            "source": source_facts | {"relative": str(source.relative_to(config["source_root"]))},
            "git": git_identity, "metadata": {"file": metadata, "value": metadata_value},
            "retained_manifest": retained, "matrix_manifest": matrix,
            "representative_ii": ii_facts | {"relative": str(representative.relative_to(corpus_root))},
            "compile_database": compile_entry,
        }
    runner = _runner_contract(repo)
    for corpus, row in corpora.items():
        source_root = Path(row["source_root"])
        compile_source = Path(row["compile_database"]["source"]).relative_to(source_root)
        row["runner_template"] = (
            f"ICECC_TEST_TOP_SRCDIR={repo} ICECC_TEST_TOP_BUILDDIR=$PRODUCT_BUILD "
            f"ICECC_P50_PROFILE=$PROFILE ICECC_P50_C1F1_WARM=$WARM "
            f"ICECC_P50_C1F1_SOURCE_ROOT={row['corpus_root']} "
            f"ICECC_P50_C1F1_SOURCE_RELATIVE={row['representative_ii']['relative']} "
            f"ICECC_P50_C1F1_COMPILE_DB={row['compile_database']['file']['path']} "
            f"ICECC_P50_C1F1_COMPILE_SOURCE={compile_source} {runner['path']}"
        )
    runner["product_build"] = {
        "status": "NOT_CHECKED",
        "required_for_runtime": True,
        "required_binaries": ["daemon/iceccd", "scheduler/icecc-scheduler",
                               "client/icecc", "cache/icecc-cache-service"],
        "grz_requirement": "config.h ICECC_P50_WITH_LIBBSC and cache/Makefile libbsc inputs",
    }
    return {"schema": SCHEMA, "status": "PASS", "root": str(root),
            "corpora": corpora, "runner": runner}


def _write_new(path: Path, raw: bytes) -> None:
    if path.exists() or path.is_symlink():
        raise PreflightError(f"output_already_exists:{path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as stream:
        stream.write(raw)
        stream.flush()
        os.fsync(stream.fileno())


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path)
    args = parser.parse_args(argv)
    try:
        value = preflight(args.root, args.repo)
        raw = (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()
        if args.out:
            _write_new(args.out.absolute(), raw)
    except PreflightError as exc:
        print(f"s8_heldout_preflight: {exc}", file=sys.stderr)
        return 2
    print(raw.decode(), end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
