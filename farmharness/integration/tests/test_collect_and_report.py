from __future__ import annotations

import json
import shutil
from pathlib import Path

import pytest

from farmharness.integration import farmtest
from farmharness.integration.collect import (
    CollectError,
    collect_bundle,
    load_verified_bundle,
)
from farmharness.integration.farm_spec import load_farm_spec
from farmharness.integration.images import RecordingTransport
from farmharness.integration.lifecycle import bundle_root
from farmharness.integration.remote import CommandResult, PlannedCommand
from farmharness.integration.report import report_bundle, verify_bundle
from farmharness.integration.scenario_spec import load_scenario_spec
from farmharness.integration.schema_validation import canonical_bytes


INTEGRATION = Path(__file__).resolve().parents[1]
SHA = "a" * 64
C_GUID = "1" * 32


def _write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(canonical_bytes(value))


def _write_jsonl(path: Path, values: list[dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"".join(canonical_bytes(value) for value in values))


def _raw_collection(tmp_path: Path):
    farm = load_farm_spec(INTEGRATION / "farm.example.json")
    farm.data["hub"]["results_root"] = str(tmp_path)
    farm.data["corpora"]["fmt-100"]["tus"] = 1
    farm.data["corpora"]["fmt-100"]["repeat"] = 1
    scenario = load_scenario_spec(INTEGRATION / "scenarios" / "S00-smoke.json", farm)
    plan = farmtest.build_plan(farm, scenario, run_id="collect-fixture")
    root = bundle_root(farm, plan["run_id"])

    bindings = {
        "farm_digest": plan["farm_digest"],
        "run_id": plan["run_id"],
        "scenario_digest": plan["scenario_digest"],
        "topology_digest": plan["topology_digest"],
    }
    used_hosts = {item["host"] for item in plan["topology"]["instances"]}
    _write_json(
        root / "preflight.json",
        {
            **bindings,
            "hosts": {
                host: {"protected": {"bigfarm": 1}} for host in sorted(used_hosts)
            },
            "images": {"authenticated": True},
        },
    )
    _write_json(root / "lifecycle.json", {**bindings, "plan": plan, "status": "UP"})
    _write_json(root / "workload.json", {**bindings, "status": "COMPLETE"})

    instances = {item["name"]: item for item in plan["topology"]["instances"]}
    for name in instances:
        (root / f"{name}.results").mkdir(parents=True)
    diagnostics = root / "diagnostics"
    scheduler = instances["S1"]
    worker = instances["F1"]
    endpoint = f"{worker['address']}:{plan['ports']['instances']['F1']}"
    scheduler_log = (
        diagnostics / scheduler["host"] / "S1.log" / "scheduler.log"
    )
    scheduler_log.parent.mkdir(parents=True)
    scheduler_log.write_text(
        f"RELOGIN F1(x86_64): cache={endpoint} cache_wire=v1 "
        "cache_protocol=50 cache_profiles=p29v1 zstd_tu zstd_route\n"
        "RELOGIN F1(x86_64): cache=off\n",
        encoding="utf-8",
    )
    worker_log = diagnostics / worker["host"] / "F1.log" / "iceccd.log"
    worker_log.parent.mkdir(parents=True)
    worker_log.write_text(
        "P50 CompileFile attached exact P29V1 input for job 2\n",
        encoding="utf-8",
    )

    client_results = root / "C1.results"
    job = client_results / "workload" / "jobs" / "000001"
    job.mkdir(parents=True)
    (job / "result.tsv").write_text(
        "\t".join(
            (
                "1",
                "A",
                "0",
                "files/x.ii",
                "2",
                endpoint,
                "1000",
                "1025",
                "0",
                SHA,
                SHA,
                "1",
                "1",
                "0",
            )
        )
        + "\n",
        encoding="utf-8",
    )
    (job / "client-debug.log").write_text(
        "P29V1 source committed for P50 CompileFile: 100 exact bytes, "
        "TU sequence 1\n",
        encoding="utf-8",
    )
    (job / "client-output.log").write_text("", encoding="utf-8")
    workload = client_results / "workload"
    (workload / "oracle-summary.tsv").write_text(
        "sample_total\t1\nsample_mismatches\t0\n", encoding="utf-8"
    )
    (workload / "oracle-samples.tsv").write_text(
        f"files/x.ii\t{SHA}\t{SHA}\t1\n", encoding="utf-8"
    )
    _write_jsonl(
        client_results / "source-result.jsonl",
        [
            {
                "assignment_epoch": 1,
                "assignment_nonce": 1,
                "attempts": 1,
                "c_store_guid": C_GUID,
                "c_to_f_bytes": 321,
                "f_to_c_bytes": 123,
                "logical_job": 2,
                "profile": "P29V1",
                "raw_bytes": 100,
                "schema": "icecream-p50-source-result-v1",
                "status": 0,
                "system_source_reuse": True,
                "tu_seq": 1,
                "wire_job_id": 2,
            }
        ],
    )
    _write_jsonl(
        client_results / "c-action.jsonl",
        [{"action": "COMMIT_ACCEPTED", "c_store_guid": C_GUID, "tu_seq": 1}],
    )
    _write_jsonl(
        root / "F1.results" / "f-action.jsonl",
        [
            {"action": "SESSION_OPENED"},
            {"action": "INPUT_COMMITTED", "c_store_guid": C_GUID, "tu_seq": 1},
        ],
    )
    return farm, scenario, plan, root


def test_collection_verdict_report_and_replay_are_reproducible(tmp_path: Path) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    bundle = collect_bundle(farm, scenario, plan, sync_remote=False)

    assert bundle["rows"] == [
        {
            "c_to_f_bytes": 321,
            "client_instance": "C1",
            "client_version": 50,
            "cs": "F1",
            "cs_version": 50,
            "event_epoch": 0,
            "exact": True,
            "f_to_c_bytes": 123,
            "job_id": "C1:2",
            "object_sha_local": SHA,
            "object_sha_remote": SHA,
            "retries": 0,
            "reuse": True,
            "schema": "icecream-newgen-farm-acceptance-v1",
            "session_outcome": "committed",
            "tail_present": True,
            "tail_profile": "P29V1",
            "tu": "files/x.ii",
            "wall_ms": 25,
        }
    ]
    assert bundle["observations"]["logins"] == [
        {
            "cache_profiles": ["P29V1", "ZSTD_TU", "ZSTD_ROUTE"],
            "instance": "F1",
            "protocol": 50,
        }
    ]
    assert bundle["observations"]["sidecars"]["F1"]["process_count"] == 1
    verdict = verify_bundle(root)
    assert verdict["status"] == "PASS", verdict
    reported, markdown = report_bundle(root)
    assert reported == verdict
    assert "Verdict: **PASS**" in markdown
    assert (root / "witness.json").is_file()
    assert collect_bundle(farm, scenario, plan, sync_remote=False) == bundle
    assert load_verified_bundle(root) == bundle


def test_verified_bundle_refuses_mutated_evidence(tmp_path: Path) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    collect_bundle(farm, scenario, plan, sync_remote=False)
    result = root / "evidence" / "instances" / "C1" / "results" / "source-result.jsonl"
    result.write_bytes(result.read_bytes() + b"\n")

    with pytest.raises(CollectError, match="checksum mismatch"):
        load_verified_bundle(root)


@pytest.mark.parametrize(
    ("field", "replacement", "message"),
    (
        ("images", {}, "bundle images differ"),
        ("instances", [], "bundle instances differ"),
        ("run_id", "another-run", "bundle run_id differs"),
    ),
)
def test_verified_bundle_refuses_unbound_duplicate_fields(
    tmp_path: Path, field: str, replacement: object, message: str
) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    collect_bundle(farm, scenario, plan, sync_remote=False)
    path = root / "bundle.json"
    bundle = json.loads(path.read_text(encoding="utf-8"))
    bundle[field] = replacement
    _write_json(path, bundle)

    with pytest.raises(CollectError, match=message):
        load_verified_bundle(root)


def test_collection_refuses_a_marker_without_exact_source_result(tmp_path: Path) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    (root / "C1.results" / "source-result.jsonl").unlink()

    with pytest.raises(CollectError, match="has no exact source-result witness"):
        collect_bundle(farm, scenario, plan, sync_remote=False)


def test_collection_refuses_duplicate_source_result_json_key(tmp_path: Path) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    path = root / "C1.results" / "source-result.jsonl"
    record = json.loads(path.read_text(encoding="utf-8"))
    encoded = json.dumps(record, sort_keys=True, separators=(",", ":"))
    path.write_text(encoded[:-1] + ',"status":0}\n', encoding="utf-8")

    with pytest.raises(CollectError, match="duplicate JSON key 'status'"):
        collect_bundle(farm, scenario, plan, sync_remote=False)


def test_collection_refuses_a_commit_without_exact_wire_byte_counts(
    tmp_path: Path,
) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    path = root / "C1.results" / "source-result.jsonl"
    record = json.loads(path.read_text(encoding="utf-8"))
    record["c_to_f_bytes"] = 0
    _write_jsonl(path, [record])

    with pytest.raises(CollectError, match="committed source-result has no wire bytes"):
        collect_bundle(farm, scenario, plan, sync_remote=False)


class _LiveCollection:
    def __init__(self, plan: dict[str, object], source_root: Path) -> None:
        self.plan = plan
        self.source_root = source_root
        topology = plan["topology"]
        assert isinstance(topology, dict)
        instances = topology["instances"]
        assert isinstance(instances, list)
        self.ids = {
            instance["name"]: f"{index + 1:064x}"
            for index, instance in enumerate(instances)
        }
        self.names = {value: key for key, value in self.ids.items()}

    def _name_from_argv(self, command: PlannedCommand) -> str:
        for container_id, name in self.names.items():
            if container_id in command.argv or any(
                item.startswith(container_id + ":") for item in command.argv
            ):
                return name
        for name in self.ids:
            if f"icefarm-{self.plan['run_id']}-{name}" in command.argv:
                return name
        raise AssertionError(f"command has no fixture container: {command}")

    def invoke(self, command: PlannedCommand) -> CommandResult:
        if command.phase == "collect.authenticate":
            name = self._name_from_argv(command)
            return CommandResult(
                0,
                json.dumps(
                    {
                        "Config": {
                            "Labels": {
                                "icefarm.instance": name,
                                "icefarm.run": self.plan["run_id"],
                            }
                        },
                        "Id": self.ids[name],
                    }
                ),
                "",
            )
        if command.phase == "collect.top":
            name = self._name_from_argv(command)
            content = "1 icecc-cache-service cache\n" if name == "F1" else "1 init idle\n"
            return CommandResult(0, content, "")
        if command.phase == "collect.stats":
            return CommandResult(0, "{}\n", "")
        if command.phase == "collect.stop":
            return CommandResult(0, self._name_from_argv(command) + "\n", "")
        if command.phase == "diagnostics.inspect":
            return CommandResult(0, "[]\n", "")
        if command.phase == "diagnostics.logs":
            return CommandResult(0, "container output\n", "")
        if command.phase == "diagnostics.sync-log":
            destination = Path(command.argv[-1])
            source = self.source_root / "diagnostics" / command.host / destination.name
            if source.is_dir():
                shutil.copytree(source, destination, dirs_exist_ok=True)
            else:
                destination.mkdir(parents=True, exist_ok=True)
            return CommandResult(0, "", "")
        if command.phase == "collect.results":
            name = self._name_from_argv(command)
            shutil.copytree(
                self.source_root / f"{name}.results",
                Path(command.argv[-1]),
                dirs_exist_ok=True,
            )
            return CommandResult(0, "", "")
        raise AssertionError(f"unexpected collection command: {command.phase}")


def test_live_collection_authenticates_samples_then_freezes_before_copy(
    tmp_path: Path,
) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    delegate = _LiveCollection(plan, root)
    recorder = RecordingTransport(delegate)

    bundle = collect_bundle(farm, scenario, plan, recorder=recorder)

    assert bundle["rows"][0]["session_outcome"] == "committed"
    phases = [command.phase for command in recorder.commands]
    first_stop = phases.index("collect.stop")
    assert all(
        phases.index(phase) < first_stop
        for phase in ("collect.authenticate", "collect.top", "collect.stats")
    )
    assert phases.index("diagnostics.inspect") > max(
        index for index, phase in enumerate(phases) if phase == "collect.stop"
    )
    assert phases.index("collect.results") > phases.index("diagnostics.sync-log")
    stopped = [
        delegate._name_from_argv(command)
        for command in recorder.commands
        if command.phase == "collect.stop"
    ]
    assert stopped == ["C1", "F1", "S1"]
