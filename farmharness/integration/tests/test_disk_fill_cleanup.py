from __future__ import annotations

import threading
from pathlib import Path
from typing import Any

import pytest

from farmharness.integration.events import EventError, EventProducer, TimelineEvent
from farmharness.integration.tests.test_events import _fixture
from farmharness.integration import farmtest


def _producer(tmp_path: Path, *, s95: bool = False, multi_client: bool = False):
    farm, scenario, _ = _fixture(tmp_path)
    scenario.data["timeline"] = [
        {"trigger": "t+0", "action": "disk_fill", "instance": "F1"}
    ]
    if s95:
        scenario.data.setdefault("expect", {})["engagement"] = "s95-cache-disk-full"
    plan = farmtest.build_plan(farm, scenario, run_id="event-unit")
    if multi_client:
        scenario.data["workload"]["clients"] = ["C1", "C2"]
        c1 = next(item for item in plan["topology"]["instances"] if item["name"] == "C1")
        plan["topology"]["instances"].append(
            dict(c1, name="C2", host="tt-quietbox3")
        )
    producer = object.__new__(EventProducer)
    producer.scenario = scenario
    producer.plan = plan
    producer._lock = threading.Lock()
    producer._active_turn = "turn-A"
    producer._records = []

    instances = {item["name"]: item for item in plan["topology"]["instances"]}
    producer._container = lambda name: (f"icefarm-event-unit-{name}", instances[name])
    producer._inspect = lambda name: {"id": "3" * 64}
    producer._command_timeout = lambda: 10
    return producer, instances


def _exercise(
    tmp_path: Path,
    *,
    s95: bool = False,
    multi_client: bool = False,
    fail_at: tuple[str, str] | None = None,
) -> tuple[list[tuple[str, str | None]], BaseException | None]:
    producer, instances = _producer(
        tmp_path, s95=s95, multi_client=multi_client
    )
    trace: list[tuple[str, str | None]] = []

    def fill(event: TimelineEvent, instance: dict[str, Any], before: dict[str, Any]):
        trace.append(("fill", None))
        if fail_at == ("fill", ""):
            raise RuntimeError("fill failed")
        return {"action": "disk_fill", "instance": event.instance}

    def gates(
        selected: list[dict[str, Any]], *, action: str, turn: str, epoch: int,
        timeout_s: int, receipts: dict[str, dict[str, Any]],
    ) -> None:
        failure: BaseException | None = None
        for client in selected:
            name = client["name"]
            trace.append((action, name))
            if fail_at == (action, name):
                failure = RuntimeError(f"{action} failed for {name}")
                # A partial pause/resume models a later client failing after
                # earlier clients have already received the control.
                if action not in {"pause", "resume"}:
                    raise failure
                break
            receipts[name] = {"action": action}
        if failure is not None:
            raise failure

    producer._authenticated_disk_fill = fill
    producer._gate_controls = gates
    event = TimelineEvent.from_dict(
        0, {"trigger": "t+0", "action": "disk_fill", "instance": "F1"}
    )
    error: BaseException | None = None
    try:
        producer._dispatch(event)
    except BaseException as exc:
        error = exc
    return trace, error


def _client_names(trace: list[tuple[str, str | None]], action: str) -> list[str]:
    return [name for seen_action, name in trace if seen_action == action and name]


def test_ordinary_fill_failure_aborts_all_clients_after_pause(tmp_path: Path) -> None:
    trace, error = _exercise(tmp_path, fail_at=("fill", ""))

    assert isinstance(error, RuntimeError)
    assert [action for action, _ in trace] == ["pause", "fill", "abort"]
    assert _client_names(trace, "abort") == _client_names(trace, "pause")


def test_s95_partial_pause_failure_aborts_all_clients_after_fill(tmp_path: Path) -> None:
    producer, instances = _producer(tmp_path, s95=True, multi_client=True)
    clients = [instances[name] for name in producer.scenario.data["workload"]["clients"]]

    trace, error = _exercise(
        tmp_path,
        s95=True,
        multi_client=True,
        fail_at=("pause", clients[-1]["name"]),
    )

    assert isinstance(error, RuntimeError)
    assert [action for action, _ in trace].count("fill") == 1
    assert trace.index(("fill", None)) < trace.index(("pause", clients[0]["name"]))
    assert _client_names(trace, "pause") == [client["name"] for client in clients]
    assert _client_names(trace, "abort") == [client["name"] for client in clients]


@pytest.mark.parametrize("s95", [False, True])
def test_resume_failure_aborts_all_clients(tmp_path: Path, s95: bool) -> None:
    producer, instances = _producer(tmp_path, s95=s95, multi_client=True)
    clients = [instances[name] for name in producer.scenario.data["workload"]["clients"]]

    trace, error = _exercise(
        tmp_path, s95=s95, multi_client=True,
        fail_at=("resume", clients[-1]["name"]),
    )

    assert isinstance(error, RuntimeError)
    assert _client_names(trace, "abort") == [client["name"] for client in clients]


@pytest.mark.parametrize(
    ("s95", "expected"),
    [
        (False, ["pause", "fill", "resume"]),
        (True, ["fill", "pause", "resume"]),
    ],
)
def test_success_preserves_expected_admission_order(
    tmp_path: Path, s95: bool, expected: list[str]
) -> None:
    trace, error = _exercise(tmp_path, s95=s95)

    assert error is None
    # The gate is applied once per client, so compare phase order after
    # collapsing adjacent per-client calls into one transition.
    phases: list[str] = []
    for action, _name in trace:
        if not phases or phases[-1] != action:
            phases.append(action)
    assert phases == expected


def test_abort_failure_keeps_primary_failure_and_reports_cleanup_failure(
    tmp_path: Path,
) -> None:
    producer, instances = _producer(tmp_path)
    clients = [instances[name] for name in producer.scenario.data["workload"]["clients"]]
    trace: list[tuple[str, str | None]] = []

    def gates(selected, *, action, **kwargs):
        trace.extend((action, client["name"]) for client in selected)
        if action == "resume":
            raise RuntimeError("original resume failure")
        if action == "abort":
            raise RuntimeError("abort transport failure")

    producer._authenticated_disk_fill = lambda *args: {"action": "disk_fill"}
    producer._gate_controls = gates
    event = TimelineEvent.from_dict(
        0, {"trigger": "t+0", "action": "disk_fill", "instance": "F1"}
    )

    with pytest.raises(EventError) as caught:
        producer._dispatch(event)

    assert "original resume failure" in str(caught.value)
    assert "abort transport failure" in str(caught.value)
    assert isinstance(caught.value.__cause__, RuntimeError)
    assert str(caught.value.__cause__) == "original resume failure"
    assert _client_names(trace, "abort") == [client["name"] for client in clients]


def test_disk_fill_without_active_turn_keeps_direct_operation_path(
    tmp_path: Path,
) -> None:
    producer, _instances = _producer(tmp_path)
    producer._active_turn = None
    trace: list[str] = []
    producer._authenticated_disk_fill = lambda *args: trace.append("fill") or {
        "action": "disk_fill"
    }
    producer._gate_controls = lambda *args, **kwargs: pytest.fail(
        "inactive unit dispatch must not gate clients"
    )
    event = TimelineEvent.from_dict(
        0, {"trigger": "t+0", "action": "disk_fill", "instance": "F1"}
    )

    assert producer._dispatch(event) == {"action": "disk_fill"}
    assert trace == ["fill"]
