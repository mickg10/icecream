from __future__ import annotations

import dataclasses
import hashlib
import json
from pathlib import Path

import pytest

from farmharness.integration.tests import farm_fixture

from farmharness.integration import farmtest
from farmharness.integration.farm_spec import load_farm_spec
from farmharness.integration.images import (
    CommandFactory,
    ImageError,
    RecordingTransport,
    _image_identity,
    build_and_distribute,
    build_and_distribute_foundations,
    build_image,
    create_source_archive,
    distribute_image,
    ensure_product_image,
    foundation_targets_for_farm,
    foundation_targets_for_scenario,
    image_bindings,
)
from farmharness.integration.remote import (
    CommandResult,
    PlannedCommand,
    RemoteError,
    decode_ssh_payload,
)


INTEGRATION = Path(__file__).resolve().parents[1]
REPO = INTEGRATION.parents[1]
GOOD_ID = "sha256:" + "1" * 64
BAD_ID = "sha256:" + "2" * 64
REQUIRES_GIT_HISTORY = pytest.mark.skipif(
    not (REPO / ".git").exists(),
    reason="requires a Git checkout to verify historical source-archive authority",
)


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
        warm_hosts: set[str] | None = None,
    ) -> None:
        self.hub_id = hub_id
        self.host_ids = host_ids or {}
        self.host_closures = host_closures or {}
        self.push_fails = push_fails
        self.warm_hosts = warm_hosts or set()
        self.commands: list[PlannedCommand] = []

    def invoke(self, command: PlannedCommand) -> CommandResult:
        self.commands.append(command)
        if command.phase == "images.push" and self.push_fails:
            raise RemoteError("registry unavailable")
        if command.phase in (
            "images.inspect-hub",
            "images.inspect-hub-cached",
            "images.inspect-transport-hub",
            "images.foundation-inspect-cached",
            "images.foundation-inspect-candidate",
            "images.foundation-inspect-final",
        ):
            return CommandResult(0, _inspect_json(self.hub_id) + "\n", "")
        if command.phase == "images.inspect-repodigests":
            reference = command.argv[-1].rsplit(":", 1)[0]
            return CommandResult(0, f'["{reference}@sha256:{"3" * 64}"]\n', "")
        if command.phase == "images.save-compressed":
            archive = Path(command.argv[-1])
            archive.parent.mkdir(parents=True, exist_ok=True)
            compressed = b"saved-image-zstd19-long31"
            raw = b"saved-image-raw-stream"
            archive.write_bytes(compressed)
            return CommandResult(
                0,
                json.dumps(
                    {
                        "compressed_bytes": len(compressed),
                        "compressed_sha256": hashlib.sha256(compressed).hexdigest(),
                        "raw_bytes": len(raw),
                        "raw_sha256": hashlib.sha256(raw).hexdigest(),
                        "zstd": {
                            "check": True,
                            "level": 19,
                            "long": 31,
                            "threads": 8,
                            "version": "*** Zstandard CLI (64-bit) v1.5.7",
                        },
                    }
                )
                + "\n",
                "",
            )
        if command.phase == "images.verify-compressed":
            remote = decode_ssh_payload(command.argv)
            path, size, sha256 = remote[-3:]
            return CommandResult(
                0,
                json.dumps({"bytes": int(size), "path": path, "sha256": sha256}) + "\n",
                "",
            )
        if command.phase == "images.inspect-host-cached":
            if command.host not in self.warm_hosts:
                raise RemoteError("image is not cached on this host")
            return CommandResult(0, _inspect_json(GOOD_ID) + "\n", "")
        if command.phase in ("images.inspect-host", "images.inspect-transport-host"):
            values = self.host_ids.setdefault(command.host, [GOOD_ID])
            value = values.pop(0) if len(values) > 1 else values[0]
            markers = self.host_closures.setdefault(command.host, ["same"])
            marker = markers.pop(0) if len(markers) > 1 else markers[0]
            return CommandResult(0, _inspect_json(value, marker) + "\n", "")
        return CommandResult(0, "", "")


def _farm():
    farm = load_farm_spec(farm_fixture.example_farm_path())
    for image in farm.data["authority"]["images"].values():
        image["closure_sha256"] = GOOD_IDENTITY.closure_sha256
    return farm


@pytest.mark.parametrize(
    "label",
    ("p43-1.4.0", "p50-4c994915", "p50s2-624702e9"),
)
@REQUIRES_GIT_HISTORY
def test_each_source_archive_matches_immutable_authority(
    label: str, tmp_path: Path
) -> None:
    farm = _farm()
    binding = image_bindings(farm, [label])[0]
    archive = tmp_path / f"{label}.tar"
    create_source_archive(REPO, binding, archive)
    assert archive.is_file()


def test_scheduler_entrypoint_accepts_only_named_assignment_fence_modes() -> None:
    entrypoint = (INTEGRATION / "docker" / "entry-scheduler.sh").read_text()
    assert "--assignment-fence-mode" in entrypoint
    assert "legacy|advisory|enforcing-compat|strict-nonce" in entrypoint
    assert 'set -- "$@" --assignment-fence-mode "$assignment_fence_mode"' in entrypoint


def test_role_entrypoints_use_the_foundation_portable_runtime_account() -> None:
    """Product runtimes must not depend on users from their build image.

    The product's ``/opt/icecream`` tree is mounted read-only into the stable
    S/F image or one of four heterogeneous C images.  Those foundations all
    provide the standard non-root ``nobody`` account, whereas the product-only
    ``icecc`` account is deliberately not part of their identity.
    """

    for name in ("entry-scheduler.sh", "entry-daemon.sh", "entry-client.sh"):
        entrypoint = (INTEGRATION / "docker" / name).read_text()
        assert "runtime_user=nobody" in entrypoint
        assert "-u icecc" not in entrypoint
        assert "-o icecc" not in entrypoint
        assert "chown -R icecc:icecc" not in entrypoint
        assert '-u "$runtime_user"' in entrypoint


@REQUIRES_GIT_HISTORY
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


def test_build_uses_commit_not_display_label_and_binds_runtime_closure(
    tmp_path: Path,
) -> None:
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
    assert all(
        "sh" not in command.argv and "bash" not in command.argv
        for command in recorder.commands
    )


def test_receipt_binds_saved_transport_archive(tmp_path: Path) -> None:
    farm = _farm()
    farm.data["registry"]["mode"] = "save-load"
    output = tmp_path / "images.json"
    recorder = RecordingTransport(ScriptedRecorder())
    receipt = build_and_distribute(
        farm,
        ["p50s2-624702e9"],
        repo=REPO,
        output=output,
        recorder=recorder,
    )
    image = receipt["images"]["p50s2-624702e9"]
    assert image["built"] is False
    assert all(command.phase != "images.build" for command in recorder.commands)
    assert image["closure_schema"] == "docker-inspect-runtime-closure-v1"
    transport = image["transport_archive"]
    assert transport["schema"] == "icefarm-docker-save-zstd-v1"
    assert transport["format"] == "docker-save-tar-zstd"
    assert transport["artifact"] == {
        "bytes": len(b"saved-image-zstd19-long31"),
        "sha256": hashlib.sha256(b"saved-image-zstd19-long31").hexdigest(),
    }
    assert transport["zstd"] == {
        "check": True,
        "level": 19,
        "long": 31,
        "threads": 8,
        "version": "*** Zstandard CLI (64-bit) v1.5.7",
    }
    assert transport["path"].endswith(".docker.tar.zst")
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


def test_authority_bound_cached_product_image_skips_rebuild(tmp_path: Path) -> None:
    farm = _farm()
    binding = dataclasses.replace(
        image_bindings(farm, ["p50s2-624702e9"])[0], expected_id=GOOD_ID
    )
    recorder = RecordingTransport(ScriptedRecorder())

    observed, built = ensure_product_image(
        farm,
        binding,
        REPO,
        tmp_path / "unused-context",
        recorder,
        CommandFactory(),
        timeout_s=10,
    )

    assert observed.native_id == GOOD_ID
    assert observed.closure_sha256 == binding.expected_closure
    assert built is False
    assert not (tmp_path / "unused-context").exists()
    assert [command.phase for command in recorder.commands] == [
        "images.inspect-hub-cached"
    ]


def test_uncaptured_candidate_product_image_is_always_rebuilt(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    binding = image_bindings(
        farm, ["p50s30-f-refusal-mutant-candidate"]
    )[0]
    assert binding.expected_closure is None
    recorder = RecordingTransport(ScriptedRecorder())
    prepared: list[tuple[Path, object, Path]] = []

    def prepare(repo: Path, selected: object, context: Path) -> None:
        prepared.append((repo, selected, context))
        context.mkdir(parents=True)
        (context / "source.tar").write_bytes(b"source-archive-fixture")

    monkeypatch.setattr(
        "farmharness.integration.images.prepare_build_context", prepare
    )

    _observed, built = ensure_product_image(
        farm,
        binding,
        REPO,
        tmp_path / "candidate-context",
        recorder,
        CommandFactory(),
        timeout_s=10,
    )

    assert built is True
    assert prepared == [(REPO, binding, tmp_path / "candidate-context")]
    assert (tmp_path / "candidate-context" / "source.tar").is_file()
    assert any(command.phase == "images.build" for command in recorder.commands)
    assert all(
        command.phase != "images.inspect-hub-cached"
        for command in recorder.commands
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
    assert (
        sum(command.phase == "images.save-compressed" for command in recorder.commands)
        == 1
    )
    assert sum(
        command.phase == "images.load-compressed" for command in recorder.commands
    ) == len(farm.hosts)
    assert all(
        item["transport_archive"]["path"].endswith(".docker.tar.zst")
        for item in result.values()
    )


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
            str(farm_fixture.example_farm_path()),
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


def test_foundation_packager_keeps_four_clients_distinct_and_targets_only_selected_host(
    tmp_path: Path,
) -> None:
    farm = _farm()
    farm.data["runtime_image"]["id"] = GOOD_ID
    farm.data["runtime_image"]["closure_sha256"] = GOOD_IDENTITY.closure_sha256
    for environment in farm.data["client_environments"].values():
        environment["id"] = GOOD_ID
        environment["closure_sha256"] = GOOD_IDENTITY.closure_sha256
    assert sorted(farm.data["client_environments"]) == [
        "conan-gcc",
        "debian-gcc",
        "fedora-clang-libcxx",
        "linuxbrew",
    ]
    target = next(
        name for name, host in farm.hosts.items() if "C" in host["roles_allowed"]
    )
    recorder = RecordingTransport(ScriptedRecorder())
    receipt = build_and_distribute_foundations(
        farm,
        repo=REPO,
        output=tmp_path / "foundations.json",
        target_hosts={"linuxbrew": [target]},
        recorder=recorder,
    )
    assert receipt["schema"] == "icefarm-foundation-images-v1"
    assert set(receipt["images"]) == {"linuxbrew"}
    image = receipt["images"]["linuxbrew"]
    assert image["built"] is False
    assert set(image["hosts"]) == {target}
    assert image["transport_archive"]["zstd"]["level"] == 19
    assert image["transport_archive"]["zstd"]["long"] == 31
    assert image["transport_archive"]["path"].endswith(".docker.tar.zst")
    assert (
        sum(command.phase == "images.save-compressed" for command in recorder.commands)
        == 1
    )
    assert (
        sum(command.phase == "images.sync-compressed" for command in recorder.commands)
        == 1
    )
    assert all(
        command.phase != "images.foundation-build" for command in recorder.commands
    )


def test_foundation_distribution_skips_transfer_and_load_for_authenticated_warm_host(
    tmp_path: Path,
) -> None:
    farm = _farm()
    farm.data["runtime_image"]["id"] = GOOD_ID
    farm.data["runtime_image"]["closure_sha256"] = GOOD_IDENTITY.closure_sha256
    target = next(
        name for name, host in farm.hosts.items() if "F" in host["roles_allowed"]
    )
    recorder = RecordingTransport(ScriptedRecorder(warm_hosts={target}))
    receipt = build_and_distribute_foundations(
        farm,
        repo=REPO,
        output=tmp_path / "foundations.json",
        target_hosts={"runtime": [target]},
        recorder=recorder,
    )
    assert receipt["images"]["runtime"]["hosts"][target]["closure_sha256"] == (
        GOOD_IDENTITY.closure_sha256
    )
    assert (
        sum(
            command.phase == "images.inspect-host-cached"
            for command in recorder.commands
        )
        == 1
    )
    assert all(
        command.phase not in {"images.sync-compressed", "images.load-compressed"}
        for command in recorder.commands
    )


def test_foundation_cached_native_id_may_differ_when_portable_closure_matches(
    tmp_path: Path,
) -> None:
    farm = _farm()
    farm.data["runtime_image"]["id"] = GOOD_ID
    farm.data["runtime_image"]["closure_sha256"] = GOOD_IDENTITY.closure_sha256
    recorder = RecordingTransport(ScriptedRecorder(hub_id=BAD_ID))

    receipt = build_and_distribute_foundations(
        farm,
        repo=REPO,
        output=tmp_path / "foundations.json",
        target_hosts={"runtime": []},
        recorder=recorder,
    )

    assert receipt["images"]["runtime"]["hub_id"] == BAD_ID
    assert receipt["images"]["runtime"]["built"] is False
    assert all(
        command.phase != "images.foundation-build" for command in recorder.commands
    )


def test_foundation_targets_select_runtime_for_sf_and_one_environment_per_c() -> None:
    targets = foundation_targets_for_scenario(
        {
            "instances": [
                {"name": "S1", "role": "S", "host": "q3"},
                {"name": "F1", "role": "F", "host": "q2"},
                {
                    "name": "C1",
                    "role": "C",
                    "host": "q3",
                    "client_environment": "conan-gcc",
                },
                {
                    "name": "C2",
                    "role": "C",
                    "host": "q2",
                    "client_environment": "linuxbrew",
                },
            ]
        }
    )
    assert targets == {
        "conan-gcc": ["q3"],
        "linuxbrew": ["q2"],
        "runtime": ["q2", "q3"],
    }


def test_default_foundation_targets_cover_stable_runtime_and_all_four_clients() -> None:
    farm = _farm()

    targets = foundation_targets_for_farm(farm)

    runtime_hosts = sorted(
        name
        for name, host in farm.hosts.items()
        if set(host["roles_allowed"]).intersection(("S", "F"))
    )
    client_hosts = sorted(
        name for name, host in farm.hosts.items() if "C" in host["roles_allowed"]
    )
    assert targets["runtime"] == runtime_hosts
    assert set(targets) == {"runtime", *farm.data["client_environments"]}
    assert all(targets[key] == client_hosts for key in farm.data["client_environments"])


def test_farmtest_foundations_cli_uses_scenario_specific_targets(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    observed: dict[str, object] = {}

    def fake_build(farm, *, repo, output, target_hosts):
        observed.update(repo=repo, output=output, target_hosts=target_hosts)
        return {"schema": "icefarm-foundation-images-v1", "images": {}}

    monkeypatch.setattr(farmtest, "build_and_distribute_foundations", fake_build)
    output = tmp_path / "foundations.json"
    rc = farmtest.main(
        [
            "images",
            "--farm",
            str(farm_fixture.example_farm_path()),
            "--foundations",
            "--scenario",
            str(INTEGRATION / "scenarios" / "S00-smoke.json"),
            "--repo",
            str(REPO),
            "--output",
            str(output),
        ]
    )
    assert rc == 0
    assert observed == {
        "output": output,
        "repo": REPO,
        "target_hosts": {
            "debian-gcc": ["tt-quietbox3"],
            "runtime": ["tt-quietbox2", "tt-quietbox3"],
        },
    }
