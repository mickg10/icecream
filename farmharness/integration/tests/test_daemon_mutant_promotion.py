from __future__ import annotations

import copy
import json
from pathlib import Path

import pytest

from farmharness.integration.tests import farm_fixture

from farmharness.integration.daemon_mutant_promotion import (
    DaemonMutantPromotionError,
    promote_daemon_mutant,
)
from farmharness.integration import farmtest
from farmharness.integration.farm_spec import load_farm_spec
from farmharness.integration.images import (
    CommandFactory,
    DAEMON_ROLE_PATH,
    DAEMON_ROLE_PROBE_LABEL,
    ImageError,
    ImageIdentity,
    build_and_distribute,
    _probe_daemon_role_hash,
    image_bindings,
)
from farmharness.integration.remote import CommandResult


INTEGRATION = Path(__file__).resolve().parents[1]
LABEL = "p50s30-f-refusal-mutant-candidate"
S90_LABEL = "p50s90-f-revision-2-candidate"
ROLE_SHA = "d" * 64


def _farm():
    return load_farm_spec(farm_fixture.example_farm_path())


def _receipt(farm, label: str = LABEL):
    binding = image_bindings(farm, [label])[0]
    closure = "a" * 64
    reference = binding.reference
    probe_argv = [
        "docker", "run", "--rm", "--pull=never", "--network", "none",
        "--cap-drop=ALL", "--security-opt=no-new-privileges", "--read-only",
        "--label", DAEMON_ROLE_PROBE_LABEL, "--entrypoint", "/usr/bin/sha256sum",
        reference, DAEMON_ROLE_PATH,
    ]
    observed = {
        "archive_sha256": binding.archive_sha256,
        "closure_schema": "docker-inspect-runtime-closure-v1",
        "closure_sha256": closure,
        "commit": binding.commit,
        "daemon_role_sha256": ROLE_SHA,
        "hosts": {
            host: {"closure_sha256": closure, "id": "sha256:" + "b" * 64}
            for host in farm.hosts
        },
        "hub_id": "sha256:" + "c" * 64,
        "reference": reference,
    }
    command = {
        "argv": probe_argv,
        "host": "hub",
        "instance": None,
        "phase": "images.probe-daemon-role",
        "sequence": 2,
        "timeout_s": 1800,
        "transport": "local-docker",
    }
    commands = [
        {**command, "argv": ["docker", "build"], "phase": "images.build", "sequence": 0},
        {**command, "argv": ["docker", "image", "inspect"], "phase": "images.inspect-hub", "sequence": 1},
        command,
    ]
    return {
        "commands": commands,
        "farm_digest": farm.digest,
        "images": {label: observed},
        "schema": "icefarm-images-v1",
    }


class ProbeRecorder:
    def __init__(self, stdout: str) -> None:
        self.stdout = stdout
        self.commands = []

    def invoke(self, command):
        self.commands.append(command)
        return CommandResult(0, self.stdout, "")


def test_daemon_probe_is_bounded_and_strictly_authenticated() -> None:
    farm = _farm()
    binding = image_bindings(farm, [LABEL])[0]
    recorder = ProbeRecorder(f"{ROLE_SHA}  {DAEMON_ROLE_PATH}\n")
    observed = _probe_daemon_role_hash(
        binding, recorder, CommandFactory(), timeout_s=17
    )
    assert observed == ROLE_SHA
    command = recorder.commands[0]
    assert command.phase == "images.probe-daemon-role"
    assert command.timeout_s == 17
    assert command.argv[0:4] == ("docker", "run", "--rm", "--pull=never")
    assert "--network" in command.argv and command.argv[command.argv.index("--network") + 1] == "none"
    assert "--cap-drop=ALL" in command.argv
    assert "--security-opt=no-new-privileges" in command.argv
    assert "--read-only" in command.argv
    assert DAEMON_ROLE_PROBE_LABEL in command.argv
    assert command.argv[-1] == DAEMON_ROLE_PATH


def test_daemon_probe_refuses_ambiguous_output() -> None:
    farm = _farm()
    binding = image_bindings(farm, [LABEL])[0]
    recorder = ProbeRecorder(f"{ROLE_SHA}  {DAEMON_ROLE_PATH}\nextra\n")
    with pytest.raises(ImageError, match="malformed output"):
        _probe_daemon_role_hash(binding, recorder, CommandFactory(), timeout_s=10)


def test_daemon_candidate_promotes_only_from_matching_receipt() -> None:
    farm = _farm()
    receipt = _receipt(farm)
    promoted = promote_daemon_mutant(farm, LABEL, receipt)
    candidate = promoted["authority"]["images"][LABEL]
    assert candidate["closure_sha256"] == "a" * 64
    assert candidate["id"] == "sha256:" + "c" * 64
    assert candidate["role_overrides"] == {"daemon": {"sha256": ROLE_SHA}}
    assert "closure_sha256" not in farm.data["authority"]["images"][LABEL]
    assert "role_overrides" not in farm.data["authority"]["images"][LABEL]


def test_two_daemon_promotions_require_fresh_sequential_farm_digest() -> None:
    original = _farm()
    stale_s90_receipt = _receipt(original, S90_LABEL)

    after_s30_data = promote_daemon_mutant(original, LABEL, _receipt(original))
    after_s30 = type(original)(path=original.path, data=after_s30_data)
    with pytest.raises(DaemonMutantPromotionError, match="farm digest differs"):
        promote_daemon_mutant(after_s30, S90_LABEL, stale_s90_receipt)

    after_s90 = promote_daemon_mutant(
        after_s30, S90_LABEL, _receipt(after_s30, S90_LABEL)
    )
    assert "closure_sha256" in after_s90["authority"]["images"][LABEL]
    assert "closure_sha256" in after_s90["authority"]["images"][S90_LABEL]
    assert "authority_capture" not in after_s90


@pytest.mark.parametrize("tamper", ("extra-image", "reference", "missing-probe", "duplicate-probe", "unsafe-probe"))
def test_daemon_candidate_refuses_probe_and_receipt_structure_tampering(tamper: str) -> None:
    farm = _farm()
    receipt = _receipt(farm)
    if tamper == "extra-image":
        receipt["images"]["other"] = dict(receipt["images"][LABEL])
    elif tamper == "reference":
        receipt["images"][LABEL]["reference"] = "registry.invalid/other:label"
    elif tamper == "missing-probe":
        receipt["commands"] = []
    elif tamper == "duplicate-probe":
        receipt["commands"].append(dict(receipt["commands"][0]))
    else:
        receipt["commands"][2]["argv"] = list(receipt["commands"][2]["argv"]) + ["unsafe"]
    with pytest.raises(DaemonMutantPromotionError):
        promote_daemon_mutant(farm, LABEL, receipt)


def test_daemon_candidate_refuses_already_promoted_authority() -> None:
    farm = _farm()
    candidate = farm.data["authority"]["images"][LABEL]
    candidate["closure_sha256"] = "a" * 64
    with pytest.raises(DaemonMutantPromotionError, match="already promoted"):
        promote_daemon_mutant(farm, LABEL, _receipt(farm))


def test_promoted_document_discards_stale_capture_without_mutating_input(tmp_path: Path) -> None:
    farm = _farm()
    captured = copy.deepcopy(farm.data)
    captured["authority_capture"] = {"authority_sha256": "0" * 64}
    farm_with_capture = type(farm)(path=farm.path, data=captured)
    receipt = _receipt(farm_with_capture)
    promoted = promote_daemon_mutant(farm_with_capture, LABEL, receipt)
    assert "authority_capture" not in promoted
    assert "authority_capture" in farm_with_capture.data
    output = tmp_path / "farm.json"
    output.write_text(json.dumps(promoted), encoding="utf-8")
    loaded = load_farm_spec(output)
    assert "authority_capture" not in loaded.data
    with_capture = tmp_path / "stale.json"
    with_capture.write_text(json.dumps(captured), encoding="utf-8")
    with pytest.raises(Exception, match="authority_capture|authority_sha256|bind"):
        load_farm_spec(with_capture)


def test_promotion_cli_writes_valid_uncaptured_output_once(tmp_path: Path) -> None:
    farm_path = farm_fixture.example_farm_path()
    farm = load_farm_spec(farm_path)
    receipt_path = tmp_path / "images.json"
    receipt_path.write_text(json.dumps(_receipt(farm)), encoding="utf-8")
    output = tmp_path / "farm-promoted.json"
    assert farmtest.main([
        "authority", "promote-daemon-mutant",
        "--farm", str(farm_path), "--receipt", str(receipt_path),
        "--label", LABEL, "--output", str(output),
    ]) == 0
    loaded = load_farm_spec(output)
    assert "authority_capture" not in loaded.data
    assert "authority_capture" not in farm.data
    assert farmtest.main([
        "authority", "promote-daemon-mutant",
        "--farm", str(farm_path), "--receipt", str(receipt_path),
        "--label", LABEL, "--output", str(output),
    ]) == 3


def test_promotion_cli_refuses_non_object_receipt(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    farm_path = farm_fixture.example_farm_path()
    receipt_path = tmp_path / "images.json"
    receipt_path.write_text("[]", encoding="utf-8")
    output = tmp_path / "farm-promoted.json"
    assert farmtest.main([
        "authority", "promote-daemon-mutant",
        "--farm", str(farm_path), "--receipt", str(receipt_path),
        "--label", LABEL, "--output", str(output),
    ]) == 3
    assert "image receipt must be a JSON object" in capsys.readouterr().err
    assert not output.exists()


def test_promotion_cli_refuses_dangling_symlink_output(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    farm_path = farm_fixture.example_farm_path()
    farm = load_farm_spec(farm_path)
    receipt_path = tmp_path / "images.json"
    receipt_path.write_text(json.dumps(_receipt(farm)), encoding="utf-8")
    output = tmp_path / "missing-target"
    output.symlink_to(tmp_path / "does-not-exist")
    assert farmtest.main([
        "authority", "promote-daemon-mutant",
        "--farm", str(farm_path), "--receipt", str(receipt_path),
        "--label", LABEL, "--output", str(output),
    ]) == 3
    assert "promotion output already exists" in capsys.readouterr().err
    assert output.is_symlink()


def test_promotion_cli_refuses_destination_created_after_initial_check(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    farm_path = farm_fixture.example_farm_path()
    farm = load_farm_spec(farm_path)
    receipt_path = tmp_path / "images.json"
    receipt_path.write_text(json.dumps(_receipt(farm)), encoding="utf-8")
    output = tmp_path / "farm-promoted.json"

    def race(source: Path, destination: Path) -> None:
        destination.write_text("raced", encoding="utf-8")
        raise FileExistsError(destination)

    monkeypatch.setattr(farmtest.os, "link", race)
    assert farmtest.main([
        "authority", "promote-daemon-mutant",
        "--farm", str(farm_path), "--receipt", str(receipt_path),
        "--label", LABEL, "--output", str(output),
    ]) == 3
    assert output.read_text(encoding="utf-8") == "raced"


def test_daemon_image_receipt_retains_observed_role_hash(tmp_path: Path, monkeypatch) -> None:
    farm = _farm()
    closure = "a" * 64
    monkeypatch.setattr(
        "farmharness.integration.images.prepare_build_context",
        lambda *_args: None,
    )
    monkeypatch.setattr(
        "farmharness.integration.images.build_image",
        lambda *_args, **_kwargs: ImageIdentity("sha256:" + "c" * 64, closure),
    )
    monkeypatch.setattr(
        "farmharness.integration.images._probe_daemon_role_hash",
        lambda *_args, **_kwargs: ROLE_SHA,
    )
    monkeypatch.setattr(
        "farmharness.integration.images._save_once",
        lambda *_args, **_kwargs: {
            "artifact": {"bytes": 1, "sha256": "e" * 64},
            "format": "docker-save-tar-zstd",
            "reference": "icefarm-transport:" + closure,
            "schema": "icefarm-docker-save-zstd-v1",
            "zstd": {"check": True, "level": 19, "long": 31, "threads": 8},
        },
    )
    monkeypatch.setattr(
        "farmharness.integration.images.distribute_image",
        lambda *_args, **_kwargs: {
            host: {"closure_sha256": closure, "id": "sha256:" + "b" * 64}
            for host in farm.hosts
        },
    )
    receipt = build_and_distribute(
        farm,
        [LABEL],
        repo=tmp_path,
        output=tmp_path / "images.json",
    )
    assert receipt["images"][LABEL]["daemon_role_sha256"] == ROLE_SHA


@pytest.mark.parametrize("tamper", ("farm_digest", "commit", "closure", "host", "role"))
def test_daemon_candidate_refuses_receipt_tampering(tamper: str) -> None:
    farm = _farm()
    receipt = _receipt(farm)
    if tamper == "farm_digest":
        receipt["farm_digest"] = "0" * 64
    elif tamper == "commit":
        receipt["images"][LABEL]["commit"] = "0" * 40
    elif tamper == "closure":
        receipt["images"][LABEL]["hosts"][next(iter(farm.hosts))]["closure_sha256"] = "0" * 64
    elif tamper == "host":
        receipt["images"][LABEL]["hosts"].pop(next(iter(farm.hosts)))
    else:
        receipt["images"][LABEL]["daemon_role_sha256"] = "not-a-digest"
    with pytest.raises(DaemonMutantPromotionError):
        promote_daemon_mutant(farm, LABEL, receipt)
