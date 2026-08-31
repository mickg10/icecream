"""Focused receipt-boundary tests for the non-executing S4 binder."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

import pytest

from farmharness import s4_artifact_binder as binder
from farmharness.s4_version_transition_planner import artifact_binding_contract, build_plan


def _receipt(root: Path, version: int, *, include_x: bool = False,
             commit: str | None = None, tree: str = "b" * 40,
             authority: str | None = None) -> dict:
    expected_commit = {
        43: binder.P43_SOURCE_SHA,
        44: binder.P44_SOURCE_SHA,
        50: binder.P50_RUNTIME_SOURCE_SHA,
    }[version]
    commit = expected_commit if commit is None else commit
    roles = {"S": "scheduler/icecc-scheduler", "C": "client/icecc",
             "F": "daemon/iceccd", "E": "client/icecc-create-env"}
    if include_x:
        roles["X"] = "cache/icecc-cache-service"
    descriptors = {}
    for role, relative in roles.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(f"P{version}-{role}\n".encode())
        path.chmod(0o500)
        descriptors[role] = {"path": relative, "bytes": path.stat().st_size,
                             "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    return {
        "schema": binder.RECEIPT_SCHEMA, "version": version, "label": f"P{version}",
        "artifact_root": str(root), "source": {"commit": commit, "tree": tree},
        "build": {"source_commit": commit, "source_tree": tree,
                  "authority": authority or ("product-runtime" if version == 50 else "release-build"),
                  "receipt_id": f"fixture-p{version}"},
        "protocol_assertion": {"path": binder.PROTOCOL_PATH,
                               "text": binder.PROTOCOL_ASSERTIONS[version],
                               "version": version},
        "roles": descriptors,
    }


def test_p44_is_not_bound_without_receipt(tmp_path: Path) -> None:
    manifest = binder.bind_receipts([])
    assert manifest["versions"]["44"]["status"] == "NOT_BOUND"
    assert manifest["overall"]["status"] == "NOT_READY"


def test_p44_exact_receipt_binds_without_retained_hash_set(tmp_path: Path) -> None:
    manifest = binder.bind_receipts([_receipt(tmp_path / "p44", 44)])
    row = manifest["versions"]["44"]
    assert row["status"] == "BOUND"
    assert row["role_completeness"]["complete"] is True
    assert manifest["overall"]["status"] == "NOT_READY"  # P43/P50 absent


def test_p50_requires_product_runtime_authority_and_current_source(tmp_path: Path) -> None:
    receipt = _receipt(tmp_path / "p50", 50, include_x=True)
    assert binder.bind_receipts([receipt])["versions"]["50"]["status"] == "BOUND"

    stale = _receipt(tmp_path / "stale", 50, include_x=True,
                     commit=binder.P50_PRODUCT_SOURCE_METADATA_SHA)
    row = binder.bind_receipts([stale])["versions"]["50"]
    assert row["status"] == "NOT_BOUND"
    assert any("planner-source-metadata" in error for error in row["errors"])


def test_missing_p50_x_is_fail_closed(tmp_path: Path) -> None:
    row = binder.bind_receipts([_receipt(tmp_path / "p50", 50)])["versions"]["50"]
    assert row["status"] == "NOT_BOUND"
    assert any("X:missing" in error for error in row["errors"])


def test_changed_file_fails_manifest_audit(tmp_path: Path) -> None:
    root = tmp_path / "p44"
    manifest = binder.bind_receipts([_receipt(root, 44)])
    (root / "client/icecc").chmod(0o600)
    (root / "client/icecc").write_bytes(b"changed\n")
    result = binder.audit_artifact_manifest(manifest)
    assert result["status"] == "FAIL"
    assert any("sha256-changed" in error for error in result["errors"])


def test_symlink_and_role_alias_are_rejected(tmp_path: Path) -> None:
    receipt = _receipt(tmp_path / "p44", 44)
    root = Path(receipt["artifact_root"])
    target = root / "client/icecc"
    target.unlink()
    target.symlink_to(root / "scheduler/icecc-scheduler")
    row = binder.bind_receipts([receipt])["versions"]["44"]
    assert row["status"] == "NOT_BOUND"
    assert any("C:not-private-regular-file" in error for error in row["errors"])


def test_p43_retained_hashes_are_an_explicit_authority(monkeypatch: pytest.MonkeyPatch,
                                                       tmp_path: Path) -> None:
    receipt = _receipt(tmp_path / "p43", 43)
    generated = {role: descriptor["sha256"] for role, descriptor in receipt["roles"].items()}
    monkeypatch.setitem(binder.RETAINED_ROLE_HASHES, "43", generated)
    assert binder.bind_receipts([receipt])["versions"]["43"]["status"] == "BOUND"


def test_manifest_is_canonical_and_write_once(tmp_path: Path) -> None:
    manifest = binder.bind_receipts([_receipt(tmp_path / "p44", 44)])
    output = tmp_path / "manifest.json"
    binder.write_manifest(manifest, output)
    assert output.stat().st_mode & 0o777 == 0o444
    assert json.loads(output.read_text()) == manifest
    with pytest.raises(FileExistsError):
        binder.write_manifest(manifest, output)


def test_receipt_can_join_an_exact_role_manifest(tmp_path: Path) -> None:
    receipt = _receipt(tmp_path / "p44", 44)
    role_manifest = {"root": receipt["artifact_root"], "roles": receipt.pop("roles")}
    manifest_path = tmp_path / "roles.json"
    manifest_path.write_text(json.dumps(role_manifest), encoding="utf-8")
    receipt["artifact_manifest"] = str(manifest_path)
    # Exercise the on-disk path without changing the receipt authority.
    receipt_path = tmp_path / "receipt.json"
    receipt_path.write_text(json.dumps(receipt), encoding="utf-8")
    loaded = binder.load_receipt(receipt_path)
    assert binder.bind_receipts([loaded])["versions"]["44"]["status"] == "BOUND"


def test_planner_embeds_binding_contract() -> None:
    contract = build_plan()["artifact_binding_contract"]
    assert contract == artifact_binding_contract()
    assert contract["roles"]["50"] == ["S", "C", "F", "E", "X"]
    assert contract["p50_runtime_source_commit"] != contract["p50_planner_source_commit"]
