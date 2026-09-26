"""Manifest workload drivers and the deterministic local-SHA oracle."""

from __future__ import annotations

import json
import os
import re
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Any

try:
    from .events import EventError, EventProducer, JobReader
    from .farm_spec import FarmSpec
    from .images import CommandFactory, RecordingTransport
    from .layout import compiler_identity_digest
    from .lifecycle import LifecycleError, activate_corpus_turn, bundle_root
    from .remote import CommandResult, PlannedCommand, RemoteError, docker_argv
    from .scenario_spec import PROFILES, ScenarioSpec
    from .schema_validation import canonical_bytes
except ImportError:  # Direct execution from this directory.
    from events import EventError, EventProducer, JobReader
    from farm_spec import FarmSpec
    from images import CommandFactory, RecordingTransport
    from layout import compiler_identity_digest
    from lifecycle import LifecycleError, activate_corpus_turn, bundle_root
    from remote import CommandResult, PlannedCommand, RemoteError, docker_argv
    from scenario_spec import PROFILES, ScenarioSpec
    from schema_validation import canonical_bytes


WORKLOAD_SCHEMA = "icefarm-workload-v1"
SUMMARY_RE = re.compile(
    r"^ICEFARM_WORKLOAD jobs=([0-9]+) failures=([0-9]+) samples=([0-9]+)$",
    re.MULTILINE,
)
D18_R2_ADOPTION_RE = re.compile(
    r"P51 cache-link descriptor adopted by sidecar \(generation ([1-9][0-9]*)/([1-9][0-9]*)\)"
)
IMAGE_GENERATION_RE = re.compile(
    r"^p(43|44|50)(?:s[0-9]+)?(?:-|$)", re.IGNORECASE
)
D18_PROCESS_OBSERVER = r'''
for statfile in /proc/[0-9]*/stat
do
    pid=${statfile#/proc/}
    pid=${pid%/stat}
    IFS= read -r comm <"/proc/$pid/comm" 2>/dev/null || continue
    test "$comm" = cc1plus || continue
    IFS= read -r statline <"$statfile" 2>/dev/null || continue
    rest=${statline##*) }
    set -- $rest
    test "$#" -ge 20 || continue
    printf '%s %s\n' "$pid" "${20}"
done'''


# Values from farm/scenario documents are passed as argv after this fixed
# program.  They are never interpolated into shell source.
MANIFEST_DRIVER = (
    (Path(__file__).parent / "workers" / "manifest_driver.sh").read_text(
        encoding="utf-8"
    )
)


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
    return (
        "docker-context"
        if farm.hosts[host_name].get("docker_context")
        else "ssh-docker"
    )


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


def _strict_p50_required(scenario: ScenarioSpec, plan: dict[str, Any]) -> bool:
    """Require P50 only when every phase of this workload requires it.

    The client wrapper is fixed for a complete turn, so a scenario that starts
    with P50 disabled, or disables it during the turn, must retain the remote
    legacy path.  Remote-only execution is enforced independently by the
    workload and verdict layers.
    """

    # Only active scheduler loss permits the one authenticated fresh legacy
    # retry; all ordinary all-new P50 cells remain strict.
    if scenario.data.get("id") == "S70-b4-scheduler-active-loss":
        return False
    if (
        scenario.data["shape"] != "S'C'F'"
        or scenario.data["controls"]
        or scenario.data.get("id") == "S30-mutant-f-refusal"
        or not all(
            item.get("version") == 50
            for item in plan.get("topology", {}).get("instances", [])
        )
    ):
        return False

    schedulers = [
        item
        for item in scenario.data["instances"]
        if item.get("role") == "S"
    ]
    if len(schedulers) != 1:
        raise WorkloadError("strict P50 requires exactly one scheduler")
    scheduler = schedulers[0]
    scheduler_name = scheduler.get("name")
    named_instances = {
        item.get("name"): item
        for item in scenario.data["instances"]
        if isinstance(item, dict)
    }
    environment = scheduler.get("env", {})
    if not isinstance(environment, dict):
        raise WorkloadError("strict P50 scheduler environment is invalid")
    profile = environment.get("ICECC_P50_PROFILE", "P29V1")
    if profile == "OFF":
        return False
    if profile not in PROFILES:
        raise WorkloadError(f"strict P50 scheduler profile is invalid: {profile!r}")

    timeline = scenario.data.get("timeline", [])
    if not isinstance(timeline, list):
        raise WorkloadError("strict P50 timeline is invalid")
    for event in timeline:
        if not isinstance(event, dict):
            raise WorkloadError("strict P50 timeline event is invalid")
        action = event.get("action")
        target = named_instances.get(event.get("instance"))
        if action in {"upgrade", "downgrade"}:
            alias = event.get("image")
            label = scenario.data.get("images", {}).get(alias)
            generation = (
                IMAGE_GENERATION_RE.match(label.rsplit(":", 1)[-1])
                if isinstance(label, str)
                else None
            )
            if generation is None:
                raise WorkloadError("strict P50 transition image is invalid")
            if int(generation.group(1)) != 50:
                return False
            continue
        if action != "env_set":
            continue
        update = event.get("env")
        if not isinstance(update, dict):
            raise WorkloadError("strict P50 env_set is invalid")
        if isinstance(target, dict) and target.get("role") == "C":
            if update.get("ICECC_P50_MODE") == "off":
                return False
            continue
        if event.get("instance") != scheduler_name:
            continue
        event_profile = update.get("ICECC_P50_PROFILE")
        if event_profile == "OFF":
            return False
        if event_profile not in PROFILES:
            raise WorkloadError(
                f"strict P50 scheduler env_set profile is invalid: {event_profile!r}"
            )
    return True


def _active_loss_serial_through(scenario: ScenarioSpec) -> int:
    """Return the ordered job prefix serialized before active scheduler loss."""

    if scenario.data.get("id") != "S70-b4-scheduler-active-loss":
        return 0
    events = [
        event
        for event in scenario.data.get("timeline", [])
        if isinstance(event, dict)
        and event.get("action") == "scheduler-loss-active"
    ]
    if len(events) != 1:
        raise WorkloadError("active scheduler loss needs one serial admission event")
    trigger = events[0].get("trigger")
    match = re.fullmatch(r"job ([1-9][0-9]*)", trigger or "")
    if match is None:
        raise WorkloadError("active scheduler loss needs a positive serial job boundary")
    return int(match.group(1))


def _s60_admit_through(scenario: ScenarioSpec, corpus: dict[str, Any]) -> int:
    """Bound each S60 client's parallel prefix, preserving a post-event suffix."""
    if not scenario.data.get("id", "").startswith("S60-"):
        return 0
    events = scenario.data.get("timeline", [])
    if len(events) != 1 or events[0].get("action") not in {"upgrade", "downgrade"}:
        raise WorkloadError("S60 admission needs one upgrade/downgrade event")
    match = re.fullmatch(r"job ([1-9][0-9]*)", events[0].get("trigger", ""))
    if match is None:
        raise WorkloadError("S60 admission needs a positive job boundary")
    boundary = int(match.group(1))
    total = corpus["tus"] * corpus.get("repeat", 1) * scenario.data["workload"]["repeat"]
    if boundary >= total:
        raise WorkloadError("S60 admission boundary must preserve a client suffix")
    return boundary


def _driver_command(
    farm: FarmSpec,
    scenario: ScenarioSpec,
    plan: dict[str, Any],
    client: dict[str, Any],
    turn: str,
    factory: CommandFactory,
    *,
    resume: bool = False,
    checkpoint_sha256: str | None = None,
) -> PlannedCommand:
    workload = scenario.data["workload"]
    corpus = farm.data["corpora"][workload["corpus"]]
    layout = "single" if "manifest" in corpus else "paired"
    corpus_repeat = corpus.get("repeat", 1)
    strict_p50 = int(_strict_p50_required(scenario, plan))
    active_loss_serial_through = _active_loss_serial_through(scenario)
    s60_admit_through = _s60_admit_through(scenario, corpus)
    timeline = scenario.data.get("timeline", [])
    preferred_worker = (
        timeline[0].get("instance")
        if scenario.data.get("expect", {}).get("engagement")
        == "s95-cache-disk-full"
        and isinstance(timeline, list)
        and len(timeline) == 1
        and isinstance(timeline[0], dict)
        and timeline[0].get("action") == "disk_fill"
        else None
    )
    disk_fill_trigger = 0
    if preferred_worker is not None:
        match = re.fullmatch(r"job ([1-9][0-9]*)", timeline[0].get("trigger", ""))
        if match is None:
            raise WorkloadError("S95 disk fill needs a positive job trigger")
        disk_fill_trigger = int(match.group(1))
    container = f"icefarm-{plan['run_id']}-{client['name']}"
    d18_roles = workload.get("d18_roles")
    d18_role = (
        next(
            (role for role, name in d18_roles["clients"].items() if name == client["name"]),
            None,
        )
        if isinstance(d18_roles, dict)
        else None
    )
    compiler_args = list(client["compiler_recipe"]["arguments"])
    fault = scenario.data.get("fault", {})
    timeout_s = scenario.data["timeouts"]["turn_s"] + 300
    argv = docker_argv(
        farm,
        client["host"],
        (
            "exec",
            "--user",
            "65534:65534",
            *(
                (
                    "--env", f"ICEFARM_DISK_FILL_WORKER={preferred_worker}",
                    "--env", f"ICEFARM_DISK_FILL_TRIGGER={disk_fill_trigger}",
                )
                if isinstance(preferred_worker, str) and preferred_worker
                else ()
            ),
            *(
                ("--env", f"ICEFARM_S60_ADMIT_THROUGH={s60_admit_through}")
                if s60_admit_through else ()
            ),
            *(("--env", "ICEFARM_D18_BARRIER=1") if d18_role is not None else ()),
            *(
                (
                    "--env",
                    f"ICEFARM_EVENT_SERIAL_THROUGH={active_loss_serial_through}",
                    "--env",
                    "ICECC_REMOTE_REQUIRED=1",
                )
                if active_loss_serial_through
                else ()
            ),
            container,
            "/bin/bash",
            "-c",
            MANIFEST_DRIVER,
            "icefarm-manifest-driver",
            f"/results/workload/{turn}",
            "/corpus",
            f"/oracle/{turn}",
            client["name"],
            str(corpus["tus"]),
            str(corpus_repeat),
            str(workload["repeat"]),
            str(workload["jobs"]),
            str(scenario.data["timeouts"]["turn_s"]),
            layout,
            str(strict_p50),
            client["compiler_recipe"]["executable"],
            client["compiler_recipe"]["binary_sha256"],
            compiler_identity_digest(client),
            str(len(compiler_args)),
            *compiler_args,
            turn,
            fault.get("kind", ""),
            fault.get("client", ""),
            str(fault.get("job", 0)),
            *(
                (f"/results/workload/{turn}/checkpoint.json", "1")
                + ((checkpoint_sha256,) if checkpoint_sha256 is not None else ())
                if resume
                else ()
            ),
        ),
    )
    return factory.make(
        phase="run.workload",
        host=client["host"],
        instance=client["name"],
        transport=_docker_transport(farm, client["host"]),
        timeout_s=timeout_s,
        argv=argv,
    )


def _d18_docker_call(
    farm: FarmSpec,
    plan: dict[str, Any],
    instance: dict[str, Any],
    factory: CommandFactory,
    transport: RecordingTransport,
    suffix: str,
    argv: tuple[str, ...],
    *,
    user: str = "0",
) -> CommandResult:
    command = factory.make(
        phase=f"run.d18.{suffix}",
        host=instance["host"],
        instance=instance["name"],
        transport=_docker_transport(farm, instance["host"]),
        timeout_s=20,
        argv=docker_argv(
            farm,
            instance["host"],
            (
                "exec", "--user", user,
                f"icefarm-{plan['run_id']}-{instance['name']}", *argv,
            ),
        ),
    )
    result = transport.invoke(command)
    if result.returncode != 0:
        raise WorkloadError(
            f"D18 {suffix} failed for {instance['name']}: "
            f"{result.stderr.strip() or result.stdout.strip() or result.returncode}"
        )
    return result


def _d18_processes(
    farm: FarmSpec,
    plan: dict[str, Any],
    worker: dict[str, Any],
    factory: CommandFactory,
    transport: RecordingTransport,
) -> dict[int, int]:
    result = _d18_docker_call(
        farm,
        plan,
        worker,
        factory,
        transport,
        "observe-cc1plus",
        (
            "/bin/bash", "-c", D18_PROCESS_OBSERVER,
            "d18-observer",
        ),
    )
    observed: dict[int, int] = {}
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) != 2 or not all(field.isdecimal() for field in fields):
            continue
        pid, starttime = map(int, fields)
        if pid > 0 and starttime > 0:
            observed[pid] = starttime
    return observed


def _d18_concurrent_witness(
    first: dict[str, dict[int, int]],
    second: dict[str, dict[int, int]],
    expected_by_worker: dict[str, int],
) -> dict[str, list[dict[str, int]]] | None:
    """Require exact process counts and the same live identities in two sweeps."""
    if set(first) != set(expected_by_worker) or set(second) != set(expected_by_worker):
        return None
    if any(
        len(first[worker]) != count
        or len(second[worker]) != count
        or first[worker] != second[worker]
        for worker, count in expected_by_worker.items()
    ):
        return None
    return {
        worker: [
            {"pid": pid, "starttime_ticks": starttime}
            for pid, starttime in sorted(identities.items())
        ]
        for worker, identities in first.items()
    }


def _d18_barrier_and_observe(
    farm: FarmSpec,
    scenario: ScenarioSpec,
    plan: dict[str, Any],
    clients: list[dict[str, Any]],
    futures: list[Any],
    factory: CommandFactory,
    transport: RecordingTransport,
    deadline_s: int,
) -> dict[str, dict[str, Any]]:
    roles = scenario.data["workload"]["d18_roles"]
    by_name = {item["name"]: item for item in plan["topology"]["instances"]}
    worker_names = roles["workers"]
    expected_by_worker = {worker_names["R1"]: 2, worker_names["R2"]: 1}
    client_by_role = {role: by_name[name] for role, name in roles["clients"].items()}
    expected_ready = set(roles["clients"].values())
    ready_deadline = time.monotonic() + min(60, deadline_s)
    while True:
        ready = set()
        for client in clients:
            if client["name"] not in expected_ready:
                continue
            result = _d18_docker_call(
                farm, plan, client, factory, transport, "barrier-ready",
                ("/bin/bash", "-c", "test ! -f /results/workload/A/d18/ready || echo D18_READY"),
            )
            if "D18_READY" in result.stdout:
                ready.add(client["name"])
        if ready == expected_ready:
            break
        if any(future.done() for future in futures):
            for future in futures:
                if future.done():
                    future.result()
            raise WorkloadError("D18 client exited before the shared measured-start barrier")
        if time.monotonic() >= ready_deadline:
            raise WorkloadError("D18 clients did not all reach the measured-start barrier")
        time.sleep(0.1)

    # Release all three client-side gates concurrently only after each has
    # completed oracle warmup and published its ready marker.
    with ThreadPoolExecutor(max_workers=3) as release_pool:
        release_futures = [
            release_pool.submit(
                _d18_docker_call,
                farm, plan, client_by_role[role], factory, transport,
                f"barrier-release-{role}",
                ("/usr/bin/touch", "/results/workload/A/d18/go"),
            )
            for role in ("P43", "R1", "R2")
        ]
        for future in release_futures:
            future.result()

    deadline = time.monotonic() + min(60, deadline_s)
    while True:
        first: dict[str, dict[int, int]] = {}
        for worker_name in expected_by_worker:
            worker = by_name[worker_name]
            snapshots = _d18_processes(farm, plan, worker, factory, transport)
            if snapshots:
                first[worker_name] = snapshots
        second: dict[str, dict[int, int]] = {}
        if set(first) == set(expected_by_worker):
            for worker_name in expected_by_worker:
                worker = by_name[worker_name]
                second[worker_name] = _d18_processes(
                    farm, plan, worker, factory, transport
                )
            witness = _d18_concurrent_witness(first, second, expected_by_worker)
            if witness is not None:
                return {"workers": witness}
        if all(future.done() for future in futures):
            for future in futures:
                future.result()
            raise WorkloadError(
                "D18 jobs completed without simultaneous two-process R1-worker "
                "and one-process R2-worker evidence"
            )
        if time.monotonic() >= deadline:
            raise WorkloadError("D18 simultaneous three-role process witness timed out")
        time.sleep(0.05)


def _d18_verify_remote_rows(
    farm: FarmSpec,
    scenario: ScenarioSpec,
    plan: dict[str, Any],
    client: dict[str, Any],
    role: str,
    worker_name: str,
    factory: CommandFactory,
    transport: RecordingTransport,
) -> dict[str, Any]:
    script = r'''python3 - "$1" "$2" "$3" <<'PY'
import json, pathlib, sys
root, worker, expected = pathlib.Path(sys.argv[1]), sys.argv[2], int(sys.argv[3])
rows = []
for path in sorted(root.glob("*/result.tsv")):
    values = path.read_text(encoding="ascii").splitlines()
    if len(values) != 1 or len(values[0].split("\t")) != 14:
        raise SystemExit("malformed measured result row")
    row = values[0].split("\t")
    if row[5] != worker or row[8] != "0" or row[11] != "1" or row[12] != "1":
        raise SystemExit("role job was not exact remote work on its required F")
    rows.append({"job_id": row[4], "worker": row[5], "index": int(row[0])})
if len(rows) != expected:
    raise SystemExit(f"expected {expected} measured rows, observed {len(rows)}")
print(json.dumps({"jobs": rows, "count": len(rows)}, sort_keys=True))
PY'''
    expected = farm.data["corpora"][scenario.data["workload"]["corpus"]]["tus"]
    expected *= farm.data["corpora"][scenario.data["workload"]["corpus"]].get("repeat", 1)
    expected *= scenario.data["workload"]["repeat"]
    result = _d18_docker_call(
        farm, plan, client, factory, transport, f"verify-remote-{role}",
        ("/bin/bash", "-c", script, "d18-verify",
         "/results/workload/A/jobs", worker_name, str(expected)),
    )
    try:
        receipt = json.loads(result.stdout)
    except (TypeError, ValueError, json.JSONDecodeError) as exc:
        raise WorkloadError(f"D18 {role} remote-row receipt is malformed") from exc
    if not isinstance(receipt, dict) or receipt.get("count") != expected:
        raise WorkloadError(f"D18 {role} lacks complete exact remote-job evidence")
    return receipt


def _d18_verify_r2_link_adoption(
    farm: FarmSpec,
    plan: dict[str, Any],
    worker: dict[str, Any],
    factory: CommandFactory,
    transport: RecordingTransport,
) -> dict[str, Any]:
    result = _d18_docker_call(
        farm,
        plan,
        worker,
        factory,
        transport,
        "verify-r2-link-adoption",
        ("/bin/bash", "-c", "cat /var/log/icecream/iceccd.log"),
    )
    matches = [
        {"generation": int(generation), "attempt": int(attempt)}
        for generation, attempt in D18_R2_ADOPTION_RE.findall(result.stdout)
    ]
    if not matches:
        raise WorkloadError(
            "D18 R2 worker has no cache-link descriptor adoption evidence"
        )
    return {"adoptions": matches, "worker": worker["name"]}


def _parse_summary(result: CommandResult, client: str) -> dict[str, int | str]:
    if result.returncode != 0:
        detail = result.stderr.strip() or "no stderr"
        raise WorkloadError(
            f"client {client} workload command failed rc={result.returncode}: {detail}"
        )
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
    event_job_reader: JobReader | None = None,
    event_path: Path | None = None,
) -> dict[str, Any]:
    """Run each authenticated turn, preserving live daemon state between turns."""

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
    command_offset = len(transport.commands)
    totals = {
        client["name"]: {
            "client": client["name"],
            "failures": 0,
            "jobs": 0,
            "samples": 0,
        }
        for client in clients
    }
    turn_receipts: list[dict[str, Any]] = []
    turn_context: dict[str, Any] = {
        "turn": None,
        "futures": [],
        "commands": [],
        "ready": None,
    }
    turn_overrides: dict[str, list[CommandResult]] = {}
    d18_roles = scenario.data["workload"].get("d18_roles")
    checkpointed_client_events = [
        event
        for event in scenario.data.get("timeline", [])
        if event.get("trigger", "").startswith("job ")
        and event.get("action") in {"upgrade", "downgrade", "env_set"}
        and any(
            item.get("name") == event.get("instance") and item.get("role") == "C"
            for item in scenario.data["instances"]
        )
    ]
    if checkpointed_client_events and len(scenario.data["workload"]["turns"]) != 1:
        raise WorkloadError(
            "checkpointed C transitions require one unambiguous workload turn"
        )
    transition_waiters: dict[str, threading.Event] = (
        {scenario.data["workload"]["turns"][0]: threading.Event()}
        if checkpointed_client_events
        else {}
    )

    def _checkpoint_documents(
        turn: str, workload_clients: tuple[dict[str, Any], ...]
    ) -> dict[str, Any]:
        if turn_context["turn"] != turn:
            raise WorkloadError("checkpoint requested for an inactive workload turn")
        ready = turn_context["ready"]
        if not isinstance(ready, threading.Event) or not ready.wait(
            timeout=max(1.0, scenario.data["timeouts"]["turn_s"])
        ):
            raise WorkloadError("checkpoint requested before workload dispatch was reserved")
        if turn_context["turn"] != turn:
            raise WorkloadError("checkpoint workload turn changed during dispatch")
        for future in turn_context["futures"]:
            future.result()
        documents: dict[str, Any] = {}
        for client in workload_clients:
            container = f"icefarm-{plan['run_id']}-{client['name']}"
            command = factory.make(
                phase="event.checkpoint",
                host=client["host"],
                instance=client["name"],
                transport=_docker_transport(farm, client["host"]),
                timeout_s=scenario.data["timeouts"]["turn_s"],
                argv=docker_argv(
                    farm,
                    client["host"],
                    (
                        "exec",
                        "--user",
                        "65534:65534",
                        container,
                        "cat",
                        f"/results/workload/{turn}/checkpoint.json",
                    ),
                ),
            )
            result = transport.invoke(command)
            if result.returncode != 0:
                raise WorkloadError(f"client {client['name']} checkpoint read failed")
            try:
                documents[client["name"]] = json.loads(result.stdout)
            except (TypeError, ValueError, json.JSONDecodeError) as exc:
                raise WorkloadError(f"client {client['name']} checkpoint is malformed") from exc
        if turn not in transition_waiters:
            raise WorkloadError("checkpoint requested without a reserved transition turn")
        return documents

    def _relaunch_from_checkpoint(
        turn: str, checkpoints: dict[str, Any]
    ) -> dict[str, Any]:
        commands = [
            _driver_command(
                farm,
                scenario,
                plan,
                client,
                turn,
                factory,
                resume=True,
                checkpoint_sha256=checkpoints[client["name"]]["checkpoint_sha256"],
            )
            for client in clients
        ]
        with ThreadPoolExecutor(max_workers=len(commands)) as resume_executor:
            futures = [resume_executor.submit(transport.invoke, command) for command in commands]
            results = [future.result() for future in futures]
        evidence: dict[str, Any] = {}
        for client, result in zip(clients, results, strict=True):
            if result.returncode != 0:
                detail = result.stderr.strip() or "no stderr"
                raise WorkloadError(
                    f"client {client['name']} checkpoint relaunch failed "
                    f"rc={result.returncode}: {detail}"
                )
            summary = _parse_summary(result, client["name"])
            expected_jobs = int(checkpoints[client["name"]]["expected_jobs"])
            evidence[client["name"]] = {
                "client": client["name"],
                "expected_jobs": expected_jobs,
                "failures": int(summary["failures"]),
                "jobs": int(summary["jobs"]),
                "status": "COMPLETE",
            }
        turn_overrides[turn] = results
        if turn not in transition_waiters:
            raise WorkloadError("checkpoint relaunch has no reserved transition turn")
        transition_waiters[turn].set()
        return evidence

    # Validate all actions before dispatching a workload command.  The event
    # worker is kept under this function's ownership and is always joined.
    events = EventProducer(
        farm,
        scenario,
        plan,
        recorder=transport,
        factory=factory,
        job_reader=event_job_reader,
        event_path=event_path,
        deadline_s=scenario.data["timeouts"]["turn_s"]
        * len(scenario.data["workload"]["turns"]),
        quiesce_workload=_checkpoint_documents,
        relaunch_workload=_relaunch_from_checkpoint,
    )
    primary: BaseException | None = None
    try:
        events.start()
        for index, turn in enumerate(scenario.data["workload"]["turns"]):
            events.raise_if_failed()
            activation = None
            if index > 0:
                activation = activate_corpus_turn(
                    farm,
                    scenario,
                    plan,
                    turn,
                    transport,
                    factory,
                    timeout_s=scenario.data["timeouts"]["turn_s"],
                )
            events.signal_turn_start(turn)
            commands = [
                _driver_command(farm, scenario, plan, client, turn, factory)
                for client in clients
            ]
            executor = ThreadPoolExecutor(max_workers=len(commands))
            dispatch_ready = threading.Event()
            futures = []
            d18_evidence: dict[str, Any] | None = None
            try:
                turn_context.update(
                    {
                        "turn": turn,
                        "futures": futures,
                        "commands": commands,
                        "ready": dispatch_ready,
                    }
                )
                for command in commands:
                    futures.append(executor.submit(transport.invoke, command))
                dispatch_ready.set()
                if isinstance(d18_roles, dict):
                    d18_evidence = _d18_barrier_and_observe(
                        farm,
                        scenario,
                        plan,
                        clients,
                        futures,
                        factory,
                        transport,
                        scenario.data["timeouts"]["turn_s"],
                    )
                if _active_loss_serial_through(scenario):
                    events.prepare_active_compiler_boundary(turn)
                results = [future.result() for future in futures]
            finally:
                dispatch_ready.set()
                executor.shutdown(wait=True, cancel_futures=True)
            waiter = transition_waiters.get(turn)
            if waiter is not None:
                deadline = time.monotonic() + max(
                    1.0, scenario.data["timeouts"]["turn_s"]
                )
                while not waiter.wait(timeout=0.05):
                    events.raise_if_failed()
                    if time.monotonic() >= deadline:
                        raise WorkloadError(
                            "checkpointed C transition did not relaunch the workload"
                        )
                events.raise_if_failed()
            results = turn_overrides.pop(turn, results)
            turn_context.update(
                {"turn": None, "futures": [], "commands": [], "ready": None}
            )
            summaries = [
                _parse_summary(result, client["name"])
                for result, client in zip(results, clients, strict=True)
            ]
            for summary in summaries:
                total = totals[str(summary["client"])]
                for field in ("failures", "jobs", "samples"):
                    total[field] += int(summary[field])
            turn_receipt: dict[str, Any] = {
                "activation": activation,
                "clients": summaries,
                "turn": turn,
            }
            if isinstance(d18_roles, dict):
                if d18_evidence is None:
                    raise WorkloadError("D18 workload lacks a live overlap observation")
                role_jobs: dict[str, Any] = {}
                for role, client_name in d18_roles["clients"].items():
                    target_role = "R1" if role in ("P43", "R1") else "R2"
                    target_worker = d18_roles["workers"][target_role]
                    role_jobs[role] = _d18_verify_remote_rows(
                        farm,
                        scenario,
                        plan,
                        next(item for item in clients if item["name"] == client_name),
                        role,
                        target_worker,
                        factory,
                        transport,
                    )
                d18_evidence["measured_remote_jobs"] = role_jobs
                r2_worker = next(
                    item
                    for item in plan["topology"]["instances"]
                    if item["name"] == d18_roles["workers"]["R2"]
                )
                d18_evidence["r2_link_adoption"] = _d18_verify_r2_link_adoption(
                    farm, plan, r2_worker, factory, transport
                )
                turn_receipt["d18_concurrent_processes"] = d18_evidence
            turn_receipts.append(turn_receipt)
            events.signal_turn_complete(turn)
            events.raise_if_failed()
        # A successful workload cannot cancel still-pending timeline events.
        # Wait for every declared trigger/action to become terminal so a
        # missing job/turn trigger is a harness error rather than absent
        # evidence that silently looks like an empty timeline.
        events.wait()
    except (LifecycleError, RemoteError, EventError) as exc:
        try:
            events.raise_if_failed()
        except BaseException as event_exc:
            primary = WorkloadError(f"timeline failure: {event_exc}; workload failure: {exc}")
            primary.__cause__ = event_exc
        else:
            primary = WorkloadError(str(exc))
    except BaseException as exc:
        primary = exc
    finally:
        try:
            events.stop()
        except BaseException as event_exc:
            if primary is None:
                primary = event_exc
            else:
                combined = WorkloadError(
                    f"timeline failure: {event_exc}; workload failure: {primary}"
                )
                combined.__cause__ = event_exc
                primary = combined
    if primary is not None:
        raise primary
    summaries = [totals[client["name"]] for client in clients]
    commands = sorted(
        transport.commands[command_offset:], key=lambda command: command.sequence
    )
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
        "turns": turn_receipts,
    }
    _atomic_json(bundle_root(farm, plan["run_id"]) / "workload.json", receipt)
    return receipt
