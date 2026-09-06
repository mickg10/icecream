"""Hermetic farm document for integration unit tests.

The committed example intentionally names retained operator artifacts.  Unit
tests must not require those host-specific paths, so this module preserves the
example's topology and authority while replacing its Firefox corpus authority
with one tiny, fully hash-bound A/B pair under an owned temporary directory.
"""

from __future__ import annotations

import atexit
import hashlib
import json
import os
import shutil
import tempfile
from functools import lru_cache
from pathlib import Path

from farmharness.integration.schema_validation import canonical_bytes


INTEGRATION = Path(__file__).resolve().parents[1]
FIXTURES = Path(__file__).resolve().parent / "fixtures"


def _sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _write_json(path: Path, value: object) -> str:
    path.write_bytes(canonical_bytes(value))
    return _sha(path)


def _archive_authority(corpus: dict[str, object], group: str, manifest: Path) -> str:
    source_authority = {
        key: value
        for key, value in corpus.items()
        if key not in ("archives", "compression")
    }
    return hashlib.sha256(
        canonical_bytes(
            {
                "corpus_authority_sha256": hashlib.sha256(
                    canonical_bytes(source_authority)
                ).hexdigest(),
                "group": group,
                "manifest_sha256": _sha(manifest),
            }
        )
    ).hexdigest()


@lru_cache(maxsize=1)
def example_farm_path() -> Path:
    temporary_root = os.environ.get("ICEFARM_TMPDIR")
    if temporary_root is not None:
        Path(temporary_root).mkdir(parents=True, exist_ok=True)
    root = Path(
        tempfile.mkdtemp(prefix="icefarm-unit-farm-", dir=temporary_root)
    ).resolve()
    atexit.register(shutil.rmtree, root, ignore_errors=True)

    corpus_root = root / "corpus"
    corpus_root.mkdir()
    turn_a = root / "turn-a.ii"
    turn_b = root / "turn-b.ii"
    turn_a.write_bytes(b'int value() { return 1; }\n')
    turn_b.write_bytes(b'int value() { return 2; }\n')
    manifest_a = root / "turn-a.manifest"
    manifest_b = root / "turn-b.manifest"
    manifest_a.write_text(f"{turn_a}\n", encoding="utf-8")
    manifest_b.write_text(f"{turn_b}\n", encoding="utf-8")

    logical = "source/test.ii"
    normalized = hashlib.sha256(f"{logical}\n".encode()).hexdigest()
    pair = root / "pair.json"
    pair_sha = _write_json(
        pair,
        {
            "pairs": [
                {
                    "affected": True,
                    "index": 0,
                    "path": logical,
                    "turn_a_sha256": _sha(turn_a),
                    "turn_b_sha256": _sha(turn_b),
                }
            ],
            "schema": "icefarm-firefox-pair-index-v1",
        },
    )
    selection = root / "selection.json"
    selection_sha = _write_json(
        selection,
        {
            "count": 1,
            "rows": [
                {
                    "audit": "hermetic-fixture",
                    "corrected_trace_logical": 0,
                    "index": 0,
                    "relative": (
                        f"{corpus_root.as_posix().lstrip('/')}/{logical}"
                    ),
                    "turn_a_path": str(turn_a),
                    "turn_a_sha256": _sha(turn_a),
                    "turn_b_path": str(turn_b),
                    "turn_b_sha256": _sha(turn_b),
                }
            ],
            "schema": "icefarm-firefox-selection-v1",
        },
    )

    compiler = (FIXTURES / "test-compiler.sh").resolve()
    compiler_sha = _sha(compiler)
    compiler_arguments = ["-O2"]
    validation = root / "compile-validation.json"
    validation_sha = _write_json(
        validation,
        {
            "compiler": str(compiler),
            "compiler_arguments": compiler_arguments,
            "compiler_sha256": compiler_sha,
            "jobs": 1,
            "rows": [
                {
                    "exit_code": 0,
                    "index": 0,
                    "input_sha256": _sha(turn_a),
                    "output_sha256": "d" * 64,
                    "path": str(turn_a),
                    "timed_out": False,
                    "turn": "A",
                },
                {
                    "exit_code": 0,
                    "index": 0,
                    "input_sha256": _sha(turn_b),
                    "output_sha256": "e" * 64,
                    "path": str(turn_b),
                    "timed_out": False,
                    "turn": "B",
                },
            ],
            "schema": "icefarm-firefox-compile-validation-v1",
            "timeout_s": 1,
        },
    )
    authority = root / "authority.json"
    authority_sha = _write_json(
        authority,
        {
            "compile_validation": {
                "path": str(validation),
                "sha256": validation_sha,
                "turn_a_pass": 1,
                "turn_b_pass": 1,
            },
            "compiler": {
                "arguments": compiler_arguments,
                "path": str(compiler),
                "sha256": compiler_sha,
            },
            "normalized_manifest_sha256": normalized,
            "pair_index": {"path": str(pair), "sha256": pair_sha},
            "schema": "icefarm-firefox-corpus-authority-v1",
            "selection": {"path": str(selection), "sha256": selection_sha},
            "turn_a_manifest": {
                "path": str(manifest_a),
                "sha256": _sha(manifest_a),
            },
            "turn_b_manifest": {
                "path": str(manifest_b),
                "sha256": _sha(manifest_b),
            },
            "tus": 1,
        },
    )

    corpus: dict[str, object] = {
        "kind": "tu-manifest",
        "tus": 1,
        "turn_a_manifest": str(manifest_a),
        "turn_b_manifest": str(manifest_b),
        "normalized_manifest_sha256": normalized,
        "pair_index_sha256": pair_sha,
        "authority_receipt": {"path": str(authority), "sha256": authority_sha},
        "root": str(corpus_root),
        "compression": {
            "codec": "zstd",
            "level": 19,
            "long": 31,
            "threads": 8,
            "checksum": True,
        },
        "compiler_recipes": {
            "fedora-clang-libcxx": {
                "executable": str(compiler),
                "binary_sha256": compiler_sha,
                "configuration_sha256": "c" * 64,
                "version": "icefarm hermetic test compiler v1",
                "arguments": compiler_arguments,
                "toolchain": {
                    "archive": str(root / "toolchain.tar.zst"),
                    "archive_bytes": 1,
                    "archive_sha256": "f" * 64,
                    "compression": {
                        "codec": "zstd",
                        "level": 19,
                        "long": 31,
                        "threads": 8,
                        "checksum": True,
                    },
                    "mount": str(root / "toolchain"),
                    "unpacked_bytes": 1,
                },
            }
        },
    }
    corpus["archives"] = {
        group: {
            "archive": str(root / f"turn-{group.lower()}.tar.zst"),
            "archive_bytes": 1,
            "archive_sha256": hashlib.sha256(
                f"hermetic-{group}".encode()
            ).hexdigest(),
            "authority_sha256": _archive_authority(corpus, group, manifest),
            "files": 1,
            "manifest_sha256": _sha(manifest),
            "unpacked_bytes": source.stat().st_size,
        }
        for group, manifest, source in (
            ("A", manifest_a, turn_a),
            ("B", manifest_b, turn_b),
        )
    }

    document = json.loads(
        (INTEGRATION / "farm.example.json").read_text(encoding="utf-8")
    )
    document["corpora"]["firefox-1000"] = corpus
    farm = root / "farm.json"
    farm.write_bytes(canonical_bytes(document))
    return farm
