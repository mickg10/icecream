from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest

from farmharness.integration.corpus_archive import COMPRESSION, _compress


def _executable(path: Path, body: str) -> None:
    path.write_text(f"#!{sys.executable}\n{body}", encoding="utf-8")
    path.chmod(0o755)


def test_corpus_archive_uses_exact_zstd19_long31_policy(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    binaries = tmp_path / "bin"
    binaries.mkdir()
    raw = b"deterministic corpus tar stream\n" * 10
    _executable(
        binaries / "tar",
        """
import sys
sys.stdout.buffer.write(%r)
"""
        % raw,
    )
    _executable(
        binaries / "zstd",
        """
import pathlib
import sys
expected = ["-q", "-19", "--long=31", "--threads=8", "--check", "-o"]
if sys.argv[1:7] != expected or len(sys.argv) != 8:
    raise SystemExit(92)
pathlib.Path(sys.argv[7]).write_bytes(sys.stdin.buffer.read())
""",
    )
    monkeypatch.setenv("PATH", str(binaries) + os.pathsep + os.environ["PATH"])
    staging = tmp_path / "staging"
    staging.mkdir()
    destination = tmp_path / "corpus.tar.zst"

    _compress(staging, destination)

    assert destination.read_bytes() == raw
    assert COMPRESSION == {
        "checksum": True,
        "codec": "zstd",
        "level": 19,
        "long": 31,
        "threads": 8,
    }
