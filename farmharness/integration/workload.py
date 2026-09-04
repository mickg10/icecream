"""Manifest workload drivers and the deterministic local-SHA oracle."""

from __future__ import annotations

import json
import os
import re
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Any

try:
    from .farm_spec import FarmSpec
    from .images import CommandFactory, RecordingTransport
    from .lifecycle import bundle_root
    from .remote import CommandResult, PlannedCommand, RemoteError, docker_argv
    from .scenario_spec import ScenarioSpec
    from .schema_validation import canonical_bytes
except ImportError:  # Direct execution from this directory.
    from farm_spec import FarmSpec
    from images import CommandFactory, RecordingTransport
    from lifecycle import bundle_root
    from remote import CommandResult, PlannedCommand, RemoteError, docker_argv
    from scenario_spec import ScenarioSpec
    from schema_validation import canonical_bytes


WORKLOAD_SCHEMA = "icefarm-workload-v1"
SUMMARY_RE = re.compile(
    r"^ICEFARM_WORKLOAD jobs=([0-9]+) failures=([0-9]+) samples=([0-9]+)$",
    re.MULTILINE,
)


# Values from farm/scenario documents are passed as argv after this fixed
# program.  They are never interpolated into shell source.
MANIFEST_DRIVER = r"""
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
shift 11
turns=("$@")

test "$base_tus" -ge 1
test "$corpus_repeat" -ge 1
test "$workload_repeat" -ge 1
test "$jobs" -ge 1
test "$per_job_timeout" -ge 1
test "${#turns[@]}" -ge 1
test "$layout" = single -o "$layout" = paired
test "$strict_p50" = 0 -o "$strict_p50" = 1
test -f "$corpus_root/MANIFEST.sha256"
test ! -e "$result_root"
mkdir -p "$result_root/jobs" "$result_root/oracle-samples" "$oracle_root"
chmod 0777 "$result_root/jobs" "$result_root/oracle-samples"

environment=$(find /results/env -maxdepth 1 -type f -name '*.tar.gz' -print -quit)
test -n "$environment"
test -r "$environment"

manifest_digest=$(sha256sum "$corpus_root/MANIFEST.sha256" | awk '{print $1}')
compiler_digest=$(sha256sum /usr/bin/g++-11 | awk '{print $1}')
oracle_command='g++-11 -O2 -fdiagnostics-color=never -c'
oracle_identity=$(printf '%s\n%s\n%s\n' \
    "$manifest_digest" "$compiler_digest" "$oracle_command" | sha256sum | awk '{print $1}')
worklist="$result_root/worklist.bin"
unique="$result_root/unique.tsv"
: >"$worklist"
: >"$unique"
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
        printf '%s\0%s\0%s\0%s\0' "$ordinal" "$turn" "$occurrence" "$relative" >>"$worklist"
        printf '%s\t%s\n' "$relative" "$resolved" >>"$unique"
    done <"$corpus_root/MANIFEST.sha256"
    test "$found" -eq "$base_tus"
}

for turn in "${turns[@]}"
do
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
done

expected_jobs=$((base_tus * corpus_repeat * workload_repeat * ${#turns[@]}))
test "$ordinal" -eq "$expected_jobs"
sort -u "$unique" -o "$unique"

exec 9>"$oracle_root/.lock"
flock -x 9
cache_ready=0
if test -f "$oracle_root/COMPLETE.sha256" \
    && test "$(cat "$oracle_root/COMPLETE.sha256")" = "$oracle_identity"
then
    cache_ready=1
fi

while IFS=$'\t' read -r relative source
do
    key=$(printf '%s' "$relative" | sha256sum | awk '{print $1}')
    digest_file="$oracle_root/$key.sha256"
    if test "$cache_ready" -eq 0
    then
        object="$oracle_root/.build-$$.o"
        /usr/bin/g++-11 -O2 -fdiagnostics-color=never -c "$source" -o "$object"
        observed=$(sha256sum "$object" | awk '{print $1}')
        rm -f -- "$object"
        temporary="$oracle_root/.$key.sha256.$$"
        printf '%s\n' "$observed" >"$temporary"
        mv -f -- "$temporary" "$digest_file"
    fi
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
while IFS=$'\t' read -r relative source
do
    unique_index=$((unique_index + 1))
    key=$(printf '%s' "$relative" | sha256sum | awk '{print $1}')
    bucket=$((16#${key:0:7} % 20))
    if test "$bucket" -ne "$sample_bucket" -a "$unique_index" -ne 1
    then
        continue
    fi
    sample_total=$((sample_total + 1))
    object="$result_root/oracle-samples/$unique_index.o"
    /usr/bin/g++-11 -O2 -fdiagnostics-color=never -c "$source" -o "$object"
    observed=$(sha256sum "$object" | awk '{print $1}')
    expected=$(cat "$oracle_root/$key.sha256")
    exact=0
    test "$observed" = "$expected" && exact=1 || sample_mismatches=$((sample_mismatches + 1))
    printf '%s\t%s\t%s\t%s\n' "$relative" "$observed" "$expected" "$exact" \
        >>"$result_root/oracle-samples.tsv"
    rm -f -- "$object"
done <"$unique"
test "$sample_total" -ge 1
printf 'sample_total\t%s\nsample_mismatches\t%s\n' "$sample_total" "$sample_mismatches" \
    >"$result_root/oracle-summary.tsv"
flock -u 9
test "$sample_mismatches" -eq 0

compile_one() {
    index=$1
    turn=$2
    occurrence=$3
    relative=$4
    job_dir=$(printf '%s/jobs/%06d' "$result_root" "$index")
    mkdir "$job_dir"
    source="$corpus_root/$relative"
    key=$(printf '%s' "$relative" | sha256sum | awk '{print $1}')
    local_sha=$(cat "$oracle_root/$key.sha256")
    remote_object="$job_dir/remote.o"
    debug_log="$job_dir/client-debug.log"
    output_log="$job_dir/client-output.log"
    started=$(date +%s%3N)
    strict=()
    test "$strict_p50" -eq 0 || strict=(ICECC_P50_C1F1_REQUIRED=1)
    set +e
    env \
        ICECC_DEBUG=debug \
        ICECC_LOGFILE="$debug_log" \
        ICECC_TEST_REMOTEBUILD=1 \
        ICECC_VERSION="$environment" \
        "${strict[@]}" \
        timeout "$per_job_timeout" \
        /opt/icecream/bin/icecc /usr/bin/g++-11 -O2 -fdiagnostics-color=never \
            -c "$source" -o "$remote_object" >"$output_log" 2>&1
    compile_rc=$?
    set -e
    finished=$(date +%s%3N)
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
    remote=1
    if test -z "$selected" \
        || grep -qF 'building myself, but telling localhost' \
            "$debug_log" "$output_log" 2>/dev/null
    then
        remote=0
    fi
    retries=$((assignment_count > 0 ? assignment_count - 1 : 0))
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$index" "$turn" "$occurrence" "$relative" "$scheduler_job" "$worker" \
        "$started" "$finished" "$compile_rc" "$remote_sha" "$local_sha" "$exact" "$remote" \
        "$retries" \
        >"$job_dir/result.tsv"
    rm -f -- "$remote_object"
    return 0
}
export -f compile_one
export result_root corpus_root oracle_root environment per_job_timeout strict_p50

xargs -0 -n 4 -P "$jobs" /bin/bash -c 'compile_one "$@"' icefarm-job <"$worklist"
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
""".strip()


class WorkloadError(RuntimeError):
    """The workload could not produce a complete evidence surface."""


def _atomic_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + f".tmp-{os.getpid()}")
    try:
        temporary.write_bytes(canonical_bytes(value))
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def _docker_transport(farm: FarmSpec, host_name: str) -> str:
    return "docker-context" if farm.hosts[host_name].get("docker_context") else "ssh-docker"


def _assert_up(farm: FarmSpec, scenario: ScenarioSpec, plan: dict[str, Any]) -> None:
    path = bundle_root(farm, plan["run_id"]) / "lifecycle.json"
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise WorkloadError(f"cannot load UP lifecycle receipt: {exc}") from exc
    if (
        not isinstance(value, dict)
        or value.get("status") != "UP"
        or value.get("run_id") != plan["run_id"]
        or value.get("scenario_digest") != scenario.digest
        or value.get("topology_digest") != plan["topology_digest"]
    ):
        raise WorkloadError("lifecycle receipt does not authenticate this UP run")


def _driver_command(
    farm: FarmSpec,
    scenario: ScenarioSpec,
    plan: dict[str, Any],
    client: dict[str, Any],
    factory: CommandFactory,
) -> PlannedCommand:
    workload = scenario.data["workload"]
    corpus = farm.data["corpora"][workload["corpus"]]
    layout = "single" if "manifest" in corpus else "paired"
    corpus_repeat = corpus.get("repeat", 1)
    strict_p50 = int(
        scenario.data["shape"] == "S'C'F'"
        and all(item["version"] == 50 for item in plan["topology"]["instances"])
    )
    container = f"icefarm-{plan['run_id']}-{client['name']}"
    timeout_s = scenario.data["timeouts"]["turn_s"] * len(workload["turns"]) + 300
    argv = docker_argv(
        farm,
        client["host"],
        (
            "exec",
            "--user",
            "icecc",
            container,
            "/bin/bash",
            "-c",
            MANIFEST_DRIVER,
            "icefarm-manifest-driver",
            "/results/workload",
            "/corpus",
            "/oracle",
            client["name"],
            str(corpus["tus"]),
            str(corpus_repeat),
            str(workload["repeat"]),
            str(workload["jobs"]),
            str(scenario.data["timeouts"]["turn_s"]),
            layout,
            str(strict_p50),
            *workload["turns"],
        ),
    )
    return factory.make(
        phase="run.workload",
        host=client["host"],
        transport=_docker_transport(farm, client["host"]),
        timeout_s=timeout_s,
        argv=argv,
    )


def _parse_summary(result: CommandResult, client: str) -> dict[str, int | str]:
    matches = SUMMARY_RE.findall(result.stdout)
    if len(matches) != 1:
        raise WorkloadError(f"client {client} returned no unique workload summary")
    jobs, failures, samples = (int(value) for value in matches[0])
    if jobs < 1 or samples < 1 or failures > jobs:
        raise WorkloadError(f"client {client} returned an invalid workload summary")
    return {"client": client, "failures": failures, "jobs": jobs, "samples": samples}


def run_workload(
    farm: FarmSpec,
    scenario: ScenarioSpec,
    plan: dict[str, Any],
    *,
    recorder: RecordingTransport | None = None,
    require_up: bool = True,
) -> dict[str, Any]:
    """Run all declared clients concurrently and retain a bounded receipt."""

    if require_up:
        _assert_up(farm, scenario, plan)
    transport = recorder or RecordingTransport()
    factory = CommandFactory()
    client_names = set(scenario.data["workload"]["clients"])
    clients = sorted(
        (
            item
            for item in plan["topology"]["instances"]
            if item["role"] == "C" and item["name"] in client_names
        ),
        key=lambda item: item["name"],
    )
    if {item["name"] for item in clients} != client_names:
        raise WorkloadError("resolved topology does not contain every workload client")
    commands = [
        _driver_command(farm, scenario, plan, client, factory) for client in clients
    ]
    try:
        with ThreadPoolExecutor(max_workers=len(commands)) as executor:
            results = list(executor.map(transport.invoke, commands))
    except RemoteError as exc:
        raise WorkloadError(str(exc)) from exc
    summaries = [
        _parse_summary(result, client["name"])
        for result, client in zip(results, clients, strict=True)
    ]
    receipt = {
        "clients": summaries,
        "commands": [command.as_dict() for command in commands],
        "farm_digest": farm.digest,
        "run_id": plan["run_id"],
        "scenario_digest": scenario.digest,
        "schema": WORKLOAD_SCHEMA,
        "status": "COMPLETE"
        if all(item["failures"] == 0 for item in summaries)
        else "COMPLETE_WITH_JOB_FAILURES",
        "topology_digest": plan["topology_digest"],
    }
    _atomic_json(bundle_root(farm, plan["run_id"]) / "workload.json", receipt)
    return receipt
