#!/usr/bin/env python3
"""Build a protected, host-visible supervisor launch for S8 all-mode.

The launcher is declarative by default.  Its command runs the existing S8
campaign driver in a Docker-daemon-created supervisor with the host PID and
network views, so the driver's process and CPU gates remain physical-host
observations.  The supervisor owns no output as root: it runs at the caller's
UID/GID and receives the Docker socket's supplementary group.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import stat
import subprocess
import sys
from pathlib import Path
from typing import Any, Callable


SCHEMA = "icecream-s8-protected-launcher-v1"
IMAGE_ID = re.compile(r"^sha256:[0-9a-f]{64}$")
SAFE_IMAGE = re.compile(r"^[A-Za-z0-9_.:/@-]+$")
DEFAULT_SOCKET = Path("/var/run/docker.sock")
DEFAULT_MACHINE_ID = Path("/etc/machine-id")
DEFAULT_CPUINFO = Path("/proc/cpuinfo")
DEFAULT_DMI = (Path("/sys/class/dmi/id/product_uuid"),
               Path("/sys/class/dmi/id/board_serial"))


class LauncherError(ValueError):
    """A protected launch cannot be expressed or authenticated."""


def _private_file(path: Path, label: str) -> None:
    try:
        info = path.lstat()
    except OSError as exc:
        raise LauncherError(f"{label}:unavailable") from exc
    if path.is_symlink() or not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
        raise LauncherError(f"{label}:not_private_regular_file")


def _private_dir(path: Path, label: str) -> Path:
    if not path.is_absolute() or path.is_symlink() or not path.is_dir():
        raise LauncherError(f"{label}:unavailable")
    return path


def _socket(path: Path) -> None:
    try:
        info = path.lstat()
    except OSError as exc:
        raise LauncherError("docker_socket:unavailable") from exc
    if path.is_symlink() or not stat.S_ISSOCK(info.st_mode):
        raise LauncherError("docker_socket:not_socket")


def _sha(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1 << 20), b""):
                digest.update(block)
    except OSError as exc:
        raise LauncherError(f"file:unavailable:{path}") from exc
    return digest.hexdigest()


def resolve_supervisor_image(image: str, expected_id: str,
                             inspector: Callable[[str], dict[str, Any]] | None = None
                             ) -> dict[str, str]:
    """Resolve an image reference to its exact content ID by read-only inspect."""
    if not isinstance(image, str) or not SAFE_IMAGE.fullmatch(image):
        raise LauncherError("supervisor_image:reference_invalid")
    if not isinstance(expected_id, str) or IMAGE_ID.fullmatch(expected_id.lower()) is None:
        raise LauncherError("supervisor_image:content_id_required")
    if inspector is None:
        def inspect(reference: str) -> dict[str, Any]:
            try:
                completed = subprocess.run(
                    ["docker", "image", "inspect", reference], check=True,
                    capture_output=True, text=True, timeout=30)
                values = json.loads(completed.stdout)
            except (OSError, subprocess.SubprocessError, json.JSONDecodeError) as exc:
                raise LauncherError("supervisor_image:inspect_failed") from exc
            if not isinstance(values, list) or len(values) != 1 or not isinstance(values[0], dict):
                raise LauncherError("supervisor_image:inspect_ambiguous")
            return values[0]
        inspector = inspect
    value = inspector(image)
    actual = value.get("Id")
    if (not isinstance(actual, str) or IMAGE_ID.fullmatch(actual.lower()) is None or
            actual.lower() != expected_id.lower()):
        raise LauncherError("supervisor_image:content_id_mismatch")
    if value.get("Architecture") != "amd64" or value.get("Os") != "linux":
        raise LauncherError("supervisor_image:platform_invalid")
    return {"reference": image, "image_id": actual.lower(),
            "architecture": "amd64", "os": "linux",
            "created": str(value.get("Created", ""))}


def _docker_group_id(path: Path) -> int:
    try:
        gid = path.stat().st_gid
    except OSError as exc:
        raise LauncherError("docker_socket:gid_unavailable") from exc
    if gid < 0:
        raise LauncherError("docker_socket:gid_invalid")
    return gid


def build_command(campaign_argv: list[str], *, workspace: Path,
                  experiment_root: Path, container_temp_root: Path,
                  image_identity: dict[str, str],
                  docker_socket: Path = DEFAULT_SOCKET,
                  machine_id: Path = DEFAULT_MACHINE_ID,
                  cpuinfo: Path = DEFAULT_CPUINFO,
                  dmi_paths: tuple[Path, ...] | None = None,
                  owner_uid: int | None = None, owner_gid: int | None = None,
                  docker_gid: int | None = None,
                  docker_group_supported: bool = False) -> list[str]:
    """Return the exact supervisor Docker argv; never execute it."""
    if not campaign_argv:
        raise LauncherError("campaign_argv:required")
    if not any(item == "all" or item == "--mode=all" for item in campaign_argv):
        try:
            mode_index = campaign_argv.index("--mode")
        except ValueError:
            mode_index = -1
        if mode_index < 0 or mode_index + 1 >= len(campaign_argv) or campaign_argv[mode_index + 1] != "all":
            raise LauncherError("campaign_argv:all_mode_required")
    if (set(image_identity) != {"reference", "image_id", "architecture", "os", "created"} or
            not isinstance(image_identity.get("reference"), str) or
            SAFE_IMAGE.fullmatch(image_identity["reference"]) is None or
            IMAGE_ID.fullmatch(image_identity.get("image_id", "")) is None or
            image_identity.get("architecture") != "amd64" or
            image_identity.get("os") != "linux" or not image_identity.get("created")):
        raise LauncherError("supervisor_image:identity_invalid")
    if not docker_group_supported:
        raise LauncherError("docker_group:support_not_authenticated")
    workspace = _private_dir(workspace, "workspace")
    experiment_root = _private_dir(experiment_root, "experiment_root")
    container_temp_root = _private_dir(container_temp_root, "container_temp_root")
    for path, label in ((machine_id, "host_machine_id"), (cpuinfo, "host_cpuinfo")):
        _private_file(path, label)
    dmi_paths = dmi_paths if dmi_paths is not None else tuple(
        path for path in DEFAULT_DMI if path.is_file() and not path.is_symlink())
    for path in dmi_paths:
        _private_file(path, "host_dmi")
    _socket(docker_socket)
    uid = os.geteuid() if owner_uid is None else owner_uid
    gid = os.getegid() if owner_gid is None else owner_gid
    socket_gid = _docker_group_id(docker_socket) if docker_gid is None else docker_gid
    if min(uid, gid, socket_gid) < 0:
        raise LauncherError("owner_identity:invalid")
    script = "for tool in python3 docker git; do command -v \"$tool\" >/dev/null 2>&1 || { echo supervisor_tool_missing:$tool >&2; exit 78; }; done\n"
    script += "exec " + shlex.join(campaign_argv) + "\n"
    command = ["docker", "run", "--rm", "--init", "--pid=host", "--network=host",
               "--entrypoint", "/bin/sh",
               "--user", f"{uid}:{gid}", "--group-add", str(socket_gid),
               "--oom-score-adj=-1000", "--env", "PYTHONUNBUFFERED=1"]
    for path in (workspace, experiment_root, container_temp_root):
        command.extend(("--volume", f"{path}:{path}:rw"))
    command.extend(("--volume", f"{docker_socket}:{docker_socket}:rw",
                    "--volume", f"{machine_id}:{machine_id}:ro",
                    "--volume", f"{cpuinfo}:{cpuinfo}:ro"))
    for path in dmi_paths:
        command.extend(("--volume", f"{path}:{path}:ro"))
    command.extend((image_identity["image_id"], "/bin/sh", "-lc", script))
    return command


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--campaign-argv-json", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--experiment-root", type=Path, required=True)
    parser.add_argument("--container-temp-root", type=Path, required=True)
    parser.add_argument("--supervisor-image", required=True)
    parser.add_argument("--supervisor-image-id", required=True)
    parser.add_argument("--docker-group-supported", action="store_true")
    parser.add_argument("--execute", action="store_true")
    parser.add_argument("--docker-socket", type=Path, default=DEFAULT_SOCKET)
    args = parser.parse_args(argv)
    try:
        raw = args.campaign_argv_json.read_bytes()
        campaign_argv = json.loads(raw)
        if not isinstance(campaign_argv, list) or not all(isinstance(item, str) for item in campaign_argv):
            raise LauncherError("campaign_argv:invalid_json")
        identity = resolve_supervisor_image(args.supervisor_image, args.supervisor_image_id)
        command = build_command(
            campaign_argv, workspace=args.workspace, experiment_root=args.experiment_root,
            container_temp_root=args.container_temp_root, image_identity=identity,
            docker_socket=args.docker_socket,
            docker_group_supported=args.docker_group_supported)
        envelope = {"schema": SCHEMA, "status": "EXECUTE" if args.execute else "DRY_RUN",
                    "command": command, "campaign_argv": campaign_argv, "image": identity,
                    "campaign_argv_sha256": hashlib.sha256(raw).hexdigest()}
        print(json.dumps(envelope, sort_keys=True))
        if not args.execute:
            return 0
        completed = subprocess.run(command, check=False)
        return int(completed.returncode)
    except (LauncherError, OSError, UnicodeError, json.JSONDecodeError) as exc:
        print(f"s8_protected_launcher: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
