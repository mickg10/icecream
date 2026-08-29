from __future__ import annotations

import json
import subprocess
from pathlib import Path

import pytest

import s8_campaign_driver as driver


def _runner_factory(fail_producer: int | None = None):
    calls: list[tuple[str, list[str]]] = []
    planned_result_dirs: list[Path] = []

    def run(command: dict[str, object], _cwd: Path, stdout: Path, stderr: Path) -> int:
        stage = str(command["stage"])
        argv = [str(item) for item in command["argv"]]  # type: ignore[index]
        calls.append((stage, argv))
        stdout.parent.mkdir(parents=True, exist_ok=True)
        stdout.write_text(stdout.read_text() + stage + "\n" if stdout.exists() else stage + "\n")
        stderr.touch()
        if stage.startswith("predictive_plan"):
            out = Path(argv[argv.index("--out") + 1])
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text("{}\n")
            planned_result_dirs.append(Path(argv[argv.index("--result-dir") + 1]))
        elif stage == "predictive_producer":
            producer_calls = sum(call[0] == stage for call in calls)
            if fail_producer is not None and producer_calls == fail_producer:
                return 17
            if "--output-dir" in argv:
                results = [Path(argv[argv.index("--output-dir") + 1])]
            else:
                # Paired full producer writes both plan-bound result paths.
                results = planned_result_dirs[-2:]
            for result in results:
                result.mkdir(parents=True)
                (result / "predictive_sim.jsonl").write_text(
                    '{"cumulative":{"channel_bytes":10,"elapsed_ns":1}}\n')
                (result / "producer_manifest.json").write_text("{}\n")
        return 0

    return run, calls


def _kwargs(tmp_path: Path, **extra: object) -> dict[str, object]:
    repo = tmp_path / "repo"
    if not (repo / ".git").is_dir():
        repo.mkdir()
        (repo / "source-marker").write_text("initial\n")
        subprocess.run(["git", "init", "-q", str(repo)], check=True)
        subprocess.run(["git", "-C", str(repo), "config", "user.email",
                        "test@example.invalid"], check=True)
        subprocess.run(["git", "-C", str(repo), "config", "user.name",
                        "S8 test"], check=True)
        subprocess.run(["git", "-C", str(repo), "add", "source-marker"], check=True)
        subprocess.run(["git", "-C", str(repo), "commit", "-qm", "initial"], check=True)
    values: dict[str, object] = {
        "output_root": tmp_path / "experiments",
        "repo": repo,
        "corpus": "DuckDB",
        "depth": "100",
        "source_manifest": str(tmp_path / "source" / "{profile}.txt"),
        "source_root": str(tmp_path / "source"),
        "matrix_audit": tmp_path / "matrix.json",
        "engine_manifest_template": str(tmp_path / "engine-{profile}-{regime}.json"),
        "product_build_root": tmp_path / "product",
    }
    values.update(extra)
    return values


def test_predictive_campaign_retains_all_cells_commands_hashes_and_summary(tmp_path: Path) -> None:
    runner, calls = _runner_factory()
    campaign = driver.run_campaign(**_kwargs(tmp_path), command_runner=runner,
                                   timestamp="20260829T120000Z")
    summary = json.loads((campaign / "summary.json").read_text())
    assert summary["status"] == "PASS"
    assert summary["expected_cells"] == 16
    assert summary["counts"]["PASS"] == 16
    assert len(calls) == 32
    states = list((campaign / "cells").glob("*/status.json"))
    assert len(states) == 16
    for path in states:
        state = json.loads(path.read_text())
        assert state["status"] == "PASS"
        commands = json.loads((path.parent / "attempt-001" / "commands.json").read_text())
        assert set(commands) == {"predictive_plan", "predictive_producer", "live", "comparison"}
        assert commands["predictive_plan"][0]["sha256"]
        assert commands["live"]["prepare"]["executable"] is False
        assert len(commands["comparison"]) == 1
        assert commands["comparison"][0]["reason"] == "requires authenticated live curve"
        comparison = commands["comparison"][0]
        assert comparison["argv"][comparison["argv"].index("--live-manifest") + 1].endswith(
            "live_curve_manifest.json")
        assert "/live-output/icecream/" in comparison["argv"][
            comparison["argv"].index("--live-manifest") + 1]
        live_argv = commands["live"]["run"]["argv"]
        assert live_argv[live_argv.index("--timestamp") + 1] == "20260829T120000Z"
        assert state["result"]["artifacts"]


def test_campaign_failure_is_honest_and_resume_keeps_old_attempt(tmp_path: Path) -> None:
    failing_runner, _ = _runner_factory(fail_producer=4)
    kwargs = _kwargs(tmp_path)
    campaign = driver.run_campaign(**kwargs, command_runner=failing_runner,
                                   timestamp="20260829T120001Z")
    summary = json.loads((campaign / "summary.json").read_text())
    assert summary["status"] == "PARTIAL_FAILURE"
    assert summary["counts"]["PASS"] == 15
    assert summary["counts"]["FAIL"] == 1
    failed = next(row for row in summary["cells"] if row["status"] == "FAIL")

    # A normal resume does not silently rerun or relabel a failed cell.
    no_retry, calls = _runner_factory()
    resumed = driver.run_campaign(**kwargs, resume=campaign, command_runner=no_retry)
    assert resumed == campaign
    assert calls == []
    assert json.loads((campaign / "summary.json").read_text())["counts"]["FAIL"] == 1

    retry, calls = _runner_factory()
    driver.run_campaign(**kwargs, resume=campaign, retry_failed=True, command_runner=retry)
    summary = json.loads((campaign / "summary.json").read_text())
    assert summary["status"] == "PASS"
    assert summary["counts"]["PASS"] == 16
    assert summary["counts"]["FAIL"] == 0
    state_path = next(path for path in (campaign / "cells").glob("*/status.json")
                      if json.loads(path.read_text())["cell"] == failed["cell"])
    assert (state_path.parent / "attempt-001").is_dir()
    assert (state_path.parent / "attempt-002").is_dir()
    state = json.loads(state_path.read_text())
    assert any(item["status"] == "FAIL" and item["attempt"] == 1
               for item in state["history"])
    assert any(stage == "predictive_producer" for stage, _ in calls)
    first_commands = json.loads((state_path.parent / "attempt-001" / "commands.json").read_text())
    second_commands = json.loads((state_path.parent / "attempt-002" / "commands.json").read_text())
    assert first_commands["predictive_producer"]["argv"] != second_commands["predictive_producer"]["argv"]
    assert first_commands["predictive_producer"]["sha256"] != second_commands["predictive_producer"]["sha256"]


def test_plan_only_stages_live_and_comparison_without_running_predictive(tmp_path: Path) -> None:
    campaign = driver.run_campaign(**_kwargs(tmp_path), execute=False,
                                   timestamp="20260829T120002Z")
    summary = json.loads((campaign / "summary.json").read_text())
    assert summary["status"] == "PARTIAL_STAGED"
    assert summary["counts"]["STAGED"] == 16
    assert not list(campaign.glob("cells/*/attempt-001/*log"))
    assert len(list(campaign.glob("cells/*/attempt-001/result.json"))) == 16


def test_new_campaign_uses_suffix_on_same_second_without_overwrite(tmp_path: Path) -> None:
    first = driver._new_campaign_root(tmp_path, "20260829T120003Z")
    second = driver._new_campaign_root(tmp_path, "20260829T120003Z")
    assert first != second
    assert first.name == "s8-campaign-20260829T120003Z"
    assert second.name == "s8-campaign-20260829T120003Z-01"


def test_resume_rejects_changed_declaration(tmp_path: Path) -> None:
    runner, _ = _runner_factory()
    campaign = driver.run_campaign(**_kwargs(tmp_path), command_runner=runner,
                                   timestamp="20260829T120004Z")
    with pytest.raises(driver.CampaignError, match="configuration_mismatch"):
        driver.run_campaign(**_kwargs(tmp_path, depth="200"), resume=campaign,
                             command_runner=runner)


def test_resume_rejects_changed_source_identity(tmp_path: Path) -> None:
    runner, _ = _runner_factory()
    kwargs = _kwargs(tmp_path)
    campaign = driver.run_campaign(**kwargs, command_runner=runner,
                                   timestamp="20260829T120006Z")
    metadata_path = campaign / "campaign.json"
    metadata = json.loads(metadata_path.read_text())
    metadata["git"]["head"] = "0" * 40
    metadata_path.write_bytes(driver.canonical(metadata))
    with pytest.raises(driver.CampaignError, match="source_identity_mismatch"):
        driver.run_campaign(**kwargs, resume=campaign, command_runner=runner)


def test_resume_rejects_changed_tracked_source_bytes(tmp_path: Path) -> None:
    runner, _ = _runner_factory()
    kwargs = _kwargs(tmp_path)
    campaign = driver.run_campaign(**kwargs, command_runner=runner,
                                   timestamp="20260829T120007Z")
    repo = Path(kwargs["repo"])
    (repo / "source-marker").write_text("changed without a commit\n")
    with pytest.raises(driver.CampaignError, match="source_identity_mismatch"):
        driver.run_campaign(**kwargs, resume=campaign, command_runner=runner)


def test_full_campaign_stages_and_retains_two_plan_bound_segments(tmp_path: Path) -> None:
    runner, calls = _runner_factory()
    campaign = driver.run_campaign(**_kwargs(tmp_path, depth="full"), command_runner=runner,
                                   timestamp="20260829T120005Z")
    assert len(calls) == 48
    for path in (campaign / "cells").glob("*/status.json"):
        state = json.loads(path.read_text())
        assert state["status"] == "PASS"
        attempt = path.parent / "attempt-001"
        commands = json.loads((attempt / "commands.json").read_text())
        assert [item["stage"] for item in commands["predictive_plan"]] == [
            "predictive_plan_full_1", "predictive_plan_full_2"]
        assert "--repeat-plan" in commands["predictive_producer"]["argv"]
        assert "--output-dir" not in commands["predictive_producer"]["argv"]
        assert len(state["result"]["segments"]) == 2
        assert commands["live"]["run"]["argv"][commands["live"]["run"]["argv"].index("--passes") + 1] == "2"
        assert commands["live"]["run"]["argv"][
            commands["live"]["run"]["argv"].index("--repeat-predictive-plan") + 1
        ].endswith("depth-plan-full-2.json")
        assert [item["stage"] for item in commands["comparison"]] == [
            "comparison_full_1", "comparison_full_2"]
        first_comparison, second_comparison = commands["comparison"]
        assert first_comparison["argv"][first_comparison["argv"].index("--live-manifest") + 1].endswith(
            "live_curve_manifest_full-1.json")
        assert second_comparison["argv"][second_comparison["argv"].index("--live-manifest") + 1].endswith(
            "live_curve_manifest_full-2.json")
        assert first_comparison["argv"][first_comparison["argv"].index("--predictive-manifest") + 1] != \
            second_comparison["argv"][second_comparison["argv"].index("--predictive-manifest") + 1]
        assert first_comparison["argv"][first_comparison["argv"].index("--out") + 1].endswith(
            "records-full-1.jsonl")
        assert second_comparison["argv"][second_comparison["argv"].index("--out") + 1].endswith(
            "records-full-2.jsonl")
        assert len(state["result"]["plans"]) == 2
        assert all(item["artifacts"] for item in state["result"]["segments"])
