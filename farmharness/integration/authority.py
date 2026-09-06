"""Capture and verify the newgen farm authority without mutating farm hosts."""

from __future__ import annotations

import copy
import hashlib
import ipaddress
import json
import os
import re
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path
from typing import Any, Protocol

try:
    from .farm_spec import FarmSpec
    from .remote import CommandResult, PlannedCommand, SubprocessTransport, docker_argv, ssh_argv
    from .schema_validation import canonical_bytes
except ImportError:  # Direct execution from this directory.
    from farm_spec import FarmSpec
    from remote import CommandResult, PlannedCommand, SubprocessTransport, docker_argv, ssh_argv
    from schema_validation import canonical_bytes


CAPTURE_SCHEMA = "icecream-newgen-authority-capture-v2"
PLAN_SCHEMA = "icecream-newgen-authority-capture-plan-v2"
ISO_UTC_RE = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")


class AuthorityCaptureError(RuntimeError):
    """The declared authority could not be authenticated against the farm."""


class Recorder(Protocol):
    def invoke(self, command: PlannedCommand) -> CommandResult:
        ...


HOST_PROBE_SCRIPT = r"""
import fcntl, hashlib, json, os, pathlib, platform, socket, struct
from datetime import datetime, timezone

expected_address = os.environ.get("ICEFARM_EXPECTED_ADDRESS", "")

def digest(raw):
    return hashlib.sha256(raw).hexdigest()

def read_required(path):
    return pathlib.Path(path).read_bytes().strip()

ipv4 = []
nic_rows = []
probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
try:
    for _index, name in socket.if_nameindex():
        if name == "lo":
            continue
        sys_path = pathlib.Path("/sys/class/net") / name
        try:
            target = str(sys_path.resolve(strict=True))
            address = (sys_path / "address").read_text(encoding="ascii").strip().lower()
        except OSError:
            continue
        if "/virtual/" not in target and address != "00:00:00:00:00:00":
            nic_rows.append(f"{name}:{address}:{target}")
        try:
            packed = fcntl.ioctl(
                probe.fileno(), 0x8915, struct.pack("256s", name.encode("ascii")[:15])
            )
        except OSError:
            continue
        ipv4.append(socket.inet_ntoa(packed[20:24]))
finally:
    probe.close()

if expected_address not in ipv4:
    raise SystemExit("declared LAN address is not present")
meminfo = pathlib.Path("/proc/meminfo").read_text(encoding="ascii")
mem_kib = int(next(line.split()[1] for line in meminfo.splitlines() if line.startswith("MemTotal:")))
payload = {
    "arch": platform.machine(),
    "boot_id_sha256": digest(read_required("/proc/sys/kernel/random/boot_id")),
    "captured_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "cpu_count": os.cpu_count(),
    "hostname": socket.gethostname(),
    "ipv4": sorted(set(ipv4)),
    "machine_id_sha256": digest(read_required("/etc/machine-id")),
    "mem_bytes": mem_kib * 1024,
    "nic_identity_sha256": digest(("\n".join(sorted(nic_rows)) + "\n").encode("utf-8")),
    "schema": "icecream-newgen-host-capture-v2",
}
print(json.dumps(payload, sort_keys=True, separators=(",", ":")))
""".strip()


@dataclass(frozen=True)
class _CaptureStep:
    command: PlannedCommand
    kind: str
    host: str


def _authority_sha256(farm: FarmSpec) -> str:
    return hashlib.sha256(canonical_bytes(farm.data["authority"])).hexdigest()


def _capture_steps(farm: FarmSpec, timeout_s: int) -> tuple[_CaptureStep, ...]:
    if not 1 <= timeout_s <= 600:
        raise AuthorityCaptureError("capture timeout must be between 1 and 600 seconds")
    steps: list[_CaptureStep] = []
    sequence = 0
    # Image/source bindings are immutable declarations validated by FarmSpec;
    # their live distribution is authenticated by the separate `images`
    # phase.  Requiring images here would create an authority -> images ->
    # authority bootstrap cycle.  Capture only physical-host and Docker-daemon
    # identity, with no container or image mutation.
    for host_name, host in sorted(farm.hosts.items()):
        probe_argv = (
            "env",
            f"ICEFARM_EXPECTED_ADDRESS={host['lan_ip']}",
            "python3",
            "-c",
            HOST_PROBE_SCRIPT,
        )
        steps.append(
            _CaptureStep(
                PlannedCommand(
                    sequence,
                    "authority.probe-host",
                    host_name,
                    None,
                    "ssh",
                    timeout_s,
                    ssh_argv(farm, host_name, probe_argv),
                ),
                "host",
                host_name,
            )
        )
        sequence += 1
        context = host.get("docker_context")
        steps.append(
            _CaptureStep(
                PlannedCommand(
                    sequence,
                    "authority.probe-docker",
                    host_name,
                    None,
                    "docker-context" if context else "ssh-docker",
                    timeout_s,
                    docker_argv(
                        farm,
                        host_name,
                        ("info", "--format", "{{json .}}"),
                    ),
                ),
                "docker",
                host_name,
            )
        )
        sequence += 1
    return tuple(steps)


def authority_capture_plan(farm: FarmSpec, *, timeout_s: int = 120) -> dict[str, Any]:
    """Return the stable, side-effect-free command plan for a capture."""

    steps = _capture_steps(farm, timeout_s)
    return {
        "authority_sha256": _authority_sha256(farm),
        "commands": [step.command.as_dict() for step in steps],
        "farm_digest": farm.digest,
        "schema": PLAN_SCHEMA,
    }


def _load_json_object(raw: str, subject: str) -> dict[str, Any]:
    try:
        value = json.loads(raw)
    except (json.JSONDecodeError, UnicodeError) as exc:
        raise AuthorityCaptureError(f"{subject} did not return JSON") from exc
    if not isinstance(value, dict):
        raise AuthorityCaptureError(f"{subject} did not return one JSON object")
    return value


def _parse_host_capture(
    raw: str, farm: FarmSpec, host_name: str, *, now: datetime
) -> dict[str, Any]:
    value = _load_json_object(raw.strip(), f"host {host_name}")
    required = {
        "arch",
        "boot_id_sha256",
        "captured_at",
        "cpu_count",
        "hostname",
        "ipv4",
        "machine_id_sha256",
        "mem_bytes",
        "nic_identity_sha256",
        "schema",
    }
    if set(value) != required or value.get("schema") != "icecream-newgen-host-capture-v2":
        raise AuthorityCaptureError(f"host {host_name} returned an invalid capture shape")
    host = farm.hosts[host_name]
    for field in ("boot_id_sha256", "machine_id_sha256", "nic_identity_sha256"):
        item = value[field]
        if not isinstance(item, str) or re.fullmatch(r"[0-9a-f]{64}", item) is None:
            raise AuthorityCaptureError(f"host {host_name} has invalid {field}")
    if value["arch"] != host["arch"] or value["cpu_count"] != host["cores"]:
        raise AuthorityCaptureError(f"host {host_name} CPU identity differs from farm policy")
    if (
        not isinstance(value["mem_bytes"], int)
        or isinstance(value["mem_bytes"], bool)
        or value["mem_bytes"] < int(host["mem_gb"] * (1024**3) * 0.80)
    ):
        raise AuthorityCaptureError(f"host {host_name} memory is below farm policy")
    if (
        not isinstance(value["ipv4"], list)
        or not all(isinstance(item, str) for item in value["ipv4"])
        or host["lan_ip"] not in value["ipv4"]
    ):
        raise AuthorityCaptureError(f"host {host_name} does not own its declared LAN address")
    try:
        addresses = [ipaddress.IPv4Address(item) for item in value["ipv4"]]
    except ipaddress.AddressValueError as exc:
        raise AuthorityCaptureError(f"host {host_name} returned an invalid IPv4 address") from exc
    if len(addresses) != len(set(addresses)):
        raise AuthorityCaptureError(f"host {host_name} returned duplicate IPv4 addresses")
    captured_at = value["captured_at"]
    if not isinstance(captured_at, str) or ISO_UTC_RE.fullmatch(captured_at) is None:
        raise AuthorityCaptureError(f"host {host_name} has invalid capture time")
    captured = datetime.strptime(captured_at, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=UTC)
    age = (now - captured).total_seconds()
    if age < -30 or age > 300:
        raise AuthorityCaptureError(f"host {host_name} capture is stale")
    physical = hashlib.sha256(
        canonical_bytes(
            {
                "machine_id_sha256": value["machine_id_sha256"],
                "nic_identity_sha256": value["nic_identity_sha256"],
            }
        )
    ).hexdigest()
    return {
        "address": host["lan_ip"],
        "arch": value["arch"],
        "boot_id_sha256": value["boot_id_sha256"],
        "captured_at": captured_at,
        "cpu_count": value["cpu_count"],
        "hostname": value["hostname"],
        "machine_id_sha256": value["machine_id_sha256"],
        "mem_bytes": value["mem_bytes"],
        "nic_identity_sha256": value["nic_identity_sha256"],
        "physical_host_sha256": physical,
    }


def _parse_docker_info(raw: str, farm: FarmSpec, host_name: str) -> dict[str, Any]:
    subject = f"host {host_name} Docker daemon"
    value = _load_json_object(raw.strip(), subject)
    required = ("Architecture", "DockerRootDir", "MemTotal", "NCPU", "Name", "OSType", "ServerVersion")
    if any(key not in value for key in required):
        raise AuthorityCaptureError(f"{subject} returned incomplete identity")
    expected_arch = farm.hosts[host_name]["arch"]
    observed_arch = value["Architecture"]
    arch_equivalent = {"amd64": "x86_64", "x86_64": "x86_64"}
    if arch_equivalent.get(observed_arch, observed_arch) != arch_equivalent.get(
        expected_arch, expected_arch
    ):
        raise AuthorityCaptureError(f"{subject} architecture differs from farm policy")
    if value["OSType"] != "linux":
        raise AuthorityCaptureError(f"{subject} is not a Linux daemon")
    if value["NCPU"] != farm.hosts[host_name]["cores"]:
        raise AuthorityCaptureError(f"{subject} CPU count differs from farm policy")
    if not isinstance(value["MemTotal"], int) or isinstance(value["MemTotal"], bool):
        raise AuthorityCaptureError(f"{subject} returned invalid memory capacity")
    for key in ("DockerRootDir", "Name", "ServerVersion"):
        if not isinstance(value[key], str) or not value[key]:
            raise AuthorityCaptureError(f"{subject} returned invalid {key}")
    return {
        "arch": observed_arch,
        "mem_bytes": value["MemTotal"],
        "name": value["Name"],
        "os": value["OSType"],
        "root_dir": value["DockerRootDir"],
        "server_version": value["ServerVersion"],
    }


def _write_once(path: Path, raw: bytes, subject: str) -> None:
    if path.exists() or path.is_symlink():
        raise AuthorityCaptureError(f"{subject} already exists")
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    try:
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, "wb") as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
    except FileExistsError as exc:
        raise AuthorityCaptureError(f"{subject} already exists") from exc
    except OSError as exc:
        raise AuthorityCaptureError(f"cannot create {subject}: {exc}") from exc


def _safe_output_path(path: Path, subject: str) -> Path:
    path = path.absolute()
    for ancestor in (path, *path.parents):
        if ancestor.is_symlink():
            raise AuthorityCaptureError(f"{subject} has a symlink ancestor")
    return path


def capture_authority(
    farm: FarmSpec,
    *,
    output: Path,
    descriptor_dir: Path,
    timeout_s: int = 120,
    recorder: Recorder | None = None,
    now: datetime | None = None,
) -> dict[str, Any]:
    """Authenticate every declared host/image and write one complete farm file."""

    output = _safe_output_path(output, "authority output")
    descriptor_dir = _safe_output_path(
        descriptor_dir, "authority descriptor directory"
    )
    if output.exists() or output.is_symlink():
        raise AuthorityCaptureError("authority output already exists")
    if descriptor_dir.exists() or descriptor_dir.is_symlink():
        raise AuthorityCaptureError("authority descriptor directory already exists")
    steps = _capture_steps(farm, timeout_s)
    transport = recorder or SubprocessTransport()
    observed_at = now or datetime.now(UTC)
    hosts: dict[str, dict[str, Any]] = {}
    for step in steps:
        result = transport.invoke(step.command)
        if step.kind == "host":
            hosts[step.host] = _parse_host_capture(
                result.stdout, farm, step.host, now=observed_at
            )
            continue
        if step.host not in hosts:
            raise AuthorityCaptureError(f"Docker probe for {step.host} preceded host identity")
        hosts[step.host]["docker"] = _parse_docker_info(result.stdout, farm, step.host)
    if set(hosts) != set(farm.hosts):
        raise AuthorityCaptureError("captured host set differs from farm policy")
    physical = [host["physical_host_sha256"] for host in hosts.values()]
    if len(physical) != len(set(physical)):
        raise AuthorityCaptureError("multiple farm names resolve to one physical host")
    capture = {
        "authority_sha256": _authority_sha256(farm),
        "captured_at": observed_at.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "hosts": dict(sorted(hosts.items())),
        "schema": CAPTURE_SCHEMA,
    }
    document = copy.deepcopy(farm.data)
    document["authority_capture"] = capture
    descriptor_dir.mkdir(parents=True, mode=0o700)
    for host_name, descriptor in sorted(hosts.items()):
        _write_once(
            descriptor_dir / f"{host_name}.json",
            canonical_bytes(descriptor),
            f"authority descriptor for {host_name}",
        )
    _write_once(output, canonical_bytes(document), "authority output")
    return {
        "authority_sha256": capture["authority_sha256"],
        "capture_sha256": hashlib.sha256(canonical_bytes(capture)).hexdigest(),
        "commands": [step.command.as_dict() for step in steps],
        "farm_sha256": hashlib.sha256(canonical_bytes(document)).hexdigest(),
        "hosts": sorted(hosts),
        "output": str(output),
        "schema": CAPTURE_SCHEMA,
    }


def render_capture_plan(farm: FarmSpec, *, timeout_s: int = 120) -> str:
    return canonical_bytes(authority_capture_plan(farm, timeout_s=timeout_s)).decode()
