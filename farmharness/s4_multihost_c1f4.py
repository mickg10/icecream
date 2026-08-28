#!/usr/bin/env python3
"""Run an exact Protocol-50 C1F2 or C1F4 physical contention cell.

The scheduler and client run on q3.  One worker container runs on each of q3,
research6, research7 and q2.  Every host receives the same hash-pinned role
bundle and the same uniquely tagged Docker image.  One preferred-host compile
per selected worker must return a byte-identical object, and every worker must
show the ZSTD_TU cache path without a legacy fallback.

The runner deliberately refuses to overlap an S5 timing process.  It retains
all logs and one JSONL result under ``experiments/icecream`` before removing
only its uniquely named containers and private ``/tmp`` directories.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


SCHEMA = "icecream-s4-physical-contention-v2"
BASE_SOURCE_SHA = "04006b9d94161a047154121f47785aec747ffd87"
EXPECTED_IMAGE_ID = "sha256:bdb55d4287a473e3ebfbaa7715a50ee670659777278b8d84c350724e6fa8de58"
# Docker reports the multi-platform index on q3/research6/q2, while the
# single-platform image loaded on research7 reports its exact config ID.
# Both IDs are bound to the same exported pinned image; any other ID is
# rejected.
EXPECTED_IMAGE_CONFIG_ID = "sha256:fe001a6138f017608b8846b43bf268a76a9d7a5b66c3364ba3f881da2ff0c54b"
EXPECTED_IMAGE_IDS = frozenset((EXPECTED_IMAGE_ID, EXPECTED_IMAGE_CONFIG_ID))
SOURCE_IMAGE = "icecream/farm-node:ubuntu22-gcc11-boost174"
PINNED_IMAGE = "icecream/p50-farm-node:ubuntu22-gcc11-boost174-bdb55d"

ROLE_FILES = {
    "scheduler/icecc-scheduler": "f1bfc8e6b4fb44815b00efa281c36b0ee35a0faeffb394a320ae908a7fb60750",
    "daemon/iceccd": "b803bf877ff4ff5023a8f96819bd86c530ab54e32405e1f6d12fee9c68126b19",
    "client/icecc": "781804278af9aff93bff9d4f2f87a6d3152bc1cbe1804e41b926b1a0f22ebb9d",
    "client/icecc-create-env": "ee7d30b240c38bccf66d4afcdd45993f115a01d4a2fb4e9143d38596609d2ba4",
    "cache/icecc-cache-service": "5af26a01bc98fd6070b8c1e075b68f5969f1d15fb08aa1a231dd9319d238a062",
}

HOSTS: dict[str, dict[str, Any]] = {
    "q3": {
        "target": "mickg10@tt-quietbox3",
        "options": ["-o", "HostName=10.0.27.101", "-o", "HostKeyAlias=tt-quietbox3"],
        "lan": "10.0.27.101",
    },
    "research6": {
        "target": "mickg@research6",
        "options": [],
        "lan": "10.0.27.56",
    },
    "research7": {
        "target": "mickg@research7",
        "options": [],
        "lan": "10.0.27.58",
    },
    "q2": {
        "target": "mickg10@tt-quietbox2",
        "options": ["-o", "HostName=100.91.242.69", "-o", "HostKeyAlias=tt-quietbox2"],
        "lan": "10.0.27.212",
    },
}

WORKER_NAMES = {
    "q3": "s4-p50-q3",
    "research6": "s4-p50-research6",
    "research7": "s4-p50-research7",
    "q2": "s4-p50-q2",
}

SSH_COMMON = ["-o", "BatchMode=yes", "-o", "ConnectTimeout=10"]


class HoldError(RuntimeError):
    """An execution prerequisite is unavailable; no product failure implied."""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def ssh_argv(host: str) -> list[str]:
    config = HOSTS[host]
    return ["ssh", *SSH_COMMON, *config["options"], config["target"]]


def run_script(
    host: str,
    script: str,
    args: list[str] | tuple[str, ...] = (),
    *,
    timeout: float = 120,
    check: bool = False,
) -> subprocess.CompletedProcess[str]:
    command = [*ssh_argv(host), "bash", "-s", "--", *(shlex.quote(str(arg)) for arg in args)]
    result = subprocess.run(
        command, input=script, text=True, capture_output=True, timeout=timeout, check=False
    )
    if check and result.returncode != 0:
        raise HoldError(
            f"{host} command rc={result.returncode}: "
            f"{(result.stderr or result.stdout)[-400:].strip()}"
        )
    return result


def validate_local_roles(root: Path) -> dict[str, str]:
    observed: dict[str, str] = {}
    for relative, expected in ROLE_FILES.items():
        path = root / relative
        if not path.is_file() or path.is_symlink():
            raise HoldError(f"exact role artifact missing: {path}")
        if relative != "client/icecc-create-env" and not os.access(path, os.X_OK):
            raise HoldError(f"exact role artifact is not executable: {path}")
        observed[relative] = sha256(path)
        if observed[relative] != expected:
            raise HoldError(
                f"exact role artifact hash mismatch: {relative}: {observed[relative]}"
            )
    return observed


def active_s5_processes(process_text: str | None = None) -> list[str]:
    if process_text is None:
        process_text = subprocess.run(
            ["ps", "-eo", "pid=,args="], text=True, capture_output=True, check=True
        ).stdout
    return [
        line.strip()
        for line in process_text.splitlines()
        if "farmharness/s5_paired_build.py" in line and "s4_multihost_c1f4.py" not in line
    ]


def require_no_s5() -> None:
    active = active_s5_processes()
    if active:
        raise HoldError("S5 timing is active: " + " | ".join(active[:3]))


IDENTITY_SCRIPT = r"""
set -eu
hostname
id -u
id -g
command -v docker >/dev/null
docker version --format '{{.Server.Version}}'
"""


def host_preflight(hosts: list[str], timeout: float) -> dict[str, dict[str, Any]]:
    rows: dict[str, dict[str, Any]] = {}
    for host in hosts:
        result = run_script(host, IDENTITY_SCRIPT, timeout=timeout, check=True)
        lines = result.stdout.splitlines()
        if len(lines) < 4 or not lines[1].isdigit() or not lines[2].isdigit():
            raise HoldError(f"{host} returned incomplete identity preflight")
        rows[host] = {
            "hostname": lines[0],
            "uid": int(lines[1]),
            "gid": int(lines[2]),
            "docker_version": lines[3],
            "lan": HOSTS[host]["lan"],
        }
    return rows


IMAGE_INSPECT_SCRIPT = r"""
set -eu
image=$1
docker image inspect --format '{{.Id}}' "$image" 2>/dev/null || true
"""


def image_id(host: str, image: str, timeout: float) -> str:
    result = run_script(host, IMAGE_INSPECT_SCRIPT, [image], timeout=timeout, check=True)
    return result.stdout.strip().splitlines()[-1] if result.stdout.strip() else ""


def valid_image_id(value: str) -> bool:
    """Return whether Docker's exact index or platform-config ID is bound."""
    return value in EXPECTED_IMAGE_IDS


def valid_image_closure(image_ids: dict[str, str]) -> bool:
    """Require q3's source index and only the two bound Docker identities."""
    return image_ids.get("q3") == EXPECTED_IMAGE_ID and all(
        valid_image_id(value) for value in image_ids.values()
    )


TAG_IMAGE_SCRIPT = r"""
set -eu
expected=$1; tag=$2
source_id=$(docker image inspect --format '{{.Id}}' "$expected" 2>/dev/null || true)
[ "$source_id" = "$expected" ] || { echo "source-image-mismatch:$source_id"; exit 77; }
existing=$(docker image inspect --format '{{.Id}}' "$tag" 2>/dev/null || true)
[ -z "$existing" ] || [ "$existing" = "$expected" ] || {
  echo "pinned-tag-already-differs:$existing"; exit 77;
}
docker tag "$expected" "$tag"
docker image inspect --format '{{.Id}}' "$tag"
"""


def pin_image_on_existing_hosts(hosts: list[str], timeout: float) -> None:
    for host in hosts:
        if host == "research7":
            continue
        result = run_script(
            host, TAG_IMAGE_SCRIPT, [EXPECTED_IMAGE_ID, PINNED_IMAGE], timeout=timeout
        )
        if result.returncode != 0 or result.stdout.strip().splitlines()[-1:] != [EXPECTED_IMAGE_ID]:
            raise HoldError(
                f"{host} could not pin exact image: "
                f"{(result.stderr or result.stdout)[-300:].strip()}"
            )


def copy_pinned_image_to_research7(timeout: float) -> None:
    existing = image_id("research7", PINNED_IMAGE, timeout)
    if existing == EXPECTED_IMAGE_CONFIG_ID:
        return
    if existing:
        raise HoldError(f"research7 pinned image tag already differs: {existing}")
    source = subprocess.Popen(
        [*ssh_argv("q3"), "docker", "save", PINNED_IMAGE],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert source.stdout is not None
    destination = subprocess.Popen(
        [*ssh_argv("research7"), "docker", "load"],
        stdin=source.stdout,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    source.stdout.close()
    out, err = destination.communicate(timeout=timeout)
    source_err = source.stderr.read() if source.stderr else b""
    source_rc = source.wait(timeout=30)
    if source_rc != 0 or destination.returncode != 0:
        raise HoldError(
            "exact image copy to research7 failed: "
            + (source_err + err + out)[-500:].decode(errors="replace")
        )
    if image_id("research7", PINNED_IMAGE, timeout) != EXPECTED_IMAGE_CONFIG_ID:
        raise HoldError("research7 loaded image ID differs from the pinned source")


def stage_roles(host: str, root: Path, timeout: float) -> str:
    tar_command = ["tar", "-C", str(root), "-cf", "-", *ROLE_FILES]
    tar_process = subprocess.Popen(tar_command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    remote = (
        "set -eu; root=$(mktemp -d /tmp/s4-p50-fourhost.XXXXXX); "
        "mkdir -p \"$root/scheduler\" \"$root/daemon\" \"$root/client\" \"$root/cache\"; "
        "tar -xf - -C \"$root\"; printf 'S4_STAGE_ROOT=%s\\n' \"$root\""
    )
    assert tar_process.stdout is not None
    receiver = subprocess.Popen(
        [*ssh_argv(host), "bash", "-c", shlex.quote(remote)],
        stdin=tar_process.stdout,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    tar_process.stdout.close()
    output, error = receiver.communicate(timeout=timeout)
    tar_error = tar_process.stderr.read() if tar_process.stderr else b""
    tar_rc = tar_process.wait(timeout=30)
    if tar_rc != 0 or receiver.returncode != 0:
        raise HoldError(
            f"{host} role staging failed: "
            + (tar_error + error + output)[-500:].decode(errors="replace")
        )
    match = re.search(rb"^S4_STAGE_ROOT=(/tmp/s4-p50-fourhost\.[A-Za-z0-9]+)$", output, re.M)
    if not match:
        raise HoldError(f"{host} role staging returned no private root")
    return match.group(1).decode()


VERIFY_STAGE_SCRIPT = r"""
set -eu
root=$1; shift
while [ "$#" -gt 0 ]; do
  rel=$1; expected=$2; shift 2
  path="$root/$rel"
  [ -f "$path" ] && [ ! -L "$path" ] || { echo "missing:$rel"; exit 77; }
  got=$(sha256sum "$path" | awk '{print $1}')
  [ "$got" = "$expected" ] || { echo "hash:$rel:$got"; exit 77; }
  printf '%s %s\n' "$rel" "$got"
done
"""


def verify_stage(host: str, root: str, timeout: float) -> str:
    args = [root]
    for relative, expected in ROLE_FILES.items():
        args.extend((relative, expected))
    result = run_script(host, VERIFY_STAGE_SCRIPT, args, timeout=timeout)
    if result.returncode != 0 or len(result.stdout.splitlines()) != len(ROLE_FILES):
        raise HoldError(
            f"{host} staged role verification failed: "
            f"{(result.stderr or result.stdout)[-400:].strip()}"
        )
    return result.stdout


START_SCHEDULER_SCRIPT = r"""
set -eu
root=$1; image=$2; container=$3; port=$4; network=$5; uid=$6; gid=$7
work=$(mktemp -d /tmp/s4-p50-fourhost-scheduler.XXXXXX)
chmod 1777 "$work"
cat >"$work/wrapper.sh" <<'WRAPPER'
#!/bin/sh
set -eu
printf 'icecc:x:%s:%s:icecc:/nonexistent:/usr/sbin/nologin\n' "$S4_UID" "$S4_GID" >>/etc/passwd
printf 'icecc:x:%s:\n' "$S4_GID" >>/etc/group
exec /role/scheduler/icecc-scheduler "$@"
WRAPPER
chmod 755 "$work/wrapper.sh"
docker run -d --name "$container" --network host --user 0 \
  -v "$root:/role:ro" -v "$work:/probe" -e S4_UID="$uid" -e S4_GID="$gid" \
  --entrypoint /bin/sh "$image" /probe/wrapper.sh -p "$port" -n "$network" \
  --assignment-fence-mode strict-nonce -l /probe/scheduler.log -vvv \
  >"$work/container.id"
sleep 1
[ "$(docker inspect --format '{{.State.Running}}' "$container")" = true ] || exit 77
printf 'S4_REMOTE_WORK=%s\n' "$work"
"""


START_WORKER_SCRIPT = r"""
set -eu
root=$1; image=$2; container=$3; scheduler=$4; sport=$5; network=$6
name=$7; port=$8; uid=$9; gid=${10}
work=$(mktemp -d /tmp/s4-p50-fourhost-worker.XXXXXX)
mkdir -p "$work/envs" "$work/cache-runtime"
chmod 1777 "$work/envs"; chmod 700 "$work/cache-runtime"
cat >"$work/wrapper.sh" <<'WRAPPER'
#!/bin/sh
set -eu
printf 'icecc:x:%s:%s:icecc:/nonexistent:/usr/sbin/nologin\n' "$S4_UID" "$S4_GID" >>/etc/passwd
printf 'icecc:x:%s:\n' "$S4_GID" >>/etc/group
exec /role/daemon/iceccd "$@"
WRAPPER
chmod 755 "$work/wrapper.sh"
docker run -d --name "$container" --network host --user 0 --cap-add=SYS_CHROOT \
  -v "$root:/role:ro" -v "$work:/probe" -e S4_UID="$uid" -e S4_GID="$gid" \
  -e ICECC_TEST_SOCKET=/probe/f.sock --entrypoint /bin/sh "$image" /probe/wrapper.sh \
  -p "$port" -m 2 -s "$scheduler:$sport" -n "$network" -N "$name" \
  -b /probe/envs -l /probe/fdaemon.log -vvv \
  --cache-service /role/cache/icecc-cache-service \
  --cache-runtime-dir /probe/cache-runtime >"$work/container.id"
sleep 2
[ "$(docker inspect --format '{{.State.Running}}' "$container")" = true ] || exit 77
printf 'S4_REMOTE_WORK=%s\n' "$work"
"""


CLIENT_SCRIPT = r"""
set -u
root=$1; scheduler=$2; sport=$3; network=$4; scheduler_log=$5; cport=$6; load=$7
shift 7
names=("$@")
worker_count=${#names[@]}
[ "$worker_count" -eq 2 ] || [ "$worker_count" -eq 4 ] || {
  echo 'S4_STATUS=FAIL reason=unsupported-worker-count'; exit 1;
}
work=$(mktemp -d /tmp/s4-p50-fourhost-client.XXXXXX)
printf 'S4_CLIENT_WORK=%s\n' "$work"
C_PID=
cleanup() {
  rc=$?; trap - EXIT HUP INT TERM
  [ -n "$C_PID" ] && kill "$C_PID" 2>/dev/null || :
  [ -n "$C_PID" ] && wait "$C_PID" 2>/dev/null || :
  printf 'S4_CLIENT_CLEANUP=bounded\n'
  exit "$rc"
}
trap cleanup EXIT HUP INT TERM
mkdir -p "$work/env" "$work/envs-c" "$work/out"; chmod 1777 "$work/envs-c"
for i in $(seq 1 "$worker_count"); do
  if [ "$load" = same ]; then
    printf '#include <cstdint>\nextern "C" int s4_physical_%s() { return %s; }\n' \
      "$i" "$((90+i))" >"$work/main-$i.cpp"
  else
    case "$i" in
      1) values_count=32;; 2) values_count=512;;
      3) values_count=4096;; 4) values_count=16384;;
    esac
    {
      printf '#include <cstdint>\nstatic const std::uint32_t values_%s[] = {' "$i"
      seq 1 "$values_count" | awk '{printf "%s,", ($1 * 2654435761) % 4294967291}'
      printf '};\nextern "C" std::uint32_t s4_physical_%s() { return values_%s[%s]; }\n' \
        "$i" "$i" "$((values_count-1))"
    } >"$work/main-$i.cpp"
  fi
  g++ -std=c++17 -O2 -c "$work/main-$i.cpp" -o "$work/out/local-$i.o" \
    2>"$work/local-$i.log" || { echo 'S4_STATUS=FAIL reason=local-reference'; exit 1; }
done
(cd "$work/env" && timeout 120 bash "$root/client/icecc-create-env" "$(command -v g++)" \
  >"$work/create-env.log" 2>&1) || { echo 'S4_STATUS=FAIL reason=create-env'; exit 1; }
envtar=$(find "$work/env" -maxdepth 1 -type f -name '*.tar.gz' -print -quit)
[ -n "$envtar" ] || { echo 'S4_STATUS=FAIL reason=no-compiler-environment'; exit 1; }
ICECC_TEST_SOCKET="$work/c.sock" "$root/daemon/iceccd" --no-remote -m 0 -p "$cport" \
  -s "$scheduler:$sport" -n "$network" -N s4-p50-client -b "$work/envs-c" \
  -l "$work/cdaemon.log" -vvv >"$work/cdaemon.stdout.log" 2>&1 & C_PID=$!
sleep 5
printf '#include <cstdint>\nextern "C" int s4_physical_warmup() { return 177; }\n' >"$work/warmup.cpp"
for name in "${names[@]}"; do
  env ICECC_TEST_SOCKET="$work/c.sock" ICECC_TEST_REMOTEBUILD=1 ICECC_VERSION="$envtar" \
    ICECC_PREFERRED_HOST="$name" ICECC_DEBUG=debug \
    ICECC_LOGFILE="$work/warmup-$name.log" timeout 150 "$root/client/icecc" \
    g++ -std=c++17 -O2 -c "$work/warmup.cpp" -o "$work/out/warmup-$name.o" \
    >"$work/warmup-$name.stdout.log" 2>&1 || :
done
for name in "${names[@]}"; do
  ready=0
  for _ in $(seq 1 45); do
    grep -E "RELOGIN $name.*\\[[^]]+\\].*cache_profiles=.*zstd_tu" "$scheduler_log" \
      >/dev/null 2>&1 && { ready=1; break; }
    sleep 1
  done
  [ "$ready" -eq 1 ] || { echo "S4_STATUS=HOLD reason=worker-$name-environment-not-ready"; exit 77; }
done
sleep 5
before=$(wc -l <"$scheduler_log")
pids=()
for i in $(seq 1 "$worker_count"); do
  name=${names[$((i-1))]}
  (
    env ICECC_TEST_SOCKET="$work/c.sock" ICECC_TEST_REMOTEBUILD=1 ICECC_VERSION="$envtar" \
      ICECC_PREFERRED_HOST="$name" ICECC_DEBUG=debug ICECC_P50_C1F1_REQUIRED=1 \
      ICECC_P50_PROFILE=ZSTD_TU ICECC_LOGFILE="$work/client-debug-$i.log" \
      timeout 180 "$root/client/icecc" g++ -std=c++17 -O2 -c "$work/main-$i.cpp" \
      -o "$work/out/remote-$i.o" >"$work/client-$i.log" 2>&1
  ) &
  pids+=("$!")
done
compile_failed=0
for i in $(seq 1 "$worker_count"); do
  wait "${pids[$((i-1))]}" || compile_failed=$i
done
[ "$compile_failed" -eq 0 ] || {
  echo "S4_STATUS=FAIL reason=parallel-remote-compile-$compile_failed"; exit 1;
}
for i in $(seq 1 "$worker_count"); do
  name=${names[$((i-1))]}
  cmp -s "$work/out/remote-$i.o" "$work/out/local-$i.o" || {
    echo "S4_STATUS=FAIL reason=object-not-byte-identical-$name"; exit 1;
  }
  sha=$(sha256sum "$work/out/remote-$i.o" | awk '{print $1}')
  bytes=$(stat -c %s "$work/out/remote-$i.o")
  printf 'S4_OBJECT_%s=%s:%s:%s\n' "$i" "$name" "$sha" "$bytes"
done
tail -n "+$((before+1))" "$scheduler_log" >"$work/final-scheduler.log"
for name in "${names[@]}"; do
  grep -E "BEGIN: .*server=$name([ (]|$)" "$work/final-scheduler.log" >/dev/null || {
    echo "S4_STATUS=HOLD reason=preferred-worker-not-selected-$name"; exit 77;
  }
done
cache_seen=0; legacy_seen=0
grep -E 'ZSTD_TU|CACHE_SESSION' "$work/cdaemon.log" "$work/client-"*.log \
  "$work/client-debug-"*.log >/dev/null 2>&1 && cache_seen=1 || :
grep -E 'write_fd_to_server from cpp|write_fd_to_server preprocessed|building myself|building_local|local build forced|client_exception|fallback_local' \
  "$work/cdaemon.log" "$work/client-"*.log "$work/client-debug-"*.log \
  >/dev/null 2>&1 && legacy_seen=1 || :
printf 'S4_CACHE_OBSERVED=%s\nS4_LEGACY_OBSERVED=%s\n' "$cache_seen" "$legacy_seen"
[ "$cache_seen" -eq 1 ] || { echo 'S4_STATUS=HOLD reason=client-cache-not-observed'; exit 77; }
[ "$legacy_seen" -eq 0 ] || { echo 'S4_STATUS=FAIL reason=legacy-fallback-observed'; exit 1; }
selection=$(IFS=,; printf '%s' "${names[*]}")
printf 'S4_WORKER_SELECTION=%s\n' "$selection"
printf 'S4_REMOTE_COMPILE=%s\n' "$worker_count"
printf 'S4_BYTE_IDENTICAL=%s\n' "$worker_count"
echo 'S4_STATUS=PASS reason=physical-workers-remote-byte-identical'
exit 0
"""


def parse_work(stdout: str, marker: str, prefix: str) -> str:
    match = re.search(rf"^{re.escape(marker)}=({re.escape(prefix)}[A-Za-z0-9]+)$", stdout, re.M)
    if not match:
        raise HoldError(f"remote launcher did not return {marker}")
    return match.group(1)


def start_scheduler(
    stage: str,
    identity: dict[str, Any],
    container: str,
    port: int,
    network: str,
    timeout: float,
) -> str:
    args = [stage, PINNED_IMAGE, container, str(port), network,
            str(identity["uid"]), str(identity["gid"])]
    result = run_script("q3", START_SCHEDULER_SCRIPT, args, timeout=timeout)
    if result.returncode != 0:
        raise HoldError(f"scheduler launch failed: {(result.stderr or result.stdout)[-400:]}")
    return parse_work(result.stdout, "S4_REMOTE_WORK", "/tmp/s4-p50-fourhost-scheduler.")


def start_worker(
    host: str,
    stage: str,
    identity: dict[str, Any],
    container: str,
    scheduler_port: int,
    network: str,
    worker_port: int,
    timeout: float,
) -> str:
    args = [stage, PINNED_IMAGE, container, HOSTS["q3"]["lan"], str(scheduler_port),
            network, WORKER_NAMES[host], str(worker_port), str(identity["uid"]),
            str(identity["gid"])]
    result = run_script(host, START_WORKER_SCRIPT, args, timeout=timeout)
    if result.returncode != 0:
        raise HoldError(f"{host} worker launch failed: {(result.stderr or result.stdout)[-400:]}")
    return parse_work(result.stdout, "S4_REMOTE_WORK", "/tmp/s4-p50-fourhost-worker.")


READ_FILE_SCRIPT = r"""
set -eu
file=$1
[ -f "$file" ] || exit 77
cat "$file"
"""


def read_remote_file(host: str, path: str, timeout: float) -> str:
    result = run_script(host, READ_FILE_SCRIPT, [path], timeout=timeout)
    if result.returncode != 0:
        raise HoldError(f"{host} evidence file unavailable: {path}")
    return result.stdout


def wait_for_workers(
    scheduler_work: str,
    worker_names: list[str],
    timeout: float,
) -> str:
    deadline = time.monotonic() + min(timeout, 120)
    last = ""
    while time.monotonic() < deadline:
        last = read_remote_file("q3", f"{scheduler_work}/scheduler.log", min(timeout, 30))
        if all(
            re.search(rf"RELOGIN {re.escape(name)}.*cache_profiles=.*zstd_tu", last)
            for name in worker_names
        ):
            return last
        time.sleep(1)
    raise HoldError(f"{len(worker_names)} cache-capable physical workers did not register")


def run_client(
    stage: str,
    scheduler_work: str,
    scheduler_port: int,
    network: str,
    client_port: int,
    load: str,
    worker_names: list[str],
    timeout: float,
) -> tuple[subprocess.CompletedProcess[str], str]:
    args = [stage, HOSTS["q3"]["lan"], str(scheduler_port), network,
            f"{scheduler_work}/scheduler.log", str(client_port), load,
            *worker_names]
    result = run_script("q3", CLIENT_SCRIPT, args, timeout=timeout)
    work = parse_work(result.stdout, "S4_CLIENT_WORK", "/tmp/s4-p50-fourhost-client.")
    return result, work


def copy_remote_tree(host: str, remote: str, destination: Path, timeout: float) -> None:
    allowed = (
        "/tmp/s4-p50-fourhost-scheduler.",
        "/tmp/s4-p50-fourhost-worker.",
        "/tmp/s4-p50-fourhost-client.",
    )
    if not remote.startswith(allowed) or not re.fullmatch(r"/tmp/s4-p50-fourhost-[a-z]+\.[A-Za-z0-9]+", remote):
        raise HoldError(f"refusing unexpected evidence directory: {remote}")
    destination.mkdir(parents=True, exist_ok=False)
    source = subprocess.Popen(
        [*ssh_argv(host), "tar", "-C", remote, "-cf", "-", "."],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert source.stdout is not None
    sink = subprocess.Popen(
        ["tar", "-C", str(destination), "-xf", "-"],
        stdin=source.stdout,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    source.stdout.close()
    _, sink_error = sink.communicate(timeout=timeout)
    source_error = source.stderr.read() if source.stderr else b""
    source_rc = source.wait(timeout=30)
    if source_rc != 0 or sink.returncode != 0:
        raise HoldError(
            f"copying {host}:{remote} failed: "
            + (source_error + sink_error)[-400:].decode(errors="replace")
        )


def parse_fields(stdout: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for line in stdout.splitlines():
        if line.startswith("S4_") and "=" in line:
            key, value = line.split("=", 1)
            fields[key] = value
    return fields


def worker_cache_observed(log: str) -> bool:
    return bool(re.search(r"ZSTD_TU|CACHE_SESSION|attached exact ZSTD_TU input", log))


def worker_legacy_observed(log: str) -> bool:
    return bool(re.search(
        r"write_fd_to_server from cpp|write_fd_to_server preprocessed|building myself|"
        r"building_local|local build forced|fallback_local",
        log,
    ))


def classify_result(
    client_returncode: int,
    fields: dict[str, str],
    worker_logs: dict[str, str],
    worker_names: list[str] | None = None,
) -> tuple[str, str]:
    if worker_names is None:
        worker_names = list(WORKER_NAMES.values())
    marker = fields.get("S4_STATUS", "")
    if client_returncode == 77 or marker.startswith("HOLD"):
        return "HOLD", marker.split("reason=", 1)[-1] or "client-hold"
    if client_returncode != 0 or not marker.startswith("PASS"):
        return "FAIL", marker.split("reason=", 1)[-1] or "client-failure"
    expected_count = str(len(worker_names))
    expected_selection = ",".join(worker_names)
    if (
        fields.get("S4_REMOTE_COMPILE") != expected_count
        or fields.get("S4_BYTE_IDENTICAL") != expected_count
    ):
        return "FAIL", f"{expected_count}-compile-byte-ledger-incomplete"
    if fields.get("S4_WORKER_SELECTION") != expected_selection:
        return "HOLD", f"{expected_count}-physical-worker-selection-incomplete"
    if fields.get("S4_CACHE_OBSERVED") != "1" or fields.get("S4_LEGACY_OBSERVED") != "0":
        return "FAIL", "client-cache-path-observation-differs"
    if not all(worker_cache_observed(log) for log in worker_logs.values()):
        return "HOLD", "cache-path-not-observed-on-every-physical-worker"
    if any(worker_legacy_observed(log) for log in worker_logs.values()):
        return "FAIL", "legacy-path-observed-on-physical-worker"
    return "PASS", f"{expected_count}-physical-workers-remote-byte-identical"


CLEANUP_SCRIPT = r"""
set -eu
container=$1; work=$2; stage=$3
docker rm -f "$container" >/dev/null 2>&1 || true
case "$work" in
  /tmp/s4-p50-fourhost-scheduler.*|/tmp/s4-p50-fourhost-worker.*|/tmp/s4-p50-fourhost-client.*)
    rm -rf -- "$work" ;;
  "") ;;
  *) echo "refuse-work:$work"; exit 77 ;;
esac
case "$stage" in
  /tmp/s4-p50-fourhost.*) rm -rf -- "$stage" ;;
  "") ;;
  *) echo "refuse-stage:$stage"; exit 77 ;;
esac
"""


def cleanup_remote(
    host: str,
    container: str,
    work: str,
    stage: str,
    timeout: float,
) -> dict[str, Any]:
    try:
        result = run_script(host, CLEANUP_SCRIPT, [container, work, stage], timeout=timeout)
        return {
            "returncode": result.returncode,
            "stdout": result.stdout,
            "stderr": result.stderr,
        }
    except (OSError, subprocess.SubprocessError) as exc:
        return {"returncode": None, "error": f"{type(exc).__name__}: {exc}"}


def write_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, sort_keys=True, indent=2) + "\n", encoding="utf-8")


def evidence_hashes(root: Path) -> dict[str, str]:
    return {
        str(path.relative_to(root)): sha256(path)
        for path in sorted(root.rglob("*"))
        if path.is_file() and path.name != "evidence-sha256.txt"
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--p50-root", type=Path, required=True)
    parser.add_argument(
        "--out-root",
        type=Path,
        default=Path("/tanksmall/scratch/ictmp/experiments/icecream"),
    )
    parser.add_argument("--timeout", type=float, default=900)
    parser.add_argument("--load", choices=("same", "mixed"), default="same")
    parser.add_argument("--topology", choices=("c1f2", "c1f4"), default="c1f4")
    args = parser.parse_args(argv)
    if args.timeout <= 0:
        parser.error("--timeout must be positive")

    started = datetime.now(timezone.utc)
    stamp = started.strftime("%Y%m%dT%H%M%SZ")
    selected_hosts = list(HOSTS)[:2] if args.topology == "c1f2" else list(HOSTS)
    selected_worker_names = [WORKER_NAMES[host] for host in selected_hosts]
    topology = args.topology.upper()
    run_root = args.out_root / f"s4-physical-contention-{args.topology}-{args.load}-{stamp}"
    run_root.mkdir(parents=True, exist_ok=False)
    local_hashes: dict[str, str] = {}
    identities: dict[str, dict[str, Any]] = {}
    stages: dict[str, str] = {}
    works: dict[str, str] = {}
    containers: dict[str, str] = {}
    cleanup: dict[str, Any] = {}
    result: dict[str, Any] = {}
    status = "HOLD"
    reason = "preflight-not-complete"
    scheduler_port = 27000 + (os.getpid() % 500)
    worker_base = scheduler_port + 1000
    client_port = scheduler_port + 2000
    run_token = re.sub(r"[^a-z0-9]", "", stamp.lower())[-12:] + f"-{os.getpid()}"
    network = f"s4-p50-{run_token}"

    try:
        require_no_s5()
        local_hashes = validate_local_roles(args.p50_root.absolute())
        identities = host_preflight(selected_hosts, min(args.timeout, 60))
        preflight = {
            "schema": SCHEMA,
            "started_at": started.isoformat(),
            "base_source_sha": BASE_SOURCE_SHA,
            "p50_root": str(args.p50_root.absolute()),
            "role_hashes": local_hashes,
            "hosts": identities,
            "source_image": SOURCE_IMAGE,
            "pinned_image": PINNED_IMAGE,
            "expected_image_id": EXPECTED_IMAGE_ID,
            "network": network,
            "scheduler_port": scheduler_port,
            "worker_ports": {
                host: worker_base + index for index, host in enumerate(selected_hosts)
            },
            "client_port": client_port,
            "load": args.load,
            "topology": topology,
        }
        write_json(run_root / "preflight.json", preflight)

        pin_image_on_existing_hosts(selected_hosts, min(args.timeout, 120))
        if "research7" in selected_hosts:
            copy_pinned_image_to_research7(args.timeout)
        image_ids = {host: image_id(host, PINNED_IMAGE, 60) for host in selected_hosts}
        if not valid_image_closure(image_ids):
            raise HoldError(f"pinned image IDs differ across hosts: {image_ids}")

        for host in selected_hosts:
            stages[host] = stage_roles(host, args.p50_root.absolute(), args.timeout)
            verification = verify_stage(host, stages[host], min(args.timeout, 60))
            (run_root / f"{host}-stage-verify.txt").write_text(verification, encoding="utf-8")

        containers["q3-scheduler"] = f"s4-p50-scheduler-{run_token}"
        works["q3-scheduler"] = start_scheduler(
            stages["q3"], identities["q3"], containers["q3-scheduler"],
            scheduler_port, network, min(args.timeout, 120),
        )
        for index, host in enumerate(selected_hosts):
            key = f"{host}-worker"
            containers[key] = f"s4-p50-{host}-worker-{run_token}"
            works[key] = start_worker(
                host, stages[host], identities[host], containers[key], scheduler_port,
                network, worker_base + index, min(args.timeout, 120),
            )

        scheduler_registration = wait_for_workers(
            works["q3-scheduler"], selected_worker_names, args.timeout
        )
        (run_root / "scheduler-registration.log").write_text(
            scheduler_registration, encoding="utf-8"
        )
        client_result, client_work = run_client(
            stages["q3"], works["q3-scheduler"], scheduler_port, network,
            client_port, args.load, selected_worker_names, args.timeout,
        )
        works["q3-client"] = client_work
        (run_root / "client.stdout").write_text(client_result.stdout, encoding="utf-8")
        (run_root / "client.stderr").write_text(client_result.stderr, encoding="utf-8")

        copy_remote_tree("q3", works["q3-scheduler"], run_root / "q3-scheduler", args.timeout)
        copy_remote_tree("q3", client_work, run_root / "q3-client", args.timeout)
        worker_logs: dict[str, str] = {}
        for host in selected_hosts:
            key = f"{host}-worker"
            copy_remote_tree(host, works[key], run_root / f"{host}-worker", args.timeout)
            worker_log_path = run_root / f"{host}-worker" / "fdaemon.log"
            worker_logs[host] = (
                worker_log_path.read_text(encoding="utf-8", errors="replace")
                if worker_log_path.is_file()
                else ""
            )

        fields = parse_fields(client_result.stdout)
        status, reason = classify_result(
            client_result.returncode, fields, worker_logs, selected_worker_names
        )
        result = {
            "schema": SCHEMA,
            "status": status,
            "reason": reason,
            "base_source_sha": BASE_SOURCE_SHA,
            "profile": "ZSTD_TU",
            "topology": topology,
            "load": args.load,
            "scheduler_host": "q3",
            "client_host": "q3",
            "physical_worker_hosts": selected_hosts,
            "worker_names": {
                host: WORKER_NAMES[host] for host in selected_hosts
            },
            "worker_lan_addresses": {
                host: HOSTS[host]["lan"] for host in selected_hosts
            },
            "worker_selection": fields.get("S4_WORKER_SELECTION", "").split(","),
            "remote_compile_count": int(fields.get("S4_REMOTE_COMPILE", "0") or 0),
            "byte_identical_count": int(fields.get("S4_BYTE_IDENTICAL", "0") or 0),
            "cache_observed_client": fields.get("S4_CACHE_OBSERVED") == "1",
            "cache_observed_workers": {
                host: worker_cache_observed(log) for host, log in worker_logs.items()
            },
            "legacy_observed_client": fields.get("S4_LEGACY_OBSERVED") == "1",
            "legacy_observed_workers": {
                host: worker_legacy_observed(log) for host, log in worker_logs.items()
            },
            "objects": {
                key.removeprefix("S4_OBJECT_"): value
                for key, value in fields.items()
                if key.startswith("S4_OBJECT_")
            },
            "role_hashes": local_hashes,
            "image_ids": image_ids,
            "pinned_image": PINNED_IMAGE,
            "network": network,
            "ports": {
                "scheduler": scheduler_port,
                "client": client_port,
                "workers": {
                    host: worker_base + index
                    for index, host in enumerate(selected_hosts)
                },
            },
            "client_returncode": client_result.returncode,
            "started_at": started.isoformat(),
            "finished_at": datetime.now(timezone.utc).isoformat(),
            "evidence": str(run_root),
        }
    except HoldError as exc:
        status, reason = "HOLD", str(exc)
    except subprocess.TimeoutExpired as exc:
        status, reason = "HOLD", f"timeout:{exc.cmd}"
    except (OSError, subprocess.SubprocessError) as exc:
        status, reason = "HOLD", f"environment:{type(exc).__name__}:{exc}"
    except Exception as exc:  # Product/harness failures must remain visible in results.jsonl.
        status, reason = "FAIL", f"{type(exc).__name__}:{exc}"
    finally:
        # Stop every consumer before removing any staged role root.  In
        # particular, q3's worker and scheduler share the same read-only root.
        for host in reversed(selected_hosts):
            key = f"{host}-worker"
            if containers.get(key) or works.get(key):
                cleanup[key] = cleanup_remote(
                    host, containers.get(key, ""), works.get(key, ""), "",
                    min(args.timeout, 120),
                )
            else:
                cleanup[key] = {"returncode": 0, "status": "not-started"}
        if works.get("q3-client"):
            cleanup["q3-client"] = cleanup_remote(
                "q3", "", works["q3-client"], "", min(args.timeout, 120)
            )
        else:
            cleanup["q3-client"] = {"returncode": 0, "status": "not-started"}
        if containers.get("q3-scheduler") or works.get("q3-scheduler"):
            cleanup["q3-scheduler"] = cleanup_remote(
                "q3", containers.get("q3-scheduler", ""),
                works.get("q3-scheduler", ""), "", min(args.timeout, 120),
            )
        else:
            cleanup["q3-scheduler"] = {"returncode": 0, "status": "not-started"}
        for host in reversed(selected_hosts):
            if stages.get(host):
                cleanup[f"{host}-stage"] = cleanup_remote(
                    host, "", "", stages[host], min(args.timeout, 120)
                )
            else:
                cleanup[f"{host}-stage"] = {"returncode": 0, "status": "not-staged"}
        write_json(run_root / "cleanup.json", cleanup)

    if not result:
        result = {
            "schema": SCHEMA,
            "status": status,
            "reason": reason,
            "base_source_sha": BASE_SOURCE_SHA,
            "profile": "ZSTD_TU",
            "topology": topology,
            "load": args.load,
            "physical_worker_hosts": selected_hosts,
            "role_hashes": local_hashes,
            "started_at": started.isoformat(),
            "finished_at": datetime.now(timezone.utc).isoformat(),
            "evidence": str(run_root),
        }
    result["status"] = status
    result["reason"] = reason
    result["cleanup"] = cleanup
    result_path = run_root / "results.jsonl"
    result_path.write_text(
        json.dumps(result, sort_keys=True, separators=(",", ":")) + "\n", encoding="utf-8"
    )
    hashes = evidence_hashes(run_root)
    (run_root / "evidence-sha256.txt").write_text(
        "".join(f"{digest}  {relative}\n" for relative, digest in hashes.items()),
        encoding="utf-8",
    )
    print(json.dumps({"status": status, "reason": reason, "run_root": str(run_root)}, sort_keys=True))
    return 0 if status in {"PASS", "HOLD"} else 1


if __name__ == "__main__":
    raise SystemExit(main())
