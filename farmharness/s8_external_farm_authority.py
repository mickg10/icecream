#!/usr/bin/env python3
"""Create authenticated external-farm host authority (read-only by default)."""
from __future__ import annotations
import argparse
import datetime as dt
import hashlib
import json
import os
import re
import stat
import subprocess
from pathlib import Path
from typing import Mapping, Sequence

try:
    from . import s4_multihost_c1f4 as s4
except ImportError:
    import s4_multihost_c1f4 as s4

SCHEMA = "icecream-s8-external-farm-authority-v1"
HOST_DESCRIPTOR_SCHEMA = "icecream-s8-external-host-descriptor-v1"
CAPTURE_SCHEMA = "icecream-s8-external-host-capture-v1"
HOSTS = ("q3", "q2", "research6", "research7")
F_HOSTS = ("q2", "research6", "research7")
ROLE_PATHS = ("scheduler/icecc-scheduler", "daemon/iceccd", "client/icecc",
              "client/icecc-create-env", "cache/icecc-cache-service")
EXPECTED_CPU_COUNTS = {"q3": 32, "q2": 32, "research6": 20, "research7": 12}
PINNED_IMAGE = s4.PINNED_IMAGE
IMAGE_INDEX = s4.EXPECTED_IMAGE_ID
IMAGE_CONFIG = s4.EXPECTED_IMAGE_CONFIG_ID
IDLE_LOAD_THRESHOLD = 0.50
MIN_IDLE_PERCENT = 95.0
MAX_CAPTURE_AGE_SECONDS = 300.0
HEX64 = re.compile(r"^[0-9a-f]{64}$")
IMAGE_ID = re.compile(r"^sha256:[0-9a-f]{64}$")
ISO_UTC = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")
DEFAULT_PARALLEL = ("q2",) * 15 + ("research7",) * 5
RESEARCH6_PARALLEL = ("q2",) * 10 + ("research6",) * 6 + ("research7",) * 4


class AuthorityError(ValueError):
    """An authority input is missing, stale, or fails an identity gate."""


def canonical(value: object) -> bytes:
    try:
        return (json.dumps(value, sort_keys=True, separators=(",", ":"),
                           ensure_ascii=True, allow_nan=False) + "\n").encode("ascii")
    except (TypeError, ValueError, OverflowError, UnicodeError) as exc:
        raise AuthorityError("canonical_json:invalid") from exc


def digest(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def _hex(value: object, label: str) -> str:
    if not isinstance(value, str) or HEX64.fullmatch(value.lower()) is None:
        raise AuthorityError(f"{label}:invalid_digest")
    return value.lower()


def physical_host_digest(machine_id_sha256: str, nic_identity_sha256: str) -> str:
    """Derive stable physical identity from hashed machine-id and NIC facts."""
    value = {"machine_id_sha256": _hex(machine_id_sha256, "machine_id_sha256"),
             "nic_identity_sha256": _hex(nic_identity_sha256, "nic_identity_sha256")}
    # The pre-existing S8 intake's canonical digest has no trailing newline.
    return digest(json.dumps(value, sort_keys=True, separators=(",", ":"),
                             ensure_ascii=True, allow_nan=False).encode("ascii"))


def _private_parent(path: Path, label: str) -> None:
    if not path.is_absolute():
        raise AuthorityError(f"{label}:relative_path")
    for ancestor in (path, *path.parents):
        try:
            if ancestor.is_symlink():
                raise AuthorityError(f"{label}:symlink_ancestor")
        except OSError as exc:
            raise AuthorityError(f"{label}:unavailable") from exc


def _private_file(path: Path, label: str) -> bytes:
    _private_parent(path, label)
    try:
        info = path.lstat()
    except OSError as exc:
        raise AuthorityError(f"{label}:unavailable") from exc
    if path.is_symlink() or not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
        raise AuthorityError(f"{label}:not_private_regular_file")
    try:
        with path.open("rb") as stream:
            before = os.fstat(stream.fileno())
            raw = stream.read()
            after = os.fstat(stream.fileno())
    except OSError as exc:
        raise AuthorityError(f"{label}:unavailable") from exc
    if (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) != \
            (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns):
        raise AuthorityError(f"{label}:changed_during_read")
    return raw


def final_head_role_hashes(root: Path) -> dict[str, str]:
    """Hash all invoked binaries from the selected final-head tree."""
    if not root.is_absolute() or not root.is_dir():
        raise AuthorityError("final_head_root:directory_required")
    result = {}
    for relative in ROLE_PATHS:
        path = root / relative
        executable_required = relative != "client/icecc-create-env"
        if (path.is_symlink() or not path.is_file() or
                (executable_required and not os.access(path, os.X_OK))):
            raise AuthorityError(f"final_head_role:{relative}:missing_or_not_executable")
        result[relative] = digest(_private_file(path, f"final_head_role:{relative}"))
    return result


def final_head_source_identity(root: Path) -> dict[str, str]:
    try:
        commit = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"], text=True,
                                capture_output=True, check=True, timeout=10).stdout.strip()
        tree = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD^{tree}"], text=True,
                              capture_output=True, check=True, timeout=10).stdout.strip()
    except (OSError, subprocess.SubprocessError) as exc:
        raise AuthorityError("final_head_source_identity:unavailable") from exc
    if not re.fullmatch(r"[0-9a-f]{40}", commit) or not re.fullmatch(r"[0-9a-f]{40}", tree):
        raise AuthorityError("final_head_source_identity:invalid")
    return {"commit": commit, "tree": tree}


def _capture_time(value: object, label: str, now: dt.datetime, max_age: float) -> str:
    if not isinstance(value, str) or ISO_UTC.fullmatch(value) is None:
        raise AuthorityError(f"{label}:invalid")
    try:
        captured = dt.datetime.strptime(value, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=dt.timezone.utc)
    except ValueError as exc:
        raise AuthorityError(f"{label}:invalid") from exc
    age = (now - captured).total_seconds()
    if age < -30 or age > max_age:
        raise AuthorityError(f"{label}:stale")
    return value


def _sample_digest(sample: object) -> str:
    if not isinstance(sample, dict) or set(sample) != {"before", "after", "duration_seconds", "idle_percent"}:
        raise AuthorityError("cpu_sample:fields_invalid")
    if any(not isinstance(sample[key], str) or not sample[key].strip() for key in ("before", "after")):
        raise AuthorityError("cpu_sample:raw_invalid")
    if (type(sample["duration_seconds"]) not in (int, float) or not .5 <= float(sample["duration_seconds"]) <= 5 or
            type(sample["idle_percent"]) not in (int, float) or not 0 <= float(sample["idle_percent"]) <= 100):
        raise AuthorityError("cpu_sample:values_invalid")
    return digest(canonical(sample))


def _expected_image(host: str) -> str:
    return IMAGE_CONFIG if host == "research7" else IMAGE_INDEX


def _validate_capture(host: str, value: Mapping[str, object], expected: Mapping[str, str],
                      *, now: dt.datetime, max_age: float) -> dict[str, object]:
    required = {"hostname", "machine_id_sha256", "boot_id_sha256", "nic_identity_sha256",
                "cpu_vendor_sha256", "cpu_model_sha256", "cpu_count", "image", "binaries",
                "load_1m", "captured_at", "baseline_digest", "cpu_sample", "cpu_sample_digest"}
    if set(value) != required:
        raise AuthorityError(f"capture:{host}:fields_invalid")
    for key in ("machine_id_sha256", "boot_id_sha256", "nic_identity_sha256",
                "cpu_vendor_sha256", "cpu_model_sha256", "baseline_digest"):
        _hex(value[key], f"capture:{host}.{key}")
    if not isinstance(value["hostname"], str) or not value["hostname"].strip():
        raise AuthorityError(f"capture:{host}:hostname_invalid")
    if type(value["cpu_count"]) is not int or value["cpu_count"] != EXPECTED_CPU_COUNTS[host]:
        raise AuthorityError(f"capture:{host}:cpu_count_mismatch")
    if type(value["load_1m"]) not in (int, float) or float(value["load_1m"]) < 0:
        raise AuthorityError(f"capture:{host}:load_invalid")
    captured = _capture_time(value["captured_at"], f"capture:{host}.captured_at", now, max_age)
    sample = value["cpu_sample"]
    sample_sha = _sample_digest(sample)
    if sample_sha != _hex(value["cpu_sample_digest"], f"capture:{host}.cpu_sample_digest"):
        raise AuthorityError(f"capture:{host}:cpu_sample_digest_mismatch")
    physical = physical_host_digest(str(value["machine_id_sha256"]), str(value["nic_identity_sha256"]))
    image = value["image"]
    if (not isinstance(image, dict) or set(image) != {"reference", "image_id", "architecture", "os", "created"} or
            image.get("reference") != PINNED_IMAGE or image.get("image_id") != _expected_image(host) or
            image.get("architecture") != "amd64" or image.get("os") != "linux" or
            not isinstance(image.get("created"), str) or not image["created"]):
        raise AuthorityError(f"capture:{host}:image_mismatch")
    binaries = value["binaries"]
    if not isinstance(binaries, dict) or set(binaries) != set(ROLE_PATHS):
        raise AuthorityError(f"capture:{host}:role_binaries_incomplete")
    normalized_binaries = {}
    for relative in ROLE_PATHS:
        observed = _hex(binaries[relative], f"capture:{host}.binaries.{relative}")
        if observed != expected[relative]:
            raise AuthorityError(f"capture:{host}:role_binary_mismatch:{relative}")
        normalized_binaries[relative] = observed
    load, idle = float(value["load_1m"]), float(sample["idle_percent"])
    status = "PASS" if load <= IDLE_LOAD_THRESHOLD and idle >= MIN_IDLE_PERCENT else "HOLD"
    return {"hostname": value["hostname"], "machine_id_sha256": str(value["machine_id_sha256"]).lower(),
            "boot_id_digest": str(value["boot_id_sha256"]).lower(), "nic_identity_sha256": str(value["nic_identity_sha256"]).lower(),
            "cpu_vendor_sha256": str(value["cpu_vendor_sha256"]).lower(), "cpu_model_sha256": str(value["cpu_model_sha256"]).lower(),
            "cpu_count": EXPECTED_CPU_COUNTS[host], "physical_host_digest": physical,
            "image": {**image, "image_id": image["image_id"].lower()}, "binaries": normalized_binaries,
            "idle": {"status": status, "load_1m": load, "captured_at": captured, "baseline_digest": str(value["baseline_digest"]).lower()},
            "cpu_sample": sample, "cpu_sample_digest": sample_sha}


def relationship_mappings(*, include_research6: bool = False, c1f1_host: str = "q2") -> dict[str, list[str]]:
    if c1f1_host not in F_HOSTS or (c1f1_host == "research6" and not include_research6):
        raise AuthorityError("placement:c1f1_host_requires_explicit_research6")
    return {"C1F1/100000": [c1f1_host],
            "C1F20/40": list(RESEARCH6_PARALLEL if include_research6 else DEFAULT_PARALLEL)}


def _validate_mappings(mappings: Mapping[str, Sequence[str]], hosts: Mapping[str, object], *, include_research6: bool) -> dict[str, list[str]]:
    if set(mappings) != {"C1F1/100000", "C1F20/40"}:
        raise AuthorityError("placement:missing_or_duplicate_mapping")
    result = {}
    for topology, count in (("C1F1/100000", 1), ("C1F20/40", 20)):
        values = list(mappings[topology])
        if len(values) != count or any(host not in F_HOSTS for host in values):
            raise AuthorityError(f"placement:{topology}:mapping_invalid")
        if "research6" in values and not include_research6:
            raise AuthorityError("placement:research6_requires_explicit_request")
        for host in set(values):
            if hosts[host]["idle"]["status"] != "PASS":
                raise AuthorityError(f"placement:host_not_idle:{host}")
        result[topology] = values
    return result


def _write_once(path: Path, raw: bytes, label: str) -> None:
    _private_parent(path, label)
    if path.exists() or path.is_symlink():
        raise AuthorityError(f"{label}:already_exists")
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    try:
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, "wb") as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
    except FileExistsError as exc:
        raise AuthorityError(f"{label}:already_exists") from exc
    except OSError as exc:
        raise AuthorityError(f"{label}:create_failed") from exc


def build_authority(*, root: Path, captures: Mapping[str, Mapping[str, object]],
                    descriptor_dir: Path, output: Path, include_research6: bool = False,
                    mappings: Mapping[str, Sequence[str]] | None = None,
                    now: dt.datetime | None = None,
                    max_capture_age: float = MAX_CAPTURE_AGE_SECONDS) -> dict[str, object]:
    """Validate fresh captures and create one private authority plus descriptors."""
    if set(captures) != set(HOSTS):
        raise AuthorityError("capture:host_set_invalid")
    expected = final_head_role_hashes(root)
    now = now or dt.datetime.now(dt.timezone.utc)
    normalized = {host: _validate_capture(host, captures[host], expected,
                                          now=now, max_age=max_capture_age)
                  for host in HOSTS}
    physical = [normalized[host]["physical_host_digest"] for host in HOSTS]
    if len(set(physical)) != len(physical):
        raise AuthorityError("physical_hosts_not_unique")
    chosen = (relationship_mappings(include_research6=include_research6)
              if mappings is None else mappings)
    chosen = _validate_mappings(chosen, normalized, include_research6=include_research6)
    if normalized["q3"]["idle"]["status"] != "PASS":
        raise AuthorityError("placement:host_not_idle:q3")
    q3_digest = normalized["q3"]["physical_host_digest"]
    if any(normalized[host]["physical_host_digest"] == q3_digest
           for values in chosen.values() for host in values):
        raise AuthorityError("placement:q3_forbidden")
    _private_parent(descriptor_dir, "descriptor_dir")
    descriptor_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
    try:
        descriptor_dir.chmod(0o700)
    except OSError as exc:
        raise AuthorityError("descriptor_dir:not_private") from exc
    host_values = {}
    for host in HOSTS:
        facts = {"machine_id_sha256": normalized[host]["machine_id_sha256"],
                 "boot_id_sha256": normalized[host]["boot_id_digest"],
                 "nic_identity_sha256": normalized[host]["nic_identity_sha256"],
                 "cpu_vendor_sha256": normalized[host]["cpu_vendor_sha256"],
                 "cpu_model_sha256": normalized[host]["cpu_model_sha256"],
                 "cpu_count": normalized[host]["cpu_count"],
                 "physical_host_digest": normalized[host]["physical_host_digest"]}
        descriptor = {"schema": HOST_DESCRIPTOR_SCHEMA, "facts": facts}
        descriptor_path = descriptor_dir / f"{host}-descriptor.json"
        raw_descriptor = canonical(descriptor)
        _write_once(descriptor_path, raw_descriptor, "descriptor")
        host_values[host] = {
            "target": s4.HOSTS[host]["target"], "lan": s4.HOSTS[host]["lan"],
            "hostname": normalized[host]["hostname"],
            "descriptor": {"path": str(descriptor_path.resolve()),
                           "sha256": digest(raw_descriptor), "bytes": len(raw_descriptor)},
            "physical_host_digest": normalized[host]["physical_host_digest"],
            "boot_id_digest": normalized[host]["boot_id_digest"],
            "image": normalized[host]["image"], "binaries": normalized[host]["binaries"],
            "cpu_count": normalized[host]["cpu_count"], "idle": normalized[host]["idle"],
            "cpu_sample": normalized[host]["cpu_sample"],
            "cpu_sample_digest": normalized[host]["cpu_sample_digest"]}
    authority = {"schema": SCHEMA, "hosts": host_values,
                 "placements": {topology: {"relationship_hosts": values}
                                for topology, values in chosen.items()}}
    _write_once(output, canonical(authority), "authority")
    return authority


# Read-only remote capture: /proc, /sys, role files and Docker image inspect.
# It intentionally has no Docker/Icecream lifecycle operation or remote write.
REMOTE_CAPTURE_SCRIPT = r'''set -eu
root=$1; image=$2
machine=$(sha256sum /etc/machine-id | awk '{print $1}')
boot=$(sha256sum /proc/sys/kernel/random/boot_id | awk '{print $1}')
nic_rows=$(for p in /sys/class/net/*; do n=${p##*/}; test "$n" = lo && continue; real=$(readlink -f "$p" 2>/dev/null || true); mac=$(cat "$p/address" 2>/dev/null || true); case "$real" in */virtual/*) continue;; esac; test -n "$mac" && test "$mac" != 00:00:00:00:00:00 && printf '%s:%s:%s\n' "$n" "$mac" "$real"; done | sort)
test -n "$nic_rows"
nic=$(printf '%s\n' "$nic_rows" | sha256sum | awk '{print $1}')
vendor_raw=$(awk -F: 'tolower($1)=="vendor_id" {gsub(/^ +| +$/, "", $2); print $2; exit}' /proc/cpuinfo); test -n "$vendor_raw"
model_raw=$(awk -F: 'tolower($1)=="model name" {gsub(/^ +| +$/, "", $2); print $2; exit}' /proc/cpuinfo); test -n "$model_raw"
vendor=$(printf '%s' "$vendor_raw" | sha256sum | awk '{print $1}')
model=$(printf '%s' "$model_raw" | sha256sum | awk '{print $1}')
before=$(awk '/^cpu / {print; exit}' /proc/stat); started=$(date +%s%N); sleep 1; after=$(awk '/^cpu / {print; exit}' /proc/stat); ended=$(date +%s%N)
idle=$(awk -v b="$before" -v a="$after" 'BEGIN {split(b,x," "); split(a,y," "); total=0; for(i=2;i<=NF;i++) total+=y[i]-x[i]; print (100*(y[5]-x[5])/total)}')
duration=$(awk -v a="$started" -v b="$ended" 'BEGIN {print (b-a)/1000000000}')
baseline=$(ps -eo pid=,ppid=,comm=,stat=,etimes= --sort=pid | sha256sum | awk '{print $1}')
image_json=$(docker image inspect --format '{{.Id}}\t{{.Os}}\t{{.Architecture}}\t{{.Created}}' "$image")
image_id=$(printf '%s' "$image_json" | cut -f1); image_os=$(printf '%s' "$image_json" | cut -f2); image_arch=$(printf '%s' "$image_json" | cut -f3); image_created=$(printf '%s' "$image_json" | cut -f4)
printf 'S8_SCHEMA=%s\nS8_HOSTNAME=%s\nS8_MACHINE=%s\nS8_BOOT=%s\nS8_NIC=%s\nS8_VENDOR=%s\nS8_MODEL=%s\nS8_CPU=%s\nS8_IMAGE_ID=%s\nS8_IMAGE_OS=%s\nS8_IMAGE_ARCH=%s\nS8_IMAGE_CREATED=%s\nS8_LOAD=%s\nS8_CAPTURED_AT=%s\nS8_BASELINE=%s\nS8_SAMPLE_BEFORE=%s\nS8_SAMPLE_AFTER=%s\nS8_SAMPLE_DURATION=%s\nS8_SAMPLE_IDLE=%s\n' 'icecream-s8-external-host-capture-v1' "$(hostname)" "$machine" "$boot" "$nic" "$vendor" "$model" "$(nproc)" "$image_id" "$image_os" "$image_arch" "$image_created" "$(cut -d' ' -f1 /proc/loadavg)" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$baseline" "$before" "$after" "$duration" "$idle"
for rel in scheduler/icecc-scheduler daemon/iceccd client/icecc client/icecc-create-env cache/icecc-cache-service; do printf 'S8_BIN_%s=%s\n' "$rel" "$(sha256sum "$root/$rel" | awk '{print $1}')"; done
'''


def _parse_remote_capture(raw: str, host: str) -> dict[str, object]:
    rows = {}
    for line in raw.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        rows[key] = value
    if rows.get("S8_SCHEMA") != CAPTURE_SCHEMA:
        raise AuthorityError(f"capture:{host}:schema_invalid")
    try:
        sample = {"before": rows["S8_SAMPLE_BEFORE"], "after": rows["S8_SAMPLE_AFTER"],
                  "duration_seconds": float(rows["S8_SAMPLE_DURATION"]),
                  "idle_percent": float(rows["S8_SAMPLE_IDLE"])}
        binaries = {rel: rows["S8_BIN_" + rel] for rel in ROLE_PATHS}
        image = {"reference": PINNED_IMAGE, "image_id": rows["S8_IMAGE_ID"],
                 "os": rows["S8_IMAGE_OS"], "architecture": rows["S8_IMAGE_ARCH"],
                 "created": rows["S8_IMAGE_CREATED"]}
        return {"hostname": rows["S8_HOSTNAME"], "machine_id_sha256": rows["S8_MACHINE"],
                "boot_id_sha256": rows["S8_BOOT"], "nic_identity_sha256": rows["S8_NIC"],
                "cpu_vendor_sha256": rows["S8_VENDOR"], "cpu_model_sha256": rows["S8_MODEL"],
                "cpu_count": int(rows["S8_CPU"]), "image": image, "binaries": binaries,
                "load_1m": float(rows["S8_LOAD"]), "captured_at": rows["S8_CAPTURED_AT"],
                "baseline_digest": rows["S8_BASELINE"], "cpu_sample": sample,
                "cpu_sample_digest": digest(canonical(sample))}
    except (KeyError, TypeError, ValueError) as exc:
        raise AuthorityError(f"capture:{host}:fields_invalid") from exc


def capture_host(host: str, remote_root: str, *, timeout: float = 120.0) -> dict[str, object]:
    """Capture one host over SSH; this command has no remote state change."""
    if host not in HOSTS or not isinstance(remote_root, str) or not remote_root.startswith("/"):
        raise AuthorityError(f"capture:{host}:remote_root_invalid")
    if not 0 < timeout <= 600:
        raise AuthorityError("capture:timeout_invalid")
    result = subprocess.run([*s4.ssh_argv(host), "bash", "-s", "--", remote_root, PINNED_IMAGE],
                            input=REMOTE_CAPTURE_SCRIPT, text=True, capture_output=True,
                            timeout=timeout, check=False)
    if result.returncode:
        raise AuthorityError(f"capture:{host}:remote_failed:{result.returncode}")
    return _parse_remote_capture(result.stdout, host)


def dry_run(*, root: Path, include_research6: bool = False) -> dict[str, object]:
    """Describe work without SSH, Docker, Icecream, or output writes."""
    return {"schema": "icecream-s8-external-farm-authority-dry-run-v1",
            "status": "DRY_RUN", "execute_required": True,
            "source": final_head_source_identity(root),
            "roles": final_head_role_hashes(root),
            "hosts": {host: {"target": s4.HOSTS[host]["target"],
                              "lan": s4.HOSTS[host]["lan"]} for host in HOSTS},
            "placements": relationship_mappings(include_research6=include_research6)}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--output", type=Path)
    parser.add_argument("--descriptor-dir", type=Path)
    parser.add_argument("--remote-root", action="append", metavar="HOST=PATH")
    parser.add_argument("--include-research6", action="store_true")
    parser.add_argument("--execute", action="store_true")
    parser.add_argument("--timeout", type=float, default=120.0)
    args = parser.parse_args(argv)
    try:
        root = args.root.absolute()
        if not args.execute:
            print(canonical(dry_run(root=root,
                                    include_research6=args.include_research6)).decode(), end="")
            return 0
        if args.output is None or args.descriptor_dir is None:
            parser.error("--execute requires --output and --descriptor-dir")
        if not args.remote_root or len(args.remote_root) != len(HOSTS):
            parser.error("--execute requires exactly four --remote-root HOST=PATH values")
        remote: dict[str, str] = {}
        for item in args.remote_root:
            if "=" not in item:
                parser.error("--remote-root must be HOST=PATH")
            host, path = item.split("=", 1)
            if host not in HOSTS or host in remote:
                parser.error("--remote-root host set must be exact and unique")
            remote[host] = path
        if set(remote) != set(HOSTS):
            parser.error("--remote-root host set must be q3,q2,research6,research7")
        final_head_role_hashes(root)
        captures = {host: capture_host(host, remote[host], timeout=args.timeout)
                    for host in HOSTS}
        authority = build_authority(root=root, captures=captures,
                                    descriptor_dir=args.descriptor_dir.absolute(),
                                    output=args.output.absolute(),
                                    include_research6=args.include_research6)
        print(canonical(authority).decode(), end="")
        return 0
    except AuthorityError as exc:
        parser.error(str(exc))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
