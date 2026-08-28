#!/usr/bin/env python3
"""Execute and verify the S5 rollback/roll-forward acceptance scenario.

The existing S5 runner owns the real compile cell and the role-manifest
authority.  This module owns only the release transition around that cell:
drain, stop/reclaim, bind-path selection, legacy verification, and restoring
the current ZSTD_TU set.  All lifecycle and compile observations come from
the supplied commands; missing or contradictory observations are failures.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Mapping, Sequence

from farmharness import s4_real_cells
from farmharness.s5_paired_build import P50_ROLE_HASHES

SCHEMA = "icecream-s5-rollback-gate-v1"
PHASES = (
    "drain_declared",
    "sidecar_stopped",
    "sidecar_reclaimed",
    "bound_previous",
    "legacy_verified",
    "bound_current",
    "current_profile_verified",
)
ROLE_NAMES = ("S", "C", "F", "E", "X")
COUNTERS = (
    "orphan_assignments",
    "duplicate_acceptances",
    "leaked_input_records",
    "stale_endpoint_reuse",
    "sidecar_processes",
    "sidecar_leases",
    "staging_slots",
)


class RollbackError(ValueError):
    """Raised when rollback evidence is incomplete or inconsistent."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise RollbackError(message)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _inside(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def _role_paths(root: Path) -> dict[str, Path]:
    return {
        "S": root / "scheduler/icecc-scheduler",
        "F": root / "daemon/iceccd",
        "C": root / "client/icecc",
        "E": root / "client/icecc-create-env",
        "X": root / "cache/icecc-cache-service",
    }


def artifact_manifest(root: Path, *, require_cache: bool,
                      expected_hashes: Mapping[str, str] | None = None) -> dict[str, Any]:
    """Describe a regular, non-symlink role root without trusting labels."""
    _require(root.is_dir() and not root.is_symlink(), "artifact root is not a regular directory")
    root = root.absolute()
    roles: dict[str, Any] = {}
    errors: list[str] = []
    for role, path in _role_paths(root).items():
        required = require_cache or role != "X"
        item: dict[str, Any] = {
            "role": role,
            "path": str(path),
            "exists": path.is_file(),
            "symlink": path.is_symlink(),
        }
        if path.is_file() and not path.is_symlink():
            item["bytes"] = path.stat().st_size
            item["sha256"] = _sha256(path)
            item["executable"] = role == "E" or os.access(path, os.X_OK)
            if not item["executable"]:
                errors.append(f"{role}:not-executable")
            if expected_hashes is not None and expected_hashes.get(role) != item["sha256"]:
                errors.append(f"{role}:hash-mismatch")
        elif required:
            errors.append(f"{role}:missing-or-symlink")
        roles[role] = item
    if errors:
        raise RollbackError("artifact-manifest-invalid:" + ";".join(errors))
    return {
        "root": str(root),
        "role_hashes": {role: item.get("sha256") for role, item in roles.items()},
        "roles": roles,
        "cache_service_required": require_cache,
    }


def _verify_manifest(value: Mapping[str, Any], label: str, *, require_cache: bool) -> None:
    root = Path(str(value.get("root", "")))
    _require(root.is_dir() and not root.is_symlink(), f"{label}:invalid root")
    observed = artifact_manifest(root, require_cache=require_cache)
    supplied = value.get("roles")
    _require(isinstance(supplied, Mapping), f"{label}:missing roles")
    for role in ROLE_NAMES:
        item = supplied.get(role)
        _require(isinstance(item, Mapping), f"{label}:missing {role} role")
        actual = observed["roles"][role]
        if role == "X" and not require_cache and not actual["exists"]:
            continue
        _require(item.get("exists") is True and item.get("symlink") is False,
                 f"{label}:{role}:not-regular")
        _require(item.get("sha256") == actual.get("sha256"),
                 f"{label}:{role}:hash-mismatch")
        _require(item.get("executable") is True and actual.get("executable") is True,
                 f"{label}:{role}:not-executable")


def _verify_compile_observation(value: Mapping[str, Any], label: str,
                                manifest: Mapping[str, Any]) -> None:
    """Bind compile evidence to the artifact that was actually launched."""
    expected_root = Path(str(manifest.get("root", ""))).absolute()
    observed_root = Path(str(value.get("artifact_root", ""))).absolute()
    _require(observed_root == expected_root,
             f"{label}:launched artifact root mismatch")
    expected_hashes = manifest.get("role_hashes")
    observed_hashes = value.get("role_hashes")
    _require(isinstance(expected_hashes, Mapping) and
             isinstance(observed_hashes, Mapping),
             f"{label}:missing role hashes")
    _require(dict(observed_hashes) == dict(expected_hashes),
             f"{label}:launched role hash mismatch")
    remote = value.get("remote_sha256")
    reference = value.get("reference_sha256")
    for name, digest in (("remote", remote), ("reference", reference)):
        _require(isinstance(digest, str) and len(digest) == 64 and
                 all(character in "0123456789abcdefABCDEF" for character in digest),
                 f"{label}:{name} object SHA256 is missing or invalid")
    _require(remote.lower() == reference.lower(),
             f"{label}:remote/reference object SHA256 mismatch")


def _zero_counter_map(value: Mapping[str, Any], label: str) -> None:
    for name in COUNTERS:
        raw = value.get(name)
        _require(isinstance(raw, int) and not isinstance(raw, bool),
                 f"{label}:{name}:missing-or-noninteger")
        _require(raw == 0, f"{label}:{name}:nonzero")


def verify_evidence(evidence: Mapping[str, Any]) -> dict[str, Any]:
    """Verify one complete rollback/roll-forward observation."""
    _require(evidence.get("schema") == SCHEMA, "wrong rollback schema")
    observed_phases = evidence.get("phases")
    _require(observed_phases == list(PHASES), "rollback phase sequence is incomplete or reordered")
    artifacts = evidence.get("artifacts")
    _require(isinstance(artifacts, Mapping), "missing artifact manifests")
    _verify_manifest(artifacts.get("current", {}), "current", require_cache=True)
    _verify_manifest(artifacts.get("previous", {}), "previous", require_cache=False)

    drain = evidence.get("drain")
    _require(isinstance(drain, Mapping) and drain.get("declared") is True,
             "drain was not declared")
    _require(drain.get("in_flight") == "stopped-and-reclaimed",
             "in-flight assignments were not stopped and reclaimed")
    _require(isinstance(drain.get("deadline_ns"), int) and drain["deadline_ns"] > 0,
             "drain deadline is absent")

    sidecar = evidence.get("sidecar")
    _require(isinstance(sidecar, Mapping) and sidecar.get("stopped") is True and
             sidecar.get("reclaimed") is True,
             "sidecar state was not stopped and reclaimed")
    _zero_counter_map(sidecar, "sidecar")

    binding = evidence.get("binding")
    _require(isinstance(binding, Mapping), "missing bind-path transition")
    _require(binding.get("path") and binding.get("before") == "current" and
             binding.get("rollback") == "previous" and binding.get("restored") == "current",
             "bind-path transition did not rollback and restore")

    legacy = evidence.get("legacy")
    _require(isinstance(legacy, Mapping), "missing legacy compile observation")
    _verify_compile_observation(legacy, "legacy", artifacts["previous"])
    _require(legacy.get("remote_compile") is True and legacy.get("byte_identical") is True,
             "post-rollback legacy compile was not remote byte-identical")
    _require(legacy.get("cache_observed") is False and
             legacy.get("profile_use_count") == 0 and
             legacy.get("selected_profile") is None,
             "legacy compile used a cache or profile")
    _zero_counter_map(legacy, "legacy")

    current = evidence.get("current")
    _require(isinstance(current, Mapping), "missing restored current compile observation")
    _verify_compile_observation(current, "current", artifacts["current"])
    _require(current.get("remote_compile") is True and current.get("byte_identical") is True and
             current.get("cache_observed") is True and
             current.get("selected_profile") == "ZSTD_TU" and
             isinstance(current.get("selected_tu_count"), int) and
             current["selected_tu_count"] > 0,
             "restored current ZSTD_TU compile was not byte-identical")
    _zero_counter_map(current, "current")

    cleanup = evidence.get("cleanup")
    _require(isinstance(cleanup, Mapping) and cleanup.get("status") == "complete" and
             cleanup.get("skipped") is False,
             "rollback cleanup was skipped or incomplete")
    _zero_counter_map(cleanup, "cleanup")
    return {"schema": SCHEMA, "status": "PASS", "phases": list(PHASES)}


@dataclass(frozen=True)
class HookResult:
    name: str
    returncode: int
    stdout: str
    stderr: str


def _run_hook(name: str, command: str, timeout: float) -> HookResult:
    if not command.strip():
        raise RollbackError(f"missing required rollback hook: {name}")
    try:
        completed = subprocess.run(command, shell=True, text=True,
                                   capture_output=True, timeout=timeout, check=False)
    except subprocess.TimeoutExpired as exc:
        raise RollbackError(f"rollback hook timed out: {name}") from exc
    return HookResult(name, completed.returncode, completed.stdout, completed.stderr)


def make_evidence(current_root: Path, previous_root: Path, bind_path: Path,
                  hooks: Mapping[str, str], timeout: float,
                  expected_current_hashes: Mapping[str, str] | None = None,
                  expected_previous_hashes: Mapping[str, str] | None = None) -> dict[str, Any]:
    """Run the declared hooks and require each to emit machine-readable facts.

    Hook output is intentionally not interpreted as a PASS claim by itself;
    the resulting document is passed through ``verify_evidence`` after all
    phases complete.  Compile hooks should be the existing S4 real-cell
    invocations, with one legacy and one all-P50 ZSTD_TU observation.
    """
    current_root = current_root.absolute()
    previous_root = previous_root.absolute()
    bind_path = bind_path.absolute()
    _require(current_root != previous_root, "current and previous roots must differ")
    _require(not _inside(bind_path, current_root) and not _inside(bind_path, previous_root),
             "bind path must be outside both artifact roots")
    current_manifest = artifact_manifest(current_root, require_cache=True,
                                         expected_hashes=expected_current_hashes)
    previous_manifest = artifact_manifest(previous_root, require_cache=False,
                                          expected_hashes=expected_previous_hashes)
    phases: list[str] = []
    results: dict[str, HookResult] = {}
    for phase, hook_name in (
        ("drain_declared", "drain"),
        ("sidecar_stopped", "stop"),
        ("sidecar_reclaimed", "reclaim"),
    ):
        result = _run_hook(hook_name, hooks.get(hook_name, ""), timeout)
        results[hook_name] = result
        if result.returncode != 0:
            raise RollbackError(f"rollback hook failed: {hook_name}")
        phases.append(phase)
    # The bind path is an explicit symlink flip.  Never overwrite a regular
    # path: a deployment owner must provide a dedicated bind path.
    if not bind_path.is_symlink() or bind_path.resolve() != current_root:
        raise RollbackError("bind path is not bound to the current role root")

    def bind_to(root: Path) -> None:
        bind_path.parent.mkdir(parents=True, exist_ok=True)
        temporary = bind_path.with_name(bind_path.name + ".rollback-new")
        temporary.unlink(missing_ok=True)
        temporary.symlink_to(root.resolve(), target_is_directory=True)
        os.replace(temporary, bind_path)
        _require(bind_path.resolve() == root,
                 f"bind path did not select {root.name} role root")

    # Hooks emit JSON objects on their final line.  Parse observations before
    # advancing to the next lifecycle phase so cleanup follows a verified
    # restored-current compile, rather than merely a zero return code.
    def hook_json(name: str) -> dict[str, Any]:
        result = results.get(name)
        _require(result is not None, f"{name} hook did not run")
        lines = result.stdout.splitlines()
        if not lines:
            raise RollbackError(f"{name} hook emitted no evidence")
        try:
            value = json.loads(lines[-1])
        except json.JSONDecodeError as exc:
            raise RollbackError(f"{name} hook emitted invalid JSON") from exc
        if not isinstance(value, dict):
            raise RollbackError(f"{name} hook evidence is not an object")
        return value

    rollback_started = True
    try:
        bind_to(previous_root)
        phases.append("bound_previous")
        legacy = _run_hook("legacy", hooks.get("legacy", ""), timeout)
        results["legacy"] = legacy
        if legacy.returncode != 0:
            raise RollbackError("post-rollback legacy compile hook failed")
        phases.append("legacy_verified")
        bind_to(current_root)
        phases.append("bound_current")
        current = _run_hook("current", hooks.get("current", ""), timeout)
        results["current"] = current
        if current.returncode != 0:
            raise RollbackError("restored current compile hook failed")
        current_observed = hook_json("current")
        _verify_compile_observation(current_observed, "current", current_manifest)
        _require(current_observed.get("remote_compile") is True and
                 current_observed.get("byte_identical") is True and
                 current_observed.get("cache_observed") is True and
                 current_observed.get("selected_profile") == "ZSTD_TU" and
                 isinstance(current_observed.get("selected_tu_count"), int) and
                 current_observed["selected_tu_count"] > 0,
                 "restored current ZSTD_TU compile was not byte-identical")
        _zero_counter_map(current_observed, "current")
        phases.append("current_profile_verified")
        cleanup = _run_hook("cleanup", hooks.get("cleanup", ""), timeout)
        results["cleanup"] = cleanup
        if cleanup.returncode != 0:
            raise RollbackError("final rollback cleanup hook failed")
    finally:
        # Keep the deployment on the current artifact even when legacy,
        # current, cleanup, parsing, or validation fails after the rollback
        # flip.  The operation is deliberately idempotent.
        if rollback_started:
            bind_to(current_root)
    drain = hook_json("drain")
    sidecar = {**hook_json("stop"), **hook_json("reclaim")}
    legacy_observed = hook_json("legacy")
    evidence = {
        "schema": SCHEMA,
        "phases": phases,
        "artifacts": {"current": current_manifest, "previous": previous_manifest},
        "drain": drain,
        "sidecar": sidecar,
        "binding": {"path": str(bind_path), "before": "current",
                     "rollback": "previous", "restored": "current"},
        "legacy": legacy_observed,
        "current": current_observed,
        "cleanup": hook_json("cleanup"),
    }
    verify_evidence(evidence)
    return evidence


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--current-root", type=Path, required=True)
    parser.add_argument("--previous-root", type=Path, required=True)
    parser.add_argument("--bind-path", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=300.0)
    for name in ("drain", "stop", "reclaim", "legacy", "current", "cleanup"):
        parser.add_argument(f"--{name}-command", required=True)
    args = parser.parse_args(argv)
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    hooks = {name: getattr(args, f"{name}_command")
             for name in ("drain", "stop", "reclaim", "legacy", "current", "cleanup")}
    try:
        evidence = make_evidence(args.current_root, args.previous_root, args.bind_path,
                                 hooks, args.timeout,
                                 expected_current_hashes=P50_ROLE_HASHES,
                                 expected_previous_hashes=s4_real_cells.ROLE_HASHES["43"])
        args.out.parent.mkdir(parents=True, exist_ok=True)
        with args.out.open("x", encoding="utf-8") as stream:
            json.dump(evidence, stream, sort_keys=True, separators=(",", ":"))
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
    except (OSError, RollbackError) as exc:
        print(f"S5_ROLLBACK_STATUS=FAIL reason={exc}", file=sys.stderr)
        return 1
    print("S5_ROLLBACK_STATUS=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
