#!/usr/bin/env python3
"""Bind immutable S4 role artifacts from build receipts.

The S4 planner is intentionally non-executing.  This module is the equally
non-executing boundary between an exact build receipt and a future 27-state /
729-transition runner.  It never configures, compiles, starts, or stages a
process.  A receipt is accepted only when the files named by it are still
private regular files with the recorded byte counts and SHA-256 digests.

The receipt format is deliberately explicit::

    {
      "schema": "icecream-s4-build-receipt-v1",
      "version": 43,
      "label": "P43",
      "artifact_root": "/private/p43",
      "source": {"commit": "<40 hex>", "tree": "<40 hex>"},
      "build": {"source_commit": "<40 hex>",
                "source_tree": "<40 hex>",
                "authority": "release-build"},
      "protocol_assertion": {"path": "services/comm.h",
                              "text": "#define PROTOCOL_VERSION 43"},
      "roles": {"S": {"path": "scheduler/icecc-scheduler",
                         "bytes": 123, "sha256": "<64 hex>"}, ...}
    }

P50 additionally requires ``build.authority`` to be ``product-runtime`` and
the final product source commit used by the real-cell runner.  The planner's
source metadata is recorded separately and is never accepted as runtime
authority.  P44 has no retained digest set; a complete, exact receipt is
therefore required before it can become BOUND.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import sys
from pathlib import Path
from typing import Any, Mapping, Sequence

try:
    from . import s8_depth_runner
except ImportError:  # pragma: no cover
    import s8_depth_runner

try:
    from .s4_version_transition_planner import (
        P43_SOURCE_SHA,
        P44_PROTOCOL_ASSERTION,
        P44_SOURCE_SHA,
        P50_SOURCE_SHA as P50_PLANNER_SOURCE_SHA,
        RETAINED_ROLE_HASHES,
    )
except ImportError:  # pragma: no cover
    from s4_version_transition_planner import (
        P43_SOURCE_SHA,
        P44_PROTOCOL_ASSERTION,
        P44_SOURCE_SHA,
        P50_SOURCE_SHA as P50_PLANNER_SOURCE_SHA,
        RETAINED_ROLE_HASHES,
    )


RECEIPT_SCHEMA = "icecream-s4-build-receipt-v1"
MANIFEST_SCHEMA = "icecream-s4-artifact-manifest-v1"
AUDIT_SCHEMA = "icecream-s4-artifact-audit-v1"
ROLES = ("S", "C", "F", "E", "X")
ROLE_NAMES = {"S": "scheduler", "C": "client", "F": "daemon",
              "E": "create-env", "X": "cache-service"}
REQUIRED_ROLES = {
    43: ("S", "C", "F", "E"),
    44: ("S", "C", "F", "E"),
    50: ("S", "C", "F", "E", "X"),
}
SOURCE_AUTHORITIES = {43: P43_SOURCE_SHA, 44: P44_SOURCE_SHA}
# P50_SOURCE_SHA in the planner is metadata for the planner commit.  The
# product runner's retained build contract is the independent runtime source
# authority below.  Keep both names visible to prevent accidental conflation.
P50_RUNTIME_SOURCE_SHA = "04006b9d94161a047154121f47785aec747ffd87"
P50_RUNTIME_AUTHORITY = "product-runtime"
P50_PRODUCT_SOURCE_METADATA_SHA = P50_PLANNER_SOURCE_SHA
PROTOCOL_ASSERTIONS = {
    43: "#define PROTOCOL_VERSION 43",
    44: P44_PROTOCOL_ASSERTION,
    50: "#define PROTOCOL_VERSION 50",
}
PROTOCOL_PATH = "services/comm.h"
HEX40 = re.compile(r"^[0-9a-f]{40}$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")
RECEIPT_FIELDS = frozenset({"schema", "version", "label", "artifact_root",
                            "source", "build", "protocol_assertion", "roles",
                            "artifact_manifest", "manifest"})
SOURCE_FIELDS = frozenset({"commit", "tree", "repository"})
BUILD_FIELDS = frozenset({"source_commit", "source_tree", "authority",
                          "receipt_id", "command"})
PROTOCOL_FIELDS = frozenset({"path", "text", "version"})
ROLE_DESCRIPTOR_FIELDS = frozenset({"path", "bytes", "sha256"})
MANIFEST_FIELDS = frozenset({"schema", "manifest_version", "planner", "matrix",
                             "versions", "overall", "manifest_sha256"})
PLANNER_FIELDS = frozenset({"source_metadata_commit", "runtime_source_commit",
                            "runtime_authority"})
MATRIX_FIELDS = frozenset({"state_count", "transition_count", "state_space"})
OVERALL_FIELDS = frozenset({"status", "required_versions"})
BOUND_ROLE_FIELDS = frozenset({"role", "name", "path", "bytes", "sha256",
                               "executable"})
VERSION_FIELDS = frozenset({"status", "version", "label", "artifact_root", "source",
                            "build", "receipt", "protocol_assertion",
                            "role_completeness", "roles", "errors"})
COMPLETENESS_FIELDS = frozenset({"required", "present", "complete"})
RECEIPT_DIGEST_FIELDS = frozenset({"bytes", "sha256", "document"})


class ArtifactBindingError(ValueError):
    """Raised for malformed receipts or an invalid immutable manifest."""


def _canonical(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _valid_hex(value: object, pattern: re.Pattern[str]) -> bool:
    return isinstance(value, str) and bool(pattern.fullmatch(value))


def _required(mapping: Mapping[str, Any], name: str, errors: list[str]) -> object:
    if name not in mapping:
        errors.append(f"missing:{name}")
        return None
    return mapping[name]


def _reject_unknown(mapping: Mapping[str, Any], allowed: frozenset[str],
                    label: str, errors: list[str]) -> None:
    errors.extend(f"{label}:unknown:{key}" for key in sorted(set(mapping) - allowed))


def _first_ancestor_symlink(path: Path) -> Path | None:
    """Return the first symlink in the lexical path, including ancestors."""
    absolute = Path(os.path.abspath(str(path)))
    current = Path(absolute.anchor)
    for component in absolute.parts[1:]:
        current /= component
        try:
            if current.is_symlink():
                return current
        except OSError:
            return None
    return None


def _root_from(receipt: Mapping[str, Any], errors: list[str]) -> Path | None:
    raw = receipt.get("artifact_root")
    if raw is None:
        raw = receipt.get("root")
    if not isinstance(raw, str) or not raw:
        errors.append("missing:artifact_root")
        return None
    root = Path(raw)
    try:
        # Do not resolve a symlink away before checking it.  The root itself
        # is part of the provenance boundary and must be a real directory.
        if (_first_ancestor_symlink(root) is not None or
                root.is_symlink() or not root.is_dir()):
            errors.append("artifact-root:not-private-directory")
            return None
        root_info = root.lstat()
        if root_info.st_mode & 0o022:
            errors.append("artifact-root:not-private")
        return Path(os.path.normpath(str(root.absolute())))
    except OSError as exc:
        errors.append(f"artifact-root:unreadable:{exc}")
        return None


def _role_path(root: Path, descriptor: Mapping[str, Any], role: str,
               errors: list[str]) -> Path | None:
    raw = descriptor.get("path")
    if not isinstance(raw, str) or not raw:
        errors.append(f"{role}:missing:path")
        return None
    candidate = Path(raw)
    if not candidate.is_absolute():
        candidate = root / candidate
    # lexical containment catches ../ aliases; resolving is only used for the
    # comparison after the lexical check and never to bless a symlink.
    try:
        lexical = Path(os.path.normpath(str(candidate.absolute())))
        lexical.relative_to(root)
        if _first_ancestor_symlink(lexical) is not None:
            errors.append(f"{role}:path-alias")
            return None
        lexical.resolve(strict=False).relative_to(root.resolve())
    except ValueError:
        errors.append(f"{role}:path-outside-root")
        return None
    except OSError as exc:
        errors.append(f"{role}:path-unreadable:{exc}")
        return None
    return lexical


def _protocol(receipt: Mapping[str, Any], version: int,
              errors: list[str]) -> dict[str, Any] | None:
    value = receipt.get("protocol_assertion")
    if not isinstance(value, Mapping):
        errors.append("protocol-assertion:missing")
        return None
    _reject_unknown(value, PROTOCOL_FIELDS, "protocol-assertion", errors)
    path = value.get("path")
    text = value.get("text")
    expected = PROTOCOL_ASSERTIONS[version]
    if path != PROTOCOL_PATH:
        errors.append("protocol-assertion:path-mismatch")
    if text != expected:
        errors.append(f"protocol-assertion:expected-{version}")
    raw_version = value.get("version")
    if raw_version != version:
        errors.append(f"protocol-assertion:version-mismatch:{raw_version}")
    return {"path": path, "text": text, "version": raw_version}


def _version(receipt: Mapping[str, Any], errors: list[str]) -> int | None:
    raw = receipt.get("version")
    label = receipt.get("label")
    if raw is None and isinstance(label, str) and label.startswith("P"):
        raw = label[1:]
    if type(raw) is not int or raw not in REQUIRED_ROLES:
        errors.append("version:unknown")
        return None
    if label != f"P{raw}":
        errors.append("version:label-mismatch")
    return raw


def _source_and_build(receipt: Mapping[str, Any], version: int,
                      errors: list[str]) -> tuple[dict[str, Any], dict[str, Any]]:
    source = receipt.get("source")
    build = receipt.get("build")
    if not isinstance(source, Mapping):
        errors.append("source:missing")
        source = {}
    if not isinstance(build, Mapping):
        errors.append("build:missing")
        build = {}
    _reject_unknown(source, SOURCE_FIELDS, "source", errors)
    _reject_unknown(build, BUILD_FIELDS, "build", errors)
    commit = source.get("commit")
    tree = source.get("tree")
    for name, value in (("commit", commit), ("tree", tree)):
        if not _valid_hex(value, HEX40):
            errors.append(f"source:{name}:invalid")
    build_commit = build.get("source_commit")
    build_tree = build.get("source_tree")
    for name, value in (("source_commit", build_commit), ("source_tree", build_tree)):
        if not _valid_hex(value, HEX40):
            errors.append(f"build:{name}:invalid")
    if commit != build_commit:
        errors.append("source-build:commit-mismatch")
    if tree != build_tree:
        errors.append("source-build:tree-mismatch")
    expected = SOURCE_AUTHORITIES.get(version)
    if version == 50:
        expected = P50_RUNTIME_SOURCE_SHA
        if build.get("authority") != P50_RUNTIME_AUTHORITY:
            errors.append("P50:runtime-authority-missing-or-wrong")
        if commit == P50_PLANNER_SOURCE_SHA:
            errors.append("P50:planner-source-metadata-is-not-runtime-authority")
    if commit != expected:
        errors.append(f"P{version}:source-commit-mismatch")
    expected_authority = P50_RUNTIME_AUTHORITY if version == 50 else "release-build"
    if build.get("authority") != expected_authority:
        errors.append(f"P{version}:build-authority-mismatch")
    if not isinstance(build.get("receipt_id"), str) or not build["receipt_id"].strip():
        errors.append(f"P{version}:build-receipt-id-missing")
    return (
        {"commit": commit, "tree": tree, **({"repository": source["repository"]}
                                               if isinstance(source.get("repository"), str)
                                               else {})},
        {key: build[key] for key in sorted(build) if key not in {"artifact_root"}},
    )


def _check_source_repository(source: Mapping[str, Any], errors: list[str]) -> None:
    """Check an optional repository identity when the receipt names one.

    Receipts from offline build stores need not retain a checkout.  If one is
    supplied, however, it is evidence and must agree with the receipt.  Git is
    used read-only; no build or working-tree operation is performed.
    """
    raw = source.get("repository")
    if raw is None:
        return
    repository = Path(str(raw))
    try:
        import subprocess
        commit = subprocess.run(["git", "-C", str(repository), "rev-parse", "HEAD"],
                                capture_output=True, text=True, timeout=5,
                                check=False)
        tree = subprocess.run(["git", "-C", str(repository), "rev-parse", "HEAD^{tree}"],
                              capture_output=True, text=True, timeout=5,
                              check=False)
    except (OSError, subprocess.TimeoutExpired) as exc:
        errors.append(f"source-repository:unavailable:{exc}")
        return
    if commit.returncode or tree.returncode:
        errors.append("source-repository:identity-unavailable")
        return
    if commit.stdout.strip() != source.get("commit"):
        errors.append("source-repository:commit-mismatch")
    if tree.stdout.strip() != source.get("tree"):
        errors.append("source-repository:tree-mismatch")


def _roles(receipt: Mapping[str, Any], root: Path, version: int,
           errors: list[str]) -> dict[str, Any]:
    supplied = receipt.get("roles")
    if not isinstance(supplied, Mapping):
        errors.append("roles:missing-or-not-object")
        supplied = {}
    unknown = sorted(set(supplied) - set(REQUIRED_ROLES[version]))
    errors.extend(f"roles:unexpected:{role}" for role in unknown)
    result: dict[str, Any] = {}
    seen_paths: dict[str, str] = {}
    seen_inodes: dict[tuple[int, int], str] = {}
    for role in REQUIRED_ROLES[version]:
        descriptor = supplied.get(role)
        if not isinstance(descriptor, Mapping):
            errors.append(f"{role}:missing-or-not-object")
            result[role] = {"role": role, "status": "NOT_BOUND"}
            continue
        _reject_unknown(descriptor, ROLE_DESCRIPTOR_FIELDS, f"{role}:descriptor", errors)
        path = _role_path(root, descriptor, role, errors)
        item: dict[str, Any] = {
            "role": role, "name": ROLE_NAMES[role],
            "path": str(path) if path is not None else descriptor.get("path"),
            "bytes": descriptor.get("bytes"),
            "sha256": descriptor.get("sha256"),
        }
        supplied_bytes = descriptor.get("bytes")
        supplied_hash = descriptor.get("sha256")
        if type(supplied_bytes) is not int or supplied_bytes < 0:
            errors.append(f"{role}:bytes-invalid")
        if not _valid_hex(supplied_hash, HEX64):
            errors.append(f"{role}:sha256-invalid")
        if path is None:
            result[role] = item
            continue
        try:
            info = path.lstat()
            if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
                errors.append(f"{role}:not-private-regular-file")
                result[role] = item
                continue
            # Build artifacts must not be mutable by group/other.  A hardlink
            # is also an alias, even if its bytes currently match.
            if info.st_mode & 0o022:
                errors.append(f"{role}:not-private")
            if info.st_nlink != 1:
                errors.append(f"{role}:nlink-not-one")
            inode = (info.st_dev, info.st_ino)
            if inode in seen_inodes:
                errors.append(f"{role}:aliased-inode:{seen_inodes[inode]}")
            seen_inodes[inode] = role
            resolved = str(path)
            if resolved in seen_paths:
                errors.append(f"{role}:aliased-path:{seen_paths[resolved]}")
            seen_paths[resolved] = role
            actual_bytes = info.st_size
            actual_hash = _sha256(path)
            item.update({"bytes": actual_bytes, "sha256": actual_hash,
                         "executable": bool(info.st_mode & 0o111)})
            if supplied_bytes != actual_bytes:
                errors.append(f"{role}:bytes-mismatch")
            if supplied_hash != actual_hash:
                errors.append(f"{role}:sha256-mismatch")
            if not (info.st_mode & 0o111):
                errors.append(f"{role}:not-executable")
        except (OSError, ValueError) as exc:
            errors.append(f"{role}:missing-or-deleted:{exc}")
        result[role] = item
    return result


def _retained_check(version: int, roles: Mapping[str, Any], errors: list[str]) -> None:
    if version != 43:
        return
    expected = RETAINED_ROLE_HASHES["43"]
    for role, digest in expected.items():
        observed = roles.get(role, {}).get("sha256") if isinstance(roles.get(role), Mapping) else None
        if observed != digest:
            errors.append(f"P43:{role}:retained-hash-mismatch")


def _bind_one(receipt: Mapping[str, Any]) -> dict[str, Any]:
    errors: list[str] = []
    _reject_unknown(receipt, RECEIPT_FIELDS, "receipt", errors)
    for field in ("schema", "version", "label", "artifact_root", "source",
                  "build", "protocol_assertion", "roles"):
        if field not in receipt:
            errors.append(f"receipt:missing:{field}")
    if receipt.get("schema") != RECEIPT_SCHEMA:
        errors.append("schema-mismatch")
    version = _version(receipt, errors)
    if version is None:
        return {"status": "NOT_BOUND", "errors": errors}
    root = _root_from(receipt, errors)
    source, build = _source_and_build(receipt, version, errors)
    _check_source_repository(source, errors)
    protocol = _protocol(receipt, version, errors)
    roles = _roles(receipt, root, version, errors) if root is not None else {}
    _retained_check(version, roles, errors)
    required = list(REQUIRED_ROLES[version])
    completeness = {
        "required": required,
        "present": [role for role in required if role in roles and
                    roles[role].get("sha256")],
        "complete": not any(role not in roles or not roles[role].get("sha256")
                             for role in required),
    }
    status = "BOUND" if not errors else "NOT_BOUND"
    receipt_raw = _canonical(receipt)
    return {
        "status": status,
        "version": version,
        "label": f"P{version}",
        "artifact_root": str(root) if root is not None else receipt.get("artifact_root"),
        "source": source,
        "build": build,
        "receipt": {"bytes": len(receipt_raw),
                    "sha256": hashlib.sha256(receipt_raw).hexdigest(),
                    "document": receipt},
        "protocol_assertion": protocol,
        "role_completeness": completeness,
        "roles": roles,
        "errors": errors,
    }


def _flatten_receipts(receipts: object) -> dict[int, Mapping[str, Any]]:
    if isinstance(receipts, Mapping):
        if "versions" in receipts:
            receipts = receipts["versions"]
        elif all(str(key) in {"43", "44", "50"} for key in receipts):
            receipts = list(receipts.values())
        else:
            receipts = [receipts]
    if not isinstance(receipts, Sequence) or isinstance(receipts, (str, bytes, bytearray)):
        raise ArtifactBindingError("receipts must be an object or sequence")
    result: dict[int, Mapping[str, Any]] = {}
    for value in receipts:
        if not isinstance(value, Mapping):
            raise ArtifactBindingError("receipt is not an object")
        raw = value.get("version")
        if raw is None and isinstance(value.get("label"), str):
            label = value["label"]
            raw = int(label[1:]) if label[1:].isdigit() else None
        if type(raw) is not int or raw not in REQUIRED_ROLES:
            raise ArtifactBindingError("receipt has unknown version")
        if raw in result:
            raise ArtifactBindingError(f"duplicate receipt for P{raw}")
        result[raw] = value
    return result


def bind_receipts(receipts: object) -> dict[str, Any]:
    """Bind P43/P44/P50 receipts into one deterministic immutable manifest."""
    supplied = _flatten_receipts(receipts)
    versions: dict[str, Any] = {}
    for version in (43, 44, 50):
        if version in supplied:
            versions[str(version)] = _bind_one(supplied[version])
        else:
            versions[str(version)] = {
                "status": "NOT_BOUND", "version": version, "label": f"P{version}",
                "artifact_root": None, "source": None, "build": None,
                "receipt": None, "protocol_assertion": None,
                "role_completeness": {"required": list(REQUIRED_ROLES[version]),
                                       "present": [], "complete": False},
                "roles": {role: {"role": role, "status": "NOT_BOUND"}
                          for role in REQUIRED_ROLES[version]},
                "errors": [f"missing-receipt:P{version}"],
            }
    payload: dict[str, Any] = {
        "schema": MANIFEST_SCHEMA,
        "manifest_version": 1,
        "planner": {
            "source_metadata_commit": P50_PRODUCT_SOURCE_METADATA_SHA,
            "runtime_source_commit": P50_RUNTIME_SOURCE_SHA,
            "runtime_authority": P50_RUNTIME_AUTHORITY,
        },
        "matrix": {"state_count": 27, "transition_count": 729,
                   "state_space": "{43,44,50}^3"},
        "versions": versions,
        "overall": {"status": "READY" if all(
            row.get("status") == "BOUND" for row in versions.values()) else "NOT_READY",
                    "required_versions": [43, 44, 50]},
    }
    payload["manifest_sha256"] = hashlib.sha256(_canonical(payload)).hexdigest()
    return payload


def audit_artifact_manifest(manifest: Mapping[str, Any], *, rehash: bool = True) -> dict[str, Any]:
    """Audit a serialized manifest, including current files when requested."""
    errors: list[str] = []
    if not isinstance(manifest, Mapping):
        return {"schema": AUDIT_SCHEMA, "status": "FAIL",
                "errors": ["manifest:not-object"]}
    _reject_unknown(manifest, MANIFEST_FIELDS, "manifest", errors)
    for field in MANIFEST_FIELDS:
        if field not in manifest:
            errors.append(f"manifest:missing:{field}")
    if manifest.get("schema") != MANIFEST_SCHEMA:
        errors.append("schema-mismatch")
    supplied_digest = manifest.get("manifest_sha256")
    unsigned = dict(manifest)
    unsigned.pop("manifest_sha256", None)
    if not _valid_hex(supplied_digest, HEX64) or hashlib.sha256(_canonical(unsigned)).hexdigest() != supplied_digest:
        errors.append("manifest-sha256-mismatch")
    planner = manifest.get("planner")
    expected_planner = {
        "source_metadata_commit": P50_PRODUCT_SOURCE_METADATA_SHA,
        "runtime_source_commit": P50_RUNTIME_SOURCE_SHA,
        "runtime_authority": P50_RUNTIME_AUTHORITY,
    }
    if not isinstance(planner, Mapping):
        errors.append("planner:missing-or-not-object")
    else:
        _reject_unknown(planner, PLANNER_FIELDS, "planner", errors)
        if dict(planner) != expected_planner:
            errors.append("planner:contract-mismatch")
    matrix = manifest.get("matrix")
    expected_matrix = {"state_count": 27, "transition_count": 729,
                       "state_space": "{43,44,50}^3"}
    if not isinstance(matrix, Mapping):
        errors.append("matrix:missing-or-not-object")
    else:
        _reject_unknown(matrix, MATRIX_FIELDS, "matrix", errors)
        if dict(matrix) != expected_matrix:
            errors.append("matrix:contract-mismatch")
    overall = manifest.get("overall")
    expected_versions = [43, 44, 50]
    if not isinstance(overall, Mapping):
        errors.append("overall:missing-or-not-object")
    else:
        _reject_unknown(overall, OVERALL_FIELDS, "overall", errors)
        if overall.get("required_versions") != expected_versions:
            errors.append("overall:versions-mismatch")
    versions = manifest.get("versions")
    if not isinstance(versions, Mapping):
        errors.append("versions:missing-or-not-object")
        versions = {}
    if set(versions) != {"43", "44", "50"}:
        errors.append("versions:key-set-mismatch")
    for version in (43, 44, 50):
        row = versions.get(str(version))
        if not isinstance(row, Mapping):
            errors.append(f"missing:P{version}")
            continue
        _reject_unknown(row, VERSION_FIELDS, f"P{version}:row", errors)
        for field in VERSION_FIELDS:
            if field not in row:
                errors.append(f"P{version}:row:missing:{field}")
        row_errors = row.get("errors")
        if not isinstance(row_errors, list) or any(not isinstance(item, str) for item in row_errors):
            errors.append(f"P{version}:row:errors-invalid")
        if row.get("status") != "BOUND":
            errors.append(f"P{version}:not-bound")
            continue
        if row_errors:
            errors.append(f"P{version}:bound-row-has-errors")
        if row.get("version") != version or row.get("label") != f"P{version}":
            errors.append(f"P{version}:identity-mismatch")
        source = row.get("source")
        build = row.get("build")
        protocol = row.get("protocol_assertion")
        receipt_digest = row.get("receipt")
        if not isinstance(receipt_digest, Mapping):
            errors.append(f"P{version}:receipt-digest-missing")
        else:
            _reject_unknown(receipt_digest, RECEIPT_DIGEST_FIELDS,
                            f"P{version}:receipt", errors)
            if (type(receipt_digest.get("bytes")) is not int or
                    receipt_digest.get("bytes", -1) < 1):
                errors.append(f"P{version}:receipt-bytes-invalid")
            if not _valid_hex(receipt_digest.get("sha256"), HEX64):
                errors.append(f"P{version}:receipt-sha256-invalid")
            document = receipt_digest.get("document")
            if not isinstance(document, Mapping):
                errors.append(f"P{version}:receipt-document-invalid")
            else:
                _reject_unknown(document, RECEIPT_FIELDS, f"P{version}:receipt-document", errors)
                document_source_for_shape = document.get("source")
                if isinstance(document_source_for_shape, Mapping):
                    _reject_unknown(document_source_for_shape, SOURCE_FIELDS,
                                    f"P{version}:receipt-document:source", errors)
                document_build_for_shape = document.get("build")
                if isinstance(document_build_for_shape, Mapping):
                    _reject_unknown(document_build_for_shape, BUILD_FIELDS,
                                    f"P{version}:receipt-document:build", errors)
                document_protocol_for_shape = document.get("protocol_assertion")
                if isinstance(document_protocol_for_shape, Mapping):
                    _reject_unknown(document_protocol_for_shape, PROTOCOL_FIELDS,
                                    f"P{version}:receipt-document:protocol", errors)
                document_roles_for_shape = document.get("roles")
                if isinstance(document_roles_for_shape, Mapping):
                    for role, descriptor in document_roles_for_shape.items():
                        if isinstance(descriptor, Mapping):
                            _reject_unknown(descriptor, ROLE_DESCRIPTOR_FIELDS,
                                            f"P{version}:receipt-document:{role}", errors)
                document_raw = _canonical(document)
                if (receipt_digest.get("bytes") != len(document_raw) or
                        receipt_digest.get("sha256") != hashlib.sha256(document_raw).hexdigest()):
                    errors.append(f"P{version}:receipt-digest-mismatch")
                if (document.get("version") != version or document.get("label") != f"P{version}" or
                        document.get("schema") != RECEIPT_SCHEMA):
                    errors.append(f"P{version}:receipt-document-identity-mismatch")
                document_root = document.get("artifact_root")
                normalized_document_root = (str(Path(os.path.normpath(
                    str(Path(document_root).absolute()))))
                    if isinstance(document_root, str) else None)
                if normalized_document_root != row.get("artifact_root"):
                    errors.append(f"P{version}:receipt-document-root-mismatch")
                document_protocol = document.get("protocol_assertion")
                if document_protocol != protocol:
                    errors.append(f"P{version}:receipt-document-protocol-mismatch")
                document_source = document.get("source")
                if isinstance(document_source, Mapping) and isinstance(source, Mapping):
                    if (document_source.get("commit") != source.get("commit") or
                            document_source.get("tree") != source.get("tree")):
                        errors.append(f"P{version}:receipt-document-source-mismatch")
                document_build = document.get("build")
                if isinstance(document_build, Mapping) and isinstance(build, Mapping):
                    if (document_build.get("source_commit") != build.get("source_commit") or
                            document_build.get("source_tree") != build.get("source_tree") or
                            document_build.get("authority") != build.get("authority") or
                            document_build.get("receipt_id") != build.get("receipt_id")):
                        errors.append(f"P{version}:receipt-document-build-mismatch")
                document_roles = document.get("roles")
                row_roles = row.get("roles")
                if not isinstance(document_roles, Mapping) or not isinstance(row_roles, Mapping):
                    errors.append(f"P{version}:receipt-document-roles-missing")
                else:
                    if set(document_roles) != set(REQUIRED_ROLES[version]):
                        errors.append(f"P{version}:receipt-document-role-key-set-mismatch")
                    for role in REQUIRED_ROLES[version]:
                        document_role = document_roles.get(role)
                        row_role = row_roles.get(role)
                        if (not isinstance(document_role, Mapping) or
                                not isinstance(row_role, Mapping) or
                                document_role.get("bytes") != row_role.get("bytes") or
                                document_role.get("sha256") != row_role.get("sha256")):
                            errors.append(f"P{version}:receipt-document-role-mismatch:{role}")
        if isinstance(source, Mapping):
            _reject_unknown(source, SOURCE_FIELDS, f"P{version}:source", errors)
        if isinstance(build, Mapping):
            _reject_unknown(build, BUILD_FIELDS, f"P{version}:build", errors)
        expected_commit = (P50_RUNTIME_SOURCE_SHA if version == 50
                           else SOURCE_AUTHORITIES[version])
        if (not isinstance(source, Mapping) or source.get("commit") != expected_commit or
                not _valid_hex(source.get("tree"), HEX40)):
            errors.append(f"P{version}:source-authority-mismatch")
        if (not isinstance(build, Mapping) or
                build.get("source_commit") != (source.get("commit") if isinstance(source, Mapping) else None) or
                build.get("source_tree") != (source.get("tree") if isinstance(source, Mapping) else None)):
            errors.append(f"P{version}:build-source-mismatch")
        expected_authority = (P50_RUNTIME_AUTHORITY if version == 50 else "release-build")
        if (not isinstance(build, Mapping) or
                build.get("authority") != expected_authority):
            errors.append(f"P{version}:build-authority-mismatch")
        if (not isinstance(build, Mapping) or
                not isinstance(build.get("receipt_id"), str) or
                not build["receipt_id"].strip()):
            errors.append(f"P{version}:build-receipt-id-missing")
        if isinstance(protocol, Mapping):
            _reject_unknown(protocol, PROTOCOL_FIELDS,
                            f"P{version}:protocol-assertion", errors)
        if (not isinstance(protocol, Mapping) or protocol.get("path") != PROTOCOL_PATH or
                protocol.get("text") != PROTOCOL_ASSERTIONS[version] or
                protocol.get("version") != version):
            errors.append(f"P{version}:protocol-assertion-mismatch")
        elif set(protocol) != PROTOCOL_FIELDS:
            errors.append(f"P{version}:protocol-assertion-fields")
        completeness = row.get("role_completeness")
        if (not isinstance(completeness, Mapping) or
                completeness.get("required") != list(REQUIRED_ROLES[version]) or
                completeness.get("complete") is not True):
            errors.append(f"P{version}:role-completeness-mismatch")
        if isinstance(completeness, Mapping):
            _reject_unknown(completeness, COMPLETENESS_FIELDS,
                            f"P{version}:role-completeness", errors)
            if completeness.get("present") != list(REQUIRED_ROLES[version]):
                errors.append(f"P{version}:role-presence-mismatch")
        roles_for_retained = row.get("roles")
        if version == 43 and isinstance(roles_for_retained, Mapping):
            for role, expected_hash in RETAINED_ROLE_HASHES["43"].items():
                role_value = roles_for_retained.get(role)
                observed_hash = (role_value.get("sha256")
                                 if isinstance(role_value, Mapping) else None)
                if observed_hash != expected_hash:
                    errors.append(f"P43:{role}:retained-hash-mismatch")
        semantic_roles = row.get("roles")
        if not isinstance(semantic_roles, Mapping):
            errors.append(f"P{version}:roles-missing")
        else:
            if set(semantic_roles) != set(REQUIRED_ROLES[version]):
                errors.append(f"P{version}:role-key-set-mismatch")
            for role in REQUIRED_ROLES[version]:
                item = semantic_roles.get(role)
                if not isinstance(item, Mapping):
                    errors.append(f"P{version}:{role}:missing")
                    continue
                _reject_unknown(item, BOUND_ROLE_FIELDS, f"P{version}:{role}", errors)
                if (item.get("role") != role or item.get("name") != ROLE_NAMES[role] or
                        item.get("executable") is not True):
                    errors.append(f"P{version}:{role}:descriptor-identity-mismatch")
                if (type(item.get("bytes")) is not int or item.get("bytes", -1) < 0 or
                        not _valid_hex(item.get("sha256"), HEX64)):
                    errors.append(f"P{version}:{role}:descriptor-digest-invalid")
        semantic_root_raw = row.get("artifact_root")
        semantic_root = (Path(semantic_root_raw)
                          if isinstance(semantic_root_raw, str) else None)
        if (semantic_root is None or not semantic_root.is_absolute() or
                _first_ancestor_symlink(semantic_root) is not None or
                semantic_root.is_symlink() or not semantic_root.is_dir()):
            errors.append(f"P{version}:artifact-root-invalid")
        else:
            try:
                if semantic_root.lstat().st_mode & 0o022:
                    errors.append(f"P{version}:artifact-root-not-private")
            except OSError:
                errors.append(f"P{version}:artifact-root-unreadable")
        if rehash:
            root_raw = row.get("artifact_root")
            root = Path(str(root_raw)) if isinstance(root_raw, str) else None
            if (root is None or not root.is_absolute() or
                    _first_ancestor_symlink(root) is not None or
                    root.is_symlink() or not root.is_dir()):
                errors.append(f"P{version}:artifact-root-invalid")
                continue
            try:
                if root.lstat().st_mode & 0o022:
                    errors.append(f"P{version}:artifact-root-not-private")
            except OSError:
                errors.append(f"P{version}:artifact-root-unreadable")
            roles = row.get("roles")
            if not isinstance(roles, Mapping):
                errors.append(f"P{version}:roles-missing")
                continue
            if set(roles) != set(REQUIRED_ROLES[version]):
                errors.append(f"P{version}:role-key-set-mismatch")
            seen_inodes: set[tuple[int, int]] = set()
            seen_paths: set[Path] = set()
            for role in REQUIRED_ROLES[version]:
                item = roles.get(role)
                if not isinstance(item, Mapping):
                    errors.append(f"P{version}:{role}:missing")
                    continue
                _reject_unknown(item, BOUND_ROLE_FIELDS, f"P{version}:{role}", errors)
                if (item.get("role") != role or item.get("name") != ROLE_NAMES[role] or
                        item.get("executable") is not True):
                    errors.append(f"P{version}:{role}:descriptor-identity-mismatch")
                if (type(item.get("bytes")) is not int or item.get("bytes", -1) < 0 or
                        not _valid_hex(item.get("sha256"), HEX64)):
                    errors.append(f"P{version}:{role}:descriptor-digest-invalid")
                path = Path(str(item.get("path", "")))
                try:
                    resolved_root = root.resolve()
                    if not path.is_absolute():
                        raise ValueError("relative role path")
                    lexical = Path(os.path.normpath(str(path)))
                    lexical.relative_to(root)
                    if _first_ancestor_symlink(lexical) is not None:
                        raise ValueError("path alias")
                    resolved_path = path.resolve(strict=False)
                    resolved_path.relative_to(resolved_root)
                    if lexical in seen_paths:
                        errors.append(f"P{version}:{role}:aliased-path")
                    seen_paths.add(lexical)
                    info = path.lstat()
                    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
                        errors.append(f"P{version}:{role}:not-private-regular-file")
                        continue
                    if info.st_mode & 0o022:
                        errors.append(f"P{version}:{role}:not-private")
                    inode = (info.st_dev, info.st_ino)
                    if inode in seen_inodes:
                        errors.append(f"P{version}:{role}:aliased-inode")
                    seen_inodes.add(inode)
                    if info.st_nlink != 1:
                        errors.append(f"P{version}:{role}:nlink-not-one")
                    if not (info.st_mode & 0o111):
                        errors.append(f"P{version}:{role}:not-executable")
                    if info.st_size != item.get("bytes"):
                        errors.append(f"P{version}:{role}:bytes-changed")
                    if _sha256(path) != item.get("sha256"):
                        errors.append(f"P{version}:{role}:sha256-changed")
                except (ValueError, OSError) as exc:
                    if str(exc) == "path alias":
                        errors.append(f"P{version}:{role}:path-alias")
                    errors.append(f"P{version}:{role}:missing-or-deleted")
    expected_overall = ("READY" if all(
        isinstance(versions.get(str(version)), Mapping) and
        versions[str(version)].get("status") == "BOUND"
        for version in (43, 44, 50)) else "NOT_READY")
    if isinstance(overall, Mapping) and overall.get("status") != expected_overall:
        errors.append("overall:status-mismatch")
    return {"schema": AUDIT_SCHEMA, "status": "PASS" if not errors else "FAIL",
            "errors": errors, "overall": manifest.get("overall"),
            "manifest_sha256": supplied_digest}


def write_manifest(manifest: Mapping[str, Any], output: Path) -> None:
    """Write once, canonically, and read-only; never overwrite an artifact."""
    raw = _canonical(manifest)
    output = Path(output)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    fd = os.open(output, flags, 0o444)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
    except Exception:
        try:
            output.unlink()
        except OSError:
            pass
        raise
    os.chmod(output, 0o444)


# Descriptive aliases keep the boundary easy to discover for callers of the
# planner while retaining one implementation and one canonical output.
bind_artifacts = bind_receipts
build_artifact_manifest = bind_receipts
audit_manifest = audit_artifact_manifest


def _read_json(path: Path) -> object:
    try:
        value, _facts = s8_depth_runner._json(Path(path), "artifact_json")
        return value
    except s8_depth_runner.DepthPlanError as exc:
        raise ArtifactBindingError(str(exc)) from exc


def load_receipt(path: Path) -> Mapping[str, Any]:
    """Load one receipt, optionally joining its exact role manifest.

    Build systems commonly retain provenance and file facts as two immutable
    JSON documents.  The receipt remains authoritative for version/source/
    build identity; only the absent ``roles`` and ``artifact_root`` fields are
    filled from the explicitly named manifest.
    """
    value = _read_json(Path(path))
    if not isinstance(value, Mapping):
        raise ArtifactBindingError(f"receipt is not an object: {path}")
    if isinstance(value.get("roles"), Mapping):
        return value
    manifest_name = value.get("artifact_manifest")
    if manifest_name is None:
        manifest_name = value.get("manifest")
    if not isinstance(manifest_name, str) or not manifest_name:
        return value
    manifest_path = Path(manifest_name)
    if not manifest_path.is_absolute():
        manifest_path = Path(path).parent / manifest_path
    manifest = _read_json(manifest_path)
    if not isinstance(manifest, Mapping) or not isinstance(manifest.get("roles"), Mapping):
        raise ArtifactBindingError(f"role manifest is missing roles: {manifest_path}")
    merged = dict(value)
    merged["roles"] = manifest["roles"]
    if "artifact_root" not in merged and "root" in manifest:
        merged["artifact_root"] = manifest["root"]
    return merged


def _receipt_paths(values: Sequence[str]) -> list[Path]:
    result: list[Path] = []
    for value in values:
        # Accept P43=/path, 43=/path, and a plain path for simple automation.
        if "=" in value and value.split("=", 1)[0].lstrip("P").isdigit():
            value = value.split("=", 1)[1]
        elif ":" in value and value.split(":", 1)[0].lstrip("P").isdigit():
            value = value.split(":", 1)[1]
        result.append(Path(value))
    return result


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--receipt", action="append", default=[],
                        help="exact JSON receipt path (optionally P43=PATH)")
    parser.add_argument("--output", type=Path,
                        help="write the immutable manifest once to this path")
    parser.add_argument("--audit", type=Path,
                        help="audit an existing immutable artifact manifest")
    args = parser.parse_args(argv)
    try:
        if args.audit:
            result: Mapping[str, Any] = audit_artifact_manifest(_read_json(args.audit))
        else:
            if not args.receipt:
                raise ArtifactBindingError("at least one --receipt is required")
            docs = [load_receipt(path) for path in _receipt_paths(args.receipt)]
            result = bind_receipts(docs)
            if args.output:
                write_manifest(result, args.output)
            result = {"schema": AUDIT_SCHEMA,
                      "status": "PASS" if result["overall"]["status"] == "READY" else "FAIL",
                      "errors": [error for row in result["versions"].values()
                                 for error in row.get("errors", [])],
                      "overall": result["overall"],
                      "manifest_sha256": result["manifest_sha256"]}
    except (OSError, UnicodeError, json.JSONDecodeError, ArtifactBindingError) as exc:
        result = {"schema": AUDIT_SCHEMA, "status": "FAIL", "errors": [str(exc)]}
    sys.stdout.buffer.write(_canonical(result))
    return 0 if result.get("status") == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
