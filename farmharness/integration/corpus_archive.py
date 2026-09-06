#!/usr/bin/env python3
"""Build deterministic zstd-19/long-31 archives for authenticated corpus groups."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import tempfile
from pathlib import Path
from typing import Any, Iterable

try:
    from .farm_spec import FarmSpec, load_farm_spec
    from .firefox_corpus_promotion import (
        validate_corpus_promotion,
        verify_corpus_bodies,
    )
    from .lifecycle import CorpusGroup, corpus_layout
    from .schema_validation import canonical_bytes
except ImportError:  # Direct execution from this directory.
    from farm_spec import FarmSpec, load_farm_spec
    from firefox_corpus_promotion import (
        validate_corpus_promotion,
        verify_corpus_bodies,
    )
    from lifecycle import CorpusGroup, corpus_layout
    from schema_validation import canonical_bytes


ARCHIVE_SCHEMA = "icefarm-corpus-archives-v1"
COMPRESSION = {
    "checksum": True,
    "codec": "zstd",
    "level": 19,
    "long": 31,
    "threads": 8,
}


class CorpusArchiveError(RuntimeError):
    """A corpus group could not be represented by one authenticated archive."""


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _manifest(
    group: CorpusGroup, prevalidated: dict[Path, str] | None = None
) -> tuple[bytes, int]:
    rows: list[tuple[str, str]] = []
    unpacked_bytes = 0
    for source, relative in group.files:
        digest = (
            prevalidated[source]
            if prevalidated is not None and source in prevalidated
            else _sha256(source)
        )
        rows.append((digest, f"{group.name}/{relative}"))
        unpacked_bytes += source.stat().st_size
    rows.sort(key=lambda item: item[1])
    return (
        "".join(f"{digest}  {relative}\n" for digest, relative in rows).encode(),
        unpacked_bytes,
    )


def _stage_group(
    staging: Path,
    group: CorpusGroup,
    manifest: bytes,
    authority_sha256: str,
) -> None:
    staging.chmod(0o755)
    for source, relative in group.files:
        target = staging / group.name / Path(*relative.parts)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.symlink_to(source)
    (staging / "MANIFEST.sha256").write_bytes(manifest)
    (staging / "AUTHORITY.sha256").write_text(authority_sha256 + "\n", encoding="ascii")


def _compress(staging: Path, destination: Path) -> None:
    temporary = destination.with_name(destination.name + f".tmp-{os.getpid()}")
    if destination.exists():
        raise CorpusArchiveError(f"archive already exists: {destination}")
    if temporary.exists():
        raise CorpusArchiveError(f"temporary archive already exists: {temporary}")
    tar_command = (
        "tar",
        "--dereference",
        "--sort=name",
        "--mtime=@0",
        "--clamp-mtime",
        "--owner=0",
        "--group=0",
        "--numeric-owner",
        "--format=posix",
        "--pax-option=delete=atime,delete=ctime",
        "--mode=u+rwX,go+rX,go-w",
        "-C",
        str(staging),
        "-cf",
        "-",
        ".",
    )
    zstd_command = (
        "zstd",
        "-q",
        "-19",
        "--long=31",
        "--threads=8",
        "--check",
        "-o",
        str(temporary),
    )
    try:
        with subprocess.Popen(tar_command, stdout=subprocess.PIPE) as tar_process:
            assert tar_process.stdout is not None
            compressed = subprocess.run(
                zstd_command,
                stdin=tar_process.stdout,
                check=False,
            )
            tar_process.stdout.close()
            tar_status = tar_process.wait()
        if tar_status != 0 or compressed.returncode != 0:
            raise CorpusArchiveError(
                f"archive pipeline failed: tar={tar_status}, zstd={compressed.returncode}"
            )
        if destination.exists():
            raise CorpusArchiveError(f"archive appeared during creation: {destination}")
        os.rename(temporary, destination)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise


def build_corpus_archives(
    farm: FarmSpec,
    corpus_names: Iterable[str],
    output_root: Path,
) -> dict[str, Any]:
    output_root = output_root.resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    selected = list(corpus_names)
    if not selected or len(selected) != len(set(selected)):
        raise CorpusArchiveError("corpus names must be nonempty and unique")
    result: dict[str, Any] = {}
    for corpus_name in selected:
        if corpus_name not in farm.data["corpora"]:
            raise CorpusArchiveError(f"unknown corpus {corpus_name!r}")
        layout = corpus_layout(farm, corpus_name)
        corpus = farm.data["corpora"][corpus_name]
        prevalidated: dict[Path, str] | None = None
        if "authority_receipt" in corpus:
            promotion = validate_corpus_promotion(corpus)
            observed = verify_corpus_bodies(promotion)
            prevalidated = {
                path: digest
                for paths, digests in zip(
                    promotion.physical_paths, observed, strict=True
                )
                for path, digest in zip(paths, digests, strict=True)
            }
        groups: dict[str, Any] = {}
        for group in layout.groups:
            manifest, unpacked_bytes = _manifest(group, prevalidated)
            manifest_sha256 = hashlib.sha256(manifest).hexdigest()
            group_authority = hashlib.sha256(
                canonical_bytes(
                    {
                        "corpus_authority_sha256": layout.authority_sha256,
                        "group": group.name,
                        "manifest_sha256": manifest_sha256,
                    }
                )
            ).hexdigest()
            destination = (
                output_root
                / f"{corpus_name}-{group.name}-zstd19-long31-v1.tar.zst"
            )
            declared = corpus.get("archives", {}).get(group.name)
            generated_authority = {
                "authority_sha256": group_authority,
                "files": len(group.files),
                "manifest_sha256": manifest_sha256,
                "unpacked_bytes": unpacked_bytes,
            }
            if destination.exists():
                if (
                    isinstance(declared, dict)
                    and declared.get("archive") == str(destination)
                    and all(declared.get(key) == value for key, value in generated_authority.items())
                    and declared.get("archive_bytes") == destination.stat().st_size
                    and declared.get("archive_sha256") == _sha256(destination)
                ):
                    groups[group.name] = dict(declared)
                    continue
                raise CorpusArchiveError(
                    f"existing archive is not authenticated by the farm spec: {destination}"
                )
            with tempfile.TemporaryDirectory(
                prefix=f".{corpus_name}-{group.name}-", dir=output_root
            ) as raw_staging:
                staging = Path(raw_staging)
                _stage_group(staging, group, manifest, group_authority)
                _compress(staging, destination)
            observed_manifest, observed_bytes = _manifest(group)
            if observed_manifest != manifest or observed_bytes != unpacked_bytes:
                destination.unlink(missing_ok=True)
                raise CorpusArchiveError(
                    f"corpus {corpus_name}/{group.name} changed during archive creation"
                )
            groups[group.name] = {
                "archive": str(destination),
                "archive_bytes": destination.stat().st_size,
                "archive_sha256": _sha256(destination),
                **generated_authority,
            }
        result[corpus_name] = groups
    return {
        "compression": COMPRESSION,
        "corpora": result,
        "farm_digest": farm.digest,
        "schema": ARCHIVE_SCHEMA,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--farm", required=True)
    parser.add_argument("--corpus", action="append", required=True)
    parser.add_argument("--output-root", required=True)
    parser.add_argument("--receipt", required=True)
    args = parser.parse_args(argv)
    receipt = build_corpus_archives(
        load_farm_spec(args.farm), args.corpus, Path(args.output_root)
    )
    target = Path(args.receipt).resolve()
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(target.name + f".tmp-{os.getpid()}")
    try:
        temporary.write_bytes(canonical_bytes(receipt))
        os.replace(temporary, target)
    finally:
        temporary.unlink(missing_ok=True)
    print(json.dumps(receipt, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
