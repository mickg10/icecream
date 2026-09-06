from __future__ import annotations

import hashlib
import json
from pathlib import Path

import pytest

from farmharness.integration.tests import farm_fixture

from farmharness.integration import farmtest
from farmharness.integration.farm_spec import load_farm_spec
from farmharness.integration.images import (
    CommandFactory,
    RecordingTransport,
    _image_identity,
)
from farmharness.integration.lifecycle import (
    LifecycleError,
    MIN_FREE_BYTES,
    PreflightRefusal,
    ROLE_BINARY_PATHS,
    bring_up,
    corpus_layout,
    down_from_state,
    _run_canaries,
    _rotate_s30_canary_traces,
    _expected_image_labels,
)
from farmharness.integration.remote import (
    CommandResult,
    PlannedCommand,
    RemoteError,
    decode_ssh_payload,
)
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
            "Labels": _expected_image_labels(authority),
        },
        "Created": "2026-09-04T00:00:00Z",
        "Id": NATIVE_ID,
        "Os": "linux",
        "RootFS": {"Layers": ["sha256:" + "2" * 64], "Type": "layers"},
        "Size": 500_000_000,
    }


def test_mutant_image_labels_bind_base_source_and_mutant_recipe() -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    authority = farm.data["authority"]["images"]["p50s4-h3-tail-mutant"]

    assert _expected_image_labels(authority) == {
        "icefarm.source.archive_sha256": authority["base_archive_sha256"],
        "icefarm.source.commit": authority["base_commit"],
        "icefarm.mutant.patch_sha256": authority["patch_sha256"],
        "icefarm.mutant.recipe_sha256": authority["recipe_sha256"],
    }


def _farm_scenario_plan(tmp_path: Path, *, up_s: int = 5):
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(INTEGRATION / "scenarios" / "S00-smoke.json", farm)
    label = scenario.data["images"]["new"]
    document = _inspect_document(farm, label)
    identity = _image_identity(CommandResult(0, json.dumps(document), ""), "test image")
    farm.data["authority"]["images"][label]["closure_sha256"] = identity.closure_sha256
    farm.data["runtime_image"]["closure_sha256"] = identity.closure_sha256
    for environment in farm.data["client_environments"].values():
        environment["closure_sha256"] = identity.closure_sha256
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
        client_cache_timeout: bool = False,
    ) -> None:
        self.farm = farm
        self.scheduler_interrupt = scheduler_interrupt
        self.scheduler_timeout = scheduler_timeout
        self.free_bytes = free_bytes
        self.bad_role_hash = bad_role_hash
        self.client_cache_timeout = client_cache_timeout
        runtime_closure = farm.data["runtime_image"]["closure_sha256"]
        matching_labels = [
            label
            for label, authority in farm.data["authority"]["images"].items()
            if authority.get("closure_sha256") == runtime_closure
        ]
        assert len(matching_labels) == 1
        self.product_label = matching_labels[0]
        self.protected_count = 1
        self.remote_archives: set[str] = set()
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
            return CommandResult(
                0, "\n".join(self._containers_on(command.host)) + "\n", ""
            )
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
        if command.phase == "preflight.container-image":
            return CommandResult(
                0,
                json.dumps(_inspect_document(self.farm, self.product_label)),
                "",
            )
        if command.phase == "preflight.runtime-materialize":
            return CommandResult(
                0,
                f"materialized /icefarm-runtimes/{command.argv[-1]}\n",
                "",
            )
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
        if command.phase == "preflight.compiler":
            return CommandResult(
                0,
                f"compiler-ok {command.argv[-3]} {command.argv[-2]}\n",
                "",
            )
        if command.phase == "preflight.corpus-archive-verify":
            remote = decode_ssh_payload(command.argv)
            digest = remote[-2]
            if digest in self.remote_archives:
                return CommandResult(
                    0,
                    json.dumps(
                        {
                            "bytes": int(remote[-1]),
                            "ready": True,
                            "reason": "ready",
                            "sha256": digest,
                        }
                    ),
                    "",
                )
            return CommandResult(0, '{"ready": false, "reason": "absent"}\n', "")
        if command.phase == "preflight.corpus-archive-sync":
            filename = command.argv[-1].rsplit("/", 1)[-1]
            self.remote_archives.add(filename.removesuffix(".tar.zst"))
            return CommandResult(0, "", "")
        if command.phase == "run.corpus-clear":
            return CommandResult(0, f"cleared {command.argv[-1]}\n", "")
        if command.phase == "run.corpus-materialize":
            return CommandResult(0, "materialized /icefarm-corpus-cache/active\n", "")
        if command.phase == "run.corpus-activate":
            return CommandResult(0, f"activated {command.argv[-5]}\n", "")
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
        if command.phase == "readiness.client-cache":
            ready = not self.client_cache_timeout
            return CommandResult(
                0,
                json.dumps(
                    {
                        "lifecycle": 3 if ready else 2,
                        "line": "cache sidecar adapter state=2 lifecycle=3"
                        if ready
                        else None,
                        "ready": ready,
                        "reason": "ready" if ready else "not-ready",
                        "state": 2 if ready else 3,
                    }
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
    assert set(receipt["canaries"]) == {"C1"}
    assert set(receipt["canaries"]["C1"]) == {"F1"}
    assert receipt["client_cache_readiness"]["C1"]["ready"] is True
    planned_starts = [
        item["argv"]
        for item in plan["commands"]
        if item["phase"].startswith("up.start-")
    ]
    observed_starts = [
        list(command.argv)
        for command in transport.commands
        if command.phase.startswith("up.start-")
    ]
    assert observed_starts == planned_starts
    assert len(scripted.containers) == 3
    phases = [command.phase for command in transport.commands]
    assert phases.index("readiness.client-cache") < phases.index("readiness.canary")


def test_readiness_canaries_cover_every_workload_client_worker_pair(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    first_client = next(
        item for item in plan["topology"]["instances"] if item["name"] == "C1"
    )
    plan["topology"]["instances"].append({**first_client, "name": "C2"})
    scenario.data["workload"]["clients"].append("C2")
    transport = RecordingTransport(ScriptedLifecycle(farm))
    clock = Clock()

    canaries = _run_canaries(
        farm,
        scenario,
        plan,
        transport,
        CommandFactory(),
        deadline=10,
        monotonic=clock,
    )

    assert set(canaries) == {"C1", "C2"}
    assert all(set(by_worker) == {"F1"} for by_worker in canaries.values())
    commands = [
        command for command in transport.commands if command.phase == "readiness.canary"
    ]
    assert len(commands) == 2
    containers = {
        next(arg for arg in command.argv if arg.startswith("icefarm-unit-run-C"))
        for command in commands
    }
    assert containers == {"icefarm-unit-run-C1", "icefarm-unit-run-C2"}


def test_s30_mutant_rotates_readiness_trace_before_workload(tmp_path: Path) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    scenario.data["id"] = "S30-mutant-f-refusal"
    worker = next(
        item for item in plan["topology"]["instances"] if item["role"] == "F"
    )
    worker["image"]["kind"] = "daemon-mutant"
    transport = RecordingTransport(ScriptedLifecycle(farm))

    receipt = _rotate_s30_canary_traces(
        farm,
        scenario,
        plan,
        transport,
        CommandFactory(),
        timeout_s=30,
    )

    assert receipt == {
        "F1": {"canary_refusals": 1, "output": ""},
    }
    command = transport.commands[-1]
    assert command.phase == "readiness.s30-mutant-trace-boundary"
    assert any("s30-mutant-f-canary.jsonl" in argument for argument in command.argv)
    assert command.argv[-1] == "1"


def test_client_cache_ready_is_required_before_canary_and_timeout_cleans_up(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path, up_s=2)
    scripted = ScriptedLifecycle(farm, client_cache_timeout=True)
    transport = RecordingTransport(scripted)
    clock = Clock()
    with pytest.raises(LifecycleError, match="client cache READY"):
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
    assert any(
        command.phase == "readiness.client-cache" for command in transport.commands
    )
    assert not any(
        command.phase == "readiness.canary" for command in transport.commands
    )
    assert any(command.phase == "diagnostics.inspect" for command in transport.commands)


def test_up_syncs_only_zstd19_archive_and_activates_one_read_only_turn(
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
    )
    phases = [command.phase for command in transport.commands]
    assert "preflight.corpus-archive-sync" in phases
    assert "run.corpus-materialize" in phases
    assert "run.corpus-activate" in phases
    assert "preflight.corpus-sync" not in phases
    preflight = json.loads(
        (tmp_path / "results" / "unit-run" / "preflight.json").read_text()
    )
    assert preflight["corpus"]["active_turn"]["turn"] == "A"
    assert preflight["corpus"]["active_turn"]["group"] == "files"
    assert preflight["corpus"]["hosts"]["tt-quietbox3"]["files"]["mode"] == "synced"
    assert receipt["status"] == "UP"


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
    assert any(
        command.phase == "down.remove-container" for command in transport.commands
    )
    failure = json.loads(
        (tmp_path / "results" / "unit-run" / "lifecycle.json").read_text()
    )
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
    assert not any(
        command.phase.startswith("up.start-") for command in transport.commands
    )
    assert not any(
        command.phase.startswith("diagnostics.") or command.phase.startswith("down.")
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
    assert any(
        command.phase == "down.remove-container" for command in transport.commands
    )
    failure = json.loads(
        (tmp_path / "results" / "unit-run" / "lifecycle.json").read_text()
    )
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
    assert any(
        command.phase == "preflight.stale-reap" for command in transport.commands
    )


def test_capacity_includes_one_copy_of_each_required_image_per_host(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    required = MIN_FREE_BYTES + corpus_layout(farm, "fmt-100").bytes + 1_500_000_000
    scripted = ScriptedLifecycle(farm, free_bytes=required - 1)
    transport = RecordingTransport(scripted)
    with pytest.raises(PreflightRefusal, match="images 1500000000"):
        bring_up(
            farm,
            scenario,
            plan,
            recorder=transport,
            probe_bytes=0,
            sync_corpora=False,
        )
    assert not any(
        command.phase.startswith("up.start-") for command in transport.commands
    )


def _attach_test_toolchain(
    plan: dict[str, object], archive: Path, archive_bytes: int
) -> None:
    client = next(item for item in plan["topology"]["instances"] if item["role"] == "C")
    client["compiler_recipe"]["toolchain"] = {
        "archive": str(archive),
        "archive_bytes": archive_bytes,
        "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest()
        if archive.is_file()
        else "a" * 64,
        "compression": {
            "checksum": True,
            "codec": "zstd",
            "level": 19,
            "long": 31,
            "threads": 8,
        },
        "mount": "/opt/test-toolchain",
        "unpacked_bytes": 1,
    }


def test_toolchain_capacity_uses_declared_transport_bytes_before_local_stat(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    declared = 200_000_000_000
    _attach_test_toolchain(plan, tmp_path / "absent.tar.zst", declared)
    scripted = ScriptedLifecycle(farm)
    transport = RecordingTransport(scripted)
    with pytest.raises(PreflightRefusal, match=f"toolchains {declared + 1}"):
        bring_up(
            farm,
            scenario,
            plan,
            recorder=transport,
            probe_bytes=0,
            sync_corpora=False,
        )
    assert not any(
        command.phase.startswith("up.start-") for command in transport.commands
    )


def test_toolchain_archive_size_mismatch_is_refused_before_persistent_start(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    archive = tmp_path / "toolchain.tar.zst"
    archive.write_bytes(b"not-a-real-archive")
    _attach_test_toolchain(plan, archive, archive.stat().st_size + 1)
    scripted = ScriptedLifecycle(farm)
    transport = RecordingTransport(scripted)
    with pytest.raises(PreflightRefusal, match="toolchain archive size mismatch"):
        bring_up(
            farm,
            scenario,
            plan,
            recorder=transport,
            probe_bytes=0,
            sync_corpora=False,
        )
    assert not any(
        command.phase.startswith("up.start-") for command in transport.commands
    )


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
    assert not any(
        command.phase.startswith("up.start-") for command in transport.commands
    )
    refusal = json.loads(
        (tmp_path / "results" / "unit-run" / "preflight-refusal.json").read_text()
    )
    assert refusal["reason_code"] == "role-hash-mismatch"
    assert refusal["jobs_started"] == 0
    assert refusal["persistent_start_attempted"] is False
    mismatched = next(
        instance
        for instance in plan["topology"]["instances"]
        if instance["host"] == refusal["details"]["host"]
        and instance["role"] == refusal["details"]["role"]
    )
    assert refusal["details"] == {
        "expected_sha256": mismatched["sha256"],
        "host": mismatched["host"],
        "observed_sha256": "f" * 64,
        "product": mismatched["image"]["label"],
        "role": mismatched["role"],
    }
    assert not any(
        command["phase"].startswith("up.start-") for command in refusal["commands"]
    )
    lifecycle = json.loads(
        (tmp_path / "results" / "unit-run" / "lifecycle.json").read_text()
    )
    assert lifecycle["farm_digest"] == plan["farm_digest"]
    assert lifecycle["scenario_digest"] == plan["scenario_digest"]
    assert lifecycle["topology_digest"] == plan["topology_digest"]


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


def test_ports_are_unique_and_every_daemon_gets_an_explicit_port(
    tmp_path: Path,
) -> None:
    _farm, _scenario, plan = _farm_scenario_plan(tmp_path)
    ports = [plan["ports"]["scheduler"], plan["ports"]["scheduler_control"]]
    ports.extend(plan["ports"]["instances"].values())
    assert len(ports) == len(set(ports))
    for command in plan["commands"]:
        if command["phase"] in ("up.start-f", "up.start-c"):
            assert "--port" in command["argv"]
            assert "--pull=never" in command["argv"]
