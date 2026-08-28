#!/usr/bin/env python3
"""Run the eight real S4 scheduler/client/worker version cells.

This is deliberately a small execution harness.  It does not publish role
artifacts, create capability/offer records, or infer a result from a model:
each row is produced by one SSH invocation of the three selected binaries on
q3.  The remote cell compiles one TU through the real client and compares the
returned object with a local g++ compilation.  The only cell which enables
the Protocol-50 cache requirement is ``s50-c50-f50``; every other cell starts
the ordinary legacy FileChunk path.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import shlex
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path


SCHEMA = "icecream-s4-real-cell-v1"
BASE_SHA = "b2027f4b1e536700b99923766e5937163b3bea29"
P43_SHA = "cd74801e0fa4e83e3ae254ca1d7fe98642f36b89"
# P50 must be supplied as a build of BASE_SHA. --p50-local-root stages that
# exact build into a private q3 /tmp root for the cell run.
P50_SOURCE_SHA = BASE_SHA
P43_ROOT = "$HOME/role-artifacts/store/p43/6da186b6c4c10f2f15d9e5440156ed890112523f53e905f9dd0d2f314540a3ae"
P50_ROOT = ""

# The hashes are also checked remotely before any selected file is exec'd.
# This prevents a cell result from silently using a mutable or wrong-version
# role root. The P50 hashes below are for the exact BASE_SHA build staged by
# the runner; old 43297d artifacts are intentionally rejected.
ROLE_HASHES = {
    "43": {
        "S": "c205c1347064503b3ab7c56af4584ffb0806f52e2c5786d343883db871c44525",
        "F": "62bcc05ad91461212cd9616d960905c97c839b316aac3a9e1e1080608a193441",
        "C": "c3dc54eadca303f21eaa1f90c265a38a1038378be08f887b24bb3bbea7dabeae",
        "E": "aab94b6ea8f41335de807f814d24a56e827ce5efaf8b06797a365699e83b36cb",
    },
    "50": {
        "S": "7d657bd50860186f5e205d3d7b9943340c2cb97110a2e19da8b41ceafc1f58b0",
        "F": "e106e323cde9d086aa8ff17b35377558e945ba896bffbb3d789025fe1a349adb",
        "C": "ee583a189db8daa5e295d7c8505ab55ff5f91014078758c7baddff4a49b2dfe0",
        "E": "1082d3007d28ae9cbf7900e21f998ac8881fad548d26c809ed6b00044b67c700",
        "X": "a4494f1914220cb0482e6d5eba252bbf4f0e7cb04e93f2ca939811efaa60a718",
    },
}


REMOTE_SCRIPT = r'''#!/bin/bash
set -u
SROOT=$1; CROOT=$2; FROOT=$3; CELL=$4; EXPECT_CACHE=$5
SROOT=${SROOT//\$HOME/$HOME}; CROOT=${CROOT//\$HOME/$HOME}; FROOT=${FROOT//\$HOME/$HOME}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/s4-real-cell.XXXXXX")
SCHED_PID=; F_PID=; C_PID=; ENV_TAR=; F_CONTAINER="s4-$CELL-$$"; CLEANUP_STARTED=$(date +%s)
cleanup() {
    rc=$?
    # Disarm first: EXIT is delivered again by the final `exit` below.
    trap - EXIT HUP INT TERM
    for pid in "$C_PID" "$F_PID" "$SCHED_PID"; do
        [ -n "$pid" ] && kill "$pid" 2>/dev/null || :
    done
    docker rm -f "$F_CONTAINER" >/dev/null 2>&1 || :
    deadline=$(( $(date +%s) + 15 )); forced=0
    while [ "$(date +%s)" -lt "$deadline" ]; do
        live=0
        for pid in "$C_PID" "$F_PID" "$SCHED_PID"; do
            [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null && live=1 || :
        done
        [ "$live" -eq 0 ] && break
        sleep 0.1
    done
    for pid in "$C_PID" "$F_PID" "$SCHED_PID"; do
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            forced=1; kill -KILL "$pid" 2>/dev/null || :
        fi
    done
    for pid in "$C_PID" "$F_PID" "$SCHED_PID"; do
        [ -n "$pid" ] && wait "$pid" 2>/dev/null || :
    done
    CLEANUP_END=$(date +%s)
    printf 'S4_CLEANUP=bounded forced=%s seconds=%s\n' "$forced" "$((CLEANUP_END-CLEANUP_STARTED))"
    # Emit logs before deleting the private work tree.  Each value is one
    # base64 line, so arbitrary compiler output cannot corrupt the result row.
    for name in scheduler fdaemon cdaemon client client-debug scheduler.stdout fdaemon.stdout cdaemon.stdout create-env local; do
        file="$WORK/$name.log"
        [ -f "$file" ] || : >"$file"
        printf '__S4_LOG_BEGIN=%s\n' "$name"
        base64 -w0 "$file" 2>/dev/null || base64 "$file"
        printf '\n__S4_LOG_END=%s\n' "$name"
    done
    rm -rf "$WORK"
    exit "$rc"
}
trap cleanup EXIT HUP INT TERM

bad=0
check_file() {
    role=$1; path=$2; expected=$3
    [ -f "$path" ] && [ ! -L "$path" ] && { [ "$role" = E ] || [ -x "$path" ]; } || {
        echo "S4_REFUSE=missing-$role:$path"; bad=1; return
    }
    got=$(sha256sum "$path" 2>/dev/null | awk '{print $1}')
    [ "$got" = "$expected" ] || { echo "S4_REFUSE=hash-$role:$got"; bad=1; }
}

# Paths are expanded by the remote shell, never by the hub.  A build root is
# accepted as well as a role-artifact root, which lets callers use a fresh
# build made from BASE_SHA without copying it into the immutable store.
pick() {
    root=$1; role=$2
    case "$role" in
      S) names="obj/scheduler/icecc-scheduler scheduler/icecc-scheduler";;
      F) names="obj/daemon/iceccd daemon/iceccd";;
      C) names="obj/client/icecc client/icecc";;
      E) names="obj/client/icecc-create-env client/icecc-create-env";;
    esac
    for rel in $names; do [ -f "$root/$rel" ] && { printf '%s/%s' "$root" "$rel"; return; }; done
    return 1
}

S=$(pick "$SROOT" S || true); C=$(pick "$CROOT" C || true)
CDAEMON=$(pick "$CROOT" F || true)
F=$(pick "$FROOT" F || true); E=$(pick "$CROOT" E || true)
F_REL=${F#"$FROOT"/}
for spec in \
  "S:$S:$S_HASH" "C:$C:$C_HASH" "D:$CDAEMON:$D_HASH" "F:$F:$F_HASH" "E:$E:$E_HASH"; do
    role=${spec%%:*}; rest=${spec#*:}; path=${rest%%:*}; hash=${rest#*:}
    check_file "$role" "$path" "$hash"
done
[ "$bad" -eq 0 ] || { echo 'S4_STATUS=HOLD reason=role-root-integrity'; exit 77; }

command -v g++ >/dev/null 2>&1 || { echo 'S4_STATUS=HOLD reason=g++-unavailable'; exit 77; }
command -v timeout >/dev/null 2>&1 || { echo 'S4_STATUS=HOLD reason=timeout-unavailable'; exit 77; }
mkdir -p "$WORK/env" "$WORK/envs-f" "$WORK/envs-c" "$WORK/out"
chmod 1777 "$WORK/envs-f" "$WORK/envs-c"
printf '%s\n' '#include <cstdint>' \
  'extern "C" int s4_real_cell() { return 43 + 7; }' >"$WORK/main.cpp"

SCHED_PORT=$((24000 + $$ % 1000)); F_PORT=$((25000 + $$ % 1000)); NET="s4-$CELL-$$"
S_EXTRA=""
case "$CELL" in s50-c50-f50) S_EXTRA="--assignment-fence-mode strict-nonce";; esac
"$S" -p "$SCHED_PORT" -n "$NET" -l "$WORK/scheduler.log" -vvv $S_EXTRA >"$WORK/scheduler.stdout" 2>&1 & SCHED_PID=$!
sleep 1
kill -0 "$SCHED_PID" 2>/dev/null || { echo 'S4_STATUS=FAIL reason=scheduler-exited'; exit 1; }

# No cache-service is started for mixed cells.  Thus even a P50 daemon is
# forced through its whole-TU legacy transport.  50/50/50 alone may use the
# daemon-owned cache sidecar when one exists in the supplied build root.
F_EXTRA=""
if [ "$EXPECT_CACHE" = 1 ]; then
    SERVICE=$(find "$FROOT" -type f -name icecc-cache-service -perm -100 -print -quit 2>/dev/null || true)
    [ -n "$SERVICE" ] || { echo 'S4_STATUS=HOLD reason=p50-cache-service-unavailable'; exit 77; }
    F_EXTRA="--cache-service /role/cache/icecc-cache-service --cache-runtime-dir /work/cache-runtime"
    mkdir -p "$WORK/cache-runtime"; chmod 700 "$WORK/cache-runtime"
fi

# The farm image has no icecc account.  The daemon must drop to a user which
# owns the bind-mounted /work directories; create that account inside this
# ephemeral container, using q3's uid/gid, before exec'ing the exact worker.
cat >"$WORK/f-wrapper.sh" <<'S4_F_WRAPPER'
#!/bin/sh
set -eu
printf 'icecc:x:%s:%s:icecc:/nonexistent:/usr/sbin/nologin\n' "$S4_F_UID" "$S4_F_GID" >>/etc/passwd
printf 'icecc:x:%s:\n' "$S4_F_GID" >>/etc/group
exec /role/__S4_F_REL__ "$@"
S4_F_WRAPPER
sed -i "s#__S4_F_REL__#$F_REL#" "$WORK/f-wrapper.sh"
chmod 755 "$WORK/f-wrapper.sh"
docker run --rm --name "$F_CONTAINER" --network host --user 0 --cap-add=SYS_CHROOT \
  -v "$FROOT:/role:ro" -v "$WORK:/work" \
  -e ICECC_TEST_SOCKET=/work/f.sock -e S4_F_UID="$(id -u)" -e S4_F_GID="$(id -g)" \
  --entrypoint /work/f-wrapper.sh icecream/farm-node:ubuntu22-gcc11-boost174 \
  -p "$F_PORT" -m 2 -s "127.0.0.1:$SCHED_PORT" -n "$NET" -N s4-f \
  -b /work/envs-f -l /work/fdaemon.log -vvv $F_EXTRA \
  >"$WORK/fdaemon.stdout" 2>&1 & F_PID=$!
sleep 2
docker inspect --format 'S4_DOCKER_STATE={{.State.Status}} exit={{.State.ExitCode}}' "$F_CONTAINER" 2>/dev/null || echo 'S4_DOCKER_STATE=missing'
docker logs "$F_CONTAINER" >>"$WORK/fdaemon.stdout" 2>&1 || :
ICECC_TEST_SOCKET="$WORK/c.sock" "$CDAEMON" --no-remote -m 0 \
  -s "127.0.0.1:$SCHED_PORT" -n "$NET" -N s4-c -b "$WORK/envs-c" \
  -l "$WORK/cdaemon.log" -vvv >"$WORK/cdaemon.stdout" 2>&1 & C_PID=$!

for _ in $(seq 1 30); do
    logins=$(grep -c login "$WORK/scheduler.log" 2>/dev/null || true)
    [ "${logins:-0}" -ge 2 ] && break
    sleep 1
done
[ "${logins:-0}" -ge 2 ] || { echo 'S4_STATUS=FAIL reason=daemon-registration'; exit 1; }
if [ "$EXPECT_CACHE" = 1 ]; then
    cache_ready=0
    for _ in $(seq 1 30); do
        if grep -E 'RELOGIN s4-f.*cache=.*cache_profiles=.*zstd_tu' \
            "$WORK/scheduler.log" >/dev/null 2>&1; then
            cache_ready=1; break
        fi
        sleep 1
    done
    [ "$cache_ready" -eq 1 ] || { echo 'S4_STATUS=HOLD reason=cache-worker-not-ready'; exit 77; }
fi

( cd "$WORK/env" && timeout 120 bash "$E" "$(command -v g++)" >"$WORK/create-env.log" 2>&1 ) || {
    echo 'S4_STATUS=FAIL reason=create-env'; exit 1;
}
ENV_TAR=$(find "$WORK/env" -maxdepth 1 -type f -name '*.tar.gz' -print -quit)
[ -n "$ENV_TAR" ] || { echo 'S4_STATUS=FAIL reason=no-compiler-environment'; exit 1; }

REMOTE_OBJ="$WORK/out/remote.o"; LOCAL_OBJ="$WORK/out/local.o"
CLIENT_ENV=("ICECC_TEST_SOCKET=$WORK/c.sock" "ICECC_TEST_REMOTEBUILD=1"
            "ICECC_VERSION=$ENV_TAR" "ICECC_PREFERRED_HOST=s4-f"
            "ICECC_DEBUG=debug" "ICECC_LOGFILE=$WORK/client-debug.log")
[ "$EXPECT_CACHE" = 1 ] && CLIENT_ENV+=("ICECC_P50_C1F1_REQUIRED=1")
env "${CLIENT_ENV[@]}" timeout 150 "$C" g++ -std=c++17 -O2 -c "$WORK/main.cpp" -o "$REMOTE_OBJ" \
  >"$WORK/client.log" 2>&1 || { echo 'S4_STATUS=FAIL reason=remote-compile'; exit 1; }
g++ -std=c++17 -O2 -c "$WORK/main.cpp" -o "$LOCAL_OBJ" 2>"$WORK/local.log" || {
    echo 'S4_STATUS=FAIL reason=local-reference'; exit 1;
}
cmp -s "$REMOTE_OBJ" "$LOCAL_OBJ" || { echo 'S4_STATUS=FAIL reason=object-not-byte-identical'; exit 1; }

all_logs="$WORK/scheduler.log $WORK/fdaemon.log $WORK/cdaemon.log $WORK/client.log $WORK/client-debug.log"
cache_seen=0; legacy_seen=0; remote_seen=0
grep -E 'ZSTD_TU|CACHE_SESSION' $all_logs >/dev/null 2>&1 && cache_seen=1 || :
grep -E 'write_fd_to_server from cpp|write_fd_to_server preprocessed|building myself|building_local|local build forced|client_exception|fallback_local' $all_logs >/dev/null 2>&1 && legacy_seen=1 || :
grep -E 'BEGIN: .*server=s4-f([ (]|$)' "$WORK/scheduler.log" >/dev/null 2>&1 && remote_seen=1 || :
[ "$remote_seen" -eq 1 ] || {
    echo 'S4_REMOTE_COMPILE=0'
    echo 'S4_STATUS=HOLD reason=remote-worker-not-selected'
    exit 77
}
if [ "$EXPECT_CACHE" = 1 ]; then
    [ "$cache_seen" -eq 1 ] || { echo 'S4_STATUS=HOLD reason=cache-not-observed'; exit 77; }
    [ "$legacy_seen" -eq 0 ] || { echo 'S4_STATUS=FAIL reason=cache-cell-used-legacy-fallback'; exit 1; }
else
    [ "$cache_seen" -eq 0 ] || { echo 'S4_STATUS=FAIL reason=mixed-cell-engaged-cache'; exit 1; }
fi
echo "S4_CACHE_OBSERVED=$cache_seen"
echo "S4_LEGACY_OBSERVED=$legacy_seen"
echo "S4_REMOTE_COMPILE=$remote_seen"
echo 'S4_BYTE_IDENTICAL=1'
echo 'S4_STATUS=PASS reason=remote-byte-identical'
exit 0
'''


def _cells() -> list[dict[str, str]]:
    return [
        {"id": f"s{s}-c{c}-f{f}", "S": s, "C": c, "F": f}
        for s in ("43", "50") for c in ("43", "50") for f in ("43", "50")
    ]


def _sha(path: Path) -> str | None:
    if not path.is_file() or path.is_symlink():
        return None
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _parse_remote(stdout: str) -> tuple[dict[str, str], dict[str, str]]:
    fields: dict[str, str] = {}
    logs: dict[str, str] = {}
    lines = stdout.splitlines()
    for line in lines:
        if line.startswith("S4_") and "=" in line:
            key, value = line.split("=", 1)
            fields[key] = value
    index = 0
    while index < len(lines):
        marker = lines[index]
        if not marker.startswith("__S4_LOG_BEGIN="):
            index += 1
            continue
        name = marker.split("=", 1)[1]
        encoded = lines[index + 1] if index + 1 < len(lines) else ""
        try:
            logs[name] = base64.b64decode(encoded).decode("utf-8", "replace")
        except (ValueError, base64.binascii.Error):
            logs[name] = "<invalid base64 log>\n" + encoded
        index += 1
        while index < len(lines) and not lines[index].startswith("__S4_LOG_END="):
            index += 1
        index += 1
    return fields, logs


def _root_arg(root: str, version: str, p43_root: str, p50_root: str) -> str:
    if root:
        return root
    return p43_root if version == "43" else p50_root


def _stage_p50(args: argparse.Namespace) -> str:
    """Stage the exact local BASE_SHA build into a private q3 /tmp root."""
    source = Path(args.p50_local_root).absolute()
    required = {
        "scheduler/icecc-scheduler": "S", "daemon/iceccd": "F",
        "client/icecc": "C", "client/icecc-create-env": "E",
        "cache/icecc-cache-service": "X",
    }
    for relative, role in required.items():
        path = source / relative
        expected = ROLE_HASHES["50"].get(role)
        if not path.is_file() or path.is_symlink() or (role != "E" and not os.access(path, os.X_OK)):
            raise RuntimeError(f"exact P50 build missing {relative}")
        if expected and _sha(path) != expected:
            raise RuntimeError(f"exact P50 build hash mismatch {relative}")
    files = list(required)
    tar_command = ["tar", "-C", str(source), "-cf", "-", *files]
    tar_proc = subprocess.Popen(tar_command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    command_text = 'set -eu; root=$(mktemp -d /tmp/s4-p50-build.XXXXXX); mkdir -p "$root/scheduler" "$root/daemon" "$root/client" "$root/cache"; tar -xf - -C "$root"; printf "S4_P50_STAGE_ROOT=%s\\n" "$root"'
    extract = subprocess.Popen(["ssh", *args.ssh_option, args.host, "bash", "-c", command_text], stdin=tar_proc.stdout, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert tar_proc.stdout is not None
    tar_proc.stdout.close()
    output, error = extract.communicate(timeout=args.timeout)
    tar_rc = tar_proc.wait(timeout=10)
    if extract.returncode != 0 or tar_rc != 0:
        raise RuntimeError(f"remote P50 staging failed: {error[-300:].decode(errors='replace')}")
    line = next((line for line in output.decode(errors="replace").splitlines()
                 if line.startswith("S4_P50_STAGE_ROOT=")), "")
    if not line:
        raise RuntimeError("remote P50 staging returned no root")
    return line.split("=", 1)[1]


def _run_cell(cell: dict[str, str], args: argparse.Namespace, out_root: Path) -> dict[str, object]:
    expect_cache = cell["id"] == "s50-c50-f50"
    cell_dir = out_root / cell["id"]
    cell_dir.mkdir()
    if any(cell[role] == "50" for role in ("S", "C", "F")) and not args.p50_root:
        return {
            "schema": SCHEMA, "base_source_sha": BASE_SHA, "p43_source_sha": P43_SHA,
            "p50_source_sha": P50_SOURCE_SHA, "cell": cell["id"],
            "scheduler_version": cell["S"], "client_version": cell["C"],
            "worker_version": cell["F"], "cache_expected": expect_cache,
            "cache_observed": False, "legacy_observed": False, "remote_compile": False,
            "byte_identical": False, "cleanup": "not-started", "status": "HOLD",
            "reason": "exact-p50-build-unavailable", "returncode": None,
            "command": None, "roots": {}, "evidence": str(cell_dir),
        }
    roots = {
        role: _root_arg(getattr(args, f"{role.lower()}_root"), cell[role], args.p43_root, args.p50_root)
        for role in ("S", "C", "F")
    }
    # ssh concatenates the post-host arguments into a remote shell command;
    # quote each data argument and let the remote preamble expand its literal
    # ``$HOME`` token.  This keeps an override from becoming shell syntax.
    remote_args = [shlex.quote(value) for value in
                   (roots["S"], roots["C"], roots["F"], cell["id"],
                    "1" if expect_cache else "0")]
    command = ["ssh", *args.ssh_option, args.host, "bash", "-s", "--", *remote_args]
    script = REMOTE_SCRIPT.replace("$S_HASH", ROLE_HASHES[cell["S"]]["S"])
    script = script.replace("$C_HASH", ROLE_HASHES[cell["C"]]["C"])
    script = script.replace("$D_HASH", ROLE_HASHES[cell["C"]]["F"])
    script = script.replace("$F_HASH", ROLE_HASHES[cell["F"]]["F"])
    script = script.replace("$E_HASH", ROLE_HASHES[cell["C"]]["E"])
    # Shell variables cannot safely carry a path containing spaces through a
    # command substitution; this placeholder is replaced with the quoted C
    # executable path in the generated script.
    completed: subprocess.CompletedProcess[str] | None = None
    error = ""
    try:
        completed = subprocess.run(
            command, input=script, text=True, capture_output=True,
            timeout=args.timeout, check=False,
        )
    except subprocess.TimeoutExpired as exc:
        error = "ssh-timeout"
        stdout = (exc.stdout or "") if isinstance(exc.stdout, str) else ""
        stderr = (exc.stderr or "") if isinstance(exc.stderr, str) else ""
    except OSError as exc:
        error = f"ssh-error:{type(exc).__name__}"
        stdout = ""; stderr = str(exc)
    else:
        stdout, stderr = completed.stdout, completed.stderr
    (cell_dir / "ssh.stdout").write_text(stdout, encoding="utf-8", errors="replace")
    (cell_dir / "ssh.stderr").write_text(stderr, encoding="utf-8", errors="replace")
    fields, logs = _parse_remote(stdout)
    for name, content in logs.items():
        (cell_dir / f"{name}.log").write_text(content, encoding="utf-8")
    status = fields.get("S4_STATUS", "").split(" ", 1)[0] or ("FAIL" if error else "FAIL")
    reason = fields.get("S4_STATUS", "").split("reason=", 1)[1] if "reason=" in fields.get("S4_STATUS", "") else error or "remote-no-status"
    if status not in {"PASS", "HOLD", "FAIL"}:
        status = "FAIL"
    return {
        "schema": SCHEMA, "base_source_sha": BASE_SHA, "p43_source_sha": P43_SHA,
        "p50_source_sha": P50_SOURCE_SHA,
        "cell": cell["id"], "scheduler_version": cell["S"],
        "client_version": cell["C"], "worker_version": cell["F"],
        "cache_expected": expect_cache, "cache_observed": fields.get("S4_CACHE_OBSERVED") == "1",
        "legacy_observed": fields.get("S4_LEGACY_OBSERVED") == "1",
        "remote_compile": fields.get("S4_REMOTE_COMPILE") == "1",
        "byte_identical": fields.get("S4_BYTE_IDENTICAL") == "1",
        "cleanup": fields.get("S4_CLEANUP", "missing"), "status": status,
        "reason": reason, "returncode": completed.returncode if completed else None,
        "command": command, "roots": roots,
        "evidence": str(cell_dir),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="mickg10@tt-quietbox3")
    parser.add_argument("--ssh-option", action="append", default=[
        "-o", "HostName=10.0.27.101", "-o", "HostKeyAlias=tt-quietbox3",
        "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
    ])
    parser.add_argument("--out-root", type=Path, default=Path("/tanksmall/scratch/ictmp/experiments/icecream"))
    parser.add_argument("--p43-root", default=P43_ROOT)
    parser.add_argument("--p50-root", default=P50_ROOT)
    parser.add_argument("--p50-local-root", default="",
                        help="exact BASE_SHA build to stage privately on q3")
    parser.add_argument("--s-root", default="", help="override scheduler root for every cell")
    parser.add_argument("--c-root", default="", help="override client root for every cell")
    parser.add_argument("--f-root", default="", help="override worker root for every cell")
    parser.add_argument("--timeout", type=float, default=240.0)
    parser.add_argument("--cell", action="append", choices=[cell["id"] for cell in _cells()],
                        help="run only this cell (repeatable; useful for a bounded smoke run)")
    args = parser.parse_args(argv)
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")
    run_root = args.out_root / f"s4-real-cells-{stamp}"
    run_root.mkdir(parents=True, exist_ok=False)
    staged_root = None
    if args.p50_local_root:
        try:
            staged_root = _stage_p50(args)
            args.p50_root = staged_root
        except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
            print(f"P50 staging HOLD: {exc}", file=sys.stderr)
    rows = []
    selected = [cell for cell in _cells() if not args.cell or cell["id"] in args.cell]
    for cell in selected:
        rows.append(_run_cell(cell, args, run_root))
    result_path = run_root / "results.jsonl"
    with result_path.open("x", encoding="utf-8") as stream:
        for row in rows:
            stream.write(json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n")
        stream.flush()
        os.fsync(stream.fileno())
    if staged_root:
        cleanup = ["ssh", *args.ssh_option, args.host, "rm", "-rf", "--", staged_root]
        subprocess.run(cleanup, timeout=min(args.timeout, 30), check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    summary = {"run_root": str(run_root), "result": str(result_path),
               "pass": sum(row["status"] == "PASS" for row in rows),
               "hold": sum(row["status"] == "HOLD" for row in rows),
               "fail": sum(row["status"] == "FAIL" for row in rows)}
    print(json.dumps(summary, sort_keys=True))
    return 0 if summary["fail"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
