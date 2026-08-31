#!/usr/bin/env python3
"""Describe and receipt an immutable S8 supervisor image.

This module does not build, pull, or run Docker.  An operator builds the
Dockerfile, captures ``docker image inspect`` output, and then invokes this
module to make a create-once canonical authority receipt.  The receipt binds
the reviewed Dockerfile/base/tool contract to the resulting image's exact
config ID and platform.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
from pathlib import Path
from typing import Any


SCHEMA = "icecream-s8-supervisor-image-authority-v1"
BASE_REFERENCE = (
    "icecream/farm-node:ubuntu22-gcc11-boost174@"
    "sha256:bdb55d4287a473e3ebfbaa7715a50ee670659777278b8d84c350724e6fa8de58"
)
BASE_DIGEST = "sha256:bdb55d4287a473e3ebfbaa7715a50ee670659777278b8d84c350724e6fa8de58"
REQUIRED_TOOLS = ("python3", "git", "docker")
REQUIRED_PACKAGES = ("python3", "git", "docker.io")
IMAGE_ID = re.compile(r"^sha256:[0-9a-f]{64}$")
COMMIT = re.compile(r"^[0-9a-f]{40}$")
IMAGE_REFERENCE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.:/@-]*$")

DEFAULT_DOCKERFILE = Path(__file__).resolve().parents[1] / "docker/s8-supervisor.Dockerfile"


class ImageAuthorityError(ValueError):
    """The image definition or captured authority is invalid."""


def canonical(value: object) -> bytes:
    """Serialize authority data in the one canonical JSON representation."""
    return (json.dumps(value, sort_keys=True, separators=(",", ":"),
                       ensure_ascii=True, allow_nan=False) + "\n").encode("ascii")


def _regular_file(path: Path, label: str) -> bytes:
    try:
        info = path.lstat()
        if path.is_symlink() or not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
            raise ImageAuthorityError(f"{label}:not_private_regular_file")
        return path.read_bytes()
    except ImageAuthorityError:
        raise
    except OSError as exc:
        raise ImageAuthorityError(f"{label}:unavailable") from exc


def _digest(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def _dockerfile_contract(path: Path) -> dict[str, Any]:
    raw = _regular_file(path, "dockerfile")
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ImageAuthorityError("dockerfile:must_be_utf8") from exc
    froms = []
    for line in text.splitlines():
        match = re.match(r"^FROM\s+(\S+)", line)
        if match:
            froms.append(match.group(1))
    if froms != [BASE_REFERENCE]:
        raise ImageAuthorityError("dockerfile:base_reference_mismatch")
    if "@" not in froms[0] or BASE_DIGEST not in froms[0]:
        raise ImageAuthorityError("dockerfile:base_must_be_digest_pinned")
    missing = [package for package in REQUIRED_PACKAGES
               if not re.search(rf"(?<![A-Za-z0-9_.+-]){re.escape(package)}(?![A-Za-z0-9_.+-])", text)]
    if missing:
        raise ImageAuthorityError("dockerfile:required_package_missing:" + ",".join(missing))
    if "rm -rf /var/lib/apt/lists/*" not in text:
        raise ImageAuthorityError("dockerfile:apt_lists_not_cleaned")
    return {"path": str(path.resolve()), "bytes": len(raw), "sha256": _digest(raw)}


def _inspect_object(raw: bytes) -> dict[str, Any]:
    try:
        value = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ImageAuthorityError("inspect:invalid_json") from exc
    if isinstance(value, list):
        if len(value) != 1:
            raise ImageAuthorityError("inspect:expected_one_image")
        value = value[0]
    if not isinstance(value, dict):
        raise ImageAuthorityError("inspect:expected_object")
    return value


def _image_identity(image_ref: str, inspect: dict[str, Any]) -> dict[str, str]:
    if not isinstance(image_ref, str) or IMAGE_REFERENCE.fullmatch(image_ref) is None:
        raise ImageAuthorityError("image_reference:invalid")
    image_id = inspect.get("Id")
    operating_system = inspect.get("Os")
    architecture = inspect.get("Architecture")
    if not isinstance(image_id, str) or IMAGE_ID.fullmatch(image_id.lower()) is None:
        raise ImageAuthorityError("inspect.Id:invalid")
    if operating_system != "linux" or architecture != "amd64":
        raise ImageAuthorityError("inspect:platform_must_be_linux_amd64")
    return {"reference": image_ref, "image_id": image_id.lower(),
            "os": operating_system, "architecture": architecture}


def make_receipt(*, dockerfile: Path, image_ref: str, inspect_raw: bytes,
                 inspect_path: Path, source_commit: str | None = None) -> dict[str, Any]:
    """Validate inputs and return a canonical, immutable-image authority value."""
    contract = _dockerfile_contract(dockerfile)
    inspect = _inspect_object(inspect_raw)
    identity = _image_identity(image_ref, inspect)
    if source_commit is not None:
        if COMMIT.fullmatch(source_commit.lower()) is None:
            raise ImageAuthorityError("source_commit:invalid")
        source_commit = source_commit.lower()
    receipt: dict[str, Any] = {
        "schema": SCHEMA,
        "image": identity,
        "base": {"reference": BASE_REFERENCE, "digest": BASE_DIGEST},
        "required_tools": list(REQUIRED_TOOLS),
        "dockerfile": contract,
        "inspect": {"path": str(inspect_path.resolve()), "bytes": len(inspect_raw),
                     "sha256": _digest(inspect_raw), "Id": identity["image_id"],
                     "Os": identity["os"], "Architecture": identity["architecture"]},
    }
    if source_commit is not None:
        receipt["source_commit"] = source_commit
    return receipt


def write_once(path: Path, value: object) -> bytes:
    """Create a receipt exactly once; never replace an existing authority."""
    raw = canonical(value)
    try:
        if path.is_symlink() or path.exists():
            raise ImageAuthorityError("receipt:already_exists")
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("xb") as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
    except ImageAuthorityError:
        raise
    except FileExistsError as exc:
        # Preserve create-once semantics even when another writer wins the
        # race between the existence check and O_EXCL (``xb``) creation.
        raise ImageAuthorityError("receipt:already_exists") from exc
    except OSError as exc:
        raise ImageAuthorityError("receipt:create_failed") from exc
    return raw


def build_command(*, context: Path, dockerfile: Path, image_ref: str) -> list[str]:
    """Return the exact non-running build command for the reviewed definition."""
    if not context.is_absolute() or not context.is_dir():
        raise ImageAuthorityError("build_context:directory_required")
    _dockerfile_contract(dockerfile)
    if IMAGE_REFERENCE.fullmatch(image_ref) is None:
        raise ImageAuthorityError("image_reference:invalid")
    return ["docker", "build", "--pull=false", "--platform=linux/amd64",
            "--file", str(dockerfile.resolve()), "--tag", image_ref,
            str(context.resolve())]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dockerfile", type=Path, default=DEFAULT_DOCKERFILE)
    parser.add_argument("--image-ref", required=True)
    parser.add_argument("--inspect-json", type=Path, required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--source-commit")
    args = parser.parse_args(argv)
    try:
        inspect_raw = _regular_file(args.inspect_json, "inspect")
        receipt = make_receipt(dockerfile=args.dockerfile, image_ref=args.image_ref,
                               inspect_raw=inspect_raw, inspect_path=args.inspect_json,
                               source_commit=args.source_commit)
        write_once(args.receipt, receipt)
        print(canonical(receipt).decode("ascii"), end="")
        return 0
    except ImageAuthorityError as exc:
        parser.error(str(exc))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
