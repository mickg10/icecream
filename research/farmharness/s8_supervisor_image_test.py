from __future__ import annotations

import json
import re
from pathlib import Path

import pytest

import s8_supervisor_image as image


def _dockerfile(tmp_path: Path) -> Path:
    path = tmp_path / "Dockerfile"
    path.write_text(
        "FROM " + image.BASE_REFERENCE + "\n"
        "RUN apt-get update && apt-get install --yes --no-install-recommends "
        "python3 git docker.io && rm -rf /var/lib/apt/lists/*\n"
    )
    return path


def _inspect() -> bytes:
    return (json.dumps({"Id": "sha256:" + "a" * 64, "Os": "linux",
                        "Architecture": "amd64", "Created": "2026-08-31T00:00:00Z"},
                       sort_keys=True) + "\n").encode()


def _base_inspect(image_id: str = image.BASE_IMAGE_ID) -> bytes:
    return (json.dumps({"Id": image_id, "Os": "linux", "Architecture": "amd64",
                        "Created": "2026-08-31T00:00:00Z"}, sort_keys=True) + "\n").encode()


def test_receipt_binds_definition_image_id_platform_and_raw_inspect(tmp_path: Path) -> None:
    dockerfile = _dockerfile(tmp_path)
    inspect_path = tmp_path / "inspect.json"
    inspect_raw = _inspect()
    inspect_path.write_bytes(inspect_raw)
    base_raw = _base_inspect()
    base_path = tmp_path / "base-inspect.json"
    base_path.write_bytes(base_raw)
    receipt = image.make_receipt(dockerfile=dockerfile, image_ref="local/s8-supervisor:1",
                                 inspect_raw=inspect_raw, inspect_path=inspect_path,
                                 base_inspect_raw=base_raw, base_inspect_path=base_path,
                                 source_commit="b" * 40)
    assert receipt["schema"] == image.SCHEMA
    assert receipt["base"]["reference"] == image.BASE_REFERENCE
    assert receipt["base"]["source_reference"] == image.BASE_SOURCE_REFERENCE
    assert receipt["base"]["image_id"] == image.BASE_IMAGE_ID
    assert receipt["image"]["image_id"] == "sha256:" + "a" * 64
    assert receipt["image"]["os"] == "linux"
    assert receipt["image"]["architecture"] == "amd64"
    assert receipt["inspect"]["sha256"] == image._digest(inspect_raw)
    assert receipt["required_tools"] == ["python3", "git", "docker"]
    out = tmp_path / "receipt.json"
    raw = image.write_once(out, receipt)
    assert out.read_bytes() == raw
    with pytest.raises(image.ImageAuthorityError, match="already_exists"):
        image.write_once(out, receipt)


def test_receipt_rejects_wrong_id_or_platform(tmp_path: Path) -> None:
    dockerfile = _dockerfile(tmp_path)
    base_raw = _base_inspect()
    base_path = tmp_path / "base-inspect.json"
    base_path.write_bytes(base_raw)
    inspect_path = tmp_path / "inspect.json"
    for field, value in (("Id", "sha256:not-a-content-id"), ("Architecture", "arm64")):
        obj = {"Id": "sha256:" + "a" * 64, "Os": "linux", "Architecture": "amd64"}
        obj[field] = value
        raw = (json.dumps(obj) + "\n").encode()
        inspect_path.write_bytes(raw)
        with pytest.raises(image.ImageAuthorityError):
            image.make_receipt(dockerfile=dockerfile, image_ref="s8-supervisor:local",
                               inspect_raw=raw, inspect_path=inspect_path,
                               base_inspect_raw=base_raw, base_inspect_path=base_path)


def test_build_command_is_pinned_and_non_running(tmp_path: Path) -> None:
    dockerfile = _dockerfile(tmp_path)
    command = image.build_command(context=tmp_path, dockerfile=dockerfile,
                                  image_ref="local/s8-supervisor:1",
                                  base_inspect_raw=_base_inspect())
    assert command[:4] == ["docker", "build", "--pull=false", "--platform=linux/amd64"]
    assert "--file" in command and "--tag" in command
    assert "run" not in command
    binding = image.base_binding_command()
    assert binding[:3] == ["sh", "-eu", "-c"]
    assert "docker image inspect" in binding[3]
    assert "docker tag" in binding[3]
    assert image.BASE_SOURCE_REFERENCE in binding[3]
    assert image.BASE_REFERENCE in binding[3]
    assert image.BASE_IMAGE_ID in binding[3]


def test_base_preflight_rejects_wrong_local_content_id(tmp_path: Path) -> None:
    dockerfile = _dockerfile(tmp_path)
    child_raw = _inspect()
    child_path = tmp_path / "inspect.json"
    child_path.write_bytes(child_raw)
    with pytest.raises(image.ImageAuthorityError, match="base:content_id_mismatch"):
        image.build_command(context=tmp_path, dockerfile=dockerfile,
                            image_ref="local/s8-supervisor:1",
                            base_inspect_raw=_base_inspect("sha256:" + "c" * 64))

    with pytest.raises(image.ImageAuthorityError, match="base:content_id_mismatch"):
        image.make_receipt(dockerfile=dockerfile, image_ref="local/s8-supervisor:1",
                           inspect_raw=child_raw, inspect_path=child_path,
                           base_inspect_raw=_base_inspect("sha256:" + "d" * 64),
                           base_inspect_path=tmp_path / "base.json")


def test_dockerfile_contract_rejects_unpinned_base_or_missing_tool(tmp_path: Path) -> None:
    path = tmp_path / "Dockerfile"
    path.write_text("FROM ubuntu:22.04\nRUN apt-get install python3 git\n")
    with pytest.raises(image.ImageAuthorityError, match="base_reference_mismatch"):
        image._dockerfile_contract(path)


def test_dockerfile_contract_mutations_are_load_bearing(tmp_path: Path) -> None:
    valid = _dockerfile(tmp_path).read_text()
    changed_base = tmp_path / "changed-base.Dockerfile"
    changed_base.write_text(valid.replace(image.BASE_REFERENCE,
                                          "icecream/s8-supervisor-base:decoy"))
    with pytest.raises(image.ImageAuthorityError, match="base_reference_mismatch"):
        image._dockerfile_contract(changed_base)

    missing_cli = tmp_path / "missing-cli.Dockerfile"
    missing_cli.write_text(valid.replace("python3 git docker.io", "python3 git"))
    with pytest.raises(image.ImageAuthorityError, match="required_package_missing:docker.io"):
        image._dockerfile_contract(missing_cli)


def test_documented_base_binding_matches_authority_constants() -> None:
    document = (Path(__file__).resolve().parents[2] / "research/farmharness/docs/S8_SUPERVISOR_IMAGE.md").read_text()
    match = re.search(r"^BASE_ID=(sha256:[0-9a-f]{64})$", document, re.MULTILINE)
    assert match is not None
    assert match.group(1) == image.BASE_IMAGE_ID
    assert f"BASE={image.BASE_REFERENCE}" in document
    assert 'docker tag "$BASE_SOURCE" "$BASE"' in document
    assert "684bd6a073312363ef4913356b6e311caad9dbde" not in document
