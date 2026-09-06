from __future__ import annotations

import hashlib
import json
import shutil
from copy import deepcopy
from itertools import count
from pathlib import Path

import pytest

from farmharness.integration.tests import farm_fixture

from farmharness.integration import farmtest
from farmharness.integration.farm_spec import load_farm_spec
from farmharness.integration.performance import (
    S80_ORDER,
    S80EvidenceError,
    validate_s80_arm_scenario,
)
from farmharness.integration.report import ReportError
from farmharness.integration.scenario_spec import load_scenario_spec
from farmharness.integration.schema_validation import canonical_bytes
from farmharness.integration.suite_spec import (
    CONTROL_SCENARIOS,
    SuiteSpecError,
    load_suite_spec,
)


INTEGRATION = Path(__file__).resolve().parents[1]


def _suite_tree(tmp_path: Path, scenario_ids: list[str]) -> Path:
    integration = tmp_path / "integration"
    scenarios = integration / "scenarios"
    suites = integration / "suites"
    scenarios.mkdir(parents=True)
    suites.mkdir()
    for scenario_id in scenario_ids:
        shutil.copy2(
            INTEGRATION / "scenarios" / f"{scenario_id}.json",
            scenarios / f"{scenario_id}.json",
        )
    path = suites / "test.json"
    path.write_text(
        json.dumps(
            {
                "id": "test-suite",
                "scenarios": scenario_ids,
                "schema": "icefarm-suite-v1",
            }
        ),
        encoding="utf-8",
    )
    return path


def _s80_suite_tree(tmp_path: Path) -> Path:
    integration = tmp_path / "integration"
    scenarios = integration / "scenarios"
    suites = integration / "suites"
    scenarios.mkdir(parents=True)
    suites.mkdir()
    arm_ids = {
        "P29V1": "S80-p29v1",
        "ZSTD_TU": "S80-zstd-tu",
        "ZSTD_ROUTE": "S80-zstd-route",
        "legacy": "S80-legacy",
    }
    base = json.loads(
        (INTEGRATION / "scenarios" / "S40-full-newgen-engagement.json").read_text()
    )
    base["instances"] = [item for item in base["instances"] if item["name"] != "F2"]
    base["timeline"] = []
    base["expect"].pop("reuse_pairs")
    base["expect"]["reuse"] = "none-when-legacy"
    for arm, scenario_id in arm_ids.items():
        scenario = json.loads(json.dumps(base))
        scenario["id"] = scenario_id
        scenario["instances"][0]["env"]["ICECC_P50_PROFILE"] = (
            "OFF" if arm == "legacy" else arm
        )
        scenario["expect"]["reuse"] = (
            "all-true-when-p29v1" if arm == "P29V1" else "none-when-legacy"
        )
        (scenarios / f"{scenario_id}.json").write_text(
            json.dumps(scenario), encoding="utf-8"
        )
    path = suites / "twobuild.json"
    path.write_text(
        json.dumps(
            {
                "id": "twobuild",
                "performance": {"arms": arm_ids, "kind": "s80"},
                "scenarios": list(arm_ids.values()),
                "schema": "icefarm-suite-v1",
            }
        ),
        encoding="utf-8",
    )
    return path


def _control_suite_tree(tmp_path: Path, *, kind: str = "controls") -> Path:
    integration = tmp_path / "integration"
    scenarios = integration / "scenarios"
    suites = integration / "suites"
    scenarios.mkdir(parents=True)
    suites.mkdir()
    base = json.loads(
        (INTEGRATION / "scenarios" / "H2-client-kill-switch.json").read_text()
    )
    ids: list[str] = []
    for scenario_id, control_id in CONTROL_SCENARIOS:
        scenario = json.loads(json.dumps(base))
        scenario["id"] = scenario_id
        scenario["controls"] = [control_id]
        (scenarios / f"{scenario_id}.json").write_text(
            json.dumps(scenario), encoding="utf-8"
        )
        ids.append(scenario_id)
    if kind == "smoke":
        shutil.copy2(
            INTEGRATION / "scenarios" / "S00-smoke.json",
            scenarios / "S00-smoke.json",
        )
        ids.append("S00-smoke")
    path = suites / f"{kind}.json"
    path.write_text(
        json.dumps(
            {
                "id": kind,
                "kind": kind,
                "scenarios": ids,
                "schema": "icefarm-suite-v1",
            }
        ),
        encoding="utf-8",
    )
    return path


def _composite_suite_tree(tmp_path: Path) -> Path:
    integration = tmp_path / "integration"
    scenarios = integration / "scenarios"
    suites = integration / "suites"
    scenarios.mkdir(parents=True)
    suites.mkdir()
    children = {
        "first": "S00-smoke",
        "second": "S10-baseline-smoke",
    }
    for suite_name, scenario_id in children.items():
        shutil.copy2(
            INTEGRATION / "scenarios" / f"{scenario_id}.json",
            scenarios / f"{scenario_id}.json",
        )
        (suites / f"{suite_name}.json").write_text(
            json.dumps(
                {
                    "id": f"{suite_name}-declared-id",
                    "scenarios": [scenario_id],
                    "schema": "icefarm-suite-v1",
                }
            ),
            encoding="utf-8",
        )
    root = suites / "composite.json"
    root.write_text(
        json.dumps(
            {
                "id": "composite-test",
                "kind": "composite",
                "schema": "icefarm-suite-v1",
                "suites": list(children),
            }
        ),
        encoding="utf-8",
    )
    return root


def test_suite_spec_refuses_an_absent_scenario(tmp_path: Path) -> None:
    path = _suite_tree(tmp_path, ["S00-smoke"])
    document = json.loads(path.read_text(encoding="utf-8"))
    document["scenarios"].append("absent")
    path.write_text(json.dumps(document), encoding="utf-8")

    with pytest.raises(SuiteSpecError, match="absent or unsafe"):
        load_suite_spec(path)


@pytest.mark.parametrize("keep", ["both", "neither"])
def test_suite_spec_requires_exactly_one_cell_or_suite_list(
    tmp_path: Path, keep: str
) -> None:
    path = _composite_suite_tree(tmp_path)
    document = json.loads(path.read_text(encoding="utf-8"))
    if keep == "both":
        document["scenarios"] = ["S00-smoke"]
    else:
        document.pop("suites")
    path.write_text(json.dumps(document), encoding="utf-8")

    with pytest.raises(SuiteSpecError, match="exactly one of scenarios or suites"):
        load_suite_spec(path)


def test_composite_suite_refuses_missing_symlink_and_cycles(tmp_path: Path) -> None:
    path = _composite_suite_tree(tmp_path)
    suites = path.parent
    document = json.loads(path.read_text(encoding="utf-8"))
    document["suites"] = ["absent"]
    path.write_text(json.dumps(document), encoding="utf-8")
    with pytest.raises(SuiteSpecError, match="included suite.*absent or unsafe"):
        load_suite_spec(path)

    document["suites"] = ["linked"]
    path.write_text(json.dumps(document), encoding="utf-8")
    (suites / "linked.json").symlink_to(suites / "first.json")
    with pytest.raises(SuiteSpecError, match="included suite.*absent or unsafe"):
        load_suite_spec(path)

    for name, child in (("cycle-a", "cycle-b"), ("cycle-b", "cycle-a")):
        (suites / f"{name}.json").write_text(
            json.dumps(
                {
                    "id": name,
                    "kind": "composite",
                    "schema": "icefarm-suite-v1",
                    "suites": [child],
                }
            ),
            encoding="utf-8",
        )
    with pytest.raises(SuiteSpecError, match="suite include cycle"):
        load_suite_spec(suites / "cycle-a.json")


def test_nested_casefold_collision_refuses_before_any_result_tree(
    tmp_path: Path,
) -> None:
    path = _composite_suite_tree(tmp_path)
    suites = path.parent
    for name in ("leaf", "LEAF"):
        (suites / f"{name}.json").write_text(
            json.dumps(
                {
                    "id": f"{name}-id",
                    "scenarios": ["S00-smoke"],
                    "schema": "icefarm-suite-v1",
                }
            ),
            encoding="utf-8",
        )
    (suites / "later.json").write_text(
        json.dumps(
            {
                "id": "later",
                "kind": "composite",
                "schema": "icefarm-suite-v1",
                "suites": ["leaf", "LEAF"],
            }
        ),
        encoding="utf-8",
    )
    root = json.loads(path.read_text(encoding="utf-8"))
    root["suites"] = ["first", "later"]
    path.write_text(json.dumps(root), encoding="utf-8")

    with pytest.raises(SuiteSpecError, match="collide case-insensitively"):
        load_suite_spec(path)
    assert not (tmp_path / "results").exists()


def test_checked_in_ladder_is_fixed_order_and_keeps_s50_as_a_child_gate() -> None:
    suite = load_suite_spec(INTEGRATION / "suites" / "ladder.json")
    farm = load_farm_spec(farm_fixture.example_farm_path())

    assert suite.is_composite
    assert suite.data["suites"] == [
        "s10",
        "s20",
        "s30",
        "s30-mutant-f",
        "s40",
        "s50",
        "s60",
    ]
    s40 = load_suite_spec(INTEGRATION / "suites" / "s40.json")
    assert s40.expanded_scenario_ids() == (
        ("S40-engagement-fmt", 1),
        ("S40-full-newgen-engagement", 1),
    )
    assert load_suite_spec(suite.suite_path("s50")).data["kind"] == "s50-fairness"
    farmtest._preflight_suite(farm, suite)


def test_composite_digest_recursively_binds_child_suite_documents(tmp_path: Path) -> None:
    path = _composite_suite_tree(tmp_path)
    before = load_suite_spec(path).digest
    child_path = path.parent / "second.json"
    child = json.loads(child_path.read_text(encoding="utf-8"))
    child["id"] = "changed-child-id"
    child_path.write_text(json.dumps(child), encoding="utf-8")

    assert load_suite_spec(path).digest != before


def test_execution_digest_binds_leaf_scenario_contents(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    path = _suite_tree(tmp_path, ["S00-smoke"])
    farm = load_farm_spec(farm_fixture.example_farm_path())
    monkeypatch.setattr(farmtest, "new_run_id", lambda: "stable-cell")
    before = farmtest._prepare_execution(
        farm,
        load_suite_spec(path),
        only=None,
        suite_run_id="stable-suite",
    )["execution_digest"]
    scenario_path = path.parent.parent / "scenarios" / "S00-smoke.json"
    scenario = json.loads(scenario_path.read_text(encoding="utf-8"))
    scenario["timeouts"]["collect_s"] += 1
    scenario_path.write_text(json.dumps(scenario), encoding="utf-8")

    after = farmtest._prepare_execution(
        farm,
        load_suite_spec(path),
        only=None,
        suite_run_id="stable-suite",
    )["execution_digest"]
    assert after != before


def test_generic_partial_execution_manifest_remains_replay_admissible(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    path = _suite_tree(tmp_path, ["S10-baseline-smoke", "S40-engagement-fmt"])
    farm = load_farm_spec(farm_fixture.example_farm_path())
    monkeypatch.setattr(farmtest, "new_run_id", lambda: "partial-cell")

    manifest = farmtest._prepare_execution(
        farm,
        load_suite_spec(path),
        only={"S40-engagement-fmt"},
        suite_run_id="partial-suite",
    )["identity"]

    assert manifest["selection"] == ["S40-engagement-fmt"]
    assert farmtest._suite_manifest_run_ids(manifest) == [
        "partial-suite",
        "partial-cell",
    ]


def test_prepared_atomic_run_never_reopens_a_changed_scenario(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    path = _suite_tree(tmp_path, ["S00-smoke"])
    suite = load_suite_spec(path)
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path / "results")
    monkeypatch.setattr(farmtest, "new_run_id", lambda: "prepared-cell")
    prepared = farmtest._prepare_execution(
        farm,
        suite,
        only=None,
        suite_run_id="prepared-suite",
    )
    scenario_path = path.parent.parent / "scenarios" / "S00-smoke.json"
    scenario_path.unlink()
    observed: list[str] = []

    def run(_farm, scenario, plan, **_kwargs):
        observed.append(scenario.digest)
        assert plan is prepared["planned_cells"][0][2]
        return {"status": "PASS"}, "cell report\n"

    monkeypatch.setattr(farmtest, "run_scenario", run)
    result, _rendered = farmtest.run_suite(
        farm,
        suite,
        suite_run_id="prepared-suite",
        _prepared=prepared,
    )

    assert observed == [prepared["planned_cells"][0][0].digest]
    assert result["execution_digest"] == prepared["execution_digest"]
    assert result["status"] == "PASS"


def test_suite_replay_authenticates_manifest_plan_and_aggregate_status(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    path = _suite_tree(tmp_path, ["S10-baseline-smoke", "S40-engagement-fmt"])
    suite = load_suite_spec(path)
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path / "results")
    run_ids = iter(("replay-cell-1", "replay-cell-2"))
    monkeypatch.setattr(farmtest, "new_run_id", lambda: next(run_ids))
    prepared = farmtest._prepare_execution(
        farm,
        suite,
        only=None,
        suite_run_id="replay-suite",
    )
    retained_by_run = {
        plan["run_id"]: {
            "plan": plan,
            "run_id": plan["run_id"],
            "scenario": scenario.data,
            "scenario_digest": scenario.digest,
        }
        for scenario, _repetition, plan in prepared["planned_cells"]
    }
    monkeypatch.setattr(
        farmtest,
        "run_scenario",
        lambda *_args, **_kwargs: ({"status": "PASS"}, "cell report\n"),
    )
    result, _rendered = farmtest.run_suite(
        farm,
        suite,
        suite_run_id="replay-suite",
        _prepared=prepared,
    )
    monkeypatch.setattr(
        farmtest,
        "load_verified_bundle",
        lambda root: retained_by_run[Path(root).name],
    )
    monkeypatch.setattr(
        farmtest,
        "replay_bundle",
        lambda _root, **_kwargs: {
            "controls": None,
            "verdict": {"status": "PASS"},
        },
    )

    suite_root = tmp_path / "results" / "suites" / "replay-suite"
    receipt = farmtest.replay_suite(suite_root)

    assert result["execution_manifest"]["suite"] == suite.data
    assert receipt["cells"] == 2
    assert receipt["suite_status"] == "PASS"
    assert receipt["status"] == "REPRODUCED"
    assert farmtest.replay_suite(
        suite_root,
        expected_execution_digest=result["execution_digest"],
    )["status"] == "REPRODUCED"

    original_suite_result = farmtest._suite_result
    loaded_root = original_suite_result(suite_root / "suite.json")
    replacement = deepcopy(loaded_root)
    replacement["execution_manifest"]["stop_on_fail"] = not replacement[
        "execution_manifest"
    ]["stop_on_fail"]
    replacement["execution_digest"] = hashlib.sha256(
        canonical_bytes(replacement["execution_manifest"])
    ).hexdigest()
    reads = 0

    def concurrent_replacement(_path: Path) -> dict:
        nonlocal reads
        reads += 1
        if reads == 1:
            return deepcopy(loaded_root)
        (suite_root / "SUITE.md").write_text(
            farmtest._render_suite(replacement), encoding="utf-8"
        )
        return deepcopy(replacement)

    monkeypatch.setattr(farmtest, "_suite_result", concurrent_replacement)
    pinned = farmtest.replay_suite(
        suite_root,
        expected_execution_digest=loaded_root["execution_digest"],
    )
    assert reads == 1
    assert pinned["execution_digest"] == loaded_root["execution_digest"]
    monkeypatch.setattr(farmtest, "_suite_result", original_suite_result)

    with pytest.raises(ReportError, match="differs from expected"):
        farmtest.replay_suite(
            suite_root,
            expected_execution_digest="0" * 64,
        )

    relocated = tmp_path / "relocated-results"
    shutil.copytree(tmp_path / "results", relocated)
    assert farmtest.replay_suite(
        relocated / "suites" / "replay-suite",
        expected_execution_digest=result["execution_digest"],
    )["status"] == "REPRODUCED"

    unexpected = suite_root / "stale-report.json"
    unexpected.write_text("{}\n", encoding="utf-8")
    with pytest.raises(ReportError, match="file set differs"):
        farmtest.replay_suite(suite_root)
    unexpected.unlink()

    suite_report = suite_root / "SUITE.md"
    expected_report = suite_report.read_text(encoding="utf-8")
    suite_report.write_text("tampered report\n", encoding="utf-8")
    with pytest.raises(ReportError, match="SUITE.md"):
        farmtest.replay_suite(suite_root)
    suite_report.write_text(expected_report, encoding="utf-8")

    retained_by_run["replay-cell-2"]["run_id"] = "aliased-run"
    with pytest.raises(ReportError, match="run id differs"):
        farmtest.replay_suite(suite_root)
    retained_by_run["replay-cell-2"]["run_id"] = "replay-cell-2"

    retained_by_run["replay-cell-2"]["scenario"]["id"] = "wrong-scenario"
    with pytest.raises(ReportError, match="scenario id differs"):
        farmtest.replay_suite(suite_root)
    retained_by_run["replay-cell-2"]["scenario"]["id"] = "S40-engagement-fmt"

    retained_by_run["replay-cell-2"]["plan"] = {
        **retained_by_run["replay-cell-2"]["plan"],
        "timeouts": {
            **retained_by_run["replay-cell-2"]["plan"]["timeouts"],
            "collect_s": 999,
        },
    }
    with pytest.raises(ReportError, match="plan digest differs"):
        farmtest.replay_suite(suite_root)


def test_nested_execution_manifest_retains_complete_suite_digest_catalogue(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    path = _composite_suite_tree(tmp_path)
    suite = load_suite_spec(path)
    farm = load_farm_spec(farm_fixture.example_farm_path())
    sequence = count(1)
    monkeypatch.setattr(farmtest, "new_run_id", lambda: f"nested-{next(sequence)}")

    prepared = farmtest._prepare_execution(
        farm,
        suite,
        only={"second"},
        suite_run_id="nested-root",
    )
    manifest = prepared["identity"]

    assert manifest["suite"] == suite.data
    assert [item["name"] for item in manifest["included_suite_digests"]] == [
        "first",
        "second",
    ]
    assert [item["name"] for item in manifest["children"]] == ["second"]
    assert farmtest._suite_manifest_run_ids(manifest)

    mismatched_selection = deepcopy(manifest)
    mismatched_selection["children"][0]["selection"] = ["invented"]
    with pytest.raises(ReportError, match="parent child selection differs"):
        farmtest._suite_manifest_run_ids(mismatched_selection)

    manifest["included_suite_digests"][1]["digest"] = "0" * 64
    with pytest.raises(ReportError, match="selected child digest differs"):
        farmtest._suite_manifest_run_ids(manifest)


def test_malformed_execution_manifests_refuse_with_report_errors(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    sequence = count(1)
    monkeypatch.setattr(farmtest, "new_run_id", lambda: f"malformed-{next(sequence)}")
    composite = farmtest._prepare_execution(
        farm,
        load_suite_spec(_composite_suite_tree(tmp_path)),
        only=None,
        suite_run_id="malformed-root",
    )["identity"]
    atomic = farmtest._prepare_execution(
        farm,
        load_suite_spec(_suite_tree(tmp_path / "atomic", ["S00-smoke"])),
        only=None,
        suite_run_id="malformed-atomic",
    )["identity"]

    malformed: list[dict] = []
    value = deepcopy(composite)
    value["included_suite_digests"] = None
    malformed.append(value)
    value = deepcopy(composite)
    value["included_suite_digests"].append("not-an-object")
    malformed.append(value)
    value = deepcopy(composite)
    value["children"] = [None]
    malformed.append(value)
    value = deepcopy(composite)
    value["children"][0]["execution_manifest"] = None
    value["children"][0]["execution_digest"] = hashlib.sha256(
        canonical_bytes(None)
    ).hexdigest()
    malformed.append(value)
    value = deepcopy(atomic)
    value["suite"]["repetitions"] = {"S00-smoke": "two"}
    malformed.append(value)
    value = deepcopy(atomic)
    value["cells"][0]["metadata"] = None
    malformed.append(value)

    for manifest in malformed:
        with pytest.raises(ReportError):
            farmtest._suite_manifest_run_ids(manifest)


def test_suite_replay_enforces_bound_stop_on_fail_prefixes() -> None:
    assert farmtest._replayed_prefix_status(
        ["PASS", "FAIL"],
        complete=False,
        stop_on_fail=True,
        label="test suite",
    ) == "FAIL"
    assert farmtest._replayed_prefix_status(
        ["PASS", "FAIL", "PASS"],
        complete=True,
        stop_on_fail=False,
        label="test suite",
    ) == "FAIL"

    with pytest.raises(ReportError, match="continued after"):
        farmtest._replayed_prefix_status(
            ["FAIL", "PASS"],
            complete=True,
            stop_on_fail=True,
            label="test suite",
        )
    with pytest.raises(ReportError, match="illegitimate stopped prefix"):
        farmtest._replayed_prefix_status(
            ["PASS", "FAIL"],
            complete=False,
            stop_on_fail=False,
            label="test suite",
        )


def test_checked_in_full_suite_preserves_every_specialized_gate_and_preflights() -> None:
    suite = load_suite_spec(INTEGRATION / "suites" / "full.json")
    farm = load_farm_spec(farm_fixture.example_farm_path())

    assert suite.data["suites"] == [
        "smoke",
        "ladder",
        "s70",
        "twobuild",
        "s90",
        "s95-disk-full",
    ]
    children = dict(suite.included_suites)
    assert children["smoke"].data["kind"] == "smoke"
    assert children["ladder"].data["kind"] == "composite"
    assert children["s70"].data["kind"] == "s70-resilience"
    assert children["twobuild"].data["performance"]["kind"] == "s80"
    assert children["s90"].expanded_scenario_ids() == (("S90-revision-skew", 1),)
    assert children["s95-disk-full"].expanded_scenario_ids() == (
        ("H5-worker-kill", 1),
        ("S95-cache-disk-full", 1),
    )
    farmtest._preflight_suite(farm, suite)


def test_full_only_routes_s40_and_s60_through_ladder_without_other_rungs(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    suite = load_suite_spec(INTEGRATION / "suites" / "full.json")
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path / "results")
    original = farmtest.run_suite
    observed: list[str] = []

    def dispatch(selected_farm, selected_suite, **kwargs):
        if selected_suite.is_composite:
            return original(selected_farm, selected_suite, **kwargs)
        observed.append(selected_suite.path.stem)
        return {"status": "PASS"}, "child report\n"

    monkeypatch.setattr(farmtest, "run_suite", dispatch)
    run_ids = (f"selected-{index}" for index in count(1))
    monkeypatch.setattr(farmtest, "new_run_id", lambda: next(run_ids))

    result, _rendered = farmtest.run_suite(
        farm,
        suite,
        only={"S40", "S60"},
        stop_on_fail=True,
        suite_run_id="full-selected",
    )

    assert observed == ["s40", "s60"]
    assert result["status"] == "PASS"
    assert len(result["groups"]) == 1
    assert result["groups"][0]["suite"] == "ladder"
    assert result["groups"][0]["selection"] == ["s40", "s60"]


def test_generic_suite_expands_repetitions_in_declared_order(tmp_path: Path) -> None:
    path = _suite_tree(tmp_path, ["S10-baseline-smoke", "S10-baseline-firefox"])
    document = json.loads(path.read_text(encoding="utf-8"))
    document["repetitions"] = {"S10-baseline-firefox": 3}
    path.write_text(json.dumps(document), encoding="utf-8")

    suite = load_suite_spec(path)

    assert suite.expanded_scenario_ids() == (
        ("S10-baseline-smoke", 1),
        ("S10-baseline-firefox", 1),
        ("S10-baseline-firefox", 2),
        ("S10-baseline-firefox", 3),
    )


def test_suite_repetitions_must_name_declared_scenarios(tmp_path: Path) -> None:
    path = _suite_tree(tmp_path, ["S10-baseline-smoke"])
    document = json.loads(path.read_text(encoding="utf-8"))
    document["repetitions"] = {"absent": 3}
    path.write_text(json.dumps(document), encoding="utf-8")

    with pytest.raises(SuiteSpecError, match="repetitions name scenarios absent"):
        load_suite_spec(path)


def test_generic_suite_runs_each_repetition_with_a_fresh_plan(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    path = _suite_tree(tmp_path, ["S10-baseline-smoke", "S10-baseline-firefox"])
    document = json.loads(path.read_text(encoding="utf-8"))
    document["repetitions"] = {"S10-baseline-firefox": 3}
    path.write_text(json.dumps(document), encoding="utf-8")
    suite = load_suite_spec(path)
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path / "results")
    run_ids = iter(f"s10-cell-{index}" for index in range(1, 5))
    monkeypatch.setattr(farmtest, "new_run_id", lambda: next(run_ids))
    observed: list[tuple[str, str]] = []

    def run(_farm, scenario, plan, **_kwargs):
        retained = json.loads(
            (
                tmp_path
                / "results"
                / "suites"
                / "s10-suite-test"
                / "suite.json"
            ).read_text()
        )
        assert retained["status"] == "RUNNING"
        assert retained["cells"][-1]["status"] == "RUNNING"
        observed.append((scenario.data["id"], plan["run_id"]))
        return {"status": "PASS"}, "cell report\n"

    monkeypatch.setattr(farmtest, "run_scenario", run)

    result, rendered = farmtest.run_suite(
        farm, suite, suite_run_id="s10-suite-test"
    )

    assert observed == [
        ("S10-baseline-smoke", "s10-cell-1"),
        ("S10-baseline-firefox", "s10-cell-2"),
        ("S10-baseline-firefox", "s10-cell-3"),
        ("S10-baseline-firefox", "s10-cell-4"),
    ]
    assert [cell["repetition"] for cell in result["cells"]] == [1, 1, 2, 3]
    assert len({cell["run_id"] for cell in result["cells"]}) == 4
    assert result["status"] == "PASS"
    assert "| `S10-baseline-firefox` | 3 |" in rendered


def test_s50_suite_persists_matched_fairness_result_offline(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    suite = load_suite_spec(INTEGRATION / "suites" / "s50.json")
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path / "results")
    run_ids = iter(f"s50-cell-{index}" for index in range(1, 6))
    monkeypatch.setattr(farmtest, "new_run_id", lambda: next(run_ids))
    prepared = farmtest._prepare_execution(
        farm,
        suite,
        only=None,
        suite_run_id="s50-suite-test",
    )
    planned = {
        plan["run_id"]: (scenario, plan)
        for scenario, _repetition, plan in prepared["planned_cells"]
    }
    monkeypatch.setattr(
        farmtest,
        "run_scenario",
        lambda *_args, **_kwargs: ({"status": "PASS"}, "cell report\n"),
    )

    def bundle_for(path: Path) -> dict:
        scenario, plan = planned[path.name]
        return {
            "plan": plan,
            "run_id": plan["run_id"],
            "scenario": scenario.data,
            "scenario_digest": scenario.digest,
            "observations": {
                "client_turns": {
                        "A": {
                            "C1": {"exact_objects": 100, "jobs": 100, "wall_ms": 1000},
                            "C2": {"exact_objects": 100, "jobs": 100, "wall_ms": 1000},
                        }
                    },
                    "turns": {"A": {"exact_objects": 200, "jobs": 200, "wall_ms": 1000}},
                },
                "rows": [
                    {"client_instance": client, "exact": True}
                    for client in ("C1", "C2")
                    for _ in range(100)
                ],
        }

    monkeypatch.setattr(farmtest, "load_verified_bundle", bundle_for)
    result, _rendered = farmtest.run_suite(
        farm,
        suite,
        suite_run_id="s50-suite-test",
        _prepared=prepared,
    )
    output = tmp_path / "results" / "suites" / "s50-suite-test"
    assert result["status"] == "PASS"
    assert result["fairness"]["status"] == "PASS"
    assert (output / "fairness.json").is_file()
    assert (output / "FAIRNESS.md").is_file()
    monkeypatch.setattr(
        farmtest,
        "replay_bundle",
        lambda _root, **_kwargs: {
            "controls": None,
            "verdict": {"status": "PASS"},
        },
    )
    replayed = farmtest.replay_suite(output)
    assert replayed["fairness"] == result["fairness"]
    assert replayed["suite_status"] == "PASS"

    partial = deepcopy(result)
    partial["cells"].pop()
    partial["execution_manifest"]["cells"].pop()
    partial["execution_digest"] = hashlib.sha256(
        canonical_bytes(partial["execution_manifest"])
    ).hexdigest()
    (output / "suite.json").write_bytes(canonical_bytes(partial))
    (output / "SUITE.md").write_text(
        farmtest._render_suite(partial), encoding="utf-8"
    )
    with pytest.raises(ReportError, match="specialized suite execution"):
        farmtest.replay_suite(output)

    (output / "suite.json").write_bytes(canonical_bytes(result))
    (output / "SUITE.md").write_text(
        farmtest._render_suite(result), encoding="utf-8"
    )
    (output / "fairness.json").write_text("{}\n", encoding="utf-8")
    with pytest.raises(ReportError, match="fairness.json"):
        farmtest.replay_suite(output)


@pytest.mark.parametrize("kind", ["controls", "smoke"])
def test_exact_harness_suites_refuse_repetition_overrides(
    tmp_path: Path, kind: str
) -> None:
    path = _control_suite_tree(tmp_path, kind=kind)
    document = json.loads(path.read_text(encoding="utf-8"))
    document["repetitions"] = {document["scenarios"][0]: 2}
    path.write_text(json.dumps(document), encoding="utf-8")

    with pytest.raises(SuiteSpecError, match="cannot override exact repetitions"):
        load_suite_spec(path)


@pytest.mark.parametrize("kind", ["controls", "smoke"])
def test_harness_gate_suite_requires_exact_control_order(
    tmp_path: Path, kind: str
) -> None:
    path = _control_suite_tree(tmp_path, kind=kind)
    document = json.loads(path.read_text())
    document["scenarios"][0], document["scenarios"][1] = (
        document["scenarios"][1],
        document["scenarios"][0],
    )
    path.write_text(json.dumps(document), encoding="utf-8")

    with pytest.raises(SuiteSpecError, match="must be exactly.*in order"):
        load_suite_spec(path)


def test_control_suite_requires_exact_single_control_binding(tmp_path: Path) -> None:
    path = _control_suite_tree(tmp_path)
    scenario_path = path.parent.parent / "scenarios" / "H3-mutant-scheduler.json"
    scenario = json.loads(scenario_path.read_text())
    scenario["controls"] = ["H2", "H3"]
    scenario_path.write_text(json.dumps(scenario), encoding="utf-8")

    with pytest.raises(SuiteSpecError, match="must declare exactly"):
        load_suite_spec(path)


@pytest.mark.parametrize("kind", ["controls", "smoke"])
def test_harness_gate_suite_refuses_partial_only_before_running(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, kind: str
) -> None:
    suite = load_suite_spec(_control_suite_tree(tmp_path, kind=kind))
    farm = load_farm_spec(farm_fixture.example_farm_path())
    monkeypatch.setattr(
        farmtest,
        "run_scenario",
        lambda *_args, **_kwargs: pytest.fail("must not run"),
    )

    with pytest.raises(SuiteSpecError, match=f"partial {kind} suite"):
        farmtest.run_suite(farm, suite, only={"H2-client-kill-switch"})


def test_checked_in_twobuild_suite_authenticates_all_four_arms() -> None:
    suite = load_suite_spec(INTEGRATION / "suites" / "twobuild.json")
    farm = load_farm_spec(farm_fixture.example_farm_path())

    for arm, scenario_id in suite.data["performance"]["arms"].items():
        scenario = load_scenario_spec(suite.scenario_path(scenario_id), farm)
        validate_s80_arm_scenario(arm, scenario.data)


def test_checked_in_h1_uses_an_authorized_stale_runtime_as_the_wrong_hash() -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "H1-role-hash-refusal.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="h1-dry-plan")

    assert scenario.data["controls"] == ["H1"]
    assert scenario.data["images"] == {"wrong": "p50s2-5b2e5801"}
    assert {
        instance["image"]["label"] for instance in plan["topology"]["instances"]
    } == {"p50s2-5b2e5801"}
    assert {instance["sha256"] for instance in plan["topology"]["instances"]} == {
        item["sha256"] for item in farm.data["authority"]["role_stores"]["50"].values()
    }


def test_checked_in_h5_kills_one_of_two_workers_after_a_bounded_job_trigger() -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "H5-worker-kill.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="h5-dry-plan")

    assert scenario.data["controls"] == ["H5"]
    assert scenario.data["timeline"] == [
        {"action": "kill -9", "instance": "F1", "trigger": "job 12"}
    ]
    assert {
        instance["name"]
        for instance in plan["topology"]["instances"]
        if instance["role"] == "F"
    } == {"F1", "F2"}


def test_s80_suite_spec_refuses_arm_scenario_mismatch(tmp_path: Path) -> None:
    path = _s80_suite_tree(tmp_path)
    document = json.loads(path.read_text(encoding="utf-8"))
    document["scenarios"].pop()
    path.write_text(json.dumps(document), encoding="utf-8")

    with pytest.raises(SuiteSpecError, match="exactly match"):
        load_suite_spec(path)


def test_s80_refuses_noncomparable_arm_scenarios_before_running(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    path = _s80_suite_tree(tmp_path)
    scenario_path = path.parent.parent / "scenarios" / "S80-zstd-route.json"
    scenario = json.loads(scenario_path.read_text(encoding="utf-8"))
    scenario["workload"]["jobs"] += 1
    scenario_path.write_text(json.dumps(scenario), encoding="utf-8")
    suite = load_suite_spec(path)
    farm = load_farm_spec(farm_fixture.example_farm_path())
    monkeypatch.setattr(
        farmtest,
        "run_scenario",
        lambda *_args, **_kwargs: pytest.fail("must not run"),
    )

    with pytest.raises(S80EvidenceError, match="differ outside profile/id/reuse"):
        farmtest.run_suite(farm, suite)


def test_s80_runs_twelve_fresh_cells_and_retains_score(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    suite = load_suite_spec(_s80_suite_tree(tmp_path))
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path / "results")
    run_ids = iter(f"s80-cell-{index}" for index in range(1, 13))
    monkeypatch.setattr(farmtest, "new_run_id", lambda: next(run_ids))
    prepared = farmtest._prepare_execution(
        farm,
        suite,
        only=None,
        suite_run_id="s80-suite",
    )
    planned = {
        plan["run_id"]: (scenario, plan)
        for _sequence, _arm, _repetition, scenario, plan in prepared[
            "planned_cells"
        ]
    }
    scenario_overrides: dict[str, dict] = {}
    plan_overrides: dict[str, dict] = {}
    observed: list[tuple[str, str]] = []

    def run(_farm, scenario, plan, **_kwargs):
        profile = scenario.data["instances"][0]["env"]["ICECC_P50_PROFILE"]
        observed.append((profile, plan["run_id"]))
        return {"status": "PASS"}, "cell report\n"

    def bundle(root: Path):
        run_id = root.name
        scenario, plan = planned[run_id]
        scenario_data = scenario_overrides.get(run_id, scenario.data)
        plan_data = plan_overrides.get(run_id, plan)
        sequence = int(run_id.rsplit("-", 1)[1])
        arm, _repetition = S80_ORDER[sequence - 1]
        profile = None if arm == "legacy" else arm
        outcome = "none" if arm == "legacy" else "committed"
        rows = [
            {
                "exact": True,
                "job_id": f"{run_id}-{index}",
                "session_outcome": outcome,
                "tail_profile": profile,
            }
            for index in range(2)
        ]
        arm_wall = {"P29V1": 100, "ZSTD_TU": 200, "ZSTD_ROUTE": 300, "legacy": 400}[arm]
        turns = {
            turn: {
                "c_to_f_bytes": arm_wall * 100,
                "exact_objects": 1,
                "f_to_c_bytes": 10,
                "jobs": 1,
                "job_wall_p95_ms": arm_wall,
                "job_wall_p99_ms": arm_wall,
                "source_mutex_records": 0 if arm == "legacy" else 1,
                "source_mutex_service_ns": 0 if arm == "legacy" else 1,
                "source_mutex_wait_ns": 0,
                "wall_ms": arm_wall + (1 if turn == "B" else 0),
            }
            for turn in ("A", "B")
        }
        observations = {"turns": turns}
        if arm == "legacy":
            observations["legacy_wire"] = {"record_count": len(rows)}
        return {
            "observations": observations,
            "plan": plan_data,
            "rows": rows,
            "run_id": run_id,
            "scenario": scenario_data,
            "scenario_digest": hashlib.sha256(
                canonical_bytes(scenario_data)
            ).hexdigest(),
        }

    monkeypatch.setattr(farmtest, "run_scenario", run)
    monkeypatch.setattr(farmtest, "load_verified_bundle", bundle)

    result, rendered = farmtest.run_suite(
        farm,
        suite,
        suite_run_id="s80-suite",
        _prepared=prepared,
    )

    assert result["status"] == "PASS"
    assert len(observed) == 12
    assert [profile for profile, _run_id in observed] == [
        "OFF" if arm == "legacy" else arm for arm, _repetition in S80_ORDER
    ]
    assert len({run_id for _profile, run_id in observed}) == 12
    assert result["performance"]["status"] == "PASS"
    assert "S80-p29v1" in rendered
    output = tmp_path / "results" / "suites" / "s80-suite"
    assert (output / "performance.json").is_file()
    assert "10 Gbit/s" in (output / "PERFORMANCE.md").read_text()
    monkeypatch.setattr(
        farmtest,
        "replay_bundle",
        lambda _root, **_kwargs: {
            "controls": None,
            "verdict": {"status": "PASS"},
        },
    )
    replayed = farmtest.replay_suite(output)
    assert replayed["performance"] == result["performance"]
    assert replayed["suite_status"] == "PASS"

    incomparable = deepcopy(result)
    for identity in incomparable["execution_manifest"]["cells"]:
        if identity["scenario_id"] != "S80-zstd-route":
            continue
        run_id = identity["run_id"]
        scenario, plan = planned[run_id]
        changed_scenario = deepcopy(scenario.data)
        changed_scenario["workload"]["jobs"] += 1
        changed_digest = hashlib.sha256(
            canonical_bytes(changed_scenario)
        ).hexdigest()
        changed_plan = deepcopy(plan)
        changed_plan["scenario_digest"] = changed_digest
        scenario_overrides[run_id] = changed_scenario
        plan_overrides[run_id] = changed_plan
        identity["scenario_digest"] = changed_digest
        identity["plan_digest"] = hashlib.sha256(
            canonical_bytes(changed_plan)
        ).hexdigest()
    incomparable["execution_digest"] = hashlib.sha256(
        canonical_bytes(incomparable["execution_manifest"])
    ).hexdigest()
    (output / "suite.json").write_bytes(canonical_bytes(incomparable))
    (output / "SUITE.md").write_text(
        farmtest._render_suite(incomparable), encoding="utf-8"
    )
    with pytest.raises(ReportError, match="differ outside profile/id/reuse"):
        farmtest.replay_suite(output)


def test_s80_refuses_partial_only_selection_before_running(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    suite = load_suite_spec(_s80_suite_tree(tmp_path))
    farm = load_farm_spec(farm_fixture.example_farm_path())
    monkeypatch.setattr(
        farmtest,
        "run_scenario",
        lambda *_args, **_kwargs: pytest.fail("must not run"),
    )

    with pytest.raises(SuiteSpecError, match="incomplete S80 matrix"):
        farmtest.run_suite(farm, suite, only={"S80-p29v1"})


def test_s50_refuses_a_partial_selection_that_contains_every_scored_cell(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    suite = load_suite_spec(INTEGRATION / "suites" / "s50.json")
    farm = load_farm_spec(farm_fixture.example_farm_path())
    monkeypatch.setattr(
        farmtest,
        "run_scenario",
        lambda *_args, **_kwargs: pytest.fail("must not run"),
    )

    with pytest.raises(SuiteSpecError, match="partial s50-fairness suite"):
        farmtest.run_suite(
            farm,
            suite,
            only={
                "S50-all-legacy-control",
                "S50-mixed-pool-1of9",
                "S50-mixed-pool-5of9",
                "S50-mixed-pool-8of9",
            },
        )


def test_suite_runs_in_fixed_order_and_stops_on_verdict_failure(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    ids = ["S00-smoke", "S10-baseline-smoke", "S40-engagement-fmt"]
    suite = load_suite_spec(_suite_tree(tmp_path, ids))
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path / "results")
    run_ids = iter(("cell-1", "cell-2", "cell-3"))
    monkeypatch.setattr(farmtest, "new_run_id", lambda: next(run_ids))
    observed: list[str] = []

    def run(_farm, scenario, _plan, **_kwargs):
        observed.append(scenario.data["id"])
        status = "FAIL" if scenario.data["id"] == "S10-baseline-smoke" else "PASS"
        return {"status": status}, "cell report\n"

    monkeypatch.setattr(farmtest, "run_scenario", run)

    result, rendered = farmtest.run_suite(
        farm,
        suite,
        stop_on_fail=True,
        suite_run_id="suite-test",
    )

    assert observed == ids[:2]
    assert result["status"] == "FAIL"
    assert result["execution_manifest"]["stop_on_fail"] is True
    assert [cell["status"] for cell in result["cells"]] == ["PASS", "FAIL"]
    assert "S40-engagement-fmt" not in rendered
    retained = json.loads(
        (tmp_path / "results" / "suites" / "suite-test" / "suite.json").read_text()
    )
    assert retained == result


def test_suite_uses_control_verdict_as_the_meta_outcome(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    path = _suite_tree(tmp_path, ["S00-smoke"])
    scenario_path = path.parent.parent / "scenarios" / "S00-smoke.json"
    scenario = json.loads(scenario_path.read_text(encoding="utf-8"))
    scenario["controls"] = ["H2"]
    scenario_path.write_text(json.dumps(scenario), encoding="utf-8")
    suite = load_suite_spec(path)
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path / "results")
    monkeypatch.setattr(farmtest, "new_run_id", lambda: "control-cell")
    monkeypatch.setattr(
        farmtest,
        "run_scenario",
        lambda *_args, **_kwargs: ({"status": "FAIL"}, "product failed\n"),
    )
    monkeypatch.setattr(
        farmtest,
        "verify_control_bundle",
        lambda _root: {"status": "PASS"},
    )

    result, _rendered = farmtest.run_suite(
        farm,
        suite,
        suite_run_id="control-suite",
    )

    assert result["status"] == "PASS"
    assert result["cells"][0]["product_status"] == "FAIL"
    assert result["cells"][0]["control_status"] == "PASS"


def test_suite_only_refuses_unknown_ids_before_running(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    suite = load_suite_spec(_suite_tree(tmp_path, ["S00-smoke"]))
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path / "results")
    monkeypatch.setattr(
        farmtest,
        "run_scenario",
        lambda *_args, **_kwargs: pytest.fail("must not run"),
    )

    with pytest.raises(SuiteSpecError, match="absent from suite"):
        farmtest.run_suite(
            farm,
            suite,
            only={"absent"},
            suite_run_id="suite-test",
        )


def test_suite_resolves_every_plan_before_the_first_cell_runs(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    ids = ["S00-smoke", "S10-baseline-smoke"]
    suite = load_suite_spec(_suite_tree(tmp_path, ids))
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path / "results")
    original = farmtest.build_plan

    def plan(selected_farm, scenario, **kwargs):
        if scenario.data["id"] == "S10-baseline-smoke":
            raise farmtest.PlanError("injected later plan refusal")
        return original(selected_farm, scenario, **kwargs)

    monkeypatch.setattr(farmtest, "build_plan", plan)
    monkeypatch.setattr(
        farmtest,
        "run_scenario",
        lambda *_args, **_kwargs: pytest.fail("must not run"),
    )
    run_ids = iter(("cell-1", "cell-2"))
    monkeypatch.setattr(farmtest, "new_run_id", lambda: next(run_ids))

    with pytest.raises(farmtest.PlanError, match="later plan refusal"):
        farmtest.run_suite(farm, suite, suite_run_id="suite-test")

    assert not (tmp_path / "results" / "suites" / "suite-test").exists()


def test_composite_preflights_every_child_before_creating_results_or_running(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    suite = load_suite_spec(_composite_suite_tree(tmp_path))
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path / "results")
    original = farmtest.build_plan

    def plan(selected_farm, scenario, **kwargs):
        if scenario.data["id"] == "S10-baseline-smoke":
            raise farmtest.PlanError("injected later child refusal")
        return original(selected_farm, scenario, **kwargs)

    monkeypatch.setattr(farmtest, "build_plan", plan)
    monkeypatch.setattr(
        farmtest,
        "run_scenario",
        lambda *_args, **_kwargs: pytest.fail("must not run"),
    )

    with pytest.raises(farmtest.PlanError, match="later child refusal"):
        farmtest.run_suite(farm, suite, suite_run_id="composite-test")

    assert not (tmp_path / "results" / "suites" / "composite-test").exists()


def test_composite_uses_prepared_later_leaf_after_first_child_mutates_file(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    suite = load_suite_spec(_composite_suite_tree(tmp_path))
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path / "results")
    later_path = suite.path.parent.parent / "scenarios" / "S10-baseline-smoke.json"
    original_later = json.loads(later_path.read_text(encoding="utf-8"))
    original_build_plan = farmtest.build_plan
    planned: list[str] = []
    observed: list[tuple[str, int]] = []

    def build_plan(selected_farm, scenario, **kwargs):
        planned.append(scenario.data["id"])
        return original_build_plan(selected_farm, scenario, **kwargs)

    def run(_farm, scenario, _plan, **_kwargs):
        observed.append((scenario.data["id"], scenario.data["timeouts"]["collect_s"]))
        if scenario.data["id"] == "S00-smoke":
            changed = json.loads(json.dumps(original_later))
            changed["timeouts"]["collect_s"] += 99
            later_path.write_text(json.dumps(changed), encoding="utf-8")
        return {"status": "PASS"}, "cell report\n"

    monkeypatch.setattr(farmtest, "build_plan", build_plan)
    monkeypatch.setattr(farmtest, "run_scenario", run)
    sequence = count(1)
    monkeypatch.setattr(farmtest, "new_run_id", lambda: f"race-{next(sequence)}")

    result, _rendered = farmtest.run_suite(
        farm,
        suite,
        suite_run_id="race-root",
    )

    assert result["status"] == "PASS"
    assert planned == ["S00-smoke", "S10-baseline-smoke"]
    assert observed == [
        ("S00-smoke", 600),
        ("S10-baseline-smoke", original_later["timeouts"]["collect_s"]),
    ]


def test_prepared_tree_refuses_duplicate_run_ids_globally_before_results(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    suite = load_suite_spec(_composite_suite_tree(tmp_path))
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path / "results")
    monkeypatch.setattr(farmtest, "new_run_id", lambda: "duplicate-run")
    monkeypatch.setattr(
        farmtest,
        "run_scenario",
        lambda *_args, **_kwargs: pytest.fail("must not run"),
    )

    with pytest.raises(SuiteSpecError, match="not globally unique"):
        farmtest.run_suite(farm, suite, suite_run_id="composite-test")

    assert not (tmp_path / "results").exists()


def test_composite_preserves_child_gate_results_and_continues_in_order(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    suite = load_suite_spec(_composite_suite_tree(tmp_path))
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path / "results")
    original = farmtest.run_suite
    observed: list[str] = []

    def dispatch(selected_farm, selected_suite, **kwargs):
        if selected_suite.is_composite:
            return original(selected_farm, selected_suite, **kwargs)
        observed.append(selected_suite.path.stem)
        status = "FAIL" if selected_suite.path.stem == "first" else "PASS"
        gate = {"schema": "test-gate-v1", "status": status}
        return {"performance": gate, "status": status}, "child report\n"

    monkeypatch.setattr(farmtest, "run_suite", dispatch)
    run_ids = (f"child-{index}" for index in count(1))
    monkeypatch.setattr(farmtest, "new_run_id", lambda: next(run_ids))

    result, rendered = farmtest.run_suite(
        farm,
        suite,
        stop_on_fail=False,
        suite_run_id="composite-test",
    )

    assert observed == ["first", "second"]
    assert result["status"] == "FAIL"
    assert [group["status"] for group in result["groups"]] == ["FAIL", "PASS"]
    assert result["groups"][0]["performance"]["status"] == "FAIL"
    assert "`first`" in rendered
    assert "`second-declared-id`" in rendered


def test_composite_suite_replay_recurses_through_child_results(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    suite = load_suite_spec(_composite_suite_tree(tmp_path))
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path / "results")
    sequence = count(1)
    monkeypatch.setattr(farmtest, "new_run_id", lambda: f"recurse-{next(sequence)}")
    prepared = farmtest._prepare_execution(
        farm,
        suite,
        only=None,
        suite_run_id="recurse-root",
    )

    planned: dict[str, tuple] = {}

    def collect_plans(node: dict) -> None:
        for _name, child, _selection in node["children"]:
            collect_plans(child)
        for scenario, _repetition, plan in node["planned_cells"]:
            planned[plan["run_id"]] = (scenario, plan)

    collect_plans(prepared)
    monkeypatch.setattr(
        farmtest,
        "run_scenario",
        lambda *_args, **_kwargs: ({"status": "PASS"}, "cell report\n"),
    )
    result, _rendered = farmtest.run_suite(
        farm,
        suite,
        suite_run_id="recurse-root",
        _prepared=prepared,
    )

    def retained(root: Path) -> dict:
        scenario, plan = planned[root.name]
        return {
            "plan": plan,
            "run_id": plan["run_id"],
            "scenario": scenario.data,
            "scenario_digest": scenario.digest,
        }

    monkeypatch.setattr(farmtest, "load_verified_bundle", retained)
    monkeypatch.setattr(
        farmtest,
        "replay_bundle",
        lambda _root, **_kwargs: {
            "controls": None,
            "verdict": {"status": "PASS"},
        },
    )

    replayed = farmtest.replay_suite(
        tmp_path / "results" / "suites" / "recurse-root"
    )

    assert result["status"] == "PASS"
    assert replayed["groups"] == 2
    assert replayed["suite_status"] == "PASS"

    root_path = tmp_path / "results" / "suites" / "recurse-root"
    root_result = json.loads((root_path / "suite.json").read_text(encoding="utf-8"))
    empty = deepcopy(root_result)
    empty["groups"] = []
    empty["execution_manifest"]["children"] = []
    empty["execution_digest"] = hashlib.sha256(
        canonical_bytes(empty["execution_manifest"])
    ).hexdigest()
    (root_path / "suite.json").write_bytes(canonical_bytes(empty))
    (root_path / "SUITE.md").write_text(
        farmtest._render_suite(empty), encoding="utf-8"
    )
    with pytest.raises(ReportError, match="selects no child"):
        farmtest.replay_suite(root_path)
    (root_path / "suite.json").write_bytes(canonical_bytes(root_result))
    (root_path / "SUITE.md").write_text(
        farmtest._render_suite(root_result), encoding="utf-8"
    )

    child_path = (
        tmp_path
        / "results"
        / "suites"
        / result["groups"][0]["run_id"]
        / "suite.json"
    )
    child_result = json.loads(child_path.read_text(encoding="utf-8"))
    child_result["status"] = "FAIL"
    child_path.write_text(json.dumps(child_result), encoding="utf-8")
    with pytest.raises(ReportError, match="aggregate status does not replay"):
        farmtest.replay_suite(
            tmp_path / "results" / "suites" / "recurse-root"
        )


def test_composite_stop_and_only_operate_at_case_insensitive_suite_boundaries(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    suite = load_suite_spec(_composite_suite_tree(tmp_path))
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path / "results")
    original = farmtest.run_suite
    observed: list[str] = []

    def dispatch(selected_farm, selected_suite, **kwargs):
        if selected_suite.is_composite:
            return original(selected_farm, selected_suite, **kwargs)
        observed.append(selected_suite.path.stem)
        return {"status": "FAIL"}, "child report\n"

    monkeypatch.setattr(farmtest, "run_suite", dispatch)
    run_ids = (f"selected-child-{index}" for index in count(1))
    monkeypatch.setattr(farmtest, "new_run_id", lambda: next(run_ids))

    result, rendered = farmtest.run_suite(
        farm,
        suite,
        only={"SECOND"},
        stop_on_fail=True,
        suite_run_id="composite-selected",
    )

    assert observed == ["second"]
    assert result["status"] == "FAIL"
    assert result["groups"][0]["suite"] == "second"
    assert "Execution digest: `" in rendered
    assert "Coverage: **Selected**" in rendered
    assert "Selection: `second`" in rendered
    assert "| # | Included suite | Declared id | Selection |" in rendered

    with pytest.raises(SuiteSpecError, match="included suites absent"):
        farmtest.run_suite(
            farm,
            suite,
            only={"S10-baseline-smoke"},
            suite_run_id="composite-unknown",
        )


def test_composite_selector_deduplicates_ancestor_and_refuses_ambiguous_leaf(
    tmp_path: Path,
) -> None:
    path = _composite_suite_tree(tmp_path)
    suites = path.parent
    (suites / "leaf.json").write_text(
        json.dumps(
            {
                "id": "leaf-id",
                "scenarios": ["S00-smoke"],
                "schema": "icefarm-suite-v1",
            }
        ),
        encoding="utf-8",
    )
    composite_child = {
        "id": "first-id",
        "kind": "composite",
        "schema": "icefarm-suite-v1",
        "suites": ["leaf"],
    }
    (suites / "first.json").write_text(
        json.dumps(composite_child), encoding="utf-8"
    )
    suite = load_suite_spec(path)

    selected = farmtest._selected_composite_children(suite, {"first", "leaf"})
    assert [(name, child_only) for name, _child, child_only in selected] == [
        ("first", None)
    ]

    composite_child["id"] = "second-id"
    (suites / "second.json").write_text(
        json.dumps(composite_child), encoding="utf-8"
    )
    suite = load_suite_spec(path)
    with pytest.raises(SuiteSpecError, match="ambiguous included suites"):
        farmtest._selected_composite_children(suite, {"leaf"})
