from __future__ import annotations

import dataclasses
import hashlib
import json
from pathlib import Path

import pytest

from farmharness.integration import farmtest
from farmharness.integration.farm_spec import load_farm_spec
from farmharness.integration.images import (
    CommandFactory,
    ImageError,
    RecordingTransport,
    _image_identity,
    build_and_distribute,
    build_image,
    create_source_archive,
    distribute_image,
    image_bindings,
)
from farmharness.integration.remote import CommandResult, PlannedCommand, RemoteError


INTEGRATION = Path(__file__).resolve().parents[1]
REPO = INTEGRATION.parents[1]
GOOD_ID = "sha256:" + "1" * 64
BAD_ID = "sha256:" + "2" * 64


def _inspect_json(native_id: str, marker: str = "same") -> str:
    return json.dumps(
        {
            "Id": native_id,
            "Architecture": "amd64",
            "Os": "linux",
            "Created": "2026-09-04T00:00:00Z",
            "Config": {"Labels": {"test.closure": marker}},
            "RootFS": {"Type": "layers", "Layers": [f"sha256:{marker}"]},
        },
        separators=(",", ":"),
    )


GOOD_IDENTITY = _image_identity(CommandResult(0, _inspect_json(GOOD_ID), ""), "test")


class ScriptedRecorder:
    def __init__(
        self,
        *,
        hub_id: str = GOOD_ID,
        host_ids: dict[str, list[str]] | None = None,
        host_closures: dict[str, list[str]] | None = None,
        push_fails: bool = False,
    ) -> None:
        self.hub_id = hub_id
        self.host_ids = host_ids or {}
        self.host_closures = host_closures or {}
        self.push_fails = push_fails
        self.commands: list[PlannedCommand] = []

    def invoke(self, command: PlannedCommand) -> CommandResult:
        self.commands.append(command)
        if command.phase == "images.push" and self.push_fails:
            raise RemoteError("registry unavailable")
        if command.phase == "images.inspect-hub":
            return CommandResult(0, _inspect_json(self.hub_id) + "\n", "")
        if command.phase == "images.inspect-repodigests":
            reference = command.argv[-1].rsplit(":", 1)[0]
            return CommandResult(0, f'["{reference}@sha256:{"3" * 64}"]\n', "")
        if command.phase == "images.save":
            Path(command.argv[-2]).write_bytes(b"saved-image")
        if command.phase == "images.inspect-host":
            values = self.host_ids.setdefault(command.host, [GOOD_ID])
            value = values.pop(0) if len(values) > 1 else values[0]
            markers = self.host_closures.setdefault(command.host, ["same"])
            marker = markers.pop(0) if len(markers) > 1 else markers[0]
            return CommandResult(0, _inspect_json(value, marker) + "\n", "")
        return CommandResult(0, "", "")


def _farm():
    farm = load_farm_spec(INTEGRATION / "farm.example.json")
    for image in farm.data["authority"]["images"].values():
        image["closure_sha256"] = GOOD_IDENTITY.closure_sha256
    return farm


@pytest.mark.parametrize(
    "label",
    ("p43-1.4.0", "p50-4c994915", "p50s2-624702e9"),
)
def test_each_source_archive_matches_immutable_authority(label: str, tmp_path: Path) -> None:
    farm = _farm()
    binding = image_bindings(farm, [label])[0]
    archive = tmp_path / f"{label}.tar"
    create_source_archive(REPO, binding, archive)
    assert archive.is_file()


def test_archive_hash_mismatch_is_removed_and_refused(tmp_path: Path) -> None:
    binding = image_bindings(_farm(), ["p50s2-624702e9"])[0]
    bad = dataclasses.replace(binding, archive_sha256="f" * 64)
    archive = tmp_path / "source.tar"
    with pytest.raises(ImageError, match="source archive mismatch"):
        create_source_archive(REPO, bad, archive)
    assert not archive.exists()


def test_unknown_or_duplicate_labels_are_refused() -> None:
    farm = _farm()
    with pytest.raises(ImageError, match="immutable authority map"):
        image_bindings(farm, ["p50s9-unknown"])
    with pytest.raises(ImageError, match="duplicate"):
        image_bindings(farm, ["p43-1.4.0", "p43-1.4.0"])


def test_build_uses_commit_not_display_label_and_binds_runtime_closure(tmp_path: Path) -> None:
    farm = _farm()
    binding = image_bindings(farm, ["p50s2-624702e9"])[0]
    scripted = ScriptedRecorder()
    recorder = RecordingTransport(scripted)
    observed = build_image(
        farm,
        binding,
        tmp_path,
        recorder,
        CommandFactory(),
        timeout_s=10,
    )
    assert observed.native_id == GOOD_ID
    assert observed.closure_sha256 == GOOD_IDENTITY.closure_sha256
    build = recorder.commands[0]
    assert f"SOURCE_COMMIT={binding.commit}" in build.argv
    assert f"SOURCE_ARCHIVE_SHA256={binding.archive_sha256}" in build.argv
    assert binding.label not in build.argv[build.argv.index("--build-arg") + 1]
    assert all("sh" not in command.argv and "bash" not in command.argv for command in recorder.commands)


def test_receipt_binds_saved_transport_archive(tmp_path: Path) -> None:
    farm = _farm()
    farm.data["registry"]["mode"] = "save-load"
    output = tmp_path / "images.json"
    receipt = build_and_distribute(
        farm,
        ["p50s2-624702e9"],
        repo=REPO,
        output=output,
        recorder=RecordingTransport(ScriptedRecorder()),
    )
    image = receipt["images"]["p50s2-624702e9"]
    assert image["closure_schema"] == "docker-inspect-runtime-closure-v1"
    assert image["transport_archive"] == {
        "bytes": len(b"saved-image"),
        "sha256": hashlib.sha256(b"saved-image").hexdigest(),
    }
    assert json.loads(output.read_text()) == receipt


def test_preexisting_expected_hub_id_mismatch_is_refused(tmp_path: Path) -> None:
    farm = _farm()
    binding = dataclasses.replace(
        image_bindings(farm, ["p50s2-624702e9"])[0], expected_id=BAD_ID
    )
    recorder = RecordingTransport(ScriptedRecorder())
    with pytest.raises(ImageError, match="hub image id mismatch"):
        build_image(
            farm,
            binding,
            tmp_path,
            recorder,
            CommandFactory(),
            timeout_s=10,
        )


def test_preexisting_expected_hub_closure_mismatch_is_refused(tmp_path: Path) -> None:
    farm = _farm()
    binding = dataclasses.replace(
        image_bindings(farm, ["p50s2-624702e9"])[0], expected_closure="f" * 64
    )
    recorder = RecordingTransport(ScriptedRecorder())
    with pytest.raises(ImageError, match="hub image closure mismatch"):
        build_image(
            farm,
            binding,
            tmp_path,
            recorder,
            CommandFactory(),
            timeout_s=10,
        )


def test_auto_registry_failure_falls_back_to_one_save_and_load_per_host(
    tmp_path: Path,
) -> None:
    farm = _farm()
    farm.data["registry"]["mode"] = "auto"
    binding = image_bindings(farm, ["p50s2-624702e9"])[0]
    scripted = ScriptedRecorder(push_fails=True)
    recorder = RecordingTransport(scripted)
    result = distribute_image(
        farm,
        binding,
        GOOD_IDENTITY,
        tmp_path / "image.tar",
        recorder,
        CommandFactory(),
        timeout_s=10,
    )
    assert set(result) == set(farm.hosts)
    assert {item["mode"] for item in result.values()} == {"save-load"}
    assert sum(command.phase == "images.save" for command in recorder.commands) == 1
    assert sum(command.phase == "images.load" for command in recorder.commands) == len(farm.hosts)


def test_registry_id_mismatch_falls_back_in_auto_mode(tmp_path: Path) -> None:
    farm = _farm()
    farm.data["registry"]["mode"] = "auto"
    first_host = next(iter(farm.hosts))
    binding = image_bindings(farm, ["p50s2-624702e9"])[0]
    scripted = ScriptedRecorder(
        host_ids={first_host: [BAD_ID, GOOD_ID]},
        host_closures={first_host: ["different", "same"]},
    )
    recorder = RecordingTransport(scripted)
    result = distribute_image(
        farm,
        binding,
        GOOD_IDENTITY,
        tmp_path / "image.tar",
        recorder,
        CommandFactory(),
        timeout_s=10,
    )
    assert result[first_host]["mode"] == "save-load"
    assert result[first_host]["id"] == GOOD_ID


def test_native_id_representation_may_differ_when_runtime_closure_is_identical(
    tmp_path: Path,
) -> None:
    farm = _farm()
    farm.data["registry"]["mode"] = "save-load"
    first_host = next(iter(farm.hosts))
    binding = image_bindings(farm, ["p50s2-624702e9"])[0]
    scripted = ScriptedRecorder(host_ids={first_host: [BAD_ID]})
    recorder = RecordingTransport(scripted)
    result = distribute_image(
        farm,
        binding,
        GOOD_IDENTITY,
        tmp_path / "image.tar",
        recorder,
        CommandFactory(),
        timeout_s=10,
    )
    assert result[first_host]["id"] == BAD_ID
    assert result[first_host]["closure_sha256"] == GOOD_IDENTITY.closure_sha256


def test_save_load_id_mismatch_is_refused_before_any_up_phase(tmp_path: Path) -> None:
    farm = _farm()
    farm.data["registry"]["mode"] = "save-load"
    first_host = next(iter(farm.hosts))
    binding = image_bindings(farm, ["p50s2-624702e9"])[0]
    scripted = ScriptedRecorder(
        host_ids={first_host: [BAD_ID]},
        host_closures={first_host: ["different"]},
    )
    recorder = RecordingTransport(scripted)
    with pytest.raises(ImageError, match="image closure mismatch"):
        distribute_image(
            farm,
            binding,
            GOOD_IDENTITY,
            tmp_path / "image.tar",
            recorder,
            CommandFactory(),
            timeout_s=10,
        )
    assert all(not command.phase.startswith("up") for command in recorder.commands)


def test_entrypoints_parse_as_shell_and_dockerfile_omits_libbsc() -> None:
    docker = INTEGRATION / "docker"
    for name in ("entry-scheduler.sh", "entry-daemon.sh", "entry-client.sh"):
        result = __import__("subprocess").run(
            ["sh", "-n", str(docker / name)],
            check=False,
            capture_output=True,
            text=True,
        )
        assert result.returncode == 0, result.stderr
    dockerfile = (docker / "Dockerfile.icecream").read_text()
    assert "libxxhash-dev" in dockerfile
    assert "libbsc" not in dockerfile.lower()


def test_farmtest_images_cli_selects_only_requested_authority_labels(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    observed: dict[str, object] = {}

    def fake_build(farm, labels, *, repo, output):
        observed.update(labels=list(labels), repo=repo, output=output)
        return {"schema": "icefarm-images-v1", "images": {}}

    monkeypatch.setattr(farmtest, "build_and_distribute", fake_build)
    rc = farmtest.main(
        [
            "images",
            "--farm",
            str(INTEGRATION / "farm.example.json"),
            "--labels",
            "p43-1.4.0,p50s2-624702e9",
            "--repo",
            str(REPO),
            "--output",
            str(tmp_path / "images.json"),
        ]
    )
    assert rc == 0
    assert observed["labels"] == ["p43-1.4.0", "p50s2-624702e9"]
    assert observed["repo"] == REPO
    assert observed["output"] == tmp_path / "images.json"
    assert '"schema": "icefarm-images-v1"' in capsys.readouterr().out
