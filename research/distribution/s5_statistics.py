#!/usr/bin/env python3
"""Fail-closed verifier for the preregistered S5 ZSTD_TU statistic.

The verifier consumes an immutable S5 evidence document.  A statistical
observation is *one complete block*: an ordered pair of complete builds, one
cache and one legacy.  TU rows are accepted as audit evidence, but are never
used as observations.  This module deliberately has no dependency on numpy
or a statistics package so that the bootstrap implementation is easy to
audit and deterministic across hosts.

The input shape is documented in ``S5-STATISTICS-CONTRACT.md``.  For a
streaming JSONL input, the first record is ``{"kind": "contract", ...}``,
followed by ``{"kind": "block", ...}`` and ``{"kind": "build", ...}``
records.  A normal JSON object may contain ``contract``, ``blocks`` and
``builds`` arrays.
"""

from __future__ import annotations

import hashlib
import json
import math
import os
import re
import stat
from decimal import Decimal, localcontext
from pathlib import Path
from dataclasses import dataclass
from typing import Any, Mapping, Sequence

SCHEMA = "icecream-s5-zstd-tu-statistics-v2"
VERIFIER_IMPLEMENTATION = "s5-statistics-python-v10"
# The upstream files are a separate, closed protocol.  Keeping this identifier
# distinct from the statistical document prevents a producer from smuggling an
# open-ended payload into an authenticated evidence record.
UPSTREAM_SCHEMA = "icecream-s5-upstream-artifact-v10"
UPSTREAM_REF_FIELDS = frozenset({
    "path", "sha256", "bytes", "artifact_id", "schema", "kind", "producer",
    "experiment_id", "run_id", "scope",
})
UPSTREAM_SCOPE_FIELDS = frozenset({"experiment_id", "build_id", "tu_id"})
UPSTREAM_KINDS = frozenset({
    "role-source-binary-manifest", "compiler-environment-manifest",
    "workload-run-manifest", "per-tu-ledger", "assignment-ledger",
    "input-record-ledger", "result-ledger", "wire-ledger", "join-ledger",
    "preparation-event-ledger", "prewarm-event-ledger", "cold-reset-ledger",
    "initial-state-prewarm-snapshot-manifest", "command-log-manifest",
})
UPSTREAM_MANIFEST_KINDS = frozenset({
    "role-source-binary-manifest", "compiler-environment-manifest",
    "workload-run-manifest", "initial-state-prewarm-snapshot-manifest",
    "command-log-manifest",
})

# V10 is a versioned, closed semantic registry.  The registry is the single
# acceptance authority for record fields; the machine-readable companion
# schema is checked against these exact sets by the test/evidence harness.
_LEGACY_UPSTREAM_COMMON_RECORD_FIELDS = frozenset({
    "record_id", "artifact_id", "experiment_id", "run_id", "scope",
})
ROW_KEY_PREFIX = "s5-assignment-row-v1"
ROW_KEY_FIELDS = (
    "run_id", "scenario_id", "scheduler_epoch", "wire_id", "assignment_nonce",
    "logical_attempt", "selected_f", "f_store_generation", "endpoint_generation",
    "c_guid", "tu_position", "build_id", "block_id", "tu_id", "method_id",
    "mode", "regime", "corpus_id",
)
UPSTREAM_JOIN_FIELDS = (
    "run_id", "scenario_id", "scheduler_epoch", "wire_id", "assignment_nonce",
    "logical_attempt", "selected_f", "f_store_generation", "endpoint_generation",
    "c_guid", "tu_position", "build_id", "block_id", "tu_id", "method_id",
    "mode", "regime", "corpus_id", "assignment_id", "input_record_id",
    "result_record_id", "ledger_record_id",
)
UPSTREAM_LINK_FIELDS = ("assignment_id", "input_record_id", "result_record_id", "ledger_record_id", "wire_id")
UPSTREAM_LEDGER_KINDS = (
    "per-tu-ledger", "assignment-ledger", "input-record-ledger", "result-ledger",
    "wire-ledger", "join-ledger",
)
_UPSTREAM_IDENTITY_FIELDS = frozenset(ROW_KEY_FIELDS + ("row_key",))
_UPSTREAM_ALL_JOIN_FIELDS = frozenset(UPSTREAM_JOIN_FIELDS + ("row_key",))
_UPSTREAM_KIND_REGISTRY: dict[str, dict[str, Any]] = {
    "role-source-binary-manifest": {
        "required": frozenset({"role", "path", "sha256", "bytes", "source_sha256", "binary_sha256", "release_identity", "image_digest"}),
        "optional": frozenset({"build_id", "block_id", "compiler_env_digest", "workload_manifest_digest"}),
        "role_enum": frozenset({"source", "binary", "source-binary"}),
        "digest_fields": frozenset({"sha256", "source_sha256", "binary_sha256", "image_digest"}),
    },
    "compiler-environment-manifest": {
        "required": frozenset({"compiler_env_digest", "path", "sha256", "bytes", "compiler_path", "compiler_version", "image_digest", "toolchain_identity"}),
        "optional": frozenset({"build_id", "block_id", "release_identity"}),
        "digest_fields": frozenset({"compiler_env_digest", "sha256", "image_digest"}),
    },
    "workload-run-manifest": {
        "required": frozenset({"build_id", "block_id", "regime", "mode", "path", "sha256", "bytes", "scenario_id", "topology_id", "clock_id", "tu_plan_digest", "argv", "cwd", "environment", "start_ns", "end_ns", "duration_ns", "measurement_boundary"}),
        "optional": frozenset({"tu_id", "workload_manifest_digest", "source_sha256", "binary_sha256", "compiler_env_digest", "image_digest"}),
        "digest_fields": frozenset({"sha256", "workload_manifest_digest", "source_sha256", "binary_sha256", "compiler_env_digest", "image_digest", "tu_plan_digest"}),
        "measurement_boundary": "measured-full-build",
    },
    "command-log-manifest": {
        "required": frozenset({"build_id", "block_id", "event_kind", "path", "sha256", "bytes", "purpose", "argv", "cwd", "environment", "producer_build_id", "clock_id", "start_ns", "end_ns", "duration_ns", "result_class"}),
        "optional": frozenset({"tu_id", "phase", "status"}),
        "digest_fields": frozenset({"sha256"}),
    },
    "initial-state-prewarm-snapshot-manifest": {
        "required": frozenset({"build_id", "block_id", "mode", "regime", "digest", "bytes", "namespace", "c_guid", "f_store_generation", "prewarm_manifest_digest", "state_digest", "source_mode", "source_regime", "owning_event_id"}),
        "optional": frozenset({"tu_id", "path", "sha256", "content_digest"}),
        "digest_fields": frozenset({"digest", "sha256", "prewarm_manifest_digest", "state_digest", "content_digest"}),
    },
    "preparation-event-ledger": {"required": frozenset({"event_id", "event_kind", "build_id", "block_id", "mode", "regime", "clock_id", "start_ns", "end_ns", "duration_ns", "measurement_boundary"}), "optional": frozenset({"tu_id", "phase", "status", "path", "sha256", "bytes"}), "event_kind": "preparation", "measurement_boundary": "outside-before-measurement", "digest_fields": frozenset({"sha256"})},
    "prewarm-event-ledger": {"required": frozenset({"event_id", "event_kind", "build_id", "block_id", "mode", "regime", "clock_id", "start_ns", "end_ns", "duration_ns", "measurement_boundary", "namespace", "c_guid", "f_store_generation", "prewarm_manifest_digest", "state_digest", "snapshot_id"}), "optional": frozenset({"tu_id", "phase", "status", "path", "sha256", "bytes"}), "event_kind": "prewarm", "measurement_boundary": "outside-before-measurement", "digest_fields": frozenset({"sha256", "prewarm_manifest_digest", "state_digest"})},
    "cold-reset-ledger": {"required": frozenset({"event_id", "event_kind", "build_id", "block_id", "mode", "regime", "clock_id", "start_ns", "end_ns", "duration_ns", "measurement_boundary", "fresh_c_guid", "f_store_wiped"}), "optional": frozenset({"tu_id", "phase", "status", "path", "sha256", "bytes"}), "event_kind": "reset", "measurement_boundary": "outside-before-measurement", "digest_fields": frozenset({"sha256"})},
}
for _ledger_kind in UPSTREAM_LEDGER_KINDS:
    _UPSTREAM_KIND_REGISTRY[_ledger_kind] = {
        "required": frozenset(_LEGACY_UPSTREAM_COMMON_RECORD_FIELDS | _UPSTREAM_ALL_JOIN_FIELDS),
        "optional": frozenset({"digest", "bytes", "status"}),
        "digest_fields": frozenset({"digest"}),
    }
_UPSTREAM_KIND_REGISTRY["join-ledger"].update({
    "required": frozenset(_LEGACY_UPSTREAM_COMMON_RECORD_FIELDS | _UPSTREAM_ALL_JOIN_FIELDS | {
        "source_record_ids", "source_count", "orphan_count", "duplicate_count",
        "dropped_count", "ordered_position",
    }),
    "optional": frozenset({"digest", "bytes", "status"}),
})
UPSTREAM_KIND_FIELDS = {k: (v["required"], v["optional"]) for k, v in _UPSTREAM_KIND_REGISTRY.items()}
DECISIONS = frozenset({"GREEN", "RED", "INCONCLUSIVE"})
REGIMES = ("cold", "warm")
MODES = ("cache", "legacy")
ORDERS = ("AB", "BA")
HEX64 = set("0123456789abcdef")

# Resource limits are part of the wire contract, rather than implementation
# tuning knobs.  They are intentionally conservative so malformed input is
# rejected before malformed input can induce unbounded allocation or bootstrap work.
MAX_INPUT_BYTES = 16 * 1024 * 1024
MAX_LINE_BYTES = 1024 * 1024
MAX_RECORDS = 100_000
MAX_BUILDS = 20_000
MAX_BLOCKS = 10_000
MAX_TUS = 200_000
MAX_ARTIFACTS = 100_000
MAX_TU_PLAN_ROWS = 20_000
MAX_EVIDENCE_TUS = 200_000
MAX_EVENTS = 100_000
MAX_ARTIFACT_BYTES_PER_FILE = 64 * 1024 * 1024
MAX_ARTIFACT_BYTES_TOTAL = 512 * 1024 * 1024
MAX_BOOTSTRAP_REPLICATES = 1_000_000
MAX_BOOTSTRAP_WORK = 100_000_000
MAX_JOIN_WORK = 2_000_000
MAX_ROW_KEY_BYTES = 8 * 1024 * 1024
MAX_INDEX_ENTRIES = MAX_RECORDS * len(UPSTREAM_LEDGER_KINDS)
MAX_EXPECTED_TU_MEMBERS = MAX_EVIDENCE_TUS
MAX_SEED = (1 << 128) - 1
MAX_PATH = 4096


class ContractError(ValueError):
    """Raised only for programmer/API misuse; document errors are reported."""


@dataclass(frozen=True)
class Verification:
    """Stable result returned by :func:`verify_document`."""

    valid: bool
    decision: str
    issues: tuple[str, ...]
    regimes: Mapping[str, Mapping[str, Any]]
    retained_builds: int
    retained_blocks: int

    def as_dict(self) -> dict[str, Any]:
        return {
            "schema": SCHEMA,
            "valid": self.valid,
            "decision": self.decision,
            "issues": list(self.issues),
            "regimes": {k: dict(v) for k, v in self.regimes.items()},
            "retained_builds": self.retained_builds,
            "retained_blocks": self.retained_blocks,
            "verifier_implementation": VERIFIER_IMPLEMENTATION,
        }


@dataclass(frozen=True)
class ObservationCollection:
    """Validated observations without an evidence decision.

    This is intentionally a data-only hand-off.  A document-shaped value is
    never accepted by the arithmetic routine, and this record has no
    ``decision`` or ``valid`` member that could be mistaken for an evidence
    verdict.
    """

    issues: tuple[str, ...]
    regimes: Mapping[str, Mapping[str, Any]]
    cold_ratios: tuple[float, ...]
    warm_ratios: tuple[float, ...]
    retained_builds: int
    retained_blocks: int


def canonical_json(value: Any) -> bytes:
    """Canonical bytes used for the preregistration and evidence hashes."""
    try:
        return json.dumps(value, ensure_ascii=True, sort_keys=True,
                          separators=(",", ":"), allow_nan=False).encode()
    except (TypeError, ValueError, OverflowError, RecursionError) as exc:
        raise ContractError("canonical_json_input_invalid") from exc


def canonical_file_bytes(value: Any) -> bytes:
    """Canonical JSON file bytes: one JSON body followed by exactly one LF."""
    return canonical_json(value) + b"\n"


def canonical_file_sha256(value: Any) -> str:
    """Digest of the complete canonical file representation."""
    return hashlib.sha256(canonical_file_bytes(value)).hexdigest()


def _normalize_direct(value: Any, path: str = "$", *, seen: set[int] | None = None) -> Any:
    """Copy one direct API value into exact built-in JSON containers.

    Mapping/list subclasses are rejected deliberately: invoking their dynamic
    ``get``/iteration methods would create two potentially different views of
    the authenticated object.  Parser-produced values are already exact
    built-ins, while this boundary gives callers one stable representation.
    """
    if seen is None:
        seen = set()
    if type(value) is dict:
        identity = id(value)
        if identity in seen:
            raise ContractError("input_container_cycle")
        seen.add(identity)
        try:
            if any(type(key) is not str for key in value):
                raise ContractError("nonstring_input_key")
            return {key: _normalize_direct(item, f"{path}.{key}", seen=seen)
                    for key, item in value.items()}
        finally:
            seen.remove(identity)
    if type(value) is list:
        identity = id(value)
        if identity in seen:
            raise ContractError("input_container_cycle")
        seen.add(identity)
        try:
            return [_normalize_direct(item, f"{path}[{index}]", seen=seen)
                    for index, item in enumerate(value)]
        finally:
            seen.remove(identity)
    if isinstance(value, (Mapping, Sequence)) and not isinstance(value, (str, bytes, bytearray)):
        raise ContractError("noncanonical_input_container")
    if type(value) is float and not math.isfinite(value):
        raise ContractError("nonfinite_input_number")
    if value is None or type(value) in (str, int, float, bool):
        return value
    raise ContractError("nonjson_input_value")


def _reject_constant(value: str) -> None:
    raise ValueError(f"non-finite JSON constant: {value}")


def _pairs_no_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def parse_canonical_bytes(raw: bytes, *, jsonl: bool = False) -> Any:
    """Parse canonical JSON or assemble the one canonical JSONL document."""
    if type(raw) is not bytes:
        raise ContractError("canonical input must be exact bytes")
    if len(raw) > MAX_INPUT_BYTES:
        raise ContractError("input exceeds MAX_INPUT_BYTES")
    if b"\r" in raw:
        raise ContractError("CRLF is not canonical")
    if not raw.endswith(b"\n"):
        raise ContractError("canonical input requires a final newline")
    if jsonl:
        rows = raw.split(b"\n")[:-1]
        if any(not row or len(row) > MAX_LINE_BYTES for row in rows):
            raise ContractError("blank or oversized JSONL row")
        if len(rows) > MAX_RECORDS:
            raise ContractError("too many JSONL records")
        parsed: list[dict[str, Any]] = []
        for row in rows:
            try:
                value = json.loads(row.decode("utf-8"), object_pairs_hook=_pairs_no_duplicates,
                                   parse_constant=_reject_constant)
            except (ValueError, UnicodeError) as exc:
                raise ContractError(f"invalid canonical JSONL row: {exc}") from exc
            if type(value) is not dict or canonical_json(value) + b"\n" != row + b"\n":
                raise ContractError("non-canonical JSONL row")
            parsed.append(value)
        if not parsed or parsed[0].get("kind") not in {"header", "contract"}:
            raise ContractError("JSONL must start with one header/contract")
        kinds = [row.get("kind") for row in parsed]
        if kinds.count("header") > 1 or kinds.count("contract") > 1:
            raise ContractError("duplicate JSONL header/contract")
        if "document" in kinds:
            raise ContractError("document override record forbidden")
        allowed = {"header", "contract", "block", "build", "tu", "event", "artifact", "recomputed"}
        if any(kind not in allowed for kind in kinds):
            raise ContractError("unknown JSONL phase")
        phase = {"header": 0, "contract": 1, "block": 2, "build": 3,
                 "tu": 4, "event": 5, "artifact": 6, "recomputed": 7}
        if any(phase[kinds[i]] > phase[kinds[i + 1]] for i in range(len(kinds) - 1)):
            raise ContractError("JSONL phase order invalid")
        if kinds[0] == "header" and kinds.count("contract") != 1:
            raise ContractError("JSONL header requires contract")
        if kinds[0] == "contract" and kinds.count("header"):
            raise ContractError("JSONL header must precede contract")
        document: dict[str, Any] = {"schema": SCHEMA, "blocks": [], "builds": [],
                                    "tu_rows": [], "events": [], "artifact_refs": []}
        def row_payload(row: dict[str, Any], field: str) -> Any:
            return row[field] if field in row else {key: value for key, value in row.items() if key != "kind"}
        for row in parsed:
            kind = row["kind"]
            if kind == "header":
                payload = row.get("header", row)
                if isinstance(payload, dict) and payload.get("schema") not in (None, SCHEMA):
                    raise ContractError("JSONL schema mismatch")
            elif kind == "contract":
                payload = row_payload(row, "contract")
                if not isinstance(payload, dict):
                    raise ContractError("JSONL contract is not an object")
                document["contract"] = payload
                for field in ("contract_digest", "preregistration_digest", "experiment_id", "artifact_refs"):
                    if field in row:
                        document[field] = row[field]
            elif kind in {"block", "build", "tu", "event", "artifact", "recomputed"}:
                field = {"block": "block", "build": "build", "tu": "tu",
                         "event": "event", "artifact": "artifact",
                         "recomputed": "recomputed"}[kind]
                payload = row_payload(row, field)
                if kind == "artifact" and "artifact_ref" in row:
                    payload = row["artifact_ref"]
                if not isinstance(payload, dict):
                    raise ContractError(f"JSONL {kind} is not an object")
                target = {"block": "blocks", "build": "builds", "tu": "tu_rows",
                          "event": "events", "artifact": "artifact_refs"}.get(kind)
                if target:
                    document[target].append(payload)
                else:
                    document["recomputed"] = payload
        if "contract" not in document:
            raise ContractError("JSONL contract missing")
        return document
    body = raw[:-1]
    if not body:
        raise ContractError("empty canonical document")
    try:
        value = json.loads(body.decode("utf-8"), object_pairs_hook=_pairs_no_duplicates,
                           parse_constant=_reject_constant)
    except (ValueError, UnicodeError) as exc:
        raise ContractError(f"invalid canonical JSON: {exc}") from exc
    if canonical_json(value) + b"\n" != raw:
        raise ContractError("non-canonical JSON bytes")
    return value


def is_canonical_jsonl(raw: bytes) -> bool:
    """Detect JSONL by parsing its first canonical row, independent of key order."""
    if type(raw) is not bytes:
        return False
    if not raw.endswith(b"\n"):
        return False
    first = raw.split(b"\n", 1)[0]
    try:
        value = json.loads(first.decode("utf-8"), object_pairs_hook=_pairs_no_duplicates,
                          parse_constant=_reject_constant)
    except (ValueError, UnicodeError):
        return False
    return type(value) is dict and value.get("kind") in {
        "header", "contract", "block", "build", "tu", "event", "artifact", "recomputed"
    }


def _safe_relative_path(value: Any) -> bool:
    if type(value) is not str or not value or len(value) > MAX_PATH:
        return False
    if "\\" in value or value.startswith("/") or "//" in value:
        return False
    parts = value.split("/")
    return all(part not in ("", ".", "..") for part in parts)


_UPSTREAM_HEADER_FIELDS = frozenset({
    "schema", "kind", "artifact_id", "producer", "experiment_id", "run_id",
    "scope", "content_sha256", "content_bytes", "record_count", "records",
})
_LEGACY_UPSTREAM_RECORD_FIELDS = frozenset({
    "record_id", "artifact_id", "experiment_id", "run_id", "scope", "build_id",
    "block_id", "tu_id", "mode", "regime", "assignment_id", "input_record_id",
    "result_record_id", "ledger_record_id", "wire_id", "event_id", "event_kind",
    "start_ns", "end_ns", "phase", "digest", "bytes", "status", "row_key",
    "source_sha256", "binary_sha256", "compiler_env_digest", "workload_manifest_digest",
    "image_digest", "release_identity", "path", "sha256",
})

# Closed per-kind record registry.  The historical global optional-field union remains
# named above for compatibility with older callers, but it is not an
# acceptance authority: every parsed record is checked against this registry.
_UPSTREAM_COMMON_RECORD_FIELDS = frozenset({
    "record_id", "artifact_id", "experiment_id", "run_id", "scope",
})
_LEGACY_UPSTREAM_KIND_FIELDS: dict[str, tuple[frozenset[str], frozenset[str]]] = {
    "role-source-binary-manifest": (
        frozenset({"role", "path", "sha256", "bytes", "source_sha256", "binary_sha256", "release_identity", "image_digest"}),
        frozenset({"build_id", "block_id", "tu_id", "compiler_env_digest", "workload_manifest_digest"})),
    "compiler-environment-manifest": (
        frozenset({"compiler_env_digest", "path", "sha256", "bytes"}),
        frozenset({"build_id", "block_id", "tu_id", "image_digest", "release_identity"})),
    "workload-run-manifest": (
        frozenset({"build_id", "block_id", "regime", "mode", "path", "sha256", "bytes"}),
        frozenset({"tu_id", "workload_manifest_digest", "source_sha256", "binary_sha256", "compiler_env_digest"})),
    "command-log-manifest": (
        frozenset({"build_id", "block_id", "path", "sha256", "bytes", "event_kind"}),
        frozenset({"tu_id", "phase", "status", "start_ns", "end_ns"})),
    "initial-state-prewarm-snapshot-manifest": (
        frozenset({"build_id", "block_id", "mode", "digest", "bytes"}),
        frozenset({"tu_id", "path", "sha256", "initial_state_digest", "content_digest"})),
    "preparation-event-ledger": (
        frozenset({"event_id", "event_kind", "build_id", "block_id", "start_ns", "end_ns"}),
        frozenset({"tu_id", "mode", "regime", "phase", "status", "path", "sha256", "bytes"})),
    "prewarm-event-ledger": (
        frozenset({"event_id", "event_kind", "build_id", "block_id", "start_ns", "end_ns"}),
        frozenset({"tu_id", "mode", "regime", "phase", "status", "path", "sha256", "bytes"})),
    "cold-reset-ledger": (
        frozenset({"event_id", "event_kind", "build_id", "block_id", "start_ns", "end_ns"}),
        frozenset({"tu_id", "mode", "regime", "phase", "status", "path", "sha256", "bytes"})),
    "per-tu-ledger": (
        frozenset({"build_id", "tu_id"}),
        frozenset({"block_id", "mode", "regime", "assignment_id", "input_record_id", "result_record_id", "ledger_record_id", "digest", "bytes", "status", "row_key"})),
    "assignment-ledger": (
        frozenset({"build_id", "tu_id", "assignment_id"}),
        frozenset({"block_id", "mode", "regime", "input_record_id", "digest", "bytes", "status", "row_key"})),
    "input-record-ledger": (
        frozenset({"build_id", "tu_id", "assignment_id", "input_record_id"}),
        frozenset({"block_id", "mode", "regime", "digest", "bytes", "status", "row_key"})),
    "result-ledger": (
        frozenset({"build_id", "tu_id", "assignment_id", "result_record_id"}),
        frozenset({"block_id", "mode", "regime", "digest", "bytes", "status", "row_key"})),
    "wire-ledger": (
        frozenset({"build_id", "tu_id", "assignment_id", "wire_id"}),
        frozenset({"block_id", "mode", "regime", "digest", "bytes", "status", "row_key"})),
    "join-ledger": (
        frozenset({"build_id", "block_id", "row_key", "status"}),
        frozenset({"tu_id", "mode", "regime", "assignment_id", "input_record_id", "result_record_id", "wire_id", "bytes", "digest"})),
}
_UPSTREAM_KIND_FIELDS = UPSTREAM_KIND_FIELDS
_UPSTREAM_STRING_FIELDS = frozenset({
    "record_id", "artifact_id", "experiment_id", "run_id", "build_id", "block_id", "tu_id",
    "mode", "regime", "assignment_id", "input_record_id", "result_record_id", "ledger_record_id",
    "wire_id", "event_id", "event_kind", "phase", "status", "role", "path", "sha256",
    "source_sha256", "binary_sha256", "compiler_env_digest", "workload_manifest_digest",
    "image_digest", "release_identity", "digest", "initial_state_digest", "content_digest",
    "scenario_id", "topology_id", "clock_id", "tu_plan_digest", "compiler_path",
    "compiler_version", "toolchain_identity", "namespace", "state_digest",
    "prewarm_manifest_digest", "owning_event_id", "purpose", "producer_build_id",
    "result_class", "measurement_boundary", "cwd", "snapshot_id",
    "method_id", "corpus_id", "selected_f",
    "c_guid", "assignment_nonce", "source_mode", "source_regime",
})
_UPSTREAM_INTEGER_FIELDS = frozenset({
    "start_ns", "end_ns", "bytes", "duration_ns", "scheduler_epoch",
    "logical_attempt", "tu_position", "f_store_generation", "endpoint_generation",
    "source_count", "orphan_count", "duplicate_count", "dropped_count",
    "ordered_position",
})
_UPSTREAM_BOOLEAN_FIELDS = frozenset({"fresh_c_guid", "f_store_wiped"})
_UPSTREAM_ENUM_FIELDS: dict[str, frozenset[str]] = {
    "mode": frozenset(MODES),
    "regime": frozenset(REGIMES),
    "source_mode": frozenset(MODES),
    "source_regime": frozenset(REGIMES),
    "event_kind": frozenset({"preparation", "prewarm", "reset", "manifest_validation", "command"}),
    "measurement_boundary": frozenset({"outside-before-measurement", "measured-full-build"}),
}
_UPSTREAM_DIGEST_FIELDS = frozenset({
    "digest", "sha256", "source_sha256", "binary_sha256", "compiler_env_digest",
    "workload_manifest_digest", "initial_state_digest", "content_digest", "image_digest",
    "state_digest", "prewarm_manifest_digest", "tu_plan_digest",
})

# These metadata entries are serialized into the V10 registry companion.  The
# validator consumes the same sets; the generated JSON schema is checked
# field-for-field against them in the functional controls.
_UPSTREAM_FIELD_TYPES = {
    "scope": "object",
    "argv": "array[string]",
    "environment": "object[string,string]",
    "row_key": "array[row-key-v1]",
    "source_record_ids": "object[source-record-id]",
}
for _field in _UPSTREAM_STRING_FIELDS:
    _UPSTREAM_FIELD_TYPES.setdefault(_field, "string")
for _field in _UPSTREAM_INTEGER_FIELDS:
    _UPSTREAM_FIELD_TYPES[_field] = "integer"
for _field in _UPSTREAM_BOOLEAN_FIELDS:
    _UPSTREAM_FIELD_TYPES[_field] = "boolean"
for _kind, _spec in _UPSTREAM_KIND_REGISTRY.items():
    _fields = _spec["required"] | _spec["optional"] | _LEGACY_UPSTREAM_COMMON_RECORD_FIELDS
    _spec["field_types"] = {field: _UPSTREAM_FIELD_TYPES.get(field, "string")
                             for field in sorted(_fields)}
    _spec["integer_ranges"] = {
        field: {"minimum": 0} for field in sorted(_fields & _UPSTREAM_INTEGER_FIELDS)
    }
    _spec["enum_fields"] = {
        field: sorted(values) for field, values in _UPSTREAM_ENUM_FIELDS.items()
        if field in _fields
    }
    _spec["join_key_fields"] = list(UPSTREAM_JOIN_FIELDS) if _kind in UPSTREAM_LEDGER_KINDS else []
    _spec["cardinality"] = {
        "source": "authenticated_evidence.tu_rows" if _kind in UPSTREAM_LEDGER_KINDS
        else "authenticated_workload_or_event_inventory",
        "rule": "exact-one-per-expected-row" if _kind in UPSTREAM_LEDGER_KINDS
        else "one-record-per-declared-artifact",
    }
    _spec["relations"] = [
        {"kind": other, "relation": "same-row-key-and-all-link-fields"}
        for other in UPSTREAM_LEDGER_KINDS if other != _kind
    ] if _kind in UPSTREAM_LEDGER_KINDS else []


def _recompute_upstream_row_key(row: Mapping[str, Any]) -> list[Any]:
    """Build the immutable V10 observation key from sibling fields only."""
    return [ROW_KEY_PREFIX, *(row.get(field) for field in ROW_KEY_FIELDS)]


# Short private spelling is useful to callers that exercised the V9 helper.
_recompute_row_key = _recompute_upstream_row_key


def _validate_upstream_record_shape(row: Any, kind: str, issues: list[str], label: str) -> bool:
    """Validate one closed V10 record before any join or membership operation."""
    if type(row) is not dict:
        _issue(issues, "upstream_artifact_record_invalid", label)
        return False
    if type(kind) is not str:
        _issue(issues, "upstream_artifact_kind_invalid", label)
        return False
    spec = _UPSTREAM_KIND_FIELDS.get(kind)
    if spec is None:
        _issue(issues, "upstream_artifact_kind_invalid", label)
        return False
    required, optional = spec
    keys = set(row)
    allowed = _UPSTREAM_COMMON_RECORD_FIELDS | required | optional
    valid = True
    unknown = keys - allowed
    missing = (_UPSTREAM_COMMON_RECORD_FIELDS | required) - keys
    if unknown:
        _issue(issues, "upstream_artifact_unknown_record_field", label)
        valid = False
    if missing:
        _issue(issues, "upstream_artifact_record_required_field_missing", f"{label}:{','.join(sorted(missing))}")
        valid = False

    # All primitive checks are exact (rather than isinstance) so bool cannot
    # masquerade as an integer and user-defined containers cannot alter the
    # authenticated view.
    for field in keys & _UPSTREAM_STRING_FIELDS:
        if type(row[field]) is not str or not row[field]:
            _issue(issues, "upstream_artifact_record_string_type_invalid", f"{label}:{field}")
            valid = False
    for field in keys & _UPSTREAM_INTEGER_FIELDS:
        if type(row[field]) is not int or isinstance(row[field], bool) or row[field] < 0:
            _issue(issues, "upstream_artifact_record_integer_type_invalid", f"{label}:{field}")
            valid = False
    for field in keys & _UPSTREAM_BOOLEAN_FIELDS:
        if type(row[field]) is not bool:
            _issue(issues, "upstream_artifact_record_boolean_type_invalid", f"{label}:{field}")
            valid = False
    for field, values in _UPSTREAM_ENUM_FIELDS.items():
        if field in row and (type(row[field]) is not str or row[field] not in values):
            _issue(issues, "upstream_artifact_record_enum_invalid", f"{label}:{field}")
            valid = False
    for field in keys & _UPSTREAM_DIGEST_FIELDS:
        value = row[field]
        digest_ok = _is_digest(value)
        if field == "image_digest":
            digest_ok = type(value) is str and re.fullmatch(r"sha256:[0-9a-f]{64}", value) is not None
        if not digest_ok:
            _issue(issues, "upstream_artifact_record_digest_invalid", f"{label}:{field}")
            valid = False
    if "role" in row and row["role"] not in _UPSTREAM_KIND_REGISTRY["role-source-binary-manifest"]["role_enum"]:
        _issue(issues, "upstream_artifact_record_role_invalid", label)
        valid = False
    if "release_identity" in row and (type(row["release_identity"]) is not str or re.fullmatch(r"[a-z0-9][a-z0-9._-]*-[0-9][a-z0-9._-]*", row["release_identity"]) is None):
        _issue(issues, "upstream_artifact_record_release_identity_invalid", label)
        valid = False
    if "path" in row and not _safe_relative_path(row["path"]):
        _issue(issues, "upstream_artifact_record_path_invalid", label)
        valid = False
    if "argv" in row and (type(row["argv"]) is not list or not row["argv"] or any(type(x) is not str or not x for x in row["argv"])):
        _issue(issues, "upstream_artifact_record_argv_invalid", label)
        valid = False
    if "environment" in row:
        environment = row["environment"]
        if type(environment) is not dict or any(type(key) is not str or type(value) is not str
                                                for key, value in environment.items()):
            _issue(issues, "upstream_artifact_record_environment_invalid", label)
            valid = False
    if "start_ns" in row and "end_ns" in row:
        if not (type(row["start_ns"]) is int and not isinstance(row["start_ns"], bool)
                and row["start_ns"] >= 0 and type(row["end_ns"]) is int
                and not isinstance(row["end_ns"], bool) and row["end_ns"] > row["start_ns"]):
            _issue(issues, "upstream_artifact_record_event_interval_invalid", label)
            valid = False
        elif "duration_ns" in row and row["duration_ns"] != row["end_ns"] - row["start_ns"]:
            _issue(issues, "upstream_artifact_record_duration_mismatch", label)
            valid = False
    expected_event = _UPSTREAM_KIND_REGISTRY.get(kind, {}).get("event_kind")
    if expected_event is not None and row.get("event_kind") != expected_event:
        _issue(issues, "upstream_artifact_record_event_kind_invalid", label)
        valid = False
    expected_boundary = _UPSTREAM_KIND_REGISTRY.get(kind, {}).get("measurement_boundary")
    if expected_boundary is not None and row.get("measurement_boundary") != expected_boundary:
        _issue(issues, "upstream_artifact_record_measurement_boundary_invalid", label)
        valid = False
    if "scope" in row:
        scope = row["scope"]
        if type(scope) is not dict or set(scope) != UPSTREAM_SCOPE_FIELDS:
            _issue(issues, "upstream_artifact_record_scope_invalid", label)
            valid = False
        elif any(type(scope[field]) is not str or not scope[field] for field in UPSTREAM_SCOPE_FIELDS):
            _issue(issues, "upstream_artifact_record_scope_value_invalid", label)
            valid = False

    # A V10 row key is a derived value, never a caller-supplied identity.  The
    # length/type checks are kept separate so each malformed position yields a
    # stable, attributable issue before any key is indexed.
    if "row_key" in row:
        presented = row["row_key"]
        if type(presented) is not list:
            _issue(issues, "upstream_artifact_record_row_key_type_invalid", label)
            valid = False
        else:
            if len(presented) != len(ROW_KEY_FIELDS) + 1:
                _issue(issues, "upstream_artifact_record_row_key_arity_invalid", label)
                valid = False
            expected = _recompute_upstream_row_key(row)
            for index, item in enumerate(presented[:len(expected)]):
                if index == 0 or index - 1 < len(ROW_KEY_FIELDS):
                    field = None if index == 0 else ROW_KEY_FIELDS[index - 1]
                    expected_type = str if field is None or field not in _UPSTREAM_INTEGER_FIELDS else int
                    if expected_type is int:
                        item_type_ok = type(item) is int and not isinstance(item, bool)
                    else:
                        item_type_ok = type(item) is str and bool(item)
                    if not item_type_ok:
                        _issue(issues, "upstream_artifact_record_row_key_position_type_invalid",
                               f"{label}:{index}")
                        valid = False
                    if item != expected[index]:
                        _issue(issues, "upstream_artifact_record_row_key_position_invalid",
                               f"{label}:{index}")
                        valid = False
            if len(presented) == len(expected) and presented != expected:
                _issue(issues, "upstream_artifact_record_row_key_mismatch", label)
                valid = False

    if kind == "join-ledger":
        source_ids = row.get("source_record_ids")
        if type(source_ids) is not dict or set(source_ids) != {"per_tu", "assignment", "input_record", "result", "wire"}:
            _issue(issues, "upstream_artifact_join_source_record_ids_invalid", label)
            valid = False
        elif any(type(source_ids[field]) is not str or not source_ids[field]
                 for field in ("per_tu", "assignment", "input_record", "result", "wire")):
            _issue(issues, "upstream_artifact_join_source_record_ids_type_invalid", label)
            valid = False
        for field, expected_value in (("source_count", 5), ("orphan_count", 0),
                                      ("duplicate_count", 0), ("dropped_count", 0)):
            if row.get(field) != expected_value:
                _issue(issues, "upstream_artifact_join_count_invalid", f"{label}:{field}")
                valid = False
        if type(row.get("ordered_position")) is not int or isinstance(row.get("ordered_position"), bool):
            _issue(issues, "upstream_artifact_join_order_invalid", label)
            valid = False
        elif type(row.get("tu_position")) is int and row["ordered_position"] != row["tu_position"]:
            _issue(issues, "upstream_artifact_join_order_invalid", label)
            valid = False
    return valid


def _validate_upstream_scope(scope: Any, ref: Mapping[str, Any], issues: list[str], label: str) -> bool:
    """Validate the explicit three-level scope; no omitted/default scope exists."""
    if not isinstance(scope, Mapping) or _safe_mapping_keys(scope, "upstream_scope", issues) != UPSTREAM_SCOPE_FIELDS:
        _issue(issues, "upstream_artifact_scope_invalid", label)
        return False
    valid = True
    for field in UPSTREAM_SCOPE_FIELDS:
        if type(scope.get(field)) is not str or not scope[field]:
            _issue(issues, "upstream_artifact_scope_value_invalid", f"{label}:{field}")
            valid = False
    if valid and (scope != ref.get("scope") or scope.get("experiment_id") != ref.get("experiment_id")):
        _issue(issues, "upstream_artifact_scope_mismatch", label)
        valid = False
    return valid


def _parse_upstream_bytes(raw: bytes, ref: Mapping[str, Any], issues: list[str]) -> list[Mapping[str, Any]]:
    """Parse one authenticated upstream file without reopening its pathname.

    Both representations have one closed header and a closed record envelope.
    The returned records are views of the already opened bytes, so callers can
    bind joins without creating a second pathname-to-bytes authority.
    """
    if type(raw) is not bytes or len(raw) > MAX_ARTIFACT_BYTES_PER_FILE:
        _issue(issues, "upstream_artifact_semantic_bytes_limit_exceeded", str(ref.get("path")))
        return []
    try:
        if is_canonical_jsonl(raw):
            rows = raw.split(b"\n")[:-1]
            parsed = []
            for line in rows:
                value = json.loads(line.decode("utf-8"), object_pairs_hook=_pairs_no_duplicates,
                                   parse_constant=_reject_constant)
                if type(value) is not dict or canonical_json(value) + b"\n" != line + b"\n":
                    raise ContractError("upstream_noncanonical_jsonl")
                parsed.append(value)
            if not parsed or parsed[0].get("kind") != "header":
                raise ContractError("upstream_jsonl_header_missing")
            header = dict(parsed[0])
            if _safe_mapping_keys(header, "upstream_header", issues) != {
                    "kind", "schema", "artifact_kind", "artifact_id", "producer",
                    "experiment_id", "run_id", "scope", "content_sha256", "content_bytes", "record_count"}:
                _issue(issues, "upstream_artifact_unknown_field", str(ref.get("path")))
                return []
            header_kind = header.pop("artifact_kind")
            header["kind"] = header_kind
            records = []
            content_bytes = b""
            for row in parsed[1:]:
                if _safe_mapping_keys(row, "upstream_record_envelope", issues) != {"kind", "record"} or row.get("kind") != "record":
                    _issue(issues, "upstream_artifact_record_envelope_invalid", str(ref.get("path")))
                    continue
                records.append(row.get("record"))
                content_bytes += canonical_file_bytes(row)
        else:
            value = parse_canonical_bytes(raw)
            if not isinstance(value, Mapping) or _safe_mapping_keys(value, "upstream_header", issues) != _UPSTREAM_HEADER_FIELDS:
                _issue(issues, "upstream_artifact_unknown_field", str(ref.get("path")))
                return []
            header = value
            records = value.get("records")
            content_bytes = canonical_file_bytes(records)
            if not isinstance(records, list):
                _issue(issues, "upstream_artifact_records_invalid", str(ref.get("path")))
                return []
    except (ContractError, ValueError, TypeError, UnicodeError, RecursionError):
        _issue(issues, "upstream_artifact_noncanonical", str(ref.get("path")))
        return []
    if header.get("schema") != UPSTREAM_SCHEMA:
        _issue(issues, "upstream_artifact_file_schema_invalid", str(ref.get("path")))
    if header.get("kind") != ref.get("kind"):
        _issue(issues, "upstream_artifact_file_kind_mismatch", str(ref.get("path")))
    for field in ("artifact_id", "producer", "experiment_id", "run_id"):
        if header.get(field) != ref.get(field):
            _issue(issues, "upstream_artifact_file_identity_mismatch", f"{ref.get('path')}:{field}")
    if (header.get("content_sha256") != hashlib.sha256(content_bytes).hexdigest()
            or header.get("content_bytes") != len(content_bytes)):
        _issue(issues, "upstream_artifact_content_descriptor_mismatch", str(ref.get("path")))
    _validate_upstream_scope(header.get("scope"), ref, issues, str(ref.get("path")))
    if isinstance(records, list):
        declared = header.get("record_count")
        if declared != len(records):
            _issue(issues, "upstream_artifact_record_count_mismatch", str(ref.get("path")))
    if not isinstance(records, list) or len(records) > MAX_RECORDS:
        _issue(issues, "upstream_artifact_record_limit_exceeded", str(ref.get("path")))
        return []
    seen_ids: set[str] = set()
    for row in records:
        if type(row) is not dict:
            _issue(issues, "upstream_artifact_record_invalid", str(ref.get("path")))
            continue
        _validate_upstream_record_shape(row, str(ref.get("kind")), issues, str(ref.get("path")))
        scope = row.get("scope")
        if (type(scope) is dict and
                (scope.get("experiment_id") != row.get("experiment_id")
                 or ("build_id" in row and scope.get("build_id") != row.get("build_id"))
                 or ("tu_id" in row and scope.get("tu_id") != row.get("tu_id")))):
            _issue(issues, "upstream_artifact_record_scope_sibling_mismatch", str(row.get("record_id")))
        record_id = row.get("record_id")
        if type(record_id) is not str or not record_id:
            _issue(issues, "upstream_artifact_record_id_invalid", str(ref.get("path")))
        elif record_id in seen_ids:
            _issue(issues, "upstream_artifact_duplicate_record_id", record_id)
        else:
            seen_ids.add(record_id)
        if row.get("artifact_id") != ref.get("artifact_id"):
            _issue(issues, "upstream_artifact_record_artifact_mismatch", str(record_id))
        if row.get("experiment_id") != ref.get("experiment_id") or row.get("run_id") != ref.get("run_id"):
            _issue(issues, "upstream_artifact_record_scope_mismatch", str(record_id))
        if row.get("scope") != ref.get("scope"):
            _issue(issues, "upstream_artifact_record_scope_mismatch", str(record_id))
    return [row for row in records if isinstance(row, Mapping)]


def _validate_v10_membership(
    records_by_kind: Mapping[str, Sequence[Mapping[str, Any]]],
    parsed_records: Sequence[Mapping[str, Any]],
    refs: Sequence[Mapping[str, Any]],
    document: Mapping[str, Any],
    contract: Mapping[str, Any],
    issues: list[str],
) -> None:
    """Close V10 row membership and setup-event interval joins.

    The only authoritative row order is the preregistered TU order carried by
    the authenticated evidence rows.  Source ledgers are indexed once by the
    derived row key and then compared at the same positions; neither a source
    ledger nor its self-declared totals can create a missing member.
    """
    if type(records_by_kind) is not dict:
        _issue(issues, "upstream_artifact_index_invalid")
        return
    if len(records_by_kind) > len(UPSTREAM_KINDS):
        _issue(issues, "upstream_artifact_kind_index_limit_exceeded")
        return
    total_records = sum(len(rows) for rows in records_by_kind.values()
                        if isinstance(rows, Sequence) and not isinstance(rows, (str, bytes, bytearray)))
    if total_records > MAX_RECORDS:
        _issue(issues, "upstream_artifact_total_record_limit_exceeded")
        return
    if total_records * len(UPSTREAM_LEDGER_KINDS) > MAX_JOIN_WORK:
        _issue(issues, "upstream_artifact_join_work_limit_exceeded")
        return

    def typed_key(row: Mapping[str, Any], label: str) -> tuple[Any, ...] | None:
        presented = row.get("row_key")
        if type(presented) is not list or len(presented) != len(ROW_KEY_FIELDS) + 1:
            return None
        expected = _recompute_upstream_row_key(row)
        if presented != expected:
            return None
        for index, value in enumerate(presented):
            field = None if index == 0 else ROW_KEY_FIELDS[index - 1]
            if field is None:
                if type(value) is not str or value != ROW_KEY_PREFIX:
                    return None
            elif field in _UPSTREAM_INTEGER_FIELDS:
                if type(value) is not int or isinstance(value, bool) or value < 0:
                    return None
            elif type(value) is not str or not value:
                return None
        try:
            return tuple(presented)
        except (TypeError, ValueError):
            _issue(issues, "upstream_artifact_row_key_unhashable", label)
            return None

    # Key and membership limits are charged from authenticated records before
    # any per-kind index is allocated.
    key_bytes = 0
    for record in parsed_records:
        row_key = record.get("row_key")
        if type(row_key) is list:
            try:
                key_bytes += len(canonical_json(row_key))
            except (TypeError, ValueError, OverflowError):
                _issue(issues, "upstream_artifact_row_key_encoding_invalid", str(record.get("record_id")))
    if key_bytes > MAX_ROW_KEY_BYTES:
        _issue(issues, "upstream_artifact_row_key_bytes_limit_exceeded")
        return
    if total_records > MAX_INDEX_ENTRIES:
        _issue(issues, "upstream_artifact_index_entry_limit_exceeded")
        return

    # Derive the expected ordered key sequence from authenticated evidence TU
    # rows.  A supplied row_key is accepted only when it equals the exact
    # recomputation from its typed sibling fields.
    evidence_rows = document.get("tu_rows")
    expected_keys: list[tuple[Any, ...]] = []
    expected_key_set: set[tuple[Any, ...]] = set()
    expected_rows: list[Mapping[str, Any]] = []
    if type(evidence_rows) is not list or not evidence_rows:
        _issue(issues, "upstream_artifact_expected_tu_membership_missing")
    elif len(evidence_rows) > MAX_EXPECTED_TU_MEMBERS:
        _issue(issues, "upstream_artifact_expected_tu_member_limit_exceeded")
        return
    else:
        for position, evidence_row in enumerate(evidence_rows):
            if not isinstance(evidence_row, Mapping):
                _issue(issues, "upstream_artifact_expected_tu_row_invalid", str(position))
                continue
            # V10 evidence rows carry the same 18 sibling fields as source
            # ledgers.  Do not synthesize an identity from a short legacy
            # join_identity object.
            if not all(field in evidence_row for field in UPSTREAM_JOIN_FIELDS):
                _issue(issues, "upstream_artifact_expected_tu_row_identity_missing", str(position))
                continue
            if any(type(evidence_row.get(field)) is not str or not evidence_row.get(field)
                   for field in ("assignment_id", "input_record_id", "result_record_id", "ledger_record_id")):
                _issue(issues, "upstream_artifact_expected_tu_row_link_type_invalid", str(position))
                continue
            expected = _recompute_upstream_row_key(evidence_row)
            presented = evidence_row.get("row_key", expected)
            if type(presented) is not list or presented != expected:
                _issue(issues, "upstream_artifact_expected_tu_row_key_invalid", str(position))
                continue
            candidate = dict(evidence_row)
            candidate["row_key"] = presented
            key = typed_key(candidate, f"evidence:{position}")
            if key is None:
                _issue(issues, "upstream_artifact_expected_tu_row_key_invalid", str(position))
                continue
            if key in expected_key_set:
                _issue(issues, "upstream_artifact_expected_tu_duplicate", str(position))
            expected_keys.append(key)
            expected_key_set.add(key)
            expected_rows.append(evidence_row)
    if len(expected_keys) > MAX_EXPECTED_TU_MEMBERS:
        _issue(issues, "upstream_artifact_expected_tu_member_limit_exceeded")

    # When a TU plan is present, its immutable flattened build/TU order is an
    # additional authority.  It can never be replaced by a source ledger.
    tu_plan = contract.get("tu_plan") if isinstance(contract, Mapping) else None
    if type(tu_plan) is not list or not tu_plan:
        _issue(issues, "upstream_artifact_preregistered_tu_plan_missing")
    elif expected_rows:
        planned_pairs: list[tuple[str, str]] = []
        plan_valid = True
        seen_plan_builds: set[str] = set()
        for plan_row in tu_plan:
            if type(plan_row) is not dict:
                plan_valid = False
                continue
            build_id = plan_row.get("build_id")
            tu_ids = plan_row.get("tu_ids")
            if (type(build_id) is not str or not build_id
                    or build_id in seen_plan_builds
                    or type(tu_ids) is not list
                    or not tu_ids
                    or any(type(tu_id) is not str or not tu_id for tu_id in tu_ids)):
                plan_valid = False
                continue
            seen_plan_builds.add(build_id)
            planned_pairs.extend((build_id, tu_id) for tu_id in tu_ids)
        actual_pairs = [(key[12], key[14]) for key in expected_keys]
        if not plan_valid or actual_pairs != planned_pairs:
            _issue(issues, "upstream_artifact_preregistered_tu_order_mismatch")

    # The artifact plan is a path inventory, not the authority for how many
    # build declarations must exist: a producer cannot delete a whole
    # declaration class and rewrite that path list to make the omission look
    # intentional.  Bind the three non-ledger declarations that identify each
    # build directly to the authenticated evidence TU inventory.  Workload,
    # setup, and snapshot inventories have their stronger mode/regime joins
    # below; these declarations deliberately use their available build/block
    # identity and still require exactly one row for every expected pair.  The
    # workload/setup/snapshot declarations are included here as well: an
    # internally consistent ghost build must not replace a preregistered one,
    # and deleting a complete declaration bundle must not hide the omission.
    expected_build_blocks: set[tuple[str, str]] = set()
    for evidence_row in expected_rows:
        build_id, block_id = evidence_row.get("build_id"), evidence_row.get("block_id")
        pair = (build_id, block_id)
        expected_build_blocks.add(pair)
    scoped_declaration_kinds = {
        "workload-run-manifest", "preparation-event-ledger",
        "prewarm-event-ledger", "cold-reset-ledger",
        "initial-state-prewarm-snapshot-manifest",
    }
    expected_declaration_keys = {
        kind: {
            (row.get("build_id"), row.get("block_id"), row.get("mode"), row.get("regime"))
            for row in expected_rows
            # Warm builds intentionally have no cold-reset declaration.
            if kind != "cold-reset-ledger" or row.get("regime") == "cold"
        }
        for kind in scoped_declaration_kinds
    }
    declaration_kinds = (
        "role-source-binary-manifest", "compiler-environment-manifest",
        "command-log-manifest", "workload-run-manifest",
        "preparation-event-ledger", "prewarm-event-ledger",
        "cold-reset-ledger", "initial-state-prewarm-snapshot-manifest",
    )
    for kind in declaration_kinds:
        expected_inventory_keys = expected_declaration_keys.get(kind)
        actual_build_blocks: set[tuple[str, str]] = set()
        actual_keys: set[tuple[str, str, str, str]] = set()
        actual_count = 0
        actual_invalid = False
        for record in records_by_kind.get(kind, []):
            build_id, block_id = record.get("build_id"), record.get("block_id")
            actual_count += 1
            if type(build_id) is not str or type(block_id) is not str:
                actual_invalid = True
            else:
                actual_build_blocks.add((build_id, block_id))
                if kind in scoped_declaration_kinds:
                    mode, regime = record.get("mode"), record.get("regime")
                    if type(mode) is not str or type(regime) is not str:
                        actual_invalid = True
                    else:
                        actual_keys.add((build_id, block_id, mode, regime))
        actual_coverage = actual_keys if expected_inventory_keys is not None else actual_build_blocks
        expected_coverage = (expected_inventory_keys if expected_inventory_keys is not None
                             else expected_build_blocks)
        if (actual_invalid or actual_count != len(expected_coverage)
                or actual_coverage != expected_coverage):
            _issue(issues, "upstream_artifact_nonledger_inventory_cardinality_mismatch", kind)

    # Validate and index each of the six source sequences.  Dictionaries are
    # populated only after shape/key typing, so malformed values cannot induce
    # unbounded or exception-producing indexing work.
    indexes: dict[str, dict[tuple[Any, ...], Mapping[str, Any]]] = {}
    for kind in UPSTREAM_LEDGER_KINDS:
        index: dict[tuple[Any, ...], Mapping[str, Any]] = {}
        sequence: list[tuple[Any, ...]] = []
        kind_record_ids: set[str] = set()
        for position, record in enumerate(records_by_kind.get(kind, [])):
            label = f"{kind}:{position}"
            if not _validate_upstream_record_shape(record, kind, issues, label):
                continue
            # Scope is an independent descriptor and must agree with sibling
            # build/TU fields; V9 only compared it to the outer ref.
            scope = record.get("scope")
            if type(scope) is dict:
                if (record.get("experiment_id") != scope.get("experiment_id")
                        or ("build_id" in record and record.get("build_id") != scope.get("build_id"))
                        or ("tu_id" in record and record.get("tu_id") != scope.get("tu_id"))):
                    _issue(issues, "upstream_artifact_record_scope_sibling_mismatch", label)
            record_id = record.get("record_id")
            if type(record_id) is str:
                if record_id in kind_record_ids:
                    _issue(issues, "upstream_artifact_kind_record_id_not_unique", f"{kind}:{record_id}")
                kind_record_ids.add(record_id)
            key = typed_key(record, label)
            if key is None:
                continue
            sequence.append(key)
            if key in index:
                _issue(issues, "upstream_artifact_duplicate_row_key", f"{kind}:{position}")
            else:
                index[key] = record
        indexes[kind] = index
        if sequence != expected_keys:
            _issue(issues, "upstream_artifact_membership_order_or_cardinality_mismatch", kind)

    # Each row has one member of every kind and all identity/link fields agree
    # at that position.  The expected sequence is never read from a join row.
    for position, key in enumerate(expected_keys):
        members: dict[str, Mapping[str, Any]] = {}
        for kind in UPSTREAM_LEDGER_KINDS:
            record = indexes[kind].get(key)
            if record is None:
                _issue(issues, "upstream_artifact_membership_member_missing", f"{kind}:{position}")
            else:
                members[kind] = record
        if len(members) != len(UPSTREAM_LEDGER_KINDS):
            continue
        baseline = members[UPSTREAM_LEDGER_KINDS[0]]
        for kind, record in members.items():
            for field in UPSTREAM_JOIN_FIELDS:
                if record.get(field) != baseline.get(field):
                    _issue(issues, "upstream_artifact_cross_kind_identity_mismatch", f"{kind}:{position}:{field}")
            if isinstance(expected_rows[position], Mapping):
                expected_row = expected_rows[position]
                for field in UPSTREAM_JOIN_FIELDS:
                    if field in expected_row and record.get(field) != expected_row.get(field):
                        _issue(issues, "upstream_artifact_evidence_identity_mismatch", f"{kind}:{position}:{field}")
        join_record = members["join-ledger"]
        source_ids = join_record.get("source_record_ids")
        expected_source_ids = {
            "per_tu": members["per-tu-ledger"].get("record_id"),
            "assignment": members["assignment-ledger"].get("record_id"),
            "input_record": members["input-record-ledger"].get("record_id"),
            "result": members["result-ledger"].get("record_id"),
            "wire": members["wire-ledger"].get("record_id"),
        }
        if source_ids != expected_source_ids:
            _issue(issues, "upstream_artifact_join_source_record_ids_mismatch", str(position))
        for field, value in (("source_count", 5), ("orphan_count", 0),
                             ("duplicate_count", 0), ("dropped_count", 0),
                             ("ordered_position", key[ROW_KEY_FIELDS.index("tu_position") + 1])):
            if join_record.get(field) != value:
                _issue(issues, "upstream_artifact_join_recomputed_value_mismatch", f"{position}:{field}")

    # Every link identifier is a bijection over the complete observation key;
    # a short-ID reuse across scheduler epochs is a conflict, not a match.
    seen_links: dict[tuple[str, str], tuple[Any, ...]] = {}
    for kind in UPSTREAM_LEDGER_KINDS:
        for record in records_by_kind.get(kind, []):
            key = typed_key(record, f"{kind}:link")
            if key is None:
                continue
            identity = tuple(record.get(field) for field in UPSTREAM_JOIN_FIELDS)
            for link in UPSTREAM_LINK_FIELDS:
                value = record.get(link)
                if type(value) is not str:
                    continue
                seen = seen_links.get((link, value))
                if seen is not None and seen != identity:
                    _issue(issues, "upstream_artifact_link_identity_conflict", f"{link}:{value}")
                else:
                    seen_links[(link, value)] = identity

    # Join every setup event to exactly one measured workload interval on the
    # same experiment/run/build/block/mode/regime and clock.
    workload_by_key: dict[tuple[str, ...], Mapping[str, Any]] = {}
    setup_by_id: dict[str, Mapping[str, Any]] = {}
    setup_by_key_kind: dict[tuple[tuple[str, ...], str], list[Mapping[str, Any]]] = {}

    def event_key(record: Mapping[str, Any]) -> tuple[str, ...] | None:
        values = tuple(record.get(field) for field in
                       ("experiment_id", "run_id", "build_id", "block_id", "mode", "regime"))
        return values if all(type(value) is str and value for value in values) else None

    for record in records_by_kind.get("workload-run-manifest", []):
        if not _validate_upstream_record_shape(record, "workload-run-manifest", issues, "workload"):
            continue
        key = event_key(record)
        if key is None:
            _issue(issues, "upstream_artifact_workload_join_key_invalid")
            continue
        if key in workload_by_key:
            _issue(issues, "upstream_artifact_workload_interval_not_unique", ":".join(key))
        else:
            workload_by_key[key] = record

    setup_kinds = ("preparation-event-ledger", "prewarm-event-ledger", "cold-reset-ledger")
    for kind in setup_kinds:
        for position, record in enumerate(records_by_kind.get(kind, [])):
            if not _validate_upstream_record_shape(record, kind, issues, f"{kind}:{position}"):
                continue
            key = event_key(record)
            if key is None:
                _issue(issues, "upstream_artifact_setup_join_key_invalid", f"{kind}:{position}")
                continue
            event_id = record.get("event_id")
            if type(event_id) is str:
                if event_id in setup_by_id:
                    _issue(issues, "upstream_artifact_setup_event_id_not_unique", event_id)
                setup_by_id[event_id] = record
            setup_by_key_kind.setdefault((key, kind), []).append(record)
            workload = workload_by_key.get(key)
            if workload is None:
                _issue(issues, "upstream_artifact_setup_workload_missing", ":".join(key))
                continue
            if record.get("clock_id") != workload.get("clock_id"):
                _issue(issues, "upstream_artifact_setup_clock_mismatch", str(event_id))
            start, end = record.get("start_ns"), record.get("end_ns")
            workload_start = workload.get("start_ns")
            if (type(start) is not int or isinstance(start, bool) or type(end) is not int
                    or isinstance(end, bool) or type(workload_start) is not int
                    or isinstance(workload_start, bool) or end > workload_start):
                _issue(issues, "upstream_artifact_setup_after_measurement", str(event_id))

    for key, workload in workload_by_key.items():
        prep = setup_by_key_kind.get((key, "preparation-event-ledger"), [])
        prewarm = setup_by_key_kind.get((key, "prewarm-event-ledger"), [])
        resets = setup_by_key_kind.get((key, "cold-reset-ledger"), [])
        if len(prep) != 1:
            _issue(issues, "upstream_artifact_preparation_cardinality_invalid", ":".join(key))
        if len(prewarm) != 1:
            _issue(issues, "upstream_artifact_prewarm_cardinality_invalid", ":".join(key))
        regime = key[-1]
        if regime == "cold" and len(resets) != 1:
            _issue(issues, "upstream_artifact_cold_reset_cardinality_invalid", ":".join(key))
        if regime == "warm" and resets:
            _issue(issues, "upstream_artifact_warm_reset_present", ":".join(key))
        for reset in resets:
            if reset.get("regime") != "cold" or reset.get("fresh_c_guid") is not True or reset.get("f_store_wiped") is not True:
                _issue(issues, "upstream_artifact_cold_reset_proof_invalid", str(reset.get("event_id")))

    # Snapshot records are bound to one prewarm event and cannot share a
    # namespace across modes/regimes.  Optional event-side copies, when
    # present, are checked against the snapshot as well.
    namespace_owner: dict[str, tuple[str, str]] = {}
    prewarm_owner_count: dict[str, int] = {}
    for position, snapshot in enumerate(records_by_kind.get("initial-state-prewarm-snapshot-manifest", [])):
        if not _validate_upstream_record_shape(snapshot, "initial-state-prewarm-snapshot-manifest", issues, f"snapshot:{position}"):
            continue
        mode, regime = snapshot.get("mode"), snapshot.get("source_regime")
        if snapshot.get("source_mode") != mode or regime not in REGIMES:
            _issue(issues, "upstream_artifact_snapshot_mode_regime_mismatch", str(snapshot.get("record_id")))
        namespace = snapshot.get("namespace")
        if type(namespace) is str:
            owner = (mode, regime)
            previous = namespace_owner.get(namespace)
            if previous is not None and previous != owner:
                _issue(issues, "upstream_artifact_snapshot_namespace_reused", namespace)
            namespace_owner[namespace] = owner
        event_id = snapshot.get("owning_event_id")
        event = setup_by_id.get(event_id) if type(event_id) is str else None
        if event is None or event.get("event_kind") != "prewarm":
            _issue(issues, "upstream_artifact_snapshot_owner_invalid", str(snapshot.get("record_id")))
            continue
        prewarm_owner_count[event_id] = prewarm_owner_count.get(event_id, 0) + 1
        snapshot_key = event_key(snapshot)
        event_join_key = event_key(event)
        if snapshot_key is None or snapshot_key != event_join_key:
            _issue(issues, "upstream_artifact_snapshot_event_identity_mismatch", str(snapshot.get("record_id")))
        for field in ("mode", "source_mode", "source_regime"):
            if field == "source_mode":
                expected = event.get("mode")
            elif field == "source_regime":
                expected = event.get("regime")
            else:
                expected = event.get(field)
            if snapshot.get(field) != expected:
                _issue(issues, "upstream_artifact_snapshot_event_identity_mismatch", f"{snapshot.get('record_id')}:{field}")
        for field in ("namespace", "c_guid", "f_store_generation", "prewarm_manifest_digest", "state_digest"):
            if field in event and snapshot.get(field) != event.get(field):
                _issue(issues, "upstream_artifact_snapshot_event_state_mismatch", f"{snapshot.get('record_id')}:{field}")
        if "snapshot_id" in event and event.get("snapshot_id") != snapshot.get("record_id"):
            _issue(issues, "upstream_artifact_snapshot_event_state_mismatch", f"{snapshot.get('record_id')}:snapshot_id")
    for event_id in (event_id for event_id, event in setup_by_id.items() if event.get("event_kind") == "prewarm"):
        if prewarm_owner_count.get(event_id, 0) != 1:
            _issue(issues, "upstream_artifact_prewarm_snapshot_cardinality_invalid", event_id)


def _validate_upstream_references(refs: Any, captured: Mapping[str, bytes] | None,
                                  contract: Mapping[str, Any], document: Mapping[str, Any],
                                  issues: list[str]) -> None:
    """Validate closed descriptors and all parsed artifact joins."""
    if type(refs) is not list or not refs:
        _issue(issues, "upstream_artifact_schema_missing_hold")
        return
    if len(refs) > MAX_ARTIFACTS:
        _issue(issues, "artifact_reference_limit_exceeded")
        return
    # Charge the expected-member and join-work budgets from authenticated
    # evidence inventory before opening/parsing any producer bytes.  This is
    # deliberately conservative: an oversized expected sequence is a typed
    # rejection even if a source file would later claim fewer rows.
    expected_rows = document.get("tu_rows") if type(document) is dict else None
    if type(expected_rows) is list:
        if len(expected_rows) > MAX_EXPECTED_TU_MEMBERS:
            _issue(issues, "upstream_artifact_expected_tu_member_limit_exceeded")
            return
        if len(expected_rows) * len(UPSTREAM_LEDGER_KINDS) > MAX_JOIN_WORK:
            _issue(issues, "upstream_artifact_join_work_limit_exceeded")
            return
    if len(refs) * len(UPSTREAM_LEDGER_KINDS) > MAX_JOIN_WORK:
        _issue(issues, "upstream_artifact_join_work_limit_exceeded")
        return
    captured_map = captured if type(captured) is dict else None
    ids: set[str] = set()
    paths: set[str] = set()
    all_records: set[str] = set()
    parsed_records: list[Mapping[str, Any]] = []
    kinds_present: set[str] = set()
    records_by_kind: dict[str, list[Mapping[str, Any]]] = {}
    for ref in refs:
        if type(ref) is not dict or _safe_mapping_keys(ref, "upstream_artifact_ref", issues) != UPSTREAM_REF_FIELDS:
            _issue(issues, "upstream_artifact_reference_schema_invalid")
            continue
        path_value = ref.get("path")
        label = str(path_value if isinstance(path_value, str) else "<invalid>")
        path_valid = _safe_relative_path(path_value)
        if not path_valid:
            _issue(issues, "upstream_artifact_reference_path_invalid", label)
        if not _is_digest(ref.get("sha256")) or type(ref.get("bytes")) is not int or isinstance(ref.get("bytes"), bool) or ref.get("bytes") < 0:
            _issue(issues, "upstream_artifact_reference_descriptor_invalid", label)
        artifact_id = ref.get("artifact_id")
        if type(artifact_id) is not str or not artifact_id or artifact_id in ids:
            _issue(issues, "upstream_artifact_record_identity_invalid", label)
        if type(artifact_id) is str:
            ids.add(artifact_id)
        if path_valid and path_value in paths:
            _issue(issues, "duplicate_artifact_reference", label)
        if path_valid:
            paths.add(path_value)
        if ref.get("schema") != UPSTREAM_SCHEMA:
            _issue(issues, "upstream_artifact_reference_schema_invalid", label)
        kind_value = ref.get("kind")
        if type(kind_value) is not str or kind_value not in UPSTREAM_KINDS:
            _issue(issues, "upstream_artifact_kind_invalid", label)
        if type(ref.get("producer")) is not str or not ref.get("producer"):
            _issue(issues, "upstream_artifact_producer_invalid", label)
        if type(ref.get("experiment_id")) is not str or not ref.get("experiment_id"):
            _issue(issues, "upstream_artifact_experiment_identity_invalid", label)
        if type(ref.get("run_id")) is not str or not ref.get("run_id"):
            _issue(issues, "upstream_artifact_run_identity_invalid", label)
        _validate_upstream_scope(ref.get("scope"), ref, issues, label)
        if ref.get("experiment_id") != contract.get("experiment_id") or ref.get("run_id") != contract.get("run_identity"):
            _issue(issues, "upstream_artifact_contract_scope_mismatch", label)
        if (not path_valid or type(path_value) is not str or captured_map is None
                or path_value not in captured_map):
            _issue(issues, "upstream_artifact_same_open_bytes_missing", label)
            continue
        records = _parse_upstream_bytes(captured_map[path_value], ref, issues)
        # Manifest/event kinds are one-record declarations.  Without this
        # per-reference cardinality gate, a no-row-key artifact could hide an
        # extra semantic record behind an otherwise valid header.
        if (type(ref.get("kind")) is str
                and ref.get("kind") not in UPSTREAM_LEDGER_KINDS
                and len(records) != 1):
            _issue(issues, "upstream_artifact_kind_cardinality_invalid", label)
        if type(ref.get("kind")) is str:
            kinds_present.add(ref.get("kind"))
            records_by_kind.setdefault(ref.get("kind"), []).extend(records)
        for row in records:
            parsed_records.append(row)
            record_id = row.get("record_id")
            if type(record_id) is str and record_id in all_records:
                _issue(issues, "upstream_artifact_global_record_identity_reused", str(record_id))
            if type(record_id) is str:
                all_records.add(record_id)
            for field in ("source_sha256", "binary_sha256", "compiler_env_digest", "workload_manifest_digest"):
                if field in row and row.get(field) != contract.get(field):
                    _issue(issues, "upstream_artifact_contract_digest_mismatch", f"{record_id}:{field}")
    # The preregistered plan is a closed inventory.  Every V8 kind is
    # required; evidence-dependent additions below preserve the explicit
    # event/cold-reset checks for the applicable rows.
    required_kinds: set[str] = set(UPSTREAM_KINDS)
    if isinstance(document.get("tu_rows"), list) and document["tu_rows"]:
        required_kinds.update({"per-tu-ledger", "assignment-ledger", "input-record-ledger",
                               "result-ledger", "wire-ledger", "join-ledger"})
        required_ids = {
            row.get(field) for row in document["tu_rows"] if isinstance(row, Mapping)
            for field in ("assignment_id", "input_record_id", "result_record_id", "ledger_record_id")
            if isinstance(row.get(field), str)
        }
        known_link_ids = {
            record.get(field) for record in parsed_records for field in UPSTREAM_LINK_FIELDS
            if type(record.get(field)) is str
        }
        for record_id in sorted(required_ids - all_records - known_link_ids):
            _issue(issues, "upstream_artifact_evidence_record_unbound", record_id)
    if isinstance(document.get("events"), list) and document["events"]:
        required_kinds.update({"preparation-event-ledger", "prewarm-event-ledger"})
        if any(isinstance(build, Mapping) and build.get("regime") == "cold"
               for build in document.get("builds", [])):
            required_kinds.add("cold-reset-ledger")
    for kind in sorted(required_kinds - kinds_present):
        _issue(issues, "upstream_artifact_required_kind_missing", kind)
    for kind in sorted(kinds_present):
        if not records_by_kind.get(kind):
            _issue(issues, "upstream_artifact_empty_required_kind", kind)
    # Bind any record carrying a build/TU identity to the evidence row with
    # the same complete assignment identity.  Presence of an ID alone is not
    # sufficient: mismatched build/TU/mode/regime values are orphaned joins.
    evidence_by_record: dict[str, Mapping[str, Any]] = {}
    evidence_build_tu: set[tuple[Any, Any]] = set()
    for evidence_row in document.get("tu_rows", []):
        if not isinstance(evidence_row, Mapping):
            continue
        build_value, tu_value = evidence_row.get("build_id"), evidence_row.get("tu_id")
        if type(build_value) is str and type(tu_value) is str:
            evidence_build_tu.add((build_value, tu_value))
        for field in ("assignment_id", "input_record_id", "result_record_id", "ledger_record_id"):
            value = evidence_row.get(field)
            if type(value) is str:
                evidence_by_record[value] = evidence_row
    for record in parsed_records:
        matched: Mapping[str, Any] | None = None
        for field in ("assignment_id", "input_record_id", "result_record_id", "ledger_record_id"):
            value = record.get(field)
            if type(value) is str and value in evidence_by_record:
                matched = evidence_by_record[value]
                break
        if matched is not None:
            for field in ("build_id", "block_id", "tu_id", "mode", "regime"):
                if field in record and field in matched and record[field] != matched[field]:
                    _issue(issues, "upstream_artifact_typed_join_mismatch", f"{record.get('record_id')}:{field}")
        elif (document.get("tu_rows") and record.get("row_key") is not None):
            if (record.get("build_id"), record.get("tu_id")) not in evidence_build_tu:
                _issue(issues, "upstream_artifact_typed_join_orphan", str(record.get("record_id")))
    # Re-check every link against all candidates, rather than selecting the
    # first short-ID match.  This catches a valid assignment paired with a
    # wrong result, InputRecord, wire, or ledger row.
    evidence_candidates: dict[tuple[str, str], list[Mapping[str, Any]]] = {}
    for evidence_row in document.get("tu_rows", []):
        if not isinstance(evidence_row, Mapping):
            continue
        for link in ("assignment_id", "input_record_id", "result_record_id", "wire_id", "ledger_record_id"):
            value = evidence_row.get(link)
            if type(value) is str:
                evidence_candidates.setdefault((link, value), []).append(evidence_row)
    identity_fields = ("scenario_id", "scheduler_epoch", "assignment_nonce", "logical_attempt",
                       "selected_f", "f_store_generation", "endpoint_generation", "c_guid",
                       "tu_position", "build_id", "block_id", "tu_id", "mode", "regime", "corpus_id", "method_id")
    for record in parsed_records:
        for link in ("assignment_id", "input_record_id", "result_record_id", "wire_id", "ledger_record_id"):
            value = record.get(link)
            candidates = evidence_candidates.get((link, value), []) if type(value) is str else []
            if len(candidates) > 1:
                _issue(issues, "upstream_artifact_join_link_not_unique", f"{link}:{value}")
            for candidate in candidates:
                for field in identity_fields + ("assignment_id", "input_record_id", "result_record_id", "wire_id", "ledger_record_id"):
                    if field in record and field in candidate and record.get(field) != candidate.get(field):
                        _issue(issues, "upstream_artifact_full_join_mismatch", f"{record.get('record_id')}:{field}")
    # Every join-bearing identity is indexed once by the complete immutable
    # observation key.  A reused short ID with any differing dimension is a
    # conflict, never a reason to select the first matching row.
    if len(parsed_records) * 16 > MAX_JOIN_WORK:
        _issue(issues, "upstream_artifact_join_work_limit_exceeded")
    for field in ("assignment_id", "input_record_id", "result_record_id", "wire_id", "ledger_record_id"):
        seen_join: dict[str, tuple[Any, ...]] = {}
        for record in parsed_records:
            ident = record.get(field)
            if type(ident) is not str:
                continue
            key = tuple(record.get(name) for name in (
                "experiment_id", "run_id", "scenario_id", "scheduler_epoch",
                "assignment_nonce", "logical_attempt", "selected_f", "f_store_generation",
                "endpoint_generation", "c_guid", "tu_position", "build_id", "block_id",
                "tu_id", "mode", "regime", "corpus_id", "method_id"))
            previous = seen_join.get(ident)
            if previous is not None and previous != key:
                _issue(issues, "upstream_artifact_full_join_identity_conflict", f"{field}:{ident}")
            else:
                seen_join[ident] = key
    # V10 performs the complete ordered membership and boundary joins from
    # the typed records below.  The former V9 set-only check is intentionally
    # not an authority and is retained only as history in the comment.
    _validate_v10_membership(records_by_kind, parsed_records, refs, document, contract, issues)
    plan = contract.get("artifact_plan") if isinstance(contract, Mapping) else None
    if isinstance(plan, Mapping) and plan.get("refs") != [ref.get("path") for ref in refs if isinstance(ref, Mapping)]:
        _issue(issues, "prereg_artifact_plan_refs_mismatch")


def _authenticated_root(root: Any, refs: Any, issues: list[str],
                        captured: dict[str, bytes] | None = None) -> None:
    """Authenticate a complete tree using descriptor-relative, no-follow IO.

    ``os.walk`` is deliberately not used here.  It drops symlinked directory
    entries when ``followlinks=False`` and therefore cannot distinguish an
    empty symlink directory from an absent directory.  Every directory entry
    is inspected with ``lstat`` and every accepted regular file is opened once
    with ``O_NOFOLLOW`` from its parent directory descriptor.  The descriptor
    identity is checked before and after the read so a path replacement cannot
    make the parsed bytes differ from the bytes that were authenticated.
    """
    if not isinstance(root, (str, bytes, os.PathLike)):
        _issue(issues, "artifact_root_invalid")
        return
    try:
        base = Path(root)
    except (TypeError, ValueError, OSError):
        _issue(issues, "artifact_root_invalid")
        return
    if not base.is_dir() or base.is_symlink():
        _issue(issues, "artifact_root_invalid")
        return
    # Bind the root and every parent directory before walking.  This catches a
    # symlinked parent and later replacement of the path used to reach root.
    parent_ids: list[tuple[str, tuple[int, int]]] = []
    cursor = base
    while True:
        try:
            st = os.lstat(cursor)
            if stat.S_ISLNK(st.st_mode):
                _issue(issues, "artifact_parent_symlink", str(cursor))
            parent_ids.append((str(cursor), (st.st_dev, st.st_ino)))
        except OSError:
            _issue(issues, "artifact_parent_unreadable", str(cursor))
            return
        if cursor.parent == cursor:
            break
        cursor = cursor.parent
    flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        root_fd = os.open(base, flags)
    except OSError:
        _issue(issues, "artifact_root_open_failed")
        return
    try:
        root_st = os.fstat(root_fd)
        if (root_st.st_dev, root_st.st_ino) != parent_ids[0][1]:
            _issue(issues, "artifact_root_replaced")
            return
        expected: dict[str, tuple[str, int]] = {}
        if not isinstance(refs, list) or len(refs) > MAX_ARTIFACTS:
            _issue(issues, "artifact_reference_manifest_invalid")
            return
        total_declared_bytes = 0
        for ref in refs:
            if not isinstance(ref, Mapping):
                _issue(issues, "artifact_reference_schema_invalid")
                continue
            keys = _safe_mapping_keys(ref, "artifact_reference", issues)
            if keys not in ({"path", "sha256", "bytes"}, UPSTREAM_REF_FIELDS):
                _issue(issues, "artifact_reference_schema_invalid")
                continue
            path, digest, size = ref["path"], ref["sha256"], ref["bytes"]
            if (not _safe_relative_path(path) or not _is_digest(digest)
                    or not isinstance(size, int) or isinstance(size, bool) or size < 0):
                _issue(issues, "artifact_reference_invalid")
                continue
            if size > MAX_ARTIFACT_BYTES_PER_FILE:
                _issue(issues, "artifact_file_bytes_limit_exceeded", path)
            total_declared_bytes += size
            if path in expected:
                _issue(issues, "duplicate_artifact_reference", path)
            expected[path] = (digest, size)
        if total_declared_bytes > MAX_ARTIFACT_BYTES_TOTAL:
            _issue(issues, "artifact_total_bytes_limit_exceeded")
        if issues:
            return

        actual_files: set[str] = set()
        actual_dirs: set[str] = set()
        seen_inodes: set[tuple[int, int]] = set()

        def walk(dir_fd: int, prefix: str) -> None:
            try:
                entries = list(os.scandir(dir_fd))
            except OSError:
                _issue(issues, "artifact_unreadable", prefix or ".")
                return
            for entry in entries:
                rel = f"{prefix}/{entry.name}" if prefix else entry.name
                try:
                    st = os.lstat(entry.name, dir_fd=dir_fd)
                except OSError:
                    _issue(issues, "artifact_unreadable", rel)
                    continue
                mode = st.st_mode
                if stat.S_ISLNK(mode):
                    _issue(issues, "artifact_symlink", rel)
                    continue
                if stat.S_ISDIR(mode):
                    actual_dirs.add(rel)
                    child_flags = flags
                    try:
                        child_fd = os.open(entry.name, child_flags, dir_fd=dir_fd)
                        child_st = os.fstat(child_fd)
                        if (child_st.st_dev, child_st.st_ino) != (st.st_dev, st.st_ino):
                            _issue(issues, "artifact_directory_replaced", rel)
                        walk(child_fd, rel)
                        os.close(child_fd)
                    except OSError:
                        _issue(issues, "artifact_directory_unreadable", rel)
                    continue
                actual_files.add(rel)
                inode = (st.st_dev, st.st_ino)
                if inode in seen_inodes or st.st_nlink != 1:
                    _issue(issues, "artifact_inode_alias", rel)
                seen_inodes.add(inode)
                if not stat.S_ISREG(mode):
                    _issue(issues, "artifact_nonregular", rel)
                    continue
                if rel not in expected:
                    _issue(issues, "unreferenced_artifact", rel)
                    continue
                digest, size = expected[rel]
                if st.st_size > MAX_ARTIFACT_BYTES_PER_FILE:
                    _issue(issues, "artifact_file_bytes_limit_exceeded", rel)
                    continue
                fd: int | None = None
                try:
                    fd = os.open(entry.name, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0), dir_fd=dir_fd)
                    before = os.fstat(fd)
                    if (before.st_dev, before.st_ino) != inode:
                        _issue(issues, "artifact_replaced_before_read", rel)
                    h = hashlib.sha256()
                    total = 0
                    read_chunks: list[bytes] | None = [] if captured is not None else None
                    while True:
                        chunk = os.read(fd, 1 << 20)
                        if not chunk:
                            break
                        h.update(chunk)
                        total += len(chunk)
                        if read_chunks is not None:
                            read_chunks.append(chunk)
                    if captured is not None and read_chunks is not None:
                        captured[rel] = b"".join(read_chunks)
                    after = os.fstat(fd)
                    if ((after.st_dev, after.st_ino) != inode or after.st_size != before.st_size
                            or total != size or h.hexdigest() != digest):
                        _issue(issues, "artifact_digest_mismatch", rel)
                except OSError:
                    _issue(issues, "artifact_unreadable", rel)
                finally:
                    if fd is not None:
                        try:
                            os.close(fd)
                        except OSError:
                            _issue(issues, "artifact_close_failed", rel)

        walk(root_fd, "")
        for rel in expected:
            if rel not in actual_files:
                _issue(issues, "missing_artifact", rel)
        # An unreferenced directory is meaningful even when it has no files;
        # this is the case os.walk previously erased from the inventory.
        for rel in actual_dirs:
            if not any(path.startswith(rel + "/") for path in expected):
                _issue(issues, "unreferenced_directory", rel)
        for path, identity in parent_ids:
            try:
                after = os.lstat(path)
                if (after.st_dev, after.st_ino) != identity:
                    _issue(issues, "artifact_parent_replaced", path)
            except OSError:
                _issue(issues, "artifact_parent_unreadable", path)
        final_root = os.fstat(root_fd)
        if (final_root.st_dev, final_root.st_ino) != (root_st.st_dev, root_st.st_ino):
            _issue(issues, "artifact_root_replaced")
    finally:
        os.close(root_fd)


def sha256_json(value: Any) -> str:
    return hashlib.sha256(canonical_json(value)).hexdigest()


def _issue(issues: list[str], code: str, detail: str = "") -> None:
    issues.append(code if not detail else f"{code}: {detail}")


def _is_digest(value: Any) -> bool:
    return type(value) is str and len(value) == 64 and set(value) <= HEX64


def _finite_positive(value: Any) -> bool:
    return type(value) in (int, float) and math.isfinite(value) and value > 0


def _finite_nonnegative(value: Any) -> bool:
    return type(value) in (int, float) and math.isfinite(value) and value >= 0


def _as_list(value: Any) -> list[Any]:
    return value if isinstance(value, list) else []


def _safe_mapping_keys(value: Any, code: str, issues: list[str]) -> set[str] | None:
    """Return string mapping keys only after validating their primitive type."""
    if not isinstance(value, Mapping):
        _issue(issues, code + "_not_object")
        return None
    try:
        keys = list(value.keys())
    except (TypeError, ValueError, AttributeError, RuntimeError):
        _issue(issues, code + "_keys_invalid")
        return None
    if any(type(key) is not str for key in keys):
        _issue(issues, code + "_key_type_invalid")
        return None
    return set(keys)


def _string_list(value: Any) -> bool:
    return isinstance(value, list) and all(type(item) is str and bool(item) for item in value)


def _nonnegative_int(value: Any) -> bool:
    return type(value) is int and value >= 0


def _contract_payload(contract: Mapping[str, Any]) -> dict[str, Any]:
    """Return the exact object covered by ``contract_digest``.

    ``contract_digest`` is intentionally outside this object.  A producer may
    include a human-readable ``contract_digest`` in the contract too; it is
    ignored so there is only one unambiguous preimage.
    """
    return {k: v for k, v in contract.items() if k not in {"contract_digest", "preregistration_digest"}}


def _validate_contract(document: Mapping[str, Any], issues: list[str]) -> Mapping[str, Any] | None:
    contract = document.get("contract")
    if not isinstance(contract, Mapping):
        _issue(issues, "missing_contract")
        return None
    if document.get("schema") not in (None, SCHEMA):
        _issue(issues, "schema_mismatch")
    if contract.get("schema") not in (None, SCHEMA):
        _issue(issues, "contract_schema_mismatch")
    if contract.get("immutable") is not True:
        _issue(issues, "contract_not_immutable")
    digest = document.get("contract_digest", contract.get("contract_digest"))
    if not _is_digest(digest):
        _issue(issues, "missing_contract_digest")
    elif digest != sha256_json(_contract_payload(contract)):
        _issue(issues, "contract_digest_mismatch")

    required_digest_fields = {
        "source_sha256": "source_hash_missing",
        "binary_sha256": "binary_hash_missing",
        "compiler_env_digest": "compiler_env_digest_missing",
        "workload_manifest_digest": "workload_digest_missing",
        "wire_definition_sha256": "wire_definition_hash_missing",
        "block_plan_sha256": "block_plan_hash_missing",
    }
    for field, code in required_digest_fields.items():
        if not _is_digest(contract.get(field)):
            _issue(issues, code)

    wire = contract.get("wire_definition")
    if not isinstance(wire, Mapping):
        _issue(issues, "wire_definition_missing")
    else:
        for field in ("direction", "layer", "denominator"):
            if not isinstance(wire.get(field), str) or not wire[field]:
                _issue(issues, f"wire_{field}_missing")
        if wire.get("environment_transfer_included") is not False:
            _issue(issues, "wire_environment_transfer_not_excluded")
        if contract.get("wire_definition_sha256") != sha256_json(wire):
            _issue(issues, "wire_definition_hash_mismatch")

    plan_hash = contract.get("block_plan_sha256")
    plan = contract.get("block_plan") or {
        "mapping": {"A": "cache", "B": "legacy"},
        "orders": ["AB", "BA"],
        "required_blocks_per_regime": contract.get("required_blocks_per_regime"),
    }
    if _is_digest(plan_hash) and plan_hash != sha256_json(plan):
        _issue(issues, "block_plan_hash_mismatch")

    if contract.get("modes") != list(MODES):
        _issue(issues, "mode_mapping_not_canonical")
    if contract.get("regimes") != list(REGIMES):
        _issue(issues, "regime_mapping_not_canonical")
    if contract.get("bootstrap_unit") != "whole_block":
        _issue(issues, "bootstrap_unit_not_whole_block")
    if contract.get("ratio_definition") != "cache_over_legacy":
        _issue(issues, "ratio_definition_not_canonical")
    if contract.get("estimator") != "geometric_mean_log_ratio":
        _issue(issues, "estimator_not_canonical")
    if contract.get("bound_method") != "whole_block_bootstrap":
        _issue(issues, "bound_method_not_canonical")
    bootstrap = contract.get("bootstrap")
    if not isinstance(bootstrap, Mapping):
        _issue(issues, "bootstrap_preregistration_missing")
    else:
        seed = bootstrap.get("seed")
        reps = bootstrap.get("replicates")
        if not isinstance(seed, int) or isinstance(seed, bool) or seed < 0:
            _issue(issues, "bootstrap_seed_invalid")
        if not isinstance(reps, int) or isinstance(reps, bool) or reps < 1:
            _issue(issues, "bootstrap_replicates_invalid")
        if bootstrap.get("quantile_convention") != "linear":
            _issue(issues, "bootstrap_quantile_convention_invalid")
        if bootstrap.get("confidence_level") != 0.95:
            _issue(issues, "bootstrap_confidence_invalid")
        if bootstrap.get("implementation") != VERIFIER_IMPLEMENTATION:
            _issue(issues, "bootstrap_implementation_identity_invalid")
        if not _is_digest(bootstrap.get("code_sha256")):
            _issue(issues, "bootstrap_code_hash_invalid")

    prewarm = contract.get("prewarm")
    if not isinstance(prewarm, Mapping):
        _issue(issues, "prewarm_contract_missing")
    else:
        for field in ("manifest_digest", "content_digest"):
            if not _is_digest(prewarm.get(field)):
                _issue(issues, f"prewarm_{field}_invalid")
        if not _is_digest(prewarm.get("initial_state_digest")):
            _issue(issues, "prewarm_initial_digest_invalid")
    if not isinstance(contract.get("required_blocks_per_regime"), int) or contract["required_blocks_per_regime"] < 4:
        _issue(issues, "required_blocks_invalid")
    elif contract["required_blocks_per_regime"] % 2:
        _issue(issues, "required_blocks_not_even")
    return contract


def _quantile_linear(values: Sequence[float], probability: float) -> float:
    if not values:
        raise ContractError("cannot take a quantile of an empty sequence")
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    position = (len(ordered) - 1) * probability
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def _bootstrap_ratios(ratios: Sequence[float], seed: int, replicates: int) -> tuple[float, float]:
    """Bootstrap geometric means using a SHA-256 counter PRNG.

    The only sampled objects are complete block ratios.  Hashing a counter,
    rather than using process-global random state, makes the output stable
    under parallel callers and makes the seed part of the auditable contract.
    """
    n = len(ratios)
    estimates: list[float] = []
    for replicate in range(replicates):
        log_sum = 0.0
        for draw in range(n):
            token = f"{seed}:{replicate}:{draw}".encode("ascii")
            index = int.from_bytes(hashlib.sha256(token).digest()[:8], "big") % n
            log_sum += math.log(ratios[index])
        estimates.append(math.exp(log_sum / n))
    return _quantile_linear(estimates, 0.05), _quantile_linear(estimates, 0.95)


def _decimal_quantile(values: Sequence[Decimal], probability: Decimal) -> Decimal:
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    position = Decimal(len(ordered) - 1) * probability
    lower, upper = int(position), int(position.to_integral_value(rounding="ROUND_CEILING"))
    if lower == upper:
        return ordered[lower]
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - Decimal(lower))


def _decimal_bootstrap(ratios: Sequence[Decimal], seed: int, replicates: int) -> tuple[Decimal, Decimal]:
    with localcontext() as context:
        context.prec = 80
        estimates: list[Decimal] = []
        for replicate in range(replicates):
            log_sum = Decimal(0)
            for draw in range(len(ratios)):
                token = f"{seed}:{replicate}:{draw}".encode("ascii")
                index = int.from_bytes(hashlib.sha256(token).digest()[:8], "big") % len(ratios)
                log_sum += ratios[index].ln()
            estimates.append((log_sum / Decimal(len(ratios))).exp())
        return _decimal_quantile(estimates, Decimal("0.05")), _decimal_quantile(estimates, Decimal("0.95"))


def _build_identity(build: Mapping[str, Any]) -> str:
    build_id = build.get("build_id")
    return build_id if type(build_id) is str and build_id else "<invalid-build-id>"


def _validate_build(
    build: Mapping[str, Any],
    contract: Mapping[str, Any],
    issues: list[str],
    seen_ids: set[str],
    mode_namespaces: dict[str, set[str]],
    cold_identities: dict[str, set[str]],
) -> None:
    build_id = _build_identity(build)
    if build_id in seen_ids:
        _issue(issues, "duplicate_build_id", build_id)
    seen_ids.add(build_id)
    regime = build.get("regime")
    mode = build.get("mode")
    if type(regime) is not str or regime not in REGIMES:
        _issue(issues, "invalid_build_regime", build_id)
    if type(mode) is not str or mode not in MODES:
        _issue(issues, "invalid_build_mode", build_id)
    status = build.get("status")
    if type(status) is not str or status not in {"complete", "failed", "censored"}:
        _issue(issues, "invalid_build_status", build_id)
    if not _finite_positive(build.get("wall_seconds")):
        _issue(issues, "wall_time_not_positive", build_id)

    namespace = build.get("namespace")
    if not isinstance(namespace, Mapping):
        _issue(issues, "namespace_evidence_missing", build_id)
    else:
        namespace_id = namespace.get("namespace_id")
        if type(namespace_id) is not str or not namespace_id:
            _issue(issues, "namespace_id_missing", build_id)
        elif type(mode) is str and mode in MODES:
            mode_namespaces.setdefault(mode, set()).add(namespace_id)
        if namespace.get("mode_private") is not True:
            _issue(issues, "namespace_not_private", build_id)
        if type(namespace.get("c_guid")) is not str or not namespace.get("c_guid"):
            _issue(issues, "c_guid_missing", build_id)
        elif type(mode) is str and mode in MODES:
            mode_namespaces.setdefault(f"c_guid:{mode}", set()).add(namespace["c_guid"])
            if regime == "cold":
                cold_identities.setdefault(f"c_guid:{mode}", set()).add(namespace["c_guid"])
        if type(namespace.get("f_store")) is not str or not namespace.get("f_store"):
            _issue(issues, "f_store_missing", build_id)
        elif type(mode) is str and mode in MODES:
            mode_namespaces.setdefault(f"f_store:{mode}", set()).add(namespace["f_store"])
            if regime == "cold":
                cold_identities.setdefault(f"f_store:{mode}", set()).add(namespace["f_store"])

    measurement = build.get("measurement")
    if not isinstance(measurement, Mapping):
        _issue(issues, "measurement_evidence_missing", build_id)
    else:
        for field in ("preparation_excluded", "reset_outside_measurement", "prewarm_outside_measurement"):
            if measurement.get(field) is not True:
                _issue(issues, f"{field}_invalid", build_id)
        for field in ("contains_preparation", "contains_reset", "contains_prewarm", "preparation_inside_interval"):
            if measurement.get(field) is True:
                _issue(issues, f"{field}_detected", build_id)
        if measurement.get("work") != "full_build":
            _issue(issues, "measurement_not_full_build", build_id)
        if "start_ns" in measurement and "end_ns" in measurement:
            if (type(measurement["start_ns"]) is not int
                    or type(measurement["end_ns"]) is not int
                    or measurement["end_ns"] <= measurement["start_ns"]):
                _issue(issues, "measurement_interval_invalid", build_id)

    prewarm = build.get("prewarm")
    expected = contract.get("prewarm")
    if not isinstance(prewarm, Mapping) or not isinstance(expected, Mapping):
        _issue(issues, "prewarm_evidence_missing", build_id)
    else:
        if prewarm.get("manifest_digest") != expected.get("manifest_digest"):
            _issue(issues, "prewarm_manifest_mismatch", build_id)
        if prewarm.get("content_digest") != expected.get("content_digest"):
            _issue(issues, "prewarm_content_mismatch", build_id)
        if prewarm.get("initial_state_digest") != expected.get("initial_state_digest"):
            _issue(issues, "prewarm_initial_digest_mismatch", build_id)
        if not _is_digest(prewarm.get("initial_state_digest")):
            _issue(issues, "prewarm_initial_digest_missing", build_id)
        if type(prewarm.get("snapshot_identity")) is not str or not prewarm.get("snapshot_identity"):
            _issue(issues, "prewarm_snapshot_identity_missing", build_id)
        if prewarm.get("mode") != mode:
            _issue(issues, "prewarm_mode_mismatch", build_id)
        if (type(mode) is str and mode in MODES
                and type(prewarm.get("snapshot_identity")) is str
                and prewarm.get("snapshot_identity")):
            mode_namespaces.setdefault(f"snapshot:{mode}", set()).add(prewarm["snapshot_identity"])

    join = build.get("join")
    if not isinstance(join, Mapping):
        _issue(issues, "join_evidence_missing", build_id)
    else:
        if join.get("bijection") is not True:
            _issue(issues, "join_not_bijective", build_id)
        for field in ("orphan_count", "duplicate_count", "dropped_count"):
            if join.get(field) != 0:
                _issue(issues, f"join_{field}_nonzero", build_id)
        if type(join.get("evidence_ref")) is not str or not join.get("evidence_ref"):
            _issue(issues, "join_evidence_ref_missing", build_id)
        row_key = join.get("row_key")
        required = {"run_id", "scheduler_epoch", "wire_id", "wire_nonce", "attempt_id", "selected_f_identity", "f_store_generation", "c_guid", "tu_seq"}
        row_key_valid = isinstance(row_key, list) and all(type(key) is str for key in row_key)
        if row_key_valid:
            row_key_valid = set(row_key) == required
        if not row_key_valid:
            _issue(issues, "join_row_key_not_assignment_strength", build_id)

    if regime == "cold":
        reset = build.get("reset")
        if not isinstance(reset, Mapping):
            _issue(issues, "cold_reset_evidence_missing", build_id)
        else:
            for field in ("fresh_c_guid", "f_store_wiped", "outside_measurement"):
                if reset.get(field) is not True:
                    _issue(issues, f"cold_reset_{field}_invalid", build_id)
            if type(reset.get("evidence_ref")) is not str or not reset.get("evidence_ref"):
                _issue(issues, "cold_reset_evidence_ref_missing", build_id)
    elif regime == "warm":
        reset = build.get("reset")
        if not isinstance(reset, Mapping) or reset.get("performed") is True:
            _issue(issues, "warm_reset_or_cold_state", build_id)

    # Any row-level status must remain present in the source; a successful
    # measurement cannot hide a failed/censored companion row.
    if status != "complete" and not isinstance(build.get("failure"), Mapping) and not isinstance(build.get("censor"), Mapping):
        _issue(issues, "failure_or_censor_detail_missing", build_id)


def _validate_blocks(
    blocks: Sequence[Mapping[str, Any]],
    builds: Mapping[str, Mapping[str, Any]],
    contract: Mapping[str, Any],
    issues: list[str],
) -> tuple[dict[str, list[float]], int]:
    ratios: dict[str, list[float]] = {regime: [] for regime in REGIMES}
    complete_block_count = 0
    seen_blocks: set[str] = set()
    by_regime: dict[str, list[Mapping[str, Any]]] = {regime: [] for regime in REGIMES}
    for block in blocks:
        if not isinstance(block, Mapping):
            _issue(issues, "block_not_object")
            continue
        block_id = block.get("block_id")
        if not isinstance(block_id, str) or not block_id:
            _issue(issues, "block_id_missing")
            continue
        if block_id in seen_blocks:
            _issue(issues, "duplicate_block_id", block_id)
        seen_blocks.add(block_id)
        regime = block.get("regime")
        order = block.get("order")
        if type(regime) is not str or regime not in REGIMES:
            _issue(issues, "invalid_block_regime", block_id)
            continue
        if type(order) is not str or order not in ORDERS:
            _issue(issues, "invalid_block_order", block_id)
        by_regime[regime].append(block)
        ids = block.get("build_ids")
        cache_id, legacy_id = block.get("cache_build_id"), block.get("legacy_build_id")
        ids_are_strings = isinstance(ids, list) and all(type(item) is str for item in ids)
        ids_are_pair = (
            isinstance(ids, list)
            and len(ids) == 2
            and ids_are_strings
            and type(cache_id) is str
            and type(legacy_id) is str
            and set(ids) == {cache_id, legacy_id}
        )
        if not ids_are_pair:
            _issue(issues, "block_pair_mapping_invalid", block_id)
            continue
        cache = builds.get(cache_id)
        legacy = builds.get(legacy_id)
        if cache is None or legacy is None:
            _issue(issues, "block_build_reference_missing", block_id)
            continue
        if cache.get("mode") != "cache" or legacy.get("mode") != "legacy":
            _issue(issues, "block_mode_mapping_invalid", block_id)
        if cache.get("regime") != regime or legacy.get("regime") != regime:
            _issue(issues, "block_regime_mismatch", block_id)
        expected_modes = ["cache", "legacy"] if order == "AB" else ["legacy", "cache"]
        sequence = block.get("sequence_build_ids")
        sequence_valid = isinstance(sequence, list) and len(sequence) == 2 and all(type(item) is str for item in sequence)
        if sequence != ([cache_id, legacy_id] if order == "AB" else [legacy_id, cache_id]):
            _issue(issues, "block_order_mapping_invalid", block_id)
        if block.get("mapping") != {"A": "cache", "B": "legacy"}:
            _issue(issues, "block_mapping_not_canonical", block_id)
        if sequence_valid:
            for position, build_id in enumerate(sequence):
                if builds.get(build_id, {}).get("mode") != expected_modes[position]:
                    _issue(issues, "block_sequence_mode_invalid", block_id)
                build = builds.get(build_id, {})
                if build.get("block_id") != block_id:
                    _issue(issues, "build_block_identity_mismatch", block_id)
                if build.get("order") != order:
                    _issue(issues, "build_order_identity_mismatch", block_id)
                if build.get("sequence_in_block") != position:
                    _issue(issues, "build_sequence_identity_mismatch", block_id)
        if block.get("complete") is not (cache.get("status") == "complete" and legacy.get("status") == "complete"):
            _issue(issues, "block_complete_flag_mismatch", block_id)
        cache_join = cache.get("join", {})
        legacy_join = legacy.get("join", {})
        if (block.get("join_evidence_ref") != (cache_join.get("evidence_ref") if isinstance(cache_join, Mapping) else None)
                or block.get("join_evidence_ref") != (legacy_join.get("evidence_ref") if isinstance(legacy_join, Mapping) else None)):
            _issue(issues, "block_join_reference_mismatch", block_id)
        # Same fixed prewarm content and initial state per pair; identities
        # remain private and are checked after all builds are visited.
        cp = cache.get("prewarm", {})
        lp = legacy.get("prewarm", {})
        if not isinstance(cp, Mapping):
            cp = {}
        if not isinstance(lp, Mapping):
            lp = {}
        if cp.get("manifest_digest") != lp.get("manifest_digest") or cp.get("content_digest") != lp.get("content_digest") or cp.get("initial_state_digest") != lp.get("initial_state_digest"):
            _issue(issues, "block_initial_state_mismatch", block_id)
        if cp.get("snapshot_identity") == lp.get("snapshot_identity"):
            _issue(issues, "cross_mode_snapshot_identity", block_id)

        if cache.get("status") == "complete" and legacy.get("status") == "complete" and _finite_positive(cache.get("wall_seconds")) and _finite_positive(legacy.get("wall_seconds")):
            ratio = float(cache["wall_seconds"]) / float(legacy["wall_seconds"])
            if not math.isfinite(ratio) or ratio <= 0:
                _issue(issues, "ratio_invalid", block_id)
            else:
                ratios[regime].append(ratio)
                complete_block_count += 1
    for regime in REGIMES:
        cells = by_regime[regime]
        required = contract.get("required_blocks_per_regime", 4)
        if type(required) is not int or required < 1:
            required = 4
        if len(cells) < required:
            _issue(issues, "fewer_than_required_blocks", regime)
        if len(cells) % 2:
            _issue(issues, "block_count_not_even", regime)
        orders = [b.get("order") for b in cells]
        if orders and any(orders[i] == orders[i - 1] for i in range(1, len(orders))):
            _issue(issues, "block_orders_not_alternating", regime)
        if orders.count("AB") != orders.count("BA"):
            _issue(issues, "block_orders_not_balanced", regime)
        if len(ratios[regime]) < required:
            _issue(issues, "fewer_than_required_complete_blocks", regime)
    return ratios, complete_block_count


def _strict_evidence_gate(document: Mapping[str, Any], contract: Mapping[str, Any], issues: list[str]) -> None:
    """Apply the non-negotiable V5 evidence gate before arithmetic."""
    allowed = {"schema", "contract", "contract_digest", "preregistration_digest", "experiment_id",
               "blocks", "builds", "tu_rows", "events", "artifact_refs", "evidence_refs", "recomputed"}
    document_keys = _safe_mapping_keys(document, "evidence", issues)
    if document_keys is not None:
        unknown = document_keys - allowed
        if unknown:
            _issue(issues, "unknown_top_level_field", ",".join(sorted(unknown)))
    if not isinstance(document.get("preregistration_digest"), str):
        _issue(issues, "independent_preregistration_binding_missing")
    builds = document.get("builds")
    blocks = document.get("blocks")
    if not isinstance(builds, list) or len(builds) > MAX_BUILDS:
        _issue(issues, "build_inventory_invalid")
    if not isinstance(blocks, list) or len(blocks) > MAX_BLOCKS:
        _issue(issues, "block_inventory_invalid")
    refs = document.get("artifact_refs")
    if not isinstance(refs, list):
        _issue(issues, "structured_artifact_refs_missing")
    if not isinstance(document.get("tu_rows"), list):
        _issue(issues, "raw_tu_rows_missing")
    if not isinstance(document.get("events"), list):
        _issue(issues, "event_ledger_missing")
    expected_ids: set[str] = set()
    plan = contract.get("block_plan")
    if isinstance(plan, list):
        for item in plan:
            if (isinstance(item, Mapping) and type(item.get("block_id")) is str
                    and isinstance(item.get("build_ids"), list)
                    and all(type(value) is str for value in item["build_ids"])):
                expected_ids.update(item["build_ids"])
    if expected_ids and isinstance(builds, list):
        actual_ids = [b.get("build_id") for b in builds
                      if isinstance(b, Mapping) and type(b.get("build_id")) is str]
        if len(actual_ids) != len([b for b in builds if isinstance(b, Mapping)]) or set(actual_ids) != expected_ids:
            _issue(issues, "build_plan_bijection_failed")
    if isinstance(builds, list):
        for build in builds:
            if not isinstance(build, Mapping):
                continue
            measurement = build.get("measurement")
            if not isinstance(measurement, Mapping):
                continue
            for key in ("start_ns", "end_ns"):
                if not isinstance(measurement.get(key), int) or isinstance(measurement.get(key), bool):
                    _issue(issues, "monotonic_timestamp_missing", str(build.get("build_id")))
            if isinstance(measurement.get("start_ns"), int) and isinstance(measurement.get("end_ns"), int):
                if measurement["end_ns"] <= measurement["start_ns"]:
                    _issue(issues, "monotonic_interval_invalid", str(build.get("build_id")))
                derived = (measurement["end_ns"] - measurement["start_ns"]) / 1_000_000_000
                if "wall_seconds" in build and build["wall_seconds"] != derived:
                    _issue(issues, "wall_interval_mismatch", str(build.get("build_id")))
                elif isinstance(build, dict):
                    build["wall_seconds"] = derived


def verify_document(document: Mapping[str, Any], *, strict: bool = False) -> Verification:
    """Reject the unauthenticated legacy evidence API.

    Arithmetic is available only through the private helper for unit-level
    tests on already trusted in-memory observations.  It is not an evidence
    gate and cannot produce an authenticated result.
    """
    return Verification(False, "INCONCLUSIVE", ("independent_preregistration_required",), {}, 0, 0)


def _collect_statistics_observations(document: Mapping[str, Any], *, strict: bool = False) -> ObservationCollection:
    """Validate a document and collect ratios, but never construct a verdict.

    This compatibility collector exists for diagnostics and validation tests.
    It may consume a document-shaped mapping, but its result contains only
    issues and arithmetic observations.  The only code path that constructs a
    GREEN/RED result is the independently-bound ``verify_preregistered``
    protocol below.
    """
    issues: list[str] = []
    if not isinstance(document, Mapping):
        return ObservationCollection(("document_not_object",), {}, (), (), 0, 0)
    contract = _validate_contract(document, issues)
    if contract is None:
        return ObservationCollection(tuple(issues), {}, (), (), 0, 0)
    if strict:
        _strict_evidence_gate(document, contract, issues)
    raw_builds = _as_list(document.get("builds"))
    raw_blocks = _as_list(document.get("blocks"))
    evidence_refs = document.get("evidence_refs")
    if strict:
        # V5 uses the preregistered structured artifact inventory; the old
        # self-listed string manifest is deliberately not an authority.
        evidence_ref_set: set[str] = set()
    elif not isinstance(evidence_refs, list) or not evidence_refs or any(not isinstance(ref, str) or not ref for ref in evidence_refs):
        _issue(issues, "evidence_reference_manifest_missing")
        evidence_ref_set = set()
    else:
        evidence_ref_set = set(evidence_refs)
        if len(evidence_ref_set) != len(evidence_refs):
            _issue(issues, "duplicate_evidence_reference")
    builds: dict[str, Mapping[str, Any]] = {}
    seen_ids: set[str] = set()
    mode_namespaces: dict[str, set[str]] = {}
    cold_identities: dict[str, set[str]] = {}
    for raw in raw_builds:
        if not isinstance(raw, Mapping):
            _issue(issues, "build_not_object")
            continue
        _validate_build(raw, contract, issues, seen_ids, mode_namespaces, cold_identities)
        join = raw.get("join")
        if (not strict and isinstance(join, Mapping)
                and type(join.get("evidence_ref")) is str
                and join.get("evidence_ref") not in evidence_ref_set):
            _issue(issues, "join_evidence_reference_unlisted", _build_identity(raw))
        reset = raw.get("reset")
        if (not strict and isinstance(reset, Mapping)
                and reset.get("evidence_ref") is not None
                and type(reset.get("evidence_ref")) is str
                and reset.get("evidence_ref") not in evidence_ref_set):
            _issue(issues, "reset_evidence_reference_unlisted", _build_identity(raw))
        builds[_build_identity(raw)] = raw
    if mode_namespaces.get("cache", set()) & mode_namespaces.get("legacy", set()):
        _issue(issues, "cross_mode_namespace_identity")
    if mode_namespaces.get("snapshot:cache", set()) & mode_namespaces.get("snapshot:legacy", set()):
        _issue(issues, "cross_mode_snapshot_identity")
    for identity in ("c_guid", "f_store"):
        if mode_namespaces.get(f"{identity}:cache", set()) & mode_namespaces.get(f"{identity}:legacy", set()):
            _issue(issues, f"cross_mode_{identity}_identity")
        for mode in MODES:
            cold_builds = [
                b for b in raw_builds
                if isinstance(b, Mapping) and b.get("regime") == "cold" and b.get("mode") == mode
            ]
            values = [
                b.get("namespace", {}).get(identity)
                for b in cold_builds
                if isinstance(b.get("namespace"), Mapping)
                and isinstance(b.get("namespace", {}).get(identity), str)
            ]
            if len(values) != len(set(values)):
                _issue(issues, f"cold_{identity}_reused", mode)
    ratios, complete_blocks = _validate_blocks(raw_blocks, builds, contract, issues)
    for block in raw_blocks:
        if (not strict and isinstance(block, Mapping)
                and type(block.get("join_evidence_ref")) is str
                and block.get("join_evidence_ref") not in evidence_ref_set):
            _issue(issues, "block_join_evidence_reference_unlisted", str(block.get("block_id", "<missing>")))

    # Detect accidental or deliberate attempts to introduce TU observations
    # into the block-level inference unit.
    for field in ("tu_samples", "tu_observations", "sample_rows"):
        if field in document and document[field]:
            _issue(issues, "tu_row_resampling_detected", field)
    # Keep the collector's output to primitive observations.  It does not
    # invoke the arithmetic layer on document-derived values; the authenticated
    # caller invokes that layer only after its independent raw gate passes.
    regime_results: dict[str, dict[str, Any]] = {
        regime: {"complete_blocks": len(ratios[regime]), "ratios": list(ratios[regime])}
        for regime in REGIMES
    }

    # Any issue, including a retained failed/censored build, is retained for
    # the authenticated caller.  This collector deliberately does not turn
    # those observations into GREEN/RED.
    has_failure = any(isinstance(b, Mapping) and type(b.get("status")) is str
                      and b.get("status") in {"failed", "censored"} for b in raw_builds)
    if has_failure:
        _issue(issues, "failed_or_censored_build_retained")
    return ObservationCollection(
        tuple(issues), regime_results, tuple(ratios["cold"]), tuple(ratios["warm"]),
        len(raw_builds), len(raw_blocks),
    )


def _compute_statistics_arithmetic(
    cold_ratios: tuple[float, ...],
    warm_ratios: tuple[float, ...],
    seed: int,
    replicates: int,
) -> dict[str, dict[str, Any]]:
    """Compute arithmetic values from authenticated primitive observations.

    The function intentionally accepts no document, mapping, text, or
    decision-producing flag.  ``verify_preregistered`` supplies these tuples
    only after its independent preregistration, artifact, raw-row, and join
    gates pass.  The return value contains arithmetic fields only; GREEN/RED
    is constructed in that authenticated protocol, not here.
    """
    if type(cold_ratios) is not tuple or type(warm_ratios) is not tuple:
        raise ContractError("arithmetic ratios must be immutable tuples")
    if type(seed) is not int or isinstance(seed, bool) or not 0 <= seed <= MAX_SEED:
        raise ContractError("arithmetic seed is invalid")
    if (type(replicates) is not int or isinstance(replicates, bool)
            or not 1 <= replicates <= MAX_BOOTSTRAP_REPLICATES):
        raise ContractError("arithmetic replicate count is invalid")
    ratio_sets = {"cold": cold_ratios, "warm": warm_ratios}
    result: dict[str, dict[str, Any]] = {}
    for regime, values in ratio_sets.items():
        for value in values:
            if type(value) not in (int, float) or isinstance(value, bool):
                raise ContractError(f"{regime} ratio is not a primitive number")
            if not math.isfinite(value) or value <= 0:
                raise ContractError(f"{regime} ratio is not finite-positive")
        entry: dict[str, Any] = {
            "complete_blocks": len(values),
            "ratios": list(values),
            "estimator": None,
            "geometric_mean": None,
            "lower_95": None,
            "upper_95": None,
            "lower_bound": None,
            "upper_bound": None,
            "statistic": "exp(mean(log(R_b)))",
            "bootstrap_unit": "whole_block",
        }
        if values:
            estimator = math.exp(sum(math.log(float(value)) for value in values) / len(values))
            lower, upper = _bootstrap_ratios(values, seed, replicates)
            entry.update({
                "estimator": estimator,
                "geometric_mean": estimator,
                "lower_95": lower,
                "upper_95": upper,
                "lower_bound": lower,
                "upper_bound": upper,
            })
        result[regime] = entry
    return result


def compute_statistics(document: Mapping[str, Any], *, strict: bool = False) -> Verification:
    """Fail closed: unauthenticated mappings cannot become GREEN/RED.

    Arithmetic remains available only to internal tests through
    ``_compute_statistics_arithmetic``.  Authenticated production decisions
    are made exclusively by ``verify_preregistered`` after its independent
    preregistration, source identity, artifact inventory, and raw gate pass.
    """
    return Verification(False, "INCONCLUSIVE", ("unauthenticated_statistics_api",), {}, 0, 0)


def parse_evidence(text: str) -> dict[str, Any]:
    """Parse canonical JSON or the assembled canonical JSONL stream."""
    if type(text) is not str:
        raise ContractError("evidence text must be exact str")
    raw = text.encode("utf-8")
    try:
        value = parse_canonical_bytes(raw)
    except ContractError as json_error:
        if not is_canonical_jsonl(raw):
            raise json_error
        value = parse_canonical_bytes(raw, jsonl=True)
    if not isinstance(value, Mapping):
        raise ContractError("evidence root must be a JSON object")
    return value if type(value) is dict else dict(value)


def verify_text(text: str) -> dict[str, Any]:
    """Reject text-only verification because it has no independent contract."""
    return Verification(False, "INCONCLUSIVE", ("independent_preregistration_required",), {}, 0, 0).as_dict()


def _strict_object(value: Any, keys: set[str], code: str, issues: list[str]) -> bool:
    actual = _safe_mapping_keys(value, code, issues)
    if actual is None:
        return False
    unknown = actual - keys
    missing = keys - actual
    if unknown:
        _issue(issues, code + "_unknown_fields", ",".join(sorted(unknown)))
    if missing:
        _issue(issues, code + "_missing_fields", ",".join(sorted(missing)))
    return not unknown and not missing


def _strict_optional_object(value: Any, keys: set[str], code: str, issues: list[str]) -> bool:
    """Validate an optional nested object without allowing open extensions."""
    if value is None:
        return True
    return _strict_object(value, keys, code, issues)


def _strict_closed_object(value: Any, required: set[str], optional: set[str], code: str, issues: list[str]) -> bool:
    """Require a closed key universe while allowing explicit optionals."""
    actual = _safe_mapping_keys(value, code, issues)
    if actual is None:
        return False
    unknown = actual - required - optional
    missing = required - actual
    if unknown:
        _issue(issues, code + "_unknown_fields", ",".join(sorted(unknown)))
    if missing:
        _issue(issues, code + "_missing_fields", ",".join(sorted(missing)))
    return not unknown and not missing


def _validate_preregistration_schema(preregistration: Mapping[str, Any], issues: list[str]) -> None:
    """Close the preregistration schema before any digest is trusted.

    Hashing an object with an extensible nested map authenticates bytes, not
    the meaning of fields added after this verifier was reviewed.  This gate
    therefore enumerates every accepted object and validates the exact keys
    and primitive types needed by the statistical and provenance joins.
    """
    prereg_keys = _safe_mapping_keys(preregistration, "preregistration", issues)
    if prereg_keys is not None and prereg_keys != {"schema", "contract", "artifact_refs"}:
        _issue(issues, "preregistration_schema_not_closed")
    if preregistration.get("schema") != SCHEMA:
        _issue(issues, "preregistration_schema_invalid")
    refs = preregistration.get("artifact_refs")
    if not isinstance(refs, list) or not refs:
        _issue(issues, "preregistration_artifact_refs_missing")
    else:
        ref_paths: list[str] = []
        for ref in refs:
            if _strict_object(ref, set(UPSTREAM_REF_FIELDS), "prereg_artifact_ref", issues):
                if ref.get("schema") != UPSTREAM_SCHEMA:
                    _issue(issues, "prereg_artifact_ref_schema_invalid")
                if type(ref.get("kind")) is not str or ref.get("kind") not in UPSTREAM_KINDS:
                    _issue(issues, "prereg_artifact_ref_kind_invalid")
                if not _safe_relative_path(ref.get("path")) or not _is_digest(ref.get("sha256")):
                    _issue(issues, "prereg_artifact_ref_value_invalid")
                else:
                    ref_paths.append(ref["path"])
                if not _nonnegative_int(ref.get("bytes")):
                    _issue(issues, "prereg_artifact_ref_size_invalid")
                _validate_upstream_scope(ref.get("scope"), ref, issues, str(ref.get("path")))
        if len(ref_paths) != len(set(ref_paths)):
            _issue(issues, "duplicate_preregistered_artifact_path")
    contract = preregistration.get("contract")
    if not isinstance(contract, Mapping):
        _issue(issues, "prereg_contract_not_object")
        return
    contract_keys = {
        "schema", "immutable", "source_sha256", "binary_sha256", "compiler_env_digest",
        "workload_manifest_digest", "wire_definition_sha256", "block_plan_sha256",
        "wire_definition", "modes", "regimes", "bootstrap_unit", "ratio_definition",
        "estimator", "bound_method", "bootstrap", "prewarm", "required_blocks_per_regime",
        "experiment_id", "run_identity", "failure_policy", "clock_identity", "max_duration_ns",
        "block_plan", "tu_plan", "tu_plan_sha256", "code_identity", "artifact_plan",
    }
    _strict_object(contract, contract_keys, "prereg_contract", issues)
    required_contract_keys = contract_keys
    contract_keys_actual = _safe_mapping_keys(contract, "prereg_contract", issues)
    if contract_keys_actual is not None:
        missing_contract = required_contract_keys - contract_keys_actual
        if missing_contract:
            _issue(issues, "prereg_contract_missing_fields", ",".join(sorted(missing_contract)))
    if contract.get("schema") != SCHEMA:
        _issue(issues, "prereg_contract_schema_invalid")
    if contract.get("immutable") is not True:
        _issue(issues, "prereg_contract_immutable_invalid")
    for field in ("experiment_id", "run_identity", "clock_identity"):
        if not isinstance(contract.get(field), str) or not contract[field]:
            _issue(issues, "prereg_contract_identity_invalid", field)
    scalar_digest_fields = {
        "source_sha256", "binary_sha256", "compiler_env_digest", "workload_manifest_digest",
        "wire_definition_sha256", "block_plan_sha256", "tu_plan_sha256",
    }
    for field in scalar_digest_fields:
        if field in contract and not _is_digest(contract[field]):
            _issue(issues, "prereg_contract_digest_field_invalid", field)
    if "max_duration_ns" in contract and (not _nonnegative_int(contract["max_duration_ns"])
                                          or contract["max_duration_ns"] <= 0):
        _issue(issues, "prereg_max_duration_invalid")
    if isinstance(contract.get("tu_plan"), list) and _is_digest(contract.get("tu_plan_sha256")):
        try:
            plan_digest = sha256_json(contract["tu_plan"])
        except (TypeError, ValueError, OverflowError):
            plan_digest = ""
            _issue(issues, "prereg_tu_plan_hash_input_invalid")
        if contract["tu_plan_sha256"] != plan_digest:
            _issue(issues, "prereg_tu_plan_hash_mismatch")
    if "failure_policy" in contract:
        policy = contract["failure_policy"]
        if isinstance(policy, Mapping):
            _strict_object(policy, {"failed_statuses", "censored_statuses", "decision"}, "prereg_failure_policy", issues)
            if isinstance(policy.get("failed_statuses"), list) and any(type(v) is not str for v in policy["failed_statuses"]):
                _issue(issues, "prereg_failure_policy_type_invalid")
            if isinstance(policy.get("censored_statuses"), list) and any(type(v) is not str for v in policy["censored_statuses"]):
                _issue(issues, "prereg_failure_policy_type_invalid")
            if policy.get("decision") != "fail-closed":
                _issue(issues, "prereg_failure_policy_decision_invalid")
        elif policy != "fail-closed":
            _issue(issues, "prereg_failure_policy_invalid")
    if "code_identity" in contract:
        code = contract["code_identity"]
        if _strict_object(code, {"source_sha256", "binary_sha256", "compiler_env_digest", "verifier_sha256"}, "prereg_code_identity", issues):
            for field in code:
                if not _is_digest(code[field]):
                    _issue(issues, "prereg_code_identity_digest_invalid", field)
    if "artifact_plan" in contract:
        plan = contract["artifact_plan"]
        if _strict_object(plan, {"refs", "identity_fields"}, "prereg_artifact_plan", issues):
            if not _string_list(plan.get("refs")):
                _issue(issues, "prereg_artifact_plan_refs_invalid")
            elif isinstance(refs, list):
                actual_paths = [r.get("path") for r in refs
                                if isinstance(r, Mapping) and _safe_relative_path(r.get("path"))]
                if len(actual_paths) != len(refs) or plan["refs"] != actual_paths:
                    _issue(issues, "prereg_artifact_plan_refs_mismatch")
            if plan.get("identity_fields") != ["path", "sha256", "bytes", "artifact_id", "schema", "kind",
                                                "producer", "experiment_id", "run_id", "scope"]:
                _issue(issues, "prereg_artifact_plan_identity_invalid")
    wire = contract.get("wire_definition")
    wire_ok = _strict_object(wire, {"direction", "layer", "denominator", "environment_transfer_included"}, "prereg_wire_definition", issues)
    if wire_ok and isinstance(wire, Mapping):
        if any(type(wire.get(field)) is not str or not wire.get(field)
               for field in ("direction", "layer", "denominator")):
            _issue(issues, "prereg_wire_value_invalid")
        if type(wire.get("environment_transfer_included")) is not bool:
            _issue(issues, "prereg_wire_value_invalid")
    prewarm = contract.get("prewarm")
    prewarm_ok = _strict_object(prewarm, {"manifest_digest", "content_digest", "initial_state_digest"}, "prereg_prewarm", issues)
    if prewarm_ok and isinstance(prewarm, Mapping):
        if any(not _is_digest(prewarm.get(field))
               for field in ("manifest_digest", "content_digest", "initial_state_digest")):
            _issue(issues, "prereg_prewarm_value_invalid")
    bootstrap = contract.get("bootstrap")
    bootstrap_ok = _strict_object(bootstrap, {"seed", "replicates", "confidence_level", "quantile_convention", "implementation", "code_sha256"}, "prereg_bootstrap", issues)
    if bootstrap_ok and isinstance(bootstrap, Mapping):
        if (type(bootstrap.get("seed")) is not int or bootstrap.get("seed", -1) < 0
                or type(bootstrap.get("replicates")) is not int or bootstrap.get("replicates", 0) < 1
                or type(bootstrap.get("confidence_level")) not in (int, float)
                or type(bootstrap.get("quantile_convention")) is not str
                or type(bootstrap.get("implementation")) is not str
                or not _is_digest(bootstrap.get("code_sha256"))):
            _issue(issues, "prereg_bootstrap_value_invalid")
    blocks = contract.get("block_plan")
    if not isinstance(blocks, list) or not blocks:
        _issue(issues, "exact_block_plan_missing")
    else:
        block_ids: list[str] = []
        for row in blocks:
            if _strict_object(row, {"block_id", "build_ids", "regime", "order"}, "prereg_block_plan", issues):
                if type(row.get("block_id")) is not str or not row["block_id"]:
                    _issue(issues, "prereg_block_id_invalid")
                else:
                    block_ids.append(row["block_id"])
                if (type(row.get("regime")) is not str or row.get("regime") not in REGIMES
                        or type(row.get("order")) is not str or row.get("order") not in ORDERS):
                    _issue(issues, "prereg_block_identity_invalid")
                build_ids = row.get("build_ids")
                if (not isinstance(build_ids, list) or len(build_ids) != 2
                        or any(type(value) is not str or not value for value in build_ids)
                        or (isinstance(build_ids, list)
                            and all(type(value) is str for value in build_ids)
                            and len(set(build_ids)) != 2)):
                    _issue(issues, "prereg_block_build_ids_invalid")
        if len(block_ids) != len(set(block_ids)):
            _issue(issues, "duplicate_preregistered_block_id")
    # A per-build ordered TU plan is mandatory for an authenticated decision.
    tu_plan = contract.get("tu_plan")
    if not isinstance(tu_plan, list) or not tu_plan:
        _issue(issues, "exact_tu_plan_missing")
    else:
        plan_keys = {
            "build_id", "block_id", "regime", "mode", "tu_count", "corpus_id", "tu_ids",
            "workload_digest", "compiler_digest", "input_digest", "reference_digest",
            "assignment_ids", "input_record_ids", "result_record_ids", "ledger_record_ids",
        }
        seen_builds: set[str] = set()
        identity_sets: dict[str, set[str]] = {
            "assignment_ids": set(), "input_record_ids": set(),
            "result_record_ids": set(), "ledger_record_ids": set(),
        }
        for row in tu_plan:
            if not _strict_object(row, plan_keys, "prereg_tu_plan", issues):
                continue
            build_id = row.get("build_id")
            build_label = build_id if type(build_id) is str else "<invalid-build-id>"
            if type(build_id) is not str or not build_id:
                _issue(issues, "prereg_tu_plan_build_id_invalid", build_label)
            elif build_id in seen_builds:
                _issue(issues, "duplicate_preregistered_tu_plan", str(build_id))
            else:
                seen_builds.add(build_id)
            tu_count = row.get("tu_count")
            if type(tu_count) is not int or tu_count < 1 or tu_count > MAX_TUS:
                _issue(issues, "prereg_tu_count_invalid", build_label)
            tu_ids = row.get("tu_ids")
            valid_tu_ids = (isinstance(tu_ids, list) and bool(tu_ids)
                            and all(type(value) is str and bool(value) for value in tu_ids))
            if (not valid_tu_ids
                    or (valid_tu_ids and len(set(tu_ids)) != len(tu_ids))):
                _issue(issues, "prereg_tu_ids_invalid", build_label)
            elif type(tu_count) is int and tu_count != len(tu_ids):
                _issue(issues, "prereg_tu_count_mismatch", build_label)
            for identity_field in ("assignment_ids", "input_record_ids", "result_record_ids", "ledger_record_ids"):
                identities = row.get(identity_field)
                valid_identities = (isinstance(identities, list)
                                    and all(type(value) is str and bool(value) for value in identities))
                if not valid_identities or (type(tu_count) is int and len(identities) != tu_count):
                    _issue(issues, "prereg_tu_record_identity_invalid", f"{build_label}:{identity_field}")
                elif valid_identities:
                    for identity in identities:
                        if identity in identity_sets[identity_field]:
                            _issue(issues, "duplicate_preregistered_tu_record_identity", f"{identity_field}:{identity}")
                        identity_sets[identity_field].add(identity)
            if (type(row.get("block_id")) is not str or not row.get("block_id")
                    or type(row.get("regime")) is not str or row.get("regime") not in REGIMES
                    or type(row.get("mode")) is not str or row.get("mode") not in MODES
                    or type(row.get("corpus_id")) is not str or not row.get("corpus_id")):
                _issue(issues, "prereg_tu_plan_identity_invalid", build_label)
            for field in ("workload_digest", "compiler_digest", "input_digest", "reference_digest"):
                if not _is_digest(row.get(field)):
                    _issue(issues, "prereg_tu_plan_digest_invalid", f"{build_label}:{field}")


def _strict_raw_gate(document: Mapping[str, Any], contract: Mapping[str, Any], issues: list[str],
                     captured: Mapping[str, bytes] | None = None) -> None:
    """Require typed raw evidence before arithmetic can be reached.

    Upstream files are accepted only through the closed V10 descriptor and the
    bytes captured by the authenticated root.  Omission or malformed schema is
    a typed inconclusive outcome, never an arithmetic input.
    """
    # Keep this private gate safe when exercised directly by functional tests;
    # the authenticated core performs the same check before any root traversal.
    preflight_start = len(issues)
    _preflight_limits({"contract": contract, "artifact_refs": document.get("artifact_refs")}, document, issues)
    if len(issues) != preflight_start:
        return
    top = {"schema", "contract", "contract_digest", "preregistration_digest", "experiment_id",
           "blocks", "builds", "tu_rows", "events", "artifact_refs", "recomputed"}
    document_keys = _safe_mapping_keys(document, "evidence", issues)
    if document_keys is not None:
        unknown = document_keys - top
        if unknown:
            _issue(issues, "unknown_top_level_field", ",".join(sorted(unknown)))
    if not isinstance(document.get("tu_rows"), list) or not document["tu_rows"]:
        _issue(issues, "raw_tu_evidence_not_implemented_hold")
    if not isinstance(document.get("events"), list) or not document["events"]:
        _issue(issues, "typed_event_ledger_not_implemented_hold")
    refs = document.get("artifact_refs")
    if not isinstance(refs, list) or not refs:
        _issue(issues, "typed_artifact_root_not_implemented_hold")
    elif len(refs) > MAX_ARTIFACTS:
        _issue(issues, "artifact_reference_limit_exceeded")
    else:
        _validate_upstream_references(refs, captured, contract, document, issues)
    recomputed = document.get("recomputed")
    if recomputed is not None:
        if _strict_object(recomputed, {"statistics_sha256"}, "recomputed", issues):
            if not _is_digest(recomputed.get("statistics_sha256")):
                _issue(issues, "recomputed_statistics_digest_invalid")

    build_rows = document.get("builds") if isinstance(document.get("builds"), list) else []
    block_rows = document.get("blocks") if isinstance(document.get("blocks"), list) else []
    tu_rows = document.get("tu_rows") if isinstance(document.get("tu_rows"), list) else []
    event_rows = document.get("events") if isinstance(document.get("events"), list) else []
    ref_rows = refs if isinstance(refs, list) else []

    build_keys = {"build_id", "block_id", "order", "sequence_in_block", "regime", "mode", "status",
                  "wall_seconds", "namespace", "measurement", "prewarm", "join", "reset", "failure", "censor"}
    block_keys = {"block_id", "regime", "order", "mapping", "build_ids", "sequence_build_ids",
                  "cache_build_id", "legacy_build_id", "complete", "join_evidence_ref"}
    ns_keys = {"namespace_id", "mode_private", "c_guid", "f_store"}
    measurement_keys = {"work", "preparation_excluded", "reset_outside_measurement", "prewarm_outside_measurement",
                        "contains_preparation", "contains_reset", "contains_prewarm", "preparation_inside_interval",
                        "start_ns", "end_ns"}
    prewarm_keys = {"manifest_digest", "content_digest", "initial_state_digest", "snapshot_identity", "mode"}
    join_keys = {"bijection", "orphan_count", "duplicate_count", "dropped_count", "evidence_ref", "row_key"}
    reset_keys = {"fresh_c_guid", "f_store_wiped", "outside_measurement", "evidence_ref", "performed"}
    failure_keys = {"reason"}
    censor_keys = {"reason"}

    def require_string(value: Any, code: str, detail: str = "") -> bool:
        valid = type(value) is str and bool(value)
        if not valid:
            _issue(issues, code, detail)
        return valid

    def require_int(value: Any, code: str, detail: str = "", *, nonnegative: bool = False) -> bool:
        valid = type(value) is int and (not nonnegative or value >= 0)
        if not valid:
            _issue(issues, code, detail)
        return valid

    def validate_nested_build(build: Mapping[str, Any]) -> None:
        build_label = build.get("build_id") if type(build.get("build_id")) is str else "<invalid-build-id>"
        for field in ("build_id", "block_id", "order", "regime", "mode", "status"):
            require_string(build.get(field), f"build_{field}_type_invalid", str(build_label))
        if not _finite_positive(build.get("wall_seconds")):
            _issue(issues, "build_wall_seconds_type_invalid", str(build_label))
        namespace = build.get("namespace")
        if isinstance(namespace, Mapping):
            require_string(namespace.get("namespace_id"), "namespace_id_type_invalid", str(build_label))
            if type(namespace.get("mode_private")) is not bool:
                _issue(issues, "namespace_mode_private_type_invalid", str(build_label))
            require_string(namespace.get("c_guid"), "namespace_c_guid_type_invalid", str(build_label))
            require_string(namespace.get("f_store"), "namespace_f_store_type_invalid", str(build_label))
        measurement = build.get("measurement")
        if isinstance(measurement, Mapping):
            if type(measurement.get("work")) is not str or not measurement.get("work"):
                _issue(issues, "measurement_work_type_invalid", str(build_label))
            for field in ("preparation_excluded", "reset_outside_measurement", "prewarm_outside_measurement",
                          "contains_preparation", "contains_reset", "contains_prewarm",
                          "preparation_inside_interval"):
                if type(measurement.get(field)) is not bool:
                    _issue(issues, "measurement_flag_type_invalid", f"{build_label}:{field}")
            require_int(measurement.get("start_ns"), "measurement_start_type_invalid", str(build_label))
            require_int(measurement.get("end_ns"), "measurement_end_type_invalid", str(build_label))
        prewarm = build.get("prewarm")
        if isinstance(prewarm, Mapping):
            for field in ("manifest_digest", "content_digest", "initial_state_digest"):
                if not _is_digest(prewarm.get(field)):
                    _issue(issues, "prewarm_digest_type_invalid", f"{build_label}:{field}")
            require_string(prewarm.get("snapshot_identity"), "prewarm_snapshot_identity_type_invalid", str(build_label))
            require_string(prewarm.get("mode"), "prewarm_mode_type_invalid", str(build_label))
        join = build.get("join")
        if isinstance(join, Mapping):
            if type(join.get("bijection")) is not bool:
                _issue(issues, "join_bijection_type_invalid", str(build_label))
            for field in ("orphan_count", "duplicate_count", "dropped_count"):
                require_int(join.get(field), "join_count_type_invalid", f"{build_label}:{field}", nonnegative=True)
            require_string(join.get("evidence_ref"), "join_evidence_ref_type_invalid", str(build_label))
            row_key = join.get("row_key")
            if not isinstance(row_key, list) or any(type(item) is not str for item in row_key):
                _issue(issues, "join_row_key_type_invalid", str(build_label))
        reset = build.get("reset")
        if isinstance(reset, Mapping):
            for field in ("fresh_c_guid", "f_store_wiped", "outside_measurement"):
                if field in reset and type(reset.get(field)) is not bool:
                    _issue(issues, "reset_flag_type_invalid", f"{build_label}:{field}")
            if "performed" in reset and type(reset.get("performed")) is not bool:
                _issue(issues, "reset_performed_type_invalid", str(build_label))
            if "evidence_ref" in reset:
                require_string(reset.get("evidence_ref"), "reset_evidence_ref_type_invalid", str(build_label))

    def validate_nested_block(block: Mapping[str, Any]) -> None:
        block_label = block.get("block_id") if type(block.get("block_id")) is str else "<invalid-block-id>"
        for field in ("block_id", "regime", "order", "cache_build_id", "legacy_build_id", "join_evidence_ref"):
            require_string(block.get(field), f"block_{field}_type_invalid", str(block_label))
        if block.get("mapping") != {"A": "cache", "B": "legacy"}:
            _issue(issues, "block_mapping_type_invalid", str(block_label))
        for field in ("build_ids", "sequence_build_ids"):
            if not isinstance(block.get(field), list) or any(type(item) is not str for item in block.get(field, [])):
                _issue(issues, "block_build_id_list_type_invalid", f"{block_label}:{field}")
        if type(block.get("complete")) is not bool:
            _issue(issues, "block_complete_type_invalid", str(block_label))

    def validate_nested_tu(row: Mapping[str, Any]) -> bool:
        tu_label = row.get("tu_id") if type(row.get("tu_id")) is str else "<invalid-tu-id>"
        valid = True
        for field in ("build_id", "corpus_id", "tu_id", "argv_digest", "cwd_digest", "env_digest",
                      "input_digest", "reference_digest", "compiler_digest", "object_digest",
                      "result_digest", "status", "assignment_id", "input_record_id",
                      "result_record_id", "ledger_record_id"):
            value = row.get(field)
            if field.endswith("_digest"):
                if not _is_digest(value):
                    _issue(issues, "tu_digest_type_invalid", f"{tu_label}:{field}")
                    valid = False
            else:
                if not require_string(value, "tu_string_field_type_invalid", f"{tu_label}:{field}"):
                    valid = False
        if not require_int(row.get("order"), "tu_order_type_invalid", str(tu_label), nonnegative=True):
            valid = False
        if not require_int(row.get("bytes_c_to_f"), "tu_byte_ledger_invalid", str(tu_label), nonnegative=True):
            valid = False
        if not require_int(row.get("bytes_f_to_c"), "tu_byte_ledger_invalid", str(tu_label), nonnegative=True):
            valid = False
        identity = row.get("join_identity")
        identity_ok = isinstance(identity, Mapping) and _safe_mapping_keys(identity, "tu_assignment", issues) == assignment_keys
        if not identity_ok:
            _issue(issues, "tu_assignment_identity_invalid", str(tu_label))
            valid = False
        elif isinstance(identity, Mapping):
            for field in ("run_id", "wire_id", "selected_f_identity", "c_guid"):
                if not require_string(identity.get(field), "tu_assignment_string_type_invalid", f"{tu_label}:{field}"):
                    valid = False
            for field in ("scheduler_epoch", "wire_nonce", "attempt_id", "f_store_generation", "tu_seq"):
                if not require_int(identity.get(field), "tu_assignment_int_type_invalid", f"{tu_label}:{field}", nonnegative=True):
                    valid = False
        if "row_key" in row or all(field in row for field in ROW_KEY_FIELDS):
            for field in ROW_KEY_FIELDS:
                value = row.get(field)
                if field in _UPSTREAM_INTEGER_FIELDS:
                    if not require_int(value, "tu_row_key_field_type_invalid", f"{tu_label}:{field}", nonnegative=True):
                        valid = False
                elif field == "mode":
                    if type(value) is not str or value not in MODES:
                        _issue(issues, "tu_row_key_field_type_invalid", f"{tu_label}:{field}")
                        valid = False
                elif field == "regime":
                    if type(value) is not str or value not in REGIMES:
                        _issue(issues, "tu_row_key_field_type_invalid", f"{tu_label}:{field}")
                        valid = False
                elif not require_string(value, "tu_row_key_field_type_invalid", f"{tu_label}:{field}"):
                    valid = False
            if type(row.get("row_key")) is not list or row.get("row_key") != _recompute_upstream_row_key(row):
                _issue(issues, "tu_row_key_invalid", str(tu_label))
                valid = False
        if type(row.get("status")) is str and row.get("status") not in {"complete", "failed", "censored"}:
            _issue(issues, "tu_status_invalid", str(tu_label))
            valid = False
        for optional in ("failure", "censor"):
            detail = row.get(optional)
            if detail is not None and isinstance(detail, Mapping):
                if _safe_mapping_keys(detail, f"tu_{optional}", issues) == {"reason"}:
                    if not require_string(detail.get("reason"), "tu_failure_detail_type_invalid", str(tu_label)):
                        valid = False
        return valid

    def validate_nested_event(event: Mapping[str, Any]) -> None:
        event_label = event.get("event_id") if type(event.get("event_id")) is str else "<invalid-event-id>"
        require_string(event.get("event_id"), "event_id_type_invalid", str(event_label))
        require_string(event.get("build_id"), "event_build_id_type_invalid", str(event_label))
        require_string(event.get("kind"), "event_kind_type_invalid", str(event_label))
        require_int(event.get("start_ns"), "event_start_type_invalid", str(event_label))
        require_int(event.get("end_ns"), "event_end_type_invalid", str(event_label))
        require_string(event.get("artifact_ref"), "event_artifact_ref_type_invalid", str(event_label))

    if isinstance(document.get("builds"), list):
        for build in build_rows:
            if _strict_closed_object(build, build_keys - {"failure", "censor"}, {"failure", "censor"}, "build", issues):
                _strict_object(build.get("namespace"), ns_keys, "namespace", issues)
                _strict_object(build.get("measurement"), measurement_keys, "measurement", issues)
                _strict_object(build.get("prewarm"), prewarm_keys, "prewarm", issues)
                _strict_object(build.get("join"), join_keys, "join", issues)
                # Reset has two deliberately closed, regime-specific shapes:
                # cold runs prove a wipe outside measurement; warm runs prove
                # that no reset was performed.  Requiring one union shape
                # would make every legitimate warm record invalid.
                if build.get("regime") == "cold":
                    _strict_object(build.get("reset"),
                                   {"fresh_c_guid", "f_store_wiped", "outside_measurement", "evidence_ref"},
                                   "reset", issues)
                elif build.get("regime") == "warm":
                    _strict_object(build.get("reset"), {"performed"}, "reset", issues)
                else:
                    _strict_object(build.get("reset"), reset_keys, "reset", issues)
                _strict_optional_object(build.get("failure"), failure_keys, "failure", issues)
                _strict_optional_object(build.get("censor"), censor_keys, "censor", issues)
                validate_nested_build(build)
    if isinstance(document.get("blocks"), list):
        for block in block_rows:
            if _strict_object(block, block_keys, "block", issues):
                validate_nested_block(block)
    # The exact S4 row contract is an upstream dependency.  Accept only the
    # frozen field set; malformed or merely descriptive rows cannot pass.
    tu_keys = {"build_id", "corpus_id", "tu_id", "order", "argv_digest", "cwd_digest", "env_digest",
               "input_digest", "reference_digest", "compiler_digest", "join_identity", "object_digest",
               "result_digest", "status", "failure", "censor", "bytes_c_to_f", "bytes_f_to_c",
               "assignment_id", "input_record_id", "result_record_id", "ledger_record_id"}
    tu_v10_optional_keys = set(ROW_KEY_FIELDS) | {"row_key"}
    tu_required_keys = tu_keys - {"failure", "censor"}
    tu_optional_keys = {"failure", "censor"} | tu_v10_optional_keys
    event_keys = {"event_id", "build_id", "kind", "start_ns", "end_ns", "artifact_ref"}
    assignment_keys = {"run_id", "scheduler_epoch", "wire_id", "wire_nonce", "attempt_id",
                       "selected_f_identity", "f_store_generation", "c_guid", "tu_seq"}
    if isinstance(document.get("tu_rows"), list):
        for row in tu_rows:
            if _strict_closed_object(row, tu_required_keys, tu_optional_keys, "tu_row", issues):
                _strict_object(row.get("join_identity"), assignment_keys, "tu_assignment", issues)
                _strict_optional_object(row.get("failure"), {"reason"}, "tu_failure", issues)
                _strict_optional_object(row.get("censor"), {"reason"}, "tu_censor", issues)
    raw_build_id_values = [b.get("build_id") for b in build_rows
                           if isinstance(b, Mapping)]
    valid_build_id_values = all(type(value) is str and bool(value) for value in raw_build_id_values)
    if not valid_build_id_values:
        _issue(issues, "build_id_type_invalid")
    build_ids: set[str] = set(raw_build_id_values) if valid_build_id_values else set()
    build_by_id = {build.get("build_id"): build for build in build_rows
                   if isinstance(build, Mapping) and type(build.get("build_id")) is str}
    tu_seen: set[tuple[Any, ...]] = set()
    tu_builds: set[str] = set()
    assignment_seen: set[str] = set()
    assignment_record_seen: set[str] = set()
    input_seen: set[str] = set()
    result_seen: set[str] = set()
    ledger_seen: set[str] = set()
    for row in tu_rows:
        if not isinstance(row, Mapping) or not _strict_closed_object(row, tu_required_keys, tu_optional_keys, "tu_row", issues):
            continue
        # Do not use any row value as a set/dict key until every primitive
        # field and nested assignment member has passed the type gate.
        if not validate_nested_tu(row):
            continue
        identity = row.get("join_identity")
        if not isinstance(identity, Mapping) or _safe_mapping_keys(identity, "tu_assignment", issues) != assignment_keys:
            _issue(issues, "tu_assignment_identity_invalid", str(row.get("tu_id")))
        key = (row.get("build_id"), row.get("corpus_id"), row.get("tu_id"), row.get("order"))
        if key in tu_seen:
            _issue(issues, "duplicate_tu_row", str(row.get("tu_id")))
        tu_seen.add(key)
        assignment_key = canonical_json(row.get("join_identity")).decode("ascii", errors="replace")
        if assignment_key in assignment_seen:
            _issue(issues, "duplicate_tu_assignment", str(row.get("tu_id")))
        assignment_seen.add(assignment_key)
        # Content digests may legitimately be identical for paired workloads.
        # Ownership is instead bound to independently preregistered record
        # identities, which must be globally one-to-one across builds/TUs.
        for seen, value, code in ((input_seen, row.get("input_record_id"), "duplicate_tu_input_record_identity"),
                                  (result_seen, row.get("result_record_id"), "duplicate_tu_result_record_identity"),
                                  (ledger_seen, row.get("ledger_record_id"), "duplicate_tu_ledger_record_identity")):
            if value in seen:
                _issue(issues, code, str(row.get("tu_id")))
            seen.add(value)
        assignment_id = row.get("assignment_id")
        if assignment_id in assignment_record_seen:
            _issue(issues, "duplicate_tu_assignment_record_identity", str(row.get("tu_id")))
        assignment_record_seen.add(assignment_id)
        tu_builds.add(row.get("build_id"))
        owner = build_by_id.get(row.get("build_id"))
        if row.get("build_id") not in build_ids or owner is None:
            _issue(issues, "tu_build_join_unbound", str(row.get("tu_id")))
        else:
            namespace = owner.get("namespace")
            if isinstance(namespace, Mapping) and identity.get("c_guid") != namespace.get("c_guid"):
                _issue(issues, "tu_assignment_c_guid_mismatch", str(row.get("tu_id")))
            if identity.get("tu_seq") != row.get("order"):
                _issue(issues, "tu_assignment_sequence_mismatch", str(row.get("tu_id")))
            if identity.get("run_id") != contract.get("run_identity"):
                _issue(issues, "tu_assignment_run_identity_mismatch", str(row.get("tu_id")))
        if type(row.get("status")) is not str or row.get("status") not in {"complete", "failed", "censored"}:
            _issue(issues, "tu_status_invalid", str(row.get("tu_id")))
        if row.get("status") != "complete" and not isinstance(row.get("failure"), Mapping) and not isinstance(row.get("censor"), Mapping):
            _issue(issues, "tu_failure_or_censor_detail_missing", str(row.get("tu_id")))
        if row.get("object_digest") != row.get("reference_digest"):
            _issue(issues, "tu_object_reference_mismatch", str(row.get("tu_id")))
        if (not isinstance(row.get("bytes_c_to_f"), int) or isinstance(row.get("bytes_c_to_f"), bool) or row.get("bytes_c_to_f") < 0
                or not isinstance(row.get("bytes_f_to_c"), int) or isinstance(row.get("bytes_f_to_c"), bool) or row.get("bytes_f_to_c") < 0):
            _issue(issues, "tu_byte_ledger_invalid", str(row.get("tu_id")))
    if build_ids and tu_builds != build_ids:
        _issue(issues, "tu_build_bijection_failed")
    planned_rows = contract.get("tu_plan")
    if not isinstance(planned_rows, list) or not planned_rows:
        _issue(issues, "exact_tu_plan_missing")
    else:
        plans: dict[str, Mapping[str, Any]] = {}
        plan_identity_seen: dict[str, set[str]] = {
            "assignment_ids": set(), "input_record_ids": set(),
            "result_record_ids": set(), "ledger_record_ids": set(),
        }
        for plan_row in planned_rows:
            if not isinstance(plan_row, Mapping):
                continue
            plan_build_id = plan_row.get("build_id")
            if type(plan_build_id) is not str or not plan_build_id:
                _issue(issues, "tu_plan_build_id_type_invalid")
                continue
            if plan_build_id in plans:
                _issue(issues, "duplicate_tu_plan_build_id", plan_build_id)
            plans[plan_build_id] = plan_row
            for identity_field in plan_identity_seen:
                values = plan_row.get(identity_field)
                if not isinstance(values, list) or any(type(value) is not str or not value for value in values):
                    _issue(issues, "tu_plan_record_identity_type_invalid", f"{plan_build_id}:{identity_field}")
                    continue
                for value in values:
                    if value in plan_identity_seen[identity_field]:
                        _issue(issues, "duplicate_tu_plan_record_identity", f"{identity_field}:{value}")
                    plan_identity_seen[identity_field].add(value)
        if set(plans) != build_ids or len(plans) != len(planned_rows):
            _issue(issues, "tu_plan_build_bijection_failed")
        rows_by_build: dict[str, list[Mapping[str, Any]]] = {}
        for row in tu_rows:
            if isinstance(row, Mapping) and type(row.get("build_id")) is str:
                rows_by_build.setdefault(row.get("build_id"), []).append(row)
        for build_id, plan_row in plans.items():
            expected_ids = plan_row.get("tu_ids")
            actual_rows = rows_by_build.get(build_id, [])
            actual_ids = [row.get("tu_id") for row in actual_rows]
            valid_expected_ids = (isinstance(expected_ids, list)
                                  and all(type(value) is str and bool(value) for value in expected_ids))
            expected_id_map = ({value: index for index, value in enumerate(expected_ids)}
                               if valid_expected_ids else {})
            if not valid_expected_ids or actual_ids != expected_ids:
                _issue(issues, "tu_order_or_inventory_mismatch", str(build_id))
            if type(plan_row.get("tu_count")) is not int or len(actual_rows) != plan_row.get("tu_count"):
                _issue(issues, "tu_cardinality_mismatch", str(build_id))
            build = build_by_id.get(build_id, {})
            if (plan_row.get("block_id"), plan_row.get("regime"), plan_row.get("mode")) != (build.get("block_id"), build.get("regime"), build.get("mode")):
                _issue(issues, "tu_plan_build_identity_mismatch", str(build_id))
            for row in actual_rows:
                if row.get("corpus_id") != plan_row.get("corpus_id"):
                    _issue(issues, "tu_corpus_identity_mismatch", str(row.get("tu_id")))
                expected_order = (expected_id_map.get(row.get("tu_id"))
                                  if valid_expected_ids else None)
                if row.get("order") != expected_order:
                    _issue(issues, "tu_order_identity_mismatch", str(row.get("tu_id")))
                for row_field, plan_field in (("argv_digest", "workload_digest"), ("compiler_digest", "compiler_digest"), ("input_digest", "input_digest"), ("reference_digest", "reference_digest")):
                    if row.get(row_field) != plan_row.get(plan_field):
                        _issue(issues, "tu_immutable_identity_mismatch", f"{row.get('tu_id')}:{row_field}")
                for identity_field in ("assignment_ids", "input_record_ids", "result_record_ids", "ledger_record_ids"):
                    expected_identity_values = plan_row.get(identity_field)
                    row_identity_field = identity_field.removesuffix("s")
                    if (not isinstance(expected_identity_values, list)
                            or expected_order is None
                            or expected_order >= len(expected_identity_values)
                            or row.get(row_identity_field) != expected_identity_values[expected_order]):
                        _issue(issues, "tu_record_identity_mismatch", f"{row.get('tu_id')}:{identity_field}")
    event_seen: set[str] = set()
    event_by_build: dict[str, set[str]] = {}
    for event in event_rows:
        if not isinstance(event, Mapping) or not _strict_object(event, event_keys, "event", issues):
            continue
        validate_nested_event(event)
        event_id, build_id = event.get("event_id"), event.get("build_id")
        if type(event_id) is not str or not event_id:
            continue
        if event_id in event_seen:
            _issue(issues, "duplicate_event_id", str(event_id))
        event_seen.add(event_id)
        if type(build_id) is not str or build_id not in build_ids:
            _issue(issues, "event_build_join_unbound", str(event_id))
            continue
        kind = event.get("kind")
        if type(kind) is not str or kind not in {"preparation", "reset", "prewarm", "manifest_validation"}:
            _issue(issues, "event_kind_invalid", str(event_id))
            continue
        event_by_build.setdefault(build_id, set()).add(kind)
        start, end = event.get("start_ns"), event.get("end_ns")
        if type(start) is not int or type(end) is not int or end <= start:
            _issue(issues, "event_interval_invalid", str(event_id))
        measurement = build_by_id.get(build_id, {}).get("measurement", {})
        if (type(start) is int and type(end) is int and isinstance(measurement, Mapping)
                and type(measurement.get("start_ns")) is int and end > measurement["start_ns"]):
            _issue(issues, "preparation_event_overlaps_measurement", str(event_id))
    for build_id in build_ids:
        kinds = event_by_build.get(build_id, set())
        if "preparation" not in kinds or "prewarm" not in kinds:
            _issue(issues, "required_preparation_prewarm_event_missing", str(build_id))
        build = build_by_id.get(build_id, {})
        if build.get("regime") == "cold" and "reset" not in kinds:
            _issue(issues, "required_cold_reset_event_missing", str(build_id))
    # References used by joins/events must be structured preregistered paths;
    # self-listed evidence_refs are intentionally not an authority.
    ref_path_values = [r.get("path") for r in ref_rows
                       if isinstance(r, Mapping) and _safe_relative_path(r.get("path"))]
    ref_paths: set[str] = set(ref_path_values)
    for build in build_rows:
        if not isinstance(build, Mapping):
            continue
        join = build.get("join", {})
        if isinstance(join, Mapping) and join.get("evidence_ref") not in ref_paths:
            _issue(issues, "join_artifact_reference_unbound", str(build.get("build_id")))
        reset = build.get("reset", {})
        if isinstance(reset, Mapping) and reset.get("evidence_ref") is not None and reset.get("evidence_ref") not in ref_paths:
            _issue(issues, "reset_artifact_reference_unbound", str(build.get("build_id")))
    for row in tu_rows:
        if (isinstance(row, Mapping) and type(row.get("build_id")) is str
                and row.get("build_id") not in build_ids):
            _issue(issues, "tu_build_join_unbound", str(row.get("tu_id")))
    for event in event_rows:
        if (isinstance(event, Mapping) and type(event.get("artifact_ref")) is str
                and event.get("artifact_ref") not in ref_paths):
            _issue(issues, "event_artifact_reference_unbound", str(event.get("event_id")))


def _preflight_limits(preregistration: Mapping[str, Any], evidence: Mapping[str, Any],
                      issues: list[str]) -> None:
    """Check all aggregate resource limits before validation loops or hashing."""
    contract = preregistration.get("contract")
    if not isinstance(contract, Mapping):
        return
    plan = contract.get("block_plan")
    tu_plan = contract.get("tu_plan")
    builds = evidence.get("builds")
    blocks = evidence.get("blocks")
    tus = evidence.get("tu_rows")
    events = evidence.get("events")
    refs = preregistration.get("artifact_refs")
    checks = ((plan, MAX_BLOCKS, "block_plan_limit_exceeded"),
              (tu_plan, MAX_TU_PLAN_ROWS, "tu_plan_limit_exceeded"),
              (builds, MAX_BUILDS, "build_inventory_limit_exceeded"),
              (blocks, MAX_BLOCKS, "block_inventory_limit_exceeded"),
              (tus, MAX_EVIDENCE_TUS, "evidence_tu_limit_exceeded"),
              (events, MAX_EVENTS, "event_limit_exceeded"),
              (refs, MAX_ARTIFACTS, "artifact_reference_limit_exceeded"))
    for value, limit, code in checks:
        if isinstance(value, list) and len(value) > limit:
            _issue(issues, code)
    bootstrap = contract.get("bootstrap")
    replicates = bootstrap.get("replicates") if isinstance(bootstrap, Mapping) else None
    block_count = len(plan) if isinstance(plan, list) else 0
    if isinstance(replicates, int) and isinstance(plan, list):
        # Two regimes are evaluated independently; include both in the work
        # estimate, and reject before the arithmetic estimate list exists.
        if replicates < 0 or block_count * replicates * len(REGIMES) > MAX_BOOTSTRAP_WORK:
            _issue(issues, "combined_bootstrap_work_limit_exceeded")
    if isinstance(refs, list):
        total = 0
        for ref in refs:
            if isinstance(ref, Mapping) and type(ref.get("bytes")) is int and ref.get("bytes") >= 0:
                if ref["bytes"] > MAX_ARTIFACT_BYTES_PER_FILE:
                    _issue(issues, "artifact_file_bytes_limit_exceeded", str(ref.get("path", "<unknown>")))
                total += ref["bytes"]
        if total > MAX_ARTIFACT_BYTES_TOTAL:
            _issue(issues, "artifact_total_bytes_limit_exceeded")


def _verify_preregistered_core(preregistration: Any, evidence: Any, *, expected_digest: str | None = None,
                               evidence_root: Any | None = None) -> dict[str, Any]:
    """Verify evidence against an independently supplied V5 preregistration.

    The independently supplied preregistration is mandatory.  Producer input
    can neither define nor replace the contract, artifact inventory, or raw
    observation schemas.
    """
    issues: list[str] = []
    if not isinstance(preregistration, Mapping) or not isinstance(evidence, Mapping):
        return {"valid": False, "decision": "INCONCLUSIVE", "issues": ["input_not_object"]}
    _preflight_limits(preregistration, evidence, issues)
    if issues:
        return {"valid": False, "decision": "INCONCLUSIVE", "issues": sorted(set(issues))}
    if expected_digest is None or not _is_digest(expected_digest):
        issues.append("independent_preregistration_digest_missing")
    elif canonical_file_sha256(preregistration) != expected_digest:
        issues.append("independent_preregistration_digest_mismatch")
    prereg_digest = canonical_file_sha256(preregistration)
    if evidence.get("preregistration_digest") != prereg_digest:
        issues.append("evidence_preregistration_binding_mismatch")
    if "contract" in evidence and evidence["contract"] != preregistration.get("contract"):
        issues.append("evidence_contract_override")
    expected_code = (preregistration.get("contract", {}).get("bootstrap", {})
                     if isinstance(preregistration.get("contract"), Mapping) else {}).get("code_sha256")
    try:
        actual_code = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    except OSError:
        actual_code = ""
    if expected_code != actual_code:
        issues.append("executing_code_hash_mismatch")
    refs = preregistration.get("artifact_refs", preregistration.get("expected_artifacts"))
    if evidence.get("artifact_refs") != refs:
        issues.append("evidence_artifact_inventory_override")
    if evidence_root is None:
        issues.append("artifact_root_missing")
    else:
        # The descriptor-relative reader returns the exact bytes it hashed;
        # semantic validation below consumes this capture and never reopens a
        # producer pathname.
        authenticated_bytes: dict[str, bytes] = {}
        _authenticated_root(evidence_root, refs, issues, authenticated_bytes)
    contract = preregistration.get("contract")
    if not isinstance(contract, Mapping):
        issues.append("preregistration_contract_missing")
    else:
        _validate_preregistration_schema(preregistration, issues)
        # Verify the immutable preregistration itself, then require evidence
        # to contain no producer-controlled contract copy.
        _validate_contract({"schema": SCHEMA, "contract": contract,
                            "contract_digest": sha256_json(contract)}, issues)
        if contract.get("experiment_id") != contract.get("run_identity"):
            issues.append("contract_experiment_identity_mismatch")
        contract_keys = {"schema", "immutable", "source_sha256", "binary_sha256",
                         "compiler_env_digest", "workload_manifest_digest", "wire_definition",
                         "wire_definition_sha256", "block_plan_sha256", "block_plan",
                         "modes", "regimes", "bootstrap_unit", "ratio_definition", "estimator",
                         "bound_method", "required_blocks_per_regime", "prewarm", "bootstrap",
                         "experiment_id", "run_identity", "failure_policy", "clock_identity",
                         "max_duration_ns", "tu_plan", "tu_plan_sha256", "code_identity",
                         "artifact_plan"}
        contract_keys_actual = _safe_mapping_keys(contract, "prereg_contract", issues)
        if contract_keys_actual is not None:
            unknown = contract_keys_actual - contract_keys
            if unknown:
                issues.append("unknown_preregistration_field:" + ",".join(sorted(unknown)))
        boot = contract.get("bootstrap", {})
        if isinstance(boot, Mapping):
            if not isinstance(boot.get("replicates"), int) or boot.get("replicates", 0) > MAX_BOOTSTRAP_REPLICATES:
                issues.append("bootstrap_replicates_exceeds_limit")
            if not isinstance(boot.get("seed"), int) or boot.get("seed", 0) > MAX_SEED:
                issues.append("bootstrap_seed_exceeds_limit")
        if not isinstance(contract.get("run_identity"), str) or not contract.get("run_identity"):
            issues.append("run_identity_missing")
        if evidence.get("experiment_id") != contract.get("run_identity"):
            issues.append("experiment_identity_mismatch")
        code_identity = contract.get("code_identity")
        if isinstance(code_identity, Mapping):
            for identity_field, contract_field in (("source_sha256", "source_sha256"),
                                                    ("binary_sha256", "binary_sha256"),
                                                    ("compiler_env_digest", "compiler_env_digest")):
                if code_identity.get(identity_field) != contract.get(contract_field):
                    issues.append("code_identity_authority_mismatch:" + identity_field)
            if (code_identity.get("verifier_sha256") != boot.get("code_sha256")
                    if isinstance(boot, Mapping) else True):
                issues.append("code_identity_verifier_mismatch")
            if code_identity.get("verifier_sha256") != expected_code:
                issues.append("code_identity_executing_digest_mismatch")
        plan_rows = contract.get("block_plan")
        if isinstance(plan_rows, list) and len(plan_rows) > MAX_BLOCKS:
            issues.append("block_plan_limit_exceeded")
    if issues:
        return {"valid": False, "decision": "INCONCLUSIVE", "issues": sorted(set(issues))}
    _strict_raw_gate(evidence, contract, issues, authenticated_bytes if evidence_root is not None else None)
    if issues:
        return {"valid": False, "decision": "INCONCLUSIVE", "issues": sorted(set(issues))}
    # Verify exact evidence inventories and identities before arithmetic.
    builds = evidence.get("builds")
    blocks = evidence.get("blocks")
    plan = contract.get("block_plan", [])
    plan_by_id = {row.get("block_id"): row for row in plan if isinstance(row, Mapping)}
    if not isinstance(builds, list) or not isinstance(blocks, list):
        return {"valid": False, "decision": "INCONCLUSIVE", "issues": ["build_block_inventory_missing"]}
    if {b.get("block_id") for b in blocks if isinstance(b, Mapping)} != set(plan_by_id):
        issues.append("block_plan_bijection_failed")
    expected_builds = {bid for row in plan if isinstance(row, Mapping) for bid in row.get("build_ids", [])}
    actual_builds = [b.get("build_id") for b in builds if isinstance(b, Mapping)]
    if set(actual_builds) != expected_builds or len(actual_builds) != len(set(actual_builds)):
        issues.append("build_plan_bijection_failed")
    for block in blocks:
        if not isinstance(block, Mapping):
            continue
        expected = plan_by_id.get(block.get("block_id"))
        if expected is None or block.get("build_ids") != expected.get("build_ids") or block.get("regime") != expected.get("regime") or block.get("order") != expected.get("order"):
            issues.append("block_plan_identity_mismatch:" + str(block.get("block_id")))
    # Enforce raw monotonic intervals, declared duration, and serial AB/BA
    # order.  wall_seconds is display-only and must derive exactly.
    max_duration = contract.get("max_duration_ns")
    if not isinstance(max_duration, int) or isinstance(max_duration, bool) or max_duration <= 0:
        issues.append("max_duration_missing")
    by_id = {b.get("build_id"): b for b in builds if isinstance(b, Mapping)}
    for build in builds:
        if not isinstance(build, Mapping):
            continue
        measurement = build.get("measurement", {})
        start, end = measurement.get("start_ns"), measurement.get("end_ns")
        if not isinstance(start, int) or isinstance(start, bool) or not isinstance(end, int) or isinstance(end, bool):
            issues.append("monotonic_timestamp_missing:" + str(build.get("build_id")))
        elif end <= start:
            issues.append("monotonic_interval_invalid:" + str(build.get("build_id")))
        elif isinstance(max_duration, int) and end - start > max_duration:
            issues.append("max_duration_exceeded:" + str(build.get("build_id")))
        elif "wall_seconds" in build and build.get("wall_seconds") != (end - start) / 1_000_000_000:
            issues.append("wall_interval_mismatch:" + str(build.get("build_id")))
    for block in blocks:
        if not isinstance(block, Mapping):
            continue
        seq = block.get("sequence_build_ids", [])
        if not isinstance(seq, list) or any(type(value) is not str for value in seq):
            issues.append("sequence_build_ids_type_invalid:" + str(block.get("block_id")))
            continue
        previous_end = None
        for bid in seq:
            m = by_id.get(bid, {}).get("measurement", {})
            if not isinstance(m, Mapping):
                continue
            start, end = m.get("start_ns"), m.get("end_ns")
            if isinstance(start, int) and isinstance(end, int):
                if previous_end is not None and start < previous_end:
                    issues.append("build_intervals_overlap_or_out_of_order:" + str(block.get("block_id")))
                previous_end = end
    if issues:
        return {"valid": False, "decision": "INCONCLUSIVE", "issues": sorted(set(issues))}
    collection = _collect_statistics_observations(evidence, strict=True)
    issues.extend(collection.issues)
    if issues:
        return {"valid": False, "decision": "INCONCLUSIVE", "issues": sorted(set(issues))}
    bootstrap = contract.get("bootstrap")
    if not isinstance(bootstrap, Mapping):
        return {"valid": False, "decision": "INCONCLUSIVE", "issues": ["bootstrap_arithmetic_parameters_invalid"]}
    try:
        arithmetic = _compute_statistics_arithmetic(
            collection.cold_ratios, collection.warm_ratios,
            bootstrap["seed"], bootstrap["replicates"],
        )
    except (ContractError, KeyError, TypeError, ValueError, OverflowError):
        return {"valid": False, "decision": "INCONCLUSIVE", "issues": ["arithmetic_input_invalid"]}
    supplied = evidence.get("recomputed")
    if isinstance(supplied, Mapping) and sha256_json(arithmetic) != supplied.get("statistics_sha256"):
        return {"valid": False, "decision": "INCONCLUSIVE", "issues": ["derived_statistics_mismatch"]}
    if any(arithmetic[regime]["lower_95"] is None for regime in REGIMES):
        decision = "INCONCLUSIVE"
        valid = False
    elif any(Decimal(str(arithmetic[regime]["lower_95"])) > Decimal("1.10") for regime in REGIMES):
        decision = "RED"
        valid = True
    elif all(Decimal(str(arithmetic[regime]["upper_95"])) <= Decimal("1.10") for regime in REGIMES):
        decision = "GREEN"
        valid = True
    else:
        decision = "INCONCLUSIVE"
        valid = True
    result_dict = Verification(valid, decision, tuple(), arithmetic,
                               collection.retained_builds, collection.retained_blocks).as_dict()
    result_dict["preregistration_digest"] = prereg_digest
    result_dict["evidence_root_sha256"] = sha256_json(evidence.get("artifact_refs"))
    result_dict["source_sha256"] = contract.get("source_sha256")
    result_dict["binary_sha256"] = contract.get("binary_sha256")
    result_dict["executing_code_sha256"] = expected_code
    result_dict["inferential_unit"] = "whole_block"
    return result_dict


def verify_preregistered(preregistration: Any, evidence: Any, *, expected_digest: str | None = None,
                         evidence_root: Any | None = None) -> dict[str, Any]:
    """Run the independently-bound protocol and convert malformed input to HOLD.

    All document-shaped decisions are gated by the preregistration and raw
    evidence checks in ``_verify_preregistered_core``.  A malformed nested
    object is a typed inconclusive result, never an exception escaping to the
    caller.
    """
    try:
        preregistration = _normalize_direct(preregistration)
        evidence = _normalize_direct(evidence)
        return _verify_preregistered_core(
            preregistration, evidence, expected_digest=expected_digest,
            evidence_root=evidence_root,
        )
    except ContractError as exc:
        return {"valid": False, "decision": "INCONCLUSIVE", "issues": [str(exc)]}
    except (TypeError, ValueError, KeyError, IndexError, AttributeError,
            OverflowError, UnicodeError, RecursionError, RuntimeError):
        return {"valid": False, "decision": "INCONCLUSIVE", "issues": ["typed_input_invalid"]}


__all__ = [
    "SCHEMA",
    "VERIFIER_IMPLEMENTATION",
    "UPSTREAM_SCHEMA",
    "UPSTREAM_REF_FIELDS",
    "UPSTREAM_KINDS",
    "ROW_KEY_PREFIX",
    "ROW_KEY_FIELDS",
    "UPSTREAM_JOIN_FIELDS",
    "UPSTREAM_LINK_FIELDS",
    "UPSTREAM_LEDGER_KINDS",
    "ContractError",
    "Verification",
    "canonical_json",
    "canonical_file_bytes",
    "canonical_file_sha256",
    "is_canonical_jsonl",
    "parse_evidence",
    "sha256_json",
    "verify_document",
    "compute_statistics",
    "verify_text",
    "parse_canonical_bytes",
    "verify_preregistered",
]
