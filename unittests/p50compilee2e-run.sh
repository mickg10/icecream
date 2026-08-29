#!/bin/sh
# Real all-P50 C1F1 networked profile compile gate.
#
# Once p50compilee2e-source.sh is green this starts the actual built
# scheduler, one actual iceccd F, one actual iceccd C, and the actual
# icecc-cache-service owned by the daemon's sidecar adapter. The compiler
# invocation is required to produce positive selected-profile evidence and a
# byte-identical local reference. No fake peer or legacy FileChunk fallback
# is accepted.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
build=${ICECC_TEST_TOP_BUILDDIR:-$(CDPATH= cd -- "$src" && pwd)}
timeout_s=${ICECC_P50_C1F1_TIMEOUT:-180}
profile_marker=${ICECC_P50_PROFILE:-ZSTD_ROUTE}
suite=${ICECC_P50_SUITE:-C1F1/100000}
warm=${ICECC_P50_C1F1_WARM:-0}
passes=${ICECC_P50_C1F1_PASSES:-2}
topology=${ICECC_P50_TOPOLOGY:-}
case "$suite" in
    C1F1/100000) relationship_count=1; slots_per_f=1; execution_slots=1 ;;
    C1F20/40) relationship_count=20; slots_per_f=2; execution_slots=40 ;;
    *) echo "FAIL: ICECC_P50_SUITE must be C1F1/100000 or C1F20/40" >&2; exit 1 ;;
esac
if test "$suite" = C1F20/40 && test -z "$topology"; then
    echo "FAIL: C1F20/40 requires an authenticated topology" >&2
    exit 1
fi
grz_product_configured() {
    # GRZ is a product capability, not merely a selector spelling.  Require
    # both configure's feature definition and the generated cache product
    # makefile's libbsc compile/link inputs before starting the live gate.
    test -f "$build/config.h" || return 1
    grep -F '#define ICECC_P50_WITH_LIBBSC 1' "$build/config.h" >/dev/null \
        || return 1
    test -f "$build/cache/Makefile" || return 1
    grep -E '^LIBBSC_CFLAGS = .*ICECC_P50_WITH_LIBBSC' \
        "$build/cache/Makefile" >/dev/null || return 1
    grep -E '^LIBBSC_LIBS = .*(libbsc\.a|-lbsc)' \
        "$build/cache/Makefile" >/dev/null || return 1
}
case "$profile_marker" in
    P29) profile_advertisement=p29 ;;
    ZSTD_TU) profile_advertisement=zstd_tu ;;
    ZSTD_ROUTE) profile_advertisement=z3_long ;;
    GRZ|GRZ_RESIDUAL)
        grz_product_configured || {
            echo "FAIL: ICECC_P50_PROFILE=$profile_marker requires a product build configured with libbsc" >&2
            exit 1
        }
        # The client emits the canonical production profile marker for both
        # accepted environment aliases; the scheduler advertises its wire
        # capability spelling separately.
        profile_marker=GRZ_RESIDUAL
        profile_advertisement=grz
        ;;
    *)
        echo "FAIL: ICECC_P50_PROFILE must be P29, ZSTD_TU, ZSTD_ROUTE, GRZ, or GRZ_RESIDUAL" >&2
        exit 1
        ;;
esac
case "$warm" in
    0|1) ;;
    *)
        echo "FAIL: ICECC_P50_C1F1_WARM must be 0 or 1" >&2
        exit 1
        ;;
esac
case "$passes" in
    1|2) ;;
    *)
        echo "FAIL: ICECC_P50_C1F1_PASSES must be 1 or 2" >&2
        exit 1
        ;;
esac

set +e
"$src/unittests/p50compilee2e-source.sh"
rc=$?
set -e
test "$rc" -eq 0 || exit "$rc"

for binary in \
    "$build/daemon/iceccd" \
    "$build/scheduler/icecc-scheduler" \
    "$build/client/icecc" \
    "$build/cache/icecc-cache-service"; do
    test -x "$binary" || {
        echo "SKIP: missing built executable $binary" >&2
        exit 77
    }
done

command -v timeout >/dev/null 2>&1 || {
    echo "SKIP: timeout(1) is required for bounded C1F1 cleanup" >&2
    exit 77
}
command -v g++ >/dev/null 2>&1 || {
    echo "SKIP: g++ is required for the C1F1 compile" >&2
    exit 77
}
command -v bash >/dev/null 2>&1 || {
    echo "SKIP: bash is required for the generated icecc-create-env tool" >&2
    exit 77
}

if test -n "${ICECC_P50_C1F1_WORKDIR:-}"; then
    work=$ICECC_P50_C1F1_WORKDIR
    case "$work" in
        /*/p50compilee2e.*) ;;
        *) echo "FAIL: supplied workdir must be an absolute private p50compilee2e path" >&2; exit 1 ;;
    esac
    test ! -e "$work" || { echo "FAIL: supplied workdir already exists" >&2; exit 1; }
    mkdir -p "$work"
else
    work=$(CDPATH= cd -- "$(mktemp -d "${TMPDIR:-/tmp}/p50compilee2e.XXXXXX")" && pwd)
fi
# The scheduler changes to its configured service account before opening the
# requested log.  Keep the test root traversable and pre-create only that log
# as writable; the cache runtime and HOME below retain their own 0700 modes.
chmod 0711 "$work"
: >"$work/scheduler.log"
chmod 0666 "$work/scheduler.log"
cleanup() {
    for pid in "${service_pid:-}" "${client_service_pid:-}" "${client_pid:-}" ${worker_pids:-} ${service_pids:-} "${worker_pid:-}" "${sched_pid:-}"; do
        test -n "$pid" && kill "$pid" 2>/dev/null || :
    done
    for _ in $(seq 1 50); do
        live=0
        for pid in "${service_pid:-}" "${client_service_pid:-}" "${client_pid:-}" ${worker_pids:-} ${service_pids:-} "${worker_pid:-}" "${sched_pid:-}"; do
            if test -n "$pid" && kill -0 "$pid" 2>/dev/null; then
                live=1
            fi
        done
        test "$live" -eq 0 && break
        sleep 0.1
    done
    for pid in "${service_pid:-}" "${client_service_pid:-}" "${client_pid:-}" ${worker_pids:-} ${service_pids:-} "${worker_pid:-}" "${sched_pid:-}"; do
        test -n "$pid" && kill -9 "$pid" 2>/dev/null || :
    done
    for pid in "${client_pid:-}" "${worker_pid:-}" "${sched_pid:-}"; do
        if test -n "$pid" && ! kill -0 "$pid" 2>/dev/null; then
            wait "$pid" 2>/dev/null || :
        fi
    done
    if test "${ICECC_P50_C1F1_KEEP_WORK:-0}" = 1; then
        echo "INFO: preserving P50 C1F1 workdir $work" >&2
    else
        rm -rf "$work"
    fi
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$work/envs-f" "$work/envs-c" "$work/toolchain" "$work/src" "$work/out" \
    "$work/cache-runtime-f" "$work/cache-runtime-c" "$work/home"
if test "$suite" = C1F20/40; then
    for relationship in $(seq 0 19); do
        mkdir -p "$work/cache-runtime-f-$relationship"
    done
fi
chmod 1777 "$work/envs-f" "$work/envs-c"
chmod 0700 "$work/cache-runtime-f" "$work/cache-runtime-c" "$work/home"
if test "$suite" = C1F20/40; then
    for relationship in $(seq 0 19); do
        chmod 0700 "$work/cache-runtime-f-$relationship"
    done
fi
HOME="$work/home"
export HOME
# Action sinks are role-labelled absolute files.  The production daemons
# inherit these existing sinks; the warm runner later attributes TU0/TU1 by
# their captured session/transaction boundaries.
c_action_trace="$work/s7-warm-c-action-trace.jsonl"
f_action_trace="$work/s7-warm-f-action-trace.jsonl"
pick_port_pair() {
    python3 - "$suite" <<'PY'
import secrets
import socket
import sys

suite = sys.argv[1]

start = 40000 + 2 * secrets.randbelow(9000)
for offset in range(0, 10000, 2):
    base = start + offset
    if base + 1 >= 60000:
        base -= 18000
    sockets = []
    try:
        # The scheduler owns TCP scheduler_port + 1 for its text endpoint.
        # Reserve every relationship's port before any daemon starts so the
        # twenty persistent F identities cannot partially overlap another run.
        ports = (base, base + 1)
        if suite == "C1F20/40":
            ports += tuple(base + 2 * (relationship + 1) for relationship in range(20))
        else:
            ports += (base + 2,)
        for port in ports:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.bind(('0.0.0.0', port))
            sockets.append(sock)
    except OSError:
        for sock in sockets:
            sock.close()
        continue
    for sock in sockets:
        sock.close()
    print(base, base + 2)
    break
else:
    raise SystemExit('no available scheduler/worker port pair')
PY
}
if test -n "${ICECC_P50_C1F1_SCHED_PORT:-}" || test -n "${ICECC_P50_C1F1_WORKER_PORT:-}"; then
    port_sched=${ICECC_P50_C1F1_SCHED_PORT:-}
    port_worker=${ICECC_P50_C1F1_WORKER_PORT:-}
else
    ports=$(pick_port_pair)
    port_sched=${ports%% *}
    port_worker=${ports##* }
fi
test -n "$port_sched" && test -n "$port_worker" || {
    echo "FAIL: scheduler and worker ports must be specified together" >&2
    exit 1
}
test "$port_sched" != "$port_worker" || {
    echo "FAIL: scheduler and worker ports must be distinct" >&2
    exit 1
}
test "$port_worker" -ne "$((port_sched + 1))" || {
    echo "FAIL: worker port collides with scheduler text endpoint" >&2
    exit 1
}
network="p50c1f1-$$"

# A corpus cell may provide an exact input from an authenticated source root.
# Keep root and relative path separate so the input cannot accidentally be
# resolved against the build tree or replaced by the generated smoke TU.
source_root=${ICECC_P50_C1F1_SOURCE_ROOT:-}
source_relative=${ICECC_P50_C1F1_SOURCE_RELATIVE:-}
source_input=${ICECC_P50_C1F1_INPUT:-}
include_root=${ICECC_P50_C1F1_INCLUDE_ROOT:-}
compile_db=${ICECC_P50_C1F1_COMPILE_DB:-}
compile_source=${ICECC_P50_C1F1_COMPILE_SOURCE:-}
batch_manifest=${ICECC_P50_C1F1_BATCH_MANIFEST:-}
if test -n "$batch_manifest"; then
    test -z "$source_root" && test -z "$source_relative" && test -z "$source_input" || {
        echo "FAIL: batch manifest cannot be combined with single-input fields" >&2
        exit 1
    }
    test -f "$batch_manifest" && test ! -L "$batch_manifest" || {
        echo "FAIL: authenticated batch manifest is unavailable" >&2
        exit 1
    }
elif test -n "$source_root" || test -n "$source_relative"; then
    test -n "$source_root" && test -n "$source_relative" || {
        echo "FAIL: source root and source relative path must be supplied together" >&2
        exit 1
    }
    test "${source_root#/}" != "$source_root" || {
        echo "FAIL: source root must be an absolute path" >&2
        exit 1
    }
    case "$source_relative" in
        ''|/*|../*|*/../*|*/..)
            echo "FAIL: source relative path escapes its authenticated root" >&2
            exit 1
            ;;
    esac
    test -d "$source_root" && test ! -L "$source_root" || {
        echo "FAIL: authenticated source root is unavailable" >&2
        exit 1
    }
    source_input="$source_root/$source_relative"
fi
if test -z "$batch_manifest" && test -n "$source_input"; then
    test -f "$source_input" && test ! -L "$source_input" || {
        echo "FAIL: authenticated source input is unavailable" >&2
        exit 1
    }
    cp -- "$source_input" "$work/src/main.cpp"
elif test -z "$batch_manifest"; then
    printf '%s\n' \
        '#include <cstdint>' \
        'int p50_c1f1_translation_unit() {' \
        '    return static_cast<int>(UINT32_C(50));' \
        '}' >"$work/src/main.cpp"
fi
if test -n "$batch_manifest"; then
    # Validate every source snapshot before starting a daemon.  The TSV is
    # only a transport for authenticated paths; all observations below are
    # still emitted by the product lifecycle after each real compile.
    python3 - "$batch_manifest" >"$work/batch.tsv" <<'PY'
import hashlib, json, os, stat, sys
path = sys.argv[1]
with open(path, encoding="utf-8") as stream:
    rows = [json.loads(line) for line in stream if line.strip()]
if not rows:
    raise SystemExit("empty batch manifest")
seen = set()
for row in rows:
    if not isinstance(row, dict) or set(row) - {"tu_id", "source", "source_relative", "sha256", "predictive_input", "compile_db", "compile_db_sha256", "compile_source", "compile_output"}:
        raise SystemExit("batch manifest fields invalid")
    tu, source, expected = row.get("tu_id"), row.get("source"), row.get("sha256")
    source_relative = row.get("source_relative")
    predictive = row.get("predictive_input")
    if (not isinstance(predictive, dict) or
            set(predictive) != {"ordinal", "path", "source_relative", "sha256", "bytes"} or
            predictive.get("ordinal") != len(seen) or
            not isinstance(predictive.get("path"), str) or not os.path.isabs(predictive["path"]) or
            not isinstance(predictive.get("source_relative"), str) or not predictive["source_relative"] or
            predictive["source_relative"].startswith("/") or
            any(part in ("", ".", "..") for part in predictive["source_relative"].split("/")) or
            not isinstance(predictive.get("sha256"), str) or len(predictive["sha256"]) != 64 or
            any(char not in "0123456789abcdef" for char in predictive["sha256"]) or
            type(predictive.get("bytes")) is not int or predictive["bytes"] <= 0):
        raise SystemExit("batch predictive payload descriptor invalid")
    payload_sha, payload_bytes = predictive["sha256"], predictive["bytes"]
    predictive_info = os.lstat(predictive["path"])
    if (stat.S_ISLNK(predictive_info.st_mode) or not stat.S_ISREG(predictive_info.st_mode) or
            predictive_info.st_nlink != 1 or hashlib.sha256(open(predictive["path"], "rb").read()).hexdigest() != payload_sha or
            predictive_info.st_size != payload_bytes):
        raise SystemExit("batch predictive payload unavailable or changed")
    if (not isinstance(tu, str) or not tu or tu in seen or "\t" in tu or
            any(char not in "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-" for char in tu)):
        raise SystemExit("batch TU identity invalid")
    if (not isinstance(source_relative, str) or not source_relative or source_relative.startswith("/") or
            any(part in ("", ".", "..") for part in source_relative.split("/"))):
        raise SystemExit("batch source-relative identity invalid")
    if not isinstance(source, str) or not os.path.isabs(source) or "\t" in source:
        raise SystemExit("batch source must be absolute")
    try:
        info = os.lstat(source)
    except OSError:
        raise SystemExit("batch source unavailable")
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
        raise SystemExit("batch source is not a private regular file")
    actual = hashlib.sha256(open(source, "rb").read()).hexdigest()
    if not isinstance(expected, str) or expected != actual:
        raise SystemExit("batch source digest mismatch")
    db = row.get("compile_db", "")
    db_sha = row.get("compile_db_sha256", "")
    compile_source = row.get("compile_source", source)
    compile_output = row.get("compile_output", "")
    compile_values = (db, db_sha, row.get("compile_source", ""), compile_output)
    if any(compile_values) and not all(compile_values):
        raise SystemExit("batch compile binding incomplete")
    if db or compile_source != source:
        if not isinstance(db, str) or not os.path.isabs(db) or not os.path.isfile(db) or os.path.islink(db):
            raise SystemExit("batch compile database unavailable")
        if (not isinstance(db_sha, str) or len(db_sha) != 64 or
                any(char not in "0123456789abcdef" for char in db_sha) or
                hashlib.sha256(open(db, "rb").read()).hexdigest() != db_sha):
            raise SystemExit("batch compile database digest mismatch")
        if (not isinstance(compile_source, str) or not os.path.isabs(compile_source) or
                compile_source != source or not isinstance(compile_output, str) or
                not os.path.isabs(compile_output)):
            raise SystemExit("batch compile source/output binding invalid")
    print("\t".join((tu, source, source_relative, actual, predictive["path"], predictive["source_relative"], payload_sha, str(payload_bytes), db, db_sha, compile_source, compile_output)))
    seen.add(tu)
PY
    batch_expected_count=${ICECC_P50_C1F1_EXPECTED_COUNT:-}
    test "$batch_expected_count" -gt 0 2>/dev/null || {
        echo "FAIL: batch expected count is required" >&2
        exit 1
    }
    batch_actual_count=$(wc -l <"$work/batch.tsv")
    test "$batch_actual_count" -eq "$batch_expected_count" || {
        echo "FAIL: batch manifest count does not match selected depth" >&2
        exit 1
    }
fi
if test "$suite" = C1F20/40; then
    test -n "$batch_manifest" || {
        echo "FAIL: C1F20/40 requires an authenticated batch manifest" >&2
        exit 1
    }
    test -f "$topology" && test ! -L "$topology" || {
        echo "FAIL: authenticated C1F20/40 topology is unavailable" >&2
        exit 1
    }
    python3 - "$topology" "$work/batch.tsv" "$batch_expected_count" >"$work/topology.tsv" <<'PY'
import json, sys
topology_path, batch_path, expected_count = sys.argv[1], sys.argv[2], int(sys.argv[3])
value = json.load(open(topology_path, encoding="utf-8"))
if (not isinstance(value, dict) or value.get("schema") != "icecream-s8-topology-assignment-v1"
        or value.get("suite") != "C1F20/40"):
    raise SystemExit("topology schema or suite invalid")
assignments = value.get("assignments", value.get("inputs"))
batch = [line.rstrip("\n").split("\t") for line in open(batch_path, encoding="utf-8")]
if not isinstance(assignments, list) or len(assignments) != expected_count:
    raise SystemExit("topology assignment count mismatch")
if len(batch) != expected_count:
    raise SystemExit("batch/topology count mismatch")
seen = set(); relations = set(); last = {}
slots = set()
for ordinal, (assignment, fields) in enumerate(zip(assignments, batch)):
    if (not isinstance(assignment, dict) or assignment.get("ordinal") != ordinal
            or assignment.get("tu_id") != fields[0]):
        raise SystemExit("topology assignment identity mismatch")
    relationship, slot = assignment.get("relationship"), assignment.get("f_slot")
    if (type(relationship) is not int or not 0 <= relationship < 20
            or type(slot) is not int or not 0 <= slot < 2):
        raise SystemExit("topology relationship or slot invalid")
    key = (relationship, slot)
    if assignment["tu_id"] in seen:
        raise SystemExit("topology duplicate TU")
    # A queue may interleave with other relationships but cannot revisit a
    # relationship's source sequence out of order.
    if relationship in last and ordinal <= last[relationship]:
        raise SystemExit("topology relationship order invalid")
    seen.add(assignment["tu_id"]); relations.add(relationship); last[relationship] = ordinal
    slots.add(key)
    print("\t".join((str(relationship), str(slot))))
if relations != set(range(20)):
    raise SystemExit("topology does not bind all twenty F relationships")
if slots != {(relationship, slot) for relationship in range(20) for slot in range(2)}:
    raise SystemExit("topology does not bind both slots of every F relationship")
PY
fi
if test -n "$batch_manifest" || test -n "$compile_db" || test -n "$compile_source"; then
    if test -z "$batch_manifest"; then
        test -n "$compile_db" && test -n "$compile_source" || {
            echo "FAIL: compile database and source must be supplied together" >&2; exit 1;
        }
        test -f "$compile_db" && test ! -L "$compile_db" || {
            echo "FAIL: authoritative compile database is unavailable" >&2; exit 1;
        }
        test "${compile_source#/}" != "$compile_source" || {
            echo "FAIL: compile database source must be absolute" >&2; exit 1;
        }
    fi
    compile_args_for() {
        db=$1; source=$2; expected_db_output=$3; staged=$4; output=$5
        python3 - "$db" "$source" "$expected_db_output" "$staged" "$output" <<'PY'
import json, os, shlex, sys
db, source, expected_db_output, staged, output = sys.argv[1:]
entries = json.load(open(db, encoding='utf-8'))
def resolved_output(entry):
    value = entry.get('output')
    directory = entry.get('directory')
    if not isinstance(value, str) or not isinstance(directory, str):
        return None
    return os.path.realpath(value if os.path.isabs(value) else os.path.join(directory, value))
matches = [e for e in entries if isinstance(e, dict) and
           os.path.realpath(e.get('file', '')) == os.path.realpath(source) and
           (not expected_db_output or
            resolved_output(e) == os.path.realpath(expected_db_output))]
if len(matches) != 1 or not isinstance(matches[0].get('command'), str): raise SystemExit(1)
tokens = shlex.split(matches[0]['command'])
if len(tokens) < 2 or source not in tokens: raise SystemExit(1)
tokens = tokens[1:]
tokens[tokens.index(source)] = staged
for i, token in enumerate(tokens[:-1]):
    if token == '-o': tokens[i + 1] = output; break
else: raise SystemExit(1)
# Icecream's GCC remote arm adds -fdirectives-only and allocator parameters.
# Keep the compile database's debug output, but do not encode those
# compiler-owned switches in DW_AT_producer: otherwise semantically identical
# local and remote objects differ only in their debug string table.
if (any(token == '-g' or token.startswith('-g') for token in tokens)
        and not any(token in ('-grecord-gcc-switches', '-gno-record-gcc-switches')
                    for token in tokens)):
    tokens.append('-gno-record-gcc-switches')
print(shlex.join(tokens))
PY
    }
fi
if test -n "$include_root"; then
    test "${include_root#/}" != "$include_root" || {
        echo "FAIL: include root must be an absolute path" >&2
        exit 1
    }
    test -d "$include_root" && test ! -L "$include_root" || {
        echo "FAIL: authenticated include root is unavailable" >&2
        exit 1
    }
fi

# The environment is made by the real icecc tool, then shipped to the real F
# daemon. This is deliberately not replaced by the host compiler PATH.
(cd "$work/toolchain" && timeout "$timeout_s" \
    bash "$build/client/icecc-create-env" "$(command -v g++)" \
    >"$work/create-env.log" 2>&1)
envtar=$(find "$work/toolchain" -maxdepth 1 -type f -name '*.tar.gz' -print -quit)
test -n "$envtar" || {
    echo "FAIL: real icecc-create-env produced no compiler environment" >&2
    exit 1
}

# A root container still runs the production daemon's normal privilege drop.
# Let an explicit test account own the private tree so the daemon can create
# its log and sidecar runtime files after that drop.  No account is selected
# by default, preserving the ordinary host invocation.
set --
if test -n "${ICECC_TEST_DAEMON_UID:-}"; then
    daemon_gid=${ICECC_TEST_DAEMON_GID:-$ICECC_TEST_DAEMON_UID}
    chown -R "$ICECC_TEST_DAEMON_UID:$daemon_gid" "$work"
    set -- -u "$ICECC_TEST_DAEMON_UID"
fi
if test -n "${ICECC_P50_C1F1_WORKER_INTERFACE:-}"; then
    set -- "$@" -i "$ICECC_P50_C1F1_WORKER_INTERFACE"
fi

run_client_with_timeout() {
    if test -n "${ICECC_TEST_DAEMON_UID:-}"; then
        timeout "$timeout_s" runuser -u "$ICECC_TEST_DAEMON_UID" -- \
            "$build/client/icecc" "$@"
    else
        timeout "$timeout_s" "$build/client/icecc" "$@"
    fi
}

"$build/scheduler/icecc-scheduler" -p "$port_sched" -n "$network" \
    --assignment-fence-mode strict-nonce -l "$work/scheduler.log" -vvv &
sched_pid=$!
sleep 1
kill -0 "$sched_pid" 2>/dev/null || {
    echo "FAIL: real scheduler exited during startup" >&2
    exit 1
}

# The parallel cell has one persistent F identity per relationship.  Each F
# owns two real compiler slots; the client selects the authenticated F by
# relationship for every source-stage request.  No fake peer is introduced.
worker_pids=""
service_pids=""
if test "$suite" = C1F20/40; then
    for relationship in $(seq 0 19); do
        worker_port=$((port_worker + relationship * 2))
        test "$worker_port" -lt 60000 || { echo "FAIL: worker port range exhausted" >&2; exit 1; }
        f_trace="$work/s7-warm-f-action-trace-$relationship.jsonl"
        ICECC_TEST_SOCKET="$work/worker-$relationship.sock" ICECC_P50_C1F1_REQUIRED=1 \
            ICECC_P50_C_ACTION_TRACE="$f_trace" ICECC_P50_F_ACTION_TRACE="$f_trace" \
            ICECC_P50_RELATIONSHIP="$relationship" \
            "$build/daemon/iceccd" "$@" -p "$worker_port" -m 2 \
            -s "127.0.0.1:$port_sched" -n "$network" -N "p50-f-$relationship" \
            -b "$work/envs-f" -l "$work/f-$relationship.log" -vvv \
            --cache-service "$build/cache/icecc-cache-service" \
            --cache-runtime-dir "$work/cache-runtime-f-$relationship" &
        worker_pid=$!
        worker_pids="$worker_pids $worker_pid"
    done
else
    ICECC_TEST_SOCKET="$work/worker.sock" ICECC_P50_C1F1_REQUIRED=1 \
        ICECC_P50_C_ACTION_TRACE="$f_action_trace" ICECC_P50_F_ACTION_TRACE="$f_action_trace" \
        "$build/daemon/iceccd" "$@" -p "$port_worker" -m 1 \
        -s "127.0.0.1:$port_sched" -n "$network" -N p50-f \
        -b "$work/envs-f" -l "$work/f.log" -vvv \
        --cache-service "$build/cache/icecc-cache-service" \
        --cache-runtime-dir "$work/cache-runtime-f" &
    worker_pid=$!
    worker_pids="$worker_pid"
fi

ICECC_TEST_SOCKET="$work/client.sock" ICECC_P50_C1F1_REQUIRED=1 \
    ICECC_P50_C_ACTION_TRACE="$c_action_trace" ICECC_P50_F_ACTION_TRACE="$c_action_trace" \
    "$build/daemon/iceccd" "$@" --no-remote -m 0 \
    -s "127.0.0.1:$port_sched" -n "$network" -N p50-c \
    -b "$work/envs-c" -l "$work/c.log" -vvv \
    --cache-service "$build/cache/icecc-cache-service" \
    --cache-runtime-dir "$work/cache-runtime-c" &
client_pid=$!

logins=0
for _ in $(seq 1 30); do
    logins=$(grep -c login "$work/scheduler.log" 2>/dev/null || true)
    if test "$suite" = C1F20/40; then
        test "${logins:-0}" -ge 21 && break
    else
        test "${logins:-0}" -ge 2 && break
    fi
    sleep 1
done
required_logins=2
test "$suite" = C1F20/40 && required_logins=21
test "${logins:-0}" -ge "$required_logins" || {
    echo "FAIL: real C1F1 daemons did not register" >&2
    exit 1
}

# The cache executable must be alive as a child of the production daemon
# wiring. Merely checking that the file exists would permit a mechanism-only
# test to masquerade as an end-to-end compile.
service_pid=
if test "$suite" = C1F20/40; then
    for relationship in $(seq 0 19); do
        found=
        for _ in $(seq 1 30); do
            found=$(ps -eo pid=,ppid=,args= | \
                awk -v parent="$(printf '%s' "$worker_pids" | awk -v n="$relationship" '{print $(n + 1)}')" \
                    -v exe="$build/cache/icecc-cache-service" \
                '$2 == parent && index($0, exe) > 0 { print $1; exit }')
            test -n "$found" && break
            sleep 1
        done
        test -n "$found" || { echo "FAIL: F relationship $relationship cache service missing" >&2; exit 1; }
        service_pids="$service_pids $found"
    done
    service_pid=$(printf '%s\n' "$service_pids" | awk '{print $1}')
else
    for _ in $(seq 1 30); do
        service_pid=$(ps -eo pid=,ppid=,args= | \
            awk -v parent="$worker_pid" -v exe="$build/cache/icecc-cache-service" \
            '$2 == parent && index($0, exe) > 0 { print $1; exit }')
        test -n "$service_pid" && break
        sleep 1
    done
    test -n "$service_pid" || {
        echo "FAIL: production daemon did not start the actual icecc-cache-service" >&2
        exit 1
    }
fi

client_service_pid=
for _ in $(seq 1 30); do
    client_service_pid=$(ps -eo pid=,ppid=,args= | \
        awk -v parent="$client_pid" -v exe="$build/cache/icecc-cache-service" \
        '$2 == parent && index($0, exe) > 0 { print $1; exit }')
    test -n "$client_service_pid" && break
    sleep 1
done
test -n "$client_service_pid" || {
    echo "FAIL: C daemon did not start its authenticated local cache sidecar" >&2
    exit 1
}

# A live child is not yet a usable cache endpoint.  Submit only after the
# daemon has authenticated READY and the scheduler has consumed F's real
# cache-bearing relogin; otherwise the assignment is correctly frozen without
# a handoff and a millisecond startup race masquerades as a product failure.
cache_ready=0
for _ in $(seq 1 30); do
    if test "$suite" = C1F20/40; then
        # A scheduler may relogin the same F more than once.  Count extracted
        # authenticated service identities, not timestamped log lines.
        ready_count=$(grep -E "RELOGIN p50-f-[0-9]+.*cache=.*cache_profiles=.*$profile_advertisement" \
            "$work/scheduler.log" 2>/dev/null |
            grep -oE 'p50-f-[0-9]+' | sort -u | wc -l)
        test "$ready_count" -ge 20 && cache_ready=1 && break
    elif grep -E "RELOGIN p50-f.*cache=.*cache_profiles=.*$profile_advertisement" \
            "$work/scheduler.log" >/dev/null 2>&1; then
        cache_ready=1; break
    fi
    sleep 1
done
test "$cache_ready" -eq 1 || {
    echo "FAIL: production F cache endpoint was not advertised READY" >&2
    exit 1
}

compile_once() {
    label=$1
    input_path=${2:-$work/src/main.cpp}
    item_compile_db=${3:-$compile_db}
    item_compile_source=${4:-$compile_source}
    item_compile_output=${5:-}
    relationship=${6:-0}
    f_slot=${7:-0}
    preferred_host=p50-f
    if test "$suite" = C1F20/40; then
        preferred_host="p50-f-$relationship"
    fi
    remote_obj="$work/out/remote-$label.o"
    local_obj="$work/out/local-$label.o"
    client_log="$work/client-compile-$label.log"
    compile_include_args=""
    if test -n "$item_compile_db"; then
        # Depth batches stage the authenticated predictive .ii.  Remote and
        # local compilation must bind to that exact staged input; the source
        # path is used only to select the unique compile-database command.
        remote_compile_args=$(compile_args_for "$item_compile_db" "$item_compile_source" "$item_compile_output" "$input_path" "$remote_obj")
        local_compile_args=$(compile_args_for "$item_compile_db" "$item_compile_source" "$item_compile_output" "$input_path" "$local_obj")
    elif test -n "$include_root"; then
        compile_include_args="-I$include_root"
    else
        compile_include_args=""
    fi
    preprocessed_capture="$work/s7-$label-preprocessed.ii"
    compile_start_ns=$(date +%s%N)
    if test -n "$item_compile_db"; then
        # eval is needed to turn the safely shlex-quoted database tokens back
        # into argv. Export first: assignments before the special builtin
        # `eval` become shell variables, not necessarily the environment seen
        # by the external client called by run_client_with_timeout.
        ICECC_TEST_SOCKET="$work/client.sock"
        ICECC_TEST_REMOTEBUILD=1
        ICECC_VERSION="$envtar"
        ICECC_P50_C1F1_REQUIRED=1
        ICECC_P50_PREPROCESSED_CAPTURE="$preprocessed_capture"
        ICECC_PREFERRED_HOST="$preferred_host"
        ICECC_DEBUG=debug
        ICECC_LOGFILE="$client_log"
        export ICECC_TEST_SOCKET ICECC_TEST_REMOTEBUILD ICECC_VERSION \
            ICECC_P50_C1F1_REQUIRED ICECC_P50_PREPROCESSED_CAPTURE \
            ICECC_PREFERRED_HOST ICECC_DEBUG ICECC_LOGFILE
        eval "run_client_with_timeout g++ $remote_compile_args"
    else
        ICECC_TEST_SOCKET="$work/client.sock" ICECC_TEST_REMOTEBUILD=1 \
            ICECC_VERSION="$envtar" ICECC_P50_C1F1_REQUIRED=1 \
            ICECC_P50_PREPROCESSED_CAPTURE="$preprocessed_capture" \
            ICECC_PREFERRED_HOST="$preferred_host" ICECC_DEBUG=debug ICECC_LOGFILE="$client_log" \
            run_client_with_timeout g++ -std=c++17 -O2 -c \
            $compile_include_args "$input_path" -o "$remote_obj"
    fi
    compile_end_ns=$(date +%s%N)
    if test -n "$item_compile_db"; then
        eval "g++ $local_compile_args"
    else
        g++ -std=c++17 -O2 -c $compile_include_args \
            "$input_path" -o "$local_obj"
    fi
    test -s "$preprocessed_capture" || {
        echo "FAIL: completed $label preprocessor capture is missing" >&2
        exit 1
    }
    cmp -s "$remote_obj" "$local_obj" || {
        echo "FAIL: real P50 object differs from local reference ($label)" >&2
        exit 1
    }
}

# A batch always reuses this one scheduler/C/F/cache lifecycle.  Warm mode
# first compiles the exact selected ordered manifest as prewarm work; those
# rows remain outside measured evidence while the trace offsets below bind the
# measured pass(es) to the same service state.
prewarm_c_trace="$work/s7-prewarm-c-action-trace.jsonl"
prewarm_f_trace="$work/s7-prewarm-f-action-trace.jsonl"
measured_c_trace="$work/s7-measured-c-action-trace.jsonl"
measured_f_trace="$work/s7-measured-f-action-trace.jsonl"
merge_parallel_f_traces() {
    test "$suite" = C1F20/40 || return 0
    python3 - "$work" "$f_action_trace" <<'PY'
import json, pathlib, sys
root, output = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
rows = []
for path in sorted(root.glob("s7-warm-f-action-trace-*.jsonl")):
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.strip():
            rows.append(json.loads(line))
if not rows:
    raise SystemExit("parallel F action traces are empty")
rows.sort(key=lambda row: (int(row.get("tu_seq", -1)), int(row.get("rel_seq", -1)),
                           row.get("f_store_guid", "")))
output.write_text("".join(json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n"
                             for row in rows), encoding="utf-8")
PY
}
if test -n "$batch_manifest"; then
    mkdir -p "$work/active"
    mkdir -p "$work/input-ready"
    run_one() {
        run_label=$1
        ordinal=$2
        relationship=$3
        f_slot=$4
        predictive_path=$5
        source_sha=$6
        item_compile_db=$7
        item_compile_source=$8
        item_compile_output=$9
        payload_sha=${10}
        payload_bytes=${11}
        staged="$work/src/$run_label-$ordinal.ii"
        marker="$work/active/$run_label-$relationship-$f_slot"
        input_ready_marker="$work/input-ready/$run_label-$relationship-$ordinal"
        trap 'rm -f "$marker"' EXIT HUP INT TERM
        cp -- "$predictive_path" "$staged"
        compile_once "$run_label-$ordinal" "$staged" "$item_compile_db" \
            "$item_compile_source" "$item_compile_output" "$relationship" "$f_slot" &
        compile_pid=$!
        preprocessed_capture="$work/s7-$run_label-$ordinal-preprocessed.ii"
        # ICECC_P50_PREPROCESSED_CAPTURE is written immediately before the
        # exact .ii is attached to the live transaction.  This is the
        # relationship's source-admission witness; waiting for it does not
        # wait for compile/result completion.
        while test ! -s "$preprocessed_capture"; do
            if ! kill -0 "$compile_pid" 2>/dev/null; then
                wait "$compile_pid" || true
                echo "FAIL: compile ended before authenticated input-ready ($run_label-$ordinal)" >&2
                return 1
            fi
            sleep 0.005
        done
        : >"$input_ready_marker"
        wait "$compile_pid"
        remote_obj="$work/out/remote-$run_label-$ordinal.o"
        local_obj="$work/out/local-$run_label-$ordinal.o"
        remote_sha=$(sha256sum "$remote_obj" | awk '{print $1}')
        local_sha=$(sha256sum "$local_obj" | awk '{print $1}')
        remote_bytes=$(stat -c %s "$remote_obj")
        local_bytes=$(stat -c %s "$local_obj")
        preprocessed_sha=$(sha256sum "$preprocessed_capture" | awk '{print $1}')
        preprocessed_bytes=$(stat -c %s "$preprocessed_capture")
        test "$preprocessed_sha" = "${10}" && test "$preprocessed_bytes" -eq "${11}" || {
            echo "FAIL: preprocessed payload differs from authenticated predictive descriptor ($run_label-$ordinal)" >&2
            exit 1
        }
        wait_ms=$(sed -nE 's/.*<\/wait for cs: ([0-9]+)ms>.*/\1/p' \
            "$work/client-compile-$run_label-$ordinal.log" | tail -n 1)
        test -n "$wait_ms" || { echo "FAIL: client wait-for-cs timing missing ($run_label-$ordinal)" >&2; exit 1; }
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$ordinal" "$source_sha" "$preprocessed_capture" "$preprocessed_sha" "$preprocessed_bytes" \
            "$remote_obj" "$remote_sha" "$remote_bytes" "$local_obj" "$local_sha" "$local_bytes" \
            "$compile_start_ns" "$compile_end_ns" >"$work/result-$run_label-$ordinal.tsv"
        printf '%s\n' "$((wait_ms * 1000000))" >"$work/wait-$run_label-$ordinal.ns"
    }
    run_batch() {
        run_label=$1
        emit_rows=${2:-1}
        ordinal=0
        topology_input=/dev/null
        if test "$suite" = C1F20/40; then
            topology_input="$work/topology.tsv"
        fi
        batch_start_ns=$(date +%s%N)
        job_pids=""
        while IFS="$(printf '\t')" read -r tu_id source_path source_relative source_sha predictive_path predictive_relative payload_sha payload_bytes item_db item_db_sha item_source item_output; do
            relationship=0; f_slot=0
            if test "$suite" = C1F20/40; then
                IFS="$(printf '\t')" read -r relationship f_slot <&3
                # The topology names the exact slot.  Never substitute the
                # sibling slot when this preregistered slot is still active.
                marker="$work/active/$run_label-$relationship-$f_slot"
                while test -e "$marker"; do sleep 0.005; done
            else
                marker="$work/active/$run_label-0-0"
                while test -e "$marker"; do sleep 0.005; done
            fi
            : >"$marker"
            run_one "$run_label" "$ordinal" "$relationship" "$f_slot" "$predictive_path" "$source_sha" \
                "$item_db" "$item_source" "$item_output" "$payload_sha" "$payload_bytes" \
                >"$work/job-$run_label-$ordinal.log" 2>&1 &
            job_pid=$!
            job_pids="$job_pids $job_pid"
            # Serialize only source admission within a relationship.  The
            # prior job's compiler/result process remains active after this
            # witness, so both real F slots can overlap.
            input_ready_marker="$work/input-ready/$run_label-$relationship-$ordinal"
            while test ! -e "$input_ready_marker"; do
                if ! kill -0 "$job_pid" 2>/dev/null; then
                    wait "$job_pid" || true
                    echo "FAIL: source admission failed ($run_label-$ordinal)" >&2
                    exit 1
                fi
                sleep 0.005
            done
            ordinal=$((ordinal + 1))
        done <"$work/batch.tsv" 3<"${topology_input:-/dev/null}"
        test "$ordinal" -eq "$batch_expected_count" || { echo "FAIL: batch manifest count changed during run" >&2; exit 1; }
        for pid in $job_pids; do
            wait "$pid" || {
                echo "FAIL: real compile job failed ($run_label)" >&2
                cat "$work"/job-"$run_label"-*.log 2>/dev/null || true
                exit 1
            }
        done
        batch_end_ns=$(date +%s%N)
        if test "$emit_rows" = 1; then
            printf 'S8_BATCH_WINDOW run=%s start_ns=%s end_ns=%s\n' \
                "$run_label" "$batch_start_ns" "$batch_end_ns"
        fi
        ordinal=0
        while IFS="$(printf '\t')" read -r tu_id source_path source_relative source_sha predictive_path predictive_relative payload_sha payload_bytes item_db item_db_sha item_source item_output; do
            result=$(cat "$work/result-$run_label-$ordinal.tsv")
            IFS="$(printf '\t')" read -r _ result_source_sha preprocessed_capture preprocessed_sha preprocessed_bytes remote_obj remote_sha remote_bytes local_obj local_sha local_bytes compile_start_ns compile_end_ns <<EOF
$result
EOF
            wait_ns=$(cat "$work/wait-$run_label-$ordinal.ns")
            if test "$emit_rows" = 1; then
                printf 'S8_BATCH_TU run=%s ordinal=%s tu_id=%s source_sha256=%s preprocessed_path=%s preprocessed_sha256=%s preprocessed_bytes=%s remote_path=%s remote_sha256=%s remote_bytes=%s local_path=%s local_sha256=%s local_bytes=%s compile_start_ns=%s compile_end_ns=%s wait_for_cs_ns=%s assignment=%s relationship=%s f_slot=%s service_identity=p50-f-%s\n' \
                    "$run_label" "$ordinal" "$tu_id" "$source_sha" "$preprocessed_capture" "$preprocessed_sha" "$preprocessed_bytes" \
                    "$remote_obj" "$remote_sha" "$remote_bytes" "$local_obj" "$local_sha" "$local_bytes" \
                    "$compile_start_ns" "$compile_end_ns" "$wait_ns" \
                    "$([ "$suite" = C1F20/40 ] && printf '%s' "$ordinal" || printf '0')" "$relationship" "$f_slot" "$relationship"
            fi
            ordinal=$((ordinal + 1))
        done <"$work/batch.tsv" 3<"${topology_input:-/dev/null}"
        echo "S8_BATCH_COMPLETE run=$run_label count=$ordinal"
    }
    if test "$warm" = 1; then
        echo "S7_WARM_PREWARM_BEGIN"
        run_batch prewarm 0
        merge_parallel_f_traces
        test -s "$c_action_trace" && test -s "$f_action_trace" || {
            echo "FAIL: prewarm product action traces are missing" >&2
            exit 1
        }
        cp -- "$c_action_trace" "$prewarm_c_trace"
        cp -- "$f_action_trace" "$prewarm_f_trace"
        prewarm_c_lines=$(wc -l <"$c_action_trace")
        prewarm_f_lines=$(wc -l <"$f_action_trace")
        echo "S7_WARM_PREWARM_COMPLETE"
    fi
    run_batch full-1 1
    if test "$passes" = 2; then
        run_batch full-2 1
    fi
    merge_parallel_f_traces
else
    if test "$warm" = 1; then
        echo "S7_WARM_PREWARM_BEGIN"
        compile_once prewarm
        test -s "$c_action_trace" && test -s "$f_action_trace" || {
            echo "FAIL: prewarm product action traces are missing" >&2
            exit 1
        }
        cp -- "$c_action_trace" "$prewarm_c_trace"
        cp -- "$f_action_trace" "$prewarm_f_trace"
        prewarm_c_lines=$(wc -l <"$c_action_trace")
        prewarm_f_lines=$(wc -l <"$f_action_trace")
        echo "S7_WARM_PREWARM_COMPLETE"
    fi
    compile_once measured
fi

test -s "$c_action_trace" && test -s "$f_action_trace" || {
    echo "FAIL: measured product action traces are missing" >&2
    exit 1
}
if test "$warm" = 1; then
    tail -n "+$((prewarm_c_lines + 1))" "$c_action_trace" >"$measured_c_trace"
    tail -n "+$((prewarm_f_lines + 1))" "$f_action_trace" >"$measured_f_trace"
else
    cp -- "$c_action_trace" "$measured_c_trace"
    cp -- "$f_action_trace" "$measured_f_trace"
fi
test -s "$measured_c_trace" && test -s "$measured_f_trace" || {
    echo "FAIL: measured action-trace slice is empty" >&2
    exit 1
}
echo "S7_PREWARM_INPUT=$work/s7-prewarm-preprocessed.ii"
echo "S7_MEASURED_INPUT=$work/s7-measured-preprocessed.ii"
echo "S7_PREWARM_C_ACTION_TRACE=$prewarm_c_trace"
echo "S7_PREWARM_F_ACTION_TRACE=$prewarm_f_trace"
echo "S7_MEASURED_C_ACTION_TRACE=$measured_c_trace"
echo "S7_MEASURED_F_ACTION_TRACE=$measured_f_trace"
echo "S7_WORKDIR=$work"
if test -n "$batch_manifest"; then
    echo "S8_SUITE=$suite"
    echo "S8_BATCH_COUNT=$batch_expected_count"
    echo "S8_BATCH_PASSES=$passes"
    echo "S8_BATCH_WARM=$warm"
    if test "$suite" = C1F20/40; then
        echo "S8_SCHEDULING mode=parallel execution_slots=40 max_concurrency=40 relationships=20 slots_per_f=2"
    else
        echo "S8_SCHEDULING mode=serial execution_slots=1 max_concurrency=1"
    fi
    for binary_role in scheduler/icecc-scheduler daemon/iceccd client/icecc cache/icecc-cache-service; do
        binary_path="$build/$binary_role"
        test -x "$binary_path" || { echo "FAIL: missing binary $binary_path" >&2; exit 1; }
        printf 'S8_BINARY role=%s sha256=%s bytes=%s path=%s\n' \
            "$binary_role" "$(sha256sum "$binary_path" | awk '{print $1}')" \
            "$(stat -c %s "$binary_path")" "$binary_path"
    done
fi

echo "PASS: all-P50 C1F1 $profile_marker compile is remote and byte-identical"
