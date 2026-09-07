from __future__ import annotations

import hashlib
import http.server
import json
import subprocess
import sys
import threading
import time
from pathlib import Path

import pytest

from farmharness.integration.tests import farm_fixture

from farmharness.integration import farmtest
from farmharness.integration.collect import CollectError, _event_log, _stage_evidence
from farmharness.integration.events import (
    CACHE_DISK_FAULT_BYTES,
    CACHE_DISK_FAULT_FILE,
    CACHE_DISK_FAULT_PATH,
    DISK_FILL_SCHEMA,
    ACTIVE_COMPILER_STOP_SCRIPT,
    ACTIVE_COMPILER_WAIT_SCRIPT,
    EventError,
    EventProducer,
    EventTimeout,
    CLIENT_ROUTE_SIGNAL_SCRIPT,
    CLIENT_TRANSITION_SCHEMA,
    GATE_CONTROL_SCRIPT,
    HEADER_EDIT_SCHEMA,
    TRANSITION_SCHEMA,
    TimelineEvent,
    UnsupportedEvent,
    scheduler_generation_for_job_text,
    select_direct_compiler_pairs,
    run_events,
)
from farmharness.integration.farm_spec import load_farm_spec
from farmharness.integration.images import RecordingTransport
from farmharness.integration.images import _image_identity
from farmharness.integration.lifecycle import _expected_image_labels
from farmharness.integration.layout import runtime_root
from farmharness.integration.remote import (
    CommandResult,
    PlannedCommand,
    decode_ssh_payload,
)
from farmharness.integration.scenario_spec import load_scenario_spec
from farmharness.integration.verdict import BUNDLE_SCHEMA, ROW_SCHEMA, evaluate_bundle
from farmharness.integration.workload import run_workload


INTEGRATION = Path(__file__).resolve().parents[1]


def _fixture(tmp_path: Path):
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(INTEGRATION / "scenarios" / "S00-smoke.json", farm)
    plan = farmtest.build_plan(farm, scenario, run_id="event-unit")
    return farm, scenario, plan


class EventRecorder:
    def __init__(self) -> None:
        self.commands: list[PlannedCommand] = []

    def invoke(self, command: PlannedCommand) -> CommandResult:
        self.commands.append(command)
        if command.phase == "event.authenticate":
            name = command.instance
            identifier = {"S1": "1" * 64, "C1": "2" * 64, "F1": "3" * 64}[name]
            return CommandResult(
                0,
                json.dumps(
                    {
                        "Id": identifier,
                        "Name": f"/icefarm-event-unit-{name}",
                        "State": {"Running": True},
                        "Mounts": [],
                        "Config": {
                            "Env": [],
                            "Labels": {
                                "icefarm.run": "event-unit",
                                "icefarm.instance": name,
                            }
                        },
                    }
                ),
                "",
            )
        return CommandResult(0, "", "")


def test_trigger_order_epochs_and_exact_authenticated_argv(tmp_path: Path) -> None:
    farm, scenario, plan = _fixture(tmp_path)
    scenario.data["timeline"] = [
        {"trigger": "t+0", "action": "kill -9", "instance": "F1"},
        {"trigger": "job 2", "action": "restart", "instance": "F1"},
        {"trigger": "after turn A", "action": "kill -9", "instance": "S1"},
    ]
    recorder = EventRecorder()
    wall = iter((1000, 1001, 1002))
    scheduler = iter(
        (
            "",
            "put 1 in joblist of F1\n"
            "put 2 in joblist of F1",
        )
    )
    producer = EventProducer(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(recorder),
        job_reader=lambda: next(scheduler),
        event_path=tmp_path / "events" / "events.json",
        deadline_s=2,
        poll_interval_s=0.01,
        wall_ms=lambda: next(wall),
    )
    producer.start()
    producer.signal_turn_complete("A")
    producer.wait()
    producer.stop()

    assert [item.event_index for item in producer.records] == [0, 1, 2]
    assert [item.event_epoch for item in producer.records] == [1, 2, 3]
    assert [item.fired_ms for item in producer.records] == [1000, 1001, 1002]
    assert [item.last_dispatched_job for item in producer.records] == [None, 2, 2]
    assert [item.workload_dispatch_count for item in producer.records] == [0, 2, 2]
    persisted = json.loads((tmp_path / "events" / "events.json").read_text())
    assert persisted["events"] == [item.as_dict() for item in producer.records]

    action = [item for item in recorder.commands if item.phase.startswith("event.kill")]
    assert action[0].instance == "F1"
    assert action[0].argv[-1] == "3" * 64
    restart = next(item for item in recorder.commands if item.phase == "event.restart")
    assert restart.argv[-5:] == ("container", "restart", "--time", "10", "3" * 64)
    inspect = next(item for item in recorder.commands if item.phase == "event.authenticate")
    assert inspect.argv[-1] == "icefarm-event-unit-F1"


def test_unsupported_actions_refuse_before_workload_and_deadline_is_bounded(tmp_path: Path) -> None:
    farm, scenario, plan = _fixture(tmp_path)
    scenario.data["timeline"] = [
        {"trigger": "t+0", "action": "disk_fill", "instance": "F1"}
    ]
    with pytest.raises(UnsupportedEvent, match="refusing before workload"):
        EventProducer(farm, scenario, plan, recorder=RecordingTransport(EventRecorder()))


def test_active_scheduler_loss_scripts_are_exact_identity_bound() -> None:
    assert "len(daemons) != 1" not in ACTIVE_COMPILER_STOP_SCRIPT
    assert "parents = {p[\"pid\"]" in ACTIVE_COMPILER_STOP_SCRIPT
    assert "parent[\"pid\"]" in ACTIVE_COMPILER_STOP_SCRIPT
    assert "len(candidates) != 1" in ACTIVE_COMPILER_STOP_SCRIPT
    assert "p[\"pid\"] == p[\"pgid\"]" in ACTIVE_COMPILER_STOP_SCRIPT
    assert '"--generation" not in p["argv"]' in ACTIVE_COMPILER_STOP_SCRIPT
    parent = {"pid": 10, "ppid": 1, "pgid": 10, "exe": "/opt/icecream/sbin/iceccd", "state": "S", "argv": []}
    compiler = {"pid": 11, "ppid": 10, "pgid": 11, "exe": "/opt/icecream/sbin/iceccd", "state": "R", "argv": []}
    other = compiler | {"pid": 12, "pgid": 12, "exe": "/usr/bin/other"}
    assert select_direct_compiler_pairs([parent, compiler]) == [(parent, compiler)]
    assert select_direct_compiler_pairs([parent, other]) == []


def test_scheduler_generation_parser_requires_framed_unique_generation() -> None:
    framed = (
        "[S1] 2026-09-07 00:00:00: ICECREAM scheduler x starting up, port 23000\n"
        "[S1] 2026-09-07 00:00:01: put 2 in joblist of F1\n"
    )
    assert scheduler_generation_for_job_text(framed, 2) == 1
    with pytest.raises(ValueError):
        scheduler_generation_for_job_text("ICECREAM scheduler x starting up, port 23000\nput 2 in joblist of F1", 2)
    reused = framed + (
        "[S1] 2026-09-07 00:01:00: ICECREAM scheduler x starting up, port 23000\n"
        "[S1] 2026-09-07 00:01:01: put 2 in joblist of F1\n"
    )
    with pytest.raises(ValueError):
        scheduler_generation_for_job_text(reused, 2)


def test_direct_compiler_selector_rejects_non_iceccd_group_child() -> None:
    parent = {"pid": 10, "ppid": 1, "pgid": 10, "exe": "/opt/icecream/sbin/iceccd", "state": "S", "argv": []}
    compiler = {"pid": 11, "ppid": 10, "pgid": 11, "exe": "/opt/icecream/sbin/iceccd", "state": "R", "argv": []}
    other = {"pid": 12, "ppid": 10, "pgid": 12, "exe": "/usr/bin/other", "state": "R", "argv": []}
    assert select_direct_compiler_pairs([parent, compiler]) == [(parent, compiler)]
    assert select_direct_compiler_pairs([parent, other]) == []


def test_assignment_script_executes_multi_child_http_join() -> None:
    from farmharness.integration.events import ACTIVE_COMPILER_ASSIGNMENT_SCRIPT
    payloads = {
        "/api/internals": "Child: pid=41 pgid=41 kind=0 gen=7 client=9 state=1\nChild: pid=42 pgid=42 kind=1 gen=7 client=10 state=1\n",
        "/api/clients": json.dumps({"type": "iceccd_clients", "ts": 1, "mono_msec": 1, "total": 1, "clients": [{"client_id": 9, "status": "WAITFORCHILD", "age_msec": 1, "why": "", "local_job": False, "local_job_kind": "", "local_reason": "", "cmdline": "", "scheduler_job_id": 12, "last_waitforcs_msec": 0, "env_bytes_received": 0, "job": {"job_id": 12, "target": "", "env": ""}, "usecs": None, "outfile": "", "channel": ""}]})
    }
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            body = payloads[self.path].encode()
            self.send_response(200)
            self.end_headers()
            self.wfile.write(body)
        def log_message(self, *_args):
            pass
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        port = server.server_address[1]
        result = subprocess.run(["python3", "-c", ACTIVE_COMPILER_ASSIGNMENT_SCRIPT, "41", "41", "7", "12", str(port)], capture_output=True, text=True, timeout=5)
        assert result.returncode == 0, result.stderr
        assert json.loads(result.stdout)["client"]["client_id"] == 9
        payloads["/api/internals"] += "Child: pid=41 pgid=41 kind=0 gen=7 client=11 state=1\n"
        bad = subprocess.run(["python3", "-c", ACTIVE_COMPILER_ASSIGNMENT_SCRIPT, "41", "41", "7", "12", str(port)], capture_output=True, text=True, timeout=5)
        assert bad.returncode != 0
    finally:
        server.shutdown()
        thread.join(timeout=5)
        server.server_close()
    assert "before[\"start_ticks\"]" in ACTIVE_COMPILER_STOP_SCRIPT
    assert "os.killpg(before[\"pgid\"], signal.SIGSTOP)" in ACTIVE_COMPILER_STOP_SCRIPT
    assert "compiler PID was reused" in ACTIVE_COMPILER_WAIT_SCRIPT
    assert "value[0] == pgid" in ACTIVE_COMPILER_WAIT_SCRIPT


def test_active_compiler_selector_rejects_sidecar_and_statewriter_shapes() -> None:
    daemon = {"pid": 10, "pgid": 10, "ppid": 1, "exe": "/opt/icecream/sbin/iceccd", "argv": []}
    statewriter = {"pid": 11, "pgid": 10, "ppid": 10, "exe": daemon["exe"], "argv": []}
    sidecar = {"pid": 12, "pgid": 12, "ppid": 10, "exe": daemon["exe"], "argv": ["iceccd", "--generation", "7"]}
    compiler = {"pid": 13, "pgid": 13, "ppid": 10, "exe": daemon["exe"], "argv": ["iceccd"]}
    parents = {daemon["pid"]: daemon}
    pairs = [
        (parent, child) for child in (statewriter, sidecar, compiler)
        for parent in (parents.get(child["ppid"]),)
        if parent is not None and child["pid"] == child["pgid"]
        and "--generation" not in child["argv"]
    ]
    assert pairs == [(daemon, compiler)]


def test_active_scheduler_loss_scenario_is_not_the_drained_restart(tmp_path: Path) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S70-b4-scheduler-active-loss.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="active-loss-unit")
    producer = EventProducer(farm, scenario, plan, recorder=RecordingTransport(EventRecorder()))
    assert producer.events[0].action == "scheduler-loss-active"
    assert producer.events[0].trigger.kind == "job"


def test_active_scheduler_loss_collection_binds_post_offset_product_witness(
    tmp_path: Path,
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S70-b4-scheduler-active-loss.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="active-loss-collect")
    log = tmp_path / "diagnostics" / "tt-quietbox3" / "F1.log" / "iceccd.log"
    log.parent.mkdir(parents=True)
    log.write_text(
        "session quiescence TERM compiler pid=41 pgid=41 generation=7\n"
        "session quiescence KILL compiler pid=41 pgid=41 generation=7\n"
        "session quiescence settled compiler pid=41 pgid=41 generation=7\n"
    )
    receipt = {
        "action": "scheduler-loss-active",
        "after": {"container_id": "a" * 64, "started_at": "new"},
        "before": {"container_id": "a" * 64, "started_at": "old"},
            "compiler": {
                "container_id": "b" * 64,
                "assignment": {"schema": "icefarm-compiler-assignment-v1", "child": {"pid": 41, "pgid": 41, "generation": 1, "kind": 0, "owning_client_id": 7}, "client": {"client_id": 7, "scheduler_job_id": 2, "job_id": 2}, "listener": {"host": "127.0.0.1", "port": 8765}},
            "daemon": {"pid": 10, "pgid": 10, "ppid": 1, "exe": "/opt/icecream/sbin/iceccd"},
            "leader": {"pid": 41, "pgid": 41, "ppid": 10, "start_ticks": 9, "state": "R"},
            "stopped": {"pid": 41, "pgid": 41, "ppid": 10, "start_ticks": 9, "state": "T"},
            "group_gone": {"gone": True},
            "worker_before": {"container_id": "b" * 64, "started_at": "f"},
            "worker_after": {"container_id": "b" * 64, "started_at": "f"},
        },
            "event_epoch": 1,
            "lost_scheduler_generation": 1,
            "instance": "S1",
        "lost_scheduler_job": 2,
        "pre_fault": {"scheduler_log": {"offset": 0}, "worker_log": {"offset": 0}},
        "quiescence": {
                "client_readiness": {"C1": {"bytes": 1, "cache_line": None, "cache_required": False, "connected_line": "Connected to scheduler (I am known as C1)", "host": "tt-quietbox2", "log_path": "/scratch/C1/log/client-daemon.log", "offset": 0}},
                "client_routes": {"C1": {"before": {"container": {"container_id": "c" * 64, "started_at": "same", "running": True}, "daemon": {"argv": ["/opt/icecream/sbin/iceccd"], "exe": "/opt/icecream/sbin/iceccd", "exe_evidence": "proc-exe", "pid": 10, "ppid": 1, "start_ticks": 1, "uid": 0}, "route_owner": {"argv": ["/opt/icecream/sbin/icecc-cache-service"], "exe": "/opt/icecream/sbin/icecc-cache-service", "exe_evidence": "proc-exe", "pid": 11, "ppid": 10, "start_ticks": 2, "uid": 0}}, "after": {"container": {"container_id": "c" * 64, "started_at": "same", "running": True}, "daemon": {"argv": ["/opt/icecream/sbin/iceccd"], "exe": "/opt/icecream/sbin/iceccd", "exe_evidence": "proc-exe", "pid": 10, "ppid": 1, "start_ticks": 1, "uid": 0}, "route_owner": {"argv": ["/opt/icecream/sbin/icecc-cache-service"], "exe": "/opt/icecream/sbin/icecc-cache-service", "exe_evidence": "proc-exe", "pid": 11, "ppid": 10, "start_ticks": 2, "uid": 0}}}},
            "scheduler_snapshot": "S1",
            "scheduler_startup": {"line": "ICECREAM scheduler starting"},
            "worker_snapshot": "F1",
        },
        "schema": "icefarm-scheduler-active-loss-v1",
        "turn": "A",
    }
    event = {
        "action": "scheduler-loss-active", "event_epoch": 1, "event_index": 0,
        "fired_ms": 10, "instance": "S1", "last_dispatched_job": 2,
        "trigger": "job 2", "workload_dispatch_count": 2, "receipt": receipt,
    }
    (tmp_path / "events").mkdir()
    (tmp_path / "events" / "events.json").write_text(json.dumps({"events": [event]}))
    assert _event_log(tmp_path, scenario, farm=farm, plan=plan) == [event]
    log.write_text(log.read_text().replace("pgid=41", "pgid=42"))
    with pytest.raises(CollectError, match="post-offset F TERM witness"):
        _event_log(tmp_path, scenario, farm=farm, plan=plan)

    scenario.data["timeline"] = [
        {"trigger": "t+2", "action": "restart", "instance": "F1"}
    ]
    with pytest.raises(EventTimeout):
        EventProducer(
            farm,
            scenario,
            plan,
            recorder=RecordingTransport(EventRecorder()),
            deadline_s=1,
        )


def _disk_fill_operation() -> dict[str, object]:
    return {
        "available_after": 0,
        "available_before": CACHE_DISK_FAULT_BYTES,
        "directory_gid": 65534,
        "directory_mode": 0o700,
        "directory_uid": 65534,
        "elapsed_ms": 10,
        "errno": 28,
        "filler_bytes": CACHE_DISK_FAULT_BYTES - 4096,
        "filler_path": CACHE_DISK_FAULT_FILE,
        "limit_bytes": CACHE_DISK_FAULT_BYTES,
        "minimum_headroom_bytes": 8 * 1024 * 1024,
        "schema": "icefarm-disk-fill-operation-v1",
        "watchdog_s": 30,
    }


class DiskFillRecorder(EventRecorder):
    def invoke(self, command: PlannedCommand) -> CommandResult:
        self.commands.append(command)
        if command.phase == "event.authenticate":
            return CommandResult(
                0,
                json.dumps(
                    {
                        "Id": "3" * 64,
                        "Image": "4" * 64,
                        "Name": "/icefarm-event-unit-F1",
                        "State": {
                            "Running": True,
                            "StartedAt": "2026-09-06T12:00:00Z",
                        },
                        "Mounts": [
                            {
                                "Destination": "/opt/icecream",
                                "RW": False,
                                "Source": "/runtime/" + ("b8aa267a40f2beb17bbb53de165ab1e744b13ddc2e2fe39eee1486d57947cb0a"),
                                "Type": "bind",
                            },
                            {
                                "Destination": CACHE_DISK_FAULT_PATH,
                                "RW": True,
                                "Source": "",
                                "Type": "tmpfs",
                            }
                        ],
                        "HostConfig": {
                            "Mounts": [
                                {
                                    "Target": CACHE_DISK_FAULT_PATH,
                                    "TmpfsOptions": {
                                        "Mode": 0o700,
                                        "SizeBytes": CACHE_DISK_FAULT_BYTES,
                                    },
                                    "Type": "tmpfs",
                                }
                            ]
                        },
                        "Config": {
                            "Env": [],
                            "Labels": {
                                "icefarm.run": "event-unit",
                                "icefarm.instance": "F1",
                            },
                        },
                    }
                ),
                "",
            )
        if command.phase == "event.disk-fill":
            return CommandResult(0, json.dumps(_disk_fill_operation()), "")
        return CommandResult(0, "", "")


def test_disk_fill_uses_only_the_fixed_bounded_cache_tmpfs_and_records_enospc(
    tmp_path: Path,
) -> None:
    farm, scenario, _plan = _fixture(tmp_path)
    scenario.data["timeline"] = [
        {"trigger": "t+0", "action": "disk_fill", "instance": "F1"}
    ]
    plan = farmtest.build_plan(farm, scenario, run_id="event-unit")
    recorder = DiskFillRecorder()
    path = tmp_path / "events" / "events.json"
    producer = EventProducer(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(recorder),
        event_path=path,
        deadline_s=2,
        poll_interval_s=0.01,
        wall_ms=lambda: 1000,
    )

    producer.start()
    producer.wait()
    producer.stop()

    assert len(producer.records) == 1
    receipt = producer.records[0].receipt
    assert receipt is not None
    assert receipt["schema"] == DISK_FILL_SCHEMA
    assert receipt["before"] == receipt["after"]
    assert receipt["fill"] == _disk_fill_operation()
    command = next(item for item in recorder.commands if item.phase == "event.disk-fill")
    assert command.argv[-5:] == (
        CACHE_DISK_FAULT_PATH,
        CACHE_DISK_FAULT_FILE,
        str(CACHE_DISK_FAULT_BYTES),
        "30",
        str(8 * 1024 * 1024),
    )
    start = next(
        item
        for item in plan["commands"]
        if item["phase"] == "up.start-f" and item["instance"] == "F1"
    )
    assert (
        "type=tmpfs,"
        f"dst={CACHE_DISK_FAULT_PATH},"
        f"tmpfs-size={CACHE_DISK_FAULT_BYTES},tmpfs-mode=0700"
    ) in start["argv"]
    assert _event_log(tmp_path, scenario, farm=farm, plan=plan) == [
        producer.records[0].as_dict()
    ]
    forged = json.loads(path.read_text(encoding="utf-8"))
    forged["events"][0]["receipt"]["fill"]["errno"] = 0
    path.write_text(json.dumps(forged), encoding="utf-8")
    with pytest.raises(CollectError, match="does not prove bounded ENOSPC"):
        _event_log(tmp_path, scenario, farm=farm, plan=plan)


@pytest.mark.parametrize("action", ("upgrade", "downgrade"))
def test_c_image_transition_refuses_before_workload(tmp_path: Path, action: str) -> None:
    farm, scenario, plan = _fixture(tmp_path)
    client = next(item for item in scenario.data["instances"] if item["name"] == "C1")
    client["image"] = "new" if action == "downgrade" else "old"
    scenario.data["timeline"] = [
        {
            "trigger": "job 1",
            "action": action,
            "instance": "C1",
            "image": "old" if action == "downgrade" else "new",
        }
    ]
    with pytest.raises(UnsupportedEvent, match="checkpointed workload driver"):
        EventProducer(farm, scenario, plan, recorder=RecordingTransport(EventRecorder()))


def test_header_edit_refuses_ignored_controls_before_workload(tmp_path: Path) -> None:
    farm, scenario, plan = _fixture(tmp_path)
    scenario.data["timeline"] = [
        {
            "trigger": "job 1",
            "action": "header_edit",
            "instance": "F1",
            "path": "/usr/include/stdio.h",
            "rate": "1mbit",
        }
    ]
    with pytest.raises(EventError, match="exactly the path field"):
        EventProducer(farm, scenario, plan, recorder=RecordingTransport(EventRecorder()))


class HeaderEditRecorder(EventRecorder):
    def __init__(self, plan) -> None:
        super().__init__()
        self.plan = plan
        self.authenticate_count = 0

    def invoke(self, command: PlannedCommand) -> CommandResult:
        self.commands.append(command)
        if command.phase == "event.authenticate":
            self.authenticate_count += 1
            name = command.instance or "?"
            identifier = "3" * 64
            started = "2026-09-05T00:00:00Z" if self.authenticate_count < 3 else "2026-09-05T00:01:00Z"
            return CommandResult(
                0,
                json.dumps(
                    {
                        "Id": identifier,
                        "Name": f"/icefarm-event-unit-{name}",
                        "State": {"Running": True, "StartedAt": started},
                        "Mounts": [],
                        "Config": {
                            "Env": [],
                            "Labels": {
                                "icefarm.run": "event-unit",
                                "icefarm.instance": name,
                            },
                        },
                    }
                ),
                "",
            )
        if command.phase in {"event.pause-drain", "event.resume"}:
            action = "pause" if command.phase == "event.pause-drain" else "resume"
            client = command.instance
            return CommandResult(
                0,
                json.dumps(
                    {
                        "action": action,
                        "active_after": 0,
                        "active_before": 1 if action == "pause" else 0,
                        "client": client,
                        "epoch": 1,
                        "finished_ms": 100 if action == "pause" else 1001,
                        "schema": "icefarm-event-gate-v1",
                        "started_ms": 99 if action == "pause" else 1000,
                        "status": "PAUSED" if action == "pause" else "OPEN",
                        "turn": "A",
                    }
                ),
                "",
            )
        if command.phase == "event.readiness-baseline":
            if command.instance == "S1":
                return CommandResult(
                    0,
                    "10 /home/mickg10/farm-scratch/icefarm/event-unit/S1/log/scheduler.log\n",
                    "",
                )
            return CommandResult(
                0,
                "10 /home/mickg/farm-scratch/icefarm/event-unit/F2/log/iceccd.log\n",
                "",
            )
        if command.phase == "event.header-scheduler-rejoin":
            return CommandResult(
                0,
                json.dumps(
                    {
                        "bytes": 100,
                        "cache_line": (
                            "[1] 2026-09-05 01:02:00: RELOGIN F2(x86_64): "
                            "cache=10.0.27.56:24002 cache_wire=v1 "
                            "cache_protocol=1 cache_profiles=p29v1 zstd_tu zstd_route"
                        ),
                        "cache_protocol": 1,
                        "login_line": (
                            "[1] 2026-09-05 01:02:00: login F2 protocol version: 50"
                        ),
                        "profile": "p29v1",
                        "role_protocol": 50,
                        "ready": True,
                        "sha256": "a" * 64,
                        "target": "F2",
                    }
                ),
                "",
            )
        if command.phase == "event.header-edit":
            return CommandResult(
                0,
                json.dumps(
                    {
                        "after_cache": [],
                        "after_sha256": "b" * 64,
                        "before_cache": [
                            "p29-system-source-fingerprint-v1.cache",
                            "p29-system-source-fingerprint-v1.lock",
                        ],
                        "before_sha256": "a" * 64,
                        "cache_directory": "/var/cache/icecream/p50-runtime",
                        "header_path": "/usr/include/stdio.h",
                        "removed_cache": [
                            "p29-system-source-fingerprint-v1.cache",
                            "p29-system-source-fingerprint-v1.lock",
                        ],
                    }
                ),
                "",
            )
        if command.phase == "event.header-readiness":
            return CommandResult(
                0,
                json.dumps(
                    {
                        "bytes": 100,
                        "cache_line": "cache sidecar adapter state=2 lifecycle=3",
                        "line": "ICECREAM daemon F2 starting up",
                        "ready": True,
                    }
                ),
                "",
            )
        return CommandResult(0, "", "")


def test_header_edit_is_scoped_drained_and_rejoined_with_fingerprint_receipt(
    tmp_path: Path,
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S40-full-newgen-engagement.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="event-unit")
    recorder = HeaderEditRecorder(plan)
    scheduler = iter(
        (
            "put 1 in joblist of F2",
            "\n".join(f"put {index} in joblist of F2" for index in range(1, 26)),
        )
    )
    last_scheduler = ""

    def read_scheduler() -> str:
        nonlocal last_scheduler
        try:
            last_scheduler = next(scheduler)
        except StopIteration:
            pass
        return last_scheduler

    producer = EventProducer(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(recorder),
        job_reader=read_scheduler,
        event_path=tmp_path / "events" / "events.json",
        deadline_s=2,
        poll_interval_s=0.01,
        wall_ms=iter((1000, 2000)).__next__,
    )
    producer.signal_turn_start("A")
    producer.start()
    producer.wait()
    producer.stop()

    assert len(producer.records) == 1
    receipt = producer.records[0].receipt
    assert receipt is not None
    assert receipt["schema"] == HEADER_EDIT_SCHEMA
    assert receipt["before"]["header_sha256"] != receipt["after"]["header_sha256"]
    assert receipt["before"]["container_id"] == receipt["after"]["container_id"]
    assert receipt["before"]["p29_cache_files"] == [
        "p29-system-source-fingerprint-v1.cache",
        "p29-system-source-fingerprint-v1.lock",
    ]
    assert receipt["after"]["p29_cache_files"] == []
    assert receipt["cache_invalidation"]["removed"] == receipt["before"]["p29_cache_files"]
    assert receipt["coordination"]["scheduler_rejoin"]["target"] == "F2"
    assert receipt["coordination"]["scheduler_rejoin"]["profile"] == "P29V1"
    assert producer.records[0].fired_ms == receipt["coordination"]["ready_ms"] == 1000
    assert set(receipt["coordination"]["clients"]) == {"C1"}
    assert [item.phase for item in recorder.commands if item.phase.startswith("event.header")]
    edit = next(item for item in recorder.commands if item.phase == "event.header-edit")
    restart = next(item for item in recorder.commands if item.phase == "event.header-edit.restart")
    edit_argv = decode_ssh_payload(edit.argv) if edit.argv[0] == "ssh" else edit.argv
    assert "/usr/include/stdio.h" in edit_argv
    assert "3" * 64 in edit_argv
    assert "event-1" in edit_argv
    assert edit.instance == restart.instance == "F2"
    restart_argv = decode_ssh_payload(restart.argv) if restart.argv[0] == "ssh" else restart.argv
    assert restart_argv[-1] == "3" * 64
    assert _event_log(tmp_path, scenario, farm=farm, plan=plan)[0]["receipt"] == receipt
    event_path = tmp_path / "events" / "events.json"
    tampered = json.loads(event_path.read_text())
    tampered["events"][0]["receipt"]["cache_invalidation"]["removed"] = []
    event_path.write_text(json.dumps(tampered), encoding="utf-8")
    with pytest.raises(CollectError):
        _event_log(tmp_path, scenario, farm=farm, plan=plan)

    tampered = json.loads((tmp_path / "events" / "events.json").read_text())
    tampered["events"][0]["receipt"]["before"]["p29_cache_files"] = []
    (tmp_path / "events" / "events.json").write_text(
        json.dumps(tampered), encoding="utf-8"
    )
    with pytest.raises(CollectError):
        _event_log(tmp_path, scenario, farm=farm, plan=plan)


def test_event_command_failure_propagates_and_leaves_no_worker(tmp_path: Path) -> None:
    farm, scenario, plan = _fixture(tmp_path)
    scenario.data["timeline"] = [
        {"trigger": "t+0", "action": "restart", "instance": "F1"}
    ]

    class FailingRecorder(EventRecorder):
        def invoke(self, command: PlannedCommand) -> CommandResult:
            self.commands.append(command)
            if command.phase == "event.restart":
                return CommandResult(17, "", "restart refused")
            return super().invoke(command)

    recorder = FailingRecorder()
    with pytest.raises(EventError, match="rc=17"):
        run_events(
            farm,
            scenario,
            plan,
            recorder=RecordingTransport(recorder),
            event_path=tmp_path / "events" / "events.json",
            deadline_s=2,
        )
    assert not any(thread.name == "icefarm-events" and thread.is_alive() for thread in threading.enumerate())


def test_event_gate_pause_drains_then_resume_is_atomic(tmp_path: Path) -> None:
    root = tmp_path / "workload" / "A"
    gate = root / "event-gate"
    active = gate / "active"
    active.mkdir(parents=True)
    (gate / "state.tsv").write_text("OPEN\t0\n", encoding="ascii")
    (gate / "state.lock").touch()
    marker = active / "job-1-99.tsv"
    marker.write_text("1\t99\t0\t1\n", encoding="ascii")

    def remove_after_pause() -> None:
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            if (gate / "state.tsv").read_text(encoding="ascii") == "PAUSE\t1\n":
                marker.unlink()
                return
            time.sleep(0.005)
        raise AssertionError("event gate did not enter PAUSE")

    remover = threading.Thread(target=remove_after_pause)
    remover.start()
    paused = subprocess.run(
        (
            sys.executable,
            "-c",
            GATE_CONTROL_SCRIPT,
            "pause",
            str(root),
            "C1",
            "A",
            "1",
            "2",
        ),
        check=True,
        capture_output=True,
        text=True,
        timeout=3,
    )
    remover.join(timeout=1)
    pause_receipt = json.loads(paused.stdout)
    assert pause_receipt["status"] == "PAUSED"
    assert pause_receipt["active_before"] == 1
    assert pause_receipt["active_after"] == 0
    assert (gate / "state.tsv").read_text() == "PAUSE\t1\n"

    resumed = subprocess.run(
        (
            sys.executable,
            "-c",
            GATE_CONTROL_SCRIPT,
            "resume",
            str(root),
            "C1",
            "A",
            "1",
            "2",
        ),
        check=True,
        capture_output=True,
        text=True,
        timeout=3,
    )
    assert json.loads(resumed.stdout)["status"] == "OPEN"
    assert (gate / "state.tsv").read_text() == "OPEN\t1\n"

    subprocess.run(
        (
            sys.executable,
            "-c",
            GATE_CONTROL_SCRIPT,
            "pause",
            str(root),
            "C1",
            "A",
            "2",
            "2",
        ),
        check=True,
        capture_output=True,
        text=True,
        timeout=3,
    )
    aborted = subprocess.run(
        (
            sys.executable,
            "-c",
            GATE_CONTROL_SCRIPT,
            "abort",
            str(root),
            "C1",
            "A",
            "2",
            "2",
        ),
        check=True,
        capture_output=True,
        text=True,
        timeout=3,
    )
    assert json.loads(aborted.stdout)["status"] == "ABORT"
    assert (gate / "state.tsv").read_text() == "ABORT\t2\n"


def test_event_gate_treats_marker_vanishing_after_enumeration_as_drained(
    tmp_path: Path,
) -> None:
    root = tmp_path / "workload" / "A"
    gate = root / "event-gate"
    active = gate / "active"
    active.mkdir(parents=True)
    (gate / "state.tsv").write_text("OPEN\t0\n", encoding="ascii")
    (gate / "state.lock").touch()
    marker = active / "job-1-99.tsv"
    marker.write_text("1\t99\t0\t1\n", encoding="ascii")

    racing_glob = r'''
import pathlib

_original_glob = pathlib.Path.glob
_glob_calls = 0

def _remove_after_enumeration(path, pattern):
    global _glob_calls
    paths = list(_original_glob(path, pattern))
    _glob_calls += 1
    if _glob_calls == 2:
        assert [candidate.name for candidate in paths] == ["job-1-99.tsv"]
        paths[0].unlink()
    return iter(paths)

pathlib.Path.glob = _remove_after_enumeration
'''.strip()
    paused = subprocess.run(
        (
            sys.executable,
            "-c",
            racing_glob + "\n" + GATE_CONTROL_SCRIPT,
            "pause",
            str(root),
            "C1",
            "A",
            "1",
            "2",
        ),
        check=True,
        capture_output=True,
        text=True,
        timeout=3,
    )

    receipt = json.loads(paused.stdout)
    assert receipt["status"] == "PAUSED"
    assert receipt["active_before"] == 1
    assert receipt["active_after"] == 0
    assert not marker.exists()
    assert (gate / "state.tsv").read_text() == "PAUSE\t1\n"


@pytest.mark.parametrize(
    ("marker_kind", "marker_name", "error"),
    (
        ("malformed", "job-0-2.tsv", "malformed event-gate marker"),
        ("symlink", "job-1-2.tsv", "unsafe event-gate file"),
        ("directory", "job-1-2.tsv", "unsafe event-gate file"),
    ),
)
def test_event_gate_rejects_unsafe_or_malformed_active_markers(
    tmp_path: Path,
    marker_kind: str,
    marker_name: str,
    error: str,
) -> None:
    root = tmp_path / "workload" / "A"
    gate = root / "event-gate"
    active = gate / "active"
    active.mkdir(parents=True)
    (gate / "state.tsv").write_text("OPEN\t0\n", encoding="ascii")
    (gate / "state.lock").touch()
    marker = active / marker_name
    if marker_kind == "symlink":
        target = active / "target.tsv"
        target.write_text("1\t2\t0\t1\n", encoding="ascii")
        marker.symlink_to(target)
    elif marker_kind == "directory":
        marker.mkdir()
    else:
        marker.write_text("0\t2\t0\t1\n", encoding="ascii")

    rejected = subprocess.run(
        (
            sys.executable,
            "-c",
            GATE_CONTROL_SCRIPT,
            "pause",
            str(root),
            "C1",
            "A",
            "1",
            "2",
        ),
        check=False,
        capture_output=True,
        text=True,
        timeout=3,
    )

    assert rejected.returncode != 0
    assert error in rejected.stderr
    assert (gate / "state.tsv").read_text() == "OPEN\t0\n"


def test_scheduler_restart_pauses_drains_reauthenticates_and_resumes(tmp_path: Path) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S20-scheduler-first.json", farm
    )
    scenario.data["timeline"] = [
        {"trigger": "job 1", "action": "restart", "instance": "S1"}
    ]
    plan = farmtest.build_plan(farm, scenario, run_id="event-unit")

    class SchedulerRestartRecorder(EventRecorder):
        def __init__(self) -> None:
            super().__init__()
            self.restarted = False

        @staticmethod
        def gate_receipt(command: PlannedCommand, action: str) -> CommandResult:
            started = 1000 if action == "pause" else 2001
            return CommandResult(
                0,
                json.dumps(
                    {
                        "action": action,
                        "active_after": 0,
                        "active_before": 24 if action == "pause" else 0,
                        "client": command.instance,
                        "epoch": 1,
                        "finished_ms": started + 1,
                        "schema": "icefarm-event-gate-v1",
                        "started_ms": started,
                        "status": "PAUSED" if action == "pause" else "OPEN",
                        "turn": "A",
                    }
                ),
                "",
            )

        def invoke(self, command: PlannedCommand) -> CommandResult:
            self.commands.append(command)
            if command.phase == "event.authenticate":
                name = command.instance
                identifier = {"S1": "1" * 64, "C1": "2" * 64, "F1": "3" * 64}[name]
                return CommandResult(
                    0,
                    json.dumps(
                        {
                            "Id": identifier,
                            "Name": f"/icefarm-event-unit-{name}",
                            "State": {
                                "Running": True,
                                "StartedAt": "2026-09-05T13:03:33Z"
                                if self.restarted
                                else "2026-09-05T12:59:50Z",
                            },
                            "Mounts": [],
                            "Config": {
                                "Env": [],
                                "Labels": {
                                    "icefarm.run": "event-unit",
                                    "icefarm.instance": name,
                                },
                            },
                        }
                    ),
                    "",
                )
            if command.phase == "event.pause-drain":
                return self.gate_receipt(command, "pause")
            if command.phase == "event.restart":
                self.restarted = True
                return CommandResult(0, "", "")
            if command.phase == "event.readiness-baseline":
                return CommandResult(0, "17 scheduler.log\n", "")
            if command.phase == "event.readiness":
                return CommandResult(
                    0,
                    json.dumps(
                        {
                            "bytes": 80,
                            "line": "ICECREAM scheduler 1.4 starting up, port 23000",
                            "ready": True,
                        }
                    ),
                    "",
                )
            if command.phase == "event.client-scheduler-ready":
                return CommandResult(
                    0,
                    json.dumps(
                        {
                            "bytes": 80,
                            "cache_line": None,
                            "cache_required": False,
                            "connected_line": (
                                "[1] Connected to scheduler (I am known as 10.0.0.1)"
                            ),
                            "ready": True,
                        }
                    ),
                    "",
                )
            if command.phase == "readiness.container":
                return CommandResult(0, json.dumps({"Running": True}), "")
            if command.phase == "readiness.listcs":
                return CommandResult(0, "F1 10.0.27.127 24\n", "")
            if command.phase == "event.resume":
                return self.gate_receipt(command, "resume")
            return CommandResult(0, "", "")

    recorder = SchedulerRestartRecorder()
    samples = iter(("", "put 7 in joblist of F1\n"))
    producer = EventProducer(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(recorder),
        job_reader=lambda: next(samples),
        event_path=tmp_path / "events" / "events.json",
        deadline_s=2,
        poll_interval_s=0.01,
        wall_ms=lambda: 2000,
    )
    producer.signal_turn_start("A")
    producer.start()
    producer.wait()
    producer.signal_turn_complete("A")
    producer.stop()

    phases = [command.phase for command in recorder.commands]
    assert phases.index("event.pause-drain") < phases.index("event.restart")
    assert phases.index("event.restart") < phases.index("readiness.listcs")
    assert phases.index("readiness.listcs") < phases.index("event.client-scheduler-ready")
    assert phases.index("event.client-scheduler-ready") < phases.index("event.resume")
    record = producer.records[0].as_dict()
    assert record["fired_ms"] == 2000
    assert record["receipt"]["coordination"]["workers"] == ["F1"]
    assert _event_log(
        tmp_path, scenario, farm=farm, plan=plan
    )[0]["receipt"] == record["receipt"]

    document = json.loads((tmp_path / "events" / "events.json").read_text())
    document["events"][0]["receipt"]["coordination"]["clients"]["C1"][
        "active_after"
    ] = 1
    (tmp_path / "events" / "events.json").write_text(json.dumps(document))
    with pytest.raises(CollectError, match="before active jobs drained"):
        _event_log(tmp_path, scenario, farm=farm, plan=plan)


def test_scheduler_restart_failure_aborts_every_paused_client(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S20-scheduler-first.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="event-unit")
    producer = EventProducer(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(EventRecorder()),
        deadline_s=2,
    )
    producer.signal_turn_start("A")
    producer._start = producer.monotonic()
    actions: list[str] = []

    monkeypatch.setattr(
        producer,
        "_inspect",
        lambda _name: {
            "id": "1" * 64,
            "running": True,
            "started_at": "2026-09-05T12:59:50Z",
        },
    )

    def gate_receipt(_client, *, action, turn, epoch):
        actions.append(action)
        return {
            "action": action,
            "active_after": 0,
            "active_before": 0,
            "client": "C1",
            "epoch": epoch,
            "finished_ms": 1,
            "schema": "icefarm-event-gate-v1",
            "started_ms": 1,
            "status": {"pause": "PAUSED", "abort": "ABORT"}[action],
            "turn": turn,
        }

    def gate_controls(clients, *, action, turn, epoch, timeout_s, receipts):
        del timeout_s
        for client in clients:
            receipts[client["name"]] = gate_receipt(
                client, action=action, turn=turn, epoch=epoch
            )

    monkeypatch.setattr(producer, "_gate_controls", gate_controls)
    monkeypatch.setattr(
        producer,
        "_gate_control",
        lambda client, *, action, turn, epoch, timeout_s: gate_receipt(
            client, action=action, turn=turn, epoch=epoch
        ),
    )
    monkeypatch.setattr(
        producer,
        "_readiness_baseline",
        lambda _instance: {"host": "tt-quietbox3", "path": "/x", "offset": 0},
    )
    monkeypatch.setattr(
        producer,
        "_invoke",
        lambda _command: (_ for _ in ()).throw(EventError("restart refused")),
    )
    event = TimelineEvent.from_dict(
        0, {"trigger": "job 100", "action": "restart", "instance": "S1"}
    )
    scheduler = next(
        item for item in plan["topology"]["instances"] if item["role"] == "S"
    )
    with pytest.raises(EventError, match="restart refused"):
        producer._coordinated_scheduler_restart(event, scheduler, "1" * 64)
    assert actions == ["pause", "abort"]


def test_multi_client_gate_starts_every_pause_before_any_drain_completes(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _fixture(tmp_path)
    delegate = EventRecorder()
    started = {"C1": threading.Event(), "C2": threading.Event()}

    class ConcurrentGateRecorder:
        def invoke(self, command: PlannedCommand) -> CommandResult:
            if command.phase != "event.pause-drain":
                return delegate.invoke(command)
            delegate.commands.append(command)
            assert command.instance in started
            other = "C2" if command.instance == "C1" else "C1"
            started[command.instance].set()
            assert started[other].wait(timeout=1)
            receipt = {
                "action": "pause",
                "active_after": 0,
                "active_before": 48,
                "client": command.instance,
                "epoch": 1,
                "finished_ms": 101,
                "schema": "icefarm-event-gate-v1",
                "started_ms": 100,
                "status": "PAUSED",
                "turn": "A",
            }
            return CommandResult(0, json.dumps(receipt), "")

    producer = EventProducer(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(ConcurrentGateRecorder()),
        deadline_s=2,
    )
    c1 = next(
        item for item in plan["topology"]["instances"] if item["name"] == "C1"
    )
    c2 = dict(c1, name="C2", host="tt-quietbox3")
    receipts: dict[str, dict[str, object]] = {}
    producer._gate_controls(
        (c1, c2),
        action="pause",
        turn="A",
        epoch=1,
        timeout_s=2,
        receipts=receipts,
    )
    assert set(receipts) == {"C1", "C2"}
    pauses = [
        command for command in delegate.commands if command.phase == "event.pause-drain"
    ]
    assert [command.instance for command in sorted(pauses, key=lambda item: item.sequence)] == [
        "C1",
        "C2",
    ]


def test_empty_gate_barrier_is_a_noop(tmp_path: Path) -> None:
    farm, scenario, plan = _fixture(tmp_path)
    delegate = EventRecorder()
    producer = EventProducer(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(delegate),
        deadline_s=2,
    )
    receipts: dict[str, dict[str, object]] = {}
    producer._gate_controls(
        (),
        action="pause",
        turn="A",
        epoch=1,
        timeout_s=2,
        receipts=receipts,
    )
    assert receipts == {}
    assert delegate.commands == []


def test_multi_client_gate_retains_the_invalid_raw_receipt(tmp_path: Path) -> None:
    farm, scenario, plan = _fixture(tmp_path)
    delegate = EventRecorder()
    started = {"C1": threading.Event(), "C2": threading.Event()}

    class InvalidSecondGateRecorder:
        def invoke(self, command: PlannedCommand) -> CommandResult:
            if command.phase != "event.pause-drain":
                return delegate.invoke(command)
            delegate.commands.append(command)
            assert command.instance in started
            other = "C2" if command.instance == "C1" else "C1"
            started[command.instance].set()
            assert started[other].wait(timeout=1)
            receipt = {
                "action": "pause",
                "active_after": 0,
                "active_before": 48 if command.instance == "C1" else 0,
                "client": command.instance,
                "epoch": 1,
                "finished_ms": 101,
                "schema": "icefarm-event-gate-v1",
                "started_ms": 100,
                "status": "PAUSED",
                "turn": "A",
            }
            return CommandResult(0, json.dumps(receipt), "")

    producer = EventProducer(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(InvalidSecondGateRecorder()),
        deadline_s=2,
    )
    c1 = next(
        item for item in plan["topology"]["instances"] if item["name"] == "C1"
    )
    c2 = dict(c1, name="C2", host="tt-quietbox3")
    receipts: dict[str, dict[str, object]] = {}
    with pytest.raises(EventError, match="C2: event gate pause returned an invalid receipt"):
        producer._gate_controls(
            (c1, c2),
            action="pause",
            turn="A",
            epoch=1,
            timeout_s=2,
            receipts=receipts,
        )
    assert receipts["C1"]["active_before"] == 48
    attempt = producer._failure_evidence["gate_attempts"]["A/C2/pause"]
    assert attempt["state"] == "FAILED"
    assert attempt["receipt"]["active_before"] == 0
    assert json.loads(attempt["raw_result"]["stdout"])["active_before"] == 0


def test_partial_multi_client_pause_failure_aborts_all_before_scheduler_restart(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    farm, scenario, plan = _fixture(tmp_path)
    scenario.data["workload"]["clients"] = ["C1", "C2"]
    c1 = next(
        item for item in plan["topology"]["instances"] if item["name"] == "C1"
    )
    plan["topology"]["instances"].append(
        dict(c1, name="C2", host="tt-quietbox3")
    )

    class PartialBarrierRecorder:
        def __init__(self) -> None:
            self.commands: list[PlannedCommand] = []

        def invoke(self, command: PlannedCommand) -> CommandResult:
            self.commands.append(command)
            if command.phase == "event.restart":
                raise AssertionError("scheduler transition ran after a failed pause barrier")
            if command.phase not in {"event.pause-drain", "event.abort-resume"}:
                return CommandResult(0, "", "")
            action = "pause" if command.phase == "event.pause-drain" else "abort"
            receipt = {
                "action": action,
                "active_after": 0,
                "active_before": (
                    48
                    if action == "pause" and command.instance == "C1"
                    else 0
                ),
                "client": command.instance,
                "epoch": 1,
                "finished_ms": 101,
                "schema": "icefarm-event-gate-v1",
                "started_ms": 100,
                "status": "PAUSED" if action == "pause" else "ABORT",
                "turn": "A",
            }
            return CommandResult(0, json.dumps(receipt), "")

    delegate = PartialBarrierRecorder()
    producer = EventProducer(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(delegate),
        deadline_s=2,
    )
    producer.signal_turn_start("A")
    producer._start = producer.monotonic()
    monkeypatch.setattr(
        producer,
        "_inspect",
        lambda _name: {
            "id": "1" * 64,
            "running": True,
            "started_at": "2026-09-05T12:59:50Z",
        },
    )
    event = TimelineEvent.from_dict(
        0, {"trigger": "job 24", "action": "restart", "instance": "S1"}
    )
    scheduler = next(
        item for item in plan["topology"]["instances"] if item["role"] == "S"
    )

    with pytest.raises(
        EventError,
        match="C2: event gate pause returned an invalid receipt",
    ):
        producer._coordinated_scheduler_restart(event, scheduler, "1" * 64)

    assert not any(command.phase == "event.restart" for command in delegate.commands)
    assert sorted(
        command.instance
        for command in delegate.commands
        if command.phase == "event.abort-resume"
    ) == ["C1", "C2"]
    assert producer._failure_evidence["root_phase"] == "event.pause-drain"
    assert producer._failure_evidence["gate_receipts"]["A/C1/pause"][
        "active_before"
    ] == 48
    assert producer._failure_evidence["gate_attempts"]["A/C2/pause"]["receipt"][
        "active_before"
    ] == 0
    assert {
        producer._failure_evidence["gate_receipts"][f"A/{client}/abort"]["status"]
        for client in ("C1", "C2")
    } == {"ABORT"}


def test_worker_restart_stays_live_and_emits_collectable_rejoin_receipt(
    tmp_path: Path,
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S70-b4-worker-bounces.json", farm
    )
    scenario.data["timeline"] = [
        {"trigger": "t+0", "action": "restart", "instance": "F1"}
    ]
    plan = farmtest.build_plan(farm, scenario, run_id="worker-restart-unit")
    target = next(
        item for item in plan["topology"]["instances"] if item["name"] == "F1"
    )

    class WorkerRestartRecorder(EventRecorder):
        def __init__(self) -> None:
            super().__init__()
            self.restarted = False

        def invoke(self, command: PlannedCommand) -> CommandResult:
            self.commands.append(command)
            if command.phase == "event.authenticate":
                return CommandResult(
                    0,
                    json.dumps(
                        {
                            "Id": "3" * 64,
                            "Name": "/icefarm-worker-restart-unit-F1",
                            "State": {
                                "Running": True,
                                "StartedAt": (
                                    "2026-09-06T00:01:00Z"
                                    if self.restarted
                                    else "2026-09-06T00:00:00Z"
                                ),
                            },
                            "Mounts": [
                                {
                                    "Destination": "/opt/icecream",
                                    "Source": str(runtime_root(farm, target)),
                                }
                            ],
                            "Config": {
                                "Env": [],
                                "Labels": {
                                    "icefarm.run": "worker-restart-unit",
                                    "icefarm.instance": "F1",
                                },
                            },
                        }
                    ),
                    "",
                )
            if command.phase == "event.readiness-baseline":
                return CommandResult(0, "17 log\n", "")
            if command.phase == "event.restart":
                self.restarted = True
                return CommandResult(0, "", "")
            if command.phase == "event.header-readiness":
                return CommandResult(
                    0,
                    json.dumps(
                        {
                            "bytes": 80,
                            "cache_line": "cache sidecar adapter state=2 lifecycle=3",
                            "line": "ICECREAM daemon F1 starting up",
                            "ready": True,
                        }
                    ),
                    "",
                )
            if command.phase == "event.header-scheduler-rejoin":
                return CommandResult(
                    0,
                    json.dumps(
                        {
                            "bytes": 160,
                            "cache_line": (
                                "RELOGIN F1(10.0.0.1): cache=on cache_wire=v1 "
                                "cache_protocol=1 cache_profiles=p29v1 zstd_tu "
                                "zstd_route"
                            ),
                            "cache_protocol": 1,
                            "login_line": "login F1 protocol version: 50",
                            "loss_job_ids": [],
                            "profile": "p29v1",
                            "ready": True,
                            "role_protocol": 50,
                            "sha256": "a" * 64,
                            "target": "F1",
                        }
                    ),
                    "",
                )
            if command.phase == "readiness.container":
                return CommandResult(0, json.dumps({"Running": True}), "")
            if command.phase == "readiness.listcs":
                return CommandResult(
                    0,
                    "F1 10.0.27.127 24\nF2 10.0.27.128 12\n",
                    "",
                )
            return CommandResult(0, "", "")

    recorder = WorkerRestartRecorder()
    producer = EventProducer(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(recorder),
        event_path=tmp_path / "events" / "events.json",
        deadline_s=2,
        poll_interval_s=0.01,
        wall_ms=lambda: 2000,
    )
    producer.signal_turn_start("A")
    producer.start()
    producer.wait()
    producer.signal_turn_complete("A")
    producer.stop()

    phases = [command.phase for command in recorder.commands]
    assert "event.pause-drain" not in phases
    assert phases.index("event.restart") < phases.index("event.header-readiness")
    assert phases.index("event.header-readiness") < phases.index(
        "event.header-scheduler-rejoin"
    )
    receipt = producer.records[0].receipt
    assert receipt["schema"] == "icefarm-worker-restart-v1"
    assert receipt["before"]["container_id"] == receipt["after"]["container_id"]
    assert receipt["before"]["started_at"] != receipt["after"]["started_at"]
    assert receipt["coordination"]["workers"] == ["F1", "F2"]
    assert len(_event_log(tmp_path, scenario, farm=farm, plan=plan)) == 1

    document = json.loads((tmp_path / "events" / "events.json").read_text())
    document["events"][0]["receipt"]["after"]["started_at"] = document[
        "events"
    ][0]["receipt"]["before"]["started_at"]
    (tmp_path / "events" / "events.json").write_text(json.dumps(document))
    with pytest.raises(CollectError, match="in-place fresh restart"):
        _event_log(tmp_path, scenario, farm=farm, plan=plan)


def test_upgrade_and_downgrade_direction_is_fail_closed(tmp_path: Path) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(INTEGRATION / "scenarios" / "S20-scheduler-first.json", farm)
    plan = farmtest.build_plan(farm, scenario, run_id="event-unit")
    for action, image, message in (
        ("upgrade", "old", "upgrade"),
        ("downgrade", "new", "downgrade"),
    ):
        scenario.data["timeline"] = [
            {"trigger": "t+0", "action": action, "instance": "F1", "image": image}
        ]
        with pytest.raises(EventError, match=message):
            EventProducer(farm, scenario, plan, recorder=RecordingTransport(EventRecorder()))


@pytest.mark.parametrize(
    ("scenario_id", "action", "target_alias", "expected_env"),
    (
        ("S60-05-c2-up", "upgrade", "new", {"ICECC_P50_MODE": "on"}),
        ("S60-06-c2-down", "downgrade", "old", {}),
    ),
)
def test_client_generation_transition_owns_explicit_mode(
    scenario_id: str,
    action: str,
    target_alias: str,
    expected_env: dict[str, str],
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / f"{scenario_id}.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="client-mode-unit")
    client = next(
        item for item in plan["topology"]["instances"] if item["name"] == "C2"
    )
    producer = object.__new__(EventProducer)
    producer.farm = farm
    producer.scenario = scenario
    producer.plan = plan
    producer._state = {
        "C2": {
            "env": dict(client.get("env", {})),
            "image": dict(client["image"]),
            "sha256": client["sha256"],
        }
    }
    event = TimelineEvent.from_dict(
        0,
        {
            "trigger": "job 24",
            "action": action,
            "instance": "C2",
            "image": target_alias,
        },
    )

    assert producer._target_instance(event)["env"] == expected_env


@pytest.mark.parametrize(
    "action,initial_alias,target_alias",
    (("upgrade", "old", "new"), ("downgrade", "new", "old")),
)
def test_collector_independently_rejects_non_directional_transition_receipt(
    tmp_path: Path, action: str, initial_alias: str, target_alias: str
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S20-scheduler-first.json", farm
    )
    target_instance = next(
        item for item in scenario.data["instances"] if item["name"] == "F1"
    )
    target_instance["image"] = initial_alias
    scenario.data["timeline"] = [
        {
            "trigger": "t+0",
            "action": action,
            "instance": "F1",
            "image": target_alias,
        }
    ]
    for alias in ("old", "new"):
        label = scenario.data["images"][alias]
        identity = _image_identity(
            CommandResult(
                0, json.dumps(_transition_image_document(farm, label)), ""
            ),
            alias,
        )
        farm.data["authority"]["images"][label]["closure_sha256"] = (
            identity.closure_sha256
        )
    plan = farmtest.build_plan(farm, scenario, run_id="event-unit")
    run_events(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(TransitionRecorder(farm, plan)),
        event_path=tmp_path / "events" / "events.json",
        deadline_s=2,
    )

    event_path = tmp_path / "events" / "events.json"
    document = json.loads(event_path.read_text(encoding="utf-8"))
    receipt = document["events"][0]["receipt"]
    for field in ("closure_sha256", "env", "image", "role_sha256"):
        receipt["before"][field] = receipt["after"][field]
    event_path.write_text(json.dumps(document), encoding="utf-8")
    planned = next(
        item for item in plan["topology"]["instances"] if item["name"] == "F1"
    )
    planned["image"]["label"] = receipt["before"]["image"]
    planned["image"]["closure_sha256"] = receipt["before"]["closure_sha256"]
    planned["env"] = dict(receipt["before"]["env"])
    planned["sha256"] = receipt["before"]["role_sha256"]
    with pytest.raises(CollectError, match="protocol generation"):
        _event_log(tmp_path, scenario, farm=farm, plan=plan)


def test_job_trigger_counts_only_dispatches_after_the_workload_baseline(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _fixture(tmp_path)
    scenario.data["timeline"] = [
        {"trigger": "job 2", "action": "restart", "instance": "F1"}
    ]
    samples = [
        "put 41 in joblist of F1\n",
        "put 41 in joblist of F1\nput 42 in joblist of F1\n",
        "put 41 in joblist of F1\nput 42 in joblist of F1\n"
        "put 43 in joblist of F1\n",
    ]

    def read_jobs() -> str:
        if len(samples) > 1:
            return samples.pop(0)
        return samples[0]

    records = run_events(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(EventRecorder()),
        job_reader=read_jobs,
        event_path=tmp_path / "events" / "events.json",
        deadline_s=2,
    )
    assert records[0].last_dispatched_job == 43
    assert records[0].workload_dispatch_count == 2


def test_job_trigger_excludes_late_environment_canary(tmp_path: Path) -> None:
    farm, scenario, plan = _fixture(tmp_path)
    scenario.data["timeline"] = [
        {"trigger": "job 1", "action": "restart", "instance": "F1"}
    ]
    samples = [
        "",
        "put 1 in joblist of F1 (will install now)\n",
        "put 1 in joblist of F1 (will install now)\nput 4 in joblist of F1\n",
    ]

    def read_jobs() -> str:
        if len(samples) > 1:
            return samples.pop(0)
        return samples[0]

    records = run_events(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(EventRecorder()),
        job_reader=read_jobs,
        event_path=tmp_path / "events" / "events.json",
        deadline_s=2,
    )
    assert records[0].last_dispatched_job == 4
    assert records[0].workload_dispatch_count == 1


def test_live_job_reader_uses_exact_bind_mounted_scheduler_log(tmp_path: Path) -> None:
    farm, scenario, plan = _fixture(tmp_path)
    scenario.data["timeline"] = [
        {"trigger": "job 1", "action": "restart", "instance": "F1"}
    ]

    class LiveReaderRecorder(EventRecorder):
        def __init__(self) -> None:
            super().__init__()
            self.samples = ["", "put 7 in joblist of F1\n"]

        def invoke(self, command: PlannedCommand) -> CommandResult:
            if command.phase == "event.poll":
                self.commands.append(command)
                if len(self.samples) > 1:
                    return CommandResult(0, self.samples.pop(0), "")
                return CommandResult(0, self.samples[0], "")
            return super().invoke(command)

    recorder = LiveReaderRecorder()
    records = run_events(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(recorder),
        event_path=tmp_path / "events" / "events.json",
        deadline_s=2,
    )
    assert records[0].last_dispatched_job == 7
    assert records[0].workload_dispatch_count == 1
    polls = [item for item in recorder.commands if item.phase == "event.poll"]
    assert polls and all(item.transport == "ssh" for item in polls)
    decoded = decode_ssh_payload(polls[0].argv)
    assert decoded[0] == "cat"
    assert decoded[1].endswith(
        "/icefarm/event-unit/S1/log/scheduler.log"
    )


def _route_process(executable: str, pid: int, start_ticks: int, *, ppid: int) -> dict[str, object]:
    return {
        "argv": [executable, "--fixture"],
        "exe": executable,
        "exe_evidence": "proc-exe",
        "pid": pid,
        "ppid": ppid,
        "start_ticks": start_ticks,
        "uid": 65534,
    }


def test_route_process_snapshot_records_eacces_fallback_and_binds_argv0() -> None:
    value = _route_process("/opt/icecream/sbin/iceccd", 1, 5, ppid=0)
    assert EventProducer._valid_process_snapshot(
        value, "/opt/icecream/sbin/iceccd"
    )
    fallback = dict(value, exe_evidence="argv0-after-proc-exe-eacces")
    assert EventProducer._valid_process_snapshot(
        fallback, "/opt/icecream/sbin/iceccd"
    )
    assert not EventProducer._valid_process_snapshot(
        dict(fallback, exe_evidence="unrecorded"),
        "/opt/icecream/sbin/iceccd",
    )
    assert not EventProducer._valid_process_snapshot(
        dict(fallback, argv=["/tmp/spoofed"]),
        "/opt/icecream/sbin/iceccd",
    )
    assert 'exc.errno != 13' in CLIENT_ROUTE_SIGNAL_SCRIPT
    assert '"argv0-after-proc-exe-eacces"' in CLIENT_ROUTE_SIGNAL_SCRIPT


class RouteRestartRecorder(EventRecorder):
    def __init__(self) -> None:
        super().__init__()
        self.daemon = _route_process("/opt/icecream/sbin/iceccd", 1, 5, ppid=0)
        self.before_owner = _route_process(
            "/opt/icecream/sbin/icecc-cache-service", 10, 10, ppid=1
        )
        self.after_owner = _route_process(
            "/opt/icecream/sbin/icecc-cache-service", 11, 20, ppid=1
        )

    def invoke(self, command: PlannedCommand) -> CommandResult:
        self.commands.append(command)
        if command.phase == "event.authenticate":
            return CommandResult(
                0,
                json.dumps(
                    {
                        "Id": "2" * 64,
                        "Name": "/icefarm-event-unit-C1",
                        "State": {
                            "Running": True,
                            "StartedAt": "2026-09-05T13:00:00Z",
                        },
                        "Mounts": [],
                        "Config": {
                            "Env": ["ICECC_P50_MODE=on"],
                            "Labels": {
                                "icefarm.run": "event-unit",
                                "icefarm.instance": "C1",
                            },
                        },
                    }
                ),
                "",
            )
        if command.phase == "event.readiness-baseline":
            return CommandResult(0, "10 /tmp/client-daemon.log\n", "")
        if command.phase == "event.client-route-signal":
            return CommandResult(
                0,
                json.dumps(
                    {
                        "daemon": self.daemon,
                        "mechanism": "pidfd_send_signal",
                        "route_owner": self.before_owner,
                        "schema": "icefarm-client-route-signal-v1",
                        "sent_ms": 950,
                        "signal": 9,
                    }
                ),
                "",
            )
        if command.phase == "event.client-route-process-ready":
            return CommandResult(
                0,
                json.dumps(
                    {
                        "daemon": self.daemon,
                        "daemon_count": 1,
                        "ready": True,
                        "route_owner": self.after_owner,
                        "route_owner_count": 1,
                        "schema": "icefarm-client-route-snapshot-v1",
                    }
                ),
                "",
            )
        if command.phase == "event.client-route-log-ready":
            return CommandResult(
                0,
                json.dumps(
                    {
                        "bytes": 80,
                        "lifecycle": 3,
                        "line": "[1] cache sidecar adapter state=2 lifecycle=3",
                        "ready": True,
                        "state": 2,
                    }
                ),
                "",
            )
        if command.phase in {"event.pause-drain", "event.resume"}:
            pause = command.phase == "event.pause-drain"
            return CommandResult(
                0,
                json.dumps(
                    {
                        "action": "pause" if pause else "resume",
                        "active_after": 0 if pause else 1,
                        "active_before": 2 if pause else 0,
                        "client": "C1",
                        "epoch": 1,
                        "finished_ms": 920 if pause else 1020,
                        "schema": "icefarm-event-gate-v1",
                        "started_ms": 900 if pause else 1010,
                        "status": "PAUSED" if pause else "OPEN",
                        "turn": "A",
                    }
                ),
                "",
            )
        return CommandResult(0, "", "")


def _run_route_restart(tmp_path: Path):
    farm, scenario, plan = _fixture(tmp_path)
    scenario.data["timeline"] = [
        {"trigger": "job 1", "action": "restart", "instance": "C1"}
    ]
    jobs = iter(("", "put 7 in joblist of F1\n"))
    recorder = RouteRestartRecorder()
    producer = EventProducer(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(recorder),
        job_reader=lambda: next(jobs, "put 7 in joblist of F1\n"),
        event_path=tmp_path / "events" / "events.json",
        deadline_s=2,
        poll_interval_s=0.01,
        wall_ms=lambda: 1000,
    )
    producer.signal_turn_start("A")
    producer.start()
    producer.wait()
    producer.signal_turn_complete("A")
    producer.stop()
    return farm, scenario, plan, recorder, producer


def test_client_restart_bounces_only_route_owner_after_drain(tmp_path: Path) -> None:
    farm, scenario, plan, recorder, producer = _run_route_restart(tmp_path)
    assert len(producer.records) == 1
    receipt = producer.records[0].receipt
    assert receipt["schema"] == "icefarm-client-route-restart-v1"
    assert receipt["before"]["container_id"] == receipt["after"]["container_id"]
    assert receipt["before"]["daemon"] == receipt["after"]["daemon"]
    assert receipt["before"]["route_owner"]["pid"] == 10
    assert receipt["after"]["route_owner"]["pid"] == 11
    phases = [command.phase for command in recorder.commands]
    assert phases.index("event.pause-drain") < phases.index("event.client-route-signal")
    assert phases.index("event.client-route-log-ready") < phases.index("event.resume")
    assert not any(
        "container" in command.argv and "restart" in command.argv
        for command in recorder.commands
    )
    assert len(_event_log(tmp_path, scenario, farm=farm, plan=plan)) == 1


@pytest.mark.parametrize(
    "mutation",
    (
        "container",
        "daemon",
        "owner",
        "signal",
        "readiness",
        "drain",
    ),
)
def test_client_route_restart_receipt_tampering_fails_closed(
    tmp_path: Path, mutation: str
) -> None:
    farm, scenario, plan, _recorder, _producer = _run_route_restart(tmp_path)
    event_path = tmp_path / "events" / "events.json"
    document = json.loads(event_path.read_text(encoding="utf-8"))
    receipt = document["events"][0]["receipt"]
    if mutation == "container":
        receipt["after"]["container_started_at"] = "later"
    elif mutation == "daemon":
        receipt["after"]["daemon"]["start_ticks"] += 1
    elif mutation == "owner":
        receipt["after"]["route_owner"] = receipt["before"]["route_owner"]
    elif mutation == "signal":
        receipt["coordination"]["signal"]["mechanism"] = "kill"
    elif mutation == "readiness":
        receipt["coordination"]["readiness"]["line"] = "not ready"
    else:
        receipt["coordination"]["clients"]["C1"]["active_after"] = 1
    event_path.write_text(json.dumps(document), encoding="utf-8")
    with pytest.raises(CollectError, match="client route-owner restart"):
        _event_log(tmp_path, scenario, farm=farm, plan=plan)


@pytest.mark.parametrize(
    ("injected_phase", "expected_phase"),
    (
        ("event.readiness-baseline", "event.readiness-baseline"),
        ("event.client-route-signal", "event.client-route-signal"),
        ("event.client-route-process-ready", "event.client-route-process-ready"),
        ("event.client-route-log-ready", "event.client-route-log-ready"),
        ("event.abort-resume", "event.abort-resume"),
    ),
)
def test_client_route_restart_failure_descriptor_captures_phase_and_abort(
    tmp_path: Path, injected_phase: str, expected_phase: str
) -> None:
    farm, scenario, plan = _fixture(tmp_path)
    scenario.data["timeline"] = [
        {"trigger": "job 1", "action": "restart", "instance": "C1"}
    ]

    class FailingRouteRecorder(RouteRestartRecorder):
        def invoke(self, command: PlannedCommand) -> CommandResult:
            if command.phase == injected_phase or (
                injected_phase == "event.abort-resume"
                and command.phase == "event.client-route-process-ready"
            ):
                self.commands.append(command)
                if command.phase == "event.client-route-process-ready":
                    return CommandResult(17, "", "process readiness failure")
                if injected_phase == "event.abort-resume":
                    return CommandResult(19, "", "abort release failure")
                return CommandResult(13, "", "injected phase failure")
            return super().invoke(command)

    recorder = FailingRouteRecorder()
    jobs = iter(("", "put 7 in joblist of F1\n"))
    producer = EventProducer(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(recorder),
        job_reader=lambda: next(jobs, "put 7 in joblist of F1\n"),
        event_path=tmp_path / "events" / "events.json",
        deadline_s=2,
        poll_interval_s=0.01,
        wall_ms=lambda: 1000,
    )
    producer.signal_turn_start("A")
    producer.start()
    with pytest.raises(EventError):
        producer.wait()
    failure = json.loads((tmp_path / "events" / "failure.json").read_text())
    assert failure["phase"] == (
        "event.client-route-process-ready"
        if injected_phase == "event.abort-resume"
        else expected_phase
    )
    if injected_phase == "event.abort-resume":
        assert failure["last_phase"] == expected_phase
    assert failure["evidence"]["gate_attempts"]["A/C1/pause"]["state"] == "SUCCEEDED"
    assert failure["evidence"]["gate_attempts"]["A/C1/abort"]["state"] in {
        "ATTEMPTED", "FAILED", "SUCCEEDED"
    }
    if injected_phase == "event.client-route-signal":
        assert failure["evidence"]["route"]["signal"]["state"] == "FAILED"
    if injected_phase == "event.client-route-process-ready":
        assert failure["evidence"]["route"]["process_ready"]["state"] == "FAILED"
    if injected_phase == "event.client-route-log-ready":
        assert failure["evidence"]["route"]["log_ready"]["state"] == "FAILED"


@pytest.mark.parametrize(
    "malformed_phase",
    (
        "event.pause-drain",
        "event.readiness-baseline",
        "event.client-route-signal",
        "event.client-route-process-ready",
        "event.client-route-log-ready",
    ),
)
def test_route_failure_descriptor_marks_rc0_malformed_phase(
    tmp_path: Path, malformed_phase: str
) -> None:
    farm, scenario, plan = _fixture(tmp_path)
    scenario.data["timeline"] = [
        {"trigger": "job 1", "action": "restart", "instance": "C1"}
    ]

    class MalformedRouteRecorder(RouteRestartRecorder):
        def invoke(self, command: PlannedCommand) -> CommandResult:
            if command.phase == malformed_phase:
                self.commands.append(command)
                return CommandResult(0, "{}", "")
            return super().invoke(command)

    recorder = MalformedRouteRecorder()
    jobs = iter(("", "put 7 in joblist of F1\n"))
    producer = EventProducer(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(recorder),
        job_reader=lambda: next(jobs, "put 7 in joblist of F1\n"),
        event_path=tmp_path / "events" / "events.json",
        deadline_s=2,
        poll_interval_s=0.01,
        wall_ms=lambda: 1000,
    )
    producer.signal_turn_start("A")
    producer.start()
    with pytest.raises(EventError):
        producer.wait()
    failure = json.loads((tmp_path / "events" / "failure.json").read_text())
    assert failure["phase"] == malformed_phase
    if malformed_phase == "event.pause-drain":
        assert failure["evidence"]["gate_attempts"]["A/C1/pause"]["state"] == "FAILED"
    elif malformed_phase == "event.readiness-baseline":
        assert failure["evidence"]["route"]["signal"]["state"] == "NOT_STARTED"
    else:
        route = {
            "event.client-route-signal": "signal",
            "event.client-route-process-ready": "process_ready",
            "event.client-route-log-ready": "log_ready",
        }[malformed_phase]
        assert failure["evidence"]["route"][route]["state"] == "FAILED"


def test_route_failure_descriptor_marks_readiness_timeout(tmp_path: Path) -> None:
    farm, scenario, plan = _fixture(tmp_path)
    scenario.data["timeline"] = [
        {"trigger": "job 1", "action": "restart", "instance": "C1"}
    ]

    class NeverReadyRecorder(RouteRestartRecorder):
        def invoke(self, command: PlannedCommand) -> CommandResult:
            if command.phase == "event.client-route-process-ready":
                self.commands.append(command)
                return CommandResult(
                    0,
                    json.dumps(
                        {
                            "daemon": self.daemon,
                            "daemon_count": 1,
                            "ready": False,
                            "route_owner": self.before_owner,
                            "route_owner_count": 1,
                            "schema": "icefarm-client-route-snapshot-v1",
                        }
                    ),
                    "",
                )
            return super().invoke(command)

    ticks = iter(range(20))
    recorder = NeverReadyRecorder()
    jobs = iter(("", "put 7 in joblist of F1\n"))
    producer = EventProducer(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(recorder),
        job_reader=lambda: next(jobs, "put 7 in joblist of F1\n"),
        event_path=tmp_path / "events" / "events.json",
        deadline_s=3,
        poll_interval_s=0.01,
        monotonic=lambda: next(ticks),
        wall_ms=lambda: 1000,
    )
    producer.signal_turn_start("A")
    producer.start()
    with pytest.raises(EventError):
        producer.wait()
    failure = json.loads((tmp_path / "events" / "failure.json").read_text())
    assert failure["phase"] == "event.client-route-process-ready"
    assert failure["evidence"]["route"]["process_ready"]["state"] == "FAILED"


def test_route_failure_descriptor_resets_between_restarts(tmp_path: Path) -> None:
    farm, scenario, plan = _fixture(tmp_path)
    scenario.data["timeline"] = [
        {"trigger": "job 1", "action": "restart", "instance": "C1"},
        {"trigger": "job 2", "action": "restart", "instance": "C1"},
    ]

    class SecondSignalFails(RouteRestartRecorder):
        def __init__(self) -> None:
            super().__init__()
            self.signal_count = 0

        def invoke(self, command: PlannedCommand) -> CommandResult:
            if command.phase == "event.client-route-signal":
                self.signal_count += 1
                if self.signal_count == 2:
                    self.commands.append(command)
                    return CommandResult(0, "{}", "")
            result = super().invoke(command)
            if command.phase in {"event.pause-drain", "event.resume"}:
                document = json.loads(result.stdout)
                document["epoch"] = int(command.argv[-2])
                return CommandResult(0, json.dumps(document), result.stderr)
            return result

    recorder = SecondSignalFails()
    jobs = iter(
        (
            "",
            "put 7 in joblist of F1\n",
            "put 7 in joblist of F1\nput 8 in joblist of F1\n",
        )
    )
    producer = EventProducer(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(recorder),
        job_reader=lambda: next(jobs, "put 8 in joblist of F1\n"),
        event_path=tmp_path / "events" / "events.json",
        deadline_s=3,
        poll_interval_s=0.01,
        wall_ms=lambda: 1000,
    )
    producer.signal_turn_start("A")
    producer.start()
    with pytest.raises(EventError):
        producer.wait()
    failure = json.loads((tmp_path / "events" / "failure.json").read_text())
    assert failure["event_index"] == 1
    assert failure["event_epoch"] == 2
    assert failure["phase"] == "event.client-route-signal"
    assert failure["last_phase"] == "event.abort-resume"
    assert failure["evidence"]["route"]["signal"]["state"] == "FAILED"
    assert failure["evidence"]["gate_attempts"]["A/C1/pause"]["state"] == "SUCCEEDED"


def test_workload_joins_event_worker_and_propagates_fake_action(tmp_path: Path) -> None:
    farm, scenario, plan = _fixture(tmp_path)
    scenario.data["timeline"] = [
        {"trigger": "t+0", "action": "restart", "instance": "F1"}
    ]
    class WorkloadAndEventRecorder(EventRecorder):
        def invoke(self, command: PlannedCommand) -> CommandResult:
            result = super().invoke(command)
            if command.phase == "run.workload":
                return CommandResult(0, "ICEFARM_WORKLOAD jobs=100 failures=0 samples=3\n", "")
            return result

    transport = RecordingTransport(WorkloadAndEventRecorder())
    run_workload(
        farm,
        scenario,
        plan,
        recorder=transport,
        require_up=False,
        event_path=tmp_path / "events" / "events.json",
    )
    assert any(command.phase == "event.restart" for command in transport.commands)
    assert not any(thread.name == "icefarm-events" and thread.is_alive() for thread in threading.enumerate())


def _transition_image_document(farm, label: str) -> dict[str, object]:
    authority = farm.data["authority"]["images"][label]
    marker = "3" if label.startswith("p50") else "2"
    return {
        "Architecture": "amd64",
        "Config": {"Labels": _expected_image_labels(authority)},
        "Created": "2026-09-04T00:00:00Z",
        "Id": "sha256:" + "1" * 64,
        "Os": "linux",
        "RootFS": {"Type": "layers", "Layers": ["sha256:" + marker * 64]},
        "Size": 100,
    }


class TransitionRecorder(EventRecorder):
    def __init__(self, farm, plan) -> None:
        super().__init__()
        self.farm = farm
        self.plan = plan
        self.auth_count = 0
        self.current_ids: dict[str, str] = {}
        self.instance_auth_counts: dict[str, int] = {}
        self.next_mounts: list[dict[str, str]] = []
        self.next_env: list[str] = []
        self.next_by_instance: dict[str, tuple[list[dict[str, str]], list[str]]] = {}
        self.before_running = True
        self.after_running = True
        self.omit_current_mount = False
        self.omit_current_env = False
        self.omit_target_mount = False
        self.omit_target_env = False

    def invoke(self, command: PlannedCommand) -> CommandResult:
        self.commands.append(command)
        if command.phase == "preflight.image":
            label = command.argv[-1].rsplit(":", 1)[-1]
            return CommandResult(0, json.dumps(_transition_image_document(self.farm, label)), "")
        if command.phase == "preflight.runtime-materialize":
            return CommandResult(0, f"materialized /icefarm-runtimes/{command.argv[-1]}\n", "")
        if command.phase == "preflight.role-hashes":
            rows = []
            for path, role in (
                ("/opt/icecream/sbin/icecc-scheduler", "scheduler"),
                ("/opt/icecream/bin/icecc", "client"),
                ("/opt/icecream/sbin/iceccd", "daemon"),
            ):
                if path in command.argv:
                    mount = next(value for value in command.argv if "/runtimes/" in value)
                    closure = mount.split("/runtimes/", 1)[1].split("/", 1)[0]
                    label = next(
                        label
                        for label, authority in self.farm.data["authority"]["images"].items()
                        if authority.get("closure_sha256") == closure
                    )
                    version = "50" if label.startswith("p50") else "43"
                    digest = self.farm.data["authority"]["role_stores"][version][role]["sha256"]
                    rows.append(f"{digest}  {path}")
            return CommandResult(0, "\n".join(rows) + "\n", "")
        if command.phase == "event.authenticate":
            self.auth_count += 1
            name = command.instance or "?"
            self.instance_auth_counts[name] = self.instance_auth_counts.get(name, 0) + 1
            if name not in self.current_ids:
                self.current_ids[name] = "1" * 64
            elif self.instance_auth_counts[name] % 2 == 0:
                self.current_ids[name] = str(self.instance_auth_counts[name]) * 64
            item = next(item for item in self.plan["topology"]["instances"] if item["name"] == name)
            is_after = self.instance_auth_counts[name] % 2 == 0
            if is_after or name in self.next_by_instance:
                mounts, env = self.next_by_instance.get(name, (self.next_mounts, self.next_env))
            else:
                mounts = [
                    {
                        "Destination": "/opt/icecream",
                        "Source": str(runtime_root(self.farm, item)),
                    }
                ]
                env = [f"{key}={value}" for key, value in item.get("env", {}).items()]
            if is_after and self.omit_target_mount:
                mounts = []
            if not is_after and self.omit_current_mount:
                mounts = []
            if is_after and self.omit_target_env:
                env = []
            if not is_after and self.omit_current_env:
                env = []
            return CommandResult(
                0,
                json.dumps(
                    {
                        "Id": self.current_ids[name],
                        "Name": f"/icefarm-event-unit-{name}",
                        "State": {
                            "Running": self.after_running if is_after else self.before_running
                        },
                        "Mounts": mounts,
                        "Config": {
                            "Env": env,
                            "Labels": {
                                "icefarm.run": "event-unit",
                                "icefarm.instance": name,
                            }
                        },
                    }
                ),
                "",
            )
        if command.phase.endswith(".start") and command.phase.startswith("event."):
            self.next_mounts = []
            self.next_env = []
            for index, value in enumerate(command.argv):
                if value == "--mount" and index + 1 < len(command.argv):
                    mount = command.argv[index + 1]
                    fields = dict(
                        part.split("=", 1)
                        for part in mount.split(",")
                        if "=" in part
                    )
                    self.next_mounts.append(
                        {
                            "Destination": fields.get("dst", ""),
                            "Source": fields.get("src", ""),
                        }
                    )
                if value == "--env" and index + 1 < len(command.argv):
                    self.next_env.append(command.argv[index + 1])
            self.next_by_instance[command.instance or "?"] = (list(self.next_mounts), list(self.next_env))
            return CommandResult(0, "", "")
        if command.phase == "event.readiness-baseline":
            return CommandResult(0, "10 /log/scheduler.log\n", "")
        if command.phase == "event.readiness":
            role = next(item["role"] for item in self.plan["topology"]["instances"] if item["name"] == command.instance)
            line = "ICECREAM daemon test starting up (nice level 5)"
            if role == "S":
                line = "ICECREAM scheduler test starting up, port 23000"
            return CommandResult(0, json.dumps({"bytes": 20, "line": line, "ready": True}), "")
        return CommandResult(0, "", "")


class CoordinatedTransitionRecorder(TransitionRecorder):
    def invoke(self, command: PlannedCommand) -> CommandResult:
        result = super().invoke(command)
        if command.phase == "event.authenticate":
            document = json.loads(result.stdout)
            count = self.instance_auth_counts[command.instance or "?"]
            document["State"]["StartedAt"] = (
                "2026-09-05T00:01:00Z" if count % 2 == 0 else "2026-09-05T00:00:00Z"
            )
            return CommandResult(0, json.dumps(document), "")
        if command.phase in {"event.pause-drain", "event.resume", "event.abort-resume"}:
            action = {
                "event.pause-drain": ("pause", "PAUSED", 100, 99),
                "event.resume": ("resume", "OPEN", 1001, 1000),
                "event.abort-resume": ("abort", "ABORT", 1001, 1000),
            }[command.phase]
            return CommandResult(
                0,
                json.dumps(
                    {
                        "action": action[0],
                        "active_after": 0,
                        "active_before": 1 if action[0] == "pause" else 0,
                        "client": command.instance,
                        "epoch": 1,
                        "finished_ms": action[2],
                        "schema": "icefarm-event-gate-v1",
                        "started_ms": action[3],
                        "status": action[1],
                        "turn": "A",
                    }
                ),
                "",
            )
        if command.phase == "readiness.container":
            return CommandResult(0, '{"Running": true}\n', "")
        if command.phase == "readiness.listcs":
            return CommandResult(0, "F1\n", "")
        if command.phase == "event.scheduler-worker-rejoin":
            return CommandResult(
                0,
                json.dumps(
                    {
                        "bytes": 100,
                        "login_line": "login F1 protocol version: 50",
                        "ready": True,
                        "role_protocol": 50,
                        "target": "F1",
                    }
                ),
                "",
            )
        if command.phase == "event.client-scheduler-ready":
            return CommandResult(
                0,
                json.dumps(
                    {
                        "bytes": 100,
                        "cache_line": None,
                        "cache_required": False,
                        "connected_line": "Connected to scheduler (I am known as C1)",
                        "ready": True,
                    }
                ),
                "",
            )
        return result


def test_s_f_transition_pauses_rejoins_and_resumes_with_strict_receipt(tmp_path: Path) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(INTEGRATION / "scenarios" / "S20-scheduler-first.json", farm)
    scenario.data["timeline"] = [
        {"trigger": "job 1", "action": "upgrade", "instance": "F1", "image": "new"}
    ]
    old_document = _transition_image_document(farm, "p43-1.4.0")
    new_document = _transition_image_document(farm, "p50s4-57a1e336")
    farm.data["authority"]["images"]["p43-1.4.0"]["closure_sha256"] = _image_identity(
        CommandResult(0, json.dumps(old_document), ""), "old"
    ).closure_sha256
    farm.data["authority"]["images"]["p50s4-57a1e336"]["closure_sha256"] = _image_identity(
        CommandResult(0, json.dumps(new_document), ""), "new"
    ).closure_sha256
    plan = farmtest.build_plan(farm, scenario, run_id="event-unit")
    recorder = CoordinatedTransitionRecorder(farm, plan)
    reads = iter(("", "put 1 in joblist of F1"))
    producer = EventProducer(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(recorder),
        job_reader=lambda: next(reads),
        event_path=tmp_path / "events" / "events.json",
        deadline_s=2,
        poll_interval_s=0.01,
        wall_ms=lambda: 1000,
    )
    producer.signal_turn_start("A")
    producer.start()
    producer.wait()
    producer.stop()
    record = producer.records[0]
    assert record.receipt is not None
    assert record.receipt["schema"] == TRANSITION_SCHEMA
    assert record.fired_ms == record.receipt["coordination"]["ready_ms"]
    phases = [command.phase for command in recorder.commands]
    assert phases.index("event.pause-drain") < phases.index("event.upgrade.stop")
    assert phases.index("event.upgrade.stop") < phases.index("event.upgrade.remove") < phases.index("event.upgrade.start")
    assert phases.index("event.scheduler-worker-rejoin") < phases.index("event.resume")
    assert _event_log(tmp_path, scenario, farm=farm, plan=plan)[0]["receipt"] == record.receipt
    path = tmp_path / "events" / "events.json"
    tampered = json.loads(path.read_text(encoding="utf-8"))
    tampered["events"][0]["receipt"]["coordination"]["scheduler_worker_rejoin"]["line"] = "stale"
    path.write_text(json.dumps(tampered), encoding="utf-8")
    with pytest.raises(CollectError, match="transition coordination"):
        _event_log(tmp_path, scenario, farm=farm, plan=plan)


def test_scheduler_transition_requires_fresh_workers_and_all_clients(tmp_path: Path) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(INTEGRATION / "scenarios" / "S20-scheduler-first.json", farm)
    old_scheduler = next(item for item in scenario.data["instances"] if item["name"] == "S1")
    old_scheduler["image"] = "old"
    old_scheduler["env"] = {}
    scenario.data["timeline"] = [
        {"trigger": "job 1", "action": "upgrade", "instance": "S1", "image": "new"}
    ]
    old_document = _transition_image_document(farm, "p43-1.4.0")
    new_document = _transition_image_document(farm, "p50s4-57a1e336")
    for label, document, alias in (
        ("p43-1.4.0", old_document, "old"),
        ("p50s4-57a1e336", new_document, "new"),
    ):
        farm.data["authority"]["images"][label]["closure_sha256"] = _image_identity(
            CommandResult(0, json.dumps(document), ""), alias
        ).closure_sha256
    plan = farmtest.build_plan(farm, scenario, run_id="event-unit")
    recorder = CoordinatedTransitionRecorder(farm, plan)
    reads = iter(("", "put 1 in joblist of F1"))
    producer = EventProducer(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(recorder),
        job_reader=lambda: next(reads),
        event_path=tmp_path / "events" / "events.json",
        deadline_s=2,
        poll_interval_s=0.01,
        wall_ms=lambda: 1000,
    )
    producer.signal_turn_start("A")
    producer.start()
    producer.wait()
    producer.stop()
    receipt = producer.records[0].receipt
    assert receipt is not None
    assert receipt["coordination"]["scheduler_startup"]["role"] == "S"
    assert set(receipt["coordination"]["client_readiness"]) == {"C1"}
    phases = [command.phase for command in recorder.commands]
    assert phases.index("event.pause-drain") < phases.index("event.upgrade.stop")
    assert phases.index("event.client-scheduler-ready") < phases.index("event.resume")
    assert _event_log(tmp_path, scenario, farm=farm, plan=plan)[0]["receipt"] == receipt

    scheduler_instance = next(
        item for item in plan["topology"]["instances"] if item["name"] == "S1"
    )
    client_instance = next(
        item for item in plan["topology"]["instances"] if item["name"] == "C1"
    )
    scheduler_log = (
        tmp_path
        / "diagnostics"
        / scheduler_instance["host"]
        / "S1.log"
        / "scheduler.log"
    )
    scheduler_log.parent.mkdir(parents=True)
    scheduler_log.write_bytes(
        b"0123456789" + (receipt["readiness"]["line"] + "\n").encode()
    )
    client_log = (
        tmp_path
        / "diagnostics"
        / client_instance["host"]
        / "C1.log"
        / "client-daemon.log"
    )
    client_log.parent.mkdir(parents=True)
    connected = receipt["coordination"]["client_readiness"]["C1"][
        "connected_line"
    ]
    client_log.write_bytes(b"0123456789" + (connected + "\n").encode())
    assert _event_log(tmp_path, scenario, farm=farm, plan=plan)[0]["receipt"] == receipt

    persisted = json.loads((tmp_path / "events" / "events.json").read_text())
    nonexistent = "ICECREAM scheduler nonexistent starting up, port 23000"
    persisted["events"][0]["receipt"]["readiness"]["line"] = nonexistent
    persisted["events"][0]["receipt"]["coordination"]["scheduler_startup"][
        "line"
    ] = nonexistent
    (tmp_path / "events" / "events.json").write_text(json.dumps(persisted))
    with pytest.raises(CollectError, match="readiness witness is not bound"):
        _event_log(tmp_path, scenario, farm=farm, plan=plan)


def test_daemon_transition_aborts_paused_clients_on_missing_rejoin(tmp_path: Path) -> None:
    class MissingRejoinRecorder(CoordinatedTransitionRecorder):
        def invoke(self, command: PlannedCommand) -> CommandResult:
            if command.phase == "event.scheduler-worker-rejoin":
                super().invoke(command)
                return CommandResult(
                    0,
                    json.dumps(
                        {
                            "bytes": 0,
                            "login_line": None,
                            "ready": False,
                            "role_protocol": None,
                            "target": "F1",
                        }
                    ),
                    "",
                )
            return super().invoke(command)

    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(INTEGRATION / "scenarios" / "S20-scheduler-first.json", farm)
    scenario.data["timeline"] = [
        {"trigger": "job 1", "action": "upgrade", "instance": "F1", "image": "new"}
    ]
    old_document = _transition_image_document(farm, "p43-1.4.0")
    new_document = _transition_image_document(farm, "p50s4-57a1e336")
    for label, document, alias in (
        ("p43-1.4.0", old_document, "old"),
        ("p50s4-57a1e336", new_document, "new"),
    ):
        farm.data["authority"]["images"][label]["closure_sha256"] = _image_identity(
            CommandResult(0, json.dumps(document), ""), alias
        ).closure_sha256
    plan = farmtest.build_plan(farm, scenario, run_id="event-unit")
    recorder = MissingRejoinRecorder(farm, plan)
    reads = iter(("", "put 1 in joblist of F1"))
    producer = EventProducer(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(recorder),
        job_reader=lambda: next(reads),
        event_path=tmp_path / "events" / "events.json",
        deadline_s=1,
        poll_interval_s=0.01,
    )
    producer.signal_turn_start("A")
    producer.start()
    with pytest.raises(EventError, match="coordinated transition failed"):
        producer.wait()
    with pytest.raises(EventError, match="coordinated transition failed"):
        producer.stop()
    assert any(command.phase == "event.abort-resume" for command in recorder.commands)
    assert not producer.records


def test_upgrade_materializes_target_and_restarts_exact_container_with_receipt(tmp_path: Path) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(INTEGRATION / "scenarios" / "S20-scheduler-first.json", farm)
    scenario.data["timeline"] = [
        {"trigger": "t+0", "action": "upgrade", "instance": "F1", "image": "new"},
        {"trigger": "t+0", "action": "downgrade", "instance": "F1", "image": "old"},
    ]
    old_document = _transition_image_document(farm, "p43-1.4.0")
    new_document = _transition_image_document(farm, "p50s4-57a1e336")
    old_identity = _image_identity(CommandResult(0, json.dumps(old_document), ""), "old")
    new_identity = _image_identity(CommandResult(0, json.dumps(new_document), ""), "new")
    farm.data["authority"]["images"]["p43-1.4.0"]["closure_sha256"] = old_identity.closure_sha256
    farm.data["authority"]["images"]["p50s4-57a1e336"]["closure_sha256"] = new_identity.closure_sha256
    plan = farmtest.build_plan(farm, scenario, run_id="event-unit")
    recorder = TransitionRecorder(farm, plan)
    records = run_events(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(recorder),
        event_path=tmp_path / "events" / "events.json",
        deadline_s=2,
    )
    assert records[0].receipt is not None
    receipt = records[0].receipt
    assert receipt["before"]["image"] == "p43-1.4.0"
    assert receipt["after"]["image"] == "p50s4-57a1e336"
    assert records[1].receipt["before"]["image"] == "p50s4-57a1e336"
    assert records[1].receipt["after"]["image"] == "p43-1.4.0"
    phases = [command.phase for command in recorder.commands]
    assert phases[:3] == ["preflight.image", "preflight.runtime-mkdir", "preflight.runtime-materialize"]
    assert "preflight.role-hashes" in phases
    assert phases[-5:] == [
        "event.downgrade.stop",
        "event.downgrade.remove",
        "event.downgrade.start",
        "event.authenticate",
        "event.readiness",
    ]
    assert "event.readiness-baseline" in phases
    stop = next(command for command in recorder.commands if command.phase == "event.upgrade.stop")
    remove = next(command for command in recorder.commands if command.phase == "event.upgrade.remove")
    start = next(command for command in recorder.commands if command.phase == "event.upgrade.start")
    assert stop.argv[-1] == remove.argv[-1] == "1" * 64
    assert start.argv[start.argv.index("--name") + 1] == "icefarm-event-unit-F1"
    assert any(value.endswith("/runtimes/" + new_identity.closure_sha256 + "/root,dst=/opt/icecream,readonly") for value in start.argv)
    assert start.timeout_s >= 1
    assert _event_log(tmp_path, scenario, farm=farm, plan=plan)[0]["receipt"]["preflight"]["role_sha256"] == receipt["preflight"]["role_sha256"]
    event_path = tmp_path / "events" / "events.json"
    persisted = json.loads(event_path.read_text())
    persisted["events"][0]["receipt"]["preflight"]["role_sha256"] = "x"
    event_path.write_text(json.dumps(persisted), encoding="utf-8")
    with pytest.raises(CollectError):
        _event_log(tmp_path, scenario)


def test_env_set_is_limited_to_authorized_setting_and_receipts_are_immutable(tmp_path: Path) -> None:
    farm, scenario, plan = _fixture(tmp_path)
    document = _transition_image_document(farm, "p50s4-57a1e336")
    identity = _image_identity(CommandResult(0, json.dumps(document), ""), "env")
    farm.data["authority"]["images"]["p50s4-57a1e336"]["closure_sha256"] = identity.closure_sha256
    plan = farmtest.build_plan(farm, scenario, run_id="event-unit")
    scenario.data["timeline"] = [
        {
            "trigger": "t+0",
            "action": "env_set",
            "instance": "S1",
            "env": {"ICECC_P50_PROFILE": "OFF"},
        }
    ]
    recorder = TransitionRecorder(farm, plan)
    records = run_events(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(recorder),
        event_path=tmp_path / "events" / "events.json",
        deadline_s=2,
    )
    assert records[0].receipt["before"]["env"]["ICECC_P50_PROFILE"] == "P29V1"
    assert records[0].receipt["after"]["env"]["ICECC_P50_PROFILE"] == "OFF"
    start = next(command for command in recorder.commands if command.phase == "event.env_set.start")
    assert "ICECC_P50_PROFILE=OFF" in start.argv
    assert "ICECC_P50_MODE=off" not in start.argv


def test_scheduler_kill_switch_cycle_preserves_exact_state_chain(tmp_path: Path) -> None:
    farm, scenario, plan = _fixture(tmp_path)
    document = _transition_image_document(farm, "p50s4-57a1e336")
    identity = _image_identity(CommandResult(0, json.dumps(document), ""), "cycle")
    farm.data["authority"]["images"]["p50s4-57a1e336"][
        "closure_sha256"
    ] = identity.closure_sha256
    plan = farmtest.build_plan(farm, scenario, run_id="event-unit")
    scenario.data["timeline"] = [
        {
            "trigger": "t+0",
            "action": "env_set",
            "instance": "S1",
            "env": {"ICECC_P50_PROFILE": "OFF"},
        },
        {
            "trigger": "t+0",
            "action": "env_set",
            "instance": "S1",
            "env": {"ICECC_P50_PROFILE": "P29V1"},
        },
    ]
    recorder = TransitionRecorder(farm, plan)
    records = run_events(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(recorder),
        event_path=tmp_path / "events" / "events.json",
        deadline_s=2,
    )

    assert [record.event_epoch for record in records] == [1, 2]
    assert records[0].receipt["before"]["env"] == {
        "ICECC_P50_PROFILE": "P29V1"
    }
    assert records[0].receipt["after"]["env"] == {
        "ICECC_P50_PROFILE": "OFF"
    }
    assert records[1].receipt["before"] == records[0].receipt["after"]
    assert records[1].receipt["after"]["env"] == {
        "ICECC_P50_PROFILE": "P29V1"
    }
    starts = [
        command
        for command in recorder.commands
        if command.phase == "event.env_set.start"
    ]
    assert len(starts) == 2
    assert "ICECC_P50_PROFILE=OFF" in starts[0].argv
    assert "ICECC_P50_PROFILE=P29V1" in starts[1].argv


def test_b6_delayed_poll_requires_a_new_dispatch_before_restore(
    tmp_path: Path,
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S70-b6-kill-switch.json", farm
    )
    label = scenario.data["images"]["new"]
    identity = _image_identity(
        CommandResult(0, json.dumps(_transition_image_document(farm, label)), ""),
        "b6",
    )
    farm.data["authority"]["images"][label][
        "closure_sha256"
    ] = identity.closure_sha256
    plan = farmtest.build_plan(farm, scenario, run_id="event-unit")

    class B6Recorder(CoordinatedTransitionRecorder):
        def invoke(self, command: PlannedCommand) -> CommandResult:
            result = super().invoke(command)
            if command.phase in {
                "event.pause-drain",
                "event.resume",
                "event.abort-resume",
            }:
                document = json.loads(result.stdout)
                document["epoch"] = int(command.argv[-2])
                return CommandResult(0, json.dumps(document), result.stderr)
            if command.phase == "event.client-scheduler-ready":
                return CommandResult(
                    0,
                    json.dumps(
                        {
                            "bytes": 100,
                            "cache_line": "cache sidecar adapter state=2 lifecycle=3",
                            "cache_required": True,
                            "connected_line": (
                                "Connected to scheduler (I am known as C1)"
                            ),
                            "ready": True,
                        }
                    ),
                    "",
                )
            return result

    four = "\n".join(f"put {job} in joblist of F1" for job in range(1, 5))
    five = four + "\nput 5 in joblist of F1"
    reads = iter(("", four, four, five))
    producer = EventProducer(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(B6Recorder(farm, plan)),
        job_reader=lambda: next(reads, five),
        event_path=tmp_path / "events" / "events.json",
        deadline_s=2,
        poll_interval_s=0.01,
        wall_ms=lambda: 1000,
    )
    producer.signal_turn_start("A")
    producer.start()
    producer.wait()
    producer.stop()

    assert [record.workload_dispatch_count for record in producer.records] == [4, 5]
    assert [record.receipt["after"]["env"]["ICECC_P50_PROFILE"] for record in producer.records] == [
        "OFF",
        "P29V1",
    ]


def test_client_fault_env_set_is_exact_and_preserves_mode(tmp_path: Path) -> None:
    farm, scenario, plan = _fixture(tmp_path)
    document = _transition_image_document(farm, "p50s4-57a1e336")
    identity = _image_identity(CommandResult(0, json.dumps(document), ""), "fault-env")
    farm.data["authority"]["images"]["p50s4-57a1e336"][
        "closure_sha256"
    ] = identity.closure_sha256
    plan = farmtest.build_plan(farm, scenario, run_id="event-unit")
    scenario.data["timeline"] = [
        {
            "trigger": "t+0",
            "action": "env_set",
            "instance": "C1",
            "env": {
                "ICECC_P50_FAULT_INJECTION": "P29_INTERNER_FAIL_ONCE"
            },
        }
    ]
    recorder = TransitionRecorder(farm, plan)
    records = run_events(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(recorder),
        event_path=tmp_path / "events" / "events.json",
        deadline_s=2,
    )
    expected = {
        "ICECC_P50_FAULT_INJECTION": "P29_INTERNER_FAIL_ONCE",
        "ICECC_P50_MODE": "on",
    }
    assert records[0].receipt["before"]["env"] == {"ICECC_P50_MODE": "on"}
    assert records[0].receipt["after"]["env"] == expected
    start = next(
        command
        for command in recorder.commands
        if command.phase == "event.env_set.start"
    )
    assert "ICECC_P50_FAULT_INJECTION=P29_INTERNER_FAIL_ONCE" in start.argv
    assert "ICECC_P50_MODE=on" in start.argv
    assert _event_log(tmp_path, scenario, farm=farm, plan=plan)[0]["receipt"][
        "after"
    ]["env"] == expected


def test_same_host_same_label_preflight_is_scoped_by_role(tmp_path: Path) -> None:
    farm, scenario, plan = _fixture(tmp_path)
    identity = _image_identity(
        CommandResult(0, json.dumps(_transition_image_document(farm, "p50s4-57a1e336")), ""),
        "same-role",
    )
    farm.data["authority"]["images"]["p50s4-57a1e336"]["closure_sha256"] = identity.closure_sha256
    plan = farmtest.build_plan(farm, scenario, run_id="event-unit")
    scenario.data["timeline"] = [
        {"trigger": "t+0", "action": "env_set", "instance": "S1", "env": {"ICECC_P50_PROFILE": "OFF"}},
        {"trigger": "t+0", "action": "env_set", "instance": "C1", "env": {"ICECC_P50_MODE": "off"}},
    ]
    recorder = TransitionRecorder(farm, plan)
    records = run_events(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(recorder),
        event_path=tmp_path / "events" / "events.json",
        deadline_s=2,
    )
    assert len(records) == 2
    role_probes = [item for item in recorder.commands if item.phase == "preflight.role-hashes"]
    assert len(role_probes) == 2
    assert any("icecc-scheduler" in " ".join(item.argv) for item in role_probes)
    assert any("icecc" in " ".join(item.argv) and "icecc-scheduler" not in " ".join(item.argv) for item in role_probes)
    assert _event_log(tmp_path, scenario, farm=farm, plan=plan)[1]["receipt"]["after"]["env"] == {
        "ICECC_P50_MODE": "off"
    }


@pytest.mark.parametrize(
    "action,side,key",
    (
        ("upgrade", "before", "container_id"),
        ("upgrade", "after", "image"),
        ("upgrade", "after", "env"),
        ("upgrade", "after", "closure_sha256"),
        ("upgrade", "after", "role_sha256"),
        ("downgrade", "before", "container_id"),
        ("downgrade", "after", "image"),
        ("downgrade", "after", "env"),
        ("downgrade", "after", "closure_sha256"),
        ("downgrade", "after", "role_sha256"),
    ),
)
def test_transition_receipt_tampering_is_rejected(
    tmp_path: Path, action: str, side: str, key: str
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(INTEGRATION / "scenarios" / "S20-scheduler-first.json", farm)
    if action == "downgrade":
        next(item for item in scenario.data["instances"] if item["name"] == "F1")["image"] = "new"
    scenario.data["timeline"] = [
        {
            "trigger": "t+0",
            "action": action,
            "instance": "F1",
            "image": "new" if action == "upgrade" else "old",
        }
    ]
    old_document = _transition_image_document(farm, "p43-1.4.0")
    new_document = _transition_image_document(farm, "p50s4-57a1e336")
    farm.data["authority"]["images"]["p43-1.4.0"]["closure_sha256"] = _image_identity(
        CommandResult(0, json.dumps(old_document), ""), "old"
    ).closure_sha256
    farm.data["authority"]["images"]["p50s4-57a1e336"]["closure_sha256"] = _image_identity(
        CommandResult(0, json.dumps(new_document), ""), "new"
    ).closure_sha256
    plan = farmtest.build_plan(farm, scenario, run_id="event-unit")
    run_events(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(TransitionRecorder(farm, plan)),
        event_path=tmp_path / "events" / "events.json",
        deadline_s=2,
    )
    path = tmp_path / "events" / "events.json"
    document = json.loads(path.read_text(encoding="utf-8"))
    value = document["events"][0]["receipt"][side][key]
    document["events"][0]["receipt"][side][key] = (
        {"ICECC_P50_PROFILE": "tampered"}
        if isinstance(value, dict)
        else ("tampered-id" if key == "container_id" else "f" * 64)
    )
    path.write_text(json.dumps(document), encoding="utf-8")
    with pytest.raises(CollectError):
        _event_log(tmp_path, scenario, farm=farm, plan=plan)


@pytest.mark.parametrize(
    "attribute",
    (
        "before_running",
        "after_running",
        "omit_current_mount",
        "omit_current_env",
        "omit_target_mount",
        "omit_target_env",
    ),
)
def test_post_start_inspect_and_running_are_required(tmp_path: Path, attribute: str) -> None:
    farm, scenario, plan = _fixture(tmp_path)
    identity = _image_identity(
        CommandResult(0, json.dumps(_transition_image_document(farm, "p50s4-57a1e336")), ""),
        "inspect",
    )
    farm.data["authority"]["images"]["p50s4-57a1e336"]["closure_sha256"] = identity.closure_sha256
    plan = farmtest.build_plan(farm, scenario, run_id="event-unit")
    scenario.data["timeline"] = [
        {"trigger": "t+0", "action": "env_set", "instance": "S1", "env": {"ICECC_P50_PROFILE": "OFF"}}
    ]
    recorder = TransitionRecorder(farm, plan)
    setattr(recorder, attribute, False if attribute.endswith("running") else True)
    with pytest.raises(EventError):
        run_events(
            farm,
            scenario,
            plan,
            recorder=RecordingTransport(recorder),
            event_path=tmp_path / "events" / "events.json",
            deadline_s=2,
        )


def _checkpoint_fixture(client: str = "C1") -> dict[str, object]:
    value: dict[str, object] = {
        "checkpoint_sha256": "",
        "client": client,
        "completed_rows": [
            {"index": 1, "path": "jobs/000001/result.tsv", "sha256": "b" * 64}
        ],
        "completed_rows_sha256": "",
        "expected_jobs": 2,
        "schema": "icefarm-workload-checkpoint-v1",
        "status": "QUIESCED",
        "turn": "A",
        "worklist_sha256": "d" * 64,
    }
    value["completed_rows_sha256"] = hashlib.sha256(
        json.dumps(value["completed_rows"], sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    body = dict(value)
    body.pop("checkpoint_sha256")
    value["checkpoint_sha256"] = hashlib.sha256(
        json.dumps(body, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    return value


def _resign_checkpoint(value: dict[str, object]) -> dict[str, object]:
    value["completed_rows_sha256"] = hashlib.sha256(
        json.dumps(value["completed_rows"], sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    body = dict(value)
    body.pop("checkpoint_sha256")
    value["checkpoint_sha256"] = hashlib.sha256(
        json.dumps(body, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    return value


class CheckpointClientTransitionRecorder(TransitionRecorder):
    client_cache_required = True

    def invoke(self, command: PlannedCommand) -> CommandResult:
        result = super().invoke(command)
        if command.phase in {
            "event.quiesce-drain",
            "event.resume",
            "event.abort-resume",
        }:
            action, status = {
                "event.quiesce-drain": ("quiesce", "QUIESCED"),
                "event.resume": ("resume", "OPEN"),
                "event.abort-resume": ("abort", "ABORT"),
            }[command.phase]
            started_ms = 900 if action == "quiesce" else 1100
            return CommandResult(
                0,
                json.dumps(
                    {
                        "action": action,
                        "active_after": 0,
                        "active_before": 1 if action == "quiesce" else 0,
                        "client": command.instance,
                        "epoch": int(command.argv[-2]),
                        "finished_ms": started_ms + 10,
                        "schema": "icefarm-event-gate-v1",
                        "started_ms": started_ms,
                        "status": status,
                        "turn": "A",
                    }
                ),
                "",
            )
        if command.phase == "event.client-scheduler-ready":
            return CommandResult(
                0,
                json.dumps(
                    {
                        "bytes": 100,
                        "cache_line": (
                            "cache sidecar adapter state=2 lifecycle=3"
                            if self.client_cache_required
                            else None
                        ),
                        "cache_required": self.client_cache_required,
                        "connected_line": (
                            "Connected to scheduler (I am known as 10.0.0.1)"
                        ),
                        "ready": True,
                    }
                ),
                "",
            )
        return result


def _b5_row(
    job: int,
    *,
    worker: str = "F1",
    epoch: int,
    profile: str | None,
    retries: int = 0,
) -> dict[str, object]:
    tail = profile is not None
    return {
        "c_to_f_bytes": 1024,
        "client_instance": "C1",
        "client_version": 50,
        "cs": worker,
        "cs_version": 50,
        "event_epoch": epoch,
        "exact": True,
        "f_to_c_bytes": 512,
        "job_id": str(job),
        "object_sha_local": "a" * 64,
        "object_sha_remote": "a" * 64,
        "retries": retries,
        "reuse": False if profile == "P29V1" else None,
        "schema": ROW_SCHEMA,
        "session_outcome": "committed" if tail else "none",
        "tail_present": tail,
        "tail_profile": profile,
        "tu": f"files/tu-{job}.ii",
        "wall_ms": 25,
    }


def test_job_triggered_client_fault_uses_checkpoint_path_and_passes_b5_verdict(
    tmp_path: Path,
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S70-b5-interner-failure.json", farm
    )
    label = scenario.data["images"]["new"]
    document = _transition_image_document(farm, label)
    identity = _image_identity(CommandResult(0, json.dumps(document), ""), "b5")
    farm.data["authority"]["images"][label][
        "closure_sha256"
    ] = identity.closure_sha256
    plan = farmtest.build_plan(farm, scenario, run_id="event-unit")

    checkpoint_payload = b"authenticated checkpoint row\n"
    checkpoint_path = (
        tmp_path
        / "instances"
        / "C1"
        / "results"
        / "workload"
        / "A"
        / "jobs"
        / "000001"
        / "result.tsv"
    )
    checkpoint_path.parent.mkdir(parents=True)
    checkpoint_path.write_bytes(checkpoint_payload)
    checkpoint = _checkpoint_fixture()
    checkpoint["completed_rows"][0]["sha256"] = hashlib.sha256(
        checkpoint_payload
    ).hexdigest()
    _resign_checkpoint(checkpoint)
    callback_calls: list[str] = []

    def quiesce(turn, clients):
        callback_calls.append("quiesce:" + turn)
        assert [item["name"] for item in clients] == ["C1"]
        return {"C1": checkpoint}

    def relaunch(turn, checkpoints):
        callback_calls.append("relaunch:" + turn)
        assert checkpoints == {"C1": checkpoint}
        return {
            "C1": {
                "client": "C1",
                "expected_jobs": 2,
                "failures": 0,
                "jobs": 2,
                "status": "COMPLETE",
            }
        }

    recorder = CheckpointClientTransitionRecorder(farm, plan)
    reads = iter(("", "put 1 in joblist of F1\n"))
    producer = EventProducer(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(recorder),
        job_reader=lambda: next(reads, "put 1 in joblist of F1\n"),
        event_path=tmp_path / "events" / "events.json",
        deadline_s=2,
        poll_interval_s=0.01,
        wall_ms=lambda: 1000,
        quiesce_workload=quiesce,
        relaunch_workload=relaunch,
    )
    producer.signal_turn_start("A")
    producer.start()
    producer.wait()
    producer.stop()

    assert callback_calls == ["quiesce:A", "relaunch:A"]
    assert len(producer.records) == 1
    event = producer.records[0].as_dict()
    assert event["receipt"]["schema"] == CLIENT_TRANSITION_SCHEMA
    assert event["receipt"]["after"]["env"] == {
        "ICECC_P50_FAULT_INJECTION": "P29_INTERNER_FAIL_ONCE",
        "ICECC_P50_MODE": "on",
    }
    assert _event_log(tmp_path, scenario, farm=farm, plan=plan) == [event]

    # Collection must authenticate the event structure before it knows which
    # instances to snapshot, but the checkpoint row can only be authenticated
    # after the result trees have been copied into staging.  Exercise that
    # exact two-pass path with a partial checkpoint (one of two jobs).
    (tmp_path / "diagnostics").mkdir()
    for item in plan["topology"]["instances"]:
        if item["name"] != "C1":
            (tmp_path / f"{item['name']}.results").mkdir()
    client = next(
        item for item in plan["topology"]["instances"] if item["name"] == "C1"
    )
    witness = event["receipt"]["client_readiness"]
    client_log = (
        tmp_path
        / "diagnostics"
        / client["host"]
        / "C1.log"
        / "client-daemon.log"
    )
    client_log.parent.mkdir(parents=True)
    client_log.write_bytes(
        b"x" * witness["offset"]
        + (
            event["receipt"]["readiness"]["line"]
            + "\n"
            + witness["connected_line"]
            + "\n"
            + witness["cache_line"]
            + "\n"
        ).encode()
    )
    evidence = _stage_evidence(
        farm,
        scenario,
        plan,
        tmp_path,
        {},
        recorder=None,
        sync_remote=False,
    )
    staged_checkpoint = (
        evidence
        / "instances"
        / "C1"
        / "results"
        / "workload"
        / "A"
        / "jobs"
        / "000001"
        / "result.tsv"
    )
    assert staged_checkpoint.read_bytes() == checkpoint_payload
    assert _event_log(evidence, scenario, farm=farm, plan=plan) == [event]
    staged_checkpoint.unlink()
    with pytest.raises(CollectError, match="invalid transition coordination"):
        _event_log(evidence, scenario, farm=farm, plan=plan)

    rows = [
        _b5_row(1, epoch=0, profile="P29V1"),
        _b5_row(2, epoch=1, profile=None, retries=1),
        _b5_row(3, epoch=1, profile="ZSTD_TU"),
        _b5_row(4, worker="F2", epoch=1, profile="ZSTD_TU"),
    ]
    observations = {
        "cell_wall_ms": 2000,
        "compile_failure_job_ids": [],
        "error106_job_ids": ["2"],
        "incomplete_turns": [],
        "job_lifecycle": [
            {
                "deadline_ms": dispatch_ms + 120_000,
                "dispatch_ms": dispatch_ms,
                "job_id": str(index),
                "terminal": "completion",
                "terminal_ms": dispatch_ms + 25,
                "turn": "A",
            }
            for index, dispatch_ms in enumerate((0, 1100, 1200, 1300), start=1)
        ],
        "local_fallback_job_ids": [],
        "logins": [
            {
                "cache_profiles": ["P29V1", "ZSTD_TU", "ZSTD_ROUTE"],
                "instance": worker,
                "protocol": 50,
            }
            for worker in ("F1", "F2")
        ],
        "oracle": {"sample_mismatch_job_ids": [], "sample_total": 1},
        "p29_interner_faults": [
            {
                "client_instance": "C1",
                "fault": "p29-interner-fail-once",
                "outcome": "fired",
                "schema": "icecream-p50-fault-v1",
            }
        ],
        "sidecars": {
            "C1": {"sessions": 0},
            "F1": {"cache_ports": [24000], "process_count": 1, "sessions": 2},
            "F2": {"cache_ports": [24001], "process_count": 1, "sessions": 1},
        },
        "wire_revisions": {"C1": 1, "F1": 1, "F2": 1},
    }
    bundle = {
        "event_log": [event],
        "observations": observations,
        "rows": rows,
        "scenario": scenario.data,
        "schema": BUNDLE_SCHEMA,
    }
    assert evaluate_bundle(bundle)["status"] == "PASS"


@pytest.mark.parametrize(
    ("scenario_name", "cache_required"),
    (("S60-04-c1-up.json", True), ("S60-07-c1-down.json", False)),
)
def test_client_transition_readiness_uses_target_image_generation(
    tmp_path: Path, scenario_name: str, cache_required: bool
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / scenario_name, farm
    )
    target_alias = scenario.data["timeline"][0]["image"]
    label = scenario.data["images"][target_alias]
    document = _transition_image_document(farm, label)
    identity = _image_identity(
        CommandResult(0, json.dumps(document), ""), "client-transition"
    )
    farm.data["authority"]["images"][label][
        "closure_sha256"
    ] = identity.closure_sha256
    plan = farmtest.build_plan(farm, scenario, run_id="event-unit")
    checkpoints = {
        name: _checkpoint_fixture(name)
        for name in scenario.data["workload"]["clients"]
    }

    def quiesce(turn, clients):
        assert turn == "A"
        assert [item["name"] for item in clients] == ["C1", "C2"]
        return checkpoints

    def relaunch(turn, received):
        assert turn == "A"
        assert received == checkpoints
        return {
            name: {
                "client": name,
                "expected_jobs": 2,
                "failures": 0,
                "jobs": 2,
                "status": "COMPLETE",
            }
            for name in checkpoints
        }

    recorder = CheckpointClientTransitionRecorder(farm, plan)
    recorder.client_cache_required = cache_required
    dispatched = "".join(
        f"put {index} in joblist of F1\n" for index in range(1, 25)
    )
    reads = iter(("", dispatched))
    producer = EventProducer(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(recorder),
        job_reader=lambda: next(reads, dispatched),
        event_path=tmp_path / "events" / "events.json",
        deadline_s=2,
        poll_interval_s=0.01,
        wall_ms=lambda: 1000,
        quiesce_workload=quiesce,
        relaunch_workload=relaunch,
    )
    producer.signal_turn_start("A")
    producer.start()
    producer.wait()
    producer.stop()

    assert len(producer.records) == 1
    readiness = producer.records[0].receipt["client_readiness"]
    assert readiness["cache_required"] is cache_required
    assert readiness["cache_line"] == (
        "cache sidecar adapter state=2 lifecycle=3" if cache_required else None
    )


def test_job_triggered_client_env_set_requires_callbacks_and_is_unique(
    tmp_path: Path,
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S70-b5-interner-failure.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="event-unit")
    with pytest.raises(UnsupportedEvent, match="checkpointed workload driver"):
        EventProducer(
            farm,
            scenario,
            plan,
            recorder=RecordingTransport(EventRecorder()),
        )

    scenario.data["timeline"].append(
        {
            "trigger": "job 2",
            "action": "env_set",
            "instance": "C1",
            "env": {"ICECC_P50_MODE": "on"},
        }
    )
    with pytest.raises(UnsupportedEvent, match="multiple checkpointed C events"):
        EventProducer(
            farm,
            scenario,
            plan,
            recorder=RecordingTransport(EventRecorder()),
            quiesce_workload=lambda _turn, _clients: {},
            relaunch_workload=lambda _turn, _checkpoints: {},
        )


def test_checkpoint_receipt_rejects_wrong_identity_missing_and_duplicate_rows() -> None:
    valid = _checkpoint_fixture()
    assert EventProducer._checkpoint_receipt(valid, client="C1", turn="A") == valid
    with pytest.raises(EventError):
        EventProducer._checkpoint_receipt(valid, client="C2", turn="A")
    missing = dict(valid, completed_rows=[])
    with pytest.raises(EventError):
        EventProducer._checkpoint_receipt(missing, client="C1", turn="A")
    duplicate = dict(valid, completed_rows=valid["completed_rows"] * 2)
    with pytest.raises(EventError):
        EventProducer._checkpoint_receipt(duplicate, client="C1", turn="A")
    tampered_rows = dict(valid, completed_rows=[dict(valid["completed_rows"][0], index=2)])
    with pytest.raises(EventError):
        EventProducer._checkpoint_receipt(tampered_rows, client="C1", turn="A")
    mismatched_path = _resign_checkpoint(
        dict(valid, completed_rows=[dict(valid["completed_rows"][0], index=2)])
    )
    with pytest.raises(EventError):
        EventProducer._checkpoint_receipt(mismatched_path, client="C1", turn="A")
    tampered_receipt = dict(valid, checkpoint_sha256="e" * 64)
    with pytest.raises(EventError):
        EventProducer._checkpoint_receipt(tampered_receipt, client="C1", turn="A")


def test_checkpoint_gate_and_resume_protocol_are_explicit() -> None:
    assert CLIENT_TRANSITION_SCHEMA == "icefarm-client-transition-v1"
    assert '"pause", "quiesce", "resume", "abort"' in GATE_CONTROL_SCRIPT
    from farmharness.integration.workload import MANIFEST_DRIVER

    assert 'QUIESCE)' in MANIFEST_DRIVER
    assert 'write_checkpoint' in MANIFEST_DRIVER
    assert 'resume_mode' in MANIFEST_DRIVER


def test_event_failure_descriptor_is_retained_before_success_persistence(tmp_path: Path) -> None:
    farm, scenario, plan = _fixture(tmp_path)
    scenario.data["timeline"] = [
        {"trigger": "t+0", "action": "kill -9", "instance": "F1"}
    ]

    class FailingRecorder(EventRecorder):
        def invoke(self, command: PlannedCommand) -> CommandResult:
            if command.phase == "event.kill--9":
                self.commands.append(command)
                return CommandResult(23, "", "injected failure")
            return super().invoke(command)

    event_path = tmp_path / "events" / "events.json"
    with pytest.raises(EventError, match="failed"):
        run_events(
            farm,
            scenario,
            plan,
            recorder=RecordingTransport(FailingRecorder()),
            event_path=event_path,
            deadline_s=2,
        )
    failure = json.loads((event_path.parent / "failure.json").read_text())
    assert failure["schema"] == "icefarm-event-failure-v1"
    assert failure["event_index"] == 0
    assert failure["last_completed_epoch"] == 0
    assert failure["phase"] == "event.kill--9"
