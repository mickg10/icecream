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


def _all_runner_factory(*, fail_stage: str | None = None):
    calls: list[str] = []

    def run(command: dict[str, object], _cwd: Path, stdout: Path, stderr: Path) -> int:
        stage = str(command["stage"])
        argv = [str(item) for item in command["argv"]]  # type: ignore[index]
        calls.append(stage)
        stdout.parent.mkdir(parents=True, exist_ok=True)
        with stdout.open("ab") as stream:
            stream.write((stage + "\n").encode())
        stderr.touch()
        if stage.startswith("predictive_plan"):
            Path(argv[argv.index("--out") + 1]).write_text("{}\n")
            result = Path(argv[argv.index("--result-dir") + 1])
            result.mkdir(parents=True, exist_ok=True)
            (result / "predictive_sim.jsonl").write_text('{"cumulative":{}}\n')
            (result / "producer_manifest.json").write_text("{}\n")
        elif stage == "live_prepare":
            output = Path(argv[argv.index("--output") + 1])
            output.mkdir(parents=True, exist_ok=True)
            (output / "batch-manifest.jsonl").write_text("{}\n")
            (output / "topology.json").write_text("{}\n")
        elif stage == "live_run":
            if fail_stage == stage:
                return 19
            output = Path(argv[argv.index("--output") + 1])
            target = (output / "icecream" /
                      argv[argv.index("--suite") + 1].replace("/", "-") /
                      argv[argv.index("--timestamp") + 1] /
                      argv[argv.index("--profile") + 1])
            target.mkdir(parents=True, exist_ok=True)
            (target / "live_curve_manifest.json").write_text("{}\n")
        elif stage.startswith("comparison"):
            if fail_stage == stage:
                return 23
            output = Path(argv[argv.index("--out") + 1])
            rows = []
            for record_type in ("predictive_sim", "live", "comparison"):
                row = {"schema": "icecream-s8-predictive-live-record-v1",
                       "record_type": record_type, "point_errors": [], "loss_curve": []}
                rows.append(json.dumps(row) + "\n")
            output.write_text("".join(rows))
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


def test_all_mode_commands_are_executable_and_pin_runtime_inputs(tmp_path: Path) -> None:
    cell = {"corpus": "DuckDB", "profile": "ZSTD_TU", "regime": "cold",
            "topology": "C1F1/100000"}
    values = driver._source_commands(
        tmp_path / "cell", cell, depth="100",
        source_manifest=tmp_path / "manifest.txt", source_root=tmp_path,
        matrix_audit=tmp_path / "matrix.json", engine_manifest=tmp_path / "engine.json",
        product_root=tmp_path / "product", python="python",
        campaign_stamp="20260831T120000Z", compile_db=tmp_path / "compile.json",
        compile_source_root=tmp_path / "source", compile_output_root=tmp_path / "out",
        repo=tmp_path / "repo", container_image=driver.PINNED_IMAGE,
        container_image_id="sha256:" + "a" * 64, container_temp_root=tmp_path)
    live = values[2]
    assert live["prepare"]["executable"] is True
    assert live["run"]["executable"] is True
    run_argv = [str(item) for item in live["run"]["argv"]]
    assert run_argv[run_argv.index("--container-image") + 1] == driver.PINNED_IMAGE
    assert run_argv[run_argv.index("--container-image-id") + 1] == "sha256:" + "a" * 64
    assert run_argv[run_argv.index("--container-temp-root") + 1] == str(tmp_path)
    assert values[3][0]["executable"] is True


def test_all_mode_fails_before_campaign_creation_without_live_authority(tmp_path: Path) -> None:
    kwargs = _kwargs(tmp_path, corpus="fmt")
    with pytest.raises(driver.CampaignError, match="container_image_content_id_required"):
        driver.run_campaign(**kwargs, mode="all", timestamp="20260831T120010Z")
    assert not list((tmp_path / "experiments").glob("s8-campaign-*"))


def _all_kwargs(tmp_path: Path) -> dict[str, object]:
    values = _kwargs(tmp_path, corpus="fmt")
    compile_db = tmp_path / "compile_commands.json"
    compile_db.write_text("[]\n")
    compile_output = tmp_path / "compile-output"
    compile_output.mkdir()
    values.update(compile_db=compile_db, compile_source_root=tmp_path,
                  compile_output_root=compile_output)
    return values


def test_all_mode_retains_authenticated_live_and_comparison_result(tmp_path: Path,
                                                                     monkeypatch: pytest.MonkeyPatch) -> None:
    runner, calls = _all_runner_factory()
    monkeypatch.setattr(driver, "_validate_live_prerequisites", lambda **_kwargs: {})
    campaign = driver.run_campaign(
        **_all_kwargs(tmp_path), mode="all", command_runner=runner,
        container_image_id="sha256:" + "a" * 64, container_temp_root=tmp_path,
        idle_host_gate=lambda: True, oom_protection=lambda: None,
        timestamp="20260831T120020Z")
    state = json.loads(next((campaign / "cells").glob("*/status.json")).read_text())
    assert state["status"] == "PASS"
    assert state["result"]["live"]["sha256"]
    assert state["result"]["comparisons"][0]["record_type"] == "comparison"
    assert calls[:5] == ["predictive_plan", "predictive_producer", "live_prepare",
                         "live_run", "comparison"]


def test_all_mode_live_failure_stops_before_next_cell(tmp_path: Path,
                                                      monkeypatch: pytest.MonkeyPatch) -> None:
    runner, calls = _all_runner_factory(fail_stage="live_run")
    monkeypatch.setattr(driver, "_validate_live_prerequisites", lambda **_kwargs: {})
    campaign = driver.run_campaign(
        **_all_kwargs(tmp_path), mode="all", command_runner=runner,
        container_image_id="sha256:" + "a" * 64, container_temp_root=tmp_path,
        idle_host_gate=lambda: True, oom_protection=lambda: None,
        timestamp="20260831T120021Z")
    summary = json.loads((campaign / "summary.json").read_text())
    assert summary["counts"]["FAIL"] == 1
    assert summary["counts"]["PENDING"] == 15
    assert calls == ["predictive_plan", "predictive_producer", "live_prepare", "live_run"]


def test_all_mode_oom_refusal_happens_before_campaign_creation(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    kwargs = _all_kwargs(tmp_path)
    monkeypatch.setattr(driver, "_validate_live_prerequisites", lambda **_kwargs: {})
    def refuse() -> None:
        raise driver.CampaignError("live_preflight:oom_protection_unavailable")
    with pytest.raises(driver.CampaignError, match="oom_protection_unavailable"):
        driver.run_campaign(
            **kwargs, mode="all", container_image_id="sha256:" + "a" * 64,
            container_temp_root=tmp_path, oom_protection=refuse,
            timestamp="20260831T120022Z")
    assert not list((tmp_path / "experiments").glob("s8-campaign-*"))


def test_all_mode_refuses_heldout_corpus_even_with_other_inputs(tmp_path: Path) -> None:
    kwargs = _kwargs(tmp_path)
    with pytest.raises(driver.CampaignError, match="all_mode_is_calibration_only"):
        driver.run_campaign(
            **kwargs, mode="all", container_image_id="sha256:" + "a" * 64,
            container_temp_root=tmp_path, simulator_authority=tmp_path / "sim.json",
            timestamp="20260831T120023Z")


def test_comparison_authentication_requires_normalizer_record(tmp_path: Path) -> None:
    output = tmp_path / "records.jsonl"
    output.write_text("{}\n")
    with pytest.raises(driver.CampaignError, match="record_count_invalid"):
        driver._authenticate_comparison(output)


def test_live_lock_is_host_global_for_campaigns_sharing_temp_root(tmp_path: Path) -> None:
    with driver._live_run_lock(tmp_path):
        with pytest.raises(driver.CampaignError, match="single_live_run_owned"):
            with driver._live_run_lock(tmp_path):
                pass


def test_idle_gate_rejects_processes_and_requires_cpu_headroom() -> None:
    stats = iter(((0, 0, 0, 100, 0, 0, 0, 0),
                  (0, 0, 0, 180, 1, 0, 0, 0)))
    assert driver._host_is_idle(process_scanner=lambda: ["build"],
                                 container_scanner=lambda: [],
                                 stat_reader=lambda: next(stats), sleep_fn=lambda _: None) is False
    stats = iter(((0, 0, 0, 100, 0, 0, 0, 0),
                  (0, 0, 0, 180, 1, 0, 0, 0)))
    assert driver._host_is_idle(process_scanner=lambda: [],
                                 container_scanner=lambda: [],
                                 stat_reader=lambda: next(stats), sleep_fn=lambda _: None) is True


def test_simulator_authority_binds_current_clean_repo_and_binary(tmp_path: Path,
                                                                  monkeypatch: pytest.MonkeyPatch) -> None:
    repo = tmp_path / "repo"
    repo.mkdir()
    binary = repo / "cache" / "sim" / ".p50sim.bin"
    binary.parent.mkdir(parents=True)
    binary.write_bytes(b"simulator")
    binary.chmod(0o755)
    monkeypatch.setattr(driver, "_git_identity", lambda _path: {
        "root": str(repo), "head": "a" * 40, "tree": "b" * 40,
        "tracked_clean": True, "tracked_diff_sha256": "c" * 64})
    authority = tmp_path / ".p50sim-build.json"
    authority.write_text(json.dumps({
        "schema": "icecream-p50sim-build-v1",
        "source": {"root": str(repo), "head": "a" * 40, "tree": "b" * 40,
                   "tracked_clean": True},
        "binary": {"path": str(binary), "sha256": __import__("hashlib").sha256(
            binary.read_bytes()).hexdigest(), "bytes": binary.stat().st_size},
        "inputs": {}, "configuration": {},
    }) + "\n")
    facts = driver._validate_simulator_authority(authority, repo)
    assert facts["binary"]["path"] == str(binary)
