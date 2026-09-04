from __future__ import annotations

import copy
import json
import subprocess
from pathlib import Path

import pytest

from farmharness.integration import farmtest
from farmharness.integration.farm_spec import FarmSpecError, load_farm_spec
from farmharness.integration.remote import (
    FakeRecorder,
    PlannedCommand,
    RemoteError,
    SubprocessTransport,
    decode_ssh_payload,
    execute,
)
from farmharness.integration.scenario_spec import ScenarioSpecError, load_scenario_spec
from farmharness.integration.schema_validation import ValidationError, validate


INTEGRATION = Path(__file__).resolve().parents[1]


def _documents() -> tuple[dict[str, object], dict[str, object]]:
    farm = json.loads((INTEGRATION / "farm.example.json").read_text())
    scenario = json.loads((INTEGRATION / "scenarios" / "S00-smoke.json").read_text())
    return farm, scenario


def _write(tmp_path: Path, farm: dict[str, object], scenario: dict[str, object]) -> tuple[Path, Path]:
    farm_path = tmp_path / "farm.json"
    scenario_path = tmp_path / "scenario.json"
    farm_path.write_text(json.dumps(farm), encoding="utf-8")
    scenario_path.write_text(json.dumps(scenario), encoding="utf-8")
    return farm_path, scenario_path


def _load(tmp_path: Path, farm: dict[str, object], scenario: dict[str, object]):
    farm_path, scenario_path = _write(tmp_path, farm, scenario)
    loaded_farm = load_farm_spec(farm_path)
    return loaded_farm, load_scenario_spec(scenario_path, loaded_farm)


def test_committed_examples_validate_and_plan_is_stable() -> None:
    farm = load_farm_spec(INTEGRATION / "farm.example.json")
    scenario = load_scenario_spec(INTEGRATION / "scenarios" / "S00-smoke.json", farm)
    first = farmtest.build_plan(farm, scenario)
    second = farmtest.build_plan(farm, scenario)
    assert first == second
    assert first["topology_digest"] == second["topology_digest"]
    assert len(first["topology_digest"]) == 64
    assert first["run_id"] == f"plan-{first['topology_digest'][:12]}"
    assert first["topology"]["schema"] == "icecream-newgen-farm-topology-v2"
    assert "ICEFARM_INSTANCES" in first["icefarm_env"]
    assert "ICEFARM_IMAGE" not in first["icefarm_env"]
    assert "ICEFARM_S" not in first["icefarm_env"]


def test_missing_scratch_root_is_refused(tmp_path: Path) -> None:
    farm, scenario = _documents()
    del farm["hosts"][0]["scratch_root"]
    farm_path, _ = _write(tmp_path, farm, scenario)
    with pytest.raises(FarmSpecError, match="scratch_root"):
        load_farm_spec(farm_path)


def test_scheduler_off_shared_lan_is_refused(tmp_path: Path) -> None:
    farm, scenario = _documents()
    farm["hosts"][0]["shared_lan"] = "10.0.27.96/28"
    with pytest.raises(ScenarioSpecError, match="shared_lan"):
        _load(tmp_path, farm, scenario)


def test_f_without_chroot_authority_is_refused(tmp_path: Path) -> None:
    farm, scenario = _documents()
    farm["hosts"][1]["roles_allowed"] = ["C"]
    with pytest.raises(ScenarioSpecError, match="SYS_CHROOT"):
        _load(tmp_path, farm, scenario)


def test_shaping_without_netem_authority_is_refused(tmp_path: Path) -> None:
    farm, scenario = _documents()
    scenario["network"]["shaping"] = [
        {"instance": "F1", "rate": "100mbit", "delay_ms": 2}
    ]
    with pytest.raises(ScenarioSpecError, match="netem authority"):
        _load(tmp_path, farm, scenario)


def test_empty_protected_pattern_is_refused(tmp_path: Path) -> None:
    farm, scenario = _documents()
    farm["protected"]["process_patterns"] = []
    farm_path, _ = _write(tmp_path, farm, scenario)
    with pytest.raises(FarmSpecError, match="process_patterns"):
        load_farm_spec(farm_path)


@pytest.mark.parametrize(
    ("mutation", "error"),
    [
        (lambda farm, _scenario: farm.update(schema="wrong"), "must equal"),
        (lambda farm, _scenario: farm.update(unexpected=True), "additional property"),
        (lambda farm, _scenario: farm.update(port_range=[23000]), "at least 2"),
        (
            lambda _farm, scenario: scenario["instances"][1].update(slots=0),
            "must be >= 1",
        ),
        (
            lambda _farm, scenario: scenario.update(shape="invented"),
            "must be one of",
        ),
    ],
)
def test_schema_violations_are_refused(tmp_path: Path, mutation, error: str) -> None:
    farm, scenario = _documents()
    mutation(farm, scenario)
    farm_path, scenario_path = _write(tmp_path, farm, scenario)
    if farm.get("schema") != "icefarm-farm-v1" or "unexpected" in farm or len(farm["port_range"]) != 2:
        with pytest.raises(FarmSpecError, match=error):
            load_farm_spec(farm_path)
    else:
        loaded_farm = load_farm_spec(farm_path)
        with pytest.raises(ScenarioSpecError, match=error):
            load_scenario_spec(scenario_path, loaded_farm)


def test_duplicate_json_key_is_refused(tmp_path: Path) -> None:
    path = tmp_path / "farm.json"
    path.write_text('{"schema":"icefarm-farm-v1","schema":"other"}', encoding="utf-8")
    with pytest.raises(FarmSpecError, match="duplicate key"):
        load_farm_spec(path)


def test_invalid_utf8_is_refused_as_a_spec_error(tmp_path: Path) -> None:
    path = tmp_path / "farm.json"
    path.write_bytes(b"{\xff}")
    with pytest.raises(FarmSpecError, match="invalid UTF-8"):
        load_farm_spec(path)


def test_numeric_overflow_in_open_authority_block_is_refused(tmp_path: Path) -> None:
    farm, scenario = _documents()
    raw = json.dumps(farm).replace('"authority": {', '"authority": {"huge": 1e400,', 1)
    farm_path = tmp_path / "farm.json"
    farm_path.write_text(raw, encoding="utf-8")
    with pytest.raises(FarmSpecError, match="non-finite number"):
        load_farm_spec(farm_path)


@pytest.mark.parametrize(
    ("mutation", "error"),
    [
        (
            lambda farm: farm["authority"]["images"]["p50s2-624702e9"].update(
                commit="a" * 40 + "\n"
            ),
            "invalid immutable digest",
        ),
        (
            lambda farm: farm["authority"]["role_stores"]["50"]["daemon"].update(
                sha256="a" * 64 + "\n"
            ),
            "invalid immutable digest",
        ),
        (
            lambda farm: farm["authority"]["images"].update(
                {"p50s3-invalid\n": farm["authority"]["images"]["p50s2-624702e9"]}
            ),
            "invalid display label|does not match",
        ),
    ],
)
def test_authority_execution_fields_reject_final_newline(
    mutation, error: str, tmp_path: Path
) -> None:
    farm, scenario = _documents()
    mutation(farm)
    farm_path, _ = _write(tmp_path, farm, scenario)
    with pytest.raises(FarmSpecError, match=error):
        load_farm_spec(farm_path)


def test_optional_s8_capture_is_provenance_only(tmp_path: Path) -> None:
    farm, scenario = _documents()
    loaded_farm, loaded_scenario = _load(tmp_path, farm, scenario)
    baseline = farmtest.resolve_topology(loaded_farm, loaded_scenario)["topology_digest"]
    farm["s8_capture"] = {"schema": "unvalidated-old-capture", "arbitrary": [1, 2, 3]}
    loaded_farm, loaded_scenario = _load(tmp_path, farm, scenario)
    assert farmtest.resolve_topology(loaded_farm, loaded_scenario)["topology_digest"] == baseline


def test_unicode_surrogate_is_refused_before_hashing(tmp_path: Path) -> None:
    farm, scenario = _documents()
    farm["authority"]["note"] = "bad\ud800value"
    farm_path, _ = _write(tmp_path, farm, scenario)
    with pytest.raises(FarmSpecError, match="surrogate"):
        load_farm_spec(farm_path)


def test_unimplemented_schema_keyword_is_never_silently_ignored(tmp_path: Path) -> None:
    schema_path = tmp_path / "future-schema.json"
    schema_path.write_text(
        json.dumps({"type": "string", "oneOf": [{"const": "allowed"}]}),
        encoding="utf-8",
    )
    with pytest.raises(ValidationError, match="unsupported schema keyword 'oneOf'"):
        validate("forbidden", schema_path)


def test_plan_output_equals_fake_up_output_byte_for_byte(capsys: pytest.CaptureFixture[str]) -> None:
    common = [
        "--farm",
        str(INTEGRATION / "farm.example.json"),
        "--scenario",
        str(INTEGRATION / "scenarios" / "S00-smoke.json"),
        "--run-id",
        "golden-run",
    ]
    assert farmtest.main(["plan", *common]) == 0
    planned = capsys.readouterr()
    assert planned.err == ""
    assert farmtest.main(["up", "--fake-recorder", *common]) == 0
    recorded = capsys.readouterr()
    assert recorded.err == ""
    assert planned.out == recorded.out


def test_plan_commands_are_argv_only_and_label_scoped() -> None:
    farm = load_farm_spec(INTEGRATION / "farm.example.json")
    scenario = load_scenario_spec(INTEGRATION / "scenarios" / "S00-smoke.json", farm)
    plan = farmtest.build_plan(farm, scenario, run_id="argv-check")
    assert plan["commands"]
    for command in plan["commands"]:
        assert isinstance(command["argv"], list)
        assert all(isinstance(item, str) and "\0" not in item for item in command["argv"])
        assert "sh" not in command["argv"]
        assert "bash" not in command["argv"]
    starts = [item for item in plan["commands"] if item["phase"].startswith("up.start-")]
    assert all("icefarm.run=argv-check" in item["argv"] for item in starts)


@pytest.mark.parametrize("profile", ("P29V1", "ZSTD_TU", "ZSTD_ROUTE"))
def test_owner_kept_profiles_resolve(profile: str, tmp_path: Path) -> None:
    farm, scenario = _documents()
    scenario["instances"][0]["env"]["ICECC_P50_PROFILE"] = profile
    loaded_farm, loaded_scenario = _load(tmp_path, farm, scenario)
    plan = farmtest.build_plan(loaded_farm, loaded_scenario)
    assert plan["icefarm_env"]["ICEFARM_PROFILE"] == profile


@pytest.mark.parametrize("removed_profile", ("GRZ_RESIDUAL", "OFF"))
def test_removed_or_nonproduct_profile_is_refused(
    removed_profile: str, tmp_path: Path
) -> None:
    farm, scenario = _documents()
    scenario["instances"][0]["env"]["ICECC_P50_PROFILE"] = removed_profile
    with pytest.raises(ScenarioSpecError, match="ICECC_P50_PROFILE"):
        _load(tmp_path, farm, scenario)


@pytest.mark.parametrize("reserved", ("ICECC_NETNAME", "ICECC_SCHEDULER", "ICECC_TEST_SOCKET", "ICECC_VERSION"))
def test_scenario_cannot_override_runner_environment(reserved: str, tmp_path: Path) -> None:
    farm, scenario = _documents()
    scenario["instances"][1]["env"] = {reserved: "attacker-controlled"}
    with pytest.raises(ScenarioSpecError, match="runner-owned environment"):
        _load(tmp_path, farm, scenario)


def test_control_character_in_image_reference_is_refused(tmp_path: Path) -> None:
    farm, scenario = _documents()
    scenario["images"]["new"] = "p50-valid-prefix\ninvalid"
    with pytest.raises(ScenarioSpecError, match="does not match|control characters"):
        _load(tmp_path, farm, scenario)


def test_unix_socket_path_limit_is_enforced(tmp_path: Path) -> None:
    farm, scenario = _documents()
    scenario["instances"][1]["name"] = "F" + "x" * 99
    loaded_farm, loaded_scenario = _load(tmp_path, farm, scenario)
    with pytest.raises(farmtest.PlanError, match="Unix limit"):
        farmtest.build_plan(loaded_farm, loaded_scenario, run_id="r" * 80)


def test_ssh_docker_fallback_is_one_argv(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    farm, scenario = _documents()
    del farm["hosts"][1]["docker_context"]
    loaded_farm, loaded_scenario = _load(tmp_path, farm, scenario)
    plan = farmtest.build_plan(loaded_farm, loaded_scenario, run_id="ssh-fallback")
    f_start = next(item for item in plan["commands"] if item["phase"] == "up.start-f")
    assert f_start["transport"] == "ssh-docker"
    assert f_start["argv"][:2] == ["ssh", "-o"]
    assert decode_ssh_payload(f_start["argv"])[0] == "docker"


def test_ssh_wrapper_never_embeds_spec_values_in_remote_shell_text(tmp_path: Path) -> None:
    farm, scenario = _documents()
    del farm["hosts"][1]["docker_context"]
    scenario["instances"][1]["env"] = {"UNTRUSTED": "$(touch /tmp/forbidden); a b ' c"}
    loaded_farm, loaded_scenario = _load(tmp_path, farm, scenario)
    plan = farmtest.build_plan(loaded_farm, loaded_scenario, run_id="encoded-argv")
    command = next(item for item in plan["commands"] if item["phase"] == "up.start-f")
    shell_operands = command["argv"][-4:-1]
    assert not any("UNTRUSTED" in item or "touch" in item for item in shell_operands)
    decoded = decode_ssh_payload(command["argv"])
    assert "UNTRUSTED=$(touch /tmp/forbidden); a b ' c" in decoded


def test_fake_recorder_never_invokes_subprocess(monkeypatch: pytest.MonkeyPatch) -> None:
    invoked = False

    def forbidden(*_args, **_kwargs):
        nonlocal invoked
        invoked = True
        raise AssertionError("subprocess must not run")

    monkeypatch.setattr("subprocess.run", forbidden)
    command = PlannedCommand(0, "up", "host", None, "fake", 1, ("false",))
    recorder = FakeRecorder()
    execute([command], recorder)
    assert recorder.commands == [command]
    assert invoked is False


def test_real_up_is_fail_closed_before_transport(capsys: pytest.CaptureFixture[str]) -> None:
    args = [
        "up",
        "--farm",
        str(INTEGRATION / "farm.example.json"),
        "--scenario",
        str(INTEGRATION / "scenarios" / "S00-smoke.json"),
    ]
    assert farmtest.main(args) == 2
    output = capsys.readouterr()
    assert output.out == ""
    assert "unavailable until lifecycle I3" in output.err


def test_subprocess_transport_declares_shell_false(monkeypatch: pytest.MonkeyPatch) -> None:
    observed: dict[str, object] = {}

    class Completed:
        returncode = 0
        stdout = "ok"
        stderr = ""

    def fake_run(argv, **kwargs):
        observed["argv"] = argv
        observed.update(kwargs)
        return Completed()

    monkeypatch.setattr("subprocess.run", fake_run)
    command = PlannedCommand(0, "probe", "host", None, "local", 7, ("printf", "%s", "a b"))
    result = SubprocessTransport().invoke(command)
    assert result.stdout == "ok"
    assert observed["argv"] == ["printf", "%s", "a b"]
    assert observed["encoding"] == "utf-8"
    assert observed["errors"] == "replace"
    assert observed["shell"] is False
    assert observed["timeout"] == 7


def test_subprocess_transport_timeout_fails_closed(monkeypatch: pytest.MonkeyPatch) -> None:
    def fake_run(argv, **_kwargs):
        raise subprocess.TimeoutExpired(argv, 7)

    monkeypatch.setattr("subprocess.run", fake_run)
    command = PlannedCommand(4, "probe", "host", None, "local", 7, ("sleep", "99"))
    with pytest.raises(RemoteError, match="command 4 timed out after 7s on host"):
        SubprocessTransport().invoke(command)


def test_input_documents_are_not_mutated(tmp_path: Path) -> None:
    farm, scenario = _documents()
    before_farm = copy.deepcopy(farm)
    before_scenario = copy.deepcopy(scenario)
    _load(tmp_path, farm, scenario)
    assert farm == before_farm
    assert scenario == before_scenario


@pytest.mark.parametrize("run_id", (".", "..", "slash/not-allowed", "space not allowed"))
def test_unsafe_run_id_is_refused(run_id: str) -> None:
    farm = load_farm_spec(INTEGRATION / "farm.example.json")
    scenario = load_scenario_spec(INTEGRATION / "scenarios" / "S00-smoke.json", farm)
    with pytest.raises(farmtest.PlanError, match="run id"):
        farmtest.build_plan(farm, scenario, run_id=run_id)


def test_dot_instance_name_is_refused(tmp_path: Path) -> None:
    farm, scenario = _documents()
    scenario["instances"][1]["name"] = ".."
    with pytest.raises(ScenarioSpecError, match="does not match|dot path"):
        _load(tmp_path, farm, scenario)


def test_control_character_in_scratch_path_is_refused(tmp_path: Path) -> None:
    farm, scenario = _documents()
    farm["hosts"][0]["scratch_root"] = "/safe\nunsafe"
    farm_path, _ = _write(tmp_path, farm, scenario)
    with pytest.raises(FarmSpecError, match="control-free"):
        load_farm_spec(farm_path)


@pytest.mark.parametrize("root_alias", ("/.", "//"))
def test_root_equivalent_scratch_path_is_refused(tmp_path: Path, root_alias: str) -> None:
    farm, scenario = _documents()
    farm["hosts"][0]["scratch_root"] = root_alias
    farm_path, _ = _write(tmp_path, farm, scenario)
    with pytest.raises(FarmSpecError, match="non-root absolute path"):
        load_farm_spec(farm_path)


def test_ssh_destination_cannot_be_an_option(tmp_path: Path) -> None:
    farm, scenario = _documents()
    farm["hosts"][0]["ssh"] = "-oProxyCommand=bad@host"
    farm_path, _ = _write(tmp_path, farm, scenario)
    with pytest.raises(FarmSpecError, match="does not match"):
        load_farm_spec(farm_path)


def test_nul_in_environment_is_refused(tmp_path: Path) -> None:
    farm, scenario = _documents()
    scenario["instances"][1]["env"] = {"BAD": "a\0b"}
    with pytest.raises(ScenarioSpecError, match="NUL"):
        _load(tmp_path, farm, scenario)


@pytest.mark.parametrize(
    "mutation",
    [
        lambda farm, _scenario: farm["hosts"][0].update(name="q3\n"),
        lambda farm, _scenario: farm["hosts"][0].update(ssh="mickg10@q3\n"),
        lambda farm, _scenario: farm["hosts"][0].update(docker_context="q3\n"),
        lambda farm, _scenario: farm["protected"].update(process_patterns=["bigfarm\n"]),
    ],
)
def test_farm_execution_identifiers_reject_final_newline(
    mutation, tmp_path: Path
) -> None:
    farm, scenario = _documents()
    mutation(farm, scenario)
    farm_path, _ = _write(tmp_path, farm, scenario)
    with pytest.raises(FarmSpecError, match="control characters"):
        load_farm_spec(farm_path)


@pytest.mark.parametrize(
    "mutation",
    [
        lambda _farm, scenario: scenario.update(id="S00-smoke\n"),
        lambda _farm, scenario: scenario["instances"][1].update(name="F1\n"),
        lambda _farm, scenario: scenario["instances"][1].update(env={"SAFE\n": "value"}),
        lambda _farm, scenario: scenario["network"].update(
            shaping=[{"instance": "F1", "rate": "100mbit\n", "delay_ms": 2}]
        ),
    ],
)
def test_scenario_execution_identifiers_reject_final_newline(
    mutation, tmp_path: Path
) -> None:
    farm, scenario = _documents()
    mutation(farm, scenario)
    with pytest.raises(ScenarioSpecError, match="safe non-dot name|unsafe environment|invalid rate"):
        _load(tmp_path, farm, scenario)


@pytest.mark.parametrize(
    ("event", "error"),
    [
        ({"trigger": "job 1", "action": "upgrade", "instance": "F1"}, "requires fields.*image"),
        ({"trigger": "job 1", "action": "env_set", "instance": "C1"}, "requires fields.*env"),
        (
            {"trigger": "job 1", "action": "netem_set", "instance": "C1", "rate": "100mbit"},
            "requires fields.*delay_ms",
        ),
        ({"trigger": "job 1", "action": "restart"}, "instance: required"),
    ],
)
def test_timeline_action_required_fields_are_fail_closed(
    event: dict[str, object], error: str, tmp_path: Path
) -> None:
    farm, scenario = _documents()
    scenario["timeline"] = [event]
    with pytest.raises(ScenarioSpecError, match=error):
        _load(tmp_path, farm, scenario)


@pytest.mark.parametrize(
    ("event", "error"),
    [
        (
            {"trigger": "job 1", "action": "restart", "instance": "F1", "image": "new"},
            "does not use fields.*image",
        ),
        (
            {
                "trigger": "job 1",
                "action": "env_set",
                "instance": "C1",
                "env": {"ICECC_SCHEDULER": "attacker-controlled"},
            },
            "runner-owned environment",
        ),
        (
            {
                "trigger": "job 1",
                "action": "env_set",
                "instance": "F1",
                "env": {"ICECC_P50_MODE": "off"},
            },
            "only S or C",
        ),
        ({"trigger": "job 1", "action": "disk_fill", "instance": "C1"}, "only F"),
        (
            {
                "trigger": "job 1",
                "action": "header_edit",
                "instance": "F1",
                "path": "../unsafe/header.h",
            },
            "non-root absolute path",
        ),
    ],
)
def test_timeline_action_unsafe_or_ambiguous_fields_are_refused(
    event: dict[str, object], error: str, tmp_path: Path
) -> None:
    farm, scenario = _documents()
    scenario["timeline"] = [event]
    with pytest.raises(ScenarioSpecError, match=error):
        _load(tmp_path, farm, scenario)


@pytest.mark.parametrize(
    "event",
    [
        {"trigger": "job 1", "action": "upgrade", "instance": "F1", "image": "new"},
        {"trigger": "job 1", "action": "downgrade", "instance": "F1", "image": "new"},
        {"trigger": "job 1", "action": "restart", "instance": "F1"},
        {"trigger": "job 1", "action": "kill -9", "instance": "F1"},
        {
            "trigger": "job 1",
            "action": "env_set",
            "instance": "C1",
            "env": {"ICECC_P50_MODE": "off"},
        },
        {
            "trigger": "job 1",
            "action": "netem_set",
            "instance": "C1",
            "rate": "100mbit",
            "delay_ms": 2,
        },
        {"trigger": "job 1", "action": "disk_fill", "instance": "F1"},
        {
            "trigger": "job 1",
            "action": "header_edit",
            "instance": "F1",
            "path": "/usr/include/example.h",
        },
    ],
)
def test_each_timeline_action_has_one_unambiguous_valid_form(
    event: dict[str, object], tmp_path: Path
) -> None:
    farm, scenario = _documents()
    scenario["timeline"] = [event]
    _load(tmp_path, farm, scenario)


def _add_instance(
    scenario: dict[str, object], *, name: str, role: str, host: str, image: str
) -> None:
    instance: dict[str, object] = {"name": name, "role": role, "host": host, "image": image}
    if role == "F":
        instance["slots"] = 1
    scenario["instances"].append(instance)


@pytest.mark.parametrize("shape", ("SCF", "S'CF", "S'FC'", "S'C'F'"))
def test_simple_shape_must_match_instance_generations(shape: str, tmp_path: Path) -> None:
    farm, scenario = _documents()
    scenario["shape"] = shape
    scenario["images"]["old"] = "p43-1.4.0"
    if shape == "SCF":
        for instance in scenario["instances"]:
            instance["image"] = "old"
    elif shape == "S'CF":
        scenario["instances"][1]["image"] = "old"
        scenario["instances"][2]["image"] = "old"
    elif shape == "S'FC'":
        scenario["instances"][1]["image"] = "old"
    _load(tmp_path, farm, scenario)

    scenario["instances"][0]["image"] = "new" if shape == "SCF" else "old"
    with pytest.raises(ScenarioSpecError, match="does not match"):
        _load(tmp_path, farm, scenario)


def test_mixed_pool_shape_requires_old_and_new_workers_and_clients(tmp_path: Path) -> None:
    farm, scenario = _documents()
    scenario["shape"] = "S'[FF'][CC']"
    scenario["images"]["old"] = "p43-1.4.0"
    scenario["instances"][1]["image"] = "old"
    scenario["instances"][2]["image"] = "old"
    _add_instance(scenario, name="F2", role="F", host="tt-quietbox3", image="new")
    _add_instance(scenario, name="C2", role="C", host="tt-quietbox3", image="new")
    scenario["workload"]["clients"].append("C2")
    _load(tmp_path, farm, scenario)

    scenario["instances"][-1]["image"] = "old"
    with pytest.raises(ScenarioSpecError, match="does not match"):
        _load(tmp_path, farm, scenario)


def test_mixed_pool_workload_must_run_both_client_generations(tmp_path: Path) -> None:
    farm, scenario = _documents()
    scenario["shape"] = "S'[FF'][CC']"
    scenario["images"]["old"] = "p43-1.4.0"
    scenario["instances"][1]["image"] = "old"
    scenario["instances"][2]["image"] = "old"
    _add_instance(scenario, name="F2", role="F", host="tt-quietbox3", image="new")
    _add_instance(scenario, name="C2", role="C", host="tt-quietbox3", image="new")
    with pytest.raises(ScenarioSpecError, match="must run both old and new clients"):
        _load(tmp_path, farm, scenario)


def test_revision_skew_shape_requires_two_distinct_p50_worker_images(tmp_path: Path) -> None:
    farm, scenario = _documents()
    scenario["shape"] = "S'[F'F''][C']"
    scenario["images"] = {"r1": "p50s1-aaaaaaaa", "r2": "p50s2-bbbbbbbb"}
    farm["authority"]["images"]["p50s1-aaaaaaaa"] = {
        "commit": "a" * 40,
        "archive_sha256": "a" * 64,
    }
    farm["authority"]["images"]["p50s2-bbbbbbbb"] = {
        "commit": "b" * 40,
        "archive_sha256": "b" * 64,
    }
    for instance in scenario["instances"]:
        instance["image"] = "r1"
    _add_instance(scenario, name="F2", role="F", host="tt-quietbox3", image="r2")
    _load(tmp_path, farm, scenario)

    scenario["instances"][-1]["image"] = "r1"
    with pytest.raises(ScenarioSpecError, match="does not match"):
        _load(tmp_path, farm, scenario)


def test_image_label_must_encode_supported_generation(tmp_path: Path) -> None:
    farm, scenario = _documents()
    scenario["images"]["new"] = "latest"
    with pytest.raises(ScenarioSpecError, match="must encode p43, p44, or p50"):
        _load(tmp_path, farm, scenario)


def test_scenario_image_must_be_in_immutable_authority_map(tmp_path: Path) -> None:
    farm, scenario = _documents()
    scenario["images"]["new"] = "p50s9-deadbeef"
    with pytest.raises(ScenarioSpecError, match="immutable authority map"):
        _load(tmp_path, farm, scenario)


@pytest.mark.parametrize(
    ("instance_index", "environment", "error"),
    [
        (1, {"ICECC_P50_PROFILE": "P29V1"}, "only S owns profile selection"),
        (0, {"ICECC_P50_MODE": "off"}, "only C owns the client mode"),
        (2, {"ICECC_P50_MODE": "sometimes"}, "must be 'on' or 'off'"),
    ],
)
def test_p50_controls_are_owned_by_the_correct_instance_role(
    instance_index: int,
    environment: dict[str, str],
    error: str,
    tmp_path: Path,
) -> None:
    farm, scenario = _documents()
    scenario["instances"][instance_index]["env"] = environment
    with pytest.raises(ScenarioSpecError, match=error):
        _load(tmp_path, farm, scenario)


@pytest.mark.parametrize(
    "event",
    [
        {
            "trigger": "job 1",
            "action": "env_set",
            "instance": "S1",
            "env": {"ICECC_P50_PROFILE": "OFF"},
        },
        {
            "trigger": "job 1",
            "action": "env_set",
            "instance": "S1",
            "env": {"ICECC_P50_PROFILE": "ZSTD_ROUTE"},
        },
        {
            "trigger": "job 1",
            "action": "env_set",
            "instance": "C1",
            "env": {"ICECC_P50_MODE": "off"},
        },
    ],
)
def test_timeline_accepts_only_documented_p50_switch_forms(
    event: dict[str, object], tmp_path: Path
) -> None:
    farm, scenario = _documents()
    scenario["timeline"] = [event]
    _load(tmp_path, farm, scenario)


@pytest.mark.parametrize(
    ("instance", "environment"),
    [
        ("S1", {}),
        ("S1", {"ICECC_P50_MODE": "off"}),
        ("C1", {"PATH": "/tmp"}),
        ("C1", {"ICECC_P50_MODE": "invalid"}),
    ],
)
def test_timeline_rejects_ambiguous_or_wrong_p50_switch_forms(
    instance: str, environment: dict[str, str], tmp_path: Path
) -> None:
    farm, scenario = _documents()
    scenario["timeline"] = [
        {"trigger": "job 1", "action": "env_set", "instance": instance, "env": environment}
    ]
    with pytest.raises(ScenarioSpecError, match="env_set|must be 'on' or 'off'"):
        _load(tmp_path, farm, scenario)
