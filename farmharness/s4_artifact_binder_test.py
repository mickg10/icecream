"""Focused receipt-boundary tests for the non-executing S4 binder."""

from __future__ import annotations

import hashlib
import json
import copy
from pathlib import Path

import pytest
from jsonschema import Draft202012Validator

from farmharness import s4_artifact_binder as binder
from farmharness.s4_version_transition_planner import artifact_binding_contract, audit_plan, build_plan


def _resign(manifest: dict) -> dict:
    value = copy.deepcopy(manifest)
    value.pop("manifest_sha256", None)
    value["manifest_sha256"] = hashlib.sha256(binder._canonical(value)).hexdigest()
    return value


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


def test_receipt_requires_protocol_version_authority_and_id(tmp_path: Path) -> None:
    receipt = _receipt(tmp_path / "p44", 44)
    receipt["protocol_assertion"].pop("version")
    assert binder.bind_receipts([receipt])["versions"]["44"]["status"] == "NOT_BOUND"
    receipt = _receipt(tmp_path / "p44-authority", 44)
    receipt["build"]["authority"] = "not-authority"
    assert binder.bind_receipts([receipt])["versions"]["44"]["status"] == "NOT_BOUND"
    receipt = _receipt(tmp_path / "p44-id", 44)
    receipt["build"].pop("receipt_id")
    assert binder.bind_receipts([receipt])["versions"]["44"]["status"] == "NOT_BOUND"


def test_unknown_receipt_descriptor_is_rejected(tmp_path: Path) -> None:
    receipt = _receipt(tmp_path / "p44", 44)
    receipt["roles"]["S"]["unexpected"] = "ignored?"
    row = binder.bind_receipts([receipt])["versions"]["44"]
    assert row["status"] == "NOT_BOUND"
    assert any("S:descriptor:unknown:unexpected" in error for error in row["errors"])


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


def test_audit_rechecks_root_privacy_executable_and_nlink(tmp_path: Path) -> None:
    root = tmp_path / "p44"
    manifest = binder.bind_receipts([_receipt(root, 44)])
    root.chmod(0o770)
    result = binder.audit_artifact_manifest(_resign(manifest))
    assert any("artifact-root-not-private" in error for error in result["errors"])

    root.chmod(0o700)
    (root / "client/icecc").chmod(0o400)
    result = binder.audit_artifact_manifest(_resign(manifest))
    assert any("C:not-executable" in error for error in result["errors"])

    (root / "client/icecc").chmod(0o500)
    (root / "client/icecc").unlink()
    (root / "client/icecc").hardlink_to(root / "scheduler/icecc-scheduler")
    result = binder.audit_artifact_manifest(_resign(manifest))
    assert any("C:nlink-not-one" in error or "C:aliased-inode" in error
               for error in result["errors"])


def test_audit_rechecks_p43_retained_hashes(monkeypatch: pytest.MonkeyPatch,
                                            tmp_path: Path) -> None:
    receipt = _receipt(tmp_path / "p43", 43)
    generated = {role: descriptor["sha256"] for role, descriptor in receipt["roles"].items()}
    monkeypatch.setitem(binder.RETAINED_ROLE_HASHES, "43", generated)
    manifest = binder.bind_receipts([receipt])
    manifest["versions"]["43"]["roles"]["S"]["sha256"] = "f" * 64
    result = binder.audit_artifact_manifest(_resign(manifest), rehash=False)
    assert result["status"] == "FAIL"
    assert "P43:S:retained-hash-mismatch" in result["errors"]


def test_audit_rechecks_semantic_contracts_after_resign(tmp_path: Path) -> None:
    manifest = binder.bind_receipts([_receipt(tmp_path / "p44", 44)])
    for field, expected in (("planner", "contract-mismatch"),
                            ("matrix", "contract-mismatch")):
        mutant = copy.deepcopy(manifest)
        mutant[field][next(iter(mutant[field]))] = "changed"
        result = binder.audit_artifact_manifest(_resign(mutant), rehash=False)
        assert expected in " ".join(result["errors"])
    mutant = copy.deepcopy(manifest)
    mutant["overall"]["status"] = "READY"
    result = binder.audit_artifact_manifest(_resign(mutant), rehash=False)
    assert "overall:status-mismatch" in result["errors"]
    mutant = copy.deepcopy(manifest)
    mutant["versions"]["44"]["roles"]["C"]["new"] = True
    result = binder.audit_artifact_manifest(_resign(mutant), rehash=False)
    assert any("P44:C:unknown:new" in error for error in result["errors"])


def test_audit_requires_receipt_digest_object_and_bound_rows_error_free(tmp_path: Path) -> None:
    manifest = binder.bind_receipts([_receipt(tmp_path / "p44", 44)])
    mutant = copy.deepcopy(manifest)
    mutant["versions"]["44"]["receipt"] = {"bytes": 10, "sha256": "0" * 64}
    result = binder.audit_artifact_manifest(_resign(mutant), rehash=False)
    assert result["status"] == "FAIL"
    assert "P44:receipt-document-invalid" in result["errors"]
    mutant = copy.deepcopy(manifest)
    mutant["versions"]["44"]["receipt"]["sha256"] = "0" * 64
    result = binder.audit_artifact_manifest(_resign(mutant), rehash=False)
    assert "P44:receipt-digest-mismatch" in result["errors"]
    mutant = copy.deepcopy(manifest)
    mutant["versions"]["44"]["receipt"] = {"bytes": 0, "sha256": "0" * 64}
    result = binder.audit_artifact_manifest(_resign(mutant), rehash=False)
    assert "P44:receipt-bytes-invalid" in result["errors"]
    mutant = copy.deepcopy(manifest)
    mutant["versions"]["44"]["errors"] = ["late-error"]
    result = binder.audit_artifact_manifest(_resign(mutant), rehash=False)
    assert "P44:bound-row-has-errors" in result["errors"]


def test_planner_audit_rejects_binding_contract_mutation() -> None:
    mutant = build_plan()
    mutant["artifact_binding_contract"]["transition_count"] = 728
    assert audit_plan(mutant)["status"] == "FAIL"


def test_manifest_schema_is_strict_and_accepts_emitted_shape(tmp_path: Path) -> None:
    schema = json.loads(Path(binder.__file__).with_name("s4_artifact_manifest.schema.json").read_text())
    manifest = binder.bind_receipts([_receipt(tmp_path / "p44", 44)])
    Draft202012Validator(schema).validate(manifest)
    mutant = copy.deepcopy(manifest)
    mutant["unexpected"] = True
    assert list(Draft202012Validator(schema).iter_errors(mutant))


def test_bind_and_audit_reject_ancestor_symlink(tmp_path: Path) -> None:
    real = tmp_path / "real"
    receipt = _receipt(real, 44)
    alias = tmp_path / "alias"
    alias.symlink_to(real, target_is_directory=True)
    receipt["artifact_root"] = str(alias)
    row = binder.bind_receipts([receipt])["versions"]["44"]
    assert row["status"] == "NOT_BOUND"
    assert any("artifact-root:not-private-directory" in error for error in row["errors"])

    manifest = binder.bind_receipts([_receipt(tmp_path / "audit-root", 44)])
    old_root = Path(manifest["versions"]["44"]["artifact_root"])
    moved = tmp_path / "moved-root"
    old_root.rename(moved)
    old_root.symlink_to(moved, target_is_directory=True)
    result = binder.audit_artifact_manifest(_resign(manifest))
    assert any("P44:artifact-root-invalid" in error for error in result["errors"])

    manifest = binder.bind_receipts([_receipt(tmp_path / "audit-role", 44)])
    role_root = Path(manifest["versions"]["44"]["artifact_root"])
    client_dir = role_root / "client"
    moved_client = tmp_path / "moved-client"
    client_dir.rename(moved_client)
    client_dir.symlink_to(moved_client, target_is_directory=True)
    result = binder.audit_artifact_manifest(_resign(manifest))
    assert any("P44:C:path-alias" in error for error in result["errors"])


def test_symlink_and_role_alias_are_rejected(tmp_path: Path) -> None:
    receipt = _receipt(tmp_path / "p44", 44)
    root = Path(receipt["artifact_root"])
    target = root / "client/icecc"
    target.unlink()
    target.symlink_to(root / "scheduler/icecc-scheduler")
    row = binder.bind_receipts([receipt])["versions"]["44"]
    assert row["status"] == "NOT_BOUND"
    assert any("C:path-alias" in error for error in row["errors"])


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
