from __future__ import annotations

import json
import hashlib
import shutil
import subprocess
from pathlib import Path

import pytest

import s8_heldout_finalizer as finalizer
from s8_schema import CORPORA, PROFILES, REGIMES


def _git_source(root: Path) -> tuple[Path, str]:
    source = root / "source"
    source.mkdir(parents=True)
    for command in (
        ["git", "init", "-q", str(source)],
        ["git", "-C", str(source), "config", "user.email", "fixture@example.invalid"],
        ["git", "-C", str(source), "config", "user.name", "fixture"],
    ):
        subprocess.run(command, check=True)
    (source / "driver.cc").write_text("int driver;\n")
    (source / "compile_commands.json").write_text("[]\n")
    subprocess.run(["git", "-C", str(source), "add", "driver.cc"], check=True)
    subprocess.run(["git", "-C", str(source), "commit", "-q", "-m", "fixture"], check=True)
    commit = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    return source, commit


def _run_fixture(root: Path, cell: str, source: Path, build: Path, workload: Path) -> Path:
    corpus, profile, regime = cell.split("/")
    slug = f"s8-heldout-{finalizer.CORPUS_SLUGS[corpus]}-{finalizer.PROFILE_SLUGS[profile]}-{regime}"
    run = root / slug / "20260829T000000Z"
    run.mkdir(parents=True)
    leaf = "p50compilee2e.fixture"
    names = [
        "s7-measured-preprocessed.ii", "s7-measured-c-action-trace.jsonl",
        "s7-measured-f-action-trace.jsonl", "client-compile-measured.log",
        "out/local-measured.o", "out/remote-measured.o",
    ]
    if regime == "warm":
        names.extend(["s7-prewarm-preprocessed.ii", "s7-prewarm-c-action-trace.jsonl",
                      "s7-prewarm-f-action-trace.jsonl"])
    for name in names:
        path = run / "runtime" / leaf / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"object\n" if name.startswith("out/") else (name + "\n").encode())
    markers = [
        f"S7_MEASURED_INPUT=/x/{leaf}/s7-measured-preprocessed.ii",
        f"S7_MEASURED_C_ACTION_TRACE=/x/{leaf}/s7-measured-c-action-trace.jsonl",
        f"S7_MEASURED_F_ACTION_TRACE=/x/{leaf}/s7-measured-f-action-trace.jsonl",
    ]
    if regime == "warm":
        markers = [
            f"S7_PREWARM_INPUT=/x/{leaf}/s7-prewarm-preprocessed.ii",
            f"S7_MEASURED_INPUT=/x/{leaf}/s7-measured-preprocessed.ii",
            f"S7_PREWARM_C_ACTION_TRACE=/x/{leaf}/s7-prewarm-c-action-trace.jsonl",
            f"S7_PREWARM_F_ACTION_TRACE=/x/{leaf}/s7-prewarm-f-action-trace.jsonl",
            f"S7_MEASURED_C_ACTION_TRACE=/x/{leaf}/s7-measured-c-action-trace.jsonl",
            f"S7_MEASURED_F_ACTION_TRACE=/x/{leaf}/s7-measured-f-action-trace.jsonl",
        ]
    stdout = "\n".join([
        "ok - p50 C1F1 production wiring contract and deletion gates hold",
        *markers,
        f"PASS: all-P50 C1F1 {profile} compile is remote and byte-identical",
        "",
    ]).encode()
    (run / "run.stdout").write_bytes(stdout)
    (run / "exit-code.txt").write_text("0\n")
    config = {
        "Image": finalizer.PINNED_IMAGE,
        "Env": [
            "ICECC_TEST_TOP_SRCDIR=/src", "ICECC_TEST_TOP_BUILDDIR=/binroot",
            f"ICECC_P50_PROFILE={profile}", f"ICECC_P50_C1F1_WARM={'1' if regime == 'warm' else '0'}",
            "ICECC_P50_C1F1_SOURCE_ROOT=/corpus",
            "ICECC_P50_C1F1_SOURCE_RELATIVE=driver.ii",
            f"ICECC_P50_C1F1_COMPILE_SOURCE={workload / 'driver.cc'}",
            f"ICECC_P50_C1F1_COMPILE_DB={workload / 'compile_commands.json'}",
        ],
    }
    inspect = [{
        "State": {"Status": "exited", "ExitCode": 0, "OOMKilled": False},
        "Config": config,
        "HostConfig": {"Binds": [
            f"{source}:/src:ro", f"{build}:/binroot:ro",
            f"{run / 'runtime'}:/x:rw", f"{root / 'corpus'}:/corpus:ro",
            f"{workload}:{workload}:ro",
        ]},
    }]
    (run / "docker-inspect.json").write_text(json.dumps(inspect, sort_keys=True) + "\n")
    return run.parent


def _fixture(tmp_path: Path):
    runs = tmp_path / "runs"
    source, commit = _git_source(tmp_path)
    duckdb, duckdb_commit = _git_source(tmp_path / "duckdb-workload")
    llvm, llvm_commit = _git_source(tmp_path / "llvm-workload")
    build = tmp_path / "build"
    build.mkdir()
    for name in ("client/icecc", "daemon/iceccd", "scheduler/icecc-scheduler",
                 "cache/icecc-cache-service", "cache/sim/.p50sim.bin"):
        path = build / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(name.encode())
    (runs / "corpus").mkdir(parents=True)
    for corpus in ("DuckDB", "LLVM-1238"):
        for profile in PROFILES:
            for regime in REGIMES:
                _run_fixture(runs, f"{corpus}/{profile}/{regime}", source, build,
                             duckdb if corpus == "DuckDB" else llvm)
    bundle = tmp_path / "calibration-model-bundle.json"
    bundle_raw = b"fixture calibration bundle\n"
    bundle.write_bytes(bundle_raw)
    calibration = tmp_path / "calibration-model-manifest.json"
    calibration.write_text(json.dumps({
        "schema": finalizer.CALIBRATION_SCHEMA,
        "bundle": {"path": bundle.name, "sha256": hashlib.sha256(bundle_raw).hexdigest(),
                    "bytes": len(bundle_raw)},
    }) + "\n")
    return (runs, source, build, str(calibration),
            {"DuckDB": duckdb, "LLVM-1238": llvm},
            {"DuckDB": duckdb_commit, "LLVM-1238": llvm_commit})


def test_discover_all_sixteen_and_generate_root_commands(tmp_path: Path) -> None:
    runs, source, build, calibration_name, repos, commits = _fixture(tmp_path)
    records = finalizer.discover_runs(runs, source, build,
                                      workload_repositories=repos, workload_commits=commits)
    assert len(records) == 16
    assert {record.cell for record in records} == {
        f"{corpus}/{profile}/{regime}"
        for corpus in ("DuckDB", "LLVM-1238")
        for profile in PROFILES for regime in REGIMES
    }
    output = tmp_path.parent / (tmp_path.name + "-output")
    sim = build / "cache/sim/.p50sim.bin"
    sim_sha = hashlib.sha256(sim.read_bytes()).hexdigest()
    plan = finalizer.build_plan(records, output, Path(calibration_name), simulator_path=sim,
                                simulator_sha256=sim_sha)
    assert plan["status"] == "PASS"
    assert len(plan["commands"]) == 16
    script = (output / "run-root.sh").read_text()
    assert "--user 0" in script
    assert "cmp -s /out/experiment/runtime/run/out/local-measured.o" in script
    assert "s8_first_triple_driver.py" not in script
    assert "s8_first_triple_driver.py" in (output / "run-host.sh").read_text()
    assert ":/run:ro" in script and "/src/cache/sim/.p50sim.bin:ro" in script
    assert "mkdir -p /out/experiment/exact-replay" not in script
    assert "capture output exists without completion marker" in script
    assert "/binroot/client/icecc" in script and "/src/cache/sim/.p50sim.bin" in script
    assert "chmod" not in script and "chown" not in script
    assert finalizer.build_plan(records, output, Path(calibration_name), simulator_path=sim,
                                simulator_sha256=sim_sha) == plan


def test_cold_runner_may_retain_unused_prewarm_markers(tmp_path: Path) -> None:
    runs, source, build, _, repos, commits = _fixture(tmp_path)
    stdout_path = next((runs / "s8-heldout-duckdb-zstd-tu-cold").glob("*/run.stdout"))
    stdout = stdout_path.read_text()
    leaf = "p50compilee2e.fixture"
    prewarm = [
        f"S7_PREWARM_INPUT=/x/{leaf}/s7-prewarm-preprocessed.ii",
        f"S7_PREWARM_C_ACTION_TRACE=/x/{leaf}/s7-prewarm-c-action-trace.jsonl",
        f"S7_PREWARM_F_ACTION_TRACE=/x/{leaf}/s7-prewarm-f-action-trace.jsonl",
    ]
    stdout_path.write_text(stdout.replace(
        "S7_MEASURED_INPUT", "\n".join(prewarm) + "\nS7_MEASURED_INPUT", 1))
    records = finalizer.discover_runs(runs, source, build,
                                      workload_repositories=repos, workload_commits=commits)
    assert len(records) == 16


def test_calibration_bundle_binding_is_required(tmp_path: Path) -> None:
    runs, source, build, calibration_name, repos, commits = _fixture(tmp_path)
    records = finalizer.discover_runs(runs, source, build,
                                      workload_repositories=repos, workload_commits=commits)
    bundle = Path(calibration_name).with_name("calibration-model-bundle.json")
    bundle.write_bytes(b"tampered\n")
    with pytest.raises(finalizer.FinalizerError, match="bundle_binding_mismatch"):
        sim = build / "cache/sim/.p50sim.bin"
        finalizer.build_plan(records, tmp_path / "output", Path(calibration_name), simulator_path=sim,
                             simulator_sha256=hashlib.sha256(sim.read_bytes()).hexdigest())


@pytest.mark.parametrize("field", ("ExitCode", "OOMKilled"))
def test_bad_container_status_is_rejected(tmp_path: Path, field: str) -> None:
    runs, source, build, _, repos, commits = _fixture(tmp_path)
    inspect_path = next((runs / "s8-heldout-duckdb-zstd-tu-cold").glob("*/docker-inspect.json"))
    value = json.loads(inspect_path.read_text())
    value[0]["State"][field] = 1 if field == "ExitCode" else True
    inspect_path.write_text(json.dumps(value) + "\n")
    with pytest.raises(finalizer.FinalizerError, match="run_status:not_successful"):
        finalizer.discover_runs(runs, source, build,
                                workload_repositories=repos, workload_commits=commits)


def test_wrong_image_and_ambiguous_runtime_are_rejected(tmp_path: Path) -> None:
    runs, source, build, _, repos, commits = _fixture(tmp_path)
    inspect_path = next((runs / "s8-heldout-duckdb-zstd-tu-cold").glob("*/docker-inspect.json"))
    value = json.loads(inspect_path.read_text())
    value[0]["Config"]["Image"] = "wrong:image"
    inspect_path.write_text(json.dumps(value) + "\n")
    with pytest.raises(finalizer.FinalizerError, match="docker_image:mismatch"):
        finalizer.discover_runs(runs, source, build,
                                workload_repositories=repos, workload_commits=commits)
    value[0]["Config"]["Image"] = finalizer.PINNED_IMAGE
    inspect_path.write_text(json.dumps(value) + "\n")
    stdout_path = next((runs / "s8-heldout-duckdb-zstd-tu-cold").glob("*/run.stdout"))
    stdout_path.write_text(stdout_path.read_text().replace(
        "S7_MEASURED_F_ACTION_TRACE=/x/p50compilee2e.fixture/",
        "S7_MEASURED_F_ACTION_TRACE=/x/p50compilee2e.other/"))
    with pytest.raises(finalizer.FinalizerError, match="runtime_leaf_invalid|runtime_leaf_ambiguous"):
        finalizer.discover_runs(runs, source, build,
                                workload_repositories=repos, workload_commits=commits)


def test_workload_mapping_and_simulator_are_fail_closed(tmp_path: Path) -> None:
    runs, source, build, calibration_name, repos, commits = _fixture(tmp_path)
    with pytest.raises(finalizer.FinalizerError, match="explicit_mapping"):
        finalizer.discover_runs(runs, source, build)
    records = finalizer.discover_runs(runs, source, build,
                                      workload_repositories=repos, workload_commits=commits)
    sim = build / "cache/sim/.p50sim.bin"
    with pytest.raises(finalizer.FinalizerError, match="sha256_mismatch"):
        finalizer.build_plan(records, tmp_path / "out", Path(calibration_name),
                             simulator_path=sim, simulator_sha256="0" * 64)


def test_host_phase_has_git_backed_workload_and_all_binary_bindings(tmp_path: Path) -> None:
    runs, source, build, calibration_name, repos, commits = _fixture(tmp_path)
    records = finalizer.discover_runs(runs, source, build,
                                      workload_repositories=repos, workload_commits=commits)
    sim = build / "cache/sim/.p50sim.bin"
    digest = hashlib.sha256(sim.read_bytes()).hexdigest()
    output = tmp_path.parent / (tmp_path.name + "-out")
    finalizer.build_plan(records, output, Path(calibration_name), simulator_path=sim,
                         simulator_sha256=digest, cell="DuckDB/ZSTD_TU/cold")
    root = (output / "run-root.sh").read_text()
    host = (output / "run-host.sh").read_text()
    assert "/farmharness/s7_grz_results.py" not in root and "git -C" not in root
    assert str(repos["DuckDB"]) in host
    for name in ("client=", "daemon=", "scheduler=", "cache-service=", "simulator="):
        assert name in host
    assert "--source-commit" in host and commits["DuckDB"] in host


def test_tracked_edits_and_protected_output_overlap_are_rejected(tmp_path: Path) -> None:
    runs, source, build, calibration_name, repos, commits = _fixture(tmp_path)
    (source / "driver.cc").write_text("int edited;\n")
    with pytest.raises(finalizer.FinalizerError, match="tracked_edits_present"):
        finalizer.discover_runs(runs, source, build,
                                workload_repositories=repos, workload_commits=commits)

    clean_root = tmp_path / "clean"
    clean_root.mkdir()
    runs, source, build, calibration_name, repos, commits = _fixture(clean_root)
    records = finalizer.discover_runs(runs, source, build,
                                      workload_repositories=repos, workload_commits=commits)
    sim = build / "cache/sim/.p50sim.bin"
    digest = hashlib.sha256(sim.read_bytes()).hexdigest()
    with pytest.raises(finalizer.FinalizerError, match="overlaps_protected_input"):
        finalizer.build_plan(records, source / "generated", Path(calibration_name),
                             simulator_path=sim, simulator_sha256=digest)


def test_plan_carries_capture_and_binary_digests_for_root_reverification(tmp_path: Path) -> None:
    runs, source, build, calibration_name, repos, commits = _fixture(tmp_path)
    records = finalizer.discover_runs(runs, source, build,
                                      workload_repositories=repos, workload_commits=commits)
    sim = build / "cache/sim/.p50sim.bin"
    digest = hashlib.sha256(sim.read_bytes()).hexdigest()
    output = tmp_path.parent / (tmp_path.name + "-digest-out")
    plan = finalizer.build_plan(records, output, Path(calibration_name), simulator_path=sim,
                                simulator_sha256=digest, cell="DuckDB/ZSTD_TU/cold")
    cell = plan["cells"][0]
    assert set(cell["binaries"]) == {"client", "daemon", "scheduler", "cache-service", "simulator"}
    assert set(cell["capture_names"]) == {
        "s7-measured-preprocessed.ii", "s7-measured-c-action-trace.jsonl",
        "s7-measured-f-action-trace.jsonl", "client-compile-measured.log",
        "out/local-measured.o", "out/remote-measured.o",
    }
    root = (output / "run-root.sh").read_text()
    assert root.count("sha256sum") >= 2 * len(cell["capture_names"])


def test_unreadable_retained_capture_is_deferred_to_root_phase(tmp_path: Path) -> None:
    runs, source, build, calibration_name, repos, commits = _fixture(tmp_path)
    capture = next((runs / "s8-heldout-duckdb-zstd-tu-cold").glob(
        "*/runtime/*/s7-measured-preprocessed.ii"))
    capture.chmod(0)
    try:
        records = finalizer.discover_runs(runs, source, build,
                                          workload_repositories=repos, workload_commits=commits)
        sim = build / "cache/sim/.p50sim.bin"
        plan = finalizer.build_plan(records, tmp_path.parent / (tmp_path.name + "-deferred"),
                                    Path(calibration_name), simulator_path=sim,
                                    simulator_sha256=hashlib.sha256(sim.read_bytes()).hexdigest(),
                                    cell="DuckDB/ZSTD_TU/cold")
        assert plan["cells"][0]["capture_names"]
        assert "test ! -L" in plan["commands"][0]
    finally:
        capture.chmod(0o644)


def test_host_phase_rejects_mutated_copied_capture(tmp_path: Path) -> None:
    runs, source, build, calibration_name, repos, commits = _fixture(tmp_path)
    records = finalizer.discover_runs(runs, source, build,
                                      workload_repositories=repos, workload_commits=commits)
    sim = build / "cache/sim/.p50sim.bin"
    digest = hashlib.sha256(sim.read_bytes()).hexdigest()
    output = tmp_path.parent / (tmp_path.name + "-mutation-out")
    finalizer.build_plan(records, output, Path(calibration_name), simulator_path=sim,
                         simulator_sha256=digest, cell="DuckDB/ZSTD_TU/cold")
    record = next(item for item in records if item.cell == "DuckDB/ZSTD_TU/cold")
    cell = output / "DuckDB--ZSTD_TU--cold"
    runtime = cell / "experiment/runtime/run"
    leaf = record.timestamp_dir / "runtime" / record.runtime_leaf
    for name in record.capture_names:
        destination = runtime / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(leaf / name, destination)
    captures = {}
    for name in record.capture_names:
        src = leaf / name
        dst = runtime / name
        source_raw, destination_raw = src.read_bytes(), dst.read_bytes()
        def descriptor(path: Path, raw: bytes) -> dict[str, object]:
            return {"path": str(path), "bytes": len(raw),
                    "sha256": hashlib.sha256(raw).hexdigest()}
        source_descriptor = descriptor(src, source_raw)
        source_descriptor["path"] = "/run/" + record.runtime_leaf + "/" + name
        captures[name] = {"source": source_descriptor,
                          "destination": descriptor(dst, destination_raw)}
    manifest = {"schema": "icecream-s8-heldout-capture-manifest-v1", "cell": record.cell,
                "runtime_leaf": record.runtime_leaf,
                "run_stdout_sha256": record.run_stdout_sha256,
                "docker_inspect_sha256": record.inspect_sha256,
                "captures": captures, "binaries": json.loads(
                    (output / "plan.json").read_text())["cells"][0]["binaries"]}
    (cell / "capture-manifest.json").write_text(
        json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n")
    (cell / "replay-complete.json").write_text("{}\n")
    (runtime / "s7-measured-preprocessed.ii").write_bytes(b"tampered\n")
    result = subprocess.run([str(output / "run-host.sh")], text=True,
                            capture_output=True)
    assert result.returncode != 0
    assert "destination mutation" in result.stderr


def _phase_canary(tmp_path: Path):
    """Run generated root copy/manifest and host authentication prefixes."""
    runs, source, build, calibration_name, repos, commits = _fixture(tmp_path / "fixture")
    records = finalizer.discover_runs(runs, source, build,
                                      workload_repositories=repos, workload_commits=commits)
    record = next(item for item in records if item.cell == "DuckDB/ZSTD_TU/cold")
    simulator = build / "cache/sim/.p50sim.bin"
    simulator_sha = hashlib.sha256(simulator.read_bytes()).hexdigest()
    output = tmp_path / "output"
    finalizer.build_plan(records, output, Path(calibration_name), simulator_path=simulator,
                         simulator_sha256=simulator_sha, cell=record.cell)
    plan = json.loads((output / "plan.json").read_text())
    cell = output / "DuckDB--ZSTD_TU--cold"
    root = finalizer._root_pipeline_script(record, plan["cells"][0]["binaries"])
    root_lines = root.splitlines()
    replay_line = next(index for index, line in enumerate(root_lines)
                       if "s7_warm_replay.py" in line)
    root = "\n".join(root_lines[:replay_line] + [
        "printf '%s\\n' '{\"schema\":\"icecream-s8-heldout-finalizer-replay-v1\",\"status\":\"PASS\"}' > /out/replay-complete.json",
    ]) + "\n"
    # Emulate the two Docker bind mounts while preserving the generated checks.
    root = (root.replace("/out/experiment/runtime/run", "__RUNTIME_OUT__")
                 .replace("/out/experiment", "__EXP__")
                 .replace("/out/replay-complete.json", "__REPLAY__")
                 .replace("/out/capture-manifest.json", "__MANIFEST__")
                 .replace("/src/cache/sim/.p50sim.bin", str(simulator))
                 .replace("/metadata", str(record.timestamp_dir))
                 .replace("/run/", str(record.runtime_root) + "/")
                 .replace("__EXP__", str(cell / "experiment"))
                 .replace("__RUNTIME_OUT__", str(cell / "experiment/runtime/run"))
                 .replace("__REPLAY__", str(cell / "replay-complete.json"))
                 .replace("__MANIFEST__", str(cell / "capture-manifest.json"))
                 .replace("/src", str(source))
                 .replace("/binroot", str(build))
                 .replace("sf, df = fact(src), fact(dst)",
                          "sf, df = fact(src), fact(dst); sf['path'] = '/run/' + leaf + '/' + name"))
    root_path = tmp_path / "run-root-prefix.sh"
    root_path.write_text(root)
    root_result = subprocess.run(["bash", str(root_path)], text=True, capture_output=True)
    host = finalizer._host_pipeline_script(record, cell, Path(calibration_name), simulator,
                                            simulator_sha, plan["cells"][0]["binaries"])
    host_lines = host.splitlines()
    normalizer_line = next(index for index, line in enumerate(host_lines)
                           if "s7_grz_results.py" in line)
    host_path = tmp_path / "run-host-prefix.sh"
    host_path.write_text("\n".join(host_lines[:normalizer_line] + ["exit 0"]) + "\n")
    host_result = subprocess.run(["bash", str(host_path)], text=True, capture_output=True)
    return record, cell, root_result, host_result, host_path


def test_generated_root_and_host_authentication_positive_canary(tmp_path: Path) -> None:
    _, cell, root_result, host_result, _ = _phase_canary(tmp_path)
    assert root_result.returncode == 0, root_result.stderr
    assert host_result.returncode == 0, host_result.stderr
    manifest = json.loads((cell / "capture-manifest.json").read_text())
    assert len(manifest["captures"]) == 6


def test_host_phase_rejects_capture_source_identity_mismatch(tmp_path: Path) -> None:
    record, cell, root_result, _, host_path = _phase_canary(tmp_path)
    assert root_result.returncode == 0, root_result.stderr
    manifest_path = cell / "capture-manifest.json"
    manifest = json.loads(manifest_path.read_text())
    item = manifest["captures"]["s7-measured-preprocessed.ii"]
    item["source"]["path"] = "/run/" + record.runtime_leaf + "/out/local-measured.o"
    manifest_path.write_text(json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n")
    result = subprocess.run(["bash", str(host_path)], text=True, capture_output=True)
    assert result.returncode != 0
    assert "source identity" in result.stderr


def test_root_phase_rejects_mutated_planner_metadata(tmp_path: Path) -> None:
    runs, source, build, calibration_name, repos, commits = _fixture(tmp_path / "fixture")
    records = finalizer.discover_runs(runs, source, build,
                                      workload_repositories=repos, workload_commits=commits)
    record = next(item for item in records if item.cell == "DuckDB/ZSTD_TU/cold")
    simulator = build / "cache/sim/.p50sim.bin"
    simulator_sha = hashlib.sha256(simulator.read_bytes()).hexdigest()
    output = tmp_path / "output"
    finalizer.build_plan(records, output, Path(calibration_name), simulator_path=simulator,
                         simulator_sha256=simulator_sha, cell=record.cell)
    record.timestamp_dir.joinpath("run.stdout").write_bytes(
        record.timestamp_dir.joinpath("run.stdout").read_bytes() + b"late mutation\n")
    plan = json.loads((output / "plan.json").read_text())
    generated_root = finalizer._root_pipeline_script(record, plan["cells"][0]["binaries"])
    assert "/metadata/run.stdout" in generated_root
    assert "/metadata/docker-inspect.json" in generated_root
    lines = generated_root.splitlines()
    cut = next(index for index, line in enumerate(lines) if "s7_warm_replay.py" in line)
    root = "\n".join(lines[:cut]) + "\n"
    cell = output / "DuckDB--ZSTD_TU--cold"
    root = (root.replace("/out/experiment/runtime/run", "__RUNTIME_OUT__")
                 .replace("/out/experiment", "__EXP__")
                 .replace("/out/capture-manifest.json", "__MANIFEST__")
                 .replace("/src/cache/sim/.p50sim.bin", str(simulator))
                 .replace("/metadata", str(record.timestamp_dir))
                 .replace("/run/", str(record.runtime_root) + "/")
                 .replace("__EXP__", str(cell / "experiment"))
                 .replace("__RUNTIME_OUT__", str(cell / "experiment/runtime/run"))
                 .replace("__MANIFEST__", str(cell / "capture-manifest.json"))
                 .replace("/src", str(source))
                 .replace("/binroot", str(build)))
    root_path = tmp_path / "run-root-prefix.sh"
    root_path.write_text(root)
    result = subprocess.run(["bash", str(root_path)], text=True, capture_output=True)
    assert result.returncode != 0
    assert "metadata mutation:run.stdout" in result.stderr
