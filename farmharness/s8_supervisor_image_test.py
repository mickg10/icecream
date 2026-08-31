from __future__ import annotations

import json
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


def test_receipt_binds_definition_image_id_platform_and_raw_inspect(tmp_path: Path) -> None:
    dockerfile = _dockerfile(tmp_path)
    inspect_path = tmp_path / "inspect.json"
    inspect_raw = _inspect()
    inspect_path.write_bytes(inspect_raw)
    receipt = image.make_receipt(dockerfile=dockerfile, image_ref="local/s8-supervisor:1",
                                 inspect_raw=inspect_raw, inspect_path=inspect_path,
                                 source_commit="b" * 40)
    assert receipt["schema"] == image.SCHEMA
    assert receipt["base"]["reference"] == image.BASE_REFERENCE
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
    inspect_path = tmp_path / "inspect.json"
    for field, value in (("Id", "sha256:not-a-content-id"), ("Architecture", "arm64")):
        obj = {"Id": "sha256:" + "a" * 64, "Os": "linux", "Architecture": "amd64"}
        obj[field] = value
        raw = (json.dumps(obj) + "\n").encode()
        inspect_path.write_bytes(raw)
        with pytest.raises(image.ImageAuthorityError):
            image.make_receipt(dockerfile=dockerfile, image_ref="s8-supervisor:local",
                               inspect_raw=raw, inspect_path=inspect_path)


def test_build_command_is_pinned_and_non_running(tmp_path: Path) -> None:
    dockerfile = _dockerfile(tmp_path)
    command = image.build_command(context=tmp_path, dockerfile=dockerfile,
                                  image_ref="local/s8-supervisor:1")
    assert command[:4] == ["docker", "build", "--pull=false", "--platform=linux/amd64"]
    assert "--file" in command and "--tag" in command
    assert "run" not in command


def test_dockerfile_contract_rejects_unpinned_base_or_missing_tool(tmp_path: Path) -> None:
    path = tmp_path / "Dockerfile"
    path.write_text("FROM ubuntu:22.04\nRUN apt-get install python3 git\n")
    with pytest.raises(image.ImageAuthorityError, match="base_reference_mismatch"):
        image._dockerfile_contract(path)
