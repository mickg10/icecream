from __future__ import annotations

from contextlib import contextmanager
from farmharness.integration import farmtest
from farmharness.integration.live_lock import LiveRunLockError


class _DummyFarm:
    pass


class _DummySuite:
    pass


class _DummyScenario:
    pass


def test_cli_suite_holds_one_lock_across_all_cells(monkeypatch) -> None:
    state = {"locked": False, "calls": 0}

    @contextmanager
    def lock():
        assert not state["locked"]
        state["locked"] = True
        try:
            yield
        finally:
            state["locked"] = False

    def run_suite(*_args, **_kwargs):
        assert state["locked"]
        state["calls"] += 1
        return ({"status": "PASS"}, "suite report\n")

    monkeypatch.setattr(farmtest, "live_run_lock", lock)
    monkeypatch.setattr(farmtest, "load_farm_spec", lambda _path: _DummyFarm())
    monkeypatch.setattr(farmtest, "load_suite_spec", lambda _path: _DummySuite())
    monkeypatch.setattr(farmtest, "run_suite", run_suite)
    assert farmtest.main(["suite", "--farm", "farm.json", "--suite", "suite.json"]) == 0
    assert state["calls"] == 1
    assert not state["locked"]


def test_cli_scenario_busy_lock_refuses_before_run(monkeypatch, capsys) -> None:
    called = {"run": False}

    @contextmanager
    def busy_lock():
        raise LiveRunLockError("another process owns the live farm lock")
        yield

    monkeypatch.setattr(farmtest, "live_run_lock", busy_lock)
    monkeypatch.setattr(farmtest, "load_farm_spec", lambda _path: _DummyFarm())
    monkeypatch.setattr(farmtest, "load_scenario_spec", lambda _path, _farm: _DummyScenario())
    monkeypatch.setattr(farmtest, "build_plan", lambda *_args, **_kwargs: {"run_id": "unit"})

    def run_scenario(*_args, **_kwargs):
        called["run"] = True
        raise AssertionError("scenario mutation started despite busy lock")

    monkeypatch.setattr(farmtest, "run_scenario", run_scenario)
    assert farmtest.main([
        "scenario", "--farm", "farm.json", "--scenario", "scenario.json"
    ]) == 3
    assert not called["run"]
    assert "live farm lock unavailable" in capsys.readouterr().err


def test_cli_replay_does_not_acquire_live_lock(monkeypatch) -> None:
    called = {"lock": False}

    @contextmanager
    def lock():
        called["lock"] = True
        yield

    monkeypatch.setattr(farmtest, "live_run_lock", lock)
    monkeypatch.setattr(farmtest, "replay_bundle", lambda _path: {"status": "REPRODUCED"})
    assert farmtest.main(["replay", "--bundle", "bundle"]) == 0
    assert not called["lock"]


def test_cli_plan_does_not_acquire_live_lock(monkeypatch) -> None:
    called = {"lock": False}

    @contextmanager
    def lock():
        called["lock"] = True
        yield

    monkeypatch.setattr(farmtest, "live_run_lock", lock)
    monkeypatch.setattr(farmtest, "load_farm_spec", lambda _path: _DummyFarm())
    monkeypatch.setattr(farmtest, "load_scenario_spec", lambda _path, _farm: _DummyScenario())
    monkeypatch.setattr(farmtest, "build_plan", lambda *_args, **_kwargs: {"run_id": "unit"})
    monkeypatch.setattr(farmtest, "render_plan", lambda _plan: "plan\n")
    assert farmtest.main(["plan", "--farm", "farm.json", "--scenario", "scenario.json"]) == 0
    assert not called["lock"]
