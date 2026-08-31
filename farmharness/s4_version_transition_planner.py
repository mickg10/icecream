#!/usr/bin/env python3
"""Plan and audit the bounded S4 version-transition matrix.

This module is deliberately declarative.  It enumerates role versions and
their transitions, but never builds, starts a process, invokes Docker/SSH,
or calls :mod:`s4_real_cells`.  A later runner may bind the descriptors to
immutable artifacts and produce evidence for them.

Roles are ordered ``S, C, F`` (scheduler, client, worker).  The required
real-artifact state space is ``{43, 44, 50} ** 3``.  P48/P49 are retained as
boundary/formal/fixture metadata only: they are not silently promoted to
real-artifact states without an exact build authority.
"""

from __future__ import annotations

import argparse
import itertools
import json
import re
import sys
from pathlib import Path
from typing import Any, Mapping, Sequence


SCHEMA = "icecream-s4-version-transition-plan-v1"
ROLES = ("S", "C", "F")
REAL_VERSIONS = (43, 44, 50)
BOUNDARY_VERSIONS = (48, 49)
ALL_VERSIONS = REAL_VERSIONS + BOUNDARY_VERSIONS
P50_METHODS = ("ZSTD_TU", "ZSTD_ROUTE", "P29", "GRZ")
P50_DEPTHS = ("100", "200", "full", "repeat-full")
P50_REGIMES = ("cold", "warm")
P50_ORDERS = ("AB", "BA")
P50_TOPOLOGIES: tuple[dict[str, Any], ...] = (
    {"id": "C1F1/100000", "f_relationships": 1,
     "slots_per_f": 1, "global_slots": 1, "stream_capacity_tus": 100000},
    {"id": "C1F20/40", "f_relationships": 20,
     "slots_per_f": 2, "global_slots": 40, "stream_capacity_tus": None,
     "stream_capacity_status": "NOT_DECLARED"},
)

# The source authorities are intentionally explicit.  P44 is not a typo for
# P43: it is the distinct proto/cache lineage and its comm.h assertion is
# checked in the repository from which this plan was prepared.
P43_SOURCE_SHA = "cd74801e0fa4e83e3ae254ca1d7fe98642f36b89"
P44_SOURCE_SHA = "8c8cb881f8f5f8b7c10a608885f1b66e8e6ee7d3"
P50_SOURCE_SHA = "0aa537f746d41386aed49a88905406a2514b76f5"
P44_PROTOCOL_ASSERTION = "#define PROTOCOL_VERSION 44"

# Inventory from farmharness/s4_real_cells.py at the planning head.  These
# are contracts, not evidence that a final artifact has already been built.
HARNESS_INVENTORY: dict[str, Any] = {
    "path": "farmharness/s4_real_cells.py",
    "schema": "icecream-s4-real-cell-v1",
    "roles": list(ROLES),
    "binary_relpaths": {
        "S": ["obj/scheduler/icecc-scheduler", "scheduler/icecc-scheduler"],
        "C": ["obj/client/icecc", "client/icecc"],
        "F": ["obj/daemon/iceccd", "daemon/iceccd"],
        "E": ["obj/client/icecc-create-env", "client/icecc-create-env"],
        "X": ["cache/icecc-cache-service"],
    },
    "byte_exact_fields": ["S4_REMOTE_COMPILE", "S4_BYTE_IDENTICAL"],
    "legacy_transport_markers": [
        "write_fd_to_server from cpp", "write_fd_to_server preprocessed",
        "building_local", "local build forced", "fallback_local",
    ],
    "p50_cache_cell": "s50-c50-f50",
    "p43_source_sha_in_runner": P43_SOURCE_SHA,
    # The runner's retained P50 build contract is a BASE_SHA build.  Keep the
    # value visible so an artifact binder cannot mistake it for this plan's
    # current-head authority without an explicit refresh.
    "runner_p50_build_source_sha": "04006b9d94161a047154121f47785aec747ffd87",
    "runner_p50_root_required": True,
    "runner_p43_root": "$HOME/role-artifacts/store/p43/6da186b6c4c10f2f15d9e5440156ed890112523f53e905f9dd0d2f314540a3ae",
}

# Existing retained binary digests are useful to an artifact binder for P43
# and for the runner's P50 build.  P44 has no retained binary digest here;
# source authority alone must never be presented as an executable artifact.
RETAINED_ROLE_HASHES: dict[str, dict[str, str]] = {
    "43": {
        "S": "c205c1347064503b3ab7c56af4584ffb0806f52e2c5786d343883db871c44525",
        "F": "62bcc05ad91461212cd9616d960905c97c839b316aac3a9e1e1080608a193441",
        "C": "c3dc54eadca303f21eaa1f90c265a38a1038378be08f887b24bb3bbea7dabeae",
        "E": "aab94b6ea8f41335de807f814d24a56e827ce5efaf8b06797a365699e83b36cb",
    },
    "50_runner_build": {
        "S": "f1bfc8e6b4fb44815b00efa281c36b0ee35a0faeffb394a320ae908a7fb60750",
        "F": "b803bf877ff4ff5023a8f96819bd86c530ab54e32405e1f6d12fee9c68126b19",
        "C": "781804278af9aff93bff9d4f2f87a6d3152bc1cbe1804e41b926b1a0f22ebb9d",
        "E": "ee7d30b240c38bccf66d4afcdd45993f115a01d4a2fb4e9143d38596609d2ba4",
        "X": "5af26a01bc98fd6070b8c1e075b68f5969f1d15fb08aa1a231dd9319d238a062",
    },
}

# The current source's profile registry names GRZ_RESIDUAL at the harness
# boundary but accepts GRZ in the product-facing environment variable.  The
# planner uses the shorter product method name requested by S4.
P50_METHOD_CONTRACTS: dict[str, dict[str, Any]] = {
    "ZSTD_TU": {"source_profile": "ZSTD_TU", "implemented": True},
    "ZSTD_ROUTE": {"source_profile": "ZSTD_ROUTE", "implemented": True},
    "P29": {"source_profile": "P29", "implemented": True},
    "GRZ": {"source_profile": "GRZ_RESIDUAL", "implemented": True,
            "build_requirement": "exact current P50 build with ICECC_P50_WITH_LIBBSC"},
}

# These names occur in historical/planning material but are absent from the
# current P50CacheProfileRequest/CACHE_ADVERTISABLE_PROFILE_MASK inventory.
# Keep them visible as opt-in future fixtures, never as required methods.
OPTIONAL_UNACCEPTED_METHOD_EXTRAS: dict[str, dict[str, Any]] = {
    "ZSTD_COHORT": {
        "implemented": False, "accepted": False, "required": False,
        "source_inventory": {
            "files": ["services/comm.h"],
            "finding": "absent from P50CacheProfileRequest and advertised profile mask",
        },
    },
    "ZSTD_GLOBAL": {
        "implemented": False, "accepted": False, "required": False,
        "source_inventory": {
            "files": ["services/comm.h"],
            "finding": "global resource model is lifecycle state, not a current cache method/profile",
        },
    },
}

P44_CACHE_AMBIGUITY = (
    "P44 is a distinct proto/cache source lineage, but this retained artifact "
    "contract does not decide whether an all-P44 binary should exercise its old "
    "cache path. The planner therefore requires zero P50 cache traffic and "
    "does not preregister old-P44 cache engagement; a dedicated P44 cache arm "
    "needs exact binary/runtime authority and an explicit reviewed decision."
)

_STATE_VALUES = tuple(int(value) for value in REAL_VERSIONS)
STATIC_STATES: tuple[tuple[int, int, int], ...] = tuple(
    itertools.product(_STATE_VALUES, repeat=3)
)
STATE_SET = frozenset(STATIC_STATES)
ORDERED_STATE_PAIRS: tuple[tuple[tuple[int, int, int], tuple[int, int, int]], ...] = tuple(
    itertools.product(STATIC_STATES, repeat=2)
)
class PlannerError(ValueError):
    """Raised for malformed planner input or an invariant violation."""


def state_id(state: Sequence[int]) -> str:
    """Return the stable wire-free identifier for an S/C/F state."""
    if len(state) != 3 or any(value not in REAL_VERSIONS for value in state):
        raise PlannerError(f"invalid S/C/F state: {state!r}")
    return "s{}-c{}-f{}".format(*state)


def parse_state(value: object) -> tuple[int, int, int]:
    """Parse either a three-item sequence or ``sN-cN-fN`` identifier."""
    if isinstance(value, str):
        match = re.fullmatch(r"s(\d+)-c(\d+)-f(\d+)", value)
        if not match:
            raise PlannerError(f"invalid state id: {value!r}")
        state = tuple(int(item) for item in match.groups())
    elif isinstance(value, Sequence) and not isinstance(value, (bytes, bytearray)):
        state = tuple(value)
    else:
        raise PlannerError(f"invalid state: {value!r}")
    if len(state) != 3 or any(type(item) is not int or item not in REAL_VERSIONS for item in state):
        raise PlannerError(f"invalid S/C/F state: {value!r}")
    return state  # type: ignore[return-value]


def homogeneous(state: Sequence[int]) -> bool:
    parsed = parse_state(state)
    return len(set(parsed)) == 1


def all_p50(state: Sequence[int]) -> bool:
    return parse_state(state) == (50, 50, 50)


def changed_roles(before: Sequence[int], after: Sequence[int]) -> tuple[str, ...]:
    left, right = parse_state(before), parse_state(after)
    return tuple(role for role, old, new in zip(ROLES, left, right) if old != new)


def transition_class(before: Sequence[int], after: Sequence[int]) -> str:
    count = len(changed_roles(before, after))
    if count == 0:
        return "no-op"
    if count == 1:
        return "one-role"
    return "multi-role"


# Small named entry points make the matrix useful to auditors without
# requiring callers to know how the immutable tuples are stored.
def enumerate_states() -> tuple[tuple[int, int, int], ...]:
    return STATIC_STATES


def enumerate_transitions() -> tuple[tuple[tuple[int, int, int], tuple[int, int, int]], ...]:
    return ORDERED_STATE_PAIRS


def classify_transition(before: Sequence[int], after: Sequence[int]) -> str:
    return transition_class(before, after)


def expected_cache_engagement(state: Sequence[int]) -> bool:
    """Return the preregistered P50-cache expectation for a current state."""
    return all_p50(state)


def artifact_binding_contract() -> dict[str, Any]:
    """Describe the receipt boundary used by the later S4 matrix runner.

    This is metadata only.  Receipt validation and file hashing live in
    :mod:`s4_artifact_binder`; keeping the contract here makes a serialized
    plan self-describing without performing any binding or build work.
    """
    return {
        "schema": "icecream-s4-artifact-manifest-v1",
        "receipt_schema": "icecream-s4-build-receipt-v1",
        "versions": [43, 44, 50],
        "roles": {"43": ["S", "C", "F", "E"],
                   "44": ["S", "C", "F", "E"],
                   "50": ["S", "C", "F", "E", "X"]},
        "state_count": 27,
        "transition_count": 729,
        "p43_source_commit": P43_SOURCE_SHA,
        "p44_source_commit": P44_SOURCE_SHA,
        # P50_SOURCE_SHA is planner metadata.  The runtime receipt must use
        # the independent product build authority retained by real_cells.
        "p50_planner_source_commit": P50_SOURCE_SHA,
        "p50_runtime_source_commit": HARNESS_INVENTORY["runner_p50_build_source_sha"],
        "p44_status_without_receipt": "NOT_BOUND",
        "protocol_assertions": {
            "43": "#define PROTOCOL_VERSION 43",
            "44": P44_PROTOCOL_ASSERTION,
            "50": "#define PROTOCOL_VERSION 50",
        },
    }


def _artifact_contract(version: int) -> dict[str, Any]:
    if version == 43:
        return {
            "version": 43, "label": "P43", "source_commit": P43_SOURCE_SHA,
            "release_tag": "1.4", "authority": "exact-release-tag",
            "required_real_artifact": True,
            "runtime_identity": "true-P43-binaries",
            "runtime_required": True, "cache_mode": "legacy-only",
            "cache_disabled": True,
            "p50_cache_traffic": "forbidden",
            "retained_role_hashes": RETAINED_ROLE_HASHES["43"],
        }
    if version == 44:
        return {
            "version": 44, "label": "P44", "source_commit": P44_SOURCE_SHA,
            "release_tag": None, "authority": "exact-proto-cache-lineage",
            "protocol_assertion": {"path": "services/comm.h", "text": P44_PROTOCOL_ASSERTION},
            "required_real_artifact": True,
            "runtime_identity": "true-P44-binaries",
            "runtime_required": True, "cache_mode": "legacy-only-until-reviewed",
            "cache_disabled": True,
            "p50_cache_traffic": "forbidden", "retained_role_hashes": None,
            "cache_expectation": "UNRESOLVED_OLD_P44_BEHAVIOR",
        }
    if version == 50:
        return {
            "version": 50, "label": "P50", "source_commit": P50_SOURCE_SHA,
            "release_tag": None, "authority": "current-integration-head",
            "required_real_artifact": True,
            "runtime_identity": "true-P50-binaries",
            "runtime_required": True, "cache_mode": "current-methods-only",
            "cache_disabled": False,
            "p50_cache_traffic": "allowed-only-all-roles-P50",
            "retained_role_hashes": None,
            "legacy_mode_variant": "required-for-P50-homogeneous-performance-arm",
        }
    raise PlannerError(f"unknown real-artifact version: {version}")


def _boundary_contract(version: int) -> dict[str, Any]:
    return {
        "version": version,
        "label": f"P{version}",
        "authority": "boundary-formal-fixture-only",
        "required_real_artifact": False,
        "exact_build_authority": False,
        "included_in_static_states": False,
        "purpose": "negotiation/formal/fixture boundary coverage",
    }


def _state_descriptor(state: tuple[int, int, int]) -> dict[str, Any]:
    cache = all_p50(state)
    partial = any(value == 50 for value in state) and not cache
    return {
        "id": state_id(state),
        "roles": {role: value for role, value in zip(ROLES, state)},
        "scheduler_version": state[0], "client_version": state[1],
        "worker_version": state[2],
        "tuple": list(state),
        "homogeneous": homogeneous(state),
        "cache_expected": cache,
        "p50_cache_traffic": "expected" if cache else "zero-required",
        "partial_p50_cache_traffic": "forbidden" if partial else "not-applicable",
        "remote_output": "byte-exact-required",
        "legacy_wire": "not-required" if cache else "byte-exact-required",
        "old_p44_cache_behavior": "UNRESOLVED" if state == (44, 44, 44) else "not-applicable",
    }


def _transition_descriptor(before: tuple[int, int, int], after: tuple[int, int, int]) -> dict[str, Any]:
    roles = changed_roles(before, after)
    cache = all_p50(after)
    return {
        "before": state_id(before), "after": state_id(after),
        "before_roles": {role: value for role, value in zip(ROLES, before)},
        "after_roles": {role: value for role, value in zip(ROLES, after)},
        "before_tuple": list(before), "after_tuple": list(after),
        "changed_roles": list(roles), "changed_count": len(roles),
        "classification": transition_class(before, after),
        "cache_expected_after": cache,
        "p50_cache_traffic_after": "expected" if cache else "zero-required",
        "remote_output": "byte-exact-required",
        "legacy_wire": "not-required" if cache else "byte-exact-required",
    }


def _migration_orders(lower: int, upper: int, direction: str) -> list[dict[str, Any]]:
    if direction == "upgrade":
        start, finish = lower, upper
    elif direction == "downgrade":
        start, finish = upper, lower
    else:
        raise PlannerError(f"invalid migration direction: {direction}")
    result: list[dict[str, Any]] = []
    for order in itertools.permutations(ROLES):
        current = [start, start, start]
        path = [state_id(tuple(current))]
        for role in order:
            current[ROLES.index(role)] = finish
            path.append(state_id(tuple(current)))
        result.append({"order": list(order), "states": path})
    return result


def _measurement_contract() -> dict[str, Any]:
    """Return the frozen dimensions for each required P50 method arm."""
    def arm(method: str, mode: str, *, cache_expected: bool) -> dict[str, Any]:
        return {
            "name": "P50_RAW_II" if method == "RAW_II" else f"P50_{method}",
            "artifact_version": 50, "artifact": "P50",
            "state": "s50-c50-f50", "method": method, "mode": mode,
            "cache_expected": cache_expected, "cache_disabled": not cache_expected,
            "byte_identical_required": True,
        }

    cells = [
        {
            "id": f"p50-{method.lower()}-{depth}-{topology['id']}-{regime}-{order}",
            "state": "s50-c50-f50", "method": method,
            "depth": depth, "topology": topology["id"], "regime": regime,
            "order": order, "counterbalanced_pair": f"{method}/{depth}/{topology['id']}/{regime}",
            "cache_expected": True, "remote_compile_required": True,
            "byte_identical_required": True,
            # The block's order is authoritative.  Exactly one two-arm
            # sequence is retained, avoiding an ambiguous four-arm block.
            "sequence": ([arm("RAW_II", "whole-legacy", cache_expected=False),
                           arm(method, "current", cache_expected=True)]
                          if order == "AB" else
                          [arm(method, "current", cache_expected=True),
                           arm("RAW_II", "whole-legacy", cache_expected=False)]),
        }
        for method in P50_METHODS
        for depth in P50_DEPTHS
        for topology in P50_TOPOLOGIES
        for regime in P50_REGIMES
        for order in P50_ORDERS
    ]
    return {
        "scope": "homogeneous P50 only",
        "state": "s50-c50-f50",
        "methods": list(P50_METHODS),
        "method_contracts": P50_METHOD_CONTRACTS,
        "depths": list(P50_DEPTHS),
        "topologies": [dict(topology) for topology in P50_TOPOLOGIES],
        "regimes": list(P50_REGIMES),
        "orders": list(P50_ORDERS),
        "counterbalanced": True,
        "counterbalance_unit": "method/depth/topology/regime pair with AB and BA",
        "output_contract": {
            "remote_compile": True,
            "byte_identical": True,
            "failed_or_censored_rows_retained": True,
        },
        "measurement_cells": cells,
        "comparison_block_count": len(cells),
        "execution_run_count": len(cells) * 2,
        "measurement_cell_count": len(cells),
        "execution": {
            "planned_only": True,
            "runner": "later S4/S5 bound runner",
            "preparation_outside_measurement": True,
            "no_mixed_version_performance": True,
        },
    }


def _version_comparison_contract() -> dict[str, Any]:
    """Build mandatory old-version-vs-P50 RAW_II comparison blocks."""
    def arm(version: int, artifact: str) -> dict[str, Any]:
        return {
            "name": f"P{version}_WHOLE_LEGACY" if version != 50 else "P50_RAW_II",
            "artifact_version": version, "artifact": artifact,
            "state": f"s{version}-c{version}-f{version}",
            "method": "WHOLE_LEGACY" if version != 50 else "RAW_II",
            "mode": "legacy", "cache_expected": False, "cache_disabled": True,
            "byte_identical_required": True,
        }

    blocks: list[dict[str, Any]] = []
    for old_version in (43, 44):
        for depth in P50_DEPTHS:
            for topology in P50_TOPOLOGIES:
                for regime in P50_REGIMES:
                    for order in P50_ORDERS:
                        old = arm(old_version, f"P{old_version}")
                        current = arm(50, "P50")
                        blocks.append({
                            "id": f"p{old_version}-vs-p50-raw-ii-{depth}-{topology['id']}-{regime}-{order}",
                            "comparison_pair": f"P{old_version}_WHOLE_LEGACY_vs_P50_RAW_II",
                            "old_version": old_version, "state": f"s{old_version}-vs-s50",
                            "depth": depth, "topology": topology["id"], "regime": regime,
                            "order": order,
                            "sequence": [old, current] if order == "AB" else [current, old],
                            "cache_expected": False, "p50_cache_traffic": "zero-required",
                            "byte_identical_required": True,
                        })
    return {
        "scope": "homogeneous version cost only",
        "pairs": ["P43_WHOLE_LEGACY_vs_P50_RAW_II", "P44_WHOLE_LEGACY_vs_P50_RAW_II"],
        "depths": list(P50_DEPTHS),
        "topologies": [dict(topology) for topology in P50_TOPOLOGIES],
        "regimes": list(P50_REGIMES), "orders": list(P50_ORDERS),
        "counterbalanced": True,
        "no_cache": True,
        "blocks": blocks,
        "comparison_block_count": len(blocks),
        "execution_run_count": len(blocks) * 2,
        "output_contract": {"byte_identical": True},
        "optional_diagnostic": {
            "pair": "P43_WHOLE_LEGACY_vs_P44_WHOLE_LEGACY",
            "required": False, "accepted": False,
            "reason": "optional diagnostic; no additional mandatory cost",
        },
    }


def build_plan() -> dict[str, Any]:
    """Build the complete deterministic plan, without executing any arm."""
    states = [_state_descriptor(state) for state in STATIC_STATES]
    transitions = [
        _transition_descriptor(before, after)
        for before, after in ORDERED_STATE_PAIRS
    ]
    upgrade_orders = {
        "43_to_50": _migration_orders(43, 50, "upgrade"),
        "44_to_50": _migration_orders(44, 50, "upgrade"),
    }
    downgrade_orders = {
        "50_to_43": _migration_orders(43, 50, "downgrade"),
        "50_to_44": _migration_orders(44, 50, "downgrade"),
    }
    performance_arms = {
        "p43_homogeneous_legacy": {
            "state": "s43-c43-f43", "source_version": 43,
            "method": "WHOLE_LEGACY", "mode": "legacy", "cache_expected": False,
            "cache_disabled": True,
            "artifact_requirement": "exact P43 release-tag binaries",
        },
        "p44_homogeneous_legacy": {
            "state": "s44-c44-f44", "source_version": 44,
            "method": "WHOLE_LEGACY", "mode": "legacy", "cache_expected": False,
            "cache_disabled": True,
            "artifact_requirement": "exact P44 proto/cache-lineage binaries",
            "old_p44_cache_behavior": "UNRESOLVED",
        },
        "p50_raw_ii_whole_legacy": {
            "state": "s50-c50-f50", "source_version": 50,
            "method": "RAW_II", "mode": "whole-legacy", "cache_expected": False,
            "cache_disabled": True,
            "artifact_requirement": "exact current P50 binaries; legacy/cache disabled",
        },
    }
    for method in P50_METHODS:
        key = f"p50_{method.lower()}"
        performance_arms[key] = {
            "state": "s50-c50-f50", "source_version": 50,
            "method": method, "mode": "current", "cache_expected": True,
            "cache_disabled": False,
            "artifact_requirement": "exact current P50 binaries and cache service",
            "measurement_contract": "execution_measurement_contract",
    }
    measurement_contract = _measurement_contract()
    version_comparison_contract = _version_comparison_contract()
    return {
        "schema": SCHEMA,
        "source": {"integration_head": P50_SOURCE_SHA, "harness": HARNESS_INVENTORY},
        "artifact_binding_contract": artifact_binding_contract(),
        "roles": list(ROLES),
        "real_artifact_versions": [
            _artifact_contract(version) for version in REAL_VERSIONS
        ],
        "boundary_extras": [
            _boundary_contract(version) for version in BOUNDARY_VERSIONS
        ],
        "p44_cache_ambiguity": P44_CACHE_AMBIGUITY,
        "states": states,
        "state_count": len(states),
        "transitions": transitions,
        "transition_count": len(transitions),
        "transition_class_counts": {"no-op": 27, "one-role": 162, "multi-role": 540},
        "homogeneous_states": ["s43-c43-f43", "s44-c44-f44", "s50-c50-f50"],
        "homogeneous_version_arms": {
            "P43": "p43_homogeneous_legacy",
            "P44": "p44_homogeneous_legacy",
            "P50": "p50_raw_ii_whole_legacy",
        },
        "performance_arms": performance_arms,
        "execution_measurement_contract": measurement_contract,
        "homogeneous_version_comparison_contract": version_comparison_contract,
        "optional_unaccepted_method_extras": OPTIONAL_UNACCEPTED_METHOD_EXTRAS,
        "mixed_version_performance": "forbidden-correctness-only",
        "upgrade_orders": upgrade_orders,
        "downgrade_orders": downgrade_orders,
        "migration_orders": {
            "43_to_50": {"upgrade": upgrade_orders["43_to_50"], "downgrade": downgrade_orders["50_to_43"]},
            "44_to_50": {"upgrade": upgrade_orders["44_to_50"], "downgrade": downgrade_orders["50_to_44"]},
        },
    }


def audit_plan(plan: Mapping[str, Any]) -> dict[str, Any]:
    """Audit a generated or serialized plan and return a fail-closed summary."""
    errors: list[str] = []
    if plan.get("schema") != SCHEMA:
        errors.append("schema-mismatch")
    if tuple(plan.get("roles", ())) != ROLES:
        errors.append("role-order-mismatch")
    for version, expected in ((43, P43_SOURCE_SHA), (44, P44_SOURCE_SHA), (50, P50_SOURCE_SHA)):
        row = next((item for item in plan.get("real_artifact_versions", ())
                    if isinstance(item, Mapping) and item.get("version") == version), None)
        if row is None:
            errors.append(f"missing-artifact-P{version}")
        elif row.get("source_commit") != expected:
            errors.append(f"artifact-P{version}-source-mismatch")
        elif version == 44:
            assertion = row.get("protocol_assertion")
            if not isinstance(assertion, Mapping) or assertion.get("text") != P44_PROTOCOL_ASSERTION:
                errors.append("artifact-P44-protocol-assertion-mismatch")
    states = plan.get("states", ())
    state_ids: list[str] = []
    if not isinstance(states, list) or len(states) != 27:
        errors.append("state-count")
        states = []
    for item in states:
        if not isinstance(item, Mapping):
            errors.append("state-not-object")
            continue
        try:
            parsed = parse_state(item.get("tuple"))
            state_ids.append(state_id(parsed))
            if item.get("id") != state_id(parsed):
                errors.append(f"state-id:{item.get('id')}")
            expected_cache = all_p50(parsed)
            if item.get("cache_expected") is not expected_cache:
                errors.append(f"state-cache-policy:{state_id(parsed)}")
            if item.get("remote_output") != "byte-exact-required":
                errors.append(f"state-byte-exact:{state_id(parsed)}")
            if not expected_cache and item.get("p50_cache_traffic") != "zero-required":
                errors.append(f"state-p50-traffic:{state_id(parsed)}")
        except PlannerError as exc:
            errors.append(f"state-invalid:{exc}")
    if frozenset(state_ids) != frozenset(state_id(state) for state in STATIC_STATES) and len(state_ids) == 27:
        errors.append("state-grid-mismatch")

    transitions = plan.get("transitions", ())
    if not isinstance(transitions, list) or len(transitions) != 729:
        errors.append("transition-count")
        transitions = []
    counts = {name: 0 for name in ("no-op", "one-role", "multi-role")}
    seen_pairs: set[tuple[str, str]] = set()
    for item in transitions:
        if not isinstance(item, Mapping):
            errors.append("transition-not-object")
            continue
        try:
            before = parse_state(item.get("before_tuple"))
            after = parse_state(item.get("after_tuple"))
            pair = (state_id(before), state_id(after))
            seen_pairs.add(pair)
            classification = transition_class(before, after)
            counts[classification] += 1
            if item.get("before") != pair[0] or item.get("after") != pair[1]:
                errors.append(f"transition-id:{pair}")
            if item.get("classification") != classification:
                errors.append(f"transition-class:{pair}")
            if item.get("changed_roles") != list(changed_roles(before, after)):
                errors.append(f"transition-roles:{pair}")
            expected_cache = all_p50(after)
            if item.get("cache_expected_after") is not expected_cache:
                errors.append(f"transition-cache:{pair}")
            if not expected_cache and item.get("p50_cache_traffic_after") != "zero-required":
                errors.append(f"transition-p50-traffic:{pair}")
        except PlannerError as exc:
            errors.append(f"transition-invalid:{exc}")
    if len(seen_pairs) != 729:
        errors.append("transition-grid-mismatch")
    if counts != {"no-op": 27, "one-role": 162, "multi-role": 540}:
        errors.append("transition-class-counts")

    for key, expected_start, expected_finish in (
        ("43_to_50", "s43-c43-f43", "s50-c50-f50"),
        ("44_to_50", "s44-c44-f44", "s50-c50-f50"),
    ):
        orders = plan.get("upgrade_orders", {}).get(key, ())
        if not isinstance(orders, list) or len(orders) != 6:
            errors.append(f"upgrade-order-count:{key}")
            continue
        for order in orders:
            path = order.get("states", ()) if isinstance(order, Mapping) else ()
            if len(path) != 4 or path[0] != expected_start or path[-1] != expected_finish:
                errors.append(f"upgrade-order-path:{key}")
    for key, expected_start, expected_finish in (
        ("50_to_43", "s50-c50-f50", "s43-c43-f43"),
        ("50_to_44", "s50-c50-f50", "s44-c44-f44"),
    ):
        orders = plan.get("downgrade_orders", {}).get(key, ())
        if not isinstance(orders, list) or len(orders) != 6:
            errors.append(f"downgrade-order-count:{key}")
            continue
        for order in orders:
            path = order.get("states", ()) if isinstance(order, Mapping) else ()
            if len(path) != 4 or path[0] != expected_start or path[-1] != expected_finish:
                errors.append(f"downgrade-order-path:{key}")

    arms = plan.get("performance_arms", {})
    required_arms = {
        "p43_homogeneous_legacy": ("s43-c43-f43", 43),
        "p44_homogeneous_legacy": ("s44-c44-f44", 44),
        "p50_raw_ii_whole_legacy": ("s50-c50-f50", 50),
        **{f"p50_{method.lower()}": ("s50-c50-f50", 50) for method in P50_METHODS},
    }
    if "p50_homogeneous_current" in arms:
        errors.append("generic-p50-current-arm-forbidden")
    if not isinstance(arms, Mapping):
        errors.append("performance-arms-not-object")
        arms = {}
    for key, (expected_state, expected_version) in required_arms.items():
        arm = arms.get(key)
        if not isinstance(arm, Mapping):
            errors.append(f"missing-performance-arm:{key}")
            continue
        if arm.get("state") != expected_state or arm.get("source_version") != expected_version:
            errors.append(f"performance-arm-identity:{key}")
        try:
            if not homogeneous(parse_state(arm.get("state"))):
                errors.append(f"mixed-performance-arm:{key}")
        except PlannerError:
            errors.append(f"performance-arm-state:{key}")
    contract = plan.get("execution_measurement_contract")
    if not isinstance(contract, Mapping):
        errors.append("execution-measurement-contract-missing")
        contract = {}
    if tuple(contract.get("methods", ())) != P50_METHODS:
        errors.append("measurement-methods")
    if tuple(contract.get("depths", ())) != P50_DEPTHS:
        errors.append("measurement-depths")
    if tuple(contract.get("regimes", ())) != P50_REGIMES:
        errors.append("measurement-regimes")
    if tuple(contract.get("orders", ())) != P50_ORDERS or contract.get("counterbalanced") is not True:
        errors.append("measurement-counterbalance")
    topology_ids = tuple(item.get("id") for item in contract.get("topologies", ())
                         if isinstance(item, Mapping))
    if topology_ids != tuple(item["id"] for item in P50_TOPOLOGIES):
        errors.append("measurement-topologies")
    measurements = contract.get("measurement_cells", ())
    expected_measurements = {
        (method, depth, topology["id"], regime, order)
        for method in P50_METHODS for depth in P50_DEPTHS
        for topology in P50_TOPOLOGIES for regime in P50_REGIMES
        for order in P50_ORDERS
    }
    observed_measurements: set[tuple[object, ...]] = set()
    if not isinstance(measurements, list) or len(measurements) != len(expected_measurements):
        errors.append("measurement-cell-count")
        measurements = []
    for item in measurements:
        if not isinstance(item, Mapping):
            errors.append("measurement-cell-not-object")
            continue
        identity = (item.get("method"), item.get("depth"), item.get("topology"),
                    item.get("regime"), item.get("order"))
        observed_measurements.add(identity)
        if item.get("state") != "s50-c50-f50" or item.get("cache_expected") is not True:
            errors.append(f"measurement-state:{item.get('id')}")
        if item.get("remote_compile_required") is not True or item.get("byte_identical_required") is not True:
            errors.append(f"measurement-byte-exact:{item.get('id')}")
        def arm_signature(value: object) -> tuple[object, ...] | None:
            if not isinstance(value, Mapping):
                return None
            return (value.get("name"), value.get("artifact_version"), value.get("artifact"),
                    value.get("state"), value.get("method"), value.get("mode"),
                    value.get("cache_expected"), value.get("cache_disabled"))
        raw_signature = ("P50_RAW_II", 50, "P50", "s50-c50-f50", "RAW_II", "whole-legacy", False, True)
        method_signature = (f"P50_{item.get('method')}", 50, "P50", "s50-c50-f50", item.get("method"), "current", True, False)
        sequence = item.get("sequence")
        expected_sequence = (raw_signature, method_signature) if item.get("order") == "AB" else (method_signature, raw_signature)
        if (item.get("order") not in P50_ORDERS or "AB" in item or "BA" in item or
                not isinstance(sequence, list) or len(sequence) != 2 or
                tuple(arm_signature(part) for part in sequence) != expected_sequence):
            errors.append(f"measurement-counterbalanced-arms:{item.get('id')}")
    if observed_measurements != expected_measurements:
        errors.append("measurement-grid-mismatch")
    if contract.get("comparison_block_count") != 128 or contract.get("execution_run_count") != 256:
        errors.append("measurement-run-count")

    version_contract = plan.get("homogeneous_version_comparison_contract")
    if not isinstance(version_contract, Mapping):
        errors.append("version-comparison-contract-missing")
        version_contract = {}
    version_blocks = version_contract.get("blocks", ())
    if (version_contract.get("comparison_block_count") != 64 or
            version_contract.get("execution_run_count") != 128 or
            version_contract.get("counterbalanced") is not True or
            version_contract.get("no_cache") is not True):
        errors.append("version-comparison-contract-count-or-policy")
    expected_version_keys = {
        (old, depth, topology["id"], regime, order)
        for old in (43, 44) for depth in P50_DEPTHS
        for topology in P50_TOPOLOGIES for regime in P50_REGIMES
        for order in P50_ORDERS
    }
    observed_version_keys: set[tuple[object, ...]] = set()
    if not isinstance(version_blocks, list) or len(version_blocks) != 64:
        errors.append("version-comparison-block-count")
        version_blocks = []
    for item in version_blocks:
        if not isinstance(item, Mapping):
            errors.append("version-comparison-block-not-object")
            continue
        key = (item.get("old_version"), item.get("depth"), item.get("topology"),
               item.get("regime"), item.get("order"))
        observed_version_keys.add(key)
        old = item.get("old_version")
        old_signature = (f"P{old}_WHOLE_LEGACY", old, f"P{old}", f"s{old}-c{old}-f{old}", "WHOLE_LEGACY", "legacy", False, True)
        p50_signature = ("P50_RAW_II", 50, "P50", "s50-c50-f50", "RAW_II", "legacy", False, True)
        def vsignature(value: object) -> tuple[object, ...] | None:
            if not isinstance(value, Mapping):
                return None
            return (value.get("name"), value.get("artifact_version"), value.get("artifact"),
                    value.get("state"), value.get("method"), value.get("mode"),
                    value.get("cache_expected"), value.get("cache_disabled"))
        sequence = item.get("sequence")
        expected_sequence = (old_signature, p50_signature) if item.get("order") == "AB" else (p50_signature, old_signature)
        if (old not in (43, 44) or item.get("order") not in P50_ORDERS or
                item.get("cache_expected") is not False or
                item.get("p50_cache_traffic") != "zero-required" or
                item.get("byte_identical_required") is not True or
                "AB" in item or "BA" in item or not isinstance(sequence, list) or len(sequence) != 2 or
                tuple(vsignature(part) for part in sequence) != expected_sequence):
            errors.append(f"version-comparison-arms:{item.get('id')}")
    if observed_version_keys != expected_version_keys:
        errors.append("version-comparison-grid-mismatch")

    extras = plan.get("boundary_extras", ())
    for version in BOUNDARY_VERSIONS:
        row = next((item for item in extras if isinstance(item, Mapping) and item.get("version") == version), None)
        if row is None or row.get("required_real_artifact") is not False or row.get("included_in_static_states") is not False:
            errors.append(f"boundary-P{version}-not-fixture-only")

    return {
        "schema": "icecream-s4-version-transition-audit-v1",
        "status": "PASS" if not errors else "FAIL",
        "errors": errors,
        "state_count": len(states),
        "transition_count": len(transitions),
        "transition_class_counts": counts,
        "ordered_pairs_unique": len(seen_pairs),
        "upgrade_order_counts": {key: len(plan.get("upgrade_orders", {}).get(key, ())) for key in ("43_to_50", "44_to_50")},
        "downgrade_order_counts": {key: len(plan.get("downgrade_orders", {}).get(key, ())) for key in ("50_to_43", "50_to_44")},
        "performance_arm_count": len(arms),
        "p50_measurement_count": len(measurements),
        "comparison_block_count": len(measurements),
        "execution_run_count": len(measurements) * 2,
        "version_comparison_block_count": len(version_blocks),
        "version_comparison_execution_run_count": len(version_blocks) * 2,
    }


def _json_bytes(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--audit", type=Path, help="audit a previously serialized plan JSON")
    parser.add_argument("--summary", action="store_true", help="emit only the audit summary")
    args = parser.parse_args(argv)
    if args.audit:
        try:
            plan = json.loads(args.audit.read_text(encoding="utf-8"))
            if not isinstance(plan, dict):
                raise PlannerError("plan JSON must be an object")
            result: object = audit_plan(plan)
        except (OSError, UnicodeError, json.JSONDecodeError, PlannerError) as exc:
            result = {"schema": "icecream-s4-version-transition-audit-v1", "status": "FAIL", "errors": [str(exc)]}
    else:
        plan = build_plan()
        result = audit_plan(plan) if args.summary else plan
    sys.stdout.buffer.write(_json_bytes(result))
    if isinstance(result, Mapping) and result.get("status") == "FAIL":
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
