from __future__ import annotations

import hashlib
import os
import sys
from pathlib import Path

import pytest

from farmharness.integration.image_transport import TransportArchiveError, save


def _executable(path: Path, body: str) -> None:
    path.write_text(f"#!{sys.executable}\n{body}", encoding="utf-8")
    path.chmod(0o755)


def test_save_streams_docker_through_exact_zstd_policy(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    binaries = tmp_path / "bin"
    binaries.mkdir()
    raw = b"one authenticated docker save stream\n" * 100
    _executable(
        binaries / "docker",
        """
import sys
if sys.argv[1:3] != ["image", "save"]:
    raise SystemExit(91)
sys.stdout.buffer.write(%r)
""" % raw,
    )
    _executable(
        binaries / "zstd",
        """
import sys
if sys.argv[1:] == ["--version"]:
    print("test-zstd-1.5.7")
    raise SystemExit(0)
if sys.argv[1:] != ["-q", "-19", "--long=31", "--threads=8", "--check", "-c"]:
    raise SystemExit(92)
sys.stdout.buffer.write(sys.stdin.buffer.read())
""",
    )
    monkeypatch.setenv("PATH", str(binaries) + os.pathsep + os.environ["PATH"])
    destination = tmp_path / "image.docker.tar.zst"
    receipt = save("icefarm-transport:closure", destination)
    assert destination.read_bytes() == raw
    assert receipt == {
        "compressed_bytes": len(raw),
        "compressed_sha256": hashlib.sha256(raw).hexdigest(),
        "raw_bytes": len(raw),
        "raw_sha256": hashlib.sha256(raw).hexdigest(),
        "zstd": {
            "check": True,
            "level": 19,
            "long": 31,
            "threads": 8,
            "version": "test-zstd-1.5.7",
        },
    }


def test_failed_docker_save_never_publishes_partial_archive(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    binaries = tmp_path / "bin"
    binaries.mkdir()
    _executable(
        binaries / "docker",
        """
import sys
sys.stderr.write("deliberate docker-save failure")
raise SystemExit(7)
""",
    )
    _executable(
        binaries / "zstd",
        """
import sys
if sys.argv[1:] == ["--version"]:
    print("test-zstd-1.5.7")
    raise SystemExit(0)
sys.stdout.buffer.write(sys.stdin.buffer.read())
""",
    )
    monkeypatch.setenv("PATH", str(binaries) + os.pathsep + os.environ["PATH"])
    destination = tmp_path / "failed.docker.tar.zst"
    with pytest.raises(TransportArchiveError, match="docker=7"):
        save("icefarm-transport:closure", destination)
    assert not destination.exists()
    assert list(tmp_path.glob("*.tmp-*")) == []
