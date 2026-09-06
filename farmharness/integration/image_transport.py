#!/usr/bin/env python3
"""Create authenticated long-window zstd Docker transport archives."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import BinaryIO


ZSTD_LEVEL = 19
ZSTD_LONG = 31
ZSTD_THREADS = 8
CHUNK_BYTES = 1024 * 1024


class TransportArchiveError(RuntimeError):
    """A Docker save stream could not be archived without ambiguity."""


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(CHUNK_BYTES), b""):
            digest.update(block)
    return digest.hexdigest()


def _copy_and_hash(source: BinaryIO, destination: BinaryIO) -> tuple[int, str]:
    digest = hashlib.sha256()
    total = 0
    while True:
        block = source.read(CHUNK_BYTES)
        if not block:
            break
        digest.update(block)
        destination.write(block)
        total += len(block)
    return total, digest.hexdigest()


def save(reference: str, destination: Path) -> dict[str, object]:
    """Stream exactly one Docker reference through the pinned zstd policy."""

    destination = destination.resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + f".tmp-{os.getpid()}")
    docker_stderr = destination.with_name(destination.name + f".docker-stderr-{os.getpid()}")
    zstd_stderr = destination.with_name(destination.name + f".zstd-stderr-{os.getpid()}")
    docker: subprocess.Popen[bytes] | None = None
    zstd: subprocess.Popen[bytes] | None = None
    raw_bytes = 0
    raw_sha256 = ""
    try:
        version = subprocess.run(
            ["zstd", "--version"],
            check=True,
            capture_output=True,
            encoding="utf-8",
            errors="replace",
            shell=False,
        ).stdout.strip()
        with (
            docker_stderr.open("w+b") as docker_error,
            zstd_stderr.open("w+b") as zstd_error,
            temporary.open("w+b") as compressed,
        ):
            docker = subprocess.Popen(
                ["docker", "image", "save", reference],
                stdout=subprocess.PIPE,
                stderr=docker_error,
                shell=False,
            )
            zstd = subprocess.Popen(
                [
                    "zstd",
                    "-q",
                    f"-{ZSTD_LEVEL}",
                    f"--long={ZSTD_LONG}",
                    f"--threads={ZSTD_THREADS}",
                    "--check",
                    "-c",
                ],
                stdin=subprocess.PIPE,
                stdout=compressed,
                stderr=zstd_error,
                shell=False,
            )
            if docker.stdout is None or zstd.stdin is None:
                raise TransportArchiveError("cannot open Docker/zstd pipeline")
            try:
                raw_bytes, raw_sha256 = _copy_and_hash(docker.stdout, zstd.stdin)
            except (BrokenPipeError, OSError) as exc:
                raise TransportArchiveError(f"Docker/zstd pipeline copy failed: {exc}") from exc
            finally:
                docker.stdout.close()
                zstd.stdin.close()
            docker_status = docker.wait()
            zstd_status = zstd.wait()
            compressed.flush()
            os.fsync(compressed.fileno())
            if docker_status != 0 or zstd_status != 0:
                docker_error.seek(0)
                zstd_error.seek(0)
                details = (
                    docker_error.read().decode("utf-8", "replace")[-2000:]
                    + zstd_error.read().decode("utf-8", "replace")[-2000:]
                ).strip()
                raise TransportArchiveError(
                    f"Docker/zstd pipeline failed: docker={docker_status}, "
                    f"zstd={zstd_status}: {details}"
                )
        os.replace(temporary, destination)
        directory_fd = os.open(destination.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
        compressed_bytes = destination.stat().st_size
        return {
            "compressed_bytes": compressed_bytes,
            "compressed_sha256": _sha256(destination),
            "raw_bytes": raw_bytes,
            "raw_sha256": raw_sha256,
            "zstd": {
                "check": True,
                "level": ZSTD_LEVEL,
                "long": ZSTD_LONG,
                "threads": ZSTD_THREADS,
                "version": version,
            },
        }
    except (OSError, subprocess.CalledProcessError) as exc:
        raise TransportArchiveError(f"cannot create {destination}: {exc}") from exc
    finally:
        if docker is not None and docker.poll() is None:
            docker.kill()
            docker.wait()
        if zstd is not None and zstd.poll() is None:
            zstd.kill()
            zstd.wait()
        temporary.unlink(missing_ok=True)
        docker_stderr.unlink(missing_ok=True)
        zstd_stderr.unlink(missing_ok=True)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("save", choices=("save",))
    parser.add_argument("--reference", required=True)
    parser.add_argument("--output", required=True)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        receipt = save(args.reference, Path(args.output))
    except TransportArchiveError as exc:
        print(f"image transport failure: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(receipt, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
