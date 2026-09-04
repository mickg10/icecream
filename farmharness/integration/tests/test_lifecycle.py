from __future__ import annotations

import json
from pathlib import Path

import pytest

from farmharness.integration import farmtest
from farmharness.integration.farm_spec import load_farm_spec
from farmharness.integration.images import RecordingTransport, _image_identity
from farmharness.integration.lifecycle import (
    LifecycleError,
    MIN_FREE_BYTES,
    PreflightRefusal,
    ROLE_BINARY_PATHS,
    bring_up,
    corpus_layout,
    down_from_state,
)
from farmharness.integration.remote import CommandResult, PlannedCommand, RemoteError
from farmharness.integration.scenario_spec import load_scenario_spec


INTEGRATION = Path(__file__).resolve().parents[1]
NATIVE_ID = "sha256:" + "1" * 64


class Clock:
    def __init__(self) -> None:
        self.value = 0.0

    def __call__(self) -> float:
        return self.value

    def sleep(self, seconds: float) -> None:
        self.value += seconds


def _inspect_document(farm, label: str) -> dict[str, object]:
    authority = farm.data["authority"]["images"][label]
    return {
        "Architecture": "amd64",
        "Config": {
            "Env": ["PATH=/usr/bin"],
            "Labels": {
                "icefarm.source.archive_sha256": authority["archive_sha256"],
                "icefarm.source.commit": authority["commit"],
            },
        },
        "Created": "2026-09-04T00:00:00Z",
        "Id": NATIVE_ID,
        "Os": "linux",
        "RootFS": {"Layers": ["sha256:" + "2" * 64], "Type": "layers"},
        "Size": 500_000_000,
    }


def _farm_scenario_plan(tmp_path: Path, *, up_s: int = 5):
    farm = load_farm_spec(INTEGRATION / "farm.example.json")
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(INTEGRATION / "scenarios" / "S00-smoke.json", farm)
    label = scenario.data["images"]["new"]
    document = _inspect_document(farm, label)
    identity = _image_identity(
        CommandResult(0, json.dumps(document), ""), "test image"
    )
    farm.data["authority"]["images"][label]["closure_sha256"] = identity.closure_sha256
    scenario.data["timeouts"]["up_s"] = up_s
    plan = farmtest.build_plan(farm, scenario, run_id="unit-run")
    return farm, scenario, plan


class ScriptedLifecycle:
    def __init__(
        self,
        farm,
        *,
        scheduler_interrupt: bool = False,
        scheduler_timeout: bool = False,
        stale: bool = False,
        free_bytes: int = 100_000_000_000,
        bad_role_hash: bool = False,
    ) -> None:
        self.farm = farm
        self.scheduler_interrupt = scheduler_interrupt
        self.scheduler_timeout = scheduler_timeout
        self.free_bytes = free_bytes
        self.bad_role_hash = bad_role_hash
        self.protected_count = 1
        self.containers: dict[str, dict[str, str]] = {}
        if stale:
            self.containers["aaaaaaaaaaaa"] = {
                "created": "2026-09-01T00:00:00Z",
                "host": "tt-quietbox3",
                "name": "icefarm-old-run-S1",
                "run_id": "old-run",
            }

    def _containers_on(self, host: str) -> list[str]:
        return [key for key, value in self.containers.items() if value["host"] == host]

    def invoke(self, command: PlannedCommand) -> CommandResult:
        if command.phase == "preflight.stale-list":
            return CommandResult(0, "\n".join(self._containers_on(command.host)) + "\n", "")
        if command.phase == "preflight.stale-inspect":
            item = self.containers[command.argv[-1]]
            return CommandResult(
                0,
                json.dumps(
                    {
                        "Config": {"Labels": {"icefarm.run": item["run_id"]}},
                        "Created": item["created"],
                        "Name": "/" + item["name"],
                    }
                ),
                "",
            )
        if command.phase == "preflight.host":
            return CommandResult(
                0,
                json.dumps(
                    {
                        "device": "/dev/nvme0n1p2",
                        "free_bytes": self.free_bytes,
                        "protected": {"bigfarm": self.protected_count},
                        "rotational": False,
                        "scratch_root": self.farm.hosts[command.host]["scratch_root"],
                        "write_bps": 900_000_000,
                    }
                ),
                "",
            )
        if command.phase == "preflight.image":
            label = command.argv[-1].rsplit(":", 1)[-1]
            return CommandResult(0, json.dumps(_inspect_document(self.farm, label)), "")
        if command.phase == "preflight.role-hashes":
            binary_names = {"S": "scheduler", "C": "client", "F": "daemon"}
            rows = []
            for role, path in ROLE_BINARY_PATHS.items():
                if path not in command.argv:
                    continue
                digest = self.farm.data["authority"]["role_stores"]["50"][
                    binary_names[role]
                ]["sha256"]
                if self.bad_role_hash and not rows:
                    digest = "f" * 64
                rows.append(f"{digest}  {path}")
            return CommandResult(0, "\n".join(rows) + "\n", "")
        if command.phase == "preflight.stale-reap":
            self.containers.pop(command.argv[-1])
            return CommandResult(0, "", "")
        if command.phase.startswith("up.start-"):
            name = command.argv[command.argv.index("--name") + 1]
            run_label = command.argv[command.argv.index("--label") + 1].split("=", 1)[1]
            identifier = f"{len(self.containers) + 1:012x}"
            self.containers[identifier] = {
                "created": "2026-09-04T00:00:00Z",
                "host": command.host,
                "name": name,
                "run_id": run_label,
            }
            return CommandResult(0, identifier + "\n", "")
        if command.phase == "readiness.listcs":
            if self.scheduler_interrupt:
                raise KeyboardInterrupt
            if self.scheduler_timeout:
                raise RemoteError("scheduler unavailable")
            return CommandResult(0, " F1 x86_64 jobs=0/24\n", "")
        if command.phase == "readiness.container":
            return CommandResult(
                0,
                json.dumps(
                    {"Error": "", "ExitCode": 0, "Running": True, "Status": "running"}
                ),
                "",
            )
        if command.phase == "readiness.canary":
            return CommandResult(0, "abcd  local\nabcd  remote\n", "")
        if command.phase == "readiness.environments":
            return CommandResult(
                0,
                json.dumps(
                    {
                        "matched": {"F1": "RELOGIN F1 [gcc] cache=on"},
                        "ready": True,
                    }
                ),
                "",
            )
        if command.phase == "down.remove-container":
            self.containers.pop(command.argv[-1])
            return CommandResult(0, "", "")
        return CommandResult(0, "", "")


def test_successful_up_executes_exact_planned_starts_and_proves_canary(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    scripted = ScriptedLifecycle(farm)
    transport = RecordingTransport(scripted)
    receipt = bring_up(
        farm,
        scenario,
        plan,
        recorder=transport,
        probe_bytes=0,
        sync_corpora=False,
    )
    assert receipt["status"] == "UP"
    assert set(receipt["canaries"]) == {"F1"}
    planned_starts = [
        item["argv"] for item in plan["commands"] if item["phase"].startswith("up.start-")
    ]
    observed_starts = [
        list(command.argv)
        for command in transport.commands
        if command.phase.startswith("up.start-")
    ]
    assert observed_starts == planned_starts
    assert len(scripted.containers) == 3


def test_forced_readiness_timeout_collects_and_removes_every_labelled_object(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path, up_s=2)
    scripted = ScriptedLifecycle(farm, scheduler_timeout=True)
    transport = RecordingTransport(scripted)
    clock = Clock()
    with pytest.raises(LifecycleError, match="readiness timeout"):
        bring_up(
            farm,
            scenario,
            plan,
            recorder=transport,
            probe_bytes=0,
            sync_corpora=False,
            monotonic=clock,
            sleeper=clock.sleep,
        )
    assert scripted.containers == {}
    assert any(command.phase == "diagnostics.inspect" for command in transport.commands)
    assert any(command.phase == "down.remove-container" for command in transport.commands)
    failure = json.loads((tmp_path / "results" / "unit-run" / "lifecycle.json").read_text())
    assert failure["status"] == "FAILED"


def test_stale_labelled_container_is_refused_without_reap_and_never_started(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    scripted = ScriptedLifecycle(farm, stale=True)
    transport = RecordingTransport(scripted)
    with pytest.raises(PreflightRefusal, match="--reap-stale"):
        bring_up(
            farm,
            scenario,
            plan,
            recorder=transport,
            probe_bytes=0,
            sync_corpora=False,
        )
    assert set(scripted.containers) == {"aaaaaaaaaaaa"}
    assert not any(command.phase.startswith("up.start-") for command in transport.commands)
    assert not any(
        command.phase.startswith("diagnostics.")
        or command.phase.startswith("down.")
        for command in transport.commands
    )


def test_keyboard_interrupt_after_start_collects_and_removes_every_labelled_object(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    scripted = ScriptedLifecycle(farm, scheduler_interrupt=True)
    transport = RecordingTransport(scripted)
    with pytest.raises(KeyboardInterrupt):
        bring_up(
            farm,
            scenario,
            plan,
            recorder=transport,
            probe_bytes=0,
            sync_corpora=False,
        )
    assert scripted.containers == {}
    assert any(command.phase == "diagnostics.inspect" for command in transport.commands)
    assert any(command.phase == "down.remove-container" for command in transport.commands)
    failure = json.loads((tmp_path / "results" / "unit-run" / "lifecycle.json").read_text())
    assert failure["error"] == "KeyboardInterrupt"
    assert failure["status"] == "FAILED"


def test_explicit_old_stale_reap_removes_only_labelled_icefarm_container(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    scripted = ScriptedLifecycle(farm, stale=True)
    transport = RecordingTransport(scripted)
    bring_up(
        farm,
        scenario,
        plan,
        recorder=transport,
        probe_bytes=0,
        sync_corpora=False,
        reap_stale_hours=1,
    )
    assert "aaaaaaaaaaaa" not in scripted.containers
    assert any(command.phase == "preflight.stale-reap" for command in transport.commands)


def test_capacity_includes_one_copy_of_each_required_image_per_host(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    required = MIN_FREE_BYTES + corpus_layout(farm, "fmt-100").bytes + 500_000_000
    scripted = ScriptedLifecycle(farm, free_bytes=required - 1)
    transport = RecordingTransport(scripted)
    with pytest.raises(PreflightRefusal, match="images 500000000"):
        bring_up(
            farm,
            scenario,
            plan,
            recorder=transport,
            probe_bytes=0,
            sync_corpora=False,
        )
    assert not any(command.phase.startswith("up.start-") for command in transport.commands)


def test_runtime_role_hash_mismatch_is_refused_before_persistent_start(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    scripted = ScriptedLifecycle(farm, bad_role_hash=True)
    transport = RecordingTransport(scripted)
    with pytest.raises(PreflightRefusal, match="hash mismatch"):
        bring_up(
            farm,
            scenario,
            plan,
            recorder=transport,
            probe_bytes=0,
            sync_corpora=False,
        )
    assert not any(command.phase.startswith("up.start-") for command in transport.commands)


def test_down_removes_containers_before_reporting_protected_count_change(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    scripted = ScriptedLifecycle(farm)
    bring_up(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(scripted),
        probe_bytes=0,
        sync_corpora=False,
    )
    scripted.protected_count = 2
    with pytest.raises(LifecycleError, match="protected-count-changed"):
        down_from_state(farm, plan, recorder=RecordingTransport(scripted))
    assert scripted.containers == {}


def test_ports_are_unique_and_every_daemon_gets_an_explicit_port(tmp_path: Path) -> None:
    _farm, _scenario, plan = _farm_scenario_plan(tmp_path)
    ports = [plan["ports"]["scheduler"], plan["ports"]["scheduler_control"]]
    ports.extend(plan["ports"]["instances"].values())
    assert len(ports) == len(set(ports))
    for command in plan["commands"]:
        if command["phase"] in ("up.start-f", "up.start-c"):
            assert "--port" in command["argv"]
            assert "--pull=never" in command["argv"]
