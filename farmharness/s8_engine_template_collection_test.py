from __future__ import annotations

import hashlib
import json
from pathlib import Path

import pytest

import s8_engine_template_collection as collection
from s8_predictive_engine import MANIFEST_SCHEMA, SEMANTICS, TOPOLOGY_SCHEMA


def _canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode() + b"\n"


def _descriptor(path: Path, raw: bytes) -> dict[str, object]:
    return {"path": path.name, "bytes": len(raw),
            "sha256": hashlib.sha256(raw).hexdigest()}


def _package(root: Path, cell: tuple[str, str, str]) -> Path:
    corpus, profile, regime = cell
    root.mkdir(parents=True)
    input_raw = ("/".join(cell) + "\n").encode()
    input_path = root / "input.ii"
    input_path.write_bytes(input_raw)
    topology = {
        "schema": TOPOLOGY_SCHEMA, "semantics": SEMANTICS,
        "cell": {"corpus": corpus, "profile": profile, "regime": regime},
        "topology": {"c_store_guid": "1" * 32, "f_store_guid": "2" * 32,
                     "history_nonce": 1, "c_workers": 1, "f_workers": 1,
                     "cache_channel": {"ZSTD_TU": "direct", "ZSTD_ROUTE": "route",
                                       "P29": "route", "GRZ_RESIDUAL": "residual"}[profile]},
        "state": {"c_cache": regime, "f_cache": regime, "generation": 0},
    }
    topology_raw = _canonical(topology)
    topology_path = root / "topology.json"
    topology_path.write_bytes(topology_raw)
    manifest = {
        "schema": MANIFEST_SCHEMA, "semantics": SEMANTICS,
        "cell": {"corpus": corpus, "profile": profile, "regime": regime},
        "split": "calibration", "predictive_mode": True,
        "input": _descriptor(input_path, input_raw),
        "topology_state": _descriptor(topology_path, topology_raw),
    }
    manifest_raw = _canonical(manifest)
    manifest_path = root / "predictive-manifest.json"
    manifest_path.write_bytes(manifest_raw)
    package = {
        "schema": collection.PACKAGE_SCHEMA, "status": "PASS",
        "cell": "/".join(cell),
        "files": {"predictive_manifest": _descriptor(manifest_path, manifest_raw),
                  "input": _descriptor(input_path, input_raw),
                  "topology": _descriptor(topology_path, topology_raw)},
    }
    package_path = root / "package-manifest.json"
    package_path.write_bytes(_canonical(package))
    return package_path


def _packages(tmp_path: Path) -> list[Path]:
    return [_package(tmp_path / "sources" / f"p{index}", cell)
            for index, cell in enumerate(collection.EXPECTED_CELLS)]


def test_collects_all_16_current_calibration_templates_and_audits(tmp_path: Path) -> None:
    output = tmp_path / "experiments" / "s8-engine-templates-20260831T140000Z"
    output.parent.mkdir()
    manifest = collection.collect(_packages(tmp_path), output)
    result = collection.audit(manifest)
    assert result["status"] == "PASS"
    assert len(list(output.glob("engines/*/*/engine-manifest.json"))) == 16
    assert result["engine_manifest_template"].endswith(
        "engines/{corpus}/{profile}-{regime}/engine-manifest.json")


def test_missing_or_duplicate_cell_is_rejected_before_output(tmp_path: Path) -> None:
    packages = _packages(tmp_path)
    output = tmp_path / "out"
    with pytest.raises(collection.CollectionError, match="cell_set_mismatch"):
        collection.collect(packages[:-1], output)
    assert not output.exists()
    with pytest.raises(collection.CollectionError, match="duplicate_cell"):
        collection.collect(packages + [packages[0]], output)
    assert not output.exists()


def test_changed_source_descriptor_and_output_bytes_fail_closed(tmp_path: Path) -> None:
    packages = _packages(tmp_path)
    source_input = packages[0].parent / "input.ii"
    source_input.write_bytes(b"changed\n")
    with pytest.raises(collection.CollectionError, match="descriptor_mismatch"):
        collection.collect(packages, tmp_path / "first")

    packages = _packages(tmp_path / "fresh")
    output = tmp_path / "second"
    manifest = collection.collect(packages, output)
    target = next(output.glob("engines/*/*/input.ii"))
    target.chmod(0o644)
    target.write_bytes(b"changed\n")
    assert collection.audit(manifest)["status"] == "FAIL"


def test_intermediate_symlink_alias_is_rejected(tmp_path: Path) -> None:
    packages = _packages(tmp_path)
    package = packages[0]
    original = package.parent
    moved = original.with_name(original.name + "-real")
    original.rename(moved)
    original.symlink_to(moved, target_is_directory=True)
    aliased = original / "package-manifest.json"
    with pytest.raises(collection.CollectionError, match="path_alias|not_private"):
        collection.collect([aliased] + packages[1:], tmp_path / "out")


def test_heldout_package_cannot_enter_collection(tmp_path: Path) -> None:
    packages = _packages(tmp_path)
    value = json.loads(packages[0].read_text())
    value["cell"] = "DuckDB/ZSTD_TU/cold"
    packages[0].write_bytes(_canonical(value))
    with pytest.raises(collection.CollectionError, match="cell_not_calibration"):
        collection.collect(packages, tmp_path / "out")
