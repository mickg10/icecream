"""Offline verdict persistence and deterministic human evidence reports."""

from __future__ import annotations

import hashlib
import os
from collections import Counter
from pathlib import Path
from typing import Any

try:
    from .collect import CollectError, load_verified_bundle
    from .schema_validation import canonical_bytes
    from .verdict import evaluate_bundle
except ImportError:  # Direct execution from this directory.
    from collect import CollectError, load_verified_bundle
    from schema_validation import canonical_bytes
    from verdict import evaluate_bundle


WITNESS_SCHEMA = "icefarm-witness-v1"


class ReportError(RuntimeError):
    """A verdict or report could not be reproduced from immutable evidence."""


def _atomic_bytes(path: Path, value: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + f".tmp-{os.getpid()}")
    try:
        temporary.write_bytes(value)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def _atomic_json(path: Path, value: Any) -> None:
    _atomic_bytes(path, canonical_bytes(value))


def _read_json(path: Path) -> dict[str, Any]:
    import json

    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ReportError(f"cannot load {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise ReportError(f"{path} is not a JSON object")
    return value


def verify_bundle(root: Path | str) -> dict[str, Any]:
    """Recompute and atomically retain the pure verdict."""

    path = Path(root)
    try:
        bundle = load_verified_bundle(path)
    except CollectError as exc:
        raise ReportError(str(exc)) from exc
    verdict = evaluate_bundle(bundle)
    _atomic_json(path / "verdict.json", verdict)
    return verdict


def _cell_summary(bundle: dict[str, Any]) -> list[str]:
    rows = bundle["rows"]
    profiles = Counter(row["tail_profile"] or "legacy" for row in rows)
    outcomes = Counter(row["session_outcome"] for row in rows)
    observations = bundle["observations"]
    return [
        f"- Jobs: {len(rows)}; exact: {sum(row['exact'] is True for row in rows)}; "
        f"compile failures: {len(observations['compile_failure_job_ids'])}; "
        f"local fallbacks: {len(observations['local_fallback_job_ids'])}.",
        "- Profiles: "
        + ", ".join(f"{name}={count}" for name, count in sorted(profiles.items()))
        + ".",
        "- Session outcomes: "
        + ", ".join(f"{name}={count}" for name, count in sorted(outcomes.items()))
        + ".",
        f"- Cell wall: {observations['cell_wall_ms']} ms; "
        f"C→F bytes: {sum(row['c_to_f_bytes'] for row in rows)}; "
        f"F→C bytes: {sum(row['f_to_c_bytes'] for row in rows)}.",
        f"- Oracle samples: {observations['oracle']['sample_total']}; "
        f"mismatches: {len(observations['oracle']['sample_mismatch_job_ids'])}.",
    ]


def render_report(bundle: dict[str, Any], verdict: dict[str, Any]) -> str:
    """Render one deterministic Markdown report from bundle plus verdict."""

    lines = [
        f"# Evidence — {bundle['run_id']}",
        "",
        f"Verdict: **{verdict['status']}**",
        "",
        "## Identity",
        "",
        f"- Scenario: `{bundle['scenario'].get('id', '?')}` "
        f"(`{bundle['scenario_digest']}`)",
        f"- Farm digest: `{bundle['farm_digest']}`",
        f"- Topology digest: `{bundle['topology_digest']}`",
        f"- SHA256SUMS: `{bundle['checksum_policy']['sha256sums_sha256']}`",
        "",
        "## Result",
        "",
        *_cell_summary(bundle),
        "",
        "## Runtime images",
        "",
        "| Instance | Role | Version | Image | Commit | Closure | Binary SHA-256 |",
        "|---|---:|---:|---|---|---|---|",
    ]
    for instance in bundle["instances"]:
        image = instance["image"]
        lines.append(
            f"| {instance['name']} | {instance['role']} | {instance['version']} | "
            f"`{image['label']}` | `{image['commit']}` | "
            f"`{image.get('closure_sha256', 'absent')}` | `{instance['sha256']}` |"
        )
    lines.extend(
        [
            "",
            "## Verdict clauses",
            "",
            "| Clause | Status | Offending jobs | Detail |",
            "|---|---:|---|---|",
        ]
    )
    for clause in verdict["clauses"]:
        detail = str(clause["detail"]).replace("|", "\\|").replace("\n", " ")
        offenders = ", ".join(f"`{item}`" for item in clause["offending_job_ids"]) or "—"
        lines.append(f"| `{clause['id']}` | {clause['status']} | {offenders} | {detail} |")
    lines.extend(
        [
            "",
            "## Reproduction",
            "",
            "`SHA256SUMS` authenticates every file below `evidence/`. "
            "`verdict.json` is recomputed from `bundle.json` only after those bytes "
            "and all embedded/snapshotted bindings agree.",
            "",
        ]
    )
    return "\n".join(lines)


def report_bundle(root: Path | str) -> tuple[dict[str, Any], str]:
    """Verify, cross-check any retained verdict, write report and witness."""

    path = Path(root)
    try:
        bundle = load_verified_bundle(path)
    except CollectError as exc:
        raise ReportError(str(exc)) from exc
    recomputed = evaluate_bundle(bundle)
    verdict_path = path / "verdict.json"
    if verdict_path.exists() and _read_json(verdict_path) != recomputed:
        raise ReportError("retained verdict differs from the offline recomputation")
    _atomic_json(verdict_path, recomputed)
    report = render_report(bundle, recomputed)
    _atomic_bytes(path / "EVIDENCE.md", report.encode("utf-8"))
    witness = {
        "farm_digest": bundle["farm_digest"],
        "run_id": bundle["run_id"],
        "scenario_digest": bundle["scenario_digest"],
        "schema": WITNESS_SCHEMA,
        "sha256sums_sha256": bundle["checksum_policy"]["sha256sums_sha256"],
        "status": recomputed["status"],
        "topology_digest": bundle["topology_digest"],
        "verdict_sha256": hashlib.sha256(canonical_bytes(recomputed)).hexdigest(),
    }
    _atomic_json(path / "witness.json", witness)
    return recomputed, report
