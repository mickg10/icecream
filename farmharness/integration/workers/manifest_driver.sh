set -euo pipefail

result_root=$1
corpus_root=$2
oracle_root=$3
client_name=$4
base_tus=$5
corpus_repeat=$6
workload_repeat=$7
jobs=$8
per_job_timeout=$9
layout=${10}
strict_p50=${11}
compiler=${12}
expected_compiler_digest=${13}
recipe_identity=${14}
compiler_arg_count=${15}
shift 15
compiler_args=()
compiler_arg_index=0
while test "$compiler_arg_index" -lt "$compiler_arg_count"
do
    compiler_args+=("$1")
    shift
    compiler_arg_index=$((compiler_arg_index + 1))
done
test "$#" -ge 4 -a "$#" -le 7
turn=$1
fault_kind=$2
fault_client=$3
fault_job=$4
checkpoint_path=${5:-"$result_root/checkpoint.json"}
resume_mode=${6:-0}
expected_checkpoint_sha=${7:-}

read_boundary_release() {
    python3 -c '
import os, stat, sys
root = sys.argv[1]
directory = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
try:
    directory_stat = os.fstat(directory)
    if (not stat.S_ISDIR(directory_stat.st_mode)
            or directory_stat.st_uid != 0
            or stat.S_IMODE(directory_stat.st_mode) != 0o755):
        raise SystemExit("unsafe active-compiler control directory")
    release = os.open("release.tsv", os.O_RDONLY | os.O_NOFOLLOW, dir_fd=directory)
    try:
        release_stat = os.fstat(release)
        if (not stat.S_ISREG(release_stat.st_mode)
                or release_stat.st_uid != 0
                or release_stat.st_nlink != 1
                or stat.S_IMODE(release_stat.st_mode) != 0o644
                or release_stat.st_size > 4096):
            raise SystemExit("unsafe active-compiler release file")
        payload = os.read(release, 4097)
        if len(payload) != release_stat.st_size or len(payload) > 4096:
            raise SystemExit("unstable active-compiler release file")
    finally:
        os.close(release)
finally:
    os.close(directory)
sys.stdout.write(payload.decode("ascii"))
' "$1"
}

test "$resume_mode" = 0 -o "$resume_mode" = 1
if test "$resume_mode" -eq 1
then
    printf '%s' "$expected_checkpoint_sha" | grep -Eq '^[0-9a-f]{64}$'
fi
test -z "$fault_kind" -o "$fault_kind" = corrupt-object
test "$fault_job" -ge 0
if test "$fault_kind" = corrupt-object
then
    test -n "$fault_client"
    test "$fault_job" -ge 1
else
    test -z "$fault_client"
    test "$fault_job" -eq 0
fi

test "$base_tus" -ge 1
test "$corpus_repeat" -ge 1
test "$workload_repeat" -ge 1
test "$jobs" -ge 1
test "$per_job_timeout" -ge 1
test "$layout" = single -o "$layout" = paired
test "$strict_p50" = 0 -o "$strict_p50" = 1
event_serial_through=${ICEFARM_EVENT_SERIAL_THROUGH:-0}
s60_admit_through=${ICEFARM_S60_ADMIT_THROUGH:-0}
disk_fill_worker=${ICEFARM_DISK_FILL_WORKER:-}
disk_fill_trigger=${ICEFARM_DISK_FILL_TRIGGER:-0}
case "$s60_admit_through" in
    ''|*[!0-9]*) echo "invalid S60 admission boundary" >&2; exit 65 ;;
esac
case "$event_serial_through" in
    ''|*[!0-9]*) echo "invalid event serial boundary" >&2; exit 65 ;;
esac
case "$disk_fill_trigger" in
    ''|*[!0-9]*) echo "invalid disk-fill trigger" >&2; exit 65 ;;
esac
if test "$disk_fill_trigger" -gt 0
then
    printf '%s' "$disk_fill_worker" | grep -Eq '^F[1-9][0-9]*$'
else
    test -z "$disk_fill_worker"
fi
test "$compiler_arg_count" -ge 1
test -x "$compiler"
test "$(sha256sum "$compiler" | awk '{print $1}')" = "$expected_compiler_digest"
test -f "$corpus_root/MANIFEST.sha256"
if test "$resume_mode" -eq 0
then
    test ! -e "$result_root"
    mkdir -p "$result_root/jobs" "$result_root/oracle-samples" "$oracle_root"
    chmod 0777 "$result_root/jobs" "$result_root/oracle-samples"
    cp -- "$corpus_root/MANIFEST.sha256" "$result_root/corpus-manifest.sha256"
else
    test -d "$result_root/jobs" -a -f "$checkpoint_path"
    test -f "$result_root/worklist.bin"
fi

# Timeline events close this admission gate before a disruptive scheduler
# restart.  A wrapper publishes its active marker while holding the same lock
# used by the event controller, so once PAUSE is visible no new compiler can
# cross the boundary.  Waiting wrappers remain bounded by the per-job timeout.
gate_root="$result_root/event-gate"
gate_state="$gate_root/state.tsv"
gate_lock="$gate_root/state.lock"
gate_active="$gate_root/active"
if test "$resume_mode" -eq 0
then
    mkdir -p "$gate_active"
    printf 'OPEN\t0\n' >"$gate_state"
    : >"$gate_lock"
    chmod 0666 "$gate_state" "$gate_lock"
    chmod 0777 "$gate_root" "$gate_active"
else
    test -d "$gate_active" -a -f "$gate_state" -a -f "$gate_lock"
fi

environment=$(find /results/env -maxdepth 1 -type f -name '*.tar.gz' -print -quit)
test -n "$environment"
test -r "$environment"

manifest_digest=$(sha256sum "$corpus_root/MANIFEST.sha256" | awk '{print $1}')
compiler_digest=$(sha256sum "$compiler" | awk '{print $1}')
compiler_banner=$("$compiler" --version | sed -n '1p')
oracle_recipe=direct-v1
compiler_target=
case "$compiler_banner" in
    *[Cc]lang*)
        oracle_recipe=icecream-clang-remote-v1
        compiler_target=$("$compiler" -dumpmachine)
        printf '%s' "$compiler_target" | grep -Eq '^[A-Za-z0-9_.+-]+$'
        ;;
esac
oracle_identity=$(printf '%s\n%s\n%s\n%s\n%s\n' \
    "$manifest_digest" "$compiler_digest" "$recipe_identity" \
    "$oracle_recipe" "$compiler_target" | sha256sum | awk '{print $1}')
worklist="$result_root/worklist.bin"
unique="$result_root/unique.tsv"
ordinal=0

append_group() {
    wanted_group=$1
    turn=$2
    occurrence=$3
    found=0
    while IFS= read -r line
    do
        digest=${line%%  *}
        relative=${line#*  }
        test "$line" = "$digest  $relative"
        printf '%s' "$digest" | grep -Eq '^[0-9a-f]{64}$'
        case "$relative" in
            "$wanted_group"/*) ;;
            *) continue ;;
        esac
        source="$corpus_root/$relative"
        resolved=$(readlink -f -- "$source")
        case "$resolved" in
            "$corpus_root"/*) ;;
            *) echo "corpus path escaped root: $relative" >&2; exit 65 ;;
        esac
        test -f "$resolved" -a ! -L "$source"
        found=$((found + 1))
        ordinal=$((ordinal + 1))
        printf '%s\0%s\0%s\0%s\0%s\0' \
            "$ordinal" "$turn" "$occurrence" "$relative" "$digest" >>"$worklist"
        printf '%s\t%s\t%s\n' "$digest" "$relative" "$resolved" >>"$unique"
    done <"$corpus_root/MANIFEST.sha256"
    test "$found" -eq "$base_tus"
}

if test "$resume_mode" -eq 0
then
    : >"$worklist"
    : >"$unique"
    if test "$layout" = single
    then
        test "$turn" = A
        group=files
    else
        case "$turn" in A|B) group=$turn;; *) exit 65;; esac
    fi
    occurrence=0
    repeat_total=$((corpus_repeat * workload_repeat))
    while test "$occurrence" -lt "$repeat_total"
    do
        append_group "$group" "$turn" "$occurrence"
        occurrence=$((occurrence + 1))
    done
    expected_jobs=$((base_tus * corpus_repeat * workload_repeat))
    test "$ordinal" -eq "$expected_jobs"
    sort -u "$unique" -o "$unique"
else
    expected_jobs=$((base_tus * corpus_repeat * workload_repeat))
fi

resume_indices="$result_root/.resume-indices"
if test "$resume_mode" -eq 1
then
python3 - "$checkpoint_path" "$result_root" "$worklist" "$client_name" "$turn" "$expected_jobs" "$resume_indices" "$expected_checkpoint_sha" <<'PY'
import hashlib
import json
import pathlib
import stat
import sys

checkpoint_path, result_root, worklist, client, turn, expected, indices_path, expected_sha = sys.argv[1:]
root = pathlib.Path(result_root)
try:
    document = json.loads(pathlib.Path(checkpoint_path).read_text(encoding="utf-8"))
except (OSError, ValueError, TypeError) as exc:
    raise SystemExit(f"invalid workload checkpoint: {exc}")
if not isinstance(document, dict) or set(document) != {
    "checkpoint_sha256", "client", "completed_rows", "completed_rows_sha256",
    "expected_jobs", "schema", "status", "turn", "worklist_sha256",
}:
    raise SystemExit("invalid workload checkpoint fields")
if document["schema"] != "icefarm-workload-checkpoint-v1" or document["status"] != "QUIESCED":
    raise SystemExit("workload checkpoint is not a quiesced receipt")
if document["client"] != client or document["turn"] != turn or document["expected_jobs"] != int(expected):
    raise SystemExit("workload checkpoint identity mismatch")
if document["checkpoint_sha256"] != expected_sha:
    raise SystemExit("workload checkpoint is not the hub-retained receipt")
if not isinstance(document["completed_rows"], list):
    raise SystemExit("workload checkpoint rows are not a list")
worklist_digest = hashlib.sha256(pathlib.Path(worklist).read_bytes()).hexdigest()
if document["worklist_sha256"] != worklist_digest:
    raise SystemExit("workload checkpoint worklist digest mismatch")
rows = document["completed_rows"]
canonical = json.dumps(rows, sort_keys=True, separators=(",", ":")).encode()
if document["completed_rows_sha256"] != hashlib.sha256(canonical).hexdigest():
    raise SystemExit("workload checkpoint row digest mismatch")
body = dict(document)
body.pop("checkpoint_sha256")
body_bytes = json.dumps(body, sort_keys=True, separators=(",", ":")).encode()
if document["checkpoint_sha256"] != hashlib.sha256(body_bytes).hexdigest():
    raise SystemExit("workload checkpoint receipt digest mismatch")
seen = set()
indices = []
for row in rows:
    if not isinstance(row, dict) or set(row) != {"index", "path", "sha256"}:
        raise SystemExit("malformed workload checkpoint row")
    index = row["index"]
    if type(index) is not int or not 1 <= index <= int(expected) or index in seen:
        raise SystemExit("duplicate or out-of-range checkpoint row")
    if not isinstance(row["path"], str) or row["path"] != f"jobs/{index:06d}/result.tsv" or not isinstance(row["sha256"], str):
        raise SystemExit("malformed workload checkpoint identity")
    path = root / row["path"]
    try:
        path.relative_to(root)
        stat_info = path.lstat()
        jobs_dir = (root / "jobs").lstat()
        job_dir = (root / "jobs" / f"{index:06d}").lstat()
    except (OSError, ValueError):
        raise SystemExit("checkpoint row escaped workload root")
    if (
        path.is_symlink()
        or not stat.S_ISREG(stat_info.st_mode)
        or not stat.S_ISDIR(jobs_dir.st_mode)
        or not stat.S_ISDIR(job_dir.st_mode)
        or (root / "jobs").is_symlink()
        or (root / "jobs" / f"{index:06d}").is_symlink()
    ):
        raise SystemExit("checkpoint row is not a regular file")
    if hashlib.sha256(path.read_bytes()).hexdigest() != row["sha256"]:
        raise SystemExit("checkpoint row digest mismatch")
    fields = path.read_text(encoding="utf-8").splitlines()
    if len(fields) != 1 or len(fields[0].split("\t")) != 14:
        raise SystemExit("checkpoint row is not an immutable result object")
    values = fields[0].split("\t")
    if values[0] != str(index) or values[1] != turn or values[8] != "0":
        raise SystemExit("checkpoint row is not a successful remote completion")
    if values[11] != "1" or values[12] != "1":
        raise SystemExit("checkpoint row is not an exact remote completion")
    seen.add(index)
    indices.append(index)
pathlib.Path(indices_path).write_text("".join(f"{index}\n" for index in sorted(indices)), encoding="ascii")
PY
fi

exec 9>"$oracle_root/.lock"
flock -x 9
cache_ready=0
if test -f "$oracle_root/COMPLETE.sha256" \
    && test "$(cat "$oracle_root/COMPLETE.sha256")" = "$oracle_identity"
then
    cache_ready=1
fi

compiler_arg_index=0
while test "$compiler_arg_index" -lt "$compiler_arg_count"
do
    variable="ICEFARM_COMPILER_ARG_$compiler_arg_index"
    printf -v "$variable" '%s' "${compiler_args[$compiler_arg_index]}"
    export "$variable"
    compiler_arg_index=$((compiler_arg_index + 1))
done

oracle_compile() {
    set -euo pipefail
    source=$1
    object=$2
    oracle_compiler_args=()
    oracle_compiler_arg_index=0
    while test "$oracle_compiler_arg_index" -lt "$compiler_arg_count"
    do
        variable="ICEFARM_COMPILER_ARG_$oracle_compiler_arg_index"
        oracle_compiler_args+=("${!variable}")
        oracle_compiler_arg_index=$((oracle_compiler_arg_index + 1))
    done
    if test "$oracle_recipe" = icecream-clang-remote-v1
    then
        # Icecream sends preprocessed Clang input on stdin and restores the
        # client-side source identity explicitly.  Those frontend options can
        # affect code generation even without debug info, so a direct compile
        # is not a byte-identity oracle for Clang.
        "$compiler" \
            -x c++ \
            -Xclang -main-file-name -Xclang "$source" \
            -Xclang -fdebug-compilation-dir -Xclang "$PWD" \
            "${oracle_compiler_args[@]}" \
            -c -target "$compiler_target" - -o "$object" \
            -no-canonical-prefixes <"$source"
    else
        "$compiler" "${oracle_compiler_args[@]}" -c "$source" -o "$object"
    fi
}

oracle_one() {
    set -euo pipefail
    digest=$1
    relative=$2
    source=$3
    key=$(printf '%s\n%s\n' "$digest" "$relative" | sha256sum | awk '{print $1}')
    digest_file="$oracle_root/$key.sha256"
    object="$oracle_root/.build-$key-$BASHPID.o"
    temporary="$oracle_root/.$key.sha256.$BASHPID"
    trap 'rm -f -- "$object" "$temporary"' EXIT
    oracle_compile "$source" "$object"
    observed=$(sha256sum "$object" | awk '{print $1}')
    printf '%s\n' "$observed" >"$temporary"
    mv -f -- "$temporary" "$digest_file"
    rm -f -- "$object"
    trap - EXIT
}
export -f oracle_compile oracle_one
export oracle_root compiler compiler_arg_count oracle_recipe compiler_target

if test "$cache_ready" -eq 0
then
    while IFS=$'\t' read -r digest relative source
    do
        printf '%s\0%s\0%s\0' "$digest" "$relative" "$source"
    done <"$unique" \
        | xargs -0 -r -n 3 -P "$jobs" /bin/bash -c 'oracle_one "$@"' icefarm-oracle
fi

while IFS=$'\t' read -r digest relative source
do
    key=$(printf '%s\n%s\n' "$digest" "$relative" | sha256sum | awk '{print $1}')
    digest_file="$oracle_root/$key.sha256"
    test -f "$digest_file" -a ! -L "$digest_file"
    expected=$(cat "$digest_file")
    printf '%s' "$expected" | grep -Eq '^[0-9a-f]{64}$'
done <"$unique"
if test "$cache_ready" -eq 0
then
    temporary="$oracle_root/.COMPLETE.sha256.$$"
    printf '%s\n' "$oracle_identity" >"$temporary"
    mv -f -- "$temporary" "$oracle_root/COMPLETE.sha256"
fi

sample_bucket=$((16#$(printf '%s' "$client_name:$manifest_digest" | sha256sum | cut -c1-7) % 20))
sample_total=0
sample_mismatches=0
unique_index=0
sample_file="$result_root/oracle-samples.tsv"
sample_temporary="$sample_file.tmp-$BASHPID"
rm -f -- "$sample_temporary"
: >"$sample_temporary"
while IFS=$'\t' read -r digest relative source
do
    unique_index=$((unique_index + 1))
    key=$(printf '%s\n%s\n' "$digest" "$relative" | sha256sum | awk '{print $1}')
    bucket=$((16#${key:0:7} % 20))
    if test "$bucket" -ne "$sample_bucket" -a "$unique_index" -ne 1
    then
        continue
    fi
    sample_total=$((sample_total + 1))
    object="$result_root/oracle-samples/$unique_index.o"
    oracle_compile "$source" "$object"
    observed=$(sha256sum "$object" | awk '{print $1}')
    expected=$(cat "$oracle_root/$key.sha256")
    exact=0
    test "$observed" = "$expected" && exact=1 || sample_mismatches=$((sample_mismatches + 1))
    printf '%s\t%s\t%s\t%s\n' "$relative" "$observed" "$expected" "$exact" \
        >>"$sample_temporary"
    rm -f -- "$object"
done <"$unique"
test "$sample_total" -ge 1
mv -f -- "$sample_temporary" "$sample_file"
printf 'sample_total\t%s\nsample_mismatches\t%s\n' "$sample_total" "$sample_mismatches" \
    >"$result_root/oracle-summary.tsv"
flock -u 9
test "$sample_mismatches" -eq 0

write_checkpoint() {
    checkpoint_tmp="$checkpoint_path.tmp-$BASHPID"
    python3 - "$checkpoint_tmp" "$result_root" "$worklist" "$client_name" "$turn" "$expected_jobs" <<'PY'
import hashlib
import json
import os
import pathlib
import sys

output, result_root, worklist, client, turn, expected = sys.argv[1:]
root = pathlib.Path(result_root)
rows = []
jobs_root = root / "jobs"
if jobs_root.is_symlink() or not jobs_root.is_dir():
    raise SystemExit("checkpoint jobs root is not a regular directory")
for directory, dirs, files in os.walk(jobs_root, topdown=True, followlinks=False):
    if any((pathlib.Path(directory) / name).is_symlink() for name in dirs):
        raise SystemExit("checkpoint jobs directory contains a symlink")
    for name in files:
        candidate = pathlib.Path(directory) / name
        relative = candidate.relative_to(root).as_posix()
        if name == "result.tsv" and not __import__("re").fullmatch(r"jobs/[0-9]{6}/result\.tsv", relative):
            raise SystemExit("checkpoint result path is not exact")
for index in range(1, int(expected) + 1):
    path = jobs_root / f"{index:06d}" / "result.tsv"
    parent = path.parent
    if parent.is_symlink() or not parent.is_dir():
        if parent.exists() or parent.is_symlink():
            raise SystemExit("checkpoint result ancestor is not a regular directory")
        continue
    if path.is_symlink():
        raise SystemExit("checkpoint result path is a symlink")
    if not path.exists():
        continue
    if not path.is_file():
        raise SystemExit("checkpoint result path is not a regular file")
    if any(item.is_symlink() for item in (jobs_root, parent)):
        raise SystemExit("checkpoint result ancestor is a symlink")
    if path.is_symlink() or not path.is_file():
        raise SystemExit("checkpoint result row is not a regular file")
    values = path.read_text(encoding="utf-8").splitlines()
    if len(values) != 1 or len(values[0].split("\t")) != 14:
        raise SystemExit("checkpoint result row is malformed")
    fields = values[0].split("\t")
    if fields[0] != str(int(fields[0])) or fields[1] != turn or fields[8] != "0" or fields[11] != "1" or fields[12] != "1":
        raise SystemExit("checkpoint requires successful exact remote rows")
    if fields[0] != str(index):
        raise SystemExit("checkpoint result index/path mismatch")
    rows.append({
        "index": index,
        "path": str(path.relative_to(root)),
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
    })
if len({row["index"] for row in rows}) != len(rows):
    raise SystemExit("checkpoint has duplicate result indices")
if any(row["index"] < 1 or row["index"] > int(expected) for row in rows):
    raise SystemExit("checkpoint result index is outside the worklist")
document = {
    "client": client,
    "completed_rows": rows,
    "completed_rows_sha256": hashlib.sha256(
        json.dumps(rows, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest(),
    "expected_jobs": int(expected),
    "schema": "icefarm-workload-checkpoint-v1",
    "status": "QUIESCED",
    "turn": turn,
    "worklist_sha256": hashlib.sha256(pathlib.Path(worklist).read_bytes()).hexdigest(),
}
document["checkpoint_sha256"] = hashlib.sha256(
    json.dumps(document, sort_keys=True, separators=(",", ":")).encode()
).hexdigest()
pathlib.Path(output).write_bytes(
    json.dumps(document, sort_keys=True, separators=(",", ":")).encode() + b"\n"
)
os.chmod(output, 0o666)
PY
    mv -- "$checkpoint_tmp" "$checkpoint_path"
    chmod 0444 "$checkpoint_path"
    find "$result_root/jobs" -mindepth 2 -maxdepth 2 -name result.tsv -type f -exec chmod 0444 {} +
}

compile_one() {
    index=$1
    turn=$2
    occurrence=$3
    relative=$4
    digest=$5
    if test "$resume_mode" -eq 1 && grep -Fqx "$index" "$resume_indices"
    then
        return 0
    fi
    job_dir=$(printf '%s/jobs/%06d' "$result_root" "$index")
    source="$corpus_root/$relative"
    key=$(printf '%s\n%s\n' "$digest" "$relative" | sha256sum | awk '{print $1}')
    local_sha=$(cat "$oracle_root/$key.sha256")
    remote_object="$job_dir/remote.o"
    debug_log="$job_dir/client-debug.log"
    output_log="$job_dir/client-output.log"
    marker="$gate_active/job-$index-$BASHPID.tsv"
    gate_deadline=$((SECONDS + per_job_timeout))
    admitted=0
    while test "$admitted" -eq 0
    do
        exec 8>"$gate_lock"
        flock -x 8
        IFS=$'\t' read -r gate_mode gate_epoch <"$gate_state"
        case "$gate_mode" in
            OPEN)
                admit_now=0
                if test "$event_serial_through" -eq 0 \
                    -o "$gate_epoch" -ge 1
                then
                    admit_now=1
                elif test "$index" -le "$event_serial_through"
                then
                    # Before active scheduler loss, admit the exact prefix in
                    # numeric order and with at most one live compiler.  The
                    # event's Nth dispatch therefore has exactly one exposed
                    # workload request; later jobs remain behind epoch zero.
                    predecessor_ready=0
                    if test "$index" -eq 1
                    then
                        predecessor_ready=1
                    else
                        predecessor=$(printf '%s/jobs/%06d/result.tsv' \
                            "$result_root" "$((index - 1))")
                        test -f "$predecessor" -a ! -L "$predecessor" \
                            && predecessor_ready=1 || :
                    fi
                    active_count=$(find "$gate_active" -mindepth 1 -maxdepth 1 \
                        -name 'job-*.tsv' -type f | wc -l)
                    if test "$predecessor_ready" -eq 1 \
                        -a "$active_count" -eq 0
                    then
                        admit_now=1
                    fi
                fi
                # Reserve a suffix on every S60 client until the existing
                # authenticated transition resumes epoch one. Remote polling
                # and pause preparation may otherwise outlast the workload.
                if test "$s60_admit_through" -gt 0 \
                    -a "$gate_epoch" -eq 0 \
                    -a "$index" -gt "$s60_admit_through"
                then
                    admit_now=0
                fi
                if test "$admit_now" -eq 1
                then
                    temporary_marker="$gate_active/.job-$index-$BASHPID.tmp"
                    printf '%s\t%s\t%s\t%s\n' \
                        "$index" "$BASHPID" "$gate_epoch" "$(date +%s%3N)" \
                        >"$temporary_marker"
                    mv -- "$temporary_marker" "$marker"
                    admitted=1
                fi
                ;;
            PAUSE) ;;
            QUIESCE)
                echo "event gate quiesced at epoch $gate_epoch for job $index" >&2
                flock -u 8
                exec 8>&-
                return 75
                ;;
            ABORT)
                echo "event gate aborted at epoch $gate_epoch for job $index" >&2
                flock -u 8
                exec 8>&-
                return 75
                ;;
            *)
                echo "invalid event gate state: $gate_mode $gate_epoch" >&2
                flock -u 8
                exec 8>&-
                return 75
                ;;
        esac
        flock -u 8
        exec 8>&-
        if test "$admitted" -eq 0
        then
            if test "$SECONDS" -ge "$gate_deadline"
            then
                echo "event gate wait expired for job $index" >&2
                return 75
            fi
            sleep 0.05
        fi
    done
    trap 'rm -f -- "$marker"' EXIT
    if test "$event_serial_through" -gt 0 \
        -a "$index" -eq "$event_serial_through"
    then
        # Hold the exact trigger job before icecc can request its scheduler
        # assignment.  The event owner first authenticates this marker, then
        # arms the F-local zero-skip compiler observer, and only then publishes
        # the exact release receipt.  This removes observation-count timing
        # from active-loss target selection.
        boundary_ready="$gate_root/active-compiler-boundary-$index.ready.tsv"
        boundary_control="$gate_root/active-compiler-boundary-$index.control"
        boundary_release="$boundary_control/release.tsv"
        test ! -e "$boundary_ready" -a ! -L "$boundary_ready"
        test ! -e "$boundary_control" -a ! -L "$boundary_control"
        set -o noclobber
        printf 'icefarm-active-compiler-boundary-v1\t%s\t%s\t%s\t%s\n' \
            "$index" "$BASHPID" "$gate_epoch" "$(date +%s%3N)" \
            >"$boundary_ready"
        set +o noclobber
        boundary_deadline=$((SECONDS + per_job_timeout))
        while true
        do
            if test -e "$boundary_release" -o -L "$boundary_release"
            then
                release_payload=$(read_boundary_release "$boundary_control")
                IFS=$'\t' read -r release_schema release_index release_pid release_ms \
                    <<<"$release_payload"
                if test "$release_schema" != icefarm-active-compiler-release-v1 \
                    -o "$release_index" != "$index" \
                    -o "$release_pid" != "$BASHPID"
                then
                    echo "invalid active-compiler boundary release" >&2
                    return 75
                fi
                case "$release_ms" in
                    ''|*[!0-9]*|0) echo "invalid active-compiler boundary release time" >&2; return 75 ;;
                esac
                break
            fi
            if test -e "$boundary_release" -o -L "$boundary_release"
            then
                echo "unsafe active-compiler boundary release" >&2
                return 75
            fi
            IFS=$'\t' read -r gate_mode gate_epoch <"$gate_state"
            case "$gate_mode" in
                OPEN) ;;
                QUIESCE|ABORT)
                    echo "event gate $gate_mode while awaiting active-compiler boundary release" >&2
                    return 75
                    ;;
                *) echo "invalid event gate state while awaiting active-compiler boundary release" >&2; return 75 ;;
            esac
            if test "$SECONDS" -ge "$boundary_deadline"
            then
                echo "active-compiler boundary release wait expired" >&2
                return 75
            fi
            sleep 0.05
        done
    fi
    mkdir "$job_dir"
    started=$(date +%s%3N)
    strict=()
    preferred=()
    job_compiler_args=()
    compiler_arg_index=0
    while test "$compiler_arg_index" -lt "$compiler_arg_count"
    do
        variable="ICEFARM_COMPILER_ARG_$compiler_arg_index"
        job_compiler_args+=("${!variable}")
        compiler_arg_index=$((compiler_arg_index + 1))
    done
    test "$strict_p50" -eq 0 || strict=(ICECC_P50_C1F1_REQUIRED=1)
    if test "$disk_fill_trigger" -gt 0 -a "$index" -eq "$disk_fill_trigger"
    then
        preferred=(ICECC_PREFERRED_HOST="$disk_fill_worker")
    fi
    set +e
    env \
        ICECC_DEBUG=debug \
        ICECC_LOGFILE="$debug_log" \
        ICECC_TEST_REMOTEBUILD=1 \
        ICECC_VERSION="$environment" \
        "${strict[@]}" \
        "${preferred[@]}" \
        timeout "$per_job_timeout" \
        /opt/icecream/bin/icecc "$compiler" "${job_compiler_args[@]}" \
            -c "$source" -o "$remote_object" >"$output_log" 2>&1
    compile_rc=$?
    set -e
    finished=$(date +%s%3N)
    if test "$fault_kind" = corrupt-object \
        -a "$client_name" = "$fault_client" \
        -a "$index" -eq "$fault_job"
    then
        test "$compile_rc" -eq 0
        test -f "$remote_object"
        before_sha=$(sha256sum "$remote_object" | awk '{print $1}')
        printf '\001' >>"$remote_object"
        after_sha=$(sha256sum "$remote_object" | awk '{print $1}')
        test "$before_sha" != "$after_sha"
        mkdir -p "$result_root/fault"
        printf 'icefarm-h4-object-fault-v1\t%s\t%s\t%s\t%s\t%s\n' \
            "$client_name" "$index" "$before_sha" "$after_sha" \
            "jobs/$(printf '%06d' "$index")/remote.o" \
            >"$result_root/fault/h4.tsv"
    fi
    remote_sha=$(test -f "$remote_object" && sha256sum "$remote_object" | awk '{print $1}' || printf '%064d' 0)
    exact=0
    test "$compile_rc" -eq 0 -a "$remote_sha" = "$local_sha" && exact=1 || :
    assignments=$(sed -n 's/.*Have to use host \([^ ]*\) - Job ID: \([0-9][0-9]*\).*/\1\t\2/p' \
        "$debug_log" "$output_log" 2>/dev/null || true)
    assignment_count=$(printf '%s\n' "$assignments" | sed '/^$/d' | wc -l)
    selected=$(printf '%s\n' "$assignments" | tail -1)
    worker=${selected%%$'\t'*}
    scheduler_job=${selected#*$'\t'}
    if test -z "$selected"
    then
        worker=UNKNOWN
        scheduler_job="missing-$index"
    fi
    local_fallback=0
    if grep -qF '<building_local>' "$debug_log" "$output_log" 2>/dev/null \
        || grep -qF 'building myself, but telling localhost' \
            "$debug_log" "$output_log" 2>/dev/null
    then
        local_fallback=1
    fi
    remote=1
    if test -z "$selected" -o "$local_fallback" -eq 1
    then
        remote=0
    fi
    retries=$((assignment_count > 0 ? assignment_count - 1 : 0))
    if test "$event_serial_through" -gt 0 \
        -a "$index" -eq "$event_serial_through"
    then
        # Keep the sole exposed boundary marker alive until the event owner
        # has authenticated full S/F/C rejoin and atomically opened epoch 1.
        # The remote compile may already have completed by then; retaining the
        # wrapper marker prevents a fast retry from racing the v5 1->1 release
        # receipt.  ABORT remains a bounded fail-safe for every error path.
        serial_boundary_released=0
        serial_release_deadline=$((SECONDS + per_job_timeout))
        while test "$serial_boundary_released" -eq 0
        do
            exec 8>"$gate_lock"
            flock -x 8
            IFS=$'\t' read -r gate_mode gate_epoch <"$gate_state"
            case "$gate_mode" in
                OPEN)
                    if test "$gate_epoch" -ge 1
                    then
                        serial_boundary_released=1
                    fi
                    ;;
                PAUSE) ;;
                QUIESCE|ABORT)
                    echo "event gate $gate_mode at epoch $gate_epoch for serial boundary job $index" >&2
                    flock -u 8
                    exec 8>&-
                    return 75
                    ;;
                *)
                    echo "invalid event gate state: $gate_mode $gate_epoch" >&2
                    flock -u 8
                    exec 8>&-
                    return 75
                    ;;
            esac
            flock -u 8
            exec 8>&-
            if test "$serial_boundary_released" -eq 0
            then
                if test "$SECONDS" -ge "$serial_release_deadline"
                then
                    echo "event release wait expired for serial boundary job $index" >&2
                    return 75
                fi
                sleep 0.05
            fi
        done
    fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$index" "$turn" "$occurrence" "$relative" "$scheduler_job" "$worker" \
        "$started" "$finished" "$compile_rc" "$remote_sha" "$local_sha" "$exact" "$remote" \
        "$retries" \
        >"$job_dir/result.tsv"
    # Keep a completed prefix boundary visible until the controller closes
    # admission. It requires a positive marker even if every compile finished
    # before polling. Release on PAUSE/QUIESCE, not resume: pause drains these
    # markers before it can publish epoch one. The result is already durable
    # for a C-transition checkpoint before its marker disappears.
    if test "$s60_admit_through" -gt 0 \
        -a "$index" -eq "$s60_admit_through" -a "$gate_epoch" -eq 0
    then
        s60_boundary_deadline=$((SECONDS + per_job_timeout))
        while :
        do
            exec 8>"$gate_lock"
            flock -x 8
            IFS=$'\t' read -r boundary_mode boundary_epoch <"$gate_state"
            flock -u 8
            exec 8>&-
            case "$boundary_mode" in
                PAUSE|QUIESCE)
                    test "$boundary_epoch" -eq 1 || return 75
                    break ;;
                OPEN)
                    if test "$boundary_epoch" -eq 1; then break; fi
                    test "$boundary_epoch" -eq 0 || return 75 ;;
                *) return 75 ;;
            esac
            if test "$SECONDS" -ge "$s60_boundary_deadline"
            then
                echo "S60 boundary pause wait expired for job $index" >&2
                return 75
            fi
            sleep 0.05
        done
    fi
    rm -f -- "$remote_object"
    rm -f -- "$marker"
    trap - EXIT
    return 0
}
export -f read_boundary_release compile_one
export result_root corpus_root oracle_root environment per_job_timeout strict_p50 compiler compiler_arg_count
export client_name fault_kind fault_client fault_job
export gate_root gate_state gate_lock gate_active event_serial_through s60_admit_through
export disk_fill_worker disk_fill_trigger
export resume_mode resume_indices

set +e
xargs -0 -n 5 -P "$jobs" /bin/bash -c 'compile_one "$@"' icefarm-job <"$worklist"
xargs_rc=$?
set -e
gate_mode=$(head -n 1 "$gate_state" | cut -f1)
if test "$gate_mode" = QUIESCE
then
    write_checkpoint
    observed_jobs=$(find "$result_root/jobs" -mindepth 2 -maxdepth 2 -name result.tsv -type f | wc -l)
    test "$observed_jobs" -ge 1
    printf 'jobs\t%s\nfailures\t0\nsamples\t%s\n' \
        "$observed_jobs" "$sample_total" >"$result_root/summary.tsv"
    printf 'ICEFARM_WORKLOAD jobs=%s failures=0 samples=%s\n' \
        "$observed_jobs" "$sample_total"
    exit 0
fi
test "$xargs_rc" -eq 0
observed_jobs=$(find "$result_root/jobs" -mindepth 2 -maxdepth 2 -name result.tsv -type f | wc -l)
test "$observed_jobs" -eq "$expected_jobs"
failures=0
while IFS=$'\t' read -r _index _turn _occurrence _relative _job _worker \
    _started _finished compile_rc _remote_sha _local_sha exact remote _retries
do
    test "$compile_rc" -eq 0 -a "$exact" -eq 1 -a "$remote" -eq 1 \
        || failures=$((failures + 1))
done < <(find "$result_root/jobs" -mindepth 2 -maxdepth 2 -name result.tsv -type f -print0 \
    | sort -z | xargs -0 cat)
printf 'jobs\t%s\nfailures\t%s\nsamples\t%s\n' \
    "$observed_jobs" "$failures" "$sample_total" >"$result_root/summary.tsv"
printf 'ICEFARM_WORKLOAD jobs=%s failures=%s samples=%s\n' \
    "$observed_jobs" "$failures" "$sample_total"