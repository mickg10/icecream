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
GIT_SHA = re.compile(r"^[0-9a-f]{40}$")
SAFE_IMAGE = re.compile(r"^[A-Za-z0-9_.:/@-]+$")
DEFAULT_SOCKET = Path("/var/run/docker.sock")
DEFAULT_MACHINE_ID = Path("/etc/machine-id")
DEFAULT_CPUINFO = Path("/proc/cpuinfo")
DEFAULT_DMI = (Path("/sys/class/dmi/id/product_uuid"),
               Path("/sys/class/dmi/id/board_serial"))
# Must match s8_campaign_driver.LIVE_LOCK_PATH.  This is host-global, not a
# caller-selected campaign or container-temp namespace.
LIVE_LOCK_PATH = Path("/tmp/icecream-s8-live-run.lock")
LAUNCHER_PID_ENV = "ICECC_S8_PROTECTED_LAUNCHER_PID"


class LauncherError(ValueError):
    """A protected launch cannot be expressed or authenticated."""


def _private_file(path: Path, label: str) -> None:
    if not path.is_absolute():
        raise LauncherError(f"{label}:relative_path")
    for ancestor in (path, *path.parents):
        try:
            if ancestor.is_symlink():
                raise LauncherError(f"{label}:symlink_ancestor")
        except OSError as exc:
            raise LauncherError(f"{label}:unavailable") from exc
    try:
        info = path.lstat()
    except OSError as exc:
        raise LauncherError(f"{label}:unavailable") from exc
    if path.is_symlink() or not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
        raise LauncherError(f"{label}:not_private_regular_file")


def _private_dir(path: Path, label: str) -> Path:
    if not path.is_absolute():
        raise LauncherError(f"{label}:relative_path")
    for ancestor in (path, *path.parents):
        try:
            if ancestor.is_symlink():
                raise LauncherError(f"{label}:symlink_ancestor")
        except OSError as exc:
            raise LauncherError(f"{label}:unavailable") from exc
    if not path.is_dir():
        raise LauncherError(f"{label}:unavailable")
    return path


def _socket(path: Path) -> None:
    if not path.is_absolute():
        raise LauncherError("docker_socket:relative_path")
    for ancestor in (path, *path.parents):
        try:
            if ancestor.is_symlink():
                raise LauncherError("docker_socket:symlink_ancestor")
        except OSError as exc:
            raise LauncherError("docker_socket:unavailable") from exc
    try:
        info = path.lstat()
    except OSError as exc:
        raise LauncherError("docker_socket:unavailable") from exc
    if path.is_symlink() or not stat.S_ISSOCK(info.st_mode):
        raise LauncherError("docker_socket:not_socket")


def _option_values(argv: list[str], option: str) -> list[str]:
    values: list[str] = []
    for index, value in enumerate(argv):
        if value == option:
            if index + 1 >= len(argv) or argv[index + 1].startswith("--"):
                raise LauncherError(f"campaign_argv:{option[2:]}_invalid")
            values.append(argv[index + 1])
    return values


def _validate_campaign_argv(argv: list[str], workspace: Path) -> tuple[Path, Path]:
    """Authenticate the supervisor's exact Python driver and authority paths."""
    if len(argv) < 2 or Path(argv[0]).name not in {"python", "python3"}:
        raise LauncherError("campaign_argv:python_driver_required")
    expected_driver = workspace / "research" / "farmharness" / "s8_campaign_driver.py"
    if argv[1] != str(expected_driver):
        raise LauncherError("campaign_argv:driver_path_mismatch")
    mode_values = _option_values(argv, "--mode")
    if len(mode_values) != 1 or mode_values[0] != "all" or "--mode=all" in argv:
        raise LauncherError("campaign_argv:exactly_one_all_mode_required")
    authority_values = _option_values(argv, "--simulator-authority")
    if len(authority_values) != 1:
        raise LauncherError("campaign_argv:simulator_authority_required")
    repo_values = _option_values(argv, "--repo")
    if len(repo_values) > 1 or (repo_values and repo_values[0] != str(workspace)):
        raise LauncherError("campaign_argv:repo_path_mismatch")
    authority = Path(authority_values[0])
    if not authority.is_absolute():
        raise LauncherError("simulator_authority:relative_path")
    return authority, workspace


def _validate_simulator_binding(receipt_path: Path, repo: Path) -> Path:
    """Validate and return the exact repo-linked simulator binary."""
    expected_receipt = repo / "cache" / "sim" / ".p50sim-build.json"
    if receipt_path != expected_receipt:
        raise LauncherError("simulator_authority:path_mismatch")
    _private_file(receipt_path, "simulator_authority")
    try:
        value = json.loads(receipt_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise LauncherError("simulator_authority:invalid_json") from exc
    if (not isinstance(value, dict) or
            set(value) != {"schema", "source", "binary", "inputs", "configuration"} or
            value.get("schema") != "icecream-p50sim-build-v1" or
            not isinstance(value.get("source"), dict) or
            set(value["source"]) != {"root", "head", "tree", "tracked_clean"} or
            value["source"].get("root") != str(repo) or
            not isinstance(value["source"].get("head"), str) or
            GIT_SHA.fullmatch(value["source"]["head"]) is None or
            not isinstance(value["source"].get("tree"), str) or
            GIT_SHA.fullmatch(value["source"]["tree"]) is None or
            value["source"].get("tracked_clean") is not True):
        raise LauncherError("simulator_authority:source_mismatch")
    binary = value.get("binary")
    expected_binary = repo / "cache" / "sim" / ".p50sim.bin"
    if (not isinstance(binary, dict) or set(binary) != {"path", "sha256", "bytes"} or
            binary.get("path") != str(expected_binary)):
        raise LauncherError("simulator_authority:binary_path_mismatch")
    _private_file(expected_binary, "simulator_authority_binary")
    observed = _sha(expected_binary)
    if (observed != str(binary.get("sha256", "")).lower() or
            expected_binary.stat().st_size != binary.get("bytes")):
        raise LauncherError("simulator_authority:binary_mismatch")
    return expected_binary


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
                  launcher_pid: int | None = None,
                  docker_group_supported: bool = False) -> list[str]:
    """Return the exact supervisor Docker argv; never execute it."""
    if not campaign_argv:
        raise LauncherError("campaign_argv:required")
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
    simulator_authority, repo = _validate_campaign_argv(campaign_argv, workspace)
    simulator_binary = _validate_simulator_binding(simulator_authority, repo)
    for path, label in ((machine_id, "host_machine_id"), (cpuinfo, "host_cpuinfo")):
        _private_file(path, label)
    dmi_paths = dmi_paths if dmi_paths is not None else tuple(
        path.resolve() for path in DEFAULT_DMI if path.is_file() and not path.is_symlink())
    for path in dmi_paths:
        _private_file(path, "host_dmi")
    _socket(docker_socket)
    _private_file(LIVE_LOCK_PATH, "live_lock")
    uid = os.geteuid() if owner_uid is None else owner_uid
    gid = os.getegid() if owner_gid is None else owner_gid
    socket_gid = _docker_group_id(docker_socket) if docker_gid is None else docker_gid
    owner_pid = os.getpid() if launcher_pid is None else launcher_pid
    if min(uid, gid, socket_gid) < 0 or type(owner_pid) is not int or owner_pid <= 1:
        raise LauncherError("owner_identity:invalid")
    script = "for tool in python3 docker git; do command -v \"$tool\" >/dev/null 2>&1 || { echo supervisor_tool_missing:$tool >&2; exit 78; }; done\n"
    script += "exec " + shlex.join(campaign_argv) + "\n"
    command = ["docker", "run", "--rm", "--init", "--pid=host", "--network=host",
               "--entrypoint", "/bin/sh",
               "--workdir", str(workspace),
               "--user", f"{uid}:{gid}", "--group-add", str(socket_gid),
               "--oom-score-adj=-1000", "--env", "PYTHONUNBUFFERED=1",
               "--env", f"{LAUNCHER_PID_ENV}={owner_pid}"]
    for path in (workspace, experiment_root, container_temp_root):
        command.extend(("--volume", f"{path}:{path}:rw"))
    command.extend(("--volume", f"{docker_socket}:{docker_socket}:rw",
                    "--volume", f"{machine_id}:{machine_id}:ro",
                    "--volume", f"{cpuinfo}:{cpuinfo}:ro",
                    "--volume", f"{LIVE_LOCK_PATH}:{LIVE_LOCK_PATH}:rw",
                    "--volume", f"{simulator_authority}:{simulator_authority}:ro",
                    "--volume", f"{simulator_binary}:{simulator_binary}:ro"))
    for path in dmi_paths:
        command.extend(("--volume", f"{path}:{path}:ro"))
    command.extend((image_identity["image_id"], "-lc", script))
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
