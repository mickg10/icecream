from __future__ import annotations

import hashlib
import json
import subprocess
from contextlib import nullcontext
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

    def comparison_rows(cell: dict[str, str]) -> list[str]:
        identity = {**cell, "split": "calibration", "run_id": "run-1",
                    "source_commit": "a" * 40, "source_tree": "b" * 40,
                    "input_digest": "c" * 64, "topology_digest": "d" * 64,
                    "model_id": "model-1"}
        units = {"point": "step", "channel_bytes": "bytes", "elapsed_ns": "ns",
                 "throughput_bytes_per_s": "bytes_per_s", "C_TO_F_bytes": "bytes",
                 "F_TO_C_bytes": "bytes"}
        curve = [{"step": 0, "tu_id": "tu-0", "cumulative": {
            "C_TO_F_bytes": 6, "F_TO_C_bytes": 4, "channel_bytes": 10,
            "elapsed_ns": 1, "throughput_bytes_per_s": 1.0e10}}]
        common = {"schema": "icecream-s8-predictive-live-record-v1",
                  "semantics": "s8-current-semantics-v1", "cell": cell,
                  "split": "calibration", "identity": identity,
                  "units": units, "model_id": "model-1"}
        predicted = {**common, "record_type": "predictive_sim",
                     "raw_cumulative_curve": curve}
        observed = {**common, "record_type": "live",
                    "raw_cumulative_curve": curve}
        comparison = {**common, "record_type": "comparison",
                      "point_errors": [{"step": 0, "tu_id": "tu-0", "errors": {
                          "cumulative.C_TO_F_bytes": {"signed": 0, "absolute": 0,
                                                        "relative": 0, "squared": 0},
                          "cumulative.F_TO_C_bytes": {"signed": 0, "absolute": 0,
                                                        "relative": 0, "squared": 0},
                          "cumulative.channel_bytes": {"signed": 0, "absolute": 0,
                                                         "relative": 0, "squared": 0},
                          "cumulative.elapsed_ns": {"signed": 0, "absolute": 0,
                                                     "relative": 0, "squared": 0},
                          "cumulative.throughput_bytes_per_s": {"signed": 0, "absolute": 0,
                                                                  "relative": 0, "squared": 0},
                      }}],
                      "loss_curve": [{"step": 0, "tu_id": "tu-0",
                                      "squared_error": 0, "cumulative_loss": 0}]}
        return [json.dumps(value, sort_keys=True) + "\n"
                for value in (predicted, observed, comparison)]

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
            cell_corpus = next(corpus for corpus in driver.CORPORA
                               if any(part.startswith(f"{corpus}-") for part in output.parts))
            slug = next(part for part in output.parts
                        if part.startswith(f"{cell_corpus}-"))
            profile = next(profile for profile in driver.PROFILES
                           if slug.startswith(f"{cell_corpus}-{profile}-"))
            remainder = slug[len(f"{cell_corpus}-{profile}-"):]
            regime = remainder.split("-", 1)[0]
            comparison_cell = {"corpus": cell_corpus, "profile": profile,
                               "regime": regime}
            output.write_text("".join(comparison_rows(comparison_cell)))
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
        assert comparison["argv"][
            comparison["argv"].index("--calibration-metadata-manifest") + 1
        ].endswith("/experiment_manifest.json")
        live_argv = commands["live"]["run"]["argv"]
        assert live_argv[live_argv.index("--timestamp") + 1] == "20260829T120000Z"
        assert state["result"]["artifacts"]


def test_campaign_can_select_c1f1_only_without_changing_default_grid(tmp_path: Path) -> None:
    runner, calls = _runner_factory()
    campaign = driver.run_campaign(
        **_kwargs(tmp_path), selected_topologies=("C1F1/100000",),
        command_runner=runner, timestamp="20260829T120010Z")
    summary = json.loads((campaign / "summary.json").read_text())
    metadata = json.loads((campaign / "campaign.json").read_text())
    assert summary["status"] == "PASS"
    assert summary["expected_cells"] == 8
    assert summary["counts"]["PASS"] == 8
    assert len(calls) == 16
    assert metadata["config"]["selected_profiles"] == list(driver.PROFILES)
    assert metadata["config"]["selected_topologies"] == ["C1F1/100000"]
    assert metadata["matrix"]["expected_cells"] == 8
    assert all(row["cell"]["topology"] == "C1F1/100000"
               for row in summary["cells"])


def test_campaign_can_select_one_profile_and_topology(tmp_path: Path) -> None:
    runner, calls = _runner_factory()
    campaign = driver.run_campaign(
        **_kwargs(tmp_path), selected_profiles=("P29",),
        selected_topologies=("C1F20/40",), command_runner=runner,
        timestamp="20260829T120011Z")
    summary = json.loads((campaign / "summary.json").read_text())
    assert summary["expected_cells"] == 2
    assert summary["counts"]["PASS"] == 2
    assert len(calls) == 4
    assert {(row["cell"]["profile"], row["cell"]["regime"], row["cell"]["topology"])
            for row in summary["cells"]} == {
                ("P29", "cold", "C1F20/40"),
            ("P29", "warm", "C1F20/40"),
        }


def test_raw_ii_is_explicit_control_selection_and_retained_plan_only(tmp_path: Path) -> None:
    witness = tmp_path / "raw-ii-witness.json"
    engine = tmp_path / "raw-ii-engine-template.json"
    witness.write_text("raw witness\n")
    engine.write_text("raw engine template\n")
    campaign = driver.run_campaign(
        **_kwargs(tmp_path), execute=False, selected_profiles=("RAW_II",),
        selected_topologies=("C1F1/100000",),
        raw_ii_witness=witness,
        raw_ii_engine_manifest_template=engine,
        timestamp="20260829T120013Z")
    summary = json.loads((campaign / "summary.json").read_text())
    metadata = json.loads((campaign / "campaign.json").read_text())
    assert summary["expected_cells"] == 2
    assert summary["counts"]["STAGED"] == 2
    assert {row["cell"]["profile"] for row in summary["cells"]} == {"RAW_II"}
    assert metadata["config"]["selected_profiles"] == ["RAW_II"]
    assert metadata["config"]["raw_ii_witness"] == str(witness)
    assert metadata["config"]["raw_ii_engine_manifest_template"] == str(engine)
    state = json.loads(next((campaign / "cells").glob("*/status.json")).read_text())
    retained = campaign / "cells" / "DuckDB-RAW_II-cold-C1F1-100000" / "attempt-001" / "raw-ii-control-inputs.json"
    assert state["raw_ii_control_inputs"]["path"].endswith("raw-ii-control-inputs.json")
    assert json.loads(retained.read_text())["arm_kind"] == "control_baseline"


def test_raw_ii_fails_closed_without_control_inputs(tmp_path: Path) -> None:
    with pytest.raises(driver.CampaignError,
                       match="raw_ii:control_baseline_requires_witness_and_engine_template"):
        driver.run_campaign(**_kwargs(tmp_path), execute=False,
                             selected_profiles=("RAW_II",),
                             timestamp="20260829T120014Z")


def test_raw_ii_predictive_control_executes_dedicated_producer(tmp_path: Path) -> None:
    witness = tmp_path / "raw-ii-witness.json"
    engine = tmp_path / "raw-ii-engine-template.json"
    witness.write_text("raw witness\n")
    engine.write_text("raw engine template\n")
    runner, calls = _runner_factory()
    campaign = driver.run_campaign(
        **_kwargs(tmp_path), selected_profiles=("RAW_II",),
        raw_ii_witness=witness,
        raw_ii_engine_manifest_template=engine,
        command_runner=runner, timestamp="20260829T120015Z")
    summary = json.loads((campaign / "summary.json").read_text())
    assert summary["status"] == "PASS"
    assert summary["expected_cells"] == 4
    assert len(calls) == 8
    producer_calls = [argv for stage, argv in calls if stage == "predictive_producer"]
    assert producer_calls and all("s8_raw_ii_predictive_producer.py" in argv[1]
                                  for argv in producer_calls)
    assert all("s8_multitu_predictive_producer.py" not in argv[1]
               and str(engine) in argv[argv.index("--engine-manifest") + 1]
               and str(witness) in argv[argv.index("--raw-ii-witness") + 1]
               for argv in producer_calls)
    live_commands = json.loads(next((campaign / "cells").glob("*/attempt-001/commands.json")).read_text())["live"]
    live_argv = live_commands["run"]["argv"]
    assert live_argv[live_argv.index("--profile") + 1] == "P29"
    assert live_argv[live_argv.index("--product-profile") + 1] == "RAW_II"
    commands = json.loads(next((campaign / "cells").glob("*/attempt-001/commands.json")).read_text())
    assert commands["predictive_plan"][0]["executable"] is True
    assert commands["predictive_producer"]["executable"] is True
    producer_argv = commands["predictive_producer"]["argv"]
    assert producer_argv[producer_argv.index("--product-root") + 1] == str(tmp_path / "product")


def test_raw_ii_local_campaign_binds_plan_producer_and_live_identity(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """Exercise the RAW path locally; the live callback is a retained fixture."""
    import s8_predictive_live_normalizer as normalizer

    source_root = tmp_path / "source"
    source_root.mkdir()
    source = source_root / "tu.cc"
    source.write_bytes(b"int main() { return 0; }\n")
    (source_root / "RAW_II.txt").write_text(str(source) + "\n")
    matrix = tmp_path / "matrix.json"
    matrix.write_text(json.dumps({
        "schema": "icecream-s8-matrix-audit-v1", "status": "PASS",
        "matrix": {"expected_cells": 32, "completed_cells": 32,
                    "missing_cells": [], "invalid_candidates": [],
                    "calibration_cells": 16, "held_out_validation_cells": 16},
        "cells": [],
    }) + "\n")
    source_sha = hashlib.sha256(source.read_bytes()).hexdigest()
    source_bytes = source.stat().st_size
    for regime in ("cold", "warm"):
        witness_rows = [{
            "ordinal": ordinal, "source_relative": "tu.cc",
            "source_sha256": source_sha, "source_bytes": source_bytes,
            "c_to_f": {"compile_file_bytes": 10, "file_chunk_bytes": 20,
                       "end_bytes": 3, "total_bytes": 33}}
            for ordinal in range(100)]
        engine_rows = [{
            "ordinal": ordinal, "source_relative": "tu.cc",
            "source_sha256": source_sha, "source_bytes": source_bytes,
            "f_to_c_bytes": 17, "source_service_ns": 40,
            "execution_service_ns": 60, "elapsed_ns": 100}
            for ordinal in range(100)]
        cell = {"corpus": "fmt", "profile": "RAW_II", "regime": regime}
        (tmp_path / f"witness-{regime}.json").write_text(json.dumps({
            "schema": "icecream-s8-raw-ii-legacy-wire-witness-v1",
            "semantics": "s8-current-semantics-v1", "cell": cell,
            "split": "calibration", "formula": {
                "name": "legacy-filechunk-wire-v1",
                "c_to_f": "compile_file_bytes+file_chunk_bytes+end_bytes"},
            "rows": witness_rows}, sort_keys=True) + "\n")
        (tmp_path / f"engine-{regime}.json").write_text(json.dumps({
            "schema": "icecream-s8-raw-ii-control-engine-v1",
            "semantics": "s8-current-semantics-v1", "cell": cell,
            "split": "calibration",
            "control_baseline": normalizer.CONTROL_BASELINE,
            "engine_scope": "raw_ii_control_engine", "model_id": "raw-control-v1",
            "rows": engine_rows}, sort_keys=True) + "\n")
    product = tmp_path / "product"
    product.mkdir()
    (product / "product.txt").write_text("product\n")
    subprocess.run(["git", "init", "-q", str(product)], check=True)
    subprocess.run(["git", "-C", str(product), "config", "user.email", "raw@test.invalid"], check=True)
    subprocess.run(["git", "-C", str(product), "config", "user.name", "RAW test"], check=True)
    subprocess.run(["git", "-C", str(product), "add", "product.txt"], check=True)
    subprocess.run(["git", "-C", str(product), "commit", "-qm", "product"], check=True)
    authority = tmp_path / "authority.json"
    authority.write_text("{}\n")
    monkeypatch.setattr(driver, "LIVE_LOCK_PATH", tmp_path / "live.lock")
    monkeypatch.setattr(driver.external_farm_executor, "load_authority", lambda _path: {})

    def local_runner(command: dict[str, object], _cwd: Path, stdout: Path, stderr: Path) -> int:
        stage = str(command["stage"])
        argv = [str(item) for item in command["argv"]]  # type: ignore[index]
        stdout.parent.mkdir(parents=True, exist_ok=True)
        if stage.startswith("predictive_plan") or stage == "predictive_producer":
            completed = subprocess.run(argv, stdout=stdout.open("ab"), stderr=stderr.open("ab"),
                                       check=False)
            return completed.returncode
        if stage == "live_prepare":
            output = Path(argv[argv.index("--output") + 1])
            output.mkdir(parents=True, exist_ok=True)
            (output / "batch-manifest.jsonl").write_text("{}\n")
            (output / "topology.json").write_text("{}\n")
            return 0
        if stage.startswith("comparison"):
            output = Path(argv[argv.index("--out") + 1])
            normalizer.normalize(Path(argv[argv.index("--predictive-manifest") + 1]),
                                 Path(argv[argv.index("--live-manifest") + 1]), output)
            return 0
        raise AssertionError(stage)

    def external_fixture(_transport: object, **kwargs: object) -> Path:
        output = Path(kwargs["output"])
        target = (output / "icecream" / str(kwargs["topology"]).replace("/", "-") /
                  str(kwargs["timestamp"]) / "RAW_II")
        target.mkdir(parents=True, exist_ok=True)
        plan = json.loads(Path(kwargs["predictive_plan"]).read_text())
        predictive = Path(plan["result"]["directory"])
        curve = target / "live_curve.jsonl"
        curve.write_bytes((predictive / "predictive_sim.jsonl").read_bytes())
        manifest = json.loads((predictive / "predictive_curve_manifest.json").read_text())
        manifest["curve"] = {"path": curve.name,
                              "sha256": hashlib.sha256(curve.read_bytes()).hexdigest(),
                              "bytes": curve.stat().st_size}
        manifest["provenance"] = {"mode": "live", "producer": "local-raw-fixture",
                                   "trace_free": False}
        live_manifest = target / "live_curve_manifest.json"
        live_manifest.write_text(json.dumps(manifest, sort_keys=True) + "\n")
        return live_manifest

    kwargs = _kwargs(
        tmp_path, repo=Path(__file__).resolve().parents[1], corpus="fmt",
        source_manifest=str(source_root / "{profile}.txt"),
        source_root=source_root, matrix_audit=matrix, product_build_root=product,
        compile_db=tmp_path / "compile_commands.json",
        compile_source_root=source_root,
        raw_ii_witness=str(tmp_path / "witness-{regime}.json"),
        raw_ii_engine_manifest_template=str(tmp_path / "engine-{regime}.json"),
        selected_profiles=("RAW_II",), selected_topologies=("C1F1/100000",))
    campaign = driver.run_campaign(
        **kwargs, mode=driver.EXTERNAL_FARM_MODE,
        external_farm_authority=authority,
        external_cell_runner=external_fixture,
        external_transport_factory=lambda value: {"authority": value},
        command_runner=local_runner, timestamp="20260901T120020Z")
    summary = json.loads((campaign / "summary.json").read_text())
    assert summary["status"] == "PASS"
    assert summary["counts"]["PASS"] == 2
    state_path = next((campaign / "cells").glob("*/status.json"))
    state = json.loads(state_path.read_text())
    commands = json.loads((state_path.parent / "attempt-001" / "commands.json").read_text())
    assert commands["predictive_plan"][0]["executable"] is True
    assert commands["predictive_producer"]["executable"] is True
    assert "--product-root" in commands["predictive_producer"]["argv"]
    assert state["result"]["live"]["path"].endswith("/RAW_II/live_curve_manifest.json")
    assert state["result"]["comparisons"][0]["identity"]["profile"] == "RAW_II"


def test_campaign_selection_rejects_empty_unknown_and_duplicate_values() -> None:
    with pytest.raises(driver.CampaignError, match="profiles:selection_empty"):
        driver.cells("fmt", selected_profiles=())
    with pytest.raises(driver.CampaignError, match="topologies:undeclared"):
        driver.cells("fmt", selected_topologies=("C1F99/1",))
    with pytest.raises(driver.CampaignError, match="profiles:selection_duplicate"):
        driver.cells("fmt", selected_profiles=("P29", "P29"))
    with pytest.raises(driver.CampaignError, match="topologies:selection_invalid"):
        driver.cells("fmt", selected_topologies="C1F1/100000")  # type: ignore[arg-type]


def test_resume_rejects_changed_selected_dimensions(tmp_path: Path) -> None:
    runner, _ = _runner_factory()
    original = _kwargs(tmp_path, selected_profiles=("P29",),
                       selected_topologies=("C1F1/100000",))
    campaign = driver.run_campaign(**original, command_runner=runner,
                                   timestamp="20260829T120012Z")
    changed = _kwargs(tmp_path, selected_profiles=("GRZ_RESIDUAL",),
                      selected_topologies=("C1F1/100000",))
    with pytest.raises(driver.CampaignError, match="configuration_mismatch"):
        driver.run_campaign(**changed, resume=campaign, command_runner=runner)


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


def test_experiment_identity_maps_grz_owner_method() -> None:
    cell = {"corpus": "fmt", "profile": "GRZ_RESIDUAL", "regime": "warm",
            "topology": "C1F20/40"}
    assert driver._experiment_identity(
        cell, split="calibration", depth_class="100", pass_id="full-1") == {
            "method": "GRZ", "profile": "GRZ_RESIDUAL", "corpus": "fmt",
            "regime": "warm", "split": "calibration", "topology": "C1F20/40",
            "depth_class": "100", "pass_id": "full-1",
        }


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
    comparison_argv = [str(item) for item in values[3][0]["argv"]]
    assert comparison_argv[
        comparison_argv.index("--calibration-metadata-manifest") + 1
    ].endswith("/experiment_manifest.json")


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
    comparison = state["result"]["comparisons"][0]
    assert comparison["records"]["path"].endswith("records.jsonl")
    cell = state["cell"]
    assert comparison["identity"] == {
        "method": driver.METHOD_BY_PROFILE.get(cell["profile"], cell["profile"]),
        "profile": cell["profile"], "corpus": cell["corpus"],
        "regime": cell["regime"], "split": "calibration",
        "topology": cell["topology"], "depth_class": "100", "pass_id": "full-1",
    }
    assert comparison["predicted"]["transfer_bytes"] == 10
    assert comparison["observed"]["elapsed_ns"] == 1
    assert comparison["errors"]["transfer_bytes"]["absolute"] == 0
    assert comparison["errors"]["elapsed_ns"]["relative"] == 0
    assert comparison["aggregate"] == {
        "loss_curve_points": 1,
        "final": {"step": 0, "tu_id": "tu-0", "squared_error": 0,
                   "cumulative_loss": 0},
        "curve_in_records": True,
    }
    assert calls[:5] == ["predictive_plan", "predictive_producer", "live_prepare",
                         "live_run", "comparison"]


def test_all_mode_rejects_mutated_comparison_error_curve(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    runner, _ = _all_runner_factory()
    monkeypatch.setattr(driver, "_validate_live_prerequisites", lambda **_kwargs: {})
    campaign = driver.run_campaign(
        **_all_kwargs(tmp_path), mode="all", command_runner=runner,
        container_image_id="sha256:" + "a" * 64, container_temp_root=tmp_path,
        idle_host_gate=lambda: True, oom_protection=lambda: None,
        timestamp="20260831T120022Z")
    state = json.loads(next((campaign / "cells").glob("*/status.json")).read_text())
    comparison_path = campaign / state["result"]["comparisons"][0]["records"]["path"]
    rows = [json.loads(line) for line in comparison_path.read_text().splitlines()]
    rows[2]["point_errors"][0]["errors"]["cumulative.channel_bytes"]["absolute"] = 1
    comparison_path.write_text("".join(json.dumps(row, sort_keys=True) + "\n"
                                               for row in rows))
    cell = state["cell"]
    with pytest.raises(driver.CampaignError, match="prediction_error_value_mismatch"):
        driver._authenticate_comparison(
            comparison_path,
            expected_cell=(cell["corpus"], cell["profile"], cell["regime"]),
            experiment_identity={
                "method": driver.METHOD_BY_PROFILE.get(cell["profile"], cell["profile"]),
                "profile": cell["profile"], "corpus": cell["corpus"],
                "regime": cell["regime"], "split": "calibration",
                "topology": cell["topology"],
                "depth_class": "100", "pass_id": "full-1",
            })


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


def test_all_mode_resume_revalidates_and_compares_live_authority(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    runner, _ = _all_runner_factory()
    kwargs = _all_kwargs(tmp_path)
    authority = {"container_image": {"image_id": "sha256:" + "a" * 64},
                 "simulator_authority": {"sha256": "b" * 64}}
    monkeypatch.setattr(driver, "_validate_live_prerequisites", lambda **_kwargs: authority)
    campaign = driver.run_campaign(
        **kwargs, mode="all", command_runner=runner,
        container_image_id="sha256:" + "a" * 64, container_temp_root=tmp_path,
        idle_host_gate=lambda: True, oom_protection=lambda: None,
        timestamp="20260831T120024Z")
    changed = {"container_image": {"image_id": "sha256:" + "c" * 64},
               "simulator_authority": {"sha256": "b" * 64}}
    monkeypatch.setattr(driver, "_validate_live_prerequisites", lambda **_kwargs: changed)
    with pytest.raises(driver.CampaignError, match="live_authority_mismatch"):
        driver.run_campaign(
            **kwargs, mode="all", resume=campaign, retry_failed=True,
            command_runner=runner, container_image_id="sha256:" + "a" * 64,
            container_temp_root=tmp_path, idle_host_gate=lambda: True,
            oom_protection=lambda: None)


def test_all_mode_refuses_heldout_corpus_even_with_other_inputs(tmp_path: Path) -> None:
    kwargs = _kwargs(tmp_path)
    with pytest.raises(driver.CampaignError, match="all_mode_is_calibration_only"):
        driver.run_campaign(
            **kwargs, mode="all", container_image_id="sha256:" + "a" * 64,
            container_temp_root=tmp_path, simulator_authority=tmp_path / "sim.json",
            timestamp="20260831T120023Z")


def _external_test_setup(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> tuple[
        dict[str, object], Path]:
    product = tmp_path / "product"
    product.mkdir()
    authority_path = tmp_path / "external-authority.json"
    authority_path.write_text("fixture\n")
    authority: dict[str, object] = {
        "schema": "icecream-s8-external-farm-authority-v1",
        "path": str(authority_path), "sha256": "a" * 64, "bytes": 8,
        "placements": {"C1F1/100000": {"relationship_hosts": ["q2"]}},
    }
    monkeypatch.setattr(driver.external_farm_executor, "load_authority",
                        lambda _path: authority)
    return authority, authority_path


def test_external_mode_executes_then_finalizes_and_authenticates_comparison(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    _authority_value, authority_path = _external_test_setup(tmp_path, monkeypatch)
    command_runner, calls = _all_runner_factory()
    external_calls: list[dict[str, object]] = []
    authority_requests: list[dict[str, str]] = []

    def authority_provider(cell: dict[str, str]) -> Path:
        authority_requests.append(cell.copy())
        return authority_path

    def external_runner(
            _transport: object, *, topology: str, relationship_hosts: object,
            profile: str, batch_manifest: Path, predictive_plan: Path,
            topology_file: Path, corpus: str, regime: str, depth: str,
            output: Path, product_root: Path,
            repeat_predictive_plan: Path | None, passes: int,
            timestamp: str | None, artifact_sample: int = 2,
            retain_all_artifacts: bool = False) -> Path:
        kwargs = {"topology": topology, "relationship_hosts": relationship_hosts,
                  "profile": profile, "batch_manifest": batch_manifest,
                  "predictive_plan": predictive_plan, "topology_file": topology_file,
                  "corpus": corpus, "regime": regime, "depth": depth,
                  "output": output, "product_root": product_root,
                  "repeat_predictive_plan": repeat_predictive_plan, "passes": passes,
                  "timestamp": timestamp, "artifact_sample": artifact_sample,
                  "retain_all_artifacts": retain_all_artifacts}
        external_calls.append(kwargs)
        suite = topology.replace("/", "-")
        target = output / "icecream" / suite / str(timestamp) / profile
        target.mkdir(parents=True, exist_ok=True)
        manifest = target / "live_curve_manifest.json"
        manifest.write_text("{}\n")
        return manifest

    kwargs = _all_kwargs(tmp_path)
    campaign = driver.run_campaign(
        **kwargs, mode=driver.EXTERNAL_FARM_MODE,
        external_authority_provider=authority_provider,
        external_cell_runner=external_runner,
        external_transport_factory=lambda value: {"authority": value},
        command_runner=command_runner, timestamp="20260901T120000Z")
    summary = json.loads((campaign / "summary.json").read_text())
    assert summary["status"] == "PASS"
    assert summary["counts"]["PASS"] == 16
    assert calls[:3] == ["predictive_plan", "predictive_producer", "live_prepare"]
    assert len(external_calls) == 16
    assert len(authority_requests) == 16
    state_path = next((campaign / "cells").glob("*/status.json"))
    state = json.loads(state_path.read_text())
    assert state["status"] == "PASS"
    assert state["result"]["live"]["path"].endswith("live_curve_manifest.json")
    assert state["result"]["comparisons"][0]["record_type"] == "comparison"
    metadata = json.loads((campaign / "campaign.json").read_text())
    assert metadata["config"]["mode"] == driver.EXTERNAL_FARM_MODE
    assert metadata["config"]["external_farm_authority"] is None
    assert metadata["config"]["external_authority_provider"] is True
    assert metadata["external_farm_authority"] == "provider"
    assert state["external_farm_authority"] == {
        "path": str(authority_path), "bytes": 8,
        "sha256": hashlib.sha256(authority_path.read_bytes()).hexdigest()}
    commands = json.loads((state_path.parent / "attempt-001" / "commands.json").read_text())
    assert commands["live"]["run"]["executable"] is False
    assert commands["live"]["run"]["reason"] == "external farm adapter owns execution"
    assert commands["comparison"][0]["executable"] is True
    assert external_calls[0]["batch_manifest"]
    assert external_calls[0]["predictive_plan"]
    assert external_calls[0]["topology_file"]


def test_external_campaign_bootstrap_then_reuse_options_reach_cell_runner(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    _authority_value, authority_path = _external_test_setup(tmp_path, monkeypatch)
    monkeypatch.setattr(driver, "_live_run_lock", lambda _value: nullcontext())
    calls: list[dict[str, object]] = []

    def external_runner(_transport: object, **kwargs: object) -> Path:
        calls.append(kwargs)
        output = Path(kwargs["output"])
        target = (output / "icecream" / str(kwargs["topology"]).replace("/", "-") /
                  str(kwargs["timestamp"]) / str(kwargs["profile"]))
        target.mkdir(parents=True, exist_ok=True)
        result = target / "live_curve_manifest.json"
        result.write_text("{}\n")
        return result

    def mint(_cell: Path, *, authority: Path, package_dir: Path) -> Path:
        assert authority == authority_path
        package_dir.mkdir(parents=True)
        manifest = package_dir / "manifest.jsonl"
        manifest.write_text("fixture\n")
        return manifest

    monkeypatch.setattr(driver.external_farm_executor.reference_witness_module,
                        "create_from_cell", mint)
    first_kwargs = _all_kwargs(tmp_path)
    first_kwargs["output_root"] = tmp_path / "first"
    driver.run_campaign(
        **first_kwargs, mode=driver.EXTERNAL_FARM_MODE,
        selected_profiles=["ZSTD_ROUTE"], selected_topologies=["C1F1/100000"],
        external_farm_authority=authority_path, external_cell_runner=external_runner,
        external_transport_factory=lambda value: {"authority": value},
        command_runner=_all_runner_factory()[0], timestamp="20260901T130000Z",
        reference_witness_output=tmp_path / "witness")
    assert len(calls) == 2 and calls[0]["retain_all_artifacts"] is True

    second_kwargs = dict(first_kwargs)
    second_kwargs["output_root"] = tmp_path / "second"
    driver.run_campaign(
        **second_kwargs, mode=driver.EXTERNAL_FARM_MODE,
        selected_profiles=["ZSTD_ROUTE"], selected_topologies=["C1F1/100000"],
        external_farm_authority=authority_path, external_cell_runner=external_runner,
        external_transport_factory=lambda value: {"authority": value},
        command_runner=_all_runner_factory()[0], timestamp="20260901T130001Z",
        reference_witness=tmp_path / "witness", reference_authority=authority_path)
    assert len(calls) == 4
    assert calls[2]["reference_witness"] == tmp_path / "witness"


def test_external_static_authority_is_reloaded_and_stale_capture_fails_closed(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    authority, authority_path = _external_test_setup(tmp_path, monkeypatch)
    loads: list[Path] = []

    def load_authority(path: Path) -> dict[str, object]:
        loads.append(path)
        if len(loads) > 1:
            raise driver.external_farm_executor.ExternalFarmError("authority:idle_stale")
        return authority

    monkeypatch.setattr(driver.external_farm_executor, "load_authority", load_authority)
    command_runner, _ = _all_runner_factory()

    def external_runner(_transport: object, **kwargs: object) -> Path:
        output = Path(kwargs["output"])
        target = (output / "icecream" / str(kwargs["topology"]).replace("/", "-") /
                  str(kwargs["timestamp"]) / str(kwargs["profile"]))
        target.mkdir(parents=True, exist_ok=True)
        result = target / "live_curve_manifest.json"
        result.write_text("{}\n")
        return result

    campaign = driver.run_campaign(
        **_all_kwargs(tmp_path), mode=driver.EXTERNAL_FARM_MODE,
        external_farm_authority=authority_path,
        external_cell_runner=external_runner,
        external_transport_factory=lambda value: {"authority": value},
        command_runner=command_runner, timestamp="20260901T120000Z")
    summary = json.loads((campaign / "summary.json").read_text())
    assert len(loads) == 2
    assert summary["counts"] == {
        "FAIL": 1, "INTERRUPTED": 0, "PASS": 1, "PENDING": 14,
        "RUNNING": 0, "STAGED": 0}


def test_external_authority_command_refreshes_per_attempt_without_shell(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    _authority_value, _static_authority_path = _external_test_setup(tmp_path, monkeypatch)
    refresh_calls: list[list[str]] = []
    original_run = driver.subprocess.run

    def fake_run(argv: list[str], *args: object, **kwargs: object) -> subprocess.CompletedProcess[str]:
        if "--output" not in argv:
            return original_run(argv, *args, **kwargs)
        assert kwargs.get("shell") is not True
        refresh_calls.append(argv)
        output = Path(argv[argv.index("--output") + 1])
        output.write_text("fixture authority\n")
        return subprocess.CompletedProcess(argv, 0, "refresh stdout\n", "refresh stderr\n")

    monkeypatch.setattr(driver.subprocess, "run", fake_run)
    command_runner, _ = _all_runner_factory()

    def external_runner(_transport: object, **kwargs: object) -> Path:
        output = Path(kwargs["output"])
        target = (output / "icecream" / str(kwargs["topology"]).replace("/", "-") /
                  str(kwargs["timestamp"]) / str(kwargs["profile"]))
        target.mkdir(parents=True, exist_ok=True)
        result = target / "live_curve_manifest.json"
        result.write_text("{}\n")
        return result

    campaign = driver.run_campaign(
        **_all_kwargs(tmp_path), mode=driver.EXTERNAL_FARM_MODE,
        external_authority_command=("python3", "authority.py", "--output", "{output}",
                                     "--cell", "{cell}"),
        external_cell_runner=external_runner,
        external_transport_factory=lambda value: {"authority": value},
        command_runner=command_runner, timestamp="20260901T120002Z")
    assert len(refresh_calls) == 16
    assert all("{output}" not in call and "{cell}" not in call for call in refresh_calls)
    state_path = next((campaign / "cells").glob("*/status.json"))
    state = json.loads(state_path.read_text())
    attempt = state_path.parent / "attempt-001"
    refresh = state["external_farm_authority_refresh"]
    assert refresh["schema"] == driver.AUTHORITY_REFRESH_SCHEMA
    assert refresh["returncode"] == 0
    assert refresh["command"]["argv"][0] == "python3"
    assert refresh["command"]["shell"] is False
    assert refresh["command"]["cwd"] == str(attempt)
    assert refresh["stdout"]["path"].endswith("external-authority-refresh.stdout")
    assert refresh["stderr"]["path"].endswith("external-authority-refresh.stderr")
    assert refresh["authority"]["path"].endswith("external-farm-authority.json")
    assert state["external_farm_authority"]["path"].endswith("external-farm-authority.json")
    assert (attempt / "external-authority-refresh.json").is_file()


def test_external_mode_resume_keeps_failed_attempt_and_retries_explicitly(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    _authority_value, authority_path = _external_test_setup(tmp_path, monkeypatch)
    command_runner, _ = _all_runner_factory()
    external_attempts: list[Path] = []

    def failing_external(_transport: object, **kwargs: object) -> Path:
        external_attempts.append(Path(kwargs["output"]))
        raise driver.external_farm_executor.ExternalFarmError("fixture_failure")

    kwargs = _all_kwargs(tmp_path)
    campaign = driver.run_campaign(
        **kwargs, mode=driver.EXTERNAL_FARM_MODE,
        external_farm_authority=authority_path,
        external_cell_runner=failing_external,
        external_transport_factory=lambda value: {"authority": value},
        command_runner=command_runner, timestamp="20260901T120001Z")
    summary = json.loads((campaign / "summary.json").read_text())
    assert summary["counts"]["FAIL"] == 1
    assert summary["counts"]["PENDING"] == 15
    failed_state_path = next(path for path in (campaign / "cells").glob("*/status.json")
                             if json.loads(path.read_text())["status"] == "FAIL")
    no_retry_runner, _ = _all_runner_factory()
    resumed = driver.run_campaign(
        **kwargs, mode=driver.EXTERNAL_FARM_MODE, resume=campaign,
        external_farm_authority=authority_path,
        external_cell_runner=failing_external,
        external_transport_factory=lambda value: {"authority": value},
        command_runner=no_retry_runner)
    assert resumed == campaign
    assert len(external_attempts) == 2
    assert not failed_state_path.parent.joinpath("attempt-002").exists()

    success_runner, _ = _all_runner_factory()

    def successful_external(_transport: object, **kwargs: object) -> Path:
        external_attempts.append(Path(kwargs["output"]))
        output = Path(kwargs["output"])
        target = (output / "icecream" / str(kwargs["topology"]).replace("/", "-") /
                  str(kwargs["timestamp"]) / str(kwargs["profile"]))
        target.mkdir(parents=True, exist_ok=True)
        result = target / "live_curve_manifest.json"
        result.write_text("{}\n")
        return result

    driver.run_campaign(
        **kwargs, mode=driver.EXTERNAL_FARM_MODE, resume=campaign,
        retry_failed=True, external_farm_authority=authority_path,
        external_cell_runner=successful_external,
        external_transport_factory=lambda value: {"authority": value},
        command_runner=success_runner)
    summary = json.loads((campaign / "summary.json").read_text())
    assert summary["status"] == "PASS"
    assert len(external_attempts) == 18
    assert failed_state_path.parent.joinpath("attempt-001").is_dir()
    assert failed_state_path.parent.joinpath("attempt-002").is_dir()
    state = json.loads(failed_state_path.read_text())
    assert state["status"] == "PASS"
    assert any(item["status"] == "FAIL" for item in state["history"])


@pytest.mark.parametrize(
    "failure", [subprocess.TimeoutExpired(["external-cell"], 900),
                 subprocess.CalledProcessError(17, ["external-cell"])],
    ids=["timeout", "subprocess-error"],
)
def test_external_subprocess_failure_is_terminal_and_retained(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
        failure: subprocess.SubprocessError) -> None:
    _authority_value, authority_path = _external_test_setup(tmp_path, monkeypatch)
    command_runner, calls = _all_runner_factory()

    def failing_external(_transport: object, **_kwargs: object) -> Path:
        raise failure

    campaign = driver.run_campaign(
        **_all_kwargs(tmp_path), mode=driver.EXTERNAL_FARM_MODE,
        external_farm_authority=authority_path,
        external_cell_runner=failing_external,
        external_transport_factory=lambda value: {"authority": value},
        command_runner=command_runner, timestamp="20260901T120005Z")
    summary = json.loads((campaign / "summary.json").read_text())
    assert summary["status"] == "PARTIAL_FAILURE"
    assert summary["counts"]["FAIL"] == 1
    assert summary["counts"]["RUNNING"] == 0
    assert calls[:3] == ["predictive_plan", "predictive_producer", "live_prepare"]
    state_path = next((campaign / "cells").glob("*/status.json"))
    state = json.loads(state_path.read_text())
    assert state["status"] == "FAIL"
    assert state["failure_record"]["path"].endswith("/failure.json")
    failure_path = state_path.parent / "attempt-001" / "failure.json"
    record = json.loads(failure_path.read_text())
    assert record["status"] == "FAIL"
    assert record["error_type"] == type(failure).__name__
    campaign_status = json.loads((campaign / "campaign-status.json").read_text())
    assert campaign_status["status"] == "PARTIAL_FAILURE"


def test_external_mode_requires_private_authority_and_product_root(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    kwargs = _kwargs(tmp_path, corpus="fmt", product_build_root=tmp_path / "product")
    with pytest.raises(driver.CampaignError, match="authority_required"):
        driver.run_campaign(**kwargs, mode=driver.EXTERNAL_FARM_MODE,
                            timestamp="20260901T120002Z")
    authority_path = tmp_path / "authority.json"
    authority_path.write_text("fixture\n")
    monkeypatch.setattr(driver.external_farm_executor, "load_authority",
                        lambda _path: {})
    with pytest.raises(driver.CampaignError, match="product_root_unavailable"):
        driver.run_campaign(
            **kwargs, mode=driver.EXTERNAL_FARM_MODE,
            external_farm_authority=authority_path,
            timestamp="20260901T120003Z")
    assert not list((tmp_path / "experiments").glob("s8-campaign-*"))

    product = tmp_path / "product"
    product.mkdir()
    with pytest.raises(driver.CampaignError, match="authority_sources_ambiguous"):
        driver.run_campaign(
            **_kwargs(tmp_path, corpus="fmt", product_build_root=product),
            mode=driver.EXTERNAL_FARM_MODE,
            external_farm_authority=authority_path,
            external_authority_command=("python3", "authority.py", "--output", "{output}"),
            timestamp="20260901T120004Z")
    with pytest.raises(driver.CampaignError, match="authority_sources_ambiguous"):
        driver.run_campaign(
            **_kwargs(tmp_path, corpus="fmt", product_build_root=product),
            mode=driver.EXTERNAL_FARM_MODE,
            external_farm_authority=authority_path,
            external_authority_provider=lambda _cell: authority_path,
            timestamp="20260901T120005Z")


def test_comparison_authentication_requires_normalizer_record(tmp_path: Path) -> None:
    output = tmp_path / "records.jsonl"
    output.write_text("{}\n")
    with pytest.raises(driver.CampaignError, match="record_count_invalid"):
        driver._authenticate_comparison(output)


def test_live_lock_is_host_global_for_campaigns_sharing_temp_root(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(driver, "LIVE_LOCK_PATH", tmp_path / "host-global.lock")
    with driver._live_run_lock(tmp_path):
        with pytest.raises(driver.CampaignError, match="single_live_run_owned"):
            with driver._live_run_lock(tmp_path / "different-campaign-temp"):
                pass


def test_competing_process_scanner_ignores_host_docker_infrastructure_and_finds_work(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    proc = tmp_path / "proc"
    proc.mkdir()
    commands = {
        101: "/usr/bin/dockerd --host=unix:///var/run/docker.sock",
        102: "/usr/bin/containerd",
        103: "/usr/bin/docker-proxy -proto tcp",
        104: "python3 /repo/farmharness/s8_campaign_driver.py --mode all",
        105: "/opt/p50compile --job 1",
        106: "/usr/bin/docker run supervisor s8_campaign_driver.py",
        107: "/usr/bin/docker ps --all",
    }
    for pid, command in commands.items():
        entry = proc / str(pid)
        entry.mkdir()
        (entry / "cmdline").write_bytes(command.replace(" ", "\0").encode())
        (entry / "status").write_text("PPid:\t1\n")
    found = driver._competing_processes(proc_root=proc, ancestor_pid=99999)
    assert [item.split(":", 1)[0] for item in found] == ["104", "105", "106"]

    launcher = proc / "200"
    launcher.mkdir()
    (launcher / "cmdline").write_bytes(
        b"python3\0/repo/farmharness/s8_protected_launcher.py\0--execute\0")
    (launcher / "status").write_text("PPid:\t1\n")
    owned_docker = proc / "201"
    owned_docker.mkdir()
    (owned_docker / "cmdline").write_bytes(
        b"docker\0run\0supervisor\0s8_campaign_driver.py\0")
    (owned_docker / "status").write_text("PPid:\t200\n")
    monkeypatch.setenv(driver.LAUNCHER_PID_ENV, "200")
    found = driver._competing_processes(proc_root=proc, ancestor_pid=99999)
    assert [item.split(":", 1)[0] for item in found] == ["104", "105", "106"]


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
