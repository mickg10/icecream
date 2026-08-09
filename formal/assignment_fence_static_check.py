#!/usr/bin/env python3
"""Fail-closed static contract for the proof-bearing assignment-fence gate."""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Any, Sequence

sys.dont_write_bytecode = True

REQUIRED_MANIFEST = "assignment-fence-formal-checks-v1.json"
_TOOLCHAINS = ["stable", "differential"]


def _expected_row(
    check_id: str,
    module: str,
    config: str,
    kind: str,
    expected: str,
    property_name: str,
    timeout_seconds: int,
    *,
    trace_manifest: str | None = None,
    assumptions: list[str] | None = None,
) -> dict[str, Any]:
    row: dict[str, Any] = {
        "id": check_id,
        "module": module,
        "config": config,
        "kind": kind,
        "workers": 1,
        "expected": expected,
        "property": property_name,
    }
    if trace_manifest is not None:
        row["trace_manifest"] = trace_manifest
    row["toolchains"] = list(_TOOLCHAINS)
    row["timeout_seconds"] = timeout_seconds
    if assumptions is not None:
        row["assumptions"] = assumptions
    return row


EXPECTED_ROWS = [
    _expected_row(
        "core-fixed", "AssignmentFenceCore", "AssignmentFenceCore.cfg",
        "safety", "pass", "CoreInvariant", 1800,
    ),
    _expected_row(
        "core-heterogeneous", "AssignmentFenceCoreHeterogeneous",
        "AssignmentFenceCoreHeterogeneous.cfg", "safety", "pass",
        "CoreInvariant", 1800,
    ),
    _expected_row(
        "core-mixed-token-mutant", "AssignmentFenceCore",
        "AssignmentFenceCoreMixedMutant.cfg", "safety", "counterexample",
        "TokenRequiredExactness", 1800,
        trace_manifest=(
            "trace-manifests/AssignmentFenceCoreMixedMutant.manifest.template.json"
        ),
    ),
    _expected_row(
        "core-release-claimed-mutant", "AssignmentFenceCore",
        "AssignmentFenceCoreReleaseMutant.cfg", "safety", "counterexample",
        "ReleaseSafety", 1800,
        trace_manifest=(
            "trace-manifests/AssignmentFenceCoreReleaseMutant.manifest.template.json"
        ),
    ),
    _expected_row(
        "core-revoked-result-liveness", "AssignmentFenceCoreProgress",
        "AssignmentFenceCoreFair.cfg", "liveness", "pass",
        "RevokedEventuallyConsumed", 3600,
        assumptions=[
            "For each assignment, weak fairness only for S consuming an "
            "already-linearized REVOKED result; compiler completion and worker "
            "loss are separate environment outcomes and are not assumed by "
            "this accepted theorem"
        ],
    ),
    _expected_row(
        "network-fixed", "AssignmentFenceNetwork",
        "AssignmentFenceNetwork.cfg", "safety", "pass",
        "SafetyInvariant", 3600,
    ),
    _expected_row(
        "network-claim-wins-queued-revoke-witness",
        "AssignmentFenceNetwork",
        "AssignmentFenceNetworkClaimWinsQueuedRevokeWitness.cfg",
        "safety", "counterexample", "NoClaimBeforeRevokeConsume", 1800,
        trace_manifest=(
            "trace-manifests/"
            "AssignmentFenceNetworkClaimWinsQueuedRevokeWitness.manifest.template.json"
        ),
        assumptions=[
            "Reachability witness on the fixed model: S has queued REVOKE while "
            "a concrete claim frame remains pending, and F accepts that claim "
            "before consuming REVOKE"
        ],
    ),
    _expected_row(
        "network-fence-wins-delayed-claim-witness",
        "AssignmentFenceNetwork",
        "AssignmentFenceNetworkFenceWinsDelayedClaimWitness.cfg",
        "safety", "counterexample", "NoRejectAfterFence", 1800,
        trace_manifest=(
            "trace-manifests/"
            "AssignmentFenceNetworkFenceWinsDelayedClaimWitness.manifest.template.json"
        ),
        assumptions=[
            "Reachability witness on the fixed model: F consumes REVOKE and "
            "installs the rejection fence before the retained concrete claim "
            "reaches F"
        ],
    ),
    _expected_row(
        "network-compaction-fixed", "AssignmentFenceNetworkCompaction",
        "AssignmentFenceNetworkCompaction.cfg", "safety", "pass",
        "SafetyInvariant", 3600,
    ),
    _expected_row(
        "network-usecs-before-ready-mutant", "AssignmentFenceNetwork",
        "AssignmentFenceNetworkUseCSBeforeReadyMutant.cfg",
        "safety", "counterexample", "ReadyBeforeUseCS", 1800,
        trace_manifest=(
            "trace-manifests/"
            "AssignmentFenceNetworkUseCSBeforeReadyMutant.manifest.template.json"
        ),
    ),
    _expected_row(
        "network-release-on-enqueue-mutant", "AssignmentFenceNetwork",
        "AssignmentFenceNetworkReleaseOnEnqueueMutant.cfg",
        "safety", "counterexample", "ReleaseAfterRevokedConsume", 1800,
        trace_manifest=(
            "trace-manifests/"
            "AssignmentFenceNetworkReleaseOnEnqueueMutant.manifest.template.json"
        ),
    ),
    _expected_row(
        "network-default-allow-after-compaction-mutant",
        "AssignmentFenceNetworkCompaction",
        "AssignmentFenceNetworkDefaultAllowMutant.cfg",
        "safety", "counterexample", "NoStartAfterRevokedCompaction", 1800,
        trace_manifest=(
            "trace-manifests/"
            "AssignmentFenceNetworkDefaultAllowMutant.manifest.template.json"
        ),
    ),
    _expected_row(
        "network-f2s-bypass-mutant", "AssignmentFenceNetwork",
        "AssignmentFenceNetworkF2SBypassMutant.cfg",
        "safety", "counterexample", "F2SStrictlyIncreasing", 1800,
        trace_manifest=(
            "trace-manifests/"
            "AssignmentFenceNetworkF2SBypassMutant.manifest.template.json"
        ),
    ),
    _expected_row(
        "network-fair-liveness", "AssignmentFenceNetwork",
        "AssignmentFenceNetworkFair.cfg", "liveness", "pass",
        "NoPermanentQueuedFrame", 7200,
        assumptions=[
            "Weak fairness only for live-link S-to-F, F-to-S, S-to-D, and "
            "delayed-client-to-F drain/processing actions; connection loss "
            "remains explicit"
        ],
    ),
]
EXPECTED_IDS = [row["id"] for row in EXPECTED_ROWS]
RECEIVERS = [
    "SReceiveReady",
    "SReceiveBegin",
    "SReceiveRevoked",
    "SReceiveOwned",
    "SReceiveDone",
]
F2S_PRODUCERS = [
    "FReceivePrepare",
    "FStart",
    "FReceiveRevokeReserved",
    "FReceiveRevokeOwned",
    "FReceiveRevokeAfterDone",
    "FComplete",
]
FORBIDDEN_ABSOLUTE_HISTORY = (
    "nextF2SSeq",
    "lastF2SConsumed",
    ".seq",
    "seq |->",
)
_OPERATOR_RE = re.compile(
    r"(?m)^([A-Za-z_][A-Za-z0-9_]*)(?:\([^\n]*\))?\s*==\s*"
)


class ContractError(RuntimeError):
    """A static assignment-fence acceptance premise is violated."""


def _load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ContractError(f"cannot load JSON {path}: {exc}") from exc


def _operators(text: str) -> dict[str, str]:
    matches = list(_OPERATOR_RE.finditer(text))
    result: dict[str, str] = {}
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        name = match.group(1)
        if name in result:
            raise ContractError(f"duplicate top-level operator: {name}")
        result[name] = text[match.end():end]
    return result


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractError(message)


def _check_manifest(manifest: Path, formal_dir: Path) -> dict[str, Any]:
    expected = (formal_dir / REQUIRED_MANIFEST).resolve()
    _require(manifest.resolve() == expected, f"only {expected} is authoritative")
    document = _load_json(manifest)
    _require(isinstance(document, dict) and document.get("schema") == 1,
             "manifest schema must be exactly 1")
    checks = document.get("checks")
    _require(isinstance(checks, list), "manifest checks must be an array")
    ids = [row.get("id") for row in checks if isinstance(row, dict)]
    _require(ids == EXPECTED_IDS, f"matrix ids/order mismatch: {ids}")
    _require(len(checks) == 14, "matrix must contain exactly 14 checks")
    _require(sum(str(i).startswith("core-") for i in ids) == 5,
             "matrix must contain exactly five core rows")
    _require(sum(str(i).startswith("network-") for i in ids) == 9,
             "matrix must contain exactly nine network rows")

    for index, (row, expected_row) in enumerate(zip(checks, EXPECTED_ROWS)):
        _require(isinstance(row, dict), "every matrix row must be an object")
        _require(
            row == expected_row,
            f"{EXPECTED_IDS[index]}: row contract mismatch; "
            f"expected {json.dumps(expected_row, sort_keys=True)}, "
            f"got {json.dumps(row, sort_keys=True)}",
        )
        module = row["module"]
        config = row["config"]
        _require((formal_dir / f"{module}.tla").is_file(),
                 f"{row['id']}: missing module")
        config_path = formal_dir / config
        _require(config_path.is_file(), f"{row['id']}: missing config")
        config_text = config_path.read_text(encoding="utf-8")
        specification = re.findall(
            r"(?m)^\s*SPECIFICATION\s+([A-Za-z_][A-Za-z0-9_]*)\s*$",
            config_text,
        )
        _require(
            len(specification) == 1,
            f"{row['id']}: exactly one SPECIFICATION is required",
        )
        deadlock = re.findall(
            r"(?m)^\s*CHECK_DEADLOCK\s+(TRUE|FALSE)\s*$", config_text
        )
        _require(
            len(deadlock) == 1,
            f"{row['id']}: exactly one CHECK_DEADLOCK policy is required",
        )
        direct_checks = re.findall(
            r"(?m)^\s*(INVARIANT|PROPERTY)\s+"
            r"([A-Za-z_][A-Za-z0-9_]*)\s*$",
            config_text,
        )
        direct = (
            "PROPERTY" if row["kind"] == "liveness" else "INVARIANT",
            row["property"],
        )
        _require(
            direct in direct_checks,
            f"{row['id']}: config does not check its named property directly",
        )
        if row["expected"] == "counterexample":
            _require(
                direct_checks == [direct],
                f"{row['id']}: counterexample config must check only {direct}",
            )
            trace_path = formal_dir / row["trace_manifest"]
            _require(trace_path.is_file(), f"{row['id']}: missing trace manifest")
            trace = _load_json(trace_path)
            _require(
                trace.get("property") == row["property"],
                f"{row['id']}: trace manifest property mismatch",
            )
            metadata = trace.get("metadata", {})
            _require(isinstance(metadata, dict), f"{row['id']}: trace metadata must be an object")
            if "module" in metadata:
                _require(
                    metadata["module"] == module,
                    f"{row['id']}: trace module metadata mismatch",
                )
            if "config" in metadata:
                _require(
                    metadata["config"] == config,
                    f"{row['id']}: trace config metadata mismatch",
                )

    shared = _load_json(formal_dir / "formal-checks.json")
    shared_rows = {row.get("id"): row for row in shared.get("checks", [])}
    for expected_row in EXPECTED_ROWS:
        _require(
            shared_rows.get(expected_row["id"]) == expected_row,
            f"{expected_row['id']}: authoritative row diverges from formal-checks.json",
        )

    proofs = document.get("proofs")
    _require(isinstance(proofs, list) and len(proofs) == 1,
             "manifest must contain exactly one TLAPS proof row")
    proof = proofs[0]
    _require(
        proof == {
            "id": "core-tlaps-proof",
            "file": "AssignmentFenceCoreProof.tla",
            "expected": "pass",
            "timeout_seconds": 7200,
        },
        f"unexpected proof inventory: {proof}",
    )
    _require((formal_dir / proof["file"]).is_file(), "missing TLAPS proof file")
    return document


def _check_quotient(formal_dir: Path) -> dict[str, Any]:
    network_path = formal_dir / "AssignmentFenceNetwork.tla"
    compaction_path = formal_dir / "AssignmentFenceNetworkCompaction.tla"
    network = network_path.read_text(encoding="utf-8")
    compaction = compaction_path.read_text(encoding="utf-8")

    for token in FORBIDDEN_ABSOLUTE_HISTORY:
        _require(token not in network, f"network retains absolute F2S history token {token!r}")
        _require(token not in compaction, f"compaction retains absolute F2S history token {token!r}")

    _require("Msg(kind, assignment) ==" in network, "message constructor must omit sequence history")
    _require("[kind : AllKinds, assignment : Assignments]" in network,
             "message type must contain only kind and assignment")
    _require("f2sBypassObserved" in network, "missing monotone bypass observer")
    _require("/\\ f2sBypassObserved = FALSE" in network,
             "Init must clear the bypass observer")
    _require("/\\ f2sBypassObserved \\in BOOLEAN" in network,
             "TypeOK must type the bypass observer")
    _require("F2SStrictlyIncreasing == ~f2sBypassObserved" in network,
             "the legacy property name must denote the quotient observer")
    _require(
        re.search(
            r"ChosenF2SIndex\s*==\s*\n\s*IF MutantF2SBypass /\\ Len\(f2s\) >= 2 "
            r"THEN 2 ELSE 1",
            network,
        ) is not None,
        "ChosenF2SIndex must select index 2 only for the explicit bypass mutant",
    )

    operators = _operators(network)
    missing = [name for name in RECEIVERS + F2S_PRODUCERS if name not in operators]
    _require(not missing, f"missing load-bearing operators: {missing}")

    for name in RECEIVERS:
        body = operators[name]
        _require(body.count("f2sBypassObserved'") == 1,
                 f"{name}: must assign the bypass observer exactly once")
        _require("f2sBypassObserved \\/ (ChosenF2SIndex # 1)" in body,
                 f"{name}: observer update is not the exact monotone quotient")
        _require("RemoveAt(f2s, ChosenF2SIndex)" in body,
                 f"{name}: must consume the selected F2S queue position")

    for name in F2S_PRODUCERS:
        body = operators[name]
        _require("Append(f2s, Msg(" in body, f"{name}: must append one F2S frame")
        _require("f2sBypassObserved" in body,
                 f"{name}: must preserve the bypass observer")
        _require("f2sBypassObserved'" not in body,
                 f"{name}: producer must not mutate the bypass observer")

    drain = operators.get("DrainF2S", "")
    drained = re.findall(r"\bSReceive(?:Ready|Begin|Revoked|Owned|Done)\b", drain)
    _require(drained == RECEIVERS, f"DrainF2S order/inventory mismatch: {drained}")

    compact_ops = _operators(compaction)
    compact = compact_ops.get("CompactRevoked", "")
    _require("f2sBypassObserved" in compact and "f2sBypassObserved'" not in compact,
             "compaction must preserve the quotient observer")

    linter = formal_dir / "tla_primed_assignment_lint.py"
    result = subprocess.run(
        [sys.executable, str(linter), str(formal_dir)],
        cwd=formal_dir,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=120,
        check=False,
        env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
    )
    _require(result.returncode == 0, f"primed-assignment lint failed:\n{result.stdout}")

    return {
        "observer": "f2sBypassObserved",
        "receiver_count": len(RECEIVERS),
        "producer_count": len(F2S_PRODUCERS),
        "linter_output": result.stdout.strip(),
    }


def _check_bypass_mutant(formal_dir: Path) -> dict[str, Any]:
    config = (formal_dir / "AssignmentFenceNetworkF2SBypassMutant.cfg").read_text(
        encoding="utf-8"
    )
    _require("MutantF2SBypass = TRUE" in config, "bypass mutant must be enabled")
    _require("INVARIANT F2SStrictlyIncreasing" in config,
             "bypass config must check its directly named property")

    trace_path = (
        formal_dir
        / "trace-manifests"
        / "AssignmentFenceNetworkF2SBypassMutant.manifest.template.json"
    )
    trace = _load_json(trace_path)
    _require(trace.get("property") == "F2SStrictlyIncreasing",
             "bypass trace property mismatch")
    serialized = json.dumps(trace, sort_keys=True)
    for token in FORBIDDEN_ABSOLUTE_HISTORY:
        _require(token not in serialized, f"bypass trace retains {token!r}")
    events = trace.get("events")
    _require(isinstance(events, list), "bypass trace events must be an array")
    consume = next(
        (event for event in events if event.get("name") == "SConsumeReadyA1BeforeA0"),
        None,
    )
    _require(isinstance(consume, dict), "missing quotient bypass event")
    predicates = consume.get("all")
    _require(
        isinstance(predicates, list)
        and {
            "path": "f2sBypassObserved",
            "from": False,
            "to": True,
        }
        in predicates,
        "bypass event must validate FALSE->TRUE observer transition",
    )
    final_all = trace.get("final_all")
    _require(
        isinstance(final_all, list)
        and {"path": "f2sBypassObserved", "eq": True} in final_all,
        "bypass trace must validate the monotone final observer",
    )
    return {
        "config": "AssignmentFenceNetworkF2SBypassMutant.cfg",
        "property": "F2SStrictlyIncreasing",
        "trace": str(trace_path),
    }



def _check_liveness_and_compaction(
    matrix: dict[str, Any], formal_dir: Path
) -> dict[str, Any]:
    network = (formal_dir / "AssignmentFenceNetwork.tla").read_text(encoding="utf-8")
    compaction = (formal_dir / "AssignmentFenceNetworkCompaction.tla").read_text(
        encoding="utf-8"
    )
    operators = _operators(network)
    compaction_ops = _operators(compaction)

    unique = operators.get("QueueEntriesUnique", "")
    for queue in ("s2f", "f2s", "s2d", "c2f"):
        _require(
            f"Cardinality(SeqElems({queue})) = Len({queue})" in unique,
            f"QueueEntriesUnique must bound reusable keys in {queue}",
        )
    _require(
        "/\\ QueueEntriesUnique" in operators.get("SafetyInvariant", ""),
        "SafetyInvariant must include QueueEntriesUnique",
    )

    progress = operators.get("NoPermanentQueuedFrame", "")
    for fragment in (
        "MessageQueued(s2f, kind, a)",
        "MessageQueued(f2s, kind, a)",
        "MessageQueued(s2d, kind, a)",
        "ClaimQueued(a, exact)",
    ):
        _require(fragment in progress, f"per-frame liveness is missing {fragment}")
    for obsolete in (
        "Len(s2f) = 0",
        "Len(f2s) = 0",
        "Len(s2d) = 0",
        "Len(c2f) = 0",
    ):
        _require(obsolete not in progress, f"liveness restored total-empty premise {obsolete}")

    rows = {row["id"]: row for row in matrix["checks"]}
    liveness = rows["network-fair-liveness"]
    _require(
        liveness.get("property") == "NoPermanentQueuedFrame",
        "network liveness row must check NoPermanentQueuedFrame",
    )
    _require(
        liveness.get("workers") == 1,
        "network liveness must run with exactly one TLC worker",
    )

    direct_property = "NoStartAfterRevokedCompaction"
    config_path = formal_dir / "AssignmentFenceNetworkDefaultAllowMutant.cfg"
    config = config_path.read_text(encoding="utf-8")
    _require(
        f"INVARIANT {direct_property}" in config,
        "default-allow mutant must check the revoked-compaction property directly",
    )
    default_row = rows["network-default-allow-after-compaction-mutant"]
    _require(
        default_row.get("property") == direct_property,
        "default-allow matrix row must name the revoked-compaction property",
    )
    direct = compaction_ops.get(direct_property, "")
    _require(
        'releaseCause[a] = "Revoked"' in direct and "startAfterRelease[a] = 0" in direct,
        "revoked-compaction property must distinguish the intended release path",
    )
    trace_path = (
        formal_dir
        / "trace-manifests"
        / "AssignmentFenceNetworkDefaultAllowMutant.manifest.template.json"
    )
    trace = _load_json(trace_path)
    _require(
        trace.get("property") == direct_property,
        "default-allow trace manifest property mismatch",
    )
    required = trace.get("required_subsequence")
    _require(isinstance(required, list), "default-allow trace requires a subsequence")
    serialized_required = " ".join(str(item) for item in required)
    for premise in ("Revoke", "Revoked", "Compact", "StartsAfterRelease"):
        _require(
            premise.lower() in serialized_required.lower(),
            f"default-allow trace omits the {premise} premise",
        )

    quotient_test = formal_dir / "assignment_fence_fifo_quotient_test.py"
    result = subprocess.run(
        [sys.executable, str(quotient_test)],
        cwd=formal_dir,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=120,
        check=False,
        env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
    )
    _require(result.returncode == 0, f"FIFO quotient self-test failed:\n{result.stdout}")

    return {
        "liveness_property": "NoPermanentQueuedFrame",
        "default_allow_property": direct_property,
        "quotient_test_output": result.stdout.strip(),
    }

def check_contract(manifest: Path, repo: Path, formal_dir: Path) -> dict[str, Any]:
    repo = repo.resolve()
    formal_dir = formal_dir.resolve()
    _require(formal_dir == (repo / "formal").resolve(),
             "formal-dir must be the repository formal directory")
    _require(formal_dir.is_dir(), f"missing formal directory: {formal_dir}")
    matrix = _check_manifest(manifest, formal_dir)
    quotient = _check_quotient(formal_dir)
    bypass = _check_bypass_mutant(formal_dir)
    progress = _check_liveness_and_compaction(matrix, formal_dir)
    return {
        "schema": 1,
        "status": "PASS",
        "manifest": str(manifest.resolve()),
        "check_count": len(matrix["checks"]),
        "proof_count": len(matrix["proofs"]),
        "groups": {"core": 5, "network": 9},
        "quotient": quotient,
        "bypass_mutant": bypass,
        "progress_and_compaction": progress,
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--formal-dir", type=Path, required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        result = check_contract(args.manifest, args.repo, args.formal_dir)
    except (ContractError, OSError, subprocess.SubprocessError) as exc:
        print(f"assignment-fence static check failed: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
