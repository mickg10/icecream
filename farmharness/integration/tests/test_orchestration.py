from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path

import pytest

from farmharness.integration import farmtest
from farmharness.integration.farm_spec import load_farm_spec
from farmharness.integration.lifecycle import PreflightRefusal
from farmharness.integration.report import ReportError, verify_control_bundle
from farmharness.integration.scenario_spec import load_scenario_spec
from farmharness.integration.schema_validation import canonical_bytes
from farmharness.integration.workload import WorkloadError


INTEGRATION = Path(__file__).resolve().parents[1]


def _cell(tmp_path: Path):
    farm = load_farm_spec(INTEGRATION / "farm.example.json")
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(INTEGRATION / "scenarios" / "S00-smoke.json", farm)
    plan = farmtest.build_plan(farm, scenario, run_id="scenario-unit")
    return farm, scenario, plan


def test_new_run_id_has_normative_utc_shape() -> None:
    assert re.fullmatch(r"[0-9]{8}T[0-9]{6}Z-[0-9a-f]{6}", farmtest.new_run_id())


def test_replay_cli_auto_detects_a_suite_result(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    (tmp_path / "suite.json").write_text("{}", encoding="utf-8")
    receipt = {
        "run_id": "suite-run",
        "schema": "icefarm-suite-replay-v1",
        "status": "REPRODUCED",
        "suite_status": "PASS",
    }
    observed: list[str | None] = []

    def replay_suite(_root, *, expected_execution_digest=None):
        observed.append(expected_execution_digest)
        return receipt

    monkeypatch.setattr(farmtest, "replay_suite", replay_suite)
    monkeypatch.setattr(
        farmtest,
        "replay_bundle",
        lambda _root: pytest.fail("atomic replay must not be selected"),
    )

    expected = "a" * 64
    assert farmtest.main(
        [
            "replay",
            "--bundle",
            str(tmp_path),
            "--expected-execution-digest",
            expected,
        ]
    ) == 0
    assert json.loads(capsys.readouterr().out) == receipt
    assert observed == [expected]


def _historical_bundle() -> dict[str, object]:
    topology: dict[str, object] = {
        "instances": [],
        "schema": "icecream-newgen-farm-topology-v2",
    }
    topology["topology_digest"] = hashlib.sha256(canonical_bytes(topology)).hexdigest()
    return {
        "farm": {"schema": "icefarm-farm-v1"},
        "plan": {
            "icefarm_env": {
                "ICEFARM_INSTANCES": json.dumps(
                    [{"name": "C1", "role": "C", "host": "q3", "image": "p50"}]
                )
            }
        },
        "run_id": "historical-run",
        "scenario": {"schema": "icefarm-scenario-v1"},
        "topology": topology,
        "topology_digest": topology["topology_digest"],
    }


def test_replay_labels_the_narrow_historical_v2_digest_path(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    retained = _historical_bundle()
    monkeypatch.setattr(farmtest, "load_verified_bundle", lambda _root: retained)
    monkeypatch.setattr(farmtest, "evaluate_bundle", lambda _bundle: {"status": "PASS"})

    receipt = farmtest.replay_bundle(tmp_path)

    assert receipt["status"] == "REPRODUCED"
    assert receipt["resolver_mode"] == "historical-v2-self-digest"


def test_replay_accepts_one_already_authenticated_bundle_load(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    retained = _historical_bundle()
    monkeypatch.setattr(
        farmtest,
        "load_verified_bundle",
        lambda _root: pytest.fail("suite replay must not verify the bundle twice"),
    )
    monkeypatch.setattr(farmtest, "evaluate_bundle", lambda _bundle: {"status": "PASS"})

    receipt = farmtest.replay_bundle(tmp_path, _retained=retained)

    assert receipt["status"] == "REPRODUCED"


def test_replay_refuses_a_bad_historical_v2_self_digest(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    retained = _historical_bundle()
    retained["topology"]["instances"] = [{"name": "mutated"}]
    monkeypatch.setattr(farmtest, "load_verified_bundle", lambda _root: retained)

    with pytest.raises(ReportError, match="replayed topology digest differs"):
        farmtest.replay_bundle(tmp_path)


def test_replay_recomputes_and_authenticates_declared_controls(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    retained = _historical_bundle()
    retained["scenario"]["controls"] = ["H2"]
    control = {"control": "H2", "status": "PASS"}
    persisted = {
        "controls": [control],
        "schema": "icefarm-controls-verdict-v1",
        "status": "PASS",
    }
    (tmp_path / "control-verdict.json").write_text(
        json.dumps(persisted), encoding="utf-8"
    )
    monkeypatch.setattr(farmtest, "load_verified_bundle", lambda _root: retained)
    monkeypatch.setattr(farmtest, "evaluate_bundle", lambda _bundle: {"status": "FAIL"})
    monkeypatch.setattr(
        farmtest,
        "evaluate_control",
        lambda control_id, _bundle: {"control": control_id, "status": "PASS"},
    )

    receipt = farmtest.replay_bundle(tmp_path)

    assert receipt["controls"] == persisted


def test_replay_refuses_a_tampered_control_verdict(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    retained = _historical_bundle()
    retained["scenario"]["controls"] = ["H2"]
    (tmp_path / "control-verdict.json").write_text(
        json.dumps(
            {
                "controls": [{"control": "H2", "status": "FAIL"}],
                "schema": "icefarm-controls-verdict-v1",
                "status": "FAIL",
            }
        ),
        encoding="utf-8",
    )
    monkeypatch.setattr(farmtest, "load_verified_bundle", lambda _root: retained)
    monkeypatch.setattr(farmtest, "evaluate_bundle", lambda _bundle: {"status": "FAIL"})
    monkeypatch.setattr(
        farmtest,
        "evaluate_control",
        lambda control_id, _bundle: {"control": control_id, "status": "PASS"},
    )

    with pytest.raises(ReportError, match="control verdict differs"):
        farmtest.replay_bundle(tmp_path)


def test_scenario_orchestration_verifies_before_down_and_reports_after(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    farm, scenario, plan = _cell(tmp_path)
    order: list[str] = []
    monkeypatch.setattr(
        farmtest, "bring_up", lambda *args, **kwargs: order.append("up")
    )
    monkeypatch.setattr(
        farmtest, "run_workload", lambda *args, **kwargs: order.append("run")
    )
    monkeypatch.setattr(
        farmtest, "collect_bundle", lambda *args, **kwargs: order.append("collect")
    )
    monkeypatch.setattr(
        farmtest, "verify_bundle", lambda *args, **kwargs: order.append("verify")
    )
    monkeypatch.setattr(
        farmtest, "down_from_state", lambda *args, **kwargs: order.append("down")
    )

    def report(*_args, **_kwargs):
        order.append("report")
        return {"status": "PASS"}, "evidence\n"

    monkeypatch.setattr(farmtest, "report_bundle", report)

    verdict, rendered = farmtest.run_scenario(farm, scenario, plan)

    assert verdict == {"status": "PASS"}
    assert rendered == "evidence\n"
    assert order == ["up", "run", "collect", "verify", "down", "report"]


def test_scenario_orchestration_tears_down_after_workload_failure(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    farm, scenario, plan = _cell(tmp_path)
    order: list[str] = []
    monkeypatch.setattr(
        farmtest, "bring_up", lambda *args, **kwargs: order.append("up")
    )

    def fail(*_args, **_kwargs):
        order.append("run")
        raise WorkloadError("injected workload failure")

    monkeypatch.setattr(farmtest, "run_workload", fail)
    monkeypatch.setattr(
        farmtest, "down_from_state", lambda *args, **kwargs: order.append("down")
    )

    with pytest.raises(WorkloadError, match="injected workload failure"):
        farmtest.run_scenario(farm, scenario, plan)

    assert order == ["up", "run", "down"]


def test_h1_preflight_refusal_becomes_a_replayable_zero_job_bundle(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    farm, scenario, _old_plan = _cell(tmp_path)
    scenario.data["controls"] = ["H1"]
    plan = farmtest.build_plan(farm, scenario, run_id="h1-unit")
    root = tmp_path / "results" / "h1-unit"

    def refuse(*_args, **_kwargs):
        root.mkdir(parents=True)
        bindings = {
            "farm_digest": plan["farm_digest"],
            "run_id": plan["run_id"],
            "scenario_digest": plan["scenario_digest"],
            "topology_digest": plan["topology_digest"],
        }
        refusal = {
            **bindings,
            "commands": [],
            "details": {
                "expected_sha256": "a" * 64,
                "host": "tt-quietbox3",
                "observed_sha256": "b" * 64,
                "product": "test-product",
                "role": "S",
            },
            "error": "injected role hash mismatch",
            "jobs_started": 0,
            "persistent_start_attempted": False,
            "reason_code": "role-hash-mismatch",
            "schema": "icefarm-preflight-refusal-v1",
        }
        lifecycle = {
            **bindings,
            "commands": [],
            "error": refusal["error"],
            "plan": plan,
            "schema": "icefarm-lifecycle-v1",
            "status": "FAILED",
        }
        (root / "preflight-refusal.json").write_bytes(canonical_bytes(refusal))
        (root / "lifecycle.json").write_bytes(canonical_bytes(lifecycle))
        raise PreflightRefusal(
            refusal["error"],
            reason_code="role-hash-mismatch",
            details=refusal["details"],
        )

    monkeypatch.setattr(farmtest, "bring_up", refuse)
    monkeypatch.setattr(
        farmtest,
        "run_workload",
        lambda *_args, **_kwargs: pytest.fail("H1 must not start a workload"),
    )
    monkeypatch.setattr(
        farmtest,
        "down_from_state",
        lambda *_args, **_kwargs: pytest.fail("H1 must not need persistent teardown"),
    )

    product, rendered = farmtest.run_scenario(farm, scenario, plan)

    assert product["status"] == "FAIL"
    assert "Workload: not started" in rendered
    control = verify_control_bundle(root)
    assert control["status"] == "PASS"
    assert (root / "SHA256SUMS").is_file()
    assert (root / "witness.json").is_file()
    retained_refusal = root / "evidence" / "receipts" / "preflight-refusal.json"
    tampered = json.loads(retained_refusal.read_text())
    tampered["jobs_started"] = 1
    retained_refusal.write_text(json.dumps(tampered), encoding="utf-8")
    with pytest.raises(ReportError, match="checksum mismatch"):
        verify_control_bundle(root)
