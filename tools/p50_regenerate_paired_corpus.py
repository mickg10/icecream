#!/usr/bin/env python3
"""Regenerate matched preprocessed A/B TU manifests from one compile database.

This deliberately writes two fresh corpora. It never combines archived A inputs
with newly preprocessed B inputs, and it reports whether fresh A reproduces the
archive byte-for-byte. See doc/p50-transfer-window-firefox32-evidence.md for the
paired benchmark manifest contract.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shlex
import subprocess
import sys
from pathlib import Path
from typing import Any


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode()).hexdigest()


def git_output(root: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(root), *args],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return result.stdout.strip()


def repo_identity(root: Path) -> dict[str, str]:
    return {
        "revision": git_output(root, "rev-parse", "HEAD"),
        "status_sha256": sha256_text(git_output(root, "status", "--porcelain=v1")),
        "tracked_diff_sha256": sha256_text(git_output(root, "diff", "--binary", "HEAD")),
    }


def resolve_from(directory: Path, value: str | Path) -> Path:
    path = Path(value)
    return (path if path.is_absolute() else directory / path).resolve()


def normalized_archive_identity(
    archive_root: Path, manifest_path: str, manifest_directory: Path
) -> str:
    path = resolve_from(manifest_directory, manifest_path)
    try:
        return path.relative_to(archive_root).as_posix()
    except ValueError as exc:
        raise ValueError(f"manifest path outside archive root: {path}") from exc


def command_source_args(
    entry: dict[str, Any], source_root: Path, original_root: Path, build_root: Path,
    output_path: Path, vfs_overlay: Path | None = None,
) -> list[str]:
    command = entry.get("arguments")
    argv = list(command) if command else shlex.split(entry["command"])
    directory = Path(entry["directory"]).resolve()
    original_source = resolve_from(directory, entry["file"])
    try:
        source_relative = original_source.relative_to(original_root)
    except ValueError as exc:
        raise ValueError(f"compile source is not below --original-source-root: {original_source}") from exc
    selected_source = source_root / source_relative
    if not selected_source.is_file():
        raise ValueError(f"selected source is missing: {selected_source}")

    old_root = str(original_root)
    new_root = str(source_root)
    old_build = str(build_root)
    transformed: list[str] = []
    index = 0
    replaced_source = False
    while index < len(argv):
        token = argv[index]
        if token in ("-o", "-MF", "-MT", "-MQ"):
            index += 2
            continue
        if token in ("-c", "-MD", "-MMD"):
            index += 1
            continue
        token_path = Path(token)
        if token_path.is_absolute() and token_path.resolve() == original_source:
            # Preserve one virtual path for A/B. The VFS overlay supplies the
            # selected bytes without changing line markers or __FILE__.
            transformed.append(str(original_source))
            replaced_source = True
        elif old_root in token and old_build not in token:
            transformed.append(token.replace(old_root, new_root))
        else:
            transformed.append(token)
        index += 1
    if not replaced_source:
        raise ValueError(f"compile command did not contain its source argument: {original_source}")
    if vfs_overlay is not None:
        transformed.extend(("-ivfsoverlay", str(vfs_overlay)))
    transformed.extend(("-E", "-o", str(output_path)))
    return transformed


def compile_index(
    entries: list[dict[str, Any]], original_root: Path, build_root: Path,
) -> dict[str, dict[str, Any]]:
    original_root = original_root.resolve()
    build_root = build_root.resolve()
    index: dict[str, dict[str, Any]] = {}
    for entry in entries:
        directory = Path(entry["directory"]).resolve()
        output = resolve_from(directory, entry["output"])
        try:
            relative_output = output.relative_to(build_root)
        except ValueError:
            continue
        if output.suffix != ".o":
            continue
        identity = relative_output.with_suffix(".ii").as_posix()
        source = resolve_from(directory, entry["file"])
        if not source.is_relative_to(original_root):
            continue
        if identity in index:
            raise ValueError(f"duplicate compile output identity: {identity}")
        index[identity] = entry
    return index


def select_candidates(
    archive_files: dict[str, Path],
    by_identity: dict[str, dict[str, Any]],
    source_pattern: re.Pattern[str],
    count: int,
    max_bytes: int,
) -> tuple[list[tuple[int, str, dict[str, Any]]], int]:
    candidates: list[tuple[int, str, dict[str, Any]]] = []
    for identity, archived in archive_files.items():
        entry = by_identity.get(identity)
        if entry is None:
            continue
        source = resolve_from(Path(entry["directory"]), entry["file"])
        if source_pattern.search(source.as_posix()):
            candidates.append((archived.stat().st_size, identity, entry))
    candidates.sort(key=lambda row: (-row[0], row[1]))
    if len(candidates) < count:
        raise ValueError(
            f"only {len(candidates)} archived TUs have matching compile commands; requested {count}"
        )
    selected = candidates[:count]
    total = sum(size for size, _, _ in selected)
    if total > max_bytes:
        raise ValueError(
            f"deterministic top-{count} selection is {total} bytes, above cap {max_bytes}; "
            "choose a smaller count or an explicit source subset, do not raise the runtime cap"
        )
    return selected, total


def validate_source_roots(original_root: Path, source_a: Path, source_b: Path) -> None:
    original_root = original_root.resolve()
    if source_a.resolve() != original_root or source_b.resolve() != original_root:
        raise ValueError(
            "this generator requires A and B source roots to be the pinned original tree; "
            "represent B edits with --overlay-b-root so source/include virtual paths stay identical"
        )


def validate_overlay_sources(
    overlay_root: Path | None,
    selected: list[tuple[int, str, dict[str, Any]]],
    original_root: Path,
) -> None:
    if overlay_root is None:
        return
    if not overlay_root.is_dir():
        raise ValueError(f"B overlay root does not exist or is not a directory: {overlay_root}")
    selected_sources = {
        resolve_from(Path(entry["directory"]), entry["file"]).relative_to(original_root)
        for _, _, entry in selected
    }
    for path in overlay_root.rglob("*"):
        if not path.is_file():
            continue
        relative = path.relative_to(overlay_root)
        if relative not in selected_sources:
            raise ValueError(
                f"B overlay contains a non-selected TU or unsupported header edit: {relative}; "
                "only selected translation-unit source files may be overlaid"
            )


def preprocess(command: list[str], cwd: str, label: str, identity: str) -> None:
    result = subprocess.run(
        command,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode:
        raise RuntimeError(
            f"preprocess failed for {label}:{identity} rc={result.returncode}\n{result.stderr[-4000:]}"
        )


def archive_match(path: Path, archived: Path) -> bool:
    return sha256_file(path) == sha256_file(archived)


def write_vfs_overlay(path: Path, virtual_path: Path, external_path: Path) -> str:
    """Map external source bytes to the same Clang-visible source pathname."""
    overlay = {
        "version": 0,
        "case-sensitive": "true",
        "use-external-names": False,
        "roots": [{
            "type": "file",
            "name": str(virtual_path),
            "external-contents": str(external_path),
            "use-external-name": False,
        }],
    }
    encoded = json.dumps(overlay, indent=2) + "\n"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(encoded)
    return sha256_text(encoded)


def write_manifest(output_dir: Path, label: str, selected: list[tuple[int, str, dict[str, Any]]]) -> None:
    manifest = output_dir / f"manifest-{label}.txt"
    manifest.write_text("".join(f"{(output_dir / label / identity)}\n" for _, identity, _ in selected))


def compiler_identity(entries: list[dict[str, Any]]) -> dict[str, str]:
    identities: dict[str, dict[str, str]] = {}
    for entry in entries:
        argv = entry.get("arguments") or shlex.split(entry["command"])
        compiler = argv[0]
        resolved = shutil_which(compiler)
        result = subprocess.run(
            [resolved, "--version"], check=True, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )
        identities[resolved] = {
            "command": compiler,
            "resolved": resolved,
            "version": result.stdout.splitlines()[0],
            "version_sha256": sha256_text(result.stdout),
        }
    if len(identities) != 1:
        raise ValueError(
            "selected compile commands use multiple compiler executables; "
            "split the corpus or pin one toolchain"
        )
    return next(iter(identities.values()))


def shutil_which(command: str) -> str:
    # Keep this tiny helper local so the script has no non-stdlib dependency.
    import shutil

    found = shutil.which(command)
    if not found:
        raise ValueError(f"compiler executable not found: {command}")
    return str(Path(found).resolve())


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compile-commands", type=Path, required=True)
    parser.add_argument("--original-source-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--archive-root", type=Path, required=True)
    parser.add_argument("--archive-manifest", type=Path, required=True)
    parser.add_argument("--source-root-a", type=Path, required=True)
    parser.add_argument("--source-root-b", type=Path, required=True)
    parser.add_argument(
        "--overlay-b-root", type=Path,
        help="optional sparse source overlay; matching TU files override B only",
    )
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--count", type=int, default=32)
    parser.add_argument("--max-bytes", type=int, default=480 * 1024 * 1024)
    parser.add_argument("--source-regex", default=r"(^|/)contrib/rocksdb/")
    parser.add_argument("--expected-revision")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    original_root = args.original_source_root.resolve()
    build_root = args.build_root.resolve()
    archive_root = args.archive_root.resolve()
    source_a = args.source_root_a.resolve()
    source_b = args.source_root_b.resolve()
    overlay_b = args.overlay_b_root.resolve() if args.overlay_b_root else None
    output_dir = args.output_dir.resolve()
    validate_source_roots(original_root, source_a, source_b)
    if args.count <= 0 or args.max_bytes <= 0:
        raise ValueError("--count and --max-bytes must be positive")
    if output_dir.exists():
        raise ValueError(f"output directory already exists; refusing overwrite: {output_dir}")

    identity_a = repo_identity(source_a)
    identity_b = repo_identity(source_b)
    if identity_a["revision"] != identity_b["revision"]:
        raise ValueError("A and B source trees must share the same pinned git revision")
    if args.expected_revision and identity_a["revision"] != args.expected_revision:
        raise ValueError("source revision does not match --expected-revision")

    compile_path = args.compile_commands.resolve()
    manifest_path = args.archive_manifest.resolve()
    compile_raw = compile_path.read_bytes()
    entries = json.loads(compile_raw)
    archive_rows = [line.strip() for line in manifest_path.read_text().splitlines() if line.strip()]
    archive_directory = manifest_path.parent
    archive_files: dict[str, Path] = {}
    for row in archive_rows:
        identity = normalized_archive_identity(archive_root, row, archive_directory)
        path = archive_root / identity
        if identity in archive_files:
            raise ValueError(f"duplicate archive identity: {identity}")
        if not path.is_file():
            raise ValueError(f"archived TU missing: {path}")
        archive_files[identity] = path

    by_identity = compile_index(entries, original_root, build_root)
    pattern = re.compile(args.source_regex)
    selected, total = select_candidates(
        archive_files, by_identity, pattern, args.count, args.max_bytes
    )
    validate_overlay_sources(overlay_b, selected, original_root)

    compiler = compiler_identity([entry for _, _, entry in selected])
    output_dir.mkdir(parents=True)
    roots = {"A": source_a, "B": source_b}
    generated: dict[str, dict[str, Any]] = {"A": {}, "B": {}}
    compile_command_hashes: dict[str, str] = {}
    source_file_hashes: dict[str, dict[str, str]] = {"A": {}, "B": {}}
    overlay_identities: list[str] = []
    vfs_overlay_hashes: dict[str, dict[str, str]] = {"A": {}, "B": {}}
    generated_totals = {"A": 0, "B": 0}
    for label, source_root in roots.items():
        corpus_root = output_dir / label
        for expected_size, identity, entry in selected:
            target = corpus_root / identity
            target.parent.mkdir(parents=True, exist_ok=True)
            overlay = overlay_b if label == "B" else None
            compile_command = entry.get("command") or json.dumps(entry["arguments"])
            compile_command_hashes.setdefault(identity, sha256_text(compile_command))
            source_file = resolve_from(Path(entry["directory"]), entry["file"])
            relative_source = source_file.relative_to(original_root)
            selected_source = source_root / relative_source
            vfs_overlay_path = None
            if overlay is not None and (overlay / relative_source).is_file():
                selected_source = overlay / relative_source
                overlay_identities.append(identity)
            if selected_source != source_file:
                vfs_overlay_path = output_dir / ".vfs" / label / (sha256_text(identity) + ".json")
                vfs_overlay_hashes[label][identity] = write_vfs_overlay(
                    vfs_overlay_path, source_file, selected_source
                )
            command = command_source_args(
                entry, source_root, original_root, build_root, target, vfs_overlay_path
            )
            source_file_hashes[label][identity] = sha256_file(selected_source)
            preprocess(command, entry["directory"], label, identity)
            generated[label][identity] = {
                "bytes": target.stat().st_size,
                "sha256": sha256_file(target),
                "matches_archive": archive_match(target, archive_files[identity]),
                "archive_bytes": expected_size,
            }
            generated_totals[label] += target.stat().st_size
            if generated_totals[label] > args.max_bytes:
                raise ValueError(
                    f"regenerated {label} corpus exceeded cap {args.max_bytes} at {identity}; "
                    "do not use it with the bounded benchmark"
                )

    for label in ("A", "B"):
        if len(generated[label]) != len(selected):
            raise ValueError(f"incomplete regenerated {label} corpus; no pair manifest published")
    # Publish neither manifest until both complete corpora pass their size cap.
    for label in ("A", "B"):
        write_manifest(output_dir, label, selected)
    archive_only_ids = sorted(set(archive_files) - set(by_identity))
    compile_only_ids = sorted(set(by_identity) - set(archive_files))
    metadata = {
        "schema": "p50-paired-preprocess-v1",
        "compile_commands": str(compile_path),
        "compile_commands_sha256": hashlib.sha256(compile_raw).hexdigest(),
        "archive_root": str(archive_root),
        "archive_manifest": str(manifest_path),
        "archive_manifest_sha256": sha256_file(manifest_path),
        "original_source_root": str(original_root),
        "build_root": str(build_root),
        "source_roots": {"A": str(source_a), "B": str(source_b)},
        "source_overlay_b": str(overlay_b) if overlay_b else None,
        "source_identity": {"A": identity_a, "B": identity_b},
        "compiler": compiler,
        "selection": {
            "regex": args.source_regex,
            "count": len(selected),
            "archived_total_bytes": total,
            "max_bytes": args.max_bytes,
            "ordered_identities": [identity for _, identity, _ in selected],
            "archive_only_identities": archive_only_ids,
            "compile_only_identities": compile_only_ids,
        },
        "generated_total_bytes_by_label": generated_totals,
        "compile_command_sha256_by_identity": compile_command_hashes,
        "source_file_sha256_by_label": source_file_hashes,
        "b_overlay_identities": sorted(set(overlay_identities)),
        "vfs_overlays": vfs_overlay_hashes,
        "generated": generated,
        "a_archive_exact_match_count": sum(
            record["matches_archive"] for record in generated["A"].values()
        ),
        "b_archive_exact_match_count": sum(
            record["matches_archive"] for record in generated["B"].values()
        ),
        "changed_preprocessed_count": sum(
            generated["A"][identity]["sha256"] != generated["B"][identity]["sha256"]
            for _, identity, _ in selected
        ),
    }
    (output_dir / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(json.dumps({
        "output_dir": str(output_dir),
        "count": len(selected),
        "archived_total_bytes": total,
        "a_archive_exact_match_count": metadata["a_archive_exact_match_count"],
        "b_archive_exact_match_count": metadata["b_archive_exact_match_count"],
        "changed_preprocessed_count": metadata["changed_preprocessed_count"],
        "archive_only_identities": metadata["selection"]["archive_only_identities"],
        "source_revision": identity_a["revision"],
        "compiler": compiler["version"],
    }, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, KeyError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2)
