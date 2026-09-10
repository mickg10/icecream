from __future__ import annotations

import json
from pathlib import Path
import subprocess
import threading
import time

import pytest

from farmharness.integration.tests import farm_fixture

from farmharness.integration import farmtest, workload as workload_module
from farmharness.integration.farm_spec import load_farm_spec
from farmharness.integration.images import CommandFactory, RecordingTransport
from farmharness.integration.remote import (
    CommandResult,
    PlannedCommand,
    decode_ssh_payload,
)
from farmharness.integration.scenario_spec import ScenarioSpecError, load_scenario_spec
from farmharness.integration.workload import (
    MANIFEST_DRIVER,
    WORKLOAD_SCHEMA,
    WorkloadError,
    _active_loss_serial_through,
    _driver_command,
    _parse_summary,
    _strict_p50_required,
    run_workload,
)


INTEGRATION = Path(__file__).resolve().parents[1]


def _farm_scenario_plan(tmp_path: Path):
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(INTEGRATION / "scenarios" / "S00-smoke.json", farm)
    plan = farmtest.build_plan(farm, scenario, run_id="workload-unit")
    return farm, scenario, plan


class WorkloadRecorder:
    def __init__(
        self, stdout: str = "ICEFARM_WORKLOAD jobs=100 failures=0 samples=3\n"
    ) -> None:
        self.stdout = stdout
        self.commands: list[PlannedCommand] = []

    def invoke(self, command: PlannedCommand) -> CommandResult:
        self.commands.append(command)
        return CommandResult(0, self.stdout, "")


class PairedWorkloadRecorder(WorkloadRecorder):
    def invoke(self, command: PlannedCommand) -> CommandResult:
        self.commands.append(command)
        if command.phase == "run.corpus-clear":
            return CommandResult(0, f"cleared {command.argv[-1]}\n", "")
        if command.phase == "run.corpus-materialize":
            return CommandResult(0, "materialized /icefarm-corpus-cache/active\n", "")
        if command.phase == "run.corpus-activate":
            return CommandResult(0, f"activated {command.argv[-5]}\n", "")
        return CommandResult(0, self.stdout, "")


def test_plan_mounts_authenticated_corpus_read_only_and_closure_scoped_oracle(
    tmp_path: Path,
) -> None:
    _farm, _scenario, plan = _farm_scenario_plan(tmp_path)
    client = next(item for item in plan["commands"] if item["phase"] == "up.start-c")
    argv = client["argv"]
    corpus_mount = next(item for item in argv if item.endswith("dst=/corpus,readonly"))
    oracle_mount = next(item for item in argv if item.endswith("dst=/oracle"))
    assert "/workload-unit/C1/input," in corpus_mount
    client = next(item for item in plan["topology"]["instances"] if item["role"] == "C")
    assert client["container_image"]["closure_sha256"] not in oracle_mount
    assert "/oracle/fmt-100/" in oracle_mount
    for environment in (
        "ICECC_P50_COMPILE_IDENTITY_TRACE=/results/compile-identity.jsonl",
        "ICECC_P50_C_ACTION_TRACE=/results/c-action.jsonl",
        "ICECC_P50_C_LEGACY_WIRE_TRACE=/results/c-legacy-wire.jsonl",
    ):
        assert environment in argv


def test_manifest_driver_is_one_fixed_program_with_all_spec_values_in_argv(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    scripted = WorkloadRecorder()
    transport = RecordingTransport(scripted)
    receipt = run_workload(
        farm,
        scenario,
        plan,
        recorder=transport,
        require_up=False,
    )
    assert receipt["schema"] == WORKLOAD_SCHEMA
    assert receipt["status"] == "COMPLETE"
    assert receipt["clients"] == [
        {"client": "C1", "failures": 0, "jobs": 100, "samples": 3}
    ]
    assert len(scripted.commands) == 1
    argv = scripted.commands[0].argv
    assert MANIFEST_DRIVER in argv
    driver_index = argv.index(MANIFEST_DRIVER)
    assert argv[driver_index + 12] == "1"
    assert argv[-4:] == ("A", "", "", "0")
    assert "/usr/bin/g++" in argv
    assert argv[-6:-4] == ("-O2", "-fdiagnostics-color=never")
    assert "fmt-100" not in MANIFEST_DRIVER
    assert "/usr/bin/g++-11" not in MANIFEST_DRIVER
    assert "oracle_recipe=icecream-clang-remote-v1" in MANIFEST_DRIVER
    assert '-Xclang -main-file-name -Xclang "$source"' in MANIFEST_DRIVER
    assert '-Xclang -fdebug-compilation-dir -Xclang "$PWD"' in MANIFEST_DRIVER
    assert '-c -target "$compiler_target" - -o "$object"' in MANIFEST_DRIVER
    assert '"$compiler" "${oracle_compiler_args[@]}" -c "$source"' in MANIFEST_DRIVER
    assert MANIFEST_DRIVER.count('oracle_compile "$source" "$object"') == 2
    assert "'building myself, but telling localhost'" in MANIFEST_DRIVER
    assert "'<building_local>'" in MANIFEST_DRIVER
    assert '"$remote" -eq 1' in MANIFEST_DRIVER
    assert "printf 'OPEN\\t0\\n' >\"$gate_state\"" in MANIFEST_DRIVER
    assert 'flock -x 8' in MANIFEST_DRIVER
    assert 'mv -- "$temporary_marker" "$marker"' in MANIFEST_DRIVER
    assert 'case "$gate_mode" in' in MANIFEST_DRIVER
    assert 'QUIESCE)' in MANIFEST_DRIVER
    assert 'write_checkpoint' in MANIFEST_DRIVER
    assert 'checkpoint_sha256' in MANIFEST_DRIVER
    assert 'resume_mode' in MANIFEST_DRIVER
    assert 'event gate aborted at epoch $gate_epoch' in MANIFEST_DRIVER
    assert 'rm -f -- "$marker"' in MANIFEST_DRIVER
    assert "event_serial_through=${ICEFARM_EVENT_SERIAL_THROUGH:-0}" in MANIFEST_DRIVER
    assert 'predecessor_ready=0' in MANIFEST_DRIVER
    assert 'active_count=$(find "$gate_active"' in MANIFEST_DRIVER
    assert 'serial_boundary_released=0' in MANIFEST_DRIVER
    assert 'event release wait expired for serial boundary job' in MANIFEST_DRIVER
    assert MANIFEST_DRIVER.index('serial_boundary_released=0') < MANIFEST_DRIVER.index(
        '>"$job_dir/result.tsv"'
    )
    assert 'xargs -0 -r -n 3 -P "$jobs"' in MANIFEST_DRIVER
    assert 'object="$oracle_root/.build-$key-$BASHPID.o"' in MANIFEST_DRIVER
    assert MANIFEST_DRIVER.index("xargs -0 -r -n 3") < MANIFEST_DRIVER.index(
        'printf \'%s\\n\' "$oracle_identity"'
    )
    persisted = json.loads(
        (tmp_path / "results" / "workload-unit" / "workload.json").read_text()
    )
    assert persisted == receipt


def test_s30_mutant_workload_enables_the_legacy_recovery_under_test(
    tmp_path: Path,
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S30-mutant-f-refusal.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="s30-mutant-workload-unit")
    scripted = WorkloadRecorder()
    run_workload(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(scripted),
        require_up=False,
    )
    argv = scripted.commands[0].argv
    driver_index = argv.index(MANIFEST_DRIVER)
    assert argv[driver_index + 12] == "0"


@pytest.mark.parametrize(
    ("scenario_id", "expected"),
    (
        ("S80-legacy", False),
        ("S80-p29v1", True),
        ("S70-b6-kill-switch", False),
        ("S70-b5-interner-failure", True),
        ("S95-cache-disk-full", False),
        ("S40-full-newgen-engagement", True),
        ("S60-11-warm-f2-down", False),
        ("S60-13-warm-s-down", False),
        ("H2-client-kill-switch", False),
        ("S70-b4-scheduler-active-loss", False),
    ),
)
def test_strict_p50_tracks_whole_run_engagement_contract(
    tmp_path: Path, scenario_id: str, expected: bool
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / f"{scenario_id}.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id=f"strict-{scenario_id}")

    assert _strict_p50_required(scenario, plan) is expected


def test_strict_p50_requires_the_s70_b6_off_transition() -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S70-b6-kill-switch.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="strict-b6-mutation")
    scenario.data["timeline"][0]["env"]["ICECC_P50_PROFILE"] = "P29V1"

    assert _strict_p50_required(scenario, plan) is True


def test_active_loss_serializes_exact_trigger_prefix_and_requires_remote(
    tmp_path: Path,
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S70-b4-scheduler-active-loss.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="active-loss-workload-unit")
    assert _active_loss_serial_through(scenario) == 2

    client = next(
        item for item in plan["topology"]["instances"] if item["role"] == "C"
    )
    command = _driver_command(
        farm,
        scenario,
        plan,
        client,
        "A",
        CommandFactory(),
    )
    argv = command.argv
    assert "ICEFARM_EVENT_SERIAL_THROUGH=2" in argv
    assert "ICECC_REMOTE_REQUIRED=1" in argv

    ordinary = load_scenario_spec(
        INTEGRATION / "scenarios" / "S00-smoke.json", farm
    )
    assert _active_loss_serial_through(ordinary) == 0


def test_manifest_driver_shell_is_syntactically_valid() -> None:
    subprocess.run(
        ["/bin/bash", "-n"],
        input=MANIFEST_DRIVER,
        text=True,
        check=True,
        capture_output=True,
    )
    assert "scenario.data" not in MANIFEST_DRIVER


def test_manifest_driver_relaunch_replaces_oracle_samples_atomically(
    tmp_path: Path,
) -> None:
    start = MANIFEST_DRIVER.index("sample_bucket=$((16#")
    end = MANIFEST_DRIVER.index("\nflock -u 9", start)
    sample_program = MANIFEST_DRIVER[start:end]
    result_root = tmp_path / "results"
    oracle_root = tmp_path / "oracle"
    result_root.mkdir()
    (result_root / "oracle-samples").mkdir()
    oracle_root.mkdir()
    source = tmp_path / "source.ii"
    source.write_bytes(b"canonical object bytes\n")
    relative = "files/source.ii"
    digest = "a" * 64
    unique = tmp_path / "unique.tsv"
    unique.write_text(
        f"{digest}\t{relative}\t{source}\n",
        encoding="utf-8",
    )
    key = subprocess.run(
        ["sha256sum"],
        input=f"{digest}\n{relative}\n",
        text=True,
        check=True,
        capture_output=True,
    ).stdout.split()[0]
    observed = subprocess.run(
        ["sha256sum", str(source)],
        text=True,
        check=True,
        capture_output=True,
    ).stdout.split()[0]
    (oracle_root / f"{key}.sha256").write_text(observed + "\n", encoding="ascii")
    prefix = f"""
set -eu
client_name=C1
manifest_digest={'b' * 64}
result_root={result_root}
oracle_root={oracle_root}
unique={unique}
oracle_compile() {{ cp -- "$1" "$2"; }}
"""

    for _ in range(2):
        subprocess.run(
            ["/bin/bash"],
            input=prefix + sample_program,
            text=True,
            check=True,
            capture_output=True,
        )

    samples = (result_root / "oracle-samples.tsv").read_text(
        encoding="utf-8"
    ).splitlines()
    assert len(samples) == 1
    assert samples[0].split("\t") == [relative, observed, observed, "1"]
    assert (result_root / "oracle-summary.tsv").read_text(encoding="utf-8") == (
        "sample_total\t1\nsample_mismatches\t0\n"
    )


def test_workload_summary_is_fail_closed(tmp_path: Path) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    transport = RecordingTransport(WorkloadRecorder("not a summary\n"))
    with pytest.raises(WorkloadError, match="no unique workload summary"):
        run_workload(
            farm,
            scenario,
            plan,
            recorder=transport,
            require_up=False,
        )


def test_nonzero_workload_result_cannot_be_accepted_with_forged_summary() -> None:
    with pytest.raises(WorkloadError, match=r"failed rc=7: compiler root cause"):
        _parse_summary(
            CommandResult(7, "ICEFARM_WORKLOAD jobs=100 failures=0 samples=3\n", "compiler root cause"),
            "C1",
        )


def _checkpoint_writer_python() -> str:
    marker = 'python3 - "$checkpoint_tmp" "$result_root" "$worklist" "$client_name" "$turn" "$expected_jobs" <<\'PY\'\n'
    start = MANIFEST_DRIVER.index(marker) + len(marker)
    end = MANIFEST_DRIVER.index("\nPY\n", start)
    return MANIFEST_DRIVER[start:end]


def _run_checkpoint_writer(tmp_path: Path, relative: str, *, symlink: bool = False):
    result_root = tmp_path / "results"
    jobs = result_root / "jobs"
    jobs.mkdir(parents=True)
    worklist = tmp_path / "worklist"
    worklist.write_text("work\n", encoding="utf-8")
    path = result_root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    if symlink:
        target = tmp_path / "outside.tsv"
        target.write_text("1\tA\tx\tx\tx\tx\tx\tx\t0\tx\tx\t1\t1\tx\n", encoding="utf-8")
        path.symlink_to(target)
    else:
        path.write_text("1\tA\tx\tx\tx\tx\tx\tx\t0\tx\tx\t1\t1\tx\n", encoding="utf-8")
    output = result_root / "checkpoint.json"
    result = subprocess.run(
        ["python3", "-c", _checkpoint_writer_python(), str(output), str(result_root), str(worklist), "C1", "A", "1"],
        text=True,
        capture_output=True,
    )
    return result, output


@pytest.mark.parametrize(
    ("relative", "symlink"),
    (("jobs/evil/result.tsv", False), ("jobs/000001/result.tsv", True)),
)
def test_checkpoint_writer_refuses_malicious_result_identity(
    tmp_path: Path, relative: str, symlink: bool
) -> None:
    result, output = _run_checkpoint_writer(tmp_path, relative, symlink=symlink)
    assert result.returncode != 0
    assert not output.exists()


def test_checkpoint_writer_accepts_only_exact_result_identity(tmp_path: Path) -> None:
    result, output = _run_checkpoint_writer(tmp_path, "jobs/000001/result.tsv")
    assert result.returncode == 0, result.stderr
    document = json.loads(output.read_text(encoding="utf-8"))
    assert document["completed_rows"][0]["path"] == "jobs/000001/result.tsv"


def test_control_workload_disables_strict_mode_to_observe_the_fault(
    tmp_path: Path,
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "H2-client-kill-switch.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="control-workload-unit")
    scripted = WorkloadRecorder()

    run_workload(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(scripted),
        require_up=False,
    )

    argv = scripted.commands[0].argv
    driver_argv0 = argv.index("icefarm-manifest-driver")
    assert argv[driver_argv0 + 11] == "0"


def test_h4_passes_one_typed_object_corruption_fault_in_driver_argv(
    tmp_path: Path,
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "H4-corrupt-object.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="h4-workload-unit")
    scripted = WorkloadRecorder("ICEFARM_WORKLOAD jobs=100 failures=1 samples=3\n")

    receipt = run_workload(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(scripted),
        require_up=False,
    )

    assert receipt["status"] == "COMPLETE_WITH_JOB_FAILURES"
    assert scripted.commands[0].argv[-4:] == ("A", "corrupt-object", "C1", "1")
    assert "before_sha=$(sha256sum" in MANIFEST_DRIVER
    assert "icefarm-h4-object-fault-v1" in MANIFEST_DRIVER


def test_workload_requires_authenticated_up_receipt(tmp_path: Path) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    with pytest.raises(WorkloadError, match="cannot load UP lifecycle receipt"):
        run_workload(
            farm,
            scenario,
            plan,
            recorder=RecordingTransport(WorkloadRecorder()),
        )


def test_single_manifest_refuses_an_undefined_turn(tmp_path: Path) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    scenario = json.loads((INTEGRATION / "scenarios" / "S00-smoke.json").read_text())
    scenario["workload"]["turns"] = ["B"]
    path = tmp_path / "scenario.json"
    path.write_text(json.dumps(scenario), encoding="utf-8")
    with pytest.raises(ScenarioSpecError, match="does not define turns"):
        load_scenario_spec(path, farm)


def test_ssh_transport_keeps_driver_values_inside_encoded_argv(tmp_path: Path) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    farm.hosts["tt-quietbox3"].pop("docker_context")
    plan = farmtest.build_plan(farm, scenario, run_id="workload-ssh")
    scripted = WorkloadRecorder()
    run_workload(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(scripted),
        require_up=False,
    )
    decoded = decode_ssh_payload(scripted.commands[0].argv)
    assert decoded[:3] == ("docker", "exec", "--user")
    assert MANIFEST_DRIVER in decoded


def test_paired_workload_materializes_only_b_between_sequential_turns(
    tmp_path: Path,
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S80-p29v1.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="paired-unit")
    scripted = PairedWorkloadRecorder()
    receipt = run_workload(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(scripted),
        require_up=False,
    )
    assert receipt["clients"] == [
        {"client": "C1", "failures": 0, "jobs": 200, "samples": 6}
    ]
    assert [item["turn"] for item in receipt["turns"]] == ["A", "B"]
    assert [command.phase for command in scripted.commands] == [
        "run.workload",
        "run.corpus-input-mkdir",
        "run.corpus-clear",
        "run.corpus-materialize",
        "run.corpus-activate",
        "run.workload",
    ]
    drivers = [
        command for command in scripted.commands if command.phase == "run.workload"
    ]
    assert drivers[0].argv[-4] == "A"
    assert drivers[1].argv[-4] == "B"
    assert "/results/workload/A" in drivers[0].argv
    assert "/results/workload/B" in drivers[1].argv
    assert 'printf \'%s\\n%s\\n\' "$digest" "$relative"' in MANIFEST_DRIVER


def test_s50_runs_old_and_new_clients_concurrently_in_one_turn(
    tmp_path: Path,
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S50-mixed-pool-fmt.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="s50-workload-unit")
    scripted = WorkloadRecorder()
    receipt = run_workload(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(scripted),
        require_up=False,
    )
    assert receipt["clients"] == [
        {"client": "C1", "failures": 0, "jobs": 100, "samples": 3},
        {"client": "C2", "failures": 0, "jobs": 100, "samples": 3},
    ]
    commands = [
        command for command in scripted.commands if command.phase == "run.workload"
    ]
    assert len(commands) == 2
    assert {command.instance for command in commands} == {"C1", "C2"}
    assert all(command.argv[-4] == "A" for command in commands)


@pytest.mark.parametrize("failure", (False, True))
def test_checkpointed_turn_stays_active_after_initial_futures_finish(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, failure: bool
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S60-06-c2-down.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="checkpoint-turn-lease")

    class RaceRecorder(WorkloadRecorder):
        def __init__(self) -> None:
            super().__init__()
            self.initial = 0
            self.initial_done = threading.Event()
            self.lock = threading.Lock()

        def invoke(self, command: PlannedCommand) -> CommandResult:
            self.commands.append(command)
            if command.phase == "event.checkpoint":
                return CommandResult(
                    0,
                    json.dumps(
                        {
                            "checkpoint_sha256": "a" * 64,
                            "expected_jobs": 100,
                        }
                    ),
                    "",
                )
            if command.phase == "run.workload":
                with self.lock:
                    if not self.initial_done.is_set():
                        self.initial += 1
                        if self.initial == 2:
                            self.initial_done.set()
                return CommandResult(
                    0, "ICEFARM_WORKLOAD jobs=100 failures=0 samples=3\n", ""
                )
            return CommandResult(0, "", "")

    recorder = RaceRecorder()

    class DelayedCheckpointProducer:
        def __init__(self, _farm, _scenario, event_plan, **kwargs) -> None:
            self.plan = event_plan
            self.quiesce = kwargs["quiesce_workload"]
            self.relaunch = kwargs["relaunch_workload"]
            self.error: BaseException | None = None
            self.thread: threading.Thread | None = None

        def start(self) -> None:
            return None

        def signal_turn_start(self, turn: str) -> None:
            clients = tuple(
                item
                for item in self.plan["topology"]["instances"]
                if item["role"] == "C"
            )

            def delayed() -> None:
                try:
                    assert recorder.initial_done.wait(timeout=1)
                    # Reproduce A6's ordering: workload futures have become
                    # terminal while the event callback is still delayed.
                    time.sleep(0.05)
                    checkpoints = self.quiesce(turn, clients)
                    if failure:
                        raise WorkloadError("injected delayed checkpoint failure")
                    self.relaunch(turn, checkpoints)
                except BaseException as exc:
                    self.error = exc

            self.thread = threading.Thread(target=delayed)
            self.thread.start()

        def signal_turn_complete(self, _turn: str) -> None:
            return None

        def raise_if_failed(self) -> None:
            if self.error is not None:
                raise self.error

        def wait(self) -> None:
            assert self.thread is not None
            self.thread.join(timeout=2)
            assert not self.thread.is_alive()
            self.raise_if_failed()

        def stop(self) -> None:
            if self.thread is not None:
                self.thread.join(timeout=2)
                assert not self.thread.is_alive()
            self.raise_if_failed()

    monkeypatch.setattr(workload_module, "EventProducer", DelayedCheckpointProducer)
    if failure:
        with pytest.raises(WorkloadError, match="injected delayed checkpoint failure"):
            run_workload(
                farm,
                scenario,
                plan,
                recorder=RecordingTransport(recorder),
                require_up=False,
            )
        return

    receipt = run_workload(
        farm, scenario, plan, recorder=RecordingTransport(recorder), require_up=False
    )
    assert receipt["status"] == "COMPLETE"
    assert [item["client"] for item in receipt["clients"]] == ["C1", "C2"]
    assert sum(command.phase == "event.checkpoint" for command in recorder.commands) == 2


def test_checkpointed_client_transition_refuses_ambiguous_multiple_turns(
    tmp_path: Path,
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S60-06-c2-down.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="checkpoint-multiple-turns")
    scenario.data["workload"]["turns"] = ["A", "B"]

    with pytest.raises(WorkloadError, match="one unambiguous workload turn"):
        run_workload(
            farm,
            scenario,
            plan,
            recorder=RecordingTransport(WorkloadRecorder()),
            require_up=False,
        )


def test_checkpointed_client_transition_missing_trigger_times_out(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S60-06-c2-down.json", farm
    )
    scenario.data["timeouts"]["turn_s"] = 1
    plan = farmtest.build_plan(farm, scenario, run_id="checkpoint-missing-trigger")

    class NeverTriggeredProducer:
        def __init__(self, *_args, **_kwargs) -> None:
            return None

        def start(self) -> None:
            return None

        def signal_turn_start(self, _turn: str) -> None:
            return None

        def signal_turn_complete(self, _turn: str) -> None:
            return None

        def raise_if_failed(self) -> None:
            return None

        def wait(self) -> None:
            return None

        def stop(self) -> None:
            return None

    monkeypatch.setattr(workload_module, "EventProducer", NeverTriggeredProducer)
    with pytest.raises(
        WorkloadError, match="checkpointed C transition did not relaunch"
    ):
        run_workload(
            farm,
            scenario,
            plan,
            recorder=RecordingTransport(WorkloadRecorder()),
            require_up=False,
        )
