from __future__ import annotations

import copy
import hashlib
import json
from collections import defaultdict

import pytest

from farmharness.integration.verdict import (
    BUNDLE_SCHEMA,
    CONTROL_VERDICT_SCHEMA,
    F_INIT_LAUNCH_CONTRACT,
    ROW_SCHEMA,
    SCHEDULER_DISPATCH_EPOCH_CONTRACT,
    VERDICT_SCHEMA,
    _authenticated_active_loss_fallback_ids,
    _authenticated_strict_p50_retry_ids,
    _scenario_profile_at_epoch,
    _scheduler_dispatch_epoch,
    _shape_clauses,
    _s60_transition_epoch_errors,
    evaluate_bundle,
    evaluate_control,
    _transition_receipt_errors,
    _scheduler_active_loss_receipt_errors,
)


SHA_A = "a" * 64
SHA_B = "b" * 64
SHA_C = "c" * 64


def _coordinated_transition_event() -> tuple[dict[str, object], dict[str, object], dict[str, object]]:
    scenario = _scenario("S'CF", client_versions=(43,), worker_versions=(43,))
    scenario["timeline"] = [
        {"trigger": "job 1", "action": "upgrade", "instance": "F1", "image": "new"}
    ]
    gate = {
        "action": "pause",
        "active_after": 0,
        "active_before": 1,
        "client": "C1",
        "epoch": 1,
        "finished_ms": 100,
        "schema": "icefarm-event-gate-v1",
        "started_ms": 99,
        "status": "PAUSED",
        "turn": "A",
    }
    resume = dict(gate, action="resume", status="OPEN", started_ms=1000, finished_ms=1001)
    receipt = {
        "action": "upgrade",
        "instance": "F1",
        "event_epoch": 1,
        "schema": "icefarm-transition-v2",
        "turn": "A",
        "before": {
            "container_id": SHA_A,
            "closure_sha256": SHA_A,
            "env": {},
            "image": "p43-fixture",
            "role_sha256": SHA_A,
        },
        "after": {
            "container_id": SHA_B,
            "closure_sha256": SHA_B,
            "env": {},
            "image": "p50s4-fixture",
            "role_sha256": SHA_B,
        },
        "preflight": {
            "image_closure_sha256": SHA_B,
            "role_sha256": SHA_B,
            "runtime_path": "/runtimes/" + SHA_B + "/root",
        },
        "readiness": {
            "host": "h1",
            "line": "ICECREAM daemon F1 starting up",
            "log_path": "/run/F1/log/iceccd.log",
            "offset": 10,
            "role": "F",
        },
        "coordination": {
            "clients": {"C1": gate},
            "resume": {"C1": resume},
            "ready_ms": 1000,
            "scheduler_snapshot": "F1\n",
            "worker_snapshot": "F1\n",
            "workers": ["F1"],
            "scheduler_worker_rejoin": {
                "bytes": 40,
                "host": "h1",
                "line": "login F1 protocol version: 50",
                "log_path": "/run/S1/log/scheduler.log",
                "offset": 12,
                "role_protocol": 50,
                "target": "F1",
            },
        },
    }
    observed = {
        "action": "upgrade",
        "instance": "F1",
        "event_epoch": 1,
        "event_index": 0,
        "fired_ms": 1000,
        "trigger": "job 1",
        "receipt": receipt,
    }
    return scenario, observed, receipt


def test_pure_verdict_transition_receipt_requires_fresh_f_rejoin() -> None:
    scenario, observed, _receipt = _coordinated_transition_event()
    assert not _transition_receipt_errors(observed, scenario["timeline"][0], scenario)
    tampered = copy.deepcopy(observed)
    tampered["receipt"]["coordination"]["scheduler_worker_rejoin"]["line"] = "stale login"
    assert _transition_receipt_errors(tampered, scenario["timeline"][0], scenario)


def test_s60_worker_upgrade_keeps_only_untouched_workers_under_legacy_surface() -> None:
    scenario, observed, receipt = _coordinated_transition_event()
    scenario["id"] = "S60-02-f1-up"
    scenario["instances"].insert(2, _instance("F2", "F", 43))
    receipt["coordination"]["workers"] = ["F1", "F2"]
    receipt["coordination"]["worker_snapshot"] = "F1\nF2\n"
    rows = [
        _row(1, client_version=43, worker_version=43, tail=False, outcome="none"),
        _row(
            2,
            client_version=43,
            worker_version=50,
            tail=False,
            outcome="none",
        ),
        _row(
            3,
            client_version=43,
            worker="F2",
            worker_version=43,
            tail=False,
            outcome="none",
        ),
    ]
    rows[1]["event_epoch"] = 1
    rows[2]["event_epoch"] = 1
    observations = _observations(rows, old_workers={"F2"})

    clauses = _shape_clauses(
        scenario, rows, observations, "P29V1", [observed]
    )
    assert next(
        item for item in clauses if item["id"] == "event.transition-0"
    )["status"] == "PASS"
    assert next(
        item for item in clauses if item["id"] == "shape.legacy-surface"
    )["status"] == "PASS"
    assert not any(
        item["id"] == "shape.scheduler-first-restart" for item in clauses
    )

    unsafe_target = copy.deepcopy(observed)
    unsafe_target["receipt"]["after"]["container_id"] = unsafe_target["receipt"][
        "before"
    ]["container_id"]
    clauses = _shape_clauses(
        scenario, rows, observations, "P29V1", [unsafe_target]
    )
    assert next(
        item for item in clauses if item["id"] == "event.transition-0"
    )["status"] == "FAIL"
    assert next(
        item for item in clauses if item["id"] == "shape.legacy-surface"
    )["status"] == "FAIL"

    broken_other = copy.deepcopy(observations)
    broken_other["sidecars"]["F2"]["process_count"] = 1
    clauses = _shape_clauses(
        scenario, rows, broken_other, "P29V1", [observed]
    )
    legacy = next(
        item for item in clauses if item["id"] == "shape.legacy-surface"
    )
    assert legacy["status"] == "FAIL"
    assert legacy["offending_job_ids"] == ["@instance:F2"]


def test_s60_worker_downgrade_keeps_untouched_new_worker_surface() -> None:
    scenario, observed, receipt = _coordinated_transition_event()
    scenario["id"] = "S60-11-warm-f2-down"
    scenario["shape"] = "S'C'F'"
    scenario["instances"][1]["image"] = "new"
    scenario["instances"].insert(2, _instance("F2", "F", 50))
    scenario["timeline"] = [
        {"trigger": "job 1", "action": "downgrade", "instance": "F2", "image": "old"}
    ]
    observed["action"] = "downgrade"
    observed["instance"] = "F2"
    receipt["action"] = "downgrade"
    receipt["instance"] = "F2"
    receipt["before"]["image"] = "p50s4-fixture"
    receipt["after"]["image"] = "p43-fixture"
    receipt["coordination"]["workers"] = ["F1", "F2"]
    receipt["coordination"]["worker_snapshot"] = "F1\nF2\n"
    receipt["coordination"]["scheduler_worker_rejoin"].update(
        {"line": "login F2 protocol version: 43", "role_protocol": 43, "target": "F2"}
    )
    rows = [
        _row(1, worker="F2"),
        _row(2, worker="F2", worker_version=43, tail=False, outcome="none"),
        _row(3, worker="F1"),
    ]
    rows[1]["event_epoch"] = 1
    rows[2]["event_epoch"] = 1
    observations = _observations(rows, old_workers={"F2"})

    clauses = _shape_clauses(
        scenario, rows, observations, "P29V1", [observed]
    )
    assert next(
        item for item in clauses if item["id"] == "event.transition-0"
    )["status"] == "PASS"
    assert next(
        item for item in clauses if item["id"] == "shape.full-newgen-engagement"
    )["status"] == "PASS"

    unsafe_target = copy.deepcopy(observed)
    unsafe_target["receipt"]["after"]["container_id"] = unsafe_target["receipt"][
        "before"
    ]["container_id"]
    clauses = _shape_clauses(
        scenario, rows, observations, "P29V1", [unsafe_target]
    )
    assert next(
        item for item in clauses if item["id"] == "event.transition-0"
    )["status"] == "FAIL"
    full_newgen = next(
        item for item in clauses if item["id"] == "shape.full-newgen-engagement"
    )
    assert full_newgen["status"] == "FAIL"
    assert full_newgen["offending_job_ids"] == ["@instance:F2"]

    broken_other = copy.deepcopy(observations)
    next(
        item for item in broken_other["logins"] if item["instance"] == "F1"
    )["cache_profiles"] = []
    clause = next(
        item
        for item in _shape_clauses(
            scenario, rows, broken_other, "P29V1", [observed]
        )
        if item["id"] == "shape.full-newgen-engagement"
    )
    assert clause["status"] == "FAIL"
    assert clause["offending_job_ids"] == ["@instance:F1"]


@pytest.mark.parametrize(
    "mutation",
    (None, "missing-capability", "unexpected-session", "missing-login"),
)
def test_full_newgen_legacy_off_requires_capabilities_and_zero_sessions(
    mutation: str | None,
) -> None:
    scenario = _scenario("S'C'F'")
    scheduler = next(
        item for item in scenario["instances"] if item["role"] == "S"
    )
    scheduler["env"] = {"ICECC_P50_PROFILE": "OFF"}
    scenario["expect"]["reuse"] = "none-when-legacy"
    rows = [_row(1, tail=False, outcome="none")]
    observations = _observations(rows)

    if mutation == "missing-capability":
        observations["logins"][0]["cache_profiles"].remove("ZSTD_ROUTE")
    elif mutation == "unexpected-session":
        observations["sidecars"]["F1"]["sessions"] = 1
    elif mutation == "missing-login":
        observations["logins"] = []

    clause = next(
        item
        for item in _shape_clauses(
            scenario, rows, observations, None, []
        )
        if item["id"] == "shape.full-newgen-engagement"
    )
    expected = "PASS" if mutation is None else "FAIL"
    assert clause["status"] == expected
    assert clause["offending_job_ids"] == (
        [] if mutation is None else ["@instance:F1"]
    )


def test_active_loss_verdict_rejects_reused_compiler_identity() -> None:
    scenario = {"workload": {"turns": ["A"], "clients": ["C1"]}}
    event = {"action": "scheduler-loss-active", "instance": "S1", "last_dispatched_job": 2}
    receipt = {
        "schema": "icefarm-scheduler-active-loss-v1",
        "action": "scheduler-loss-active",
        "instance": "S1",
            "event_epoch": 1,
            "lost_scheduler_generation": 1,
            "lost_scheduler_job": 2,
        "pre_fault": {"scheduler_log": {}, "worker_log": {}},
            "compiler": {
                "daemon": {"pid": 10, "pgid": 10, "ppid": 1, "exe": "/opt/icecream/sbin/iceccd"},
                "assignment": {"schema": "icefarm-compiler-assignment-v1", "child": {"pid": 41, "pgid": 41, "generation": 1, "kind": 0, "owning_client_id": 7}, "client": {"client_id": 7, "scheduler_job_id": 2, "job_id": 2}, "listener": {"host": "127.0.0.1", "port": 8765}},
            "leader": {"pid": 41, "pgid": 41, "ppid": 10, "start_ticks": 9},
            "stopped": {"pid": 41, "pgid": 41, "ppid": 10, "start_ticks": 9},
            "group_gone": {"gone": True},
            "worker_before": {"container_id": "b" * 64, "started_at": "f"},
            "worker_after": {"container_id": "b" * 64, "started_at": "f"},
        },
        "quiescence": {
                "client_readiness": {"C1": {"bytes": 1, "cache_line": None, "cache_required": False, "connected_line": "Connected to scheduler (I am known as C1)", "host": "tt-quietbox2", "log_path": "/scratch/C1/log/client-daemon.log", "offset": 0}},
                "client_routes": {"C1": {"before": {"container": {"container_id": "c" * 64, "started_at": "same", "running": True}, "daemon": {"argv": ["/opt/icecream/sbin/iceccd"], "exe": "/opt/icecream/sbin/iceccd", "exe_evidence": "proc-exe", "pid": 10, "ppid": 1, "start_ticks": 1, "uid": 0}, "route_owner": {"argv": ["/opt/icecream/sbin/icecc-cache-service"], "exe": "/opt/icecream/sbin/icecc-cache-service", "exe_evidence": "proc-exe", "pid": 11, "ppid": 10, "start_ticks": 2, "uid": 0}}, "after": {"container": {"container_id": "c" * 64, "started_at": "same", "running": True}, "daemon": {"argv": ["/opt/icecream/sbin/iceccd"], "exe": "/opt/icecream/sbin/iceccd", "exe_evidence": "proc-exe", "pid": 10, "ppid": 1, "start_ticks": 1, "uid": 0}, "route_owner": {"argv": ["/opt/icecream/sbin/icecc-cache-service"], "exe": "/opt/icecream/sbin/icecc-cache-service", "exe_evidence": "proc-exe", "pid": 11, "ppid": 10, "start_ticks": 2, "uid": 0}}}},
            "scheduler_snapshot": "S1",
            "scheduler_startup": {"line": "ICECREAM scheduler starting"},
            "worker_snapshot": "F1",
        },
        "before": {"container_id": "a" * 64, "started_at": "old"},
        "after": {"container_id": "a" * 64, "started_at": "new"},
        "turn": "A",
    }
    assert not _scheduler_active_loss_receipt_errors(receipt, event, scenario)
    tampered = copy.deepcopy(receipt)
    tampered["compiler"]["stopped"]["start_ticks"] = 10
    assert _scheduler_active_loss_receipt_errors(tampered, event, scenario)
    tampered = copy.deepcopy(receipt)
    tampered["compiler"]["worker_after"]["container_id"] = "c" * 64
    assert _scheduler_active_loss_receipt_errors(tampered, event, scenario)
    tampered = copy.deepcopy(receipt)
    tampered["quiescence"].pop("worker_snapshot")
    assert _scheduler_active_loss_receipt_errors(tampered, event, scenario)
    for path in (
        ("quiescence", "client_routes", "C1", "before", "container", "container_id"),
        ("quiescence", "client_routes", "C1", "before", "container", "started_at"),
        ("quiescence", "client_routes", "C1", "before", "daemon", "pid"),
        ("quiescence", "client_routes", "C1", "before", "route_owner", "start_ticks"),
    ):
        tampered = copy.deepcopy(receipt)
        cursor = tampered
        for key in path[:-1]:
            cursor = cursor[key]
        cursor[path[-1]] = "bad" if path[-1] in {"container_id", "started_at"} else 999
        assert _scheduler_active_loss_receipt_errors(tampered, event, scenario)


def _active_loss_v3_fixture():
    route = {
        "container": {
            "container_id": "c" * 64,
            "running": True,
            "started_at": "client-start",
        },
        "daemon": {
            "argv": ["/opt/icecream/sbin/iceccd"],
            "exe": "/opt/icecream/sbin/iceccd",
            "exe_evidence": "proc-exe",
            "pid": 20,
            "ppid": 1,
            "start_ticks": 5,
            "uid": 0,
        },
        "route_owner": {
            "argv": ["/opt/icecream/sbin/icecc-cache-service"],
            "exe": "/opt/icecream/sbin/icecc-cache-service",
            "exe_evidence": "proc-exe",
            "pid": 21,
            "ppid": 20,
            "start_ticks": 6,
            "uid": 0,
        },
    }
    route_pair = {"after": route, "before": route}
    readiness = {
        "bytes": 200,
        "cache_expected": True,
        "cache_fresh": True,
        "cache_lifecycle": 3,
        "cache_line": "cache sidecar adapter state=2 lifecycle=3",
        "cache_line_offset": 100,
        "cache_state": 2,
        "connected_line": "Connected to scheduler (I am known as C1)",
        "connected_line_offset": 10,
        "host": "tt-quietbox2",
        "log_path": "/farm/icefarm/run/C1/log/client-daemon.log",
        "offset": 0,
        "post_sha256": "f" * 64,
        "route": route_pair,
        "schema": "icefarm-client-scheduler-readiness-v2",
    }
    closure = "d" * 64
    container_closure = "f" * 64
    container_image_id = "e" * 64
    authority_native_image_id = "9" * 64
    container_reference = "icecream/farm-node:test"
    runtime_path = f"/farm/icefarm/runtimes/{closure}/root"
    worker_identity = {
        "container_id": "b" * 64,
        "env": {"ICECC_WEB_HOSTPORT": "127.0.0.1:23004"},
        "image_id": container_image_id,
        "running": True,
        "runtime_path": runtime_path,
        "started_at": "worker-start",
    }
    scenario = {
        "images": {"new": "p50s4-current"},
        "instances": [
            {
                "env": {"ICECC_P50_MODE": "on"},
                "host": "tt-quietbox2",
                "image": "new",
                "name": "C1",
                "role": "C",
            }
        ],
        "timeline": [
            {
                "action": "scheduler-loss-active",
                "instance": "S1",
                "trigger": "job 2",
            }
        ],
        "workload": {"clients": ["C1"], "turns": ["A"]},
    }
    event = {
        "action": "scheduler-loss-active",
        "event_epoch": 1,
        "instance": "S1",
        "last_dispatched_job": 2,
    }
    plan = {
        "ports": {"web": {"F1": 23004}},
        "topology": {
            "instances": [
                {
                    "env": {},
                    "host": "tt-quietbox3",
                    "image": {"closure_sha256": closure},
                    "container_image": {
                        "closure_sha256": container_closure,
                        "reference": container_reference,
                    },
                    "name": "F1",
                    "role": "F",
                }
            ]
        },
    }
    farm = {
        "hosts": [{"name": "tt-quietbox3", "scratch_root": "/farm"}],
        "runtime_image": {"id": f"sha256:{authority_native_image_id}"},
    }
    images = {
        f"container:tt-quietbox3:{container_reference}": {
            "closure_sha256": container_closure,
            "id": f"sha256:{container_image_id}",
            "reference": container_reference,
        }
    }
    receipt = {
        "action": "scheduler-loss-active",
        "after": {"container_id": "a" * 64, "started_at": "new"},
        "before": {"container_id": "a" * 64, "started_at": "old"},
        "compiler": {
            "assignment": {
                "child": {
                    "generation": 1,
                    "kind": 0,
                    "owning_client_id": 7,
                    "pgid": 41,
                    "pid": 41,
                },
                "client": {
                    "client_id": 7,
                    "job_id": 2,
                    "scheduler_job_id": 2,
                },
                "listener": {
                    "binding_evidence": "container-env+netns-listener-uid+http-child",
                    "host": "127.0.0.1",
                    "port": 23004,
                    "socket_inode": "12345",
                    "socket_uid": 65534,
                },
                "schema": "icefarm-compiler-assignment-v1",
            },
            "container_id": "b" * 64,
            "daemon": {
                "argv": ["/opt/icecream/sbin/iceccd"],
                "comm": "iceccd",
                "exe": "/opt/icecream/sbin/iceccd",
                "exe_evidence": "proc-cmdline+comm",
                "pgid": 10,
                "pid": 10,
                "ppid": 1,
                "start_ticks": 3,
                "state": "S",
                "uids": [65534, 65534, 65534, 65534],
            },
            "group_gone": {
                "gone": True,
                "leader": None,
                "members": [],
                "schema": "icefarm-compiler-group-gone-v1",
            },
            "leader": {
                "argv": ["/opt/icecream/sbin/iceccd"],
                "comm": "iceccd",
                "exe": "/opt/icecream/sbin/iceccd",
                "exe_evidence": "proc-cmdline+comm",
                "pgid": 41,
                "pid": 41,
                "ppid": 10,
                "start_ticks": 9,
                "state": "R",
                "uids": [65534, 65534, 65534, 65534],
            },
            "listener": {
                "binding_evidence": "container-env+netns-listener-uid+http-child",
                "daemon_pid": 10,
                "daemon_start_ticks": 3,
                "host": "127.0.0.1",
                "port": 23004,
                "socket_inode": "12345",
                "socket_uid": 65534,
            },
            "stopped": {
                "argv": ["/opt/icecream/sbin/iceccd"],
                "comm": "iceccd",
                "exe": "/opt/icecream/sbin/iceccd",
                "exe_evidence": "proc-cmdline+comm",
                "pgid": 41,
                "pid": 41,
                "ppid": 10,
                "start_ticks": 9,
                "state": "T",
                "uids": [65534, 65534, 65534, 65534],
            },
            "worker_after": worker_identity,
            "worker_before": worker_identity,
        },
        "event_epoch": 1,
        "instance": "S1",
        "lost_scheduler_generation": 1,
        "lost_scheduler_job": 2,
        "pre_fault": {"scheduler_log": {}, "worker_log": {}},
        "quiescence": {
            "client_readiness": {
                "C1": {"ready": True, "witness": readiness}
            },
            "client_routes": {"C1": route_pair},
            "scheduler_snapshot": "S1",
            "scheduler_startup": {"line": "ICECREAM scheduler starting"},
            "worker_snapshot": "F1",
        },
        "schema": "icefarm-scheduler-active-loss-v3",
        "turn": "A",
    }
    return receipt, event, scenario, plan, farm, images


def test_active_loss_v3_binds_listener_worker_authority_and_readiness() -> None:
    receipt, event, scenario, plan, farm, images = _active_loss_v3_fixture()
    kwargs = {
        "readiness_v2": True,
        "plan": plan,
        "farm": farm,
        "images": images,
    }
    assert not _scheduler_active_loss_receipt_errors(
        receipt, event, scenario, **kwargs
    )
    assert receipt["compiler"]["leader"]["state"] == "R"
    assert receipt["compiler"]["stopped"]["state"] == "T"
    legacy_ptrace_state = copy.deepcopy(receipt)
    legacy_ptrace_state["compiler"]["leader"]["state"] = "t"
    assert _scheduler_active_loss_receipt_errors(
        legacy_ptrace_state, event, scenario, **kwargs
    )
    identity_mutations = {
        "argv": ["/opt/icecream/sbin/iceccd", "--tampered"],
        "comm": "tampered",
        "exe": "/opt/icecream/sbin/tampered",
        "exe_evidence": "tampered",
        "pgid": 42,
        "pid": 42,
        "ppid": 11,
        "start_ticks": 10,
        "uids": [65534, 65534, 65534, 0],
    }
    for field, value in identity_mutations.items():
        tampered = copy.deepcopy(receipt)
        tampered["compiler"]["stopped"][field] = value
        assert _scheduler_active_loss_receipt_errors(
            tampered, event, scenario, **kwargs
        ), field
    paths = (
        ("compiler", "listener", "socket_inode"),
        ("compiler", "listener", "socket_uid"),
        ("compiler", "listener", "binding_evidence"),
        ("compiler", "listener", "daemon_start_ticks"),
        ("compiler", "assignment", "listener", "socket_inode"),
        ("compiler", "assignment", "listener", "socket_uid"),
        ("compiler", "assignment", "listener", "binding_evidence"),
        ("compiler", "daemon", "start_ticks"),
        ("compiler", "stopped", "exe"),
        ("compiler", "stopped", "comm"),
        ("compiler", "stopped", "exe_evidence"),
        ("compiler", "stopped", "uids"),
        ("compiler", "worker_before", "container_id"),
        ("compiler", "worker_before", "runtime_path"),
        ("compiler", "worker_before", "image_id"),
        ("compiler", "worker_before", "env", "ICECC_WEB_HOSTPORT"),
        ("quiescence", "client_readiness", "C1", "ready"),
    )
    for path in paths:
        tampered = copy.deepcopy(receipt)
        cursor = tampered
        for key in path[:-1]:
            cursor = cursor[key]
        cursor[path[-1]] = False if path[-1] == "ready" else "tampered"
        assert _scheduler_active_loss_receipt_errors(
            tampered, event, scenario, **kwargs
        ), path

    tampered = copy.deepcopy(receipt)
    tampered["compiler"]["group_gone"]["members"] = [41]
    assert _scheduler_active_loss_receipt_errors(
        tampered, event, scenario, **kwargs
    )

    # Native Docker IDs can differ by host even when their portable closure is
    # identical.  The event must use this run's remote preflight ID, not the
    # farm authority's capture-host ID.
    tampered = copy.deepcopy(receipt)
    tampered["compiler"]["worker_before"]["image_id"] = farm[
        "runtime_image"
    ]["id"].removeprefix("sha256:")
    tampered["compiler"]["worker_after"]["image_id"] = tampered["compiler"][
        "worker_before"
    ]["image_id"]
    assert _scheduler_active_loss_receipt_errors(
        tampered, event, scenario, **kwargs
    )

    tampered_images = copy.deepcopy(images)
    only_receipt = next(iter(tampered_images.values()))
    only_receipt["closure_sha256"] = "0" * 64
    assert _scheduler_active_loss_receipt_errors(
        receipt,
        event,
        scenario,
        readiness_v2=True,
        plan=plan,
        farm=farm,
        images=tampered_images,
    )

    replay_v2 = copy.deepcopy(receipt)
    replay_v2["schema"] = "icefarm-scheduler-active-loss-v2"
    replay_v2["compiler"]["assignment"]["listener"] = {
        "host": "127.0.0.1",
        "port": 23004,
    }
    replay_v2["compiler"]["listener"].pop("binding_evidence")
    replay_v2["compiler"]["listener"].pop("socket_uid")
    assert not _scheduler_active_loss_receipt_errors(
        replay_v2, event, scenario, **kwargs
    )

    tampered_plan = copy.deepcopy(plan)
    tampered_plan["ports"]["web"]["F1"] += 1
    assert _scheduler_active_loss_receipt_errors(
        receipt,
        event,
        scenario,
        readiness_v2=True,
        plan=tampered_plan,
        farm=farm,
    )


def test_active_loss_v4_binds_active_job_below_dispatch_ceiling() -> None:
    receipt, event, scenario, plan, farm, images = _active_loss_v3_fixture()
    receipt["schema"] = "icefarm-scheduler-active-loss-v4"
    receipt["selection_last_dispatched_job"] = 5
    receipt["compiler"]["assignment"]["schema"] = (
        "icefarm-compiler-assignment-v2"
    )
    event["last_dispatched_job"] = 5
    kwargs = {
        "readiness_v2": True,
        "plan": plan,
        "farm": farm,
        "images": images,
    }
    assert not _scheduler_active_loss_receipt_errors(
        receipt, event, scenario, **kwargs
    )

    for path, value in (
        (("lost_scheduler_job",), 6),
        (("selection_last_dispatched_job",), 4),
        (("compiler", "assignment", "client", "scheduler_job_id"), 3),
        (("compiler", "assignment", "child", "generation"), 2),
    ):
        tampered = copy.deepcopy(receipt)
        cursor = tampered
        for key in path[:-1]:
            cursor = cursor[key]
        cursor[path[-1]] = value
        assert _scheduler_active_loss_receipt_errors(
            tampered, event, scenario, **kwargs
        ), path

    mismatched_event = copy.deepcopy(event)
    mismatched_event["last_dispatched_job"] = 4
    assert _scheduler_active_loss_receipt_errors(
        receipt, mismatched_event, scenario, **kwargs
    )


def test_active_loss_v5_binds_serial_admission_release() -> None:
    receipt, event, scenario, plan, farm, images = _active_loss_v3_fixture()
    receipt["schema"] = "icefarm-scheduler-active-loss-v5"
    receipt["selection_last_dispatched_job"] = 5
    receipt["compiler"]["assignment"]["schema"] = (
        "icefarm-compiler-assignment-v2"
    )
    receipt["admission_release"] = {
        "clients": {
            "C1": {
                "action": "resume",
                "active_after": 1,
                "active_before": 1,
                "client": "C1",
                "epoch": 1,
                "finished_ms": 101,
                "schema": "icefarm-event-gate-v1",
                "started_ms": 100,
                "status": "OPEN",
                "turn": "A",
            }
        },
        "schema": "icefarm-active-loss-admission-v1",
        "serial_through": 2,
    }
    event["last_dispatched_job"] = 5
    event["trigger"] = "job 2"
    kwargs = {
        "readiness_v2": True,
        "plan": plan,
        "farm": farm,
        "images": images,
    }
    assert not _scheduler_active_loss_receipt_errors(
        receipt, event, scenario, **kwargs
    )

    mutations = (
        (("admission_release", "serial_through"), 3),
        (("admission_release", "schema"), "wrong"),
        (("admission_release", "clients", "C1", "active_before"), 0),
        (("admission_release", "clients", "C1", "active_after"), 0),
        (("admission_release", "clients", "C1", "epoch"), 2),
        (("admission_release", "clients", "C1", "status"), "ABORT"),
    )
    for path, value in mutations:
        tampered = copy.deepcopy(receipt)
        cursor = tampered
        for key in path[:-1]:
            cursor = cursor[key]
        cursor[path[-1]] = value
        assert _scheduler_active_loss_receipt_errors(
            tampered, event, scenario, **kwargs
        ), path

    missing = copy.deepcopy(receipt)
    del missing["admission_release"]
    assert _scheduler_active_loss_receipt_errors(
        missing, event, scenario, **kwargs
    )


def test_active_loss_v6_binds_zero_skip_capture_to_held_trigger_job() -> None:
    receipt, event, scenario, plan, farm, images = _active_loss_v3_fixture()
    plan["run_id"] = "active-loss-v6"
    event.update(
        {
            "event_index": 0,
            "last_dispatched_job": 2,
            "trigger": "job 2",
            "workload_dispatch_count": 2,
        }
    )
    receipt.update(
        {
            "schema": "icefarm-scheduler-active-loss-v6",
            "selection_last_dispatched_job": 2,
            "admission_release": {
                "clients": {
                    "C1": {
                        "action": "resume",
                        "active_after": 1,
                        "active_before": 1,
                        "client": "C1",
                        "epoch": 1,
                        "finished_ms": 201,
                        "schema": "icefarm-event-gate-v1",
                        "started_ms": 200,
                        "status": "OPEN",
                        "turn": "A",
                    }
                },
                "schema": "icefarm-active-loss-admission-v1",
                "serial_through": 2,
            },
            "capture_boundary": {
                "arm": {
                    "capture_mode": "ptrace-fork-v1",
                    "daemon_pid": receipt["compiler"]["daemon"]["pid"],
                    "daemon_start_ticks": receipt["compiler"]["daemon"][
                        "start_ticks"
                    ],
                    "pid": 99,
                    "schema": "icefarm-compiler-capture-arm-v2",
                    "skip": 0,
                },
                "client": "C1",
                "event_epoch": 1,
                "event_index": 0,
                "release": {
                    "action": "release",
                    "index": 2,
                    "pid": 77,
                    "ready_ms": 100,
                    "released_ms": 101,
                    "schema": "icefarm-active-compiler-boundary-control-v1",
                },
                "run_id": plan["run_id"],
                "schema": "icefarm-active-compiler-boundary-v1",
                "serial_through": 2,
                "turn": "A",
                "wait": {
                    "action": "wait",
                    "index": 2,
                    "pid": 77,
                    "ready_ms": 100,
                    "schema": "icefarm-active-compiler-boundary-control-v1",
                },
            },
        }
    )
    receipt["compiler"]["assignment"]["schema"] = (
        "icefarm-compiler-assignment-v2"
    )
    receipt["compiler"]["leader"]["state"] = "t"
    kwargs = {
        "readiness_v2": True,
        "plan": plan,
        "farm": farm,
        "images": images,
    }
    assert not _scheduler_active_loss_receipt_errors(
        receipt, event, scenario, **kwargs
    )
    assert receipt["compiler"]["leader"]["state"] == "t"
    assert receipt["compiler"]["stopped"]["state"] == "T"
    for side, state in (
        ("leader", "R"),
        ("leader", "S"),
        ("leader", "Z"),
        ("leader", "X"),
        ("stopped", "R"),
        ("stopped", "S"),
        ("stopped", "Z"),
        ("stopped", "X"),
    ):
        tampered = copy.deepcopy(receipt)
        tampered["compiler"][side]["state"] = state
        assert _scheduler_active_loss_receipt_errors(
            tampered, event, scenario, **kwargs
        ), (side, state)

    for path, value in (
        (("capture_boundary", "arm", "skip"), 1),
        (("capture_boundary", "release", "pid"), 78),
        (("capture_boundary", "serial_through"), 3),
        (("selection_last_dispatched_job",), 3),
    ):
        tampered = copy.deepcopy(receipt)
        cursor = tampered
        for key in path[:-1]:
            cursor = cursor[key]
        cursor[path[-1]] = value
        assert _scheduler_active_loss_receipt_errors(
            tampered, event, scenario, **kwargs
        ), path


def _client_transition_event() -> tuple[dict[str, object], dict[str, object]]:
    scenario = _scenario("S'CF", client_versions=(50,), worker_versions=(43,))
    scenario["timeline"] = [
        {"trigger": "job 1", "action": "restart", "instance": "C1"}
    ]
    gate = {
        "action": "quiesce",
        "active_after": 0,
        "active_before": 1,
        "client": "C1",
        "epoch": 1,
        "finished_ms": 100,
        "schema": "icefarm-event-gate-v1",
        "started_ms": 99,
        "status": "QUIESCED",
        "turn": "A",
    }
    resume = dict(gate, action="resume", status="OPEN", started_ms=1001, finished_ms=1002)
    rows = [{"index": 1, "path": "jobs/000001/result.tsv", "sha256": SHA_A}]
    checkpoint = {
        "checkpoint_sha256": "",
        "client": "C1",
        "completed_rows": rows,
        "completed_rows_sha256": hashlib.sha256(
            json.dumps(rows, sort_keys=True, separators=(",", ":")).encode()
        ).hexdigest(),
        "expected_jobs": 1,
        "schema": "icefarm-workload-checkpoint-v1",
        "status": "QUIESCED",
        "turn": "A",
        "worklist_sha256": SHA_B,
    }
    body = dict(checkpoint)
    body.pop("checkpoint_sha256")
    checkpoint["checkpoint_sha256"] = hashlib.sha256(
        json.dumps(body, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    readiness = {
        "host": "h1",
        "line": "ICECREAM daemon 1.5.90 starting up",
        "log_path": "/run/C1/log/client-daemon.log",
        "offset": 10,
        "role": "C",
    }
    client_readiness = {
        "cache_line": "cache sidecar adapter state=2 lifecycle=3",
        "cache_required": True,
        "connected_line": "Connected to scheduler (I am known as 10.0.0.1)",
        "host": "h1",
        "log_path": readiness["log_path"],
        "offset": 10,
    }
    receipt = {
        "action": "restart",
        "after": {
            "container_id": SHA_B,
            "closure_sha256": SHA_B,
            "env": {"ICECC_P50_MODE": "on"},
            "image": "p50s4-fixture",
            "role_sha256": SHA_B,
        },
        "before": {
            "container_id": SHA_A,
            "closure_sha256": SHA_B,
            "env": {"ICECC_P50_MODE": "on"},
            "image": "p50s4-fixture",
            "role_sha256": SHA_B,
        },
        "checkpoints": {"C1": checkpoint},
        "client_readiness": client_readiness,
        "coordination": {
            "clients": {"C1": gate},
            "ready_ms": 1000,
            "relaunch": {
                "C1": {
                    "client": "C1",
                    "expected_jobs": 1,
                    "failures": 0,
                    "jobs": 1,
                    "status": "COMPLETE",
                }
            },
            "resume": {"C1": resume},
        },
        "event_epoch": 1,
        "instance": "C1",
        "preflight": {
            "image_closure_sha256": SHA_B,
            "role_sha256": SHA_B,
            "runtime_path": "/runtimes/" + SHA_B + "/root",
        },
        "readiness": readiness,
        "schema": "icefarm-client-transition-v1",
        "turn": "A",
    }
    observed = {
        "action": "restart",
        "event_epoch": 1,
        "event_index": 0,
        "fired_ms": 1000,
        "instance": "C1",
        "receipt": receipt,
    }
    return scenario, observed


@pytest.mark.parametrize("mutation", ("row_path", "checkpoint_hash", "host", "log_path", "cache"))
def test_client_transition_checkpoint_and_readiness_tampering_fails_closed(
    mutation: str,
) -> None:
    scenario, observed = _client_transition_event()
    assert not _transition_receipt_errors(observed, scenario["timeline"][0], scenario)
    tampered = copy.deepcopy(observed)
    receipt = tampered["receipt"]
    if mutation == "row_path":
        receipt["checkpoints"]["C1"]["completed_rows"][0]["path"] = "jobs/000002/result.tsv"
    elif mutation == "checkpoint_hash":
        receipt["checkpoints"]["C1"]["completed_rows_sha256"] = "f" * 64
    elif mutation == "host":
        receipt["client_readiness"]["host"] = "other-host"
    elif mutation == "log_path":
        receipt["client_readiness"]["log_path"] = "/other/C1/log/client-daemon.log"
    else:
        receipt["client_readiness"]["cache_line"] = "cache sidecar adapter state=1 lifecycle=2"
    assert _transition_receipt_errors(
        tampered, scenario["timeline"][0], scenario
    )


def _expect() -> dict[str, object]:
    return {
        "engagement": "expected(c,f)",
        "error106_max": 0,
        "exact": "all",
        "reuse": "all-true-when-p29v1",
        "tail_to_incapable": 0,
        "wall_s_max": None,
        "wedges": 0,
    }


def _instance(name: str, role: str, version: int) -> dict[str, object]:
    value: dict[str, object] = {
        "env": {},
        "image": "new" if version == 50 else "old",
        "name": name,
        "role": role,
        "host": "h1",
    }
    if role == "S":
        value["env"] = {"ICECC_P50_PROFILE": "P29V1"}
    elif role == "C" and version == 50:
        value["env"] = {"ICECC_P50_MODE": "on"}
    elif role == "F":
        value["slots"] = 1
    return value


def _scenario(
    shape: str,
    *,
    client_versions: tuple[int, ...] = (50,),
    worker_versions: tuple[int, ...] = (50,),
) -> dict[str, object]:
    return {
        "expect": _expect(),
        "id": "fixture-" + str(abs(hash((shape, client_versions, worker_versions)))),
        "instances": [
            _instance("S1", "S", 43 if shape == "SCF" else 50),
            *(
                _instance(f"F{index + 1}", "F", version)
                for index, version in enumerate(worker_versions)
            ),
            *(
                _instance(f"C{index + 1}", "C", version)
                for index, version in enumerate(client_versions)
            ),
        ],
        "images": {"new": "p50s4-fixture", "old": "p43-fixture"},
        "shape": shape,
        "timeline": [],
        "workload": {
            "clients": [f"C{index + 1}" for index in range(len(client_versions))],
            "turns": ["A"],
        },
    }


def _row(
    job: int,
    *,
    client: str = "C1",
    client_version: int = 50,
    worker: str = "F1",
    worker_version: int = 50,
    tail: bool | None = None,
    profile: str | None = None,
    outcome: str | None = None,
) -> dict[str, object]:
    if tail is None:
        tail = client_version == worker_version == 50
    if profile is None and tail:
        profile = "P29V1"
    if outcome is None:
        outcome = "committed" if tail else "none"
    return {
        "c_to_f_bytes": 1024,
        "client_instance": client,
        "client_version": client_version,
        "cs": worker,
        "cs_version": worker_version,
        "event_epoch": 0,
        "exact": True,
        "f_to_c_bytes": 512,
        "job_id": str(job),
        "object_sha_local": SHA_A,
        "object_sha_remote": SHA_A,
        "retries": 0,
        "reuse": True if profile == "P29V1" else None,
        "schema": ROW_SCHEMA,
        "session_outcome": outcome,
        "tail_present": tail,
        "tail_profile": profile,
        "tu": f"files/tu-{job}.ii",
        "wall_ms": 25,
    }


def _observations(
    rows: list[dict[str, object]],
    *,
    old_workers: set[str] = frozenset(),
    revisions: dict[str, int] | None = None,
) -> dict[str, object]:
    workers = sorted({str(row["cs"]) for row in rows})
    clients = sorted({str(row["client_instance"]) for row in rows})
    return {
        "cell_wall_ms": 100,
        "compile_failure_job_ids": [],
        "error106_job_ids": [],
        "failed_p50_result_identities": {"record_count": 0, "records": []},
        "failed_p50_source_transfers": {"record_count": 0, "records": []},
        "failed_p50_uncommitted_transports": {"record_count": 0, "records": []},
        "incomplete_turns": [],
        "job_lifecycle": [
            {
                "deadline_ms": 10_000,
                "dispatch_ms": index * 100,
                "job_id": row["job_id"],
                "terminal": "completion",
                "terminal_ms": index * 100 + 25,
                "turn": "A",
            }
            for index, row in enumerate(rows)
        ],
        "assignment_lifecycle": [
            {
                "job_id": row["job_id"],
                "attempts": [
                    {
                        "generation": 1,
                        "scheduler_job": int(row["job_id"]) * 10 + attempt,
                        "terminal": "completion",
                        "worker": row["cs"],
                    }
                    for attempt in range(2 if row["retries"] == 1 else 1)
                ],
            }
            for row in rows
        ],
        "logins": [
            {
                "cache_profiles": []
                if worker in old_workers
                else ["P29V1", "ZSTD_TU", "ZSTD_ROUTE"],
                "instance": worker,
                "protocol": 43 if worker in old_workers else 50,
            }
            for worker in workers
        ],
        "local_fallback_job_ids": [],
        "oracle": {"sample_mismatch_job_ids": [], "sample_total": 1},
        "sidecars": {
            worker: {
                "cache_ports": [] if worker in old_workers else [24000],
                "process_count": 0 if worker in old_workers else 1,
                "sessions": 0
                if worker in old_workers
                else sum(
                    row["cs"] == worker and row["tail_present"] is True for row in rows
                ),
            }
            for worker in workers
        }
        | {client: {"sessions": 0} for client in clients},
        "successful_strict_p50_retry_bindings": [],
        "successful_strict_p50_late_result_bindings": [],
        "wire_revisions": revisions
        if revisions is not None
        else {
            name: 1
            for name in workers + clients
            if name not in old_workers
            and any(
                row["tail_present"] is True
                and (
                    (row["cs"] == name and row["cs_version"] == 50)
                    or (row["client_instance"] == name and row["client_version"] == 50)
                )
                for row in rows
            )
        },
    }


def _bundle(
    scenario: dict[str, object],
    rows: list[dict[str, object]],
    observations: dict[str, object],
) -> dict[str, object]:
    return {
        "observations": observations,
        "rows": rows,
        "scenario": scenario,
        "schema": BUNDLE_SCHEMA,
    }


def test_verdict_requires_authenticated_f_init_witness() -> None:
    rows = [_row(1)]
    observations = _observations(rows)
    bundle = _bundle(_scenario("base"), rows, observations)
    bundle["topology"] = {
        "instances": [{"host": "h1", "name": "F1", "role": "F"}]
    }
    bundle["plan"] = {
        "commands": [
            {"argv": ["docker", "run", "--init"], "phase": "up.start-f"}
        ],
        "launch_contract": F_INIT_LAUNCH_CONTRACT,
    }
    bundle["launch_contract"] = F_INIT_LAUNCH_CONTRACT
    observations["f_init"] = {
        "instances": [
            {
                "host": "h1",
                "init": True,
                "inspect_sha256": "a" * 64,
                "instance": "F1",
            }
        ],
        "schema": "icefarm-f-init-v1",
    }
    launch = next(
        clause
        for clause in evaluate_bundle(bundle)["clauses"]
        if clause["id"] == "launch.f-init"
    )
    assert launch["status"] == "PASS"
    launch_ids = {
        clause["id"]
        for clause in evaluate_bundle(bundle)["clauses"]
        if clause["id"].startswith("launch.")
    }
    assert launch_ids == {"launch.contract", "launch.f-init"}
    transition = copy.deepcopy(bundle)
    del transition["plan"]["launch_contract"]
    del transition["launch_contract"]
    transition_ids = {
        clause["id"]
        for clause in evaluate_bundle(transition)["clauses"]
        if clause["id"].startswith("launch.")
    }
    assert transition_ids == {"launch.f-init"}
    historical = copy.deepcopy(transition)
    historical["plan"]["commands"][0]["argv"].remove("--init")
    historical_ids = {
        clause["id"]
        for clause in evaluate_bundle(historical)["clauses"]
        if clause["id"].startswith("launch.")
    }
    assert historical_ids == set()
    mixed = copy.deepcopy(transition)
    mixed["plan"]["commands"].append(
        {"argv": ["docker", "run"], "phase": "up.start-f"}
    )
    mixed_contract = next(
        clause
        for clause in evaluate_bundle(mixed)["clauses"]
        if clause["id"] == "launch.contract"
    )
    assert mixed_contract["status"] == "FAIL"
    no_f = copy.deepcopy(bundle)
    no_f["topology"]["instances"] = []
    no_f_ids = {
        clause["id"]
        for clause in evaluate_bundle(no_f)["clauses"]
        if clause["id"].startswith("launch.")
    }
    assert no_f_ids == {"launch.contract"}
    missing = copy.deepcopy(bundle)
    del missing["observations"]["f_init"]
    for bad in (
        None,
        {**observations["f_init"], "instances": []},
        {
            **observations["f_init"],
            "instances": [{**observations["f_init"]["instances"][0], "init": False}],
        },
    ):
        tampered = missing if bad is None else copy.deepcopy(bundle)
        if bad is not None:
            tampered["observations"]["f_init"] = bad
        launch = next(
            clause
            for clause in evaluate_bundle(tampered)["clauses"]
            if clause["id"] == "launch.f-init"
        )
        assert launch["status"] == "FAIL"


@pytest.mark.parametrize(
    ("initial_version", "action", "expected_profiles"),
    (
        (50, "downgrade", ("P29V1", None)),
        (43, "upgrade", (None, "P29V1")),
    ),
)
def test_scheduler_transition_profile_is_resolved_per_event_epoch(
    initial_version: int,
    action: str,
    expected_profiles: tuple[str | None, str | None],
) -> None:
    scenario = _scenario("mixed")
    scenario["id"] = (
        "S60-13-warm-s-down" if action == "downgrade" else "S60-14-warm-s-up"
    )
    scheduler = next(
        item for item in scenario["instances"] if item["role"] == "S"
    )
    scheduler["image"] = "new" if initial_version == 50 else "old"
    scheduler["env"] = (
        {"ICECC_P50_PROFILE": "P29V1"} if initial_version == 50 else {}
    )
    scenario["timeline"] = [
        {
            "trigger": "job 24",
            "action": action,
            "instance": "S1",
            "image": "old" if action == "downgrade" else "new",
        }
    ]

    assert _scenario_profile_at_epoch(scenario, 0) == (expected_profiles[0], None)
    assert _scenario_profile_at_epoch(scenario, 1) == (expected_profiles[1], None)

    rows = [
        _row(
            1,
            tail=expected_profiles[0] is not None,
            profile=expected_profiles[0],
            outcome="committed" if expected_profiles[0] else "none",
        ),
        _row(
            2,
            tail=expected_profiles[1] is not None,
            profile=expected_profiles[1],
            outcome="committed" if expected_profiles[1] else "none",
        ),
    ]
    rows[1]["event_epoch"] = 1
    observations = _observations(rows)
    observations["job_lifecycle"][0]["final_dispatch_ms"] = 900
    observations["job_lifecycle"][0]["scheduler_generation"] = 1
    observations["job_lifecycle"][1]["final_dispatch_ms"] = 1100
    observations["job_lifecycle"][1]["scheduler_generation"] = 2
    event_log = [
        {
            "action": action,
            "event_epoch": 1,
            "fired_ms": 1000,
            "instance": "S1",
            "trigger": "job 24",
        }
    ]
    fixture = _bundle(scenario, rows, observations)
    fixture["event_log"] = event_log
    assert not _s60_transition_epoch_errors(
        scenario, rows, observations, event_log
    )
    engagement = next(
        item
        for item in evaluate_bundle(fixture)["clauses"]
        if item["id"] == "engagement.expected"
    )
    assert engagement["status"] == "PASS", engagement

    wrong = copy.deepcopy(fixture)
    post = wrong["rows"][1]
    post["tail_present"] = expected_profiles[0] is not None
    post["tail_profile"] = expected_profiles[0]
    post["session_outcome"] = "committed" if expected_profiles[0] else "none"
    post["reuse"] = True if expected_profiles[0] == "P29V1" else None
    engagement = next(
        item
        for item in evaluate_bundle(wrong)["clauses"]
        if item["id"] == "engagement.expected"
    )
    assert engagement["status"] == "FAIL"

    wrong_epoch = copy.deepcopy(rows)
    wrong_epoch[0]["event_epoch"] = 1
    assert _s60_transition_epoch_errors(
        scenario, wrong_epoch, observations, event_log
    )


def test_scheduler_transition_epoch_uses_exact_generation_within_same_second() -> None:
    scenario = _scenario("mixed")
    scenario["id"] = "S60-14-warm-s-up"
    scheduler = next(
        item for item in scenario["instances"] if item["role"] == "S"
    )
    scheduler["image"] = "old"
    scheduler["env"] = {}
    scenario["timeline"] = [
        {
            "trigger": "job 24",
            "action": "upgrade",
            "instance": "S1",
            "image": "new",
        }
    ]
    rows = [
        _row(1, tail=False, profile=None, outcome="none"),
        _row(2, tail=True, profile="P29V1", outcome="committed"),
    ]
    rows[1]["event_epoch"] = 1
    observations = _observations(rows)
    observations["job_lifecycle"][0].update(
        final_dispatch_ms=900,
        scheduler_generation=1,
    )
    observations["job_lifecycle"][1].update(
        final_dispatch_ms=1000,
        scheduler_generation=2,
    )
    event_log = [
        {
            "action": "upgrade",
            "event_epoch": 1,
            "fired_ms": 1260,
            "instance": "S1",
            "trigger": "job 24",
        }
    ]
    fixture = _bundle(scenario, rows, observations)
    fixture["event_log"] = event_log
    fixture["plan"] = {
        "scheduler_dispatch_epoch_contract": SCHEDULER_DISPATCH_EPOCH_CONTRACT
    }

    assert not _s60_transition_epoch_errors(
        scenario, rows, observations, event_log, generation_epoch=True
    )
    engagement = next(
        item
        for item in evaluate_bundle(fixture)["clauses"]
        if item["id"] == "engagement.expected"
    )
    assert engagement["status"] == "PASS", engagement

    stale_generation = copy.deepcopy(observations)
    stale_generation["job_lifecycle"][1]["scheduler_generation"] = 1
    assert _s60_transition_epoch_errors(
        scenario, rows, stale_generation, event_log, generation_epoch=True
    )
    far_timestamp = copy.deepcopy(observations)
    far_timestamp["job_lifecycle"][1]["final_dispatch_ms"] = 0
    assert _s60_transition_epoch_errors(
        scenario, rows, far_timestamp, event_log, generation_epoch=True
    )


@pytest.mark.parametrize(
    "contract",
    ("unreviewed-dispatch-law", {"schema": "unreviewed"}),
)
def test_unknown_scheduler_dispatch_epoch_contract_fails_closed(
    contract: object,
) -> None:
    fixture = copy.deepcopy(SHAPE_FIXTURES["S'C'F'"])
    fixture["plan"] = {"scheduler_dispatch_epoch_contract": contract}

    verdict = evaluate_bundle(fixture)
    clause = next(
        item
        for item in verdict["clauses"]
        if item["id"] == "dispatch-epoch.contract"
    )
    assert verdict["status"] == "FAIL"
    assert clause["status"] == "FAIL"
    assert clause["offending_job_ids"] == ["@plan:scheduler-dispatch-epoch"]


def test_c_upgrade_missing_tail_cannot_erase_its_capability_expectation() -> None:
    scenario = _scenario("mixed", client_versions=(43,), worker_versions=(50,))
    scenario["id"] = "S60-04-c1-up"
    scenario["timeline"] = [
        {
            "trigger": "job 24",
            "action": "upgrade",
            "instance": "C1",
            "image": "new",
        }
    ]
    row = _row(1)
    row["event_epoch"] = 1
    observations = _observations([row])
    bundle = _bundle(scenario, [row], observations)
    bundle["farm"] = {
        "authority": {
            "images": {
                "p43-fixture": {
                    "archive_sha256": SHA_A,
                    "closure_sha256": SHA_A,
                    "commit": "a" * 40,
                },
                "p50s4-fixture": {
                    "archive_sha256": SHA_B,
                    "cache_wire_revision": 1,
                    "closure_sha256": SHA_B,
                    "commit": "b" * 40,
                },
            },
            "role_stores": {
                "43": {"client": {"sha256": SHA_A}, "daemon": {"sha256": SHA_A}},
                "50": {"client": {"sha256": SHA_C}, "daemon": {"sha256": SHA_B}},
            },
        }
    }
    bundle["topology"] = {
        "instances": [
            {
                **copy.deepcopy(instance),
                "cache_wire_revision": 1 if instance["image"] == "new" else None,
                "image": {
                    "closure_sha256": SHA_B if instance["image"] == "new" else SHA_A,
                    "label": "p50s4-fixture" if instance["image"] == "new" else "p43-fixture",
                },
                "sha256": (
                    SHA_C
                    if instance["role"] == "C" and instance["image"] == "new"
                    else SHA_B
                    if instance["role"] == "F" and instance["image"] == "new"
                    else SHA_A
                ),
                "version": 50 if instance["image"] == "new" else 43,
            }
            for instance in scenario["instances"]
        ]
    }
    bundle["event_log"] = [
        {
            "action": "upgrade",
            "event_epoch": 1,
            "event_index": 0,
            "fired_ms": 1000,
            "instance": "C1",
            "receipt": {
                "after": {
                    "closure_sha256": SHA_B,
                    "env": {"ICECC_P50_MODE": "on"},
                    "image": "p50s4-fixture",
                    "role_sha256": SHA_C,
                }
            },
            "trigger": "job 24",
        }
    ]

    positive = evaluate_bundle(bundle)
    assert next(
        clause
        for clause in positive["clauses"]
        if clause["id"] == "engagement.capability-authority"
    )["status"] == "PASS"
    assert next(
        clause
        for clause in positive["clauses"]
        if clause["id"] == "engagement.expected"
    )["status"] == "PASS"

    for mutation in ("closure", "role", "authority"):
        tampered = copy.deepcopy(bundle)
        if mutation == "closure":
            tampered["event_log"][0]["receipt"]["after"]["closure_sha256"] = SHA_C
        elif mutation == "role":
            tampered["event_log"][0]["receipt"]["after"]["role_sha256"] = SHA_A
        else:
            tampered["farm"]["authority"]["images"]["p50s4-fixture"].pop(
                "archive_sha256"
            )
        capability = next(
            clause
            for clause in evaluate_bundle(tampered)["clauses"]
            if clause["id"] == "engagement.capability-authority"
        )
        assert capability["status"] == "FAIL"

    missing = copy.deepcopy(bundle)
    missing["rows"][0].update(
        reuse=None,
        session_outcome="none",
        tail_present=False,
        tail_profile=None,
    )
    missing["observations"]["wire_revisions"].pop("C1")
    missing["observations"]["sidecars"]["F1"]["sessions"] = 0
    verdict = evaluate_bundle(missing)
    assert next(
        clause
        for clause in verdict["clauses"]
        if clause["id"] == "engagement.capability-authority"
    )["status"] == "PASS"
    engagement = next(
        clause
        for clause in verdict["clauses"]
        if clause["id"] == "engagement.expected"
    )
    assert engagement["status"] == "FAIL"
    assert engagement["offending_job_ids"] == ["1"]


def _s70_b5_bundle() -> dict[str, object]:
    scenario, observed = _client_transition_event()
    scenario["shape"] = "S'C'F'"
    scenario["expect"]["engagement"] = "s70-b5-interner-downgrade"
    scenario["expect"]["error106_max"] = 1
    scenario["workload"]["jobs"] = 1
    scenario["timeline"] = [
        {
            "trigger": "job 1",
            "action": "env_set",
            "instance": "C1",
            "env": {
                "ICECC_P50_FAULT_INJECTION": "P29_INTERNER_FAIL_ONCE"
            },
        }
    ]
    scheduler = next(
        item for item in scenario["instances"] if item["role"] == "S"
    )
    scheduler["env"].pop("ICECC_P50_PROFILE")
    worker = next(item for item in scenario["instances"] if item["role"] == "F")
    worker["image"] = "new"
    scenario["instances"].insert(2, _instance("F2", "F", 50))

    observed["action"] = "env_set"
    observed["trigger"] = "job 1"
    observed["last_dispatched_job"] = 1
    observed["workload_dispatch_count"] = 1
    receipt = observed["receipt"]
    receipt["action"] = "env_set"
    receipt["after"]["env"] = {
        "ICECC_P50_FAULT_INJECTION": "P29_INTERNER_FAIL_ONCE",
        "ICECC_P50_MODE": "on",
    }

    rows = [
        _row(1),
        _row(2, worker="F2", profile="ZSTD_TU"),
        _row(3, worker="F2", profile="ZSTD_TU"),
    ]
    rows[1]["event_epoch"] = 1
    rows[1]["retries"] = 1
    rows[2]["event_epoch"] = 1
    observations = _observations(rows)
    observations["error106_job_ids"] = []
    observations["assignment_lifecycle"][1]["attempts"] = [
        {
            "generation": 1,
            "scheduler_job": 20,
            "terminal": "cancellation",
            "worker": "F1",
        },
        {
            "generation": 1,
            "scheduler_job": 21,
            "terminal": "completion",
            "worker": "F2",
        },
    ]
    observations["job_lifecycle"][1].update(
        first_dispatch_ms=100,
        final_dispatch_ms=101,
    )
    observations["f_init"] = {
        "instances": [
            {
                "host": "h1",
                "init": True,
                "inspect_sha256": SHA_A,
                "instance": "F1",
            },
            {
                "host": "h2",
                "init": True,
                "inspect_sha256": SHA_A,
                "instance": "F2",
            },
        ],
        "schema": "icefarm-f-init-v1",
    }
    observations["failed_p50_source_transfers"] = {
        "record_count": 1,
        "records": [
            {
                "assignment_epoch": 11,
                "assignment_identity_line": 6,
                "assignment_line": 7,
                "assignment_nonce": 21,
                "attempt_index": 0,
                "c_guid": 31,
                "compile_identity_present": False,
                "error": 0x5001,
                "failed_endpoint": "10.0.27.101:23003",
                "failure_line": 11,
                "profile": "P29V1",
                "retry_assignment_epoch": 11,
                "retry_assignment_identity_line": 16,
                "retry_assignment_line": 17,
                "retry_assignment_nonce": 22,
                "retry_c_guid": 31,
                "retry_endpoint": "10.0.27.56:23004",
                "retry_line": 12,
                "retry_scheduler_job": 21,
                "retry_tu_seq": 42,
                "row_job_id": "2",
                "scheduler_job": 20,
                "source_result_present": False,
                "source_result_status": 3,
                "status": 2,
                "transfer_attempts": 0,
                "tu_seq": 41,
                "worker": "F1",
            }
        ],
    }
    observations["successful_strict_p50_retry_bindings"] = [
        {
            "failure_reason": "source-transfer-loss",
            "final_dispatch_ms": 101,
            "final_generation": 1,
            "final_scheduler_job": 21,
            "final_terminal_ms": 125,
            "final_worker": "F2",
            "first_dispatch_ms": 100,
            "first_generation": 1,
            "first_scheduler_job": 20,
            "first_terminal": "cancellation",
            "first_terminal_ms": 200,
            "first_worker": "F1",
            "job_id": "2",
        }
    ]
    observations["p29_interner_faults"] = [
        {
            "client_instance": "C1",
            "schema": "icecream-p50-fault-v1",
            "fault": "p29-interner-fail-once",
            "outcome": "fired",
        }
    ]
    bundle = _bundle(scenario, rows, observations)
    bundle["launch_contract"] = "icefarm-f-init-launch-v1"
    bundle["plan"] = {
        "launch_contract": "icefarm-f-init-launch-v1",
        "ports": {"instances": {"F1": 23003, "F2": 23004}},
        "topology": {
            "instances": [
                {
                    "address": "10.0.27.101",
                    "host": "h1",
                    "name": "F1",
                    "role": "F",
                },
                {
                    "address": "10.0.27.56",
                    "host": "h2",
                    "name": "F2",
                    "role": "F",
                },
            ]
        },
    }
    bundle["event_log"] = [observed]
    return bundle


def test_s70_b5_interner_fault_downgrades_later_rows_to_zstd_tu() -> None:
    fixture = _s70_b5_bundle()
    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "PASS", verdict


@pytest.mark.parametrize(
    "mutation",
    (
        "missing_fault",
        "duplicate_fault",
        "wrong_client",
        "p29_after",
        "controlled_no_tail",
        "missing_source_transfer",
        "duplicate_source_transfer",
        "wrong_source_error",
        "wrong_source_result_status",
        "missing_retry_binding",
        "extra_error106",
        "extra_retry",
        "explicit_profile",
        "pause_not_active",
    ),
)
def test_s70_b5_fault_evidence_and_row_law_fail_closed(mutation: str) -> None:
    fixture = _s70_b5_bundle()
    if mutation == "missing_fault":
        fixture["observations"]["p29_interner_faults"] = []
    elif mutation == "duplicate_fault":
        fixture["observations"]["p29_interner_faults"].append(
            dict(fixture["observations"]["p29_interner_faults"][0])
        )
    elif mutation == "wrong_client":
        fixture["observations"]["p29_interner_faults"][0]["client_instance"] = "C2"
    elif mutation == "p29_after":
        fixture["rows"][2]["tail_profile"] = "P29V1"
        fixture["rows"][2]["reuse"] = True
    elif mutation == "controlled_no_tail":
        fixture["rows"][1]["tail_present"] = False
        fixture["rows"][1]["tail_profile"] = None
        fixture["rows"][1]["session_outcome"] = "none"
    elif mutation == "missing_source_transfer":
        fixture["observations"]["failed_p50_source_transfers"] = {
            "record_count": 0,
            "records": [],
        }
    elif mutation == "duplicate_source_transfer":
        source = fixture["observations"]["failed_p50_source_transfers"]
        source["records"].append(copy.deepcopy(source["records"][0]))
        source["record_count"] = 2
    elif mutation == "wrong_source_error":
        fixture["observations"]["failed_p50_source_transfers"]["records"][0][
            "error"
        ] = 4
    elif mutation == "wrong_source_result_status":
        fixture["observations"]["failed_p50_source_transfers"]["records"][0][
            "source_result_status"
        ] = 4
    elif mutation == "missing_retry_binding":
        fixture["observations"]["successful_strict_p50_retry_bindings"] = []
    elif mutation == "extra_error106":
        fixture["observations"]["error106_job_ids"] = ["2"]
    elif mutation == "extra_retry":
        fixture["rows"][1]["retries"] = 2
    elif mutation == "pause_not_active":
        fixture["event_log"][0]["receipt"]["coordination"]["clients"]["C1"][
            "active_before"
        ] = 0
    else:
        scheduler = next(
            item
            for item in fixture["scenario"]["instances"]
            if item["role"] == "S"
        )
        scheduler["env"]["ICECC_P50_PROFILE"] = "P29V1"
    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "FAIL"
    failed = {item["id"] for item in verdict["clauses"] if item["status"] == "FAIL"}
    assert failed & {"engagement.expected", "s70.b5-interner-downgrade"}


def _scheduler_env_event(
    *,
    index: int,
    trigger_job: int,
    before_id: str,
    after_id: str,
    before_profile: str,
    after_profile: str,
    fired_ms: int,
) -> dict[str, object]:
    epoch = index + 1
    gate = {
        "action": "pause",
        "active_after": 0,
        "active_before": 1,
        "client": "C1",
        "epoch": epoch,
        "finished_ms": fired_ms - 10,
        "schema": "icefarm-event-gate-v1",
        "started_ms": fired_ms - 20,
        "status": "PAUSED",
        "turn": "A",
    }
    resume = dict(
        gate,
        action="resume",
        active_before=0,
        finished_ms=fired_ms + 10,
        started_ms=fired_ms,
        status="OPEN",
    )
    readiness = {
        "host": "h1",
        "line": "ICECREAM scheduler 1.5.90 starting up, port 8765",
        "log_path": "/run/S1/log/scheduler.log",
        "offset": epoch * 10,
        "role": "S",
    }
    client_readiness = {
        "cache_line": "cache sidecar adapter state=2 lifecycle=3",
        "cache_required": True,
        "connected_line": "Connected to scheduler (I am known as 10.0.0.1)",
        "host": "h1",
        "log_path": "/run/C1/log/client-daemon.log",
        "offset": epoch * 10,
    }
    receipt = {
        "action": "env_set",
        "after": {
            "container_id": after_id,
            "closure_sha256": SHA_A,
            "env": {"ICECC_P50_PROFILE": after_profile},
            "image": "p50s4-fixture",
            "role_sha256": SHA_B,
        },
        "before": {
            "container_id": before_id,
            "closure_sha256": SHA_A,
            "env": {"ICECC_P50_PROFILE": before_profile},
            "image": "p50s4-fixture",
            "role_sha256": SHA_B,
        },
        "coordination": {
            "client_readiness": {"C1": client_readiness},
            "clients": {"C1": gate},
            "ready_ms": fired_ms,
            "resume": {"C1": resume},
            "scheduler_snapshot": "scheduler ready\n",
            "scheduler_startup": readiness,
            "worker_snapshot": "F1\n",
            "workers": ["F1"],
        },
        "event_epoch": epoch,
        "instance": "S1",
        "preflight": {
            "image_closure_sha256": SHA_A,
            "role_sha256": SHA_B,
            "runtime_path": "/runtimes/" + SHA_A + "/root",
        },
        "readiness": readiness,
        "schema": "icefarm-transition-v2",
        "turn": "A",
    }
    return {
        "action": "env_set",
        "event_epoch": epoch,
        "event_index": index,
        "fired_ms": fired_ms,
        "instance": "S1",
        "last_dispatched_job": trigger_job,
        "receipt": receipt,
        "trigger": f"job {trigger_job}",
        "workload_dispatch_count": trigger_job,
    }


def _s70_b4_scheduler_bundle() -> dict[str, object]:
    scenario = _scenario("S'C'F'")
    scenario["id"] = "S70-b4-scheduler-restart"
    scenario["expect"]["engagement"] = "s70-b4-scheduler-restart"
    scenario["expect"]["reuse"] = "none-when-legacy"
    scenario["workload"]["corpus"] = "fmt-100"
    scenario["workload"]["jobs"] = 24
    scenario["timeline"] = [
        {"trigger": "job 50", "action": "restart", "instance": "S1"}
    ]
    event = _scheduler_env_event(
        index=0,
        trigger_job=50,
        before_id=SHA_A,
        after_id=SHA_A,
        before_profile="P29V1",
        after_profile="P29V1",
        fired_ms=250,
    )
    coordination = event["receipt"]["coordination"]
    event["action"] = "restart"
    event["receipt"] = {
        "action": "restart",
        "after": {
            "container_id": SHA_A,
            "started_at": "2026-09-06T00:01:00Z",
        },
        "before": {
            "container_id": SHA_A,
            "started_at": "2026-09-06T00:00:00Z",
        },
        "coordination": coordination,
        "event_epoch": 1,
        "instance": "S1",
        "schema": "icefarm-scheduler-restart-v1",
        "turn": "A",
    }
    rows = [_row(1), _row(2), _row(3), _row(4)]
    rows[2]["event_epoch"] = 1
    rows[3]["event_epoch"] = 1
    observations = _observations(rows)
    for dispatch_line, (lifecycle, dispatch_ms, generation) in enumerate(
        zip(
            observations["job_lifecycle"],
            (0, 200, 300, 400),
            (1, 1, 2, 2),
            strict=True,
        ),
        start=10,
    ):
        lifecycle["dispatch_ms"] = dispatch_ms
        lifecycle["scheduler_dispatch_line"] = dispatch_line
        lifecycle["scheduler_generation"] = generation
        lifecycle["terminal_ms"] = dispatch_ms + 25
        lifecycle["deadline_ms"] = dispatch_ms + 120_000
    bundle = _bundle(scenario, rows, observations)
    bundle["event_log"] = [event]
    return bundle


def test_s70_b4_scheduler_restart_resumes_committed_p29_rows() -> None:
    fixture = _s70_b4_scheduler_bundle()
    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "PASS", verdict


@pytest.mark.parametrize(
    "mutation",
    (
        "legacy_after",
        "missing_post_epoch",
        "no_inflight_job",
        "stale_restart",
        "dispatch_out_of_epoch",
        "wrong_target",
        "error106",
    ),
)
def test_s70_b4_scheduler_restart_evidence_fails_closed(mutation: str) -> None:
    fixture = _s70_b4_scheduler_bundle()
    if mutation == "legacy_after":
        fixture["rows"][2] = _row(3, tail=False, outcome="none")
        fixture["rows"][2]["event_epoch"] = 1
    elif mutation == "missing_post_epoch":
        fixture["rows"] = fixture["rows"][:2]
    elif mutation == "no_inflight_job":
        fixture["event_log"][0]["receipt"]["coordination"]["clients"]["C1"][
            "active_before"
        ] = 0
    elif mutation == "stale_restart":
        fixture["event_log"][0]["receipt"]["after"]["started_at"] = (
            fixture["event_log"][0]["receipt"]["before"]["started_at"]
        )
    elif mutation == "dispatch_out_of_epoch":
        fixture["observations"]["job_lifecycle"][2]["dispatch_ms"] = 100
    elif mutation == "wrong_target":
        fixture["scenario"]["timeline"][0]["instance"] = "C1"
    else:
        fixture["observations"]["error106_job_ids"] = ["3"]
    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "FAIL"
    failed = {item["id"] for item in verdict["clauses"] if item["status"] == "FAIL"}
    assert failed & {"engagement.expected", "s70.b4-scheduler-restart"}


def _route_process(
    executable: str,
    *,
    pid: int,
    ppid: int,
    start_ticks: int,
    argv: tuple[str, ...] = (),
) -> dict[str, object]:
    return {
        "argv": [executable, *argv],
        "exe": executable,
        "exe_evidence": "argv0-after-proc-exe-eacces",
        "pid": pid,
        "ppid": ppid,
        "start_ticks": start_ticks,
        "uid": 65534,
    }


def _s70_b4_client_bundle() -> dict[str, object]:
    scenario = _scenario("S'C'F'")
    scenario["id"] = "S70-b4-client-route-restart"
    scenario["expect"]["engagement"] = "s70-b4-client-route-restart"
    scenario["expect"]["reuse"] = "none-when-legacy"
    scenario["workload"]["corpus"] = "fmt-100"
    scenario["workload"]["jobs"] = 1
    scenario["timeline"] = [
        {"trigger": "job 75", "action": "restart", "instance": "C1"}
    ]
    daemon = _route_process(
        "/opt/icecream/sbin/iceccd", pid=1, ppid=0, start_ticks=5
    )
    before_owner = _route_process(
        "/opt/icecream/sbin/icecc-cache-service",
        pid=10,
        ppid=1,
        start_ticks=10,
        argv=("--c-store-guid", "1" * 32),
    )
    after_owner = _route_process(
        "/opt/icecream/sbin/icecc-cache-service",
        pid=11,
        ppid=1,
        start_ticks=20,
        argv=("--c-store-guid", "2" * 32),
    )
    pause = {
        "action": "pause",
        "active_after": 0,
        "active_before": 1,
        "client": "C1",
        "epoch": 1,
        "finished_ms": 7450,
        "schema": "icefarm-event-gate-v1",
        "started_ms": 7400,
        "status": "PAUSED",
        "turn": "A",
    }
    resume = dict(
        pause,
        action="resume",
        active_before=0,
        finished_ms=7510,
        started_ms=7500,
        status="OPEN",
    )
    event = {
        "action": "restart",
        "event_epoch": 1,
        "event_index": 0,
        "fired_ms": 7500,
        "instance": "C1",
        "last_dispatched_job": 75,
        "trigger": "job 75",
        "workload_dispatch_count": 75,
        "receipt": {
            "action": "restart",
            "after": {
                "container_id": SHA_A,
                "container_started_at": "2026-09-06T00:00:00Z",
                "daemon": copy.deepcopy(daemon),
                "route_owner": after_owner,
            },
            "before": {
                "container_id": SHA_A,
                "container_started_at": "2026-09-06T00:00:00Z",
                "daemon": copy.deepcopy(daemon),
                "route_owner": before_owner,
            },
            "coordination": {
                "clients": {"C1": pause},
                "ready_ms": 7500,
                "readiness": {
                    "host": "h1",
                    "lifecycle": 3,
                    "line": "cache sidecar adapter state=2 lifecycle=3",
                    "log_path": "/run/C1/log/client-daemon.log",
                    "offset": 10,
                    "state": 2,
                },
                "resume": {"C1": resume},
                "signal": {
                    "daemon": copy.deepcopy(daemon),
                    "mechanism": "pidfd_send_signal",
                    "route_owner": copy.deepcopy(before_owner),
                    "schema": "icefarm-client-route-signal-v1",
                    "sent_ms": 7475,
                    "signal": 9,
                },
            },
            "event_epoch": 1,
            "instance": "C1",
            "schema": "icefarm-client-route-restart-v1",
            "turn": "A",
        },
    }
    rows: list[dict[str, object]] = []
    for job in range(1, 101):
        tu_index = (job - 1) % 50 + 1
        row = _row(job)
        row["tu"] = f"files/tu-{tu_index}.ii"
        row["event_epoch"] = 0 if job <= 75 else 1
        row["c_to_f_bytes"] = (
            100 + tu_index if 51 <= job <= 75 else 1000 + tu_index
        )
        row["f_to_c_bytes"] = 500 + tu_index
        rows.append(row)
    # The first occurrence of TU 26 is not an empty-store reference: 25
    # earlier TUs have already populated the content-addressed route.  A fresh
    # route therefore transfers more bytes for the same TU after the restart.
    rows[25]["c_to_f_bytes"] = 900 + 26
    observations = _observations(rows)
    observations["p50_source_routes"] = {
        "record_count": len(rows),
        "records": [
            {
                "c_store_guid": "1" * 32 if row["event_epoch"] == 0 else "2" * 32,
                "c_to_f_bytes": row["c_to_f_bytes"],
                "client_instance": row["client_instance"],
                "f_to_c_bytes": row["f_to_c_bytes"],
                "job_id": row["job_id"],
                "profile": "P29V1",
                "raw_bytes": 4096,
                "raw_digest": "3" * 32,
                "schema": "icefarm-p50-source-route-v1",
                "tu_seq": job - 1 if job <= 75 else job - 76,
                "worker_instance": row["cs"],
            }
            for job, row in enumerate(rows, start=1)
        ],
    }
    for job, lifecycle in enumerate(observations["job_lifecycle"], start=1):
        dispatch_ms = (job - 1) * 100
        lifecycle["dispatch_ms"] = dispatch_ms
        lifecycle["terminal_ms"] = dispatch_ms + 25
        lifecycle["deadline_ms"] = dispatch_ms + 120_000
    bundle = _bundle(scenario, rows, observations)
    bundle["event_log"] = [event]
    return bundle


def test_s70_b4_client_route_restart_makes_next_tu_cold() -> None:
    fixture = _s70_b4_client_bundle()
    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "PASS", verdict


def _precise_client_epoch_bundle():
    fixture = _s70_b4_client_bundle()
    fixture.setdefault("plan", {})["client_route_epoch_contract"] = "icefarm-client-route-wrapper-epoch-v1"
    fixture["plan"].setdefault("commands", [])
    for item in fixture["observations"]["job_lifecycle"]:
        item["wrapper_started_ms"] = item["dispatch_ms"]
        item["wrapper_finished_ms"] = item["terminal_ms"]
        item["dispatch_ms"] = item["dispatch_ms"] // 1000 * 1000
        item["terminal_ms"] = item["terminal_ms"] // 1000 * 1000
    return fixture


def test_client_epoch_precision_preserves_cold_restart_checks():
    fixture = _precise_client_epoch_bundle()
    assert evaluate_bundle(fixture)["status"] == "PASS"
    del fixture["plan"]["client_route_epoch_contract"]
    assert evaluate_bundle(fixture)["status"] == "FAIL"


@pytest.mark.parametrize("mutation", ["missing_time", "early_start", "wrong_epoch", "stale_store", "no_sequence_reset", "wrong_contract"])
def test_client_epoch_precision_negative_evidence(mutation):
    fixture = _precise_client_epoch_bundle()
    timing = fixture["observations"]["job_lifecycle"][75]
    if mutation == "missing_time":
        del timing["wrapper_started_ms"]
    elif mutation == "early_start":
        timing["wrapper_started_ms"] = 7470
    elif mutation == "wrong_epoch":
        fixture["rows"][75]["event_epoch"] = 0
    elif mutation == "stale_store":
        fixture["observations"]["p50_source_routes"]["records"][75]["c_store_guid"] = "1" * 32
    elif mutation == "no_sequence_reset":
        fixture["observations"]["p50_source_routes"]["records"][75]["tu_seq"] = 76
    else:
        fixture["plan"]["client_route_epoch_contract"] = "unknown"
    assert evaluate_bundle(fixture)["status"] == "FAIL"


@pytest.mark.parametrize(
    "mutation",
    (
        "next_tu_warm",
        "no_warm_precondition",
        "same_route_owner",
        "changed_daemon",
        "dispatch_out_of_epoch",
        "missing_source_routes",
        "stale_source_guid",
        "no_source_sequence_reset",
        "missing_route_guid",
        "legacy_after",
        "error106",
        "parallel_workload",
    ),
)
def test_s70_b4_client_route_restart_evidence_fails_closed(mutation: str) -> None:
    fixture = _s70_b4_client_bundle()
    if mutation == "next_tu_warm":
        fixture["rows"][75]["c_to_f_bytes"] = 126
    elif mutation == "no_warm_precondition":
        for row in fixture["rows"][50:75]:
            tu_index = int(str(row["tu"]).removesuffix(".ii").rsplit("-", 1)[1])
            row["c_to_f_bytes"] = 1000 + tu_index
    elif mutation == "same_route_owner":
        fixture["event_log"][0]["receipt"]["after"]["route_owner"] = copy.deepcopy(
            fixture["event_log"][0]["receipt"]["before"]["route_owner"]
        )
    elif mutation == "changed_daemon":
        fixture["event_log"][0]["receipt"]["after"]["daemon"]["start_ticks"] = 6
    elif mutation == "dispatch_out_of_epoch":
        fixture["observations"]["job_lifecycle"][75]["dispatch_ms"] = 7400
    elif mutation == "missing_source_routes":
        del fixture["observations"]["p50_source_routes"]
    elif mutation == "stale_source_guid":
        fixture["observations"]["p50_source_routes"]["records"][75][
            "c_store_guid"
        ] = "1" * 32
    elif mutation == "no_source_sequence_reset":
        fixture["observations"]["p50_source_routes"]["records"][75]["tu_seq"] = 76
    elif mutation == "missing_route_guid":
        fixture["event_log"][0]["receipt"]["after"]["route_owner"]["argv"] = []
    elif mutation == "legacy_after":
        fixture["rows"][75].update(
            reuse=None,
            session_outcome="none",
            tail_present=False,
            tail_profile=None,
        )
    elif mutation == "error106":
        fixture["observations"]["error106_job_ids"] = ["76"]
    else:
        fixture["scenario"]["workload"]["jobs"] = 2
    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "FAIL"
    failed = {item["id"] for item in verdict["clauses"] if item["status"] == "FAIL"}
    assert failed & {"engagement.expected", "s70.b4-client-route-restart"}


def _worker_restart_event(
    index: int, *, before_started: str, after_started: str, fired_ms: int
) -> dict[str, object]:
    return {
        "action": "restart",
        "event_epoch": index + 1,
        "event_index": index,
        "fired_ms": fired_ms,
        "instance": "F1",
        "last_dispatched_job": None,
        "trigger": f"t+{(index + 1) * 30}",
        "workload_dispatch_count": 0,
        "receipt": {
            "action": "restart",
            "after": {
                "container_id": SHA_A,
                "started_at": after_started,
            },
            "before": {
                "container_id": SHA_A,
                "started_at": before_started,
            },
            "coordination": {
                "ready_ms": fired_ms,
                "readiness": {
                    "cache_line": "cache sidecar adapter state=2 lifecycle=3",
                    "host": "h1",
                    "line": "ICECREAM daemon F1 starting up",
                    "log_path": "/run/F1/log/iceccd.log",
                    "offset": index * 100,
                    "role": "F",
                },
                "scheduler_rejoin": {
                    "cache_line": (
                        "RELOGIN F1(10.0.0.1): cache=on cache_wire=v1 "
                        "cache_protocol=1 cache_profiles=p29v1 zstd_tu zstd_route"
                    ),
                    "cache_protocol": 1,
                    "bytes": 1,
                    "host": "h1",
                    "login_line": "login F1 protocol version: 50",
                    "log_path": "/run/S1/log/scheduler.log",
                    "loss_job_ids": [],
                    "offset": index * 100,
                    "profile": "P29V1",
                    "role_protocol": 50,
                    "scheduler": "S1",
                    "sha256": "a" * 64,
                    "target": "F1",
                },
                "worker_snapshot": "F1\nF2\n",
                "workers": ["F1", "F2"],
            },
            "event_epoch": index + 1,
            "instance": "F1",
            "schema": "icefarm-worker-restart-v1",
            "turn": "A",
        },
    }


def _s70_b4_worker_bundle() -> dict[str, object]:
    scenario = _scenario("S'C'F'", worker_versions=(50, 50))
    scenario["id"] = "S70-b4-worker-bounces"
    scenario["expect"]["engagement"] = "s70-b4-worker-bounces"
    scenario["expect"]["reuse"] = "none-when-legacy"
    scenario["workload"].update(
        corpus="firefox-1000", jobs=36, repeat=6, turns=["A"]
    )
    scenario["timeline"] = [
        {"trigger": f"t+{seconds}", "action": "restart", "instance": "F1"}
        for seconds in (30, 60, 90)
    ]
    rows: list[dict[str, object]] = []
    for job in range(1, 401):
        epoch = (job - 1) // 100
        position = (job - 1) % 100
        tu_index = position % 50 + 1
        worker = "F1" if job % 2 else "F2"
        row = _row(job, worker=worker)
        row["event_epoch"] = epoch
        row["tu"] = f"files/tu-{tu_index}.ii"
        cold = position < 50
        if worker == "F1":
            row["c_to_f_bytes"] = (1000 if cold else 100) + tu_index
        else:
            row["c_to_f_bytes"] = (1000 if epoch == 0 and cold else 100) + tu_index
        row["f_to_c_bytes"] = 500 + tu_index
        rows.append(row)
    observations = _observations(rows)
    observations["process_loss_recovery_bindings"] = []
    observations["process_loss_recovery_job_ids"] = []
    for job, lifecycle in enumerate(observations["job_lifecycle"], start=1):
        dispatch_ms = (job - 1) * 100
        lifecycle["dispatch_ms"] = dispatch_ms
        lifecycle["terminal_ms"] = dispatch_ms + 25
        lifecycle["deadline_ms"] = dispatch_ms + 120_000
        lifecycle["scheduler_dispatch_line"] = job
        lifecycle["scheduler_generation"] = 1
    observations["scheduler_reconciliation"] = {
        "preexposure_redispatches": [
            {
                "attempt_index": 0,
                "client": "C1",
                "generation": 1,
                "lost_dispatch_line": 98,
                "lost_dispatch_ms": 9_800,
                "lost_worker": "F1",
                "marker_line": 99,
                "marker_ms": 9_900,
                "replacement_dispatch_line": 100,
                "replacement_dispatch_ms": 9_900,
                "replacement_worker": "F2",
                "row_job_id": rows[99]["job_id"],
                "scheduler_job": 1_000,
            }
        ]
    }
    bundle = _bundle(scenario, rows, observations)
    bundle["event_log"] = [
        _worker_restart_event(
            0,
            before_started="2026-09-06T00:00:00Z",
            after_started="2026-09-06T00:01:00Z",
            fired_ms=10_000,
        ),
        _worker_restart_event(
            1,
            before_started="2026-09-06T00:01:00Z",
            after_started="2026-09-06T00:02:00Z",
            fired_ms=20_000,
        ),
        _worker_restart_event(
            2,
            before_started="2026-09-06T00:02:00Z",
            after_started="2026-09-06T00:03:00Z",
            fired_ms=30_000,
        ),
    ]
    return bundle


def _s70_b4_worker_action_lineage_bundle() -> dict[str, object]:
    fixture = _s70_b4_worker_bundle()
    fixture["scenario"]["expect"]["worker_cold_witness"] = (
        "p29-action-lineage-v1"
    )
    counters: dict[tuple[str, int], int] = defaultdict(int)
    source_records: list[dict[str, object]] = []
    lineage_records: list[dict[str, object]] = []
    c_store_guid = "c" * 32
    for row in fixture["rows"]:
        worker = str(row["cs"])
        epoch = int(row["event_epoch"])
        relationship = (worker, epoch if worker == "F1" else 0)
        counters[relationship] += 1
        session_serial = counters[relationship]
        guid_number = (epoch + 1) if worker == "F1" else 10
        f_store_guid = f"{guid_number:032x}"
        job_id = str(row["job_id"])
        tu_seq = int(job_id)
        raw_digest = hashlib.sha256(job_id.encode()).hexdigest()[:32]
        source_records.append(
            {
                "c_store_guid": c_store_guid,
                "c_to_f_bytes": row["c_to_f_bytes"],
                "client_instance": row["client_instance"],
                "f_to_c_bytes": row["f_to_c_bytes"],
                "job_id": job_id,
                "profile": "P29V1",
                "raw_bytes": 4096,
                "raw_digest": raw_digest,
                "schema": "icefarm-p50-source-route-v1",
                "tu_seq": tu_seq,
                "worker_instance": worker,
            }
        )
        lineage_records.append(
            {
                "c_store_guid": c_store_guid,
                "f_store_guid": f_store_guid,
                "history_nonce": 1,
                "job_id": job_id,
                "previous_f_store_guid": (
                    "0" * 32 if session_serial == 1 else f_store_guid
                ),
                "raw_digest": raw_digest,
                "rel_seq": session_serial - 1,
                "schema": "icefarm-p29-action-lineage-v1",
                "session_serial": session_serial,
                "tu_seq": tu_seq,
                "worker_instance": worker,
            }
        )
        # The deterministic witness must not depend on a later dispatch of the
        # same translation unit.
        row["tu"] = f"files/unique-{job_id}.ii"
    fixture["observations"]["p50_source_routes"] = {
        "record_count": len(source_records),
        "records": source_records,
    }
    fixture["observations"]["p29_action_lineage"] = {
        "record_count": len(lineage_records),
        "records": lineage_records,
        "schema": "icefarm-p29-action-lineage-v1",
    }
    return fixture


def test_s70_b4_worker_bounces_are_cold_while_other_worker_continues() -> None:
    fixture = _s70_b4_worker_bundle()
    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "PASS", verdict


def test_s70_b4_worker_action_lineage_does_not_require_same_tu_placement() -> None:
    fixture = _s70_b4_worker_action_lineage_bundle()
    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "PASS", verdict


@pytest.mark.parametrize("fault", ["unknown", "wrong_engagement", "missing_lineage", "missing_boundaries"])
def test_worker_rejoin_contract_fails_closed(fault):
    fixture = _s70_b4_worker_action_lineage_bundle()
    fixture.setdefault("plan", {})["worker_rejoin_epoch_contract"] = "icefarm-worker-rejoin-log-order-v1"
    if fault == "unknown":
        fixture["plan"]["worker_rejoin_epoch_contract"] = "unknown"
    elif fault == "wrong_engagement":
        fixture["scenario"]["expect"]["engagement"] = "all"
    elif fault == "missing_lineage":
        fixture["scenario"]["expect"].pop("worker_cold_witness")
    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "FAIL"
    if fault != "missing_boundaries":
        assert any(c["id"] == "worker-rejoin.contract" and c["status"] == "FAIL"
                   for c in verdict["clauses"])


def _s70_worker_rejoin_bundle():
    fixture = _s70_b4_worker_action_lineage_bundle()
    fixture.setdefault("plan", {})["worker_rejoin_epoch_contract"] = "icefarm-worker-rejoin-log-order-v1"
    fixture["plan"]["commands"] = []
    fixture["observations"]["worker_rejoin_boundaries"] = [
        {"event_epoch": i + 1, "worker_instance": "F1", "scheduler_generation": 1,
         "scheduler_rejoin_line": (i + 1) * 100,
         "container_id": event["receipt"]["after"]["container_id"],
         "started_at": event["receipt"]["after"]["started_at"],
         "rejoin_sha256": event["receipt"]["coordination"]["scheduler_rejoin"]["sha256"]}
        for i, event in enumerate(fixture["event_log"])
    ]
    for job in (101, 201, 301):
        lifecycle = fixture["observations"]["job_lifecycle"][job - 1]
        lifecycle["dispatch_ms"] -= 1
    return fixture


def test_worker_rejoin_pre_ready_cold_witness_passes_only_new_contract():
    fixture = _s70_worker_rejoin_bundle()
    assert evaluate_bundle(fixture)["status"] == "PASS"
    fixture["plan"].pop("worker_rejoin_epoch_contract")
    assert evaluate_bundle(fixture)["status"] == "FAIL"


@pytest.mark.parametrize("field,value", [
    ("container_id", "0" * 64), ("started_at", "wrong"),
    ("rejoin_sha256", "0" * 64), ("scheduler_generation", 2),
    ("scheduler_rejoin_line", True), ("worker_instance", "F2"),
    ("event_epoch", 3),
])
def test_worker_rejoin_boundary_mutations_fail(field, value):
    fixture = _s70_worker_rejoin_bundle()
    fixture["observations"]["worker_rejoin_boundaries"][0][field] = value
    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "FAIL"
    assert any("@observations:worker-rejoin-boundaries" in c["offending_job_ids"]
               for c in verdict["clauses"])


def _roll_s70_c_store_in_epoch_three(
    fixture: dict[str, object], *, records: int
) -> None:
    rows = {row["job_id"]: row for row in fixture["rows"]}
    lineages = [
        record
        for record in fixture["observations"]["p29_action_lineage"]["records"]
        if record["worker_instance"] == "F1"
        and rows[record["job_id"]]["event_epoch"] == 3
    ]
    selected = sorted(lineages, key=lambda record: record["session_serial"])[
        -records:
    ]
    selected_ids = {record["job_id"] for record in selected}
    new_c_store_guid = "d" * 32
    for rel_seq, record in enumerate(selected):
        record["c_store_guid"] = new_c_store_guid
        record["history_nonce"] = 1
        record["previous_f_store_guid"] = "0" * 32
        record["rel_seq"] = rel_seq
    for record in fixture["observations"]["p50_source_routes"]["records"]:
        if record["job_id"] in selected_ids:
            record["c_store_guid"] = new_c_store_guid


def _s70_capacity_replacement_lineage_bundle() -> dict[str, object]:
    fixture = _s70_b4_worker_source_transfer_recovery_bundle()
    lineage_fixture = _s70_b4_worker_action_lineage_bundle()
    fixture["scenario"]["expect"]["worker_cold_witness"] = (
        "p29-action-lineage-v1"
    )
    for field in ("p50_source_routes", "p29_action_lineage"):
        fixture["observations"][field] = copy.deepcopy(
            lineage_fixture["observations"][field]
        )

    retry_job = fixture["rows"][100]["job_id"]
    for record in fixture["observations"]["p50_source_routes"]["records"]:
        if record["job_id"] == retry_job:
            record["worker_instance"] = "F2"
    for record in fixture["observations"]["p29_action_lineage"]["records"]:
        row = fixture["rows"][int(record["job_id"]) - 1]
        if record["job_id"] == retry_job:
            record["worker_instance"] = "F2"
            record["f_store_guid"] = f"{10:032x}"
        elif record["worker_instance"] == "F1" and row["event_epoch"] == 1:
            record["session_serial"] -= 1
            record["rel_seq"] -= 1
            if record["session_serial"] == 1:
                record["previous_f_store_guid"] = "0" * 32

    source_failure = fixture["observations"]["failed_p50_source_transfers"]
    source_failure["records"][0]["error"] = 0x5002
    binding = fixture["observations"]["successful_strict_p50_retry_bindings"][0]
    binding["first_terminal_ms"] = 40_000
    return fixture


def test_s70_b4_worker_accepts_authenticated_c_store_capacity_rollover() -> None:
    fixture = _s70_capacity_replacement_lineage_bundle()
    _roll_s70_c_store_in_epoch_three(fixture, records=10)

    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "PASS", verdict


def test_s70_b4_worker_rejects_c_store_rollover_without_warm_lineage() -> None:
    fixture = _s70_capacity_replacement_lineage_bundle()
    _roll_s70_c_store_in_epoch_three(fixture, records=1)

    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "FAIL"
    failed = {item["id"] for item in verdict["clauses"] if item["status"] == "FAIL"}
    assert "s70.b4-worker-bounces" in failed


def test_s70_b4_worker_rejects_unauthenticated_c_store_rollover() -> None:
    fixture = _s70_b4_worker_action_lineage_bundle()
    _roll_s70_c_store_in_epoch_three(fixture, records=10)

    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "FAIL"
    failed = {item["id"] for item in verdict["clauses"] if item["status"] == "FAIL"}
    assert "s70.b4-worker-bounces" in failed


def test_s70_b4_worker_uses_f_transaction_order_not_scheduler_order() -> None:
    fixture = _s70_b4_worker_action_lineage_bundle()
    records = [
        record
        for record in fixture["observations"]["p29_action_lineage"]["records"]
        if record["worker_instance"] == "F1"
        and fixture["rows"][int(record["job_id"]) - 1]["event_epoch"] == 1
    ]
    scheduler_first = records[0]
    sixth_f_transaction = next(
        record for record in records if record["session_serial"] == 6
    )
    for field in ("previous_f_store_guid", "rel_seq", "session_serial"):
        scheduler_first[field], sixth_f_transaction[field] = (
            sixth_f_transaction[field],
            scheduler_first[field],
        )

    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "PASS", verdict


@pytest.mark.parametrize(
    "mutation",
    (
        "missing-observation",
        "bad-count",
        "missing-record",
        "missing-source-record",
        "extra-source-record",
        "source-byte-mismatch",
        "extra-field",
        "c-guid-mismatch",
        "raw-digest-mismatch",
        "tu-seq-mismatch",
        "worker-mismatch",
        "cold-previous-guid",
        "cold-session",
        "cold-history",
        "cold-rel-seq",
        "reused-store-guid",
        "missing-warm-progression",
    ),
)
def test_s70_b4_worker_action_lineage_fails_closed(mutation: str) -> None:
    fixture = _s70_b4_worker_action_lineage_bundle()
    observation = fixture["observations"]["p29_action_lineage"]
    records = observation["records"]
    first_epoch_1 = next(
        record
        for record in records
        if record["worker_instance"] == "F1"
        and fixture["rows"][int(record["job_id"]) - 1]["event_epoch"] == 1
    )
    if mutation == "missing-observation":
        del fixture["observations"]["p29_action_lineage"]
    elif mutation == "bad-count":
        observation["record_count"] -= 1
    elif mutation == "missing-record":
        records.pop()
        observation["record_count"] -= 1
    elif mutation == "missing-source-record":
        fixture["observations"]["p50_source_routes"]["records"].pop()
        fixture["observations"]["p50_source_routes"]["record_count"] -= 1
    elif mutation == "extra-source-record":
        forged = copy.deepcopy(
            fixture["observations"]["p50_source_routes"]["records"][0]
        )
        forged["job_id"] = "forged"
        fixture["observations"]["p50_source_routes"]["records"].append(forged)
        fixture["observations"]["p50_source_routes"]["record_count"] += 1
    elif mutation == "source-byte-mismatch":
        fixture["observations"]["p50_source_routes"]["records"][100][
            "c_to_f_bytes"
        ] += 1
    elif mutation == "extra-field":
        first_epoch_1["forged"] = True
    elif mutation == "c-guid-mismatch":
        first_epoch_1["c_store_guid"] = "d" * 32
    elif mutation == "raw-digest-mismatch":
        first_epoch_1["raw_digest"] = "d" * 32
    elif mutation == "tu-seq-mismatch":
        first_epoch_1["tu_seq"] += 1
    elif mutation == "worker-mismatch":
        first_epoch_1["worker_instance"] = "F2"
    elif mutation == "cold-previous-guid":
        first_epoch_1["previous_f_store_guid"] = first_epoch_1["f_store_guid"]
    elif mutation == "cold-session":
        first_epoch_1["session_serial"] = 2
    elif mutation == "cold-history":
        first_epoch_1["history_nonce"] = 2
    elif mutation == "cold-rel-seq":
        first_epoch_1["rel_seq"] = 1
    elif mutation == "reused-store-guid":
        first_epoch_2 = next(
            record
            for record in records
            if record["worker_instance"] == "F1"
            and fixture["rows"][int(record["job_id"]) - 1]["event_epoch"] == 2
        )
        first_epoch_2["f_store_guid"] = first_epoch_1["f_store_guid"]
    else:
        second_epoch_1 = next(
            record
            for record in records
            if record["worker_instance"] == "F1"
            and record["f_store_guid"] == first_epoch_1["f_store_guid"]
            and record["session_serial"] == 2
        )
        second_epoch_1["session_serial"] = 3

    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "FAIL"
    failed = {item["id"] for item in verdict["clauses"] if item["status"] == "FAIL"}
    assert "s70.b4-worker-bounces" in failed


@pytest.mark.parametrize(
    "mutation",
    (
        "wrong-lost-worker",
        "same-worker",
        "unbound-row",
        "wrong-final-worker",
        "outside-restart-window",
        "nonmonotonic-lines",
        "duplicate-row",
    ),
)
def test_s70_b4_worker_preexposure_redispatch_fails_closed(mutation: str) -> None:
    fixture = _s70_b4_worker_bundle()
    records = fixture["observations"]["scheduler_reconciliation"][
        "preexposure_redispatches"
    ]
    record = records[0]
    if mutation == "wrong-lost-worker":
        record["lost_worker"] = "F2"
    elif mutation == "same-worker":
        record["replacement_worker"] = "F1"
    elif mutation == "unbound-row":
        record["row_job_id"] = "missing"
    elif mutation == "wrong-final-worker":
        record["replacement_worker"] = "F9"
    elif mutation == "outside-restart-window":
        record["marker_ms"] = 30_001
        record["replacement_dispatch_ms"] = 30_001
    elif mutation == "nonmonotonic-lines":
        record["marker_line"] = record["replacement_dispatch_line"]
    else:
        records.append(copy.deepcopy(record))

    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "FAIL"
    failed = {item["id"] for item in verdict["clauses"] if item["status"] == "FAIL"}
    assert "s70.b4-worker-bounces" in failed


def test_s70_b4_worker_does_not_require_a_nondeterministic_preexposure_race() -> None:
    fixture = _s70_b4_worker_bundle()
    fixture["observations"]["scheduler_reconciliation"][
        "preexposure_redispatches"
    ] = []
    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "PASS", verdict


def test_s70_b4_worker_uses_scheduler_line_to_order_same_second_dispatches() -> None:
    fixture = _s70_b4_worker_bundle()
    lifecycle = fixture["observations"]["job_lifecycle"]
    # Jobs 101 and 103 are both F1 rows in epoch 1.  Scheduler timestamps are
    # only second-granular, so the dispatch line is the authoritative tie-break.
    lifecycle[102]["dispatch_ms"] = lifecycle[100]["dispatch_ms"]
    lifecycle[102]["terminal_ms"] = lifecycle[100]["terminal_ms"]
    lifecycle[102]["deadline_ms"] = lifecycle[100]["deadline_ms"]
    fixture["rows"][102]["c_to_f_bytes"] = 103
    fixture["rows"][152]["c_to_f_bytes"] = 50

    assert evaluate_bundle(fixture)["status"] == "PASS"

    lifecycle[102]["scheduler_dispatch_line"] = 100
    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "FAIL"
    failed = {item["id"] for item in verdict["clauses"] if item["status"] == "FAIL"}
    assert "s70.b4-worker-bounces" in failed


def _s70_b4_worker_recovery_bundle() -> dict[str, object]:
    fixture = _s70_b4_worker_bundle()
    row = fixture["rows"][100]
    row.update(
        cs="F2",
        retries=1,
        reuse=False,
        session_outcome="committed",
        tail_present=True,
        tail_profile="P29V1",
    )
    lifecycle = fixture["observations"]["job_lifecycle"][100]
    lifecycle.update(
        dispatch_ms=9_900,
        first_dispatch_ms=9_900,
        final_dispatch_ms=10_050,
        terminal_ms=10_075,
        deadline_ms=129_900,
    )
    fixture["observations"]["assignment_lifecycle"][100]["attempts"] = [
        {
            "generation": 1,
            "scheduler_job": 701,
            "terminal": "process-loss-recovery",
            "worker": "F1",
        },
        {
            "generation": 1,
            "scheduler_job": 702,
            "terminal": "completion",
            "worker": "F2",
        },
    ]
    fixture["event_log"][0]["receipt"]["coordination"]["scheduler_rejoin"][
        "loss_job_ids"
    ] = [701]
    fixture["observations"]["process_loss_recovery_job_ids"] = [row["job_id"]]
    fixture["observations"]["process_loss_recovery_bindings"] = [
        {
            "attempt_index": 0,
            "job_id": row["job_id"],
            "scheduler_job": 701,
            "worker": "F1",
        }
    ]
    fixture["observations"]["failed_p50_result_identities"] = {
        "record_count": 1,
        "records": [
            {
                "assignment_epoch": 1,
                "assignment_nonce": 1,
                "attempt_index": 0,
                "reason": "worker-restart-loss",
                "result_identity_present": False,
                "row_job_id": row["job_id"],
                "scheduler_job": 701,
                "worker": "F1",
            }
        ],
    }
    fixture["observations"]["successful_strict_p50_retry_bindings"] = [
        {
            "failure_reason": "worker-restart-loss",
            "final_dispatch_ms": 10_050,
            "final_generation": 1,
            "final_scheduler_job": 702,
            "final_terminal_ms": 10_075,
            "final_worker": "F2",
            "first_dispatch_ms": 9_900,
            "first_generation": 1,
            "first_scheduler_job": 701,
            "first_terminal": "process-loss-recovery",
            "first_terminal_ms": 10_000,
            "first_worker": "F1",
            "job_id": row["job_id"],
        }
    ]
    return fixture


def test_s70_b4_worker_recovery_uses_final_dispatch_epoch_and_loss_receipt() -> None:
    fixture = _s70_b4_worker_recovery_bundle()
    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "PASS", verdict


def _s70_b4_worker_late_result_bundle() -> dict[str, object]:
    fixture = _s70_b4_worker_bundle()
    row = fixture["rows"][100]
    lifecycle = fixture["observations"]["job_lifecycle"][100]
    lifecycle.update(
        dispatch_ms=10_000,
        final_dispatch_ms=10_000,
        first_dispatch_ms=10_000,
        terminal="process-loss-recovery",
        terminal_ms=10_000,
        deadline_ms=130_000,
        scheduler_generation=1,
    )
    fixture["observations"]["assignment_lifecycle"][100]["attempts"] = [
        {
            "generation": 1,
            "scheduler_job": 701,
            "terminal": "process-loss-recovery",
            "worker": "F1",
        }
    ]
    fixture["event_log"][0]["receipt"]["coordination"]["scheduler_rejoin"][
        "loss_job_ids"
    ] = [701]
    fixture["observations"]["process_loss_recovery_job_ids"] = [row["job_id"]]
    fixture["observations"]["process_loss_recovery_bindings"] = [
        {
            "attempt_index": 0,
            "job_id": row["job_id"],
            "scheduler_job": 701,
            "worker": "F1",
        }
    ]
    fixture["observations"]["successful_strict_p50_late_result_bindings"] = [
        {
            "attempt_index": 0,
            "dispatch_ms": 10_000,
            "generation": 1,
            "job_id": row["job_id"],
            "restart_event_index": 0,
            "restart_fired_ms": 10_000,
            "scheduler_job": 701,
            "terminal_ms": 10_000,
            "worker": "F1",
        }
    ]
    return fixture


def test_s70_b4_exact_late_result_is_bound_to_declared_worker_loss() -> None:
    fixture = _s70_b4_worker_late_result_bundle()
    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "PASS", verdict


@pytest.mark.parametrize(
    "mutation",
    (
        "missing-binding",
        "wrong-job",
        "wrong-worker",
        "wrong-retry-c-guid",
        "wrong-generation",
        "wrong-event",
        "wrong-fired-time",
        "missing-loss-job",
        "retry",
        "not-exact",
        "not-committed",
        "not-p29",
        "completion-terminal",
        "missing-process-binding",
    ),
)
def test_s70_b4_late_result_binding_fails_closed(mutation: str) -> None:
    fixture = _s70_b4_worker_late_result_bundle()
    bindings = fixture["observations"][
        "successful_strict_p50_late_result_bindings"
    ]
    binding = bindings[0]
    row = fixture["rows"][100]
    if mutation == "missing-binding":
        bindings.clear()
    elif mutation == "wrong-job":
        binding["scheduler_job"] = 702
    elif mutation == "wrong-worker":
        binding["worker"] = "F2"
    elif mutation == "wrong-generation":
        binding["generation"] = 2
    elif mutation == "wrong-event":
        binding["restart_event_index"] = 1
    elif mutation == "wrong-fired-time":
        binding["restart_fired_ms"] = 10_001
    elif mutation == "missing-loss-job":
        fixture["event_log"][0]["receipt"]["coordination"][
            "scheduler_rejoin"
        ]["loss_job_ids"] = []
    elif mutation == "retry":
        row["retries"] = 1
    elif mutation == "not-exact":
        row["exact"] = False
    elif mutation == "not-committed":
        row["session_outcome"] = "fallback"
    elif mutation == "not-p29":
        row["tail_profile"] = "ZSTD_TU"
    elif mutation == "completion-terminal":
        fixture["observations"]["assignment_lifecycle"][100]["attempts"][0][
            "terminal"
        ] = "completion"
    else:
        fixture["observations"]["process_loss_recovery_bindings"] = []

    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "FAIL"
    failed = {item["id"] for item in verdict["clauses"] if item["status"] == "FAIL"}
    assert failed & {
        "engagement.expected",
        "recovery.strict-p50-late-result-bindings",
        "s70.b4-worker-bounces",
    }


def _s70_b4_worker_result_stream_recovery_bundle() -> dict[str, object]:
    fixture = _s70_b4_worker_recovery_bundle()
    row = fixture["rows"][100]
    fixture["event_log"][0]["receipt"]["coordination"]["scheduler_rejoin"][
        "loss_job_ids"
    ] = []
    fixture["observations"]["process_loss_recovery_job_ids"] = []
    fixture["observations"]["process_loss_recovery_bindings"] = []
    fixture["observations"]["error106_job_ids"] = [row["job_id"]]
    fixture["observations"]["assignment_lifecycle"][100]["attempts"][0][
        "terminal"
    ] = "cancellation"
    fixture["observations"]["failed_p50_result_identities"]["records"][0][
        "reason"
    ] = "result-stream-loss"
    binding = fixture["observations"][
        "successful_strict_p50_retry_bindings"
    ][0]
    binding["failure_reason"] = "result-stream-loss"
    binding["first_terminal"] = "cancellation"
    return fixture


def test_s70_b4_result_stream_error106_is_recovered_by_exact_strict_retry() -> None:
    fixture = _s70_b4_worker_result_stream_recovery_bundle()
    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "PASS", verdict


def test_result_stream_allows_late_scheduler_cancellation_settlement() -> None:
    fixture = _s70_b4_worker_result_stream_recovery_bundle()
    binding = fixture["observations"][
        "successful_strict_p50_retry_bindings"
    ][0]
    binding["first_terminal_ms"] = (
        max(event["fired_ms"] for event in fixture["event_log"]) + 1
    )

    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "PASS", verdict


def test_result_stream_retry_accepts_only_its_exact_active_scheduler_loss() -> None:
    fixture = _s70_b4_worker_result_stream_recovery_bundle()
    binding = fixture["observations"]["successful_strict_p50_retry_bindings"][0]
    assignment = fixture["observations"]["assignment_lifecycle"][100]
    binding["first_terminal"] = "scheduler-loss"
    assignment["attempts"][0]["terminal"] = "scheduler-loss"
    fixture["event_log"] = [
        {
            "action": "scheduler-loss-active",
            "receipt": {
                "lost_scheduler_generation": binding["first_generation"],
                "lost_scheduler_job": binding["first_scheduler_job"],
            },
        }
    ]

    authenticated, bad = _authenticated_strict_p50_retry_ids(
        fixture,
        fixture["observations"],
        fixture["rows"],
    )
    assert authenticated == {binding["job_id"]}
    assert bad == set()

    fixture["event_log"][0]["receipt"]["lost_scheduler_job"] += 1
    authenticated, bad = _authenticated_strict_p50_retry_ids(
        fixture,
        fixture["observations"],
        fixture["rows"],
    )
    assert authenticated == set()
    assert bad


def test_verdict_active_scheduler_epoch_uses_exact_replacement_start() -> None:
    scenario = {"instances": [{"name": "S1", "role": "S"}]}
    events = [
        {
            "action": "scheduler-loss-active",
            "fired_ms": 1_789_042_945_000,
            "instance": "S1",
            "receipt": {
                "after": {"started_at": "2026-09-10T12:21:36.251650806Z"}
            },
        }
    ]

    assert (
        _scheduler_dispatch_epoch(
            scenario, events, 1_789_042_942_000, 2
        )
        == 1
    )
    assert _scheduler_dispatch_epoch(scenario, events, 1_789_042_896_000, 2) is None

    events[0]["receipt"]["after"]["started_at"] = "not-a-timestamp"
    assert _scheduler_dispatch_epoch(scenario, events, 1_789_042_942_000, 2) is None


def _s70_b4_worker_source_transfer_recovery_bundle() -> dict[str, object]:
    fixture = _s70_b4_worker_result_stream_recovery_bundle()
    fixture["plan"] = {
        "launch_contract": "icefarm-f-init-launch-v1",
        "ports": {"instances": {"F1": 23003, "F2": 23004}},
        "topology": {
            "instances": [
                {
                    "address": "10.0.27.101",
                    "host": "h1",
                    "name": "F1",
                    "role": "F",
                },
                {
                    "address": "10.0.27.56",
                    "host": "h2",
                    "name": "F2",
                    "role": "F",
                },
            ]
        },
    }
    fixture["launch_contract"] = "icefarm-f-init-launch-v1"
    fixture["observations"]["f_init"] = {
        "instances": [
            {
                "host": "h1",
                "init": True,
                "inspect_sha256": SHA_A,
                "instance": "F1",
            },
            {
                "host": "h2",
                "init": True,
                "inspect_sha256": SHA_A,
                "instance": "F2",
            },
        ],
        "schema": "icefarm-f-init-v1",
    }
    row = fixture["rows"][100]
    fixture["observations"]["error106_job_ids"] = []
    fixture["observations"]["failed_p50_result_identities"] = {
        "record_count": 0,
        "records": [],
    }
    fixture["observations"]["failed_p50_source_transfers"] = {
        "record_count": 1,
        "records": [
            {
                "assignment_epoch": 11,
                "assignment_identity_line": 6,
                "assignment_line": 7,
                "assignment_nonce": 21,
                "attempt_index": 0,
                "c_guid": 31,
                "compile_identity_present": False,
                "error": 4,
                "failed_endpoint": "10.0.27.101:23003",
                "failure_line": 11,
                "profile": "P29V1",
                "retry_assignment_epoch": 11,
                "retry_assignment_identity_line": 16,
                "retry_assignment_line": 17,
                "retry_assignment_nonce": 22,
                "retry_c_guid": 31,
                "retry_endpoint": "10.0.27.56:23004",
                "retry_line": 12,
                "retry_scheduler_job": 702,
                "retry_tu_seq": 42,
                "row_job_id": row["job_id"],
                "scheduler_job": 701,
                "source_result_present": False,
                "status": 2,
                "transfer_attempts": 0,
                "tu_seq": 41,
                "worker": "F1",
            }
        ],
    }
    fixture["observations"]["successful_strict_p50_retry_bindings"][0][
        "failure_reason"
    ] = "source-transfer-loss"
    return fixture


def test_s70_b4_source_transfer_loss_is_recovered_by_exact_strict_retry() -> None:
    fixture = _s70_b4_worker_source_transfer_recovery_bundle()
    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "PASS", verdict


@pytest.mark.parametrize("mode", ["legacy", "unknown"])
def test_legacy_source_failure_cannot_authenticate_strict_retry(mode: str) -> None:
    fixture = _s70_b4_worker_source_transfer_recovery_bundle()
    record = fixture["observations"]["failed_p50_source_transfers"]["records"][0]
    record["source_result_status"] = None
    record["retry_mode"] = mode
    authenticated, bad = _authenticated_strict_p50_retry_ids(
        fixture, fixture["observations"], fixture["rows"]
    )
    assert not authenticated
    assert bad


def test_legacy_source_failure_is_visible_without_strict_success() -> None:
    fixture = _s70_b4_worker_source_transfer_recovery_bundle()
    observations = fixture["observations"]
    record = observations["failed_p50_source_transfers"]["records"][0]
    record["source_result_status"] = None
    record["retry_mode"] = "legacy"
    observations["successful_strict_p50_retry_bindings"] = []
    row = next(r for r in fixture["rows"] if r["job_id"] == record["row_job_id"])
    row.update(tail_present=False, tail_profile=None, session_outcome="none")
    authenticated, bad = _authenticated_strict_p50_retry_ids(
        fixture, observations, fixture["rows"]
    )
    assert not authenticated
    assert not bad
    assert evaluate_bundle(fixture)["status"] == "FAIL"


def test_source_transfer_allows_late_scheduler_cancellation_settlement() -> None:
    fixture = _s70_b4_worker_source_transfer_recovery_bundle()
    binding = fixture["observations"]["successful_strict_p50_retry_bindings"][0]
    binding["first_terminal_ms"] = binding["final_terminal_ms"] + 1

    authenticated, bad = _authenticated_strict_p50_retry_ids(
        fixture,
        fixture["observations"],
        fixture["rows"],
    )
    assert authenticated == {binding["job_id"]}
    assert bad == set()


def test_s70_b4_accepts_capacity_rollover_retry_after_restart_windows() -> None:
    fixture = _s70_b4_worker_source_transfer_recovery_bundle()
    binding = fixture["observations"]["successful_strict_p50_retry_bindings"][0]
    fixture["observations"]["failed_p50_source_transfers"]["records"][0][
        "error"
    ] = 0x5002
    binding["first_terminal_ms"] = (
        max(event["fired_ms"] for event in fixture["event_log"]) + 1
    )

    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "PASS", verdict


def test_s70_b4_rejects_unrelated_source_retry_after_restart_windows() -> None:
    fixture = _s70_b4_worker_source_transfer_recovery_bundle()
    binding = fixture["observations"]["successful_strict_p50_retry_bindings"][0]
    binding["first_terminal_ms"] = (
        max(event["fired_ms"] for event in fixture["event_log"]) + 1
    )

    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "FAIL"
    failed = {item["id"] for item in verdict["clauses"] if item["status"] == "FAIL"}
    assert "s70.b4-worker-bounces" in failed


def test_s70_b4_rejects_forged_capacity_rollover_retry_endpoint() -> None:
    fixture = _s70_b4_worker_source_transfer_recovery_bundle()
    binding = fixture["observations"]["successful_strict_p50_retry_bindings"][0]
    record = fixture["observations"]["failed_p50_source_transfers"]["records"][0]
    record["error"] = 0x5002
    record["failed_endpoint"] = "10.0.27.99:23999"
    binding["first_terminal_ms"] = (
        max(event["fired_ms"] for event in fixture["event_log"]) + 1
    )

    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "FAIL"
    failed = {item["id"] for item in verdict["clauses"] if item["status"] == "FAIL"}
    assert failed & {"retry.strict-p50-bindings", "s70.b4-worker-bounces"}


def _s70_b4_worker_uncommitted_transport_recovery_bundle() -> dict[str, object]:
    fixture = _s70_b4_worker_source_transfer_recovery_bundle()
    row = fixture["rows"][100]
    fixture["observations"]["error106_job_ids"] = [row["job_id"]]
    fixture["observations"]["failed_p50_source_transfers"] = {
        "record_count": 0,
        "records": [],
    }
    fixture["observations"]["failed_p50_uncommitted_transports"] = {
        "record_count": 1,
        "records": [
            {
                "assignment_epoch": 11,
                "assignment_identity_line": 6,
                "assignment_line": 7,
                "assignment_nonce": 21,
                "attempt_index": 0,
                "c_guid": 31,
                "compile_identity_present": False,
                "failed_endpoint": "10.0.27.101:23003",
                "normalized_error": 106,
                "normalized_line": 10,
                "original_error": 2,
                "profile_commit_present": False,
                "retry_assignment_epoch": 11,
                "retry_assignment_identity_line": 16,
                "retry_assignment_line": 17,
                "retry_assignment_nonce": 22,
                "retry_c_guid": 31,
                "retry_endpoint": "10.0.27.56:23004",
                "retry_line": 11,
                "retry_scheduler_job": 702,
                "retry_tu_seq": 42,
                "row_job_id": row["job_id"],
                "scheduler_job": 701,
                "source_result_present": False,
                "tu_seq": 41,
                "worker": "F1",
            }
        ],
    }
    fixture["observations"]["successful_strict_p50_retry_bindings"][0][
        "failure_reason"
    ] = "uncommitted-transport-loss"
    return fixture


def test_s70_b4_uncommitted_transport_loss_is_recovered_by_exact_strict_retry() -> None:
    fixture = _s70_b4_worker_uncommitted_transport_recovery_bundle()
    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "PASS", verdict


def test_uncommitted_transport_allows_late_scheduler_cancellation_settlement() -> None:
    fixture = _s70_b4_worker_uncommitted_transport_recovery_bundle()
    binding = fixture["observations"]["successful_strict_p50_retry_bindings"][0]
    binding["first_terminal_ms"] = binding["final_terminal_ms"] + 1

    authenticated, bad = _authenticated_strict_p50_retry_ids(
        fixture,
        fixture["observations"],
        fixture["rows"],
    )
    assert authenticated == {binding["job_id"]}
    assert bad == set()


@pytest.mark.parametrize(
    "mutation",
    (
        "missing-observation",
        "wrong-first-job",
        "wrong-retry-job",
        "wrong-worker",
        "wrong-failed-endpoint",
        "wrong-retry-endpoint",
        "same-endpoint",
        "wrong-normalized-error",
        "unsupported-error",
        "profile-commit-present",
        "source-result-present",
        "compile-identity-present",
        "bad-line-order",
        "duplicate-observation",
    ),
)
def test_s70_b4_uncommitted_transport_retry_binding_fails_closed(
    mutation: str,
) -> None:
    fixture = _s70_b4_worker_uncommitted_transport_recovery_bundle()
    evidence = fixture["observations"]["failed_p50_uncommitted_transports"]
    record = evidence["records"][0]
    if mutation == "missing-observation":
        evidence["record_count"] = 0
        evidence["records"] = []
    elif mutation == "wrong-first-job":
        record["scheduler_job"] = 799
    elif mutation == "wrong-retry-job":
        record["retry_scheduler_job"] = 799
    elif mutation == "wrong-worker":
        record["worker"] = "F2"
    elif mutation == "wrong-failed-endpoint":
        record["failed_endpoint"] = "10.0.27.99:23999"
    elif mutation == "wrong-retry-endpoint":
        record["retry_endpoint"] = "10.0.27.98:23998"
    elif mutation == "same-endpoint":
        record["retry_endpoint"] = record["failed_endpoint"]
    elif mutation == "wrong-normalized-error":
        record["normalized_error"] = 2
    elif mutation == "unsupported-error":
        record["original_error"] = 3
    elif mutation == "profile-commit-present":
        record["profile_commit_present"] = True
    elif mutation == "source-result-present":
        record["source_result_present"] = True
    elif mutation == "compile-identity-present":
        record["compile_identity_present"] = True
    elif mutation == "bad-line-order":
        record["retry_line"] = record["normalized_line"]
    else:
        evidence["records"].append(copy.deepcopy(record))
        evidence["record_count"] = 2

    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "FAIL"
    failed = {item["id"] for item in verdict["clauses"] if item["status"] == "FAIL"}
    assert failed & {
        "engagement.expected",
        "retry.strict-p50-bindings",
        "s70.b4-worker-bounces",
    }


def test_source_transfer_marker_keeps_stronger_process_loss_reason() -> None:
    fixture = _s70_b4_worker_recovery_bundle()
    source_fixture = _s70_b4_worker_source_transfer_recovery_bundle()
    fixture["observations"]["failed_p50_result_identities"] = {
        "record_count": 0,
        "records": [],
    }
    fixture["observations"]["failed_p50_source_transfers"] = copy.deepcopy(
        source_fixture["observations"]["failed_p50_source_transfers"]
    )

    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "PASS", verdict


@pytest.mark.parametrize(
    "mutation",
    (
        "missing-observation",
        "wrong-first-job",
        "wrong-retry-job",
        "wrong-worker",
        "wrong-failed-endpoint",
        "wrong-retry-endpoint",
        "same-endpoint",
        "wrong-profile",
        "zero-status",
        "source-result-present",
        "bad-line-order",
        "completion-terminal",
        "duplicate-observation",
    ),
)
def test_s70_b4_source_transfer_retry_binding_fails_closed(mutation: str) -> None:
    fixture = _s70_b4_worker_source_transfer_recovery_bundle()
    source = fixture["observations"]["failed_p50_source_transfers"]
    record = source["records"][0]
    if mutation == "missing-observation":
        source["record_count"] = 0
        source["records"] = []
    elif mutation == "wrong-first-job":
        record["scheduler_job"] = 799
    elif mutation == "wrong-retry-job":
        record["retry_scheduler_job"] = 799
    elif mutation == "wrong-worker":
        record["worker"] = "F2"
    elif mutation == "wrong-retry-c-guid":
        record["retry_c_guid"] += 1
    elif mutation == "wrong-failed-endpoint":
        record["failed_endpoint"] = "10.0.27.99:23999"
    elif mutation == "wrong-retry-endpoint":
        record["retry_endpoint"] = "10.0.27.98:23998"
    elif mutation == "same-endpoint":
        record["retry_endpoint"] = record["failed_endpoint"]
    elif mutation == "wrong-profile":
        record["profile"] = "ZSTD_TU"
    elif mutation == "zero-status":
        record["status"] = 0
    elif mutation == "source-result-present":
        record["source_result_present"] = True
    elif mutation == "bad-line-order":
        record["retry_line"] = record["failure_line"]
    elif mutation == "completion-terminal":
        fixture["observations"]["assignment_lifecycle"][100]["attempts"][0][
            "terminal"
        ] = "completion"
        fixture["observations"]["successful_strict_p50_retry_bindings"][0][
            "first_terminal"
        ] = "completion"
    else:
        source["records"].append(copy.deepcopy(record))
        source["record_count"] = 2

    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "FAIL"
    failed = {item["id"] for item in verdict["clauses"] if item["status"] == "FAIL"}
    assert failed & {
        "engagement.expected",
        "retry.strict-p50-bindings",
        "s70.b4-worker-bounces",
    }


@pytest.mark.parametrize(
    "mutation",
    (
        "missing-binding",
        "wrong-first-job",
        "wrong-final-job",
        "wrong-first-generation",
        "wrong-final-generation",
        "wrong-first-worker",
        "wrong-final-worker",
        "wrong-row",
        "missing-error106",
        "unsupported-first-terminal",
        "invalid-assignment-epoch",
        "invalid-assignment-nonce",
        "missing-failed-identity",
        "outside-lifecycle-deadline",
    ),
)
def test_s70_b4_strict_retry_binding_fails_closed(mutation: str) -> None:
    fixture = _s70_b4_worker_result_stream_recovery_bundle()
    bindings = fixture["observations"][
        "successful_strict_p50_retry_bindings"
    ]
    if mutation == "missing-binding":
        bindings.clear()
    elif mutation == "wrong-first-job":
        bindings[0]["first_scheduler_job"] += 1
    elif mutation == "wrong-final-job":
        bindings[0]["final_scheduler_job"] += 1
    elif mutation == "wrong-first-generation":
        bindings[0]["first_generation"] += 1
    elif mutation == "wrong-final-generation":
        bindings[0]["final_generation"] += 1
    elif mutation == "wrong-first-worker":
        bindings[0]["first_worker"] = "F2"
    elif mutation == "wrong-final-worker":
        bindings[0]["final_worker"] = "F1"
    elif mutation == "wrong-row":
        bindings[0]["job_id"] = "missing"
    elif mutation == "missing-error106":
        fixture["observations"]["error106_job_ids"] = []
    elif mutation == "unsupported-first-terminal":
        bindings[0]["first_terminal"] = "process-loss-recovery"
    elif mutation == "invalid-assignment-epoch":
        fixture["observations"]["failed_p50_result_identities"]["records"][0][
            "assignment_epoch"
        ] = 0
    elif mutation == "invalid-assignment-nonce":
        fixture["observations"]["failed_p50_result_identities"]["records"][0][
            "assignment_nonce"
        ] = 0
    elif mutation == "missing-failed-identity":
        fixture["observations"]["failed_p50_result_identities"] = {
            "record_count": 0,
            "records": [],
        }
    else:
        lifecycle = fixture["observations"]["job_lifecycle"][100]
        bindings[0]["first_terminal_ms"] = lifecycle["deadline_ms"] + 1

    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "FAIL"
    failed = {item["id"] for item in verdict["clauses"] if item["status"] == "FAIL"}
    assert failed & {
        "engagement.expected",
        "error106.max",
        "retry.strict-p50-bindings",
        "s70.b4-worker-bounces",
    }


@pytest.mark.parametrize(
    "mutation", ("missing", "wrong-worker", "wrong-job", "unwitnessed-scheduler-job")
)
def test_s70_b4_worker_recovery_binding_fails_closed(mutation: str) -> None:
    fixture = _s70_b4_worker_recovery_bundle()
    bindings = fixture["observations"]["process_loss_recovery_bindings"]
    if mutation == "missing":
        bindings.clear()
    elif mutation == "wrong-worker":
        bindings[0]["worker"] = "F2"
    elif mutation == "wrong-job":
        bindings[0]["job_id"] = "102"
    else:
        bindings[0]["scheduler_job"] = 702

    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "FAIL"
    failed = {item["id"] for item in verdict["clauses"] if item["status"] == "FAIL"}
    assert "s70.b4-worker-bounces" in failed


@pytest.mark.parametrize("final_dispatch_ms", (9_899, 10_076))
def test_lifecycle_final_dispatch_must_be_between_first_dispatch_and_terminal(
    final_dispatch_ms: int,
) -> None:
    fixture = _s70_b4_worker_recovery_bundle()
    fixture["observations"]["job_lifecycle"][100][
        "final_dispatch_ms"
    ] = final_dispatch_ms

    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "FAIL"
    failed = {item["id"] for item in verdict["clauses"] if item["status"] == "FAIL"}
    assert "job-lifecycle.schema" in failed


@pytest.mark.parametrize(
    "mutation",
    (
        "missing_event",
        "stale_restart",
        "broken_restart_chain",
        "other_worker_stops",
        "next_tu_warm",
        "no_warm_precondition",
        "dispatch_out_of_epoch",
        "bad_rejoin",
    ),
)
def test_s70_b4_worker_bounce_evidence_fails_closed(mutation: str) -> None:
    fixture = _s70_b4_worker_bundle()
    if mutation == "missing_event":
        fixture["event_log"] = fixture["event_log"][:2]
    elif mutation == "stale_restart":
        fixture["event_log"][0]["receipt"]["after"]["started_at"] = (
            fixture["event_log"][0]["receipt"]["before"]["started_at"]
        )
    elif mutation == "broken_restart_chain":
        fixture["event_log"][1]["receipt"]["before"]["started_at"] = (
            "2026-09-06T00:00:30Z"
        )
    elif mutation == "other_worker_stops":
        for row in fixture["rows"]:
            if row["event_epoch"] == 2 and row["cs"] == "F2":
                row["cs"] = "F1"
    elif mutation == "next_tu_warm":
        first = next(
            row
            for row in fixture["rows"]
            if row["event_epoch"] == 1 and row["cs"] == "F1"
        )
        first["c_to_f_bytes"] = 101
    elif mutation == "no_later_warm_witness":
        for row in fixture["rows"]:
            if row["event_epoch"] > 0 and row["cs"] == "F1":
                tu_index = int(
                    str(row["tu"]).removesuffix(".ii").rsplit("-", 1)[1]
                )
                row["c_to_f_bytes"] = 1000 + tu_index
    elif mutation == "dispatch_out_of_epoch":
        fixture["observations"]["job_lifecycle"][100]["dispatch_ms"] = 9999
    else:
        fixture["event_log"][1]["receipt"]["coordination"]["scheduler_rejoin"][
            "cache_line"
        ] = "RELOGIN F1(10.0.0.1): cache=off"
    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "FAIL"
    failed = {item["id"] for item in verdict["clauses"] if item["status"] == "FAIL"}
    assert failed & {"engagement.expected", "s70.b4-worker-bounces"}


def _fixture_sha(value: str) -> str:
    return hashlib.sha256(value.encode()).hexdigest()


def _b7_gate(epoch: int, fired_ms: int, *, quiesce: bool) -> dict[str, object]:
    return {
        "action": "quiesce" if quiesce else "pause",
        "active_after": 0,
        "active_before": 1,
        "client": "C1",
        "epoch": epoch,
        "finished_ms": fired_ms - 10,
        "schema": "icefarm-event-gate-v1",
        "started_ms": fired_ms - 20,
        "status": "QUIESCED" if quiesce else "PAUSED",
        "turn": "A",
    }


def _b7_transition_event(
    scenario: dict[str, object],
    *,
    index: int,
    trigger: int,
    role: str,
    action: str,
    before_version: int,
    after_version: int,
    fired_ms: int,
) -> dict[str, object]:
    epoch = index + 1
    name = {"S": "S1", "F": "F1", "C": "C1"}[role]
    before_alias = "new" if before_version == 50 else "old"
    after_alias = "new" if after_version == 50 else "old"
    before_label = scenario["images"][before_alias]
    after_label = scenario["images"][after_alias]
    before_id = _fixture_sha(f"b7-{action}-{role}-before")
    after_id = _fixture_sha(f"b7-{action}-{role}-after")
    before_closure = _fixture_sha(f"b7-{before_version}-closure")
    after_closure = _fixture_sha(f"b7-{after_version}-closure")
    before_role = _fixture_sha(f"b7-{role}-{before_version}-role")
    after_role = _fixture_sha(f"b7-{role}-{after_version}-role")
    event: dict[str, object] = {
        "action": action,
        "event_epoch": epoch,
        "event_index": index,
        "fired_ms": fired_ms,
        "instance": name,
        "last_dispatched_job": trigger,
        "trigger": f"job {trigger}",
        "workload_dispatch_count": trigger,
    }
    if role == "C":
        pause = _b7_gate(epoch, fired_ms, quiesce=True)
        resume = dict(
            pause,
            action="resume",
            active_before=0,
            finished_ms=fired_ms + 10,
            started_ms=fired_ms,
            status="OPEN",
        )
        completed_rows = [
            {"index": 1, "path": "jobs/000001/result.tsv", "sha256": SHA_A}
        ]
        checkpoint = {
            "checkpoint_sha256": "",
            "client": "C1",
            "completed_rows": completed_rows,
            "completed_rows_sha256": hashlib.sha256(
                json.dumps(
                    completed_rows, sort_keys=True, separators=(",", ":")
                ).encode()
            ).hexdigest(),
            "expected_jobs": 200,
            "schema": "icefarm-workload-checkpoint-v1",
            "status": "QUIESCED",
            "turn": "A",
            "worklist_sha256": SHA_B,
        }
        checkpoint_body = dict(checkpoint)
        checkpoint_body.pop("checkpoint_sha256")
        checkpoint["checkpoint_sha256"] = hashlib.sha256(
            json.dumps(
                checkpoint_body, sort_keys=True, separators=(",", ":")
            ).encode()
        ).hexdigest()
        readiness = {
            "host": "h1",
            "line": "ICECREAM daemon 1.5.90 starting up",
            "log_path": "/run/C1/log/client-daemon.log",
            "offset": epoch * 10,
            "role": "C",
        }
        cache_required = after_version == 50
        event["receipt"] = {
            "action": action,
            "after": {
                "container_id": after_id,
                "closure_sha256": after_closure,
                "env": {"ICECC_P50_MODE": "on"} if cache_required else {},
                "image": after_label,
                "role_sha256": after_role,
            },
            "before": {
                "container_id": before_id,
                "closure_sha256": before_closure,
                "env": {"ICECC_P50_MODE": "on"}
                if before_version == 50
                else {},
                "image": before_label,
                "role_sha256": before_role,
            },
            "checkpoints": {"C1": checkpoint},
            "client_readiness": {
                "cache_line": (
                    "cache sidecar adapter state=2 lifecycle=3"
                    if cache_required
                    else None
                ),
                "cache_required": cache_required,
                "connected_line": (
                    "Connected to scheduler (I am known as 10.0.0.1)"
                ),
                "host": "h1",
                "log_path": readiness["log_path"],
                "offset": epoch * 10,
            },
            "coordination": {
                "clients": {"C1": pause},
                "ready_ms": fired_ms,
                "relaunch": {
                    "C1": {
                        "client": "C1",
                        "expected_jobs": 200,
                        "failures": 0,
                        "jobs": 200,
                        "status": "COMPLETE",
                    }
                },
                "resume": {"C1": resume},
            },
            "event_epoch": epoch,
            "instance": "C1",
            "preflight": {
                "image_closure_sha256": after_closure,
                "role_sha256": after_role,
                "runtime_path": f"/runtimes/{after_closure}/root",
            },
            "readiness": readiness,
            "schema": "icefarm-client-transition-v1",
            "turn": "A",
        }
        return event

    pause = _b7_gate(epoch, fired_ms, quiesce=False)
    resume = dict(
        pause,
        action="resume",
        active_before=0,
        finished_ms=fired_ms + 10,
        started_ms=fired_ms,
        status="OPEN",
    )
    role_env = (
        {"ICECC_P50_PROFILE": "P29V1"}
        if role == "S" and after_version == 50
        else {}
    )
    before_env = (
        {"ICECC_P50_PROFILE": "P29V1"}
        if role == "S" and before_version == 50
        else {}
    )
    readiness = {
        "host": "h1",
        "line": (
            "ICECREAM scheduler 1.5.90 starting up, port 8765"
            if role == "S"
            else "ICECREAM daemon F1 starting up"
        ),
        "log_path": f"/run/{name}/log/"
        + ("scheduler.log" if role == "S" else "iceccd.log"),
        "offset": epoch * 10,
        "role": role,
    }
    coordination: dict[str, object] = {
        "clients": {"C1": pause},
        "ready_ms": fired_ms,
        "resume": {"C1": resume},
        "scheduler_snapshot": "scheduler ready\n",
        "worker_snapshot": "F1\n",
        "workers": ["F1"],
    }
    if role == "S":
        initial_client = next(
            item for item in scenario["instances"] if item["name"] == "C1"
        )
        client_label = scenario["images"][initial_client["image"]]
        cache_required = (
            after_version == 50
            and client_label.startswith("p50")
            and initial_client.get("env", {}).get("ICECC_P50_MODE") == "on"
        )
        coordination.update(
            client_readiness={
                "C1": {
                    "cache_line": (
                        "cache sidecar adapter state=2 lifecycle=3"
                        if cache_required
                        else None
                    ),
                    "cache_required": cache_required,
                    "connected_line": (
                        "Connected to scheduler (I am known as 10.0.0.1)"
                    ),
                    "host": "h1",
                    "log_path": "/run/C1/log/client-daemon.log",
                    "offset": epoch * 10,
                }
            },
            scheduler_startup=readiness,
        )
    else:
        coordination["scheduler_worker_rejoin"] = {
            "bytes": 40,
            "host": "h1",
            "line": f"login F1 protocol version: {after_version}",
            "log_path": "/run/S1/log/scheduler.log",
            "offset": epoch * 10,
            "role_protocol": after_version,
            "target": "F1",
        }
    event["receipt"] = {
        "action": action,
        "after": {
            "container_id": after_id,
            "closure_sha256": after_closure,
            "env": role_env,
            "image": after_label,
            "role_sha256": after_role,
        },
        "before": {
            "container_id": before_id,
            "closure_sha256": before_closure,
            "env": before_env,
            "image": before_label,
            "role_sha256": before_role,
        },
        "coordination": coordination,
        "event_epoch": epoch,
        "instance": name,
        "preflight": {
            "image_closure_sha256": after_closure,
            "role_sha256": after_role,
            "runtime_path": f"/runtimes/{after_closure}/root",
        },
        "readiness": readiness,
        "schema": "icefarm-transition-v2",
        "turn": "A",
    }
    return event


def _s70_b7_bundle(*, rollback: bool) -> dict[str, object]:
    initial = 50 if rollback else 43
    scenario = _scenario(
        "mixed", client_versions=(initial,), worker_versions=(initial,)
    )
    if not rollback:
        scheduler = next(
            item for item in scenario["instances"] if item["role"] == "S"
        )
        scheduler["image"] = "old"
        scheduler["env"] = {}
    mode = "rollback" if rollback else "rollforward"
    scenario["id"] = f"S70-b7-{mode}"
    scenario["expect"]["engagement"] = f"s70-b7-{mode}"
    scenario["expect"]["reuse"] = "none-when-legacy"
    scenario["workload"].update(
        corpus="fmt-100", jobs=1, repeat=4, turns=["A"]
    )
    action = "downgrade" if rollback else "upgrade"
    target = "old" if rollback else "new"
    triggers = (75, 100, 125) if rollback else (25, 50, 75)
    scenario["timeline"] = [
        {
            "trigger": f"job {trigger}",
            "action": action,
            "instance": name,
            "image": target,
        }
        for trigger, name in zip(triggers, ("S1", "F1", "C1"), strict=True)
    ]
    versions = (
        ((50, 50, True), (50, 50, False), (50, 43, False), (43, 43, False))
        if rollback
        else ((43, 43, False), (43, 43, False), (43, 50, False), (50, 50, True))
    )
    rows: list[dict[str, object]] = []
    dispatches: list[int] = []
    job = 1
    for epoch, (client_version, worker_version, p29) in enumerate(versions):
        count = 4 if (rollback and epoch == 0) else 2
        if not rollback and epoch == 3:
            count = 3
        for position in range(count):
            row = _row(
                job,
                client_version=client_version,
                worker_version=worker_version,
                tail=p29,
            )
            row["event_epoch"] = epoch
            row["tu"] = f"files/tu-{position % 2 + 1}.ii"
            if p29:
                row["c_to_f_bytes"] = (
                    1000 if position < 2 else 100
                ) + position % 2 + 1
                row["f_to_c_bytes"] = 500 + position % 2 + 1
            rows.append(row)
            dispatches.append(epoch * 1000 + position * 100)
            job += 1
    observations = _observations(rows)
    for dispatch_line, (lifecycle, dispatch_ms, row) in enumerate(
        zip(observations["job_lifecycle"], dispatches, rows, strict=True),
        start=10,
    ):
        lifecycle["dispatch_ms"] = dispatch_ms
        lifecycle["scheduler_dispatch_line"] = dispatch_line
        lifecycle["scheduler_generation"] = (
            1 if row["event_epoch"] == 0 else 2
        )
        lifecycle["terminal_ms"] = dispatch_ms + 25
        lifecycle["deadline_ms"] = dispatch_ms + 120_000
    before_version, after_version = ((50, 43) if rollback else (43, 50))
    bundle = _bundle(scenario, rows, observations)
    bundle["event_log"] = [
        _b7_transition_event(
            scenario,
            index=index,
            trigger=trigger,
            role=role,
            action=action,
            before_version=before_version,
            after_version=after_version,
            fired_ms=(index + 1) * 1000,
        )
        for index, (trigger, role) in enumerate(
            zip(triggers, ("S", "F", "C"), strict=True)
        )
    ]
    return bundle


@pytest.mark.parametrize(
    ("action", "before_version", "after_version", "cache_required"),
    (
        ("downgrade", 50, 43, False),
        ("upgrade", 43, 50, True),
    ),
)
def test_scheduler_transition_verdict_binds_cache_to_scheduler_generation(
    action: str,
    before_version: int,
    after_version: int,
    cache_required: bool,
) -> None:
    scenario = _scenario("mixed", client_versions=(50,), worker_versions=(50,))
    scheduler = next(
        item for item in scenario["instances"] if item["role"] == "S"
    )
    scheduler["image"] = "new" if before_version == 50 else "old"
    scheduler["env"] = (
        {"ICECC_P50_PROFILE": "P29V1"} if before_version == 50 else {}
    )
    target_alias = "new" if after_version == 50 else "old"
    scenario["timeline"] = [
        {
            "trigger": "job 24",
            "action": action,
            "instance": "S1",
            "image": target_alias,
        }
    ]
    observed = _b7_transition_event(
        scenario,
        index=0,
        trigger=24,
        role="S",
        action=action,
        before_version=before_version,
        after_version=after_version,
        fired_ms=1000,
    )
    witness = observed["receipt"]["coordination"]["client_readiness"]["C1"]
    assert witness["cache_required"] is cache_required
    assert not _transition_receipt_errors(
        observed, scenario["timeline"][0], scenario
    )

    tampered = copy.deepcopy(observed)
    bad = tampered["receipt"]["coordination"]["client_readiness"]["C1"]
    if cache_required:
        bad["cache_required"] = False
        bad["cache_line"] = None
    else:
        bad["cache_line"] = "cache sidecar adapter state=2 lifecycle=3"
    assert _transition_receipt_errors(
        tampered, scenario["timeline"][0], scenario
    ) == {"@event:transition-coordination"}


@pytest.mark.parametrize("rollback", (True, False))
def test_s70_b7_rollback_and_rollforward_are_exact_and_cold(
    rollback: bool,
) -> None:
    fixture = _s70_b7_bundle(rollback=rollback)
    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "PASS", verdict


@pytest.mark.parametrize("rollback", (True, False))
@pytest.mark.parametrize(
    "mutation",
    (
        "missing_event",
        "wrong_epoch_law",
        "no_inflight",
        "wrong_image",
        "dispatch_out_of_epoch",
        "no_warm_or_cold",
        "parallel_workload",
    ),
)
def test_s70_b7_evidence_fails_closed(rollback: bool, mutation: str) -> None:
    fixture = _s70_b7_bundle(rollback=rollback)
    if mutation == "missing_event":
        fixture["event_log"] = fixture["event_log"][:2]
    elif mutation == "wrong_epoch_law":
        row = next(row for row in fixture["rows"] if row["event_epoch"] == 2)
        row["cs_version"] = 50 if rollback else 43
    elif mutation == "no_inflight":
        fixture["event_log"][0]["receipt"]["coordination"]["clients"]["C1"][
            "active_before"
        ] = 0
    elif mutation == "wrong_image":
        fixture["event_log"][1]["receipt"]["after"]["image"] = fixture[
            "event_log"
        ][1]["receipt"]["before"]["image"]
    elif mutation == "dispatch_out_of_epoch":
        fixture["observations"]["job_lifecycle"][-1]["dispatch_ms"] = 2500
    elif mutation == "no_warm_or_cold":
        p29_rows = [row for row in fixture["rows"] if row["tail_present"]]
        for row in p29_rows:
            row["c_to_f_bytes"] = 1001
    else:
        fixture["scenario"]["workload"]["jobs"] = 2
    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "FAIL"
    failed = {item["id"] for item in verdict["clauses"] if item["status"] == "FAIL"}
    expected = "s70.b7-rollback" if rollback else "s70.b7-rollforward"
    assert failed & {"engagement.expected", expected}


def _s70_b6_bundle() -> dict[str, object]:
    scenario = _scenario("S'C'F'")
    scenario["id"] = "S70-b6-kill-switch"
    scenario["expect"]["engagement"] = "s70-b6-drained-kill-switch-cycle"
    scenario["expect"]["reuse"] = "none-when-legacy"
    scenario["workload"]["jobs"] = 1
    scenario["timeline"] = [
        {
            "trigger": "job 1",
            "action": "env_set",
            "instance": "S1",
            "env": {"ICECC_P50_PROFILE": "OFF"},
        },
        {
            "trigger": "job 3",
            "action": "env_set",
            "instance": "S1",
            "env": {"ICECC_P50_PROFILE": "P29V1"},
        },
    ]
    rows = [
        _row(1),
        _row(2, tail=False, outcome="none"),
        _row(3, tail=False, outcome="none"),
        _row(4),
    ]
    rows[1]["event_epoch"] = 1
    rows[2]["event_epoch"] = 1
    rows[3]["event_epoch"] = 2
    observations = _observations(rows)
    for dispatch_line, (lifecycle, row) in enumerate(
        zip(observations["job_lifecycle"], rows, strict=True), start=10
    ):
        lifecycle["scheduler_dispatch_line"] = dispatch_line
        lifecycle["scheduler_generation"] = row["event_epoch"] + 1
    bundle = _bundle(scenario, rows, observations)
    bundle["event_log"] = [
        _scheduler_env_event(
            index=0,
            trigger_job=1,
            before_id=SHA_A,
            after_id=SHA_B,
            before_profile="P29V1",
            after_profile="OFF",
            fired_ms=50,
        ),
        _scheduler_env_event(
            index=1,
            trigger_job=3,
            before_id=SHA_B,
            after_id=SHA_C,
            before_profile="OFF",
            after_profile="P29V1",
            fired_ms=250,
        ),
    ]
    return bundle


def test_s70_b6_scheduler_kill_switch_suppresses_then_restores_tails() -> None:
    fixture = _s70_b6_bundle()
    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "PASS", verdict


@pytest.mark.parametrize("remote_clock_base", (10, 10_000))
def test_s70_b6_gate_clock_skew_is_diagnostic_only(
    remote_clock_base: int,
) -> None:
    fixture = _s70_b6_bundle()
    for index, event in enumerate(fixture["event_log"]):
        pause = event["receipt"]["coordination"]["clients"]["C1"]
        resume = event["receipt"]["coordination"]["resume"]["C1"]
        pause["started_ms"] = remote_clock_base + index * 100
        pause["finished_ms"] = remote_clock_base + index * 100 + 10
        resume["started_ms"] = remote_clock_base + index * 100 + 20
        resume["finished_ms"] = remote_clock_base + index * 100 + 30

    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "PASS", verdict


@pytest.mark.parametrize(
    "mutation",
    (
        "tail_while_off",
        "no_tail_after_restore",
        "error106",
        "wrong_restore",
        "broken_state_chain",
        "not_drained",
        "no_post_off_dispatch",
        "dispatch_out_of_epoch",
        "parallel_workload",
        "pause_not_active",
    ),
)
def test_s70_b6_cycle_and_epoch_evidence_fail_closed(mutation: str) -> None:
    fixture = _s70_b6_bundle()
    if mutation == "tail_while_off":
        fixture["rows"][1] = _row(2)
        fixture["rows"][1]["event_epoch"] = 1
    elif mutation == "no_tail_after_restore":
        fixture["rows"][3] = _row(4, tail=False, outcome="none")
        fixture["rows"][3]["event_epoch"] = 2
    elif mutation == "error106":
        fixture["observations"]["error106_job_ids"] = ["2"]
    elif mutation == "wrong_restore":
        fixture["scenario"]["timeline"][1]["env"]["ICECC_P50_PROFILE"] = "ZSTD_TU"
    elif mutation == "broken_state_chain":
        fixture["event_log"][1]["receipt"]["before"]["container_id"] = SHA_C
    elif mutation == "not_drained":
        fixture["event_log"][0]["receipt"]["coordination"]["clients"]["C1"][
            "active_after"
        ] = 1
    elif mutation == "no_post_off_dispatch":
        fixture["event_log"][1]["workload_dispatch_count"] = fixture["event_log"][
            0
        ]["workload_dispatch_count"]
    elif mutation == "dispatch_out_of_epoch":
        fixture["observations"]["job_lifecycle"][1]["scheduler_generation"] = 1
    elif mutation == "parallel_workload":
        fixture["scenario"]["workload"]["jobs"] = 2
    else:
        fixture["event_log"][0]["receipt"]["coordination"]["clients"]["C1"][
            "active_before"
        ] = 0
    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "FAIL"
    failed = {item["id"] for item in verdict["clauses"] if item["status"] == "FAIL"}
    assert failed & {"engagement.expected", "s70.b6-drained-kill-switch-cycle"}


def _shape_fixtures() -> dict[str, dict[str, object]]:
    scf_rows = [_row(1, client_version=43, worker_version=43)]
    scheduler_first_rows = [_row(1, client_version=43, worker_version=43)]
    old_worker_rows = [_row(1, client_version=50, worker_version=43)]
    full_rows = [_row(1)]
    mixed_rows = [
        _row(1, client="C1", client_version=50, worker="F1", worker_version=50),
        _row(2, client="C1", client_version=50, worker="F2", worker_version=43),
        _row(3, client="C2", client_version=43, worker="F1", worker_version=50),
        _row(4, client="C2", client_version=43, worker="F2", worker_version=43),
    ]
    skew_rows = [
        _row(1, worker="F1"),
        _row(2, worker="F2", tail=False, outcome="none"),
    ]
    skew_observations = _observations(
        skew_rows,
        revisions={"C1": 1, "F1": 1, "F2": 2},
    )
    for login in skew_observations["logins"]:
        login["cache_protocol"] = 1 if login["instance"] == "F1" else 2
    skew_observations["assignment_preference"] = {
        "compatibility": {"C1": ["F1"]},
        "counts": {"checks": 2, "escapes": 1, "preferred": 1, "violations": 0},
        "decisions": [
            {
                "client": "C1",
                "compatible_free_workers": ["F1"],
                "compatible_workers": ["F1"],
                "dispatch_line": 10,
                "escape": False,
                "occupancy": {"F1": 0, "F2": 0},
                "preferred": True,
                "row_job_id": "1",
                "scheduler_job": 1,
                "worker": "F1",
            },
            {
                "client": "C1",
                "compatible_free_workers": [],
                "compatible_workers": ["F1"],
                "dispatch_line": 20,
                "escape": True,
                "occupancy": {"F1": 1, "F2": 0},
                "preferred": False,
                "row_job_id": "2",
                "scheduler_job": 2,
                "worker": "F2",
            },
        ],
        "schema": "icefarm-assignment-preference-v1",
        "workers": {"F1": 1, "F2": 1},
        "violations": [],
    }
    mixed_bundle = _bundle(
        _scenario(
            "S'[FF'][CC']",
            client_versions=(50, 43),
            worker_versions=(50, 43),
        ),
        mixed_rows,
        _observations(mixed_rows, old_workers={"F2"}),
    )
    mixed_bundle["observations"]["assignment_preference"] = {
        "compatibility": {"C1": ["F1"], "C2": []},
        "counts": {"checks": 4, "escapes": 3, "preferred": 1, "violations": 0},
        "decisions": [
            {
                "client": "C1",
                "compatible_free_workers": ["F1"],
                "compatible_workers": ["F1"],
                "dispatch_line": 10,
                "escape": False,
                "occupancy": {"F1": 0, "F2": 0},
                "preferred": True,
                "row_job_id": "1",
                "scheduler_job": 1,
                "worker": "F1",
            },
            {
                "client": "C1",
                "compatible_free_workers": [],
                "compatible_workers": ["F1"],
                "dispatch_line": 20,
                "escape": True,
                "occupancy": {"F1": 1, "F2": 0},
                "preferred": False,
                "row_job_id": "2",
                "scheduler_job": 2,
                "worker": "F2",
            },
            {
                "client": "C2",
                "compatible_free_workers": [],
                "compatible_workers": [],
                "dispatch_line": 30,
                "escape": True,
                "occupancy": {"F1": 0, "F2": 0},
                "preferred": False,
                "row_job_id": "3",
                "scheduler_job": 3,
                "worker": "F1",
            },
            {
                "client": "C2",
                "compatible_free_workers": [],
                "compatible_workers": [],
                "dispatch_line": 40,
                "escape": True,
                "occupancy": {"F1": 0, "F2": 0},
                "preferred": False,
                "row_job_id": "4",
                "scheduler_job": 4,
                "worker": "F2",
            },
        ],
        "schema": "icefarm-assignment-preference-v1",
        "workers": {"F1": 1, "F2": 1},
        "violations": [],
    }
    skew_scenario = _scenario(
        "S'[F'F''][C']",
        client_versions=(50,),
        worker_versions=(50, 50),
    )
    skew_scenario["workload"].update(
        {
            "corpus": "fmt-100",
            "driver": "tu-manifest",
            "jobs": 2,
            "repeat": 1,
        }
    )
    skew_bundle = _bundle(skew_scenario, skew_rows, skew_observations)
    skew_bundle["topology"] = {
        "instances": [
            {"cache_wire_revision": 1, "name": "S1", "role": "S"},
            {"cache_wire_revision": 1, "name": "C1", "role": "C"},
            {"cache_wire_revision": 1, "host": "h1", "name": "F1", "role": "F"},
            {"cache_wire_revision": 2, "host": "h1", "name": "F2", "role": "F"},
        ],
        "relationships": [
            {"c": "C1", "cache_expected": True, "f": "F1"},
            {"c": "C1", "cache_expected": False, "f": "F2"},
        ],
    }
    skew_bundle["observations"]["f_init"] = {
        "instances": [
            {
                "host": "h1",
                "init": True,
                "inspect_sha256": "a" * 64,
                "instance": "F1",
            },
            {
                "host": "h1",
                "init": True,
                "inspect_sha256": "b" * 64,
                "instance": "F2",
            },
        ],
        "schema": "icefarm-f-init-v1",
    }
    return {
        "SCF": _bundle(
            _scenario("SCF", client_versions=(43,), worker_versions=(43,)),
            scf_rows,
            _observations(scf_rows, old_workers={"F1"}),
        ),
        "S'CF": _bundle(
            _scenario("S'CF", client_versions=(43,), worker_versions=(43,)),
            scheduler_first_rows,
            _observations(scheduler_first_rows, old_workers={"F1"}),
        ),
        "S'FC'": _bundle(
            _scenario("S'FC'", client_versions=(50,), worker_versions=(43,)),
            old_worker_rows,
            _observations(old_worker_rows, old_workers={"F1"}),
        ),
        "S'C'F'": _bundle(
            _scenario("S'C'F'"),
            full_rows,
            _observations(full_rows),
        ),
        "S'[FF'][CC']": mixed_bundle,
        "S'[F'F''][C']": skew_bundle,
    }


SHAPE_FIXTURES = _shape_fixtures()


def _s70_active_loss_bundle() -> dict[str, object]:
    """Small authenticated bundle exercising the active-loss row law."""
    bundle = copy.deepcopy(SHAPE_FIXTURES["S'C'F'"])
    scenario = bundle["scenario"]
    scenario["expect"]["engagement"] = "s70-b4-scheduler-active-loss"
    scenario["timeline"] = [{"action": "scheduler-loss-active", "instance": "S1", "trigger": "job 2"}]
    fallback = _row(1, tail=False, profile=None, outcome="fallback") | {"retries": 1, "event_epoch": 0}
    later = _row(2, tail=True, profile="P29V1", outcome="committed") | {"event_epoch": 1}
    bundle["rows"] = [fallback, later]
    observations = bundle["observations"]
    observations["assignment_lifecycle"] = [
        {"job_id": "1", "attempts": [
            {"generation": 1, "scheduler_job": 2, "terminal": "scheduler-loss", "worker": "F1"},
            {"generation": 2, "scheduler_job": 3, "terminal": "completion", "worker": "F1"},
        ]},
        {"job_id": "2", "attempts": [
            # Scheduler job numbers restart with the new incarnation.  Reusing
            # the lost numeric id must not make this later row look affected.
            {"generation": 2, "scheduler_job": 2, "terminal": "completion", "worker": "F1"},
        ]},
    ]
    observations["job_lifecycle"] = [
        {"deadline_ms": 10000, "dispatch_ms": 100, "job_id": "1", "terminal": "completion", "terminal_ms": 125, "turn": "A"},
        {"deadline_ms": 10000, "dispatch_ms": 200, "job_id": "2", "terminal": "completion", "terminal_ms": 225, "turn": "A"},
    ]
    observations["local_fallback_job_ids"] = []
    observations["error106_job_ids"] = ["1"]
    observations["failed_p50_result_identities"] = {
        "record_count": 1,
        "records": [
            {
                "assignment_epoch": 7,
                "assignment_nonce": 9,
                "attempt_index": 0,
                "reason": "result-stream-loss",
                "result_identity_present": False,
                "row_job_id": "1",
                "scheduler_job": 2,
                "worker": "F1",
            }
        ],
    }
    receipt = {
        "action": "scheduler-loss-active", "event_epoch": 1, "instance": "S1",
        "lost_scheduler_generation": 1, "lost_scheduler_job": 2,
        "before": {"container_id": "a" * 64, "started_at": "old"},
        "after": {"container_id": "a" * 64, "started_at": "new"},
        "compiler": {"container_id": "b" * 64, "assignment": {"schema": "icefarm-compiler-assignment-v1", "child": {"pid": 41, "pgid": 41, "generation": 1, "kind": 0, "owning_client_id": 7}, "client": {"client_id": 7, "scheduler_job_id": 2, "job_id": 2}, "listener": {"host": "127.0.0.1", "port": 8765}}, "daemon": {"pid": 10, "exe": "/opt/icecream/sbin/iceccd"},
                     "leader": {"pid": 41, "pgid": 41, "ppid": 10, "start_ticks": 9},
                     "stopped": {"pid": 41, "pgid": 41, "ppid": 10, "start_ticks": 9},
                     "group_gone": {"gone": True}, "worker_before": {"container_id": "b" * 64, "started_at": "f"},
                     "worker_after": {"container_id": "b" * 64, "started_at": "f"}},
        "pre_fault": {"scheduler_log": {}, "worker_log": {}},
        "quiescence": {"client_readiness": {"C1": {"bytes": 1, "cache_line": None, "cache_required": False, "connected_line": "Connected to scheduler (I am known as C1)", "host": "h1", "log_path": "/x/C1/log/client-daemon.log", "offset": 0}}, "client_routes": {"C1": {"before": {"container": {"container_id": "c" * 64, "started_at": "same", "running": True}, "daemon": {"argv": ["/opt/icecream/sbin/iceccd"], "exe": "/opt/icecream/sbin/iceccd", "exe_evidence": "proc-exe", "pid": 10, "ppid": 1, "start_ticks": 1, "uid": 0}, "route_owner": {"argv": ["/opt/icecream/sbin/icecc-cache-service"], "exe": "/opt/icecream/sbin/icecc-cache-service", "exe_evidence": "proc-exe", "pid": 11, "ppid": 10, "start_ticks": 2, "uid": 0}}, "after": {"container": {"container_id": "c" * 64, "started_at": "same", "running": True}, "daemon": {"argv": ["/opt/icecream/sbin/iceccd"], "exe": "/opt/icecream/sbin/iceccd", "exe_evidence": "proc-exe", "pid": 10, "ppid": 1, "start_ticks": 1, "uid": 0}, "route_owner": {"argv": ["/opt/icecream/sbin/icecc-cache-service"], "exe": "/opt/icecream/sbin/icecc-cache-service", "exe_evidence": "proc-exe", "pid": 11, "ppid": 10, "start_ticks": 2, "uid": 0}}}},
                        "scheduler_snapshot": "S1", "scheduler_startup": {"line": "ICECREAM scheduler x starting up, port 23000"}, "worker_snapshot": "F1"},
        "schema": "icefarm-scheduler-active-loss-v1", "turn": "A",
    }
    bundle["event_log"] = [{"action": "scheduler-loss-active", "event_epoch": 1, "event_index": 0,
                             "fired_ms": 100, "instance": "S1", "last_dispatched_job": 2,
                             "trigger": "job 2", "workload_dispatch_count": 2, "receipt": receipt}]
    return bundle


def test_s70_active_loss_evaluate_bundle_requires_exact_fallback_and_later_p29() -> None:
    bundle = _s70_active_loss_bundle()
    verdict = evaluate_bundle(bundle)
    assert verdict["status"] == "PASS", verdict
    for mutation in ("wrong_job", "wrong_generation", "zero_fallback", "two_fallback", "wrong_row", "no_later_p29", "local_fallback"):
        tampered = copy.deepcopy(bundle)
        if mutation == "wrong_job":
            tampered["event_log"][0]["receipt"]["lost_scheduler_job"] = 99
        elif mutation == "wrong_generation":
            tampered["event_log"][0]["receipt"]["lost_scheduler_generation"] = 9
        elif mutation == "zero_fallback":
            tampered["rows"][0]["session_outcome"] = "committed"
        elif mutation == "two_fallback":
            tampered["rows"].append(copy.deepcopy(tampered["rows"][0]) | {"job_id": "3"})
        elif mutation == "wrong_row":
            tampered["rows"][0]["job_id"] = "99"
        elif mutation == "no_later_p29":
            tampered["rows"][1]["tail_present"] = False
            tampered["rows"][1]["tail_profile"] = None
            tampered["rows"][1]["session_outcome"] = "fallback"
        else:
            tampered["observations"]["local_fallback_job_ids"] = ["1"]
        assert evaluate_bundle(tampered)["status"] == "FAIL"


def test_active_loss_fallback_recovery_requires_exact_failed_result_binding() -> None:
    bundle = _s70_active_loss_bundle()
    authenticated, bad = _authenticated_active_loss_fallback_ids(
        bundle, bundle["observations"], bundle["rows"]
    )
    assert authenticated == {"1"}
    assert bad == set()

    bundle["observations"]["failed_p50_result_identities"]["records"][0][
        "scheduler_job"
    ] = 99
    authenticated, bad = _authenticated_active_loss_fallback_ids(
        bundle, bundle["observations"], bundle["rows"]
    )
    assert authenticated == set()
    assert bad


@pytest.mark.parametrize("shape", sorted(SHAPE_FIXTURES))
def test_one_fixture_bundle_per_shape_passes(shape: str) -> None:
    verdict = evaluate_bundle(SHAPE_FIXTURES[shape])
    assert verdict["schema"] == VERDICT_SCHEMA
    assert verdict["status"] == "PASS", verdict
    assert verdict["offending_job_ids"] == []


def test_mixed_assignment_preference_clause_is_recomputed_fail_closed() -> None:
    fixture = copy.deepcopy(SHAPE_FIXTURES["S'[FF'][CC']"])
    clause = next(
        item
        for item in evaluate_bundle(fixture)["clauses"]
        if item["id"] == "shape.compatible-free-preference"
    )
    assert clause["status"] == "PASS"

    violated = copy.deepcopy(fixture)
    violated["observations"]["assignment_preference"]["decisions"][0]["worker"] = "F2"
    assert evaluate_bundle(violated)["status"] == "FAIL"

    consistent_violation = copy.deepcopy(fixture)
    preference = consistent_violation["observations"]["assignment_preference"]
    preference["decisions"][0].update(
        worker="F2", preferred=False, escape=False
    )
    preference["counts"].update(preferred=0, violations=1)
    preference["violations"] = ["1"]
    clause = next(
        item
        for item in evaluate_bundle(consistent_violation)["clauses"]
        if item["id"] == "shape.compatible-free-preference"
    )
    assert clause["status"] == "FAIL"
    assert clause["offending_job_ids"] == ["1"]

    saturated = copy.deepcopy(fixture)
    preference = saturated["observations"]["assignment_preference"]
    preference["decisions"][0]["compatible_free_workers"] = []
    preference["decisions"][0]["occupancy"]["F1"] = 1
    preference["decisions"][0]["escape"] = True
    preference["decisions"][0]["preferred"] = False
    preference["counts"] = {"checks": 4, "escapes": 4, "preferred": 0, "violations": 0}
    assert next(
        item
        for item in evaluate_bundle(saturated)["clauses"]
        if item["id"] == "shape.compatible-free-preference"
    )["status"] == "PASS"

    preloaded = copy.deepcopy(fixture)
    preference = preloaded["observations"]["assignment_preference"]
    preference["decisions"][1]["worker"] = "F1"
    preference["decisions"][2].update(
        client="C1",
        compatible_free_workers=[],
        compatible_workers=["F1"],
        occupancy={"F1": 2, "F2": 0},
        worker="F2",
    )
    assert next(
        item
        for item in evaluate_bundle(preloaded)["clauses"]
        if item["id"] == "shape.compatible-free-preference"
    )["status"] == "PASS"

    malformed = copy.deepcopy(fixture)
    malformed["observations"]["assignment_preference"]["decisions"][0].pop("occupancy")
    assert evaluate_bundle(malformed)["status"] == "FAIL"


@pytest.mark.parametrize(
    "mutation",
    (
        "missing_compatible",
        "missing_incompatible",
        "tail_to_incompatible",
        "retry_incompatible",
        "wrong_login_revision",
        "wrong_topology_revision",
        "missing_preference",
        "missing_escape",
        "named_endpoint_mismatch",
        "error106",
        "local_fallback",
        "unexpected_incompatible_session",
        "wide_worker",
        "serial_workload",
    ),
)
def test_s90_revision_routing_evidence_fails_closed(mutation: str) -> None:
    fixture = copy.deepcopy(SHAPE_FIXTURES["S'[F'F''][C']"])
    if mutation == "missing_compatible":
        fixture["rows"] = fixture["rows"][1:]
    elif mutation == "missing_incompatible":
        fixture["rows"] = fixture["rows"][:1]
    elif mutation == "tail_to_incompatible":
        fixture["rows"][1].update(
            tail_present=True,
            tail_profile="P29V1",
            session_outcome="committed",
            reuse=True,
        )
    elif mutation == "retry_incompatible":
        fixture["rows"][1]["retries"] = 1
    elif mutation == "wrong_login_revision":
        fixture["observations"]["logins"][1]["cache_protocol"] = 1
    elif mutation == "wrong_topology_revision":
        fixture["topology"]["instances"][3]["cache_wire_revision"] = 1
    elif mutation == "missing_preference":
        fixture["observations"]["assignment_preference"]["decisions"][0][
            "preferred"
        ] = False
        fixture["observations"]["assignment_preference"]["counts"]["preferred"] = 0
    elif mutation == "missing_escape":
        fixture["observations"]["assignment_preference"]["decisions"][1][
            "escape"
        ] = False
        fixture["observations"]["assignment_preference"]["counts"]["escapes"] = 0
    elif mutation == "named_endpoint_mismatch":
        fixture["observations"]["wire_revision_mismatches"] = [
            {"error": "WIRE_REVISION_MISMATCH", "job_id": "2"}
        ]
    elif mutation == "error106":
        fixture["observations"]["error106_job_ids"] = ["2"]
    elif mutation == "local_fallback":
        fixture["observations"]["local_fallback_job_ids"] = ["2"]
    elif mutation == "unexpected_incompatible_session":
        fixture["observations"]["sidecars"]["F2"]["sessions"] = 1
    elif mutation == "wide_worker":
        next(
            item
            for item in fixture["scenario"]["instances"]
            if item["name"] == "F1"
        )["slots"] = 2
    else:
        fixture["scenario"]["workload"]["jobs"] = 1

    verdict = evaluate_bundle(fixture)
    clause = next(
        item
        for item in verdict["clauses"]
        if item["id"] == "shape.revision-skew-routing"
    )
    assert verdict["status"] == "FAIL"
    assert clause["status"] == "FAIL"


def test_s90_rejects_a_consistently_reported_extra_preference_violation() -> None:
    fixture = copy.deepcopy(SHAPE_FIXTURES["S'[F'F''][C']"])
    row = _row(3, worker="F2", tail=False, outcome="none")
    fixture["rows"].append(row)
    lifecycle = copy.deepcopy(fixture["observations"]["job_lifecycle"][-1])
    lifecycle.update(job_id="3", dispatch_ms=300, terminal_ms=325)
    fixture["observations"]["job_lifecycle"].append(lifecycle)
    preference = fixture["observations"]["assignment_preference"]
    preference["decisions"].append(
        {
            "client": "C1",
            "compatible_free_workers": ["F1"],
            "compatible_workers": ["F1"],
            "dispatch_line": 30,
            "escape": False,
            "occupancy": {"F1": 0, "F2": 0},
            "preferred": False,
            "row_job_id": "3",
            "scheduler_job": 3,
            "worker": "F2",
        }
    )
    preference["counts"] = {
        "checks": 3,
        "escapes": 1,
        "preferred": 1,
        "violations": 1,
    }
    preference["violations"] = ["3"]

    verdict = evaluate_bundle(fixture)
    clause = next(
        item
        for item in verdict["clauses"]
        if item["id"] == "shape.revision-skew-routing"
    )
    assert verdict["status"] == "FAIL"
    assert clause["status"] == "FAIL"
    assert clause["offending_job_ids"] == ["3"]


def _s90_refusal_bundle() -> dict[str, object]:
    scenario = _scenario("S'[F~][C']")
    scenario["id"] = "S90-revision-refusal-retry"
    scenario["images"]["mutant"] = "p50s90-f-hidden-skew-fixture"
    worker = next(
        item for item in scenario["instances"] if item["name"] == "F1"
    )
    worker["image"] = "mutant"
    worker["env"] = {"ICECC_P50_S90_ENDPOINT_WIRE_REVISION": "2"}
    scenario["workload"].update(
        {
            "clients": ["C1"],
            "corpus": "fmt-100",
            "driver": "tu-manifest",
            "jobs": 1,
            "repeat": 1,
            "turns": ["A"],
        }
    )
    scenario["expect"].update(
        {
            "engagement": "s90-revision-refusal-retry",
            "error106_max": 100,
            "reuse": "none-when-legacy",
        }
    )
    affected = _row(1, tail=False, outcome="fallback")
    affected["retries"] = 1
    direct = _row(2, tail=False, outcome="none")
    rows = [affected, direct]
    observations = _observations(rows, revisions={"C1": 1, "F1": 1})
    observations["logins"][0]["cache_protocol"] = 1
    observations["error106_job_ids"] = ["1"]
    observations["legacy_wire"] = {
        "record_count": 2,
        "records": [
            {
                "c_to_f_bytes": row["c_to_f_bytes"],
                "client_instance": "C1",
                "f_to_c_bytes": row["f_to_c_bytes"],
                "job_id": row["job_id"],
                "turn": "A",
                "worker_instance": "F1",
            }
            for row in rows
        ],
    }
    observations["wire_revision_mismatches"] = [
        {
            "advertised_worker_wire_revision": 1,
            "client_instance": "C1",
            "client_wire_revision": 1,
            "endpoint_wire_revision": 2,
            "error": "WIRE_REVISION_MISMATCH",
            "error_code": 4,
            "first_assignment": {
                "assignment_epoch": 7,
                "assignment_nonce": 11,
                "scheduler_job": 41,
            },
            "retry_assignment": {
                "assignment_epoch": 7,
                "assignment_nonce": 12,
                "scheduler_job": 42,
            },
            "row_job_id": "1",
            "schema": "icefarm-wire-revision-mismatch-v1",
            "worker_instance": "F1",
        }
    ]
    return _bundle(scenario, rows, observations)


def test_s90_named_revision_refusal_and_fresh_remote_retry_passes() -> None:
    verdict = evaluate_bundle(_s90_refusal_bundle())
    assert verdict["status"] == "PASS", verdict


def _s90_typed_refusal_bundle():
    bundle = _s90_refusal_bundle()
    bundle["plan"] = {"s90_refusal_contract": "icefarm-s90-typed-refusal-v1", "commands": []}
    observations = bundle["observations"]
    observations["error106_job_ids"] = []
    observations["failed_p50_source_transfers"] = {"record_count": 1, "records": [{
        "row_job_id": "1", "worker": "F1", "attempt_index": 0,
        "assignment_epoch": 7, "assignment_nonce": 11, "scheduler_job": 41,
        "c_guid": 7, "tu_seq": 2, "failed_endpoint": "10.0.0.2:23003",
        "assignment_identity_line": 6, "assignment_line": 7, "failure_line": 11,
        "retry_line": 12, "retry_assignment_identity_line": 16, "retry_assignment_line": 17,
        "retry_assignment_epoch": 7, "retry_assignment_nonce": 12, "retry_scheduler_job": 42,
        "retry_c_guid": 7, "retry_tu_seq": 3, "retry_endpoint": "10.0.0.2:23003",
        "retry_mode": "legacy", "profile": "P29V1", "error": 0x5002,
        "status": 2, "source_result_status": 4, "transfer_attempts": 1,
        "compile_identity_present": False, "source_result_present": False,
    }]}
    return bundle


def test_s90_typed_refusal_binds_legacy_retry_without_literal_error106():
    bundle = _s90_typed_refusal_bundle()
    verdict = evaluate_bundle(bundle)
    assert verdict["status"] == "PASS", verdict
    del bundle["plan"]["s90_refusal_contract"]
    assert evaluate_bundle(bundle)["status"] == "FAIL"


@pytest.mark.parametrize("mutation", [
    "source_error", "source_status", "terminal_status", "attempt_count",
    "missing_source", "duplicate_source", "wrong_first", "wrong_retry",
    "wrong_client_revision", "wrong_name", "wrong_wire_error", "wrong_endpoint_revision",
    "unrelated_error106", "unknown_row", "missing_mismatch", "local_fallback",
])
def test_s90_typed_refusal_fails_closed(mutation):
    bundle = _s90_typed_refusal_bundle()
    observations = bundle["observations"]
    source = observations["failed_p50_source_transfers"]["records"][0]
    mismatch = observations["wire_revision_mismatches"][0]
    if mutation == "source_error":
        source["error"] = 4
    elif mutation == "source_status":
        source["status"] = 1
    elif mutation == "terminal_status":
        source["source_result_status"] = 3
    elif mutation == "attempt_count":
        source["transfer_attempts"] = 2
    elif mutation == "missing_source":
        observations["failed_p50_source_transfers"] = {"record_count": 0, "records": []}
    elif mutation == "duplicate_source":
        observations["failed_p50_source_transfers"]["records"].append(dict(source))
        observations["failed_p50_source_transfers"]["record_count"] = 2
    elif mutation == "wrong_first":
        source["assignment_nonce"] += 1
    elif mutation == "wrong_retry":
        source["retry_assignment_nonce"] += 1
    elif mutation == "wrong_client_revision":
        observations["wire_revisions"]["C1"] = 2
    elif mutation == "wrong_name":
        mismatch["error"] = "UNKNOWN"
    elif mutation == "wrong_wire_error":
        mismatch["error_code"] = 3
    elif mutation == "wrong_endpoint_revision":
        mismatch["endpoint_wire_revision"] = 1
    elif mutation == "unrelated_error106":
        observations["error106_job_ids"] = ["2"]
    elif mutation == "unknown_row":
        source["row_job_id"] = "2"
    elif mutation == "missing_mismatch":
        observations["wire_revision_mismatches"] = []
    elif mutation == "local_fallback":
        observations["local_fallback_job_ids"] = ["1"]
    assert evaluate_bundle(bundle)["status"] == "FAIL"


@pytest.mark.parametrize(
    "mutation",
    (
        "missing_mismatch",
        "wrong_error",
        "same_scheduler_job",
        "same_assignment_identity",
        "missing_error106",
        "local_fallback",
        "retry_tail",
        "unbound_row",
        "wrong_endpoint_revision",
    ),
)
def test_s90_named_revision_refusal_fails_closed(mutation: str) -> None:
    fixture = _s90_refusal_bundle()
    mismatch = fixture["observations"]["wire_revision_mismatches"][0]
    if mutation == "missing_mismatch":
        fixture["observations"]["wire_revision_mismatches"] = []
    elif mutation == "wrong_error":
        mismatch["error_code"] = 3
    elif mutation == "same_scheduler_job":
        mismatch["retry_assignment"]["scheduler_job"] = 41
    elif mutation == "same_assignment_identity":
        mismatch["retry_assignment"].update(
            assignment_epoch=7, assignment_nonce=11
        )
    elif mutation == "missing_error106":
        fixture["observations"]["error106_job_ids"] = []
    elif mutation == "local_fallback":
        fixture["observations"]["local_fallback_job_ids"] = ["1"]
    elif mutation == "retry_tail":
        fixture["rows"][0].update(
            tail_present=True,
            tail_profile="P29V1",
            session_outcome="committed",
            reuse=False,
        )
    elif mutation == "unbound_row":
        mismatch["row_job_id"] = "2"
    else:
        mismatch["endpoint_wire_revision"] = 1
    verdict = evaluate_bundle(fixture)
    clause = next(
        item
        for item in verdict["clauses"]
        if item["id"] == "s90.named-refusal-fresh-remote-retry"
    )
    assert verdict["status"] == "FAIL"
    assert clause["status"] == "FAIL"


def _s95_disk_fill_bundle() -> dict[str, object]:
    scenario = _scenario("S'C'F'", worker_versions=(50, 50))
    scenario["id"] = "S95-cache-disk-full"
    scenario["workload"].update(
        {
            "corpus": "fmt-100",
            "driver": "tu-manifest",
            "jobs": 2,
            "repeat": 2,
        }
    )
    scenario["timeline"] = [
        {"action": "disk_fill", "instance": "F1", "trigger": "job 12"}
    ]
    scenario["expect"].update(
        {
            "engagement": "s95-cache-disk-full",
            "error106_max": 4,
            "reuse": "all-false-when-p29v1",
        }
    )
    rows = [
        _row(1, worker="F1"),
        _row(2, worker="F2"),
        _row(3, worker="F1", tail=False, outcome="none"),
        _row(4, worker="F2"),
    ]
    rows[2].update(event_epoch=1, retries=1, reuse=None)
    rows[3]["event_epoch"] = 1
    for row in rows:
        if row["tail_profile"] == "P29V1":
            row["reuse"] = False
    observations = _observations(rows)
    observations["error106_job_ids"] = ["3"]
    observations["assignment_lifecycle"][2]["attempts"][0][
        "terminal"
    ] = "cancellation"
    mount = {
        "destination": "/var/cache/icecream",
        "size_bytes": 128 * 1024 * 1024,
        "type": "tmpfs",
    }
    snapshot = {
        "container_id": SHA_A,
        "container_name": "/icefarm-test-run-F1",
        "host": "h1",
        "image_closure_sha256": SHA_A,
        "image_id": SHA_B,
        "labels": {"icefarm.run": "test-run", "icefarm.instance": "F1"},
        "mount": mount,
        "running": True,
        "runtime_path": "/runtime/" + SHA_A,
        "started_at": "2026-09-06T12:00:00Z",
    }
    event = {
        "action": "disk_fill",
        "event_epoch": 1,
        "event_index": 0,
        "fired_ms": 1000,
        "instance": "F1",
        "last_dispatched_job": 12,
        "trigger": "job 12",
        "workload_dispatch_count": 12,
        "receipt": {
            "action": "disk_fill",
            "after": copy.deepcopy(snapshot),
            "before": copy.deepcopy(snapshot),
            "event_epoch": 1,
            "fill": {
                "available_after": 0,
                "available_before": 128 * 1024 * 1024,
                "directory_gid": 65534,
                "directory_mode": 0o700,
                "directory_uid": 65534,
                "elapsed_ms": 10,
                "errno": 28,
                "filler_bytes": 128 * 1024 * 1024 - 4096,
                "filler_path": "/var/cache/icecream/.icefarm-disk-fill",
                "limit_bytes": 128 * 1024 * 1024,
                "minimum_headroom_bytes": 8 * 1024 * 1024,
                "schema": "icefarm-disk-fill-operation-v1",
                "watchdog_s": 30,
            },
            "instance": "F1",
            "schema": "icefarm-disk-fill-v1",
        },
    }
    fixture = _bundle(scenario, rows, observations)
    fixture["event_log"] = [event]
    return fixture


def test_s95_disk_fill_accepts_one_bounded_remote_fallback() -> None:
    fixture = _s95_disk_fill_bundle()
    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "PASS", verdict
    assert next(
        item for item in verdict["clauses"] if item["id"] == "s95.cache-disk-full"
    )["status"] == "PASS"


@pytest.mark.parametrize(
    "mutation",
    (
        "wrong_size",
        "low_headroom",
        "watchdog_overrun",
        "no_enospc",
        "identity_changed",
        "no_affected_post",
        "local_fallback",
        "unbound_error106",
        "early_dispatch",
        "missing_assignment_identity",
        "first_assignment_completed",
        "final_assignment_cancelled",
        "final_worker_tamper",
        "extra_assignment_record",
        "lineage_tamper",
        "wrong_shape",
    ),
)
def test_s95_disk_fill_evidence_fails_closed(mutation: str) -> None:
    fixture = _s95_disk_fill_bundle()
    event = fixture["event_log"][0]
    if mutation == "wrong_size":
        event["receipt"]["fill"]["limit_bytes"] -= 1
    elif mutation == "low_headroom":
        event["receipt"]["fill"]["available_before"] = 8 * 1024 * 1024 - 1
    elif mutation == "watchdog_overrun":
        event["receipt"]["fill"]["elapsed_ms"] = 30_001
    elif mutation == "no_enospc":
        event["receipt"]["fill"]["errno"] = 0
    elif mutation == "identity_changed":
        event["receipt"]["after"]["container_id"] = SHA_B
    elif mutation == "no_affected_post":
        fixture["rows"][2]["cs"] = "F2"
    elif mutation == "local_fallback":
        fixture["observations"]["local_fallback_job_ids"] = ["3"]
    elif mutation == "unbound_error106":
        fixture["observations"]["error106_job_ids"] = ["4"]
    elif mutation == "early_dispatch":
        fixture["event_log"][0]["workload_dispatch_count"] = 11
    elif mutation == "missing_assignment_identity":
        fixture["observations"]["assignment_lifecycle"][2]["attempts"][1][
            "scheduler_job"
        ] = fixture["observations"]["assignment_lifecycle"][2]["attempts"][0][
            "scheduler_job"
        ]
    elif mutation == "first_assignment_completed":
        fixture["observations"]["assignment_lifecycle"][2]["attempts"][0][
            "terminal"
        ] = "completion"
    elif mutation == "final_assignment_cancelled":
        fixture["observations"]["assignment_lifecycle"][2]["attempts"][1][
            "terminal"
        ] = "cancellation"
    elif mutation == "final_worker_tamper":
        fixture["observations"]["assignment_lifecycle"][2]["attempts"][1][
            "worker"
        ] = "F2"
    elif mutation == "extra_assignment_record":
        fixture["observations"]["assignment_lifecycle"].append(
            {"attempts": [], "job_id": "999"}
        )
    elif mutation == "lineage_tamper":
        fixture["event_log"][0]["receipt"]["before"]["host"] = "wrong-host"
    else:
        fixture["scenario"]["workload"]["jobs"] = 1

    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "FAIL"
    assert next(
        item for item in verdict["clauses"] if item["id"] == "s95.cache-disk-full"
    )["status"] == "FAIL" or mutation in {
        "no_affected_post",
        "unbound_error106",
        "missing_assignment_identity",
        "first_assignment_completed",
        "final_assignment_cancelled",
        "final_worker_tamper",
        "extra_assignment_record",
    }


def test_s95_disk_fill_allows_polling_to_observe_past_trigger_threshold() -> None:
    fixture = _s95_disk_fill_bundle()
    fixture["event_log"][0]["workload_dispatch_count"] = 13
    fixture["event_log"][0]["last_dispatched_job"] = 14
    assert evaluate_bundle(fixture)["status"] == "PASS"


def test_s40_pair_scoped_reuse_expectation_is_independent_per_worker() -> None:
    scenario = _scenario("S'C'F'", worker_versions=(50, 50))
    scenario["expect"]["reuse_pairs"] = {"C1/F1": True, "C1/F2": False}
    rows = [
        _row(1, worker="F1", worker_version=50),
        _row(2, worker="F2", worker_version=50),
    ]
    rows[1]["reuse"] = False
    fixture = _bundle(scenario, rows, _observations(rows))
    assert evaluate_bundle(fixture)["status"] == "PASS"

    tampered = copy.deepcopy(fixture)
    tampered["rows"][0]["reuse"] = False
    assert evaluate_bundle(tampered)["status"] == "FAIL"
    tampered = copy.deepcopy(fixture)
    tampered["rows"][1]["reuse"] = True
    assert evaluate_bundle(tampered)["status"] == "FAIL"


def test_s40_header_receipt_is_recomputed_fail_closed() -> None:
    scenario = _scenario("S'C'F'", worker_versions=(50, 50))
    scenario["timeline"] = [
        {
            "trigger": "job 24",
            "action": "header_edit",
            "instance": "F2",
            "path": "/usr/include/stdio.h",
        }
    ]
    scenario["expect"]["reuse_pairs"] = {"C1/F1": True, "C1/F2": False}
    rows = [_row(1, worker="F1"), _row(2, worker="F2")]
    rows[1]["reuse"] = False
    rows[1]["event_epoch"] = 1
    gate = {
        "action": "pause",
        "active_after": 0,
        "active_before": 1,
        "client": "C1",
        "epoch": 1,
        "finished_ms": 100,
        "schema": "icefarm-event-gate-v1",
        "started_ms": 90,
        "status": "PAUSED",
        "turn": "A",
    }
    resume = dict(gate, action="resume", active_before=0, finished_ms=1001, started_ms=1000, status="OPEN")
    receipt = {
        "action": "header_edit",
        "before": {
            "container_id": "a" * 64,
            "header_path": "/usr/include/stdio.h",
            "header_sha256": "b" * 64,
            "p29_cache_files": [
                "p29-system-source-fingerprint-v1.cache",
                "p29-system-source-fingerprint-v1.lock",
            ],
            "running": True,
            "started_at": "before",
        },
        "after": {
            "container_id": "a" * 64,
            "header_path": "/usr/include/stdio.h",
            "header_sha256": "c" * 64,
            "p29_cache_files": [],
            "running": True,
            "started_at": "after",
        },
        "cache_invalidation": {
            "directory": "/var/cache/icecream/p50-runtime",
            "files": [
                "p29-system-source-fingerprint-v1.cache",
                "p29-system-source-fingerprint-v1.lock",
            ],
            "removed": [
                "p29-system-source-fingerprint-v1.cache",
                "p29-system-source-fingerprint-v1.lock",
            ],
        },
        "coordination": {
            "clients": {"C1": gate},
            "ready_ms": 1000,
            "readiness": {
                "cache_line": "cache sidecar adapter state=2 lifecycle=3",
                "host": "host",
                "line": "ICECREAM daemon F2 starting up",
                "log_path": "/var/icefarm/F2/log/iceccd.log",
                "offset": 10,
                "role": "F",
            },
            "resume": {"C1": resume},
            "scheduler_rejoin": {
                "cache_line": (
                    "[1] 2026-09-05 01:02:00: RELOGIN F2(x86_64): "
                    "cache=10.0.0.2:24002 cache_wire=v1 "
                    "cache_protocol=1 cache_profiles=p29v1 zstd_tu zstd_route"
                ),
                "cache_protocol": 1,
                "host": "h1",
                "login_line": (
                    "[1] 2026-09-05 01:02:00: login F2 protocol version: 50"
                ),
                "log_path": "/var/icefarm/S1/log/scheduler.log",
                "offset": 10,
                "profile": "P29V1",
                "role_protocol": 50,
                "scheduler": "S1",
                "target": "F2",
            },
        },
        "event_epoch": 1,
        "instance": "F2",
        "schema": "icefarm-header-edit-v1",
        "turn": "A",
    }
    observations = _observations(rows)
    bundle = _bundle(scenario, rows, observations)
    bundle["event_log"] = [
        {
            "action": "header_edit",
            "event_epoch": 1,
            "event_index": 0,
            "fired_ms": 1000,
            "instance": "F2",
            "last_dispatched_job": 24,
            "receipt": receipt,
            "trigger": "job 24",
            "workload_dispatch_count": 24,
        }
    ]
    assert evaluate_bundle(bundle)["status"] == "PASS"
    tampered = copy.deepcopy(bundle)
    tampered["event_log"][0]["receipt"]["coordination"]["readiness"]["cache_line"] = "stale"
    assert evaluate_bundle(tampered)["status"] == "FAIL"
    tampered = copy.deepcopy(bundle)
    tampered["event_log"][0]["receipt"]["before"]["p29_cache_files"] = []
    assert evaluate_bundle(tampered)["status"] == "FAIL"
    tampered = copy.deepcopy(bundle)
    tampered["event_log"][0]["receipt"]["coordination"]["scheduler_rejoin"][
        "target"
    ] = "F1"
    assert evaluate_bundle(tampered)["status"] == "FAIL"


def _s30_fixture() -> dict[str, object]:
    rows = [
        _row(
            1,
            client_version=50,
            worker_version=50,
            tail=False,
            profile=None,
            outcome="fallback",
        ),
        _row(
            2,
            client_version=50,
            worker_version=50,
            tail=False,
            profile=None,
            outcome="none",
        ),
        _row(
            3,
            client_version=50,
            worker_version=50,
            tail=False,
            profile=None,
            outcome="fallback",
        ),
    ]
    rows[0]["retries"] = 1
    rows[2]["retries"] = 1
    scenario = _scenario("S'C'F'", client_versions=(50,), worker_versions=(50,))
    scenario["id"] = "S30-mutant-f-refusal"
    scenario["images"]["mutant"] = "p50s30-f-refusal-fixture"
    next(item for item in scenario["instances"] if item["role"] == "F")[
        "image"
    ] = "mutant"
    scenario["expect"]["reuse"] = "none-when-legacy"
    scenario["expect"]["error106_max"] = 2
    observations = _observations(rows, revisions={"C1": 1, "F1": 1})
    observations["sidecars"]["C1"]["sessions"] = 1
    observations["sidecars"]["F1"]["sessions"] = 0
    observations["error106_job_ids"] = ["1", "3"]
    observations["legacy_wire"] = {
        "record_count": 3,
        "records": [
            {"job_id": "1", "c_to_f_bytes": 1024, "f_to_c_bytes": 512},
            {"job_id": "2", "c_to_f_bytes": 1024, "f_to_c_bytes": 512},
            {"job_id": "3", "c_to_f_bytes": 1024, "f_to_c_bytes": 512},
        ],
    }
    observations["s30_mutant_f"] = {
        "canary_records": [
            {
                "schema": "icefarm-s30-mutant-f-refusal-v1",
                "refusal": "p50-session-refused",
                "wire_revision": 1,
                "supported_profiles": 1,
            }
        ],
        "canary_refusal_count": 1,
        "schema": "icefarm-s30-mutant-f-refusal-v1",
        "records": [
            {
                "schema": "icefarm-s30-mutant-f-refusal-v1",
                "refusal": "p50-session-refused",
                "wire_revision": 1,
                "supported_profiles": 1,
            }
        ],
        "refusal_count": 1,
        "fallback_job_ids": ["1", "3"],
        "fresh_legacy_assignment_count": 2,
        "local_fallback_job_ids": [],
    }
    return _bundle(scenario, rows, observations)


def test_s30_mutant_requires_one_refusal_and_one_fresh_legacy_retry() -> None:
    fixture = _s30_fixture()
    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "PASS", verdict
    fixture = _s30_fixture()
    fixture["observations"]["sidecars"]["F1"]["sessions"] = 1
    assert evaluate_bundle(fixture)["status"] == "FAIL"
    fixture = _s30_fixture()
    fixture["observations"]["logins"][0]["cache_profiles"] = []
    assert evaluate_bundle(fixture)["status"] == "FAIL"
    fixture = _s30_fixture()
    next(
        item for item in fixture["scenario"]["instances"] if item["role"] == "F"
    )["image"] = "new"
    assert evaluate_bundle(fixture)["status"] == "FAIL"
    fixture = _s30_fixture()
    fixture["observations"]["s30_mutant_f"]["records"][0]["supported_profiles"] = 2
    assert evaluate_bundle(fixture)["status"] == "FAIL"
    fixture = _s30_fixture()
    fixture["rows"][0]["retries"] = 2
    assert evaluate_bundle(fixture)["status"] == "FAIL"
    fixture = _s30_fixture()
    fixture["rows"][1]["retries"] = 1
    assert evaluate_bundle(fixture)["status"] == "FAIL"
    fixture = _s30_fixture()
    fixture["observations"]["s30_mutant_f"]["records"] = []
    fixture["observations"]["s30_mutant_f"]["refusal_count"] = 0
    assert evaluate_bundle(fixture)["status"] == "FAIL"
    fixture = _s30_fixture()
    fixture["observations"]["s30_mutant_f"]["canary_records"] = []
    fixture["observations"]["s30_mutant_f"]["canary_refusal_count"] = 0
    assert evaluate_bundle(fixture)["status"] == "FAIL"


def _control_fixtures() -> dict[str, dict[str, object]]:
    h1 = copy.deepcopy(SHAPE_FIXTURES["S'C'F'"])
    h1.update(
        farm_digest="a" * 64,
        mode="preflight-refusal",
        scenario_digest="b" * 64,
        topology_digest="c" * 64,
    )
    h1["rows"] = []
    h1["observations"]["job_lifecycle"] = []
    h1["observations"]["preflight_refusal"] = {
        "farm_digest": h1["farm_digest"],
        "jobs_started": 0,
        "persistent_start_attempted": False,
        "reason_code": "role-hash-mismatch",
        "scenario_digest": h1["scenario_digest"],
        "schema": "icefarm-preflight-refusal-v1",
        "topology_digest": h1["topology_digest"],
    }

    h2 = copy.deepcopy(SHAPE_FIXTURES["S'C'F'"])
    h2["rows"][0].update(
        reuse=None,
        session_outcome="none",
        tail_present=False,
        tail_profile=None,
    )
    h2["scenario"]["workload"] = {"clients": ["C1"]}
    h2_client = next(
        instance
        for instance in h2["scenario"]["instances"]
        if instance["name"] == "C1"
    )
    h2_client["env"]["ICECC_P50_MODE"] = "off"
    h2["scenario"]["expect"]["reuse"] = "all-false-when-p29v1"
    h2["observations"]["sidecars"]["F1"]["sessions"] = 0
    h2["observations"]["wire_revisions"].pop("C1")
    h2["observations"]["fault"] = {"client_kill_switch": True}

    h3 = copy.deepcopy(SHAPE_FIXTURES["S'[FF'][CC']"])
    h3["mode"] = "control-failure"
    h3["scenario"] = _scenario("mixed", client_versions=(43,), worker_versions=(50,))
    h3["scenario"]["images"] = {
        "mutant": "p50s4-h3-tail-mutant",
        "new": "p50s4-89917385",
        "old": "p43-1.4.0",
    }
    h3["scenario"]["instances"][0]["image"] = "mutant"
    h3["rows"] = []
    h3["topology"] = {"instances": copy.deepcopy(h3["scenario"]["instances"])}
    h3["observations"] = {
        "fault": {"mutant_scheduler": True},
        "h3_control_failure": {
            "authenticated": True,
            "dispatches": [
                {
                    "client": "C1",
                    "dispatch_ms": 1000,
                    "scheduler_job": 1,
                    "terminal": "cancellation",
                    "terminal_ms": 2000,
                    "worker": "F1",
                }
            ],
            "emission": {
                "authenticated": True,
                "records": [
                    {
                        "assignment_epoch": 7,
                        "assignment_nonce": 11,
                        "cache_port": 43123,
                        "cache_profile_mask": 1,
                        "cache_protocol": 1,
                        "client_instance": "C1",
                        "emission": "protocol-50-tail-sent",
                        "scheduler_instance": "S1",
                        "scheduler_job": 1,
                        "schema": "icefarm-scheduler-mutant-trace-v1",
                        "tail_bytes": 12,
                        "tail_hex": "0000a8730000000100000001",
                        "worker_instance": "F1",
                    }
                ],
            },
            "failed_jobs": [
                {
                    "compile_rc": 1,
                    "exact": False,
                    "index": 1,
                    "remote": False,
                    "scheduler_job": "missing-1",
                    "turn": "A",
                    "worker": "UNKNOWN",
                }
            ],
            "rejections": [
                {
                    "bytes_read": 72,
                    "client_instance": "C1",
                    "line": 1,
                    "message_size": 84,
                    "schema": "icefarm-h3-client-rejection-v1",
                    "unread_bytes": 12,
                }
            ],
            "scheduler_dispatch_count": 1,
            "schema": "icefarm-h3-control-failure-v1",
            "successful_product_rows": 0,
            "workload_failure_count": 1,
            "workload_failed": True,
            "workload_job_count": 1,
            "workload_started": True,
        },
    }

    h4 = copy.deepcopy(SHAPE_FIXTURES["S'C'F'"])
    h4["scenario"]["fault"] = {
        "client": "C1",
        "job": 1,
        "kind": "corrupt-object",
    }
    h4["rows"][0].update(exact=False, object_sha_remote=SHA_B)
    h4["observations"]["fault"] = {"corrupt_object": True}
    h4["observations"]["object_corruption"] = {
        "after_sha256": SHA_B,
        "before_sha256": SHA_A,
        "client": "C1",
        "job": 1,
        "row_job_id": "1",
    }

    h5 = copy.deepcopy(SHAPE_FIXTURES["S'C'F'"])
    h5["scenario"]["timeline"] = [
        {"action": "kill -9", "instance": "F1", "trigger": "job 1"}
    ]
    recovered_row = copy.deepcopy(h5["rows"][0])
    recovered_row.update(
        job_id="2",
        retries=1,
        reuse=None,
        session_outcome="none",
        tail_present=False,
        tail_profile=None,
        tu="files/tu-2.ii",
    )
    h5["rows"].append(recovered_row)
    h5["observations"] = _observations(h5["rows"])
    h5["observations"].update(
        error106_job_ids=["2"],
        fault={"worker_killed_job_ids": ["2"]},
        process_loss_recovery_job_ids=["2"],
    )
    return {"H1": h1, "H2": h2, "H3": h3, "H4": h4, "H5": h5}


CONTROL_FIXTURES = _control_fixtures()


@pytest.mark.parametrize("control", sorted(CONTROL_FIXTURES))
def test_harness_control_fixture_produces_its_designed_result(control: str) -> None:
    result = evaluate_control(control, CONTROL_FIXTURES[control])
    assert result["schema"] == CONTROL_VERDICT_SCHEMA
    assert result["status"] == "PASS", result


def test_h2_rejects_negotiated_rows_despite_the_kill_switch() -> None:
    fixture = copy.deepcopy(CONTROL_FIXTURES["H2"])
    fixture["rows"][0].update(
        reuse=True,
        session_outcome="committed",
        tail_present=True,
        tail_profile="P29V1",
    )
    assert evaluate_control("H2", fixture)["status"] == "FAIL"


def test_h2_rejects_an_unrelated_product_failure() -> None:
    fixture = copy.deepcopy(CONTROL_FIXTURES["H2"])
    fixture["rows"][0]["exact"] = False
    fixture["rows"][0]["object_sha_remote"] = SHA_B
    assert evaluate_control("H2", fixture)["status"] == "FAIL"


def test_h5_refuses_a_legacy_retry_without_process_loss_authentication() -> None:
    fixture = copy.deepcopy(CONTROL_FIXTURES["H5"])
    fixture["observations"]["process_loss_recovery_job_ids"] = []

    verdict = evaluate_bundle(fixture)
    failed = {item["id"] for item in verdict["clauses"] if item["status"] == "FAIL"}

    assert {"engagement.expected", "error106.max"} <= failed
    assert evaluate_control("H5", fixture)["status"] == "FAIL"


def test_h5_refuses_more_than_one_recovery_retry() -> None:
    fixture = copy.deepcopy(CONTROL_FIXTURES["H5"])
    fixture["rows"][1]["retries"] = 2

    verdict = evaluate_bundle(fixture)
    retry = next(item for item in verdict["clauses"] if item["id"] == "retries.bounded")

    assert retry["status"] == "FAIL"
    assert retry["offending_job_ids"] == ["2"]


def test_h2_rejects_a_kill_switch_not_bound_to_every_workload_client() -> None:
    fixture = copy.deepcopy(CONTROL_FIXTURES["H2"])
    fixture["scenario"]["workload"]["clients"].append("C2")
    assert evaluate_control("H2", fixture)["status"] == "FAIL"


def test_h1_rejects_an_unbound_self_attested_refusal() -> None:
    fixture = copy.deepcopy(CONTROL_FIXTURES["H1"])
    fixture["observations"]["preflight_refusal"]["topology_digest"] = "d" * 64
    assert evaluate_control("H1", fixture)["status"] == "FAIL"


def test_h4_rejects_a_boolean_only_self_attested_corruption() -> None:
    fixture = copy.deepcopy(CONTROL_FIXTURES["H4"])
    fixture["observations"]["object_corruption"] = None
    assert evaluate_control("H4", fixture)["status"] == "FAIL"


def test_h4_rejects_corruption_digests_not_bound_to_the_named_row() -> None:
    fixture = copy.deepcopy(CONTROL_FIXTURES["H4"])
    fixture["observations"]["object_corruption"]["row_job_id"] = "other-job"
    assert evaluate_control("H4", fixture)["status"] == "FAIL"


def test_h3_rejects_tampered_emission_tuple() -> None:
    fixture = copy.deepcopy(CONTROL_FIXTURES["H3"])
    fixture["observations"]["h3_control_failure"]["emission"]["records"][0][
        "tail_hex"
    ] = "0" * 24
    assert evaluate_control("H3", fixture)["status"] == "FAIL"


def test_h3_rejects_tampered_client_rejection_binding() -> None:
    fixture = copy.deepcopy(CONTROL_FIXTURES["H3"])
    fixture["observations"]["h3_control_failure"]["rejections"][0][
        "bytes_read"
    ] = 80
    assert evaluate_control("H3", fixture)["status"] == "FAIL"


def test_h3_rejects_unknown_client_image_without_crashing() -> None:
    fixture = copy.deepcopy(CONTROL_FIXTURES["H3"])
    client = next(
        item for item in fixture["scenario"]["instances"] if item["role"] == "C"
    )
    client["image"] = "unrecognized"
    fixture["scenario"]["images"].pop("old")
    assert evaluate_control("H3", fixture)["status"] == "FAIL"


def test_h3_rejects_duplicate_rejection_and_failure_witnesses() -> None:
    fixture = copy.deepcopy(CONTROL_FIXTURES["H3"])
    failure = fixture["observations"]["h3_control_failure"]
    failure["dispatches"].append(copy.deepcopy(failure["dispatches"][0]))
    failure["dispatches"][1]["scheduler_job"] = 2
    failure["emission"]["records"].append(
        copy.deepcopy(failure["emission"]["records"][0])
    )
    failure["emission"]["records"][1]["scheduler_job"] = 2
    failure["rejections"].append(copy.deepcopy(failure["rejections"][0]))
    failure["failed_jobs"].append(copy.deepcopy(failure["failed_jobs"][0]))
    failure["scheduler_dispatch_count"] = 2
    assert evaluate_control("H3", fixture)["status"] == "FAIL"


def test_h3_allows_failed_attempts_after_the_old_client_disconnects() -> None:
    fixture = copy.deepcopy(CONTROL_FIXTURES["H3"])
    failure = fixture["observations"]["h3_control_failure"]
    failure["failed_jobs"].append(
        {
            "compile_rc": 0,
            "exact": True,
            "index": 2,
            "remote": False,
            "scheduler_job": "2",
            "turn": "A",
            "worker": "127.0.0.1:0",
        }
    )
    failure["workload_failure_count"] = 2
    failure["workload_job_count"] = 2
    assert evaluate_control("H3", fixture)["status"] == "PASS"


def test_oracle_sample_mismatch_names_the_job() -> None:
    fixture = copy.deepcopy(SHAPE_FIXTURES["S'C'F'"])
    fixture["observations"]["oracle"]["sample_mismatch_job_ids"] = ["1"]
    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "FAIL"
    assert "1" in verdict["offending_job_ids"]
    assert (
        next(item for item in verdict["clauses"] if item["id"] == "oracle.sample")[
            "status"
        ]
        == "FAIL"
    )


def test_wedge_is_recomputed_instead_of_trusting_a_summary() -> None:
    fixture = copy.deepcopy(SHAPE_FIXTURES["S'C'F'"])
    fixture["observations"]["wedges"] = []
    fixture["observations"]["job_lifecycle"][0].update(
        deadline_ms=180_000,
        terminal=None,
        terminal_ms=None,
    )
    verdict = evaluate_bundle(fixture)
    clause = next(item for item in verdict["clauses"] if item["id"] == "wedges")
    assert clause["status"] == "FAIL"
    assert clause["offending_job_ids"] == ["1"]


def test_row_schema_is_closed_and_fail_closed() -> None:
    fixture = copy.deepcopy(SHAPE_FIXTURES["S'C'F'"])
    fixture["rows"][0]["unreviewed"] = True
    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "FAIL"
    assert (
        next(item for item in verdict["clauses"] if item["id"] == "rows.schema")[
            "status"
        ]
        == "FAIL"
    )


def test_heterogeneous_p29_cell_requires_false_reuse_witnesses() -> None:
    fixture = copy.deepcopy(SHAPE_FIXTURES["S'C'F'"])
    fixture["scenario"]["expect"]["reuse"] = "all-false-when-p29v1"
    fixture["rows"][0]["reuse"] = False
    assert evaluate_bundle(fixture)["status"] == "PASS"
    fixture["rows"][0]["reuse"] = True
    verdict = evaluate_bundle(fixture)
    clause = next(item for item in verdict["clauses"] if item["id"] == "reuse")
    assert clause["status"] == "FAIL"
    assert clause["offending_job_ids"] == ["1"]


def test_unknown_reuse_expectation_fails_closed() -> None:
    fixture = copy.deepcopy(SHAPE_FIXTURES["S'C'F'"])
    fixture["scenario"]["expect"]["reuse"] = "silently-ignore"
    verdict = evaluate_bundle(fixture)
    clause = next(item for item in verdict["clauses"] if item["id"] == "reuse")
    assert clause["status"] == "FAIL"
    assert clause["offending_job_ids"] == ["@expect:reuse"]


def test_malformed_login_entry_fails_closed_without_crashing() -> None:
    fixture = copy.deepcopy(SHAPE_FIXTURES["SCF"])
    fixture["observations"]["logins"].append("not-an-object")
    verdict = evaluate_bundle(fixture)
    clause = next(
        item for item in verdict["clauses"] if item["id"] == "shape.negotiated-legacy"
    )
    assert clause["status"] == "FAIL"
    assert clause["offending_job_ids"] == ["@login:1"]


def test_scheduler_first_restart_requires_pre_and_post_event_rows() -> None:
    fixture = copy.deepcopy(SHAPE_FIXTURES["S'CF"])
    fixture["scenario"]["timeline"] = [
        {"action": "restart", "instance": "S1", "trigger": "job 100"}
    ]
    for instance in fixture["scenario"]["instances"]:
        instance["host"] = "h1"
    after = copy.deepcopy(fixture["rows"][0])
    after["job_id"] = "2"
    after["tu"] = "files/tu-2.ii"
    after["event_epoch"] = 1
    fixture["rows"].append(after)
    fixture["observations"] = _observations(
        fixture["rows"], old_workers={"F1"}
    )
    fixture["observations"]["retry_limit_per_job"] = 1
    pause = {
        "action": "pause",
        "active_after": 0,
        "active_before": 1,
        "client": "C1",
        "epoch": 1,
        "finished_ms": 950,
        "schema": "icefarm-event-gate-v1",
        "started_ms": 900,
        "status": "PAUSED",
        "turn": "A",
    }
    resume = {
        "action": "resume",
        "active_after": 1,
        "active_before": 0,
        "client": "C1",
        "epoch": 1,
        "finished_ms": 1002,
        "schema": "icefarm-event-gate-v1",
        "started_ms": 1001,
        "status": "OPEN",
        "turn": "A",
    }
    fixture["event_log"] = [
        {
            "action": "restart",
            "event_epoch": 1,
            "event_index": 0,
            "fired_ms": 1_000,
            "instance": "S1",
            "last_dispatched_job": 100,
            "trigger": "job 100",
            "workload_dispatch_count": 100,
            "receipt": {
                "action": "restart",
                "after": {
                    "container_id": "1" * 64,
                    "started_at": "2026-09-05T13:03:33Z",
                },
                "before": {
                    "container_id": "1" * 64,
                    "started_at": "2026-09-05T12:59:50Z",
                },
                "coordination": {
                    "client_readiness": {
                        "C1": {
                            "cache_line": None,
                            "cache_required": False,
                            "connected_line": (
                                "Connected to scheduler (I am known as 10.0.0.1)"
                            ),
                            "host": "h1",
                            "log_path": "/scratch/icefarm/run/C1/log/client-daemon.log",
                            "offset": 1,
                        }
                    },
                    "clients": {"C1": pause},
                    "ready_ms": 1000,
                    "resume": {"C1": resume},
                    "scheduler_snapshot": "F1 10.0.0.1 1\n",
                    "scheduler_startup": {
                        "host": "h1",
                        "line": "ICECREAM scheduler fixture starting up, port 23000",
                        "log_path": "/tmp/scheduler.log",
                        "offset": 1,
                        "role": "S",
                    },
                    "workers": ["F1"],
                    "worker_snapshot": "F1 10.0.0.1 1\n",
                },
                "event_epoch": 1,
                "instance": "S1",
                "schema": "icefarm-scheduler-restart-v1",
                "turn": "A",
            },
        }
    ]

    assert evaluate_bundle(fixture)["status"] == "PASS"
    fixture["event_log"][0]["workload_dispatch_count"] = 99
    verdict = evaluate_bundle(fixture)
    clause = next(
        item
        for item in verdict["clauses"]
        if item["id"] == "shape.scheduler-first-restart"
    )
    assert clause["status"] == "FAIL"
    fixture["event_log"][0]["workload_dispatch_count"] = 100
    fixture["rows"][1]["event_epoch"] = 1
    fixture["event_log"][0]["receipt"]["coordination"]["clients"]["C1"][
        "active_after"
    ] = 1
    verdict = evaluate_bundle(fixture)
    clause = next(
        item
        for item in verdict["clauses"]
        if item["id"] == "shape.scheduler-first-restart"
    )
    assert clause["status"] == "FAIL"
    fixture["event_log"][0]["receipt"]["coordination"]["clients"]["C1"][
        "active_after"
    ] = 0
    fixture["rows"][1]["event_epoch"] = 0
    verdict = evaluate_bundle(fixture)
    clause = next(
        item
        for item in verdict["clauses"]
        if item["id"] == "shape.scheduler-first-restart"
    )
    assert clause["status"] == "FAIL"


def test_client_route_owner_restart_requires_fresh_owner_and_surviving_daemon() -> None:
    fixture = copy.deepcopy(SHAPE_FIXTURES["S'FC'"])
    fixture["scenario"]["timeline"] = [
        {"action": "restart", "instance": "C1", "trigger": "job 100"}
    ]
    for instance in fixture["scenario"]["instances"]:
        instance["host"] = "h1"
    after = copy.deepcopy(fixture["rows"][0])
    after["job_id"] = "2"
    after["tu"] = "files/tu-2.ii"
    after["event_epoch"] = 1
    fixture["rows"].append(after)
    fixture["observations"] = _observations(
        fixture["rows"], old_workers={"F1"}
    )

    daemon = {
        "argv": ["/opt/icecream/sbin/iceccd", "--fixture"],
        "exe": "/opt/icecream/sbin/iceccd",
        "exe_evidence": "argv0-after-proc-exe-eacces",
        "pid": 1,
        "ppid": 0,
        "start_ticks": 5,
        "uid": 65534,
    }
    before_owner = {
        "argv": ["/opt/icecream/sbin/icecc-cache-service", "--old"],
        "exe": "/opt/icecream/sbin/icecc-cache-service",
        "exe_evidence": "argv0-after-proc-exe-eacces",
        "pid": 10,
        "ppid": 1,
        "start_ticks": 10,
        "uid": 65534,
    }
    after_owner = before_owner | {
        "argv": ["/opt/icecream/sbin/icecc-cache-service", "--new"],
        "pid": 11,
        "start_ticks": 20,
    }
    pause = {
        "action": "pause",
        "active_after": 0,
        "active_before": 2,
        "client": "C1",
        "epoch": 1,
        "finished_ms": 920,
        "schema": "icefarm-event-gate-v1",
        "started_ms": 900,
        "status": "PAUSED",
        "turn": "A",
    }
    resume = {
        "action": "resume",
        "active_after": 1,
        "active_before": 0,
        "client": "C1",
        "epoch": 1,
        "finished_ms": 1020,
        "schema": "icefarm-event-gate-v1",
        "started_ms": 1010,
        "status": "OPEN",
        "turn": "A",
    }
    fixture["event_log"] = [
        {
            "action": "restart",
            "event_epoch": 1,
            "event_index": 0,
            "fired_ms": 1000,
            "instance": "C1",
            "last_dispatched_job": 100,
            "trigger": "job 100",
            "workload_dispatch_count": 100,
            "receipt": {
                "action": "restart",
                "after": {
                    "container_id": "2" * 64,
                    "container_started_at": "2026-09-05T13:00:00Z",
                    "daemon": daemon,
                    "route_owner": after_owner,
                },
                "before": {
                    "container_id": "2" * 64,
                    "container_started_at": "2026-09-05T13:00:00Z",
                    "daemon": daemon,
                    "route_owner": before_owner,
                },
                "coordination": {
                    "clients": {"C1": pause},
                    "ready_ms": 1000,
                    "readiness": {
                        "host": "h1",
                        "lifecycle": 3,
                        "line": "cache sidecar adapter state=2 lifecycle=3",
                        "log_path": "/scratch/icefarm/run/C1/log/client-daemon.log",
                        "offset": 10,
                        "state": 2,
                    },
                    "resume": {"C1": resume},
                    "signal": {
                        "daemon": daemon,
                        "mechanism": "pidfd_send_signal",
                        "route_owner": before_owner,
                        "schema": "icefarm-client-route-signal-v1",
                        "sent_ms": 950,
                        "signal": 9,
                    },
                },
                "event_epoch": 1,
                "instance": "C1",
                "schema": "icefarm-client-route-restart-v1",
                "turn": "A",
            },
        }
    ]

    verdict = evaluate_bundle(fixture)
    assert not any(
        item["id"] == "event.transition-0" for item in verdict["clauses"]
    )
    clause = next(
        item
        for item in verdict["clauses"]
        if item["id"] == "shape.client-route-owner-restart"
    )
    assert clause["status"] == "PASS", verdict

    changed_daemon = dict(
        fixture["event_log"][0]["receipt"]["after"]["daemon"]
    )
    changed_daemon["start_ticks"] = 6
    fixture["event_log"][0]["receipt"]["after"]["daemon"] = changed_daemon
    verdict = evaluate_bundle(fixture)
    clause = next(
        item
        for item in verdict["clauses"]
        if item["id"] == "shape.client-route-owner-restart"
    )
    assert clause["status"] == "FAIL"
