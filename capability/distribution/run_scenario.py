#!/usr/bin/env python3
"""One scheduling and network simulator for every compression candidate.

The simulator owns workload release, F-slot reservation, placement, network sharing,
compile timing and the output ledger.  Codecs are adapters which return the ordered dialogue
phases for one dispatched TU.  They do not implement a second scheduler.

Two built-in adapters are deliberately simple references:

* ``compile-only`` transfers no bytes and isolates the measured compiler distribution.
* ``raw`` sends the complete preprocessed TU once from C to F.

Neither is a compression result.  GRZ and P29 plug into the same ``CodecAdapter`` boundary;
their physical adapters must also perform their reconstruction checks before a run can be
reported as a codec result.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import heapq
import json
import re
from collections import defaultdict, deque
from dataclasses import dataclass, field
from fractions import Fraction
from pathlib import Path, PurePosixPath
from typing import Iterable, Iterator, Mapping, Sequence


NANOSECONDS = 1_000_000_000
DIRECTIONS = ("c_to_f", "f_to_c")
DEFAULT_SNAPSHOT_NS = 10_000_000
MAX_REPORT_SNAPSHOTS = 2_000
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
EVENT_IDENTITY_FIELDS = (
    "logical_job_id",
    "attempt_id",
    "C_STORE_GUID",
    "physical_endpoint",
    "RouteLaneId",
    "F_STORE_GUID",
    "session_serial",
    "HISTORY_NONCE",
    "REL_SEQ",
    "TU_SEQ",
    "transaction_digest",
    "raw_digest",
    "negotiated_profiles",
    "route_state_profiles",
    "InputRecord_identity",
    "actor",
    "start_ns",
    "end_ns",
    "duration_ns",
    "provenance",
    "byte_account",
    "c_to_f_byte_delta",
    "f_to_c_byte_delta",
    "resource_byte_delta",
    "queue_byte_delta",
)


def ceil_fraction(value: Fraction) -> int:
    return value.numerator // value.denominator + (
        value.numerator % value.denominator != 0
    )


def checked_nonnegative_int(value: object, name: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise ValueError(f"{name} must be a non-negative integer")
    return value


def checked_positive_int(value: object, name: str) -> int:
    result = checked_nonnegative_int(value, name)
    if result == 0:
        raise ValueError(f"{name} must be positive")
    return result


@dataclass(frozen=True)
class TraceRow:
    logical: int
    job_id: str
    ii_relative: str
    raw_bytes: int
    compile_ns: int
    compile_model: str
    raw_sha256: str | None = None
    compile_provenance: str = "observed"


@dataclass
class WorkItem:
    ordinal: int
    environment: int
    workload: str
    build: int
    logical: int
    source_job_id: str
    payload: Path
    raw_bytes: int
    compile_ns: int
    release_offset_ns: int
    raw_digest: str | None = None
    compile_provenance: str = "observed"
    release_ns: int | None = None

    @property
    def key(self) -> tuple[str, int, int]:
        return self.workload, self.build, self.logical


@dataclass(frozen=True)
class Phase:
    name: str
    direction: str
    byte_count: int
    priority: int = 3
    account: str = "source"

    def __post_init__(self) -> None:
        if self.direction not in DIRECTIONS:
            raise ValueError(f"unknown phase direction {self.direction!r}")
        if self.byte_count < 0:
            raise ValueError("phase byte count is negative")
        checked_nonnegative_int(self.priority, "phase priority")
        if self.account not in {"source", "environment", "result"}:
            raise ValueError(f"unknown phase account {self.account!r}")


@dataclass(frozen=True)
class DagNode:
    """One physical frame/stream extent in a transaction dependency graph."""

    name: str
    direction: str
    byte_count: int
    dependencies: tuple[str, ...] = ()
    priority: int = 3

    def __post_init__(self) -> None:
        if not self.name or ":" in self.name:
            raise ValueError("DAG node name must be nonempty and contain no colon")
        if self.direction not in DIRECTIONS:
            raise ValueError(f"unknown DAG node direction {self.direction!r}")
        checked_positive_int(self.byte_count, f"DAG node {self.name} bytes")
        checked_nonnegative_int(self.priority, f"DAG node {self.name} priority")


@dataclass(frozen=True)
class TransactionPlan:
    """Fork/join transport graph and its independent readiness/commit joins."""

    nodes: tuple[DagNode, ...]
    initial_tokens: tuple[str, ...]
    input_ready_after: tuple[str, ...]
    commit_after: tuple[str, ...]

    def __post_init__(self) -> None:
        if not self.nodes:
            raise ValueError("transaction DAG has no nodes")
        names = [node.name for node in self.nodes]
        if len(names) != len(set(names)):
            raise ValueError("transaction DAG repeats a node name")
        initial = set(self.initial_tokens)
        if len(initial) != len(self.initial_tokens) or any(
            not token for token in initial
        ):
            raise ValueError("transaction DAG has empty or repeated initial tokens")
        allowed = initial | {
            f"{name}:{stage}" for name in names for stage in ("sent", "delivered")
        }
        for label, tokens in (
            ("input_ready_after", self.input_ready_after),
            ("commit_after", self.commit_after),
        ):
            if not tokens or len(tokens) != len(set(tokens)):
                raise ValueError(f"transaction DAG {label} is empty or repeated")
            unknown = set(tokens) - allowed
            if unknown:
                raise ValueError(
                    f"transaction DAG {label} has unknown tokens {unknown}"
                )
        dependencies: dict[str, set[str]] = {}
        for node in self.nodes:
            unknown = set(node.dependencies) - allowed
            if unknown:
                raise ValueError(
                    f"transaction DAG node {node.name} has unknown tokens {unknown}"
                )
            dependencies[node.name] = {
                token.rsplit(":", 1)[0]
                for token in node.dependencies
                if token not in initial
            }
        resolved: set[str] = set()
        while len(resolved) != len(names):
            ready = {
                name
                for name, required in dependencies.items()
                if name not in resolved and required <= resolved
            }
            if not ready:
                raise ValueError("transaction DAG contains a dependency cycle")
            resolved.update(ready)


class CodecAdapter:
    """A stateful codec boundary owned by the common simulator."""

    name = "unset"
    physical = False

    def begin(self, item: WorkItem, worker: int) -> Sequence[Phase]:
        raise NotImplementedError

    def transaction_plan(self, item: WorkItem, worker: int) -> TransactionPlan | None:
        """Return a fork/join plan, or ``None`` to use the linear phase adapter."""
        del item, worker
        return None

    def commit(self, item: WorkItem, worker: int) -> None:
        """Commit state only after the complete dialogue has finished."""

    def bind_route(
        self, item: WorkItem, worker: int, tu_seq: int, rel_seq: int
    ) -> None:
        """Validate the global prepared identity and independent ordered F route."""
        del item, worker, tu_seq, rel_seq

    def preview_c_to_f(self, item: WorkItem, worker: int) -> int:
        """Tie-break hint; never charged as a physical measurement."""
        del item, worker
        raise NotImplementedError(
            "this adapter does not provide a side-effect-free preview"
        )

    def timeline_c_state(self, environment: int) -> dict[str, object]:
        """Return adapter-owned state for one C authority.

        Physical adapters use this hook for resident definitions, prepared work and
        committed codec state.  The common event engine owns all scheduler/network state.
        """
        del environment
        return {}

    def timeline_f_state(self, environment: int, worker: int) -> dict[str, object]:
        """Return adapter-owned state for one (C authority, F) namespace."""
        del environment, worker
        return {}

    def timeline_coverage(self) -> dict[str, object]:
        return {
            "codec_state": "not-modelled-by-this-adapter",
            "codec_cpu": "not-modelled-by-this-adapter",
        }

    def dialogue_window_per_route(self) -> int | None:
        """Maximum prepared-but-uncommitted transactions on one (C,F) route.

        ``None`` keeps the diagnostic adapters unrestricted.  Physical replay adapters use
        one unless their ledger explicitly describes a wider, state-consistent window.
        """
        return None

    def result_metadata(self) -> dict[str, object]:
        return {}


class CompileOnlyAdapter(CodecAdapter):
    name = "compile-only"

    def begin(self, item: WorkItem, worker: int) -> Sequence[Phase]:
        del item, worker
        return ()

    def preview_c_to_f(self, item: WorkItem, worker: int) -> int:
        del item, worker
        return 0


class RawAdapter(CodecAdapter):
    name = "raw"

    def begin(self, item: WorkItem, worker: int) -> Sequence[Phase]:
        del worker
        return (Phase("raw-tu", "c_to_f", item.raw_bytes),)

    def preview_c_to_f(self, item: WorkItem, worker: int) -> int:
        del worker
        return item.raw_bytes

    def dialogue_window_per_route(self) -> int | None:
        return 1


@dataclass(frozen=True)
class PhysicalLedgerEntry:
    workload: str
    build: int
    logical: int
    worker: int
    tu_seq: int | None
    route_sequence: int
    raw_bytes: int
    raw_sha256: str
    phases: tuple[Phase, ...]
    plan: TransactionPlan
    state_after: dict[str, object]


class PhysicalLedgerAdapter(CodecAdapter):
    """Replay exact per-TU codec transactions inside the common event engine.

    The producer of this ledger owns encoding and byte-exact reconstruction.  This class
    rejects incomplete ledgers, estimated rows, byte-total drift and assignment drift before
    any result can carry the physical-result label.  An explicit compatibility mode permits a
    ledger to be reused for a timing-only scenario variant, but still checks every input byte
    digest and refuses any destination, TU_SEQ, or REL_SEQ drift during replay.
    """

    physical = True

    def __init__(
        self,
        path: Path,
        scenario: "LoadedScenario",
        expected_codec: str | None = None,
        allow_compatible_scenario: bool = False,
        payload_digest_cache: (
            dict[Path, tuple[tuple[int, int, int, int, int], str]] | None
        ) = None,
        assignment_closure: str = "exact_route",
    ):
        if assignment_closure not in {"exact_route", "aggregate"}:
            raise ValueError("assignment_closure must be exact_route or aggregate")
        self.assignment_closure = assignment_closure
        self.path = path.resolve()
        self.ledger_identity = self.path.name if scenario.is_v2 else str(self.path)
        rows = [json.loads(line) for line in self.path.read_text().splitlines() if line]
        if len(rows) < 3:
            raise ValueError(f"{self.path}: physical ledger is incomplete")
        descriptor, final = rows[0], rows[-1]
        if (
            descriptor.get("record") != "physical-ledger"
            or descriptor.get("schema") != "icecream-physical-codec-ledger-v1"
        ):
            raise ValueError(f"{self.path}: unknown physical ledger schema")
        codec = descriptor.get("codec")
        if not isinstance(codec, str) or not codec:
            raise ValueError(f"{self.path}: physical ledger codec is empty")
        if expected_codec is not None and codec != expected_codec:
            raise ValueError(
                f"{self.path}: ledger codec {codec!r} differs from {expected_codec!r}"
            )
        reconstruction = descriptor.get("reconstruction")
        if (
            not isinstance(reconstruction, dict)
            or reconstruction.get("status") != "pass"
        ):
            raise ValueError(f"{self.path}: reconstruction result is not pass")
        ledger_scenario_sha256 = descriptor.get("scenario_sha256")
        replay_scenario_sha256 = scenario.scenario_digest
        if (
            ledger_scenario_sha256 != replay_scenario_sha256
            and not allow_compatible_scenario
        ):
            raise ValueError(f"{self.path}: ledger belongs to a different scenario")
        self.scenario_binding = (
            "exact-scenario-sha256"
            if ledger_scenario_sha256 == replay_scenario_sha256
            else "compatible-inputs-and-runtime-route-order"
        )
        self.ledger_scenario_sha256 = ledger_scenario_sha256
        self.replay_scenario_sha256 = replay_scenario_sha256
        if final.get("record") != "physical-summary":
            raise ValueError(f"{self.path}: physical ledger has no final summary")
        self.name = codec
        self.descriptor = descriptor
        self.final = final
        self.entries: dict[tuple[str, int, int], PhysicalLedgerEntry] = {}
        route_sequences: dict[tuple[int, int], list[int]] = defaultdict(list)
        tu_sequences: dict[int, list[int]] = defaultdict(list)
        payload_digests: dict[Path, tuple[tuple[int, int, int, int, int], str]] = (
            payload_digest_cache if payload_digest_cache is not None else {}
        )
        c_to_f_total = 0
        f_to_c_total = 0
        for row_number, row in enumerate(rows[1:-1], start=2):
            if row.get("record") != "tu" or row.get("exact") is not True:
                raise ValueError(f"{self.path}:{row_number}: non-exact TU row")
            workload = row.get("workload")
            if not isinstance(workload, str) or not workload:
                raise ValueError(f"{self.path}:{row_number}: workload is empty")
            build = checked_nonnegative_int(row.get("build"), "ledger build")
            logical = checked_nonnegative_int(row.get("logical"), "ledger logical")
            scenario_key = (workload, build)
            if scenario_key not in scenario.work_items or logical >= len(
                scenario.work_items[scenario_key]
            ):
                raise ValueError(
                    f"{self.path}:{row_number}: TU identity is outside the scenario"
                )
            item = scenario.work_items[scenario_key][logical]
            worker = checked_nonnegative_int(row.get("worker"), "ledger worker")
            if worker >= int(scenario.document["workers"]["f_count"]):
                raise ValueError(
                    f"{self.path}:{row_number}: worker is outside scenario"
                )
            route_sequence = checked_nonnegative_int(
                row.get("route_sequence"), "ledger route_sequence"
            )
            rel_seq = checked_nonnegative_int(
                row.get("rel_seq", route_sequence), "ledger rel_seq"
            )
            if rel_seq != route_sequence:
                raise ValueError(
                    f"{self.path}:{row_number}: rel_seq differs from route_sequence"
                )
            tu_seq_value = row.get("tu_seq")
            tu_seq = (
                None
                if tu_seq_value is None
                else checked_nonnegative_int(tu_seq_value, "ledger tu_seq")
            )
            raw_bytes = checked_positive_int(row.get("raw_bytes"), "ledger raw_bytes")
            raw_sha256 = row.get("raw_sha256")
            if (
                not isinstance(raw_sha256, str)
                or len(raw_sha256) != 64
                or any(character not in "0123456789abcdef" for character in raw_sha256)
            ):
                raise ValueError(
                    f"{self.path}:{row_number}: raw_sha256 is not canonical"
                )
            phase_rows = row.get("phases")
            if not isinstance(phase_rows, list) or not phase_rows:
                raise ValueError(f"{self.path}:{row_number}: phases are empty")
            phases = []
            graph_declared = any(
                key in row
                for key in (
                    "initial_tokens",
                    "input_ready_after",
                    "commit_after",
                )
            ) or any(
                isinstance(phase_row, dict)
                and ("depends_on" in phase_row or "priority" in phase_row)
                for phase_row in phase_rows
            )
            dag_nodes = []
            for phase_index, phase_row in enumerate(phase_rows):
                if not isinstance(phase_row, dict):
                    raise ValueError(
                        f"{self.path}:{row_number}: phase {phase_index} is not an object"
                    )
                phase_name = phase_row.get("name")
                if not isinstance(phase_name, str) or not phase_name:
                    raise ValueError(
                        f"{self.path}:{row_number}: phase {phase_index} has no name"
                    )
                direction = phase_row.get("direction")
                byte_count = checked_nonnegative_int(
                    phase_row.get("bytes"), "ledger phase bytes"
                )
                priority = checked_nonnegative_int(
                    phase_row.get("priority", 3), "ledger phase priority"
                )
                phase = Phase(phase_name, direction, byte_count, priority)
                phases.append(phase)
                if graph_declared:
                    dependencies = phase_row.get("depends_on", [])
                    if not isinstance(dependencies, list) or not all(
                        isinstance(token, str) for token in dependencies
                    ):
                        raise ValueError(
                            f"{self.path}:{row_number}: phase {phase_index} dependencies are invalid"
                        )
                    dag_nodes.append(
                        DagNode(
                            phase_name,
                            direction,
                            byte_count,
                            tuple(dependencies),
                            priority,
                        )
                    )
            if graph_declared:
                initial_tokens = row.get("initial_tokens", [])
                input_ready_after = row.get("input_ready_after")
                commit_after = row.get("commit_after")
                if not all(
                    isinstance(tokens, list)
                    and all(isinstance(token, str) for token in tokens)
                    for tokens in (initial_tokens, input_ready_after, commit_after)
                ):
                    raise ValueError(
                        f"{self.path}:{row_number}: transaction DAG token lists are invalid"
                    )
                plan = TransactionPlan(
                    tuple(dag_nodes),
                    tuple(initial_tokens),
                    tuple(input_ready_after),
                    tuple(commit_after),
                )
            else:
                dag_nodes = []
                previous: str | None = None
                for phase in phases:
                    dependencies = (
                        () if previous is None else (f"{previous}:delivered",)
                    )
                    dag_nodes.append(
                        DagNode(
                            phase.name,
                            phase.direction,
                            phase.byte_count,
                            dependencies,
                            phase.priority,
                        )
                    )
                    previous = phase.name
                final_token = f"{dag_nodes[-1].name}:delivered"
                plan = TransactionPlan(
                    tuple(dag_nodes),
                    ("attachment:accepted",),
                    ("attachment:accepted", final_token),
                    (final_token,),
                )
            state_after = row.get("state_after", {})
            if not isinstance(state_after, dict):
                raise ValueError(
                    f"{self.path}:{row_number}: state_after is not an object"
                )
            entry = PhysicalLedgerEntry(
                workload,
                build,
                logical,
                worker,
                tu_seq,
                route_sequence,
                raw_bytes,
                raw_sha256,
                tuple(phases),
                plan,
                state_after,
            )
            key = (workload, build, logical)
            if key in self.entries:
                raise ValueError(
                    f"{self.path}:{row_number}: repeated TU identity {key}"
                )
            self.entries[key] = entry
            environment = item.environment
            route_sequences[(environment, worker)].append(route_sequence)
            if tu_seq is not None:
                tu_sequences[environment].append(tu_seq)
            c_to_f_total += sum(
                phase.byte_count for phase in phases if phase.direction == "c_to_f"
            )
            f_to_c_total += sum(
                phase.byte_count for phase in phases if phase.direction == "f_to_c"
            )

        expected_items = {
            item.key: item for items in scenario.work_items.values() for item in items
        }
        if set(self.entries) != set(expected_items):
            missing = sorted(set(expected_items) - set(self.entries))
            extra = sorted(set(self.entries) - set(expected_items))
            raise ValueError(
                f"{self.path}: ledger TU set differs: missing={missing[:1]} extra={extra[:1]}"
            )
        for key, item in expected_items.items():
            if self.entries[key].raw_bytes != item.raw_bytes:
                raise ValueError(f"{self.path}: raw byte count differs for {key}")
            if not item.payload.is_file():
                raise ValueError(
                    f"{self.path}: payload is absent for {key}: {item.payload}"
                )
            payload = item.payload.resolve()
            stat = payload.stat()
            identity = (
                stat.st_dev,
                stat.st_ino,
                stat.st_size,
                stat.st_mtime_ns,
                stat.st_ctime_ns,
            )
            cached_digest = payload_digests.get(payload)
            if cached_digest is None or cached_digest[0] != identity:
                digest = sha256(payload)
                payload_digests[payload] = (identity, digest)
            else:
                digest = cached_digest[1]
            if self.entries[key].raw_sha256 != digest:
                raise ValueError(f"{self.path}: raw content digest differs for {key}")
        for route, sequences in route_sequences.items():
            if sequences != list(range(len(sequences))):
                raise ValueError(
                    f"{self.path}: route {route} sequence is not contiguous in ledger order"
                )
        for environment, sequences in tu_sequences.items():
            expected_count = sum(
                item.environment == environment for item in expected_items.values()
            )
            if sorted(sequences) != list(range(expected_count)):
                raise ValueError(
                    f"{self.path}: C{environment} TU_SEQ does not cover its contiguous admission domain"
                )
        totals = final.get("totals")
        expected_totals = {
            "tus": len(self.entries),
            "c_to_f_bytes": c_to_f_total,
            "f_to_c_bytes": f_to_c_total,
        }
        if totals != expected_totals:
            raise ValueError(
                f"{self.path}: final totals {totals!r} differ from {expected_totals!r}"
            )
        self.committed_by_route: dict[tuple[int, int], int] = defaultdict(int)
        self.committed_by_environment = [
            0 for _ in range(int(scenario.document["environments"]["env_count"]))
        ]
        self.committed_bytes_by_environment = [
            0 for _ in range(int(scenario.document["environments"]["env_count"]))
        ]
        self.committed_bytes_by_route: dict[tuple[int, int], int] = defaultdict(int)
        self.active: set[tuple[str, int, int]] = set()
        self.last_route_state: dict[tuple[int, int], dict[str, object]] = {}
        self.item_environment = {
            key: item.environment for key, item in expected_items.items()
        }

    def dialogue_window_per_route(self) -> int | None:
        return 1

    def _activate(self, item: WorkItem, worker: int) -> PhysicalLedgerEntry:
        entry = self.entries[item.key]
        if self.assignment_closure == "exact_route" and entry.worker != worker:
            raise RuntimeError(
                f"physical ledger assigned {item.key} to F{entry.worker}, simulator chose F{worker}"
            )
        route = (item.environment, worker)
        if (
            self.assignment_closure == "exact_route"
            and entry.route_sequence != self.committed_by_route[route]
        ):
            raise RuntimeError(
                f"physical ledger route {route} expected sequence {entry.route_sequence}, "
                f"committed {self.committed_by_route[route]}"
            )
        if item.key in self.active:
            raise RuntimeError(f"physical ledger TU {item.key} began twice")
        self.active.add(item.key)
        return entry

    def bind_route(
        self, item: WorkItem, worker: int, tu_seq: int, rel_seq: int
    ) -> None:
        entry = self.entries[item.key]
        if self.assignment_closure == "aggregate":
            return
        if entry.worker != worker:
            raise RuntimeError(
                f"physical ledger assigned {item.key} to F{entry.worker}, simulator chose F{worker}"
            )
        if entry.tu_seq is not None and entry.tu_seq != tu_seq:
            raise RuntimeError(
                f"physical ledger TU {item.key} expected TU_SEQ {entry.tu_seq}, got {tu_seq}"
            )
        if entry.route_sequence != rel_seq:
            raise RuntimeError(
                f"physical ledger TU {item.key} expected REL_SEQ {entry.route_sequence}, got {rel_seq}"
            )

    def begin(self, item: WorkItem, worker: int) -> Sequence[Phase]:
        entry = self._activate(item, worker)
        return entry.phases

    def transaction_plan(self, item: WorkItem, worker: int) -> TransactionPlan | None:
        return self._activate(item, worker).plan

    def commit(self, item: WorkItem, worker: int) -> None:
        if item.key not in self.active:
            raise RuntimeError(f"physical ledger TU {item.key} committed without begin")
        entry = self.entries[item.key]
        route = (item.environment, worker)
        if self.assignment_closure == "exact_route" and (
            entry.worker != worker
            or entry.route_sequence != self.committed_by_route[route]
        ):
            raise RuntimeError(f"physical ledger commit order changed for {item.key}")
        self.active.remove(item.key)
        self.committed_by_route[route] += 1
        self.committed_by_environment[item.environment] += 1
        c_to_f = sum(
            phase.byte_count for phase in entry.phases if phase.direction == "c_to_f"
        )
        self.committed_bytes_by_environment[item.environment] += c_to_f
        self.committed_bytes_by_route[route] += c_to_f
        self.last_route_state[route] = entry.state_after

    def preview_c_to_f(self, item: WorkItem, worker: int) -> int:
        entry = self.entries[item.key]
        if self.assignment_closure == "exact_route" and entry.worker != worker:
            return 1 << 62
        return sum(
            phase.byte_count for phase in entry.phases if phase.direction == "c_to_f"
        )

    def timeline_c_state(self, environment: int) -> dict[str, object]:
        return {
            "committed_tus": self.committed_by_environment[environment],
            "committed_c_to_f_bytes": self.committed_bytes_by_environment[environment],
        }

    def timeline_f_state(self, environment: int, worker: int) -> dict[str, object]:
        route = (environment, worker)
        state = {
            "committed_transactions": self.committed_by_route[route],
            "committed_c_to_f_bytes": self.committed_bytes_by_route[route],
        }
        if route in self.last_route_state:
            state["codec_state"] = self.last_route_state[route]
        return state

    def timeline_coverage(self) -> dict[str, object]:
        return {
            "codec_state": "committed physical-ledger state_after rows",
            "codec_cpu": self.descriptor.get("codec_cpu", "not-modelled"),
            "reconstruction": self.descriptor["reconstruction"],
        }

    def result_metadata(self) -> dict[str, object]:
        return {
            "ledger": self.ledger_identity,
            "ledger_sha256": sha256(self.path),
            "scenario_binding": self.scenario_binding,
            "assignment_closure": self.assignment_closure,
            "ledger_scenario_sha256": self.ledger_scenario_sha256,
            "replay_scenario_sha256": self.replay_scenario_sha256,
            "descriptor": self.descriptor,
            "physical_summary": self.final,
        }


@dataclass
class Transaction:
    sequence: int
    item: WorkItem
    worker: int
    slot: int
    phases: tuple[Phase, ...]
    tu_seq: int = 0
    rel_seq: int = 0
    staging_slot: int | None = None
    compiler_slot: int | None = None
    staging_released: bool = False
    compiler_slot_released: bool = False
    phase_index: int = 0
    dispatch_ns: Fraction = Fraction(0)
    transfer_done_ns: Fraction | None = None
    compile_start_ns: Fraction | None = None
    compile_finish_ns: Fraction | None = None
    transaction_commit_ns: Fraction | None = None
    complete_ns: Fraction | None = None
    c_to_f_bytes: int = 0
    f_to_c_bytes: int = 0
    logical_job_id: str = ""
    attempt_id: str = ""
    c_store_guid: str = ""
    physical_endpoint: str = ""
    route_lane_id: str = ""
    f_store_guid: str = ""
    session_serial: int = 1
    history_nonce: int = 0
    transaction_digest: str = ""
    input_record_identity: str = ""
    dialogue_started: bool = False
    plan: TransactionPlan | None = None
    dag_tokens: set[str] = field(default_factory=set)
    dag_started: set[str] = field(default_factory=set)
    input_ready: bool = False
    transaction_committed: bool = False
    compile_completed: bool = False


@dataclass
class EnvironmentEnsure:
    environment: int
    worker: int
    owner: Transaction
    start_ns: Fraction
    transfer_done_ns: Fraction | None = None
    install_start_ns: Fraction | None = None
    ready_ns: Fraction | None = None


@dataclass
class Flow:
    sequence: int
    transaction: Transaction
    phase: Phase
    remaining_bits: Fraction
    start_ns: Fraction | None = None
    quantum_remaining_bits: Fraction | None = None

    @property
    def endpoint(self) -> tuple[str, int, int]:
        return (
            self.phase.direction,
            self.transaction.item.environment,
            self.transaction.worker,
        )


class EventRecorder:
    """Replayable exact-event stream with optional bounded-memory spooling."""

    def __init__(self, spool_path: Path | None = None):
        self.records: list[dict[str, object]] = []
        self.spool_path = None if spool_path is None else spool_path.resolve()
        self._spool = None
        if self.spool_path is not None:
            self.spool_path.parent.mkdir(parents=True, exist_ok=True)
            self._spool = self.spool_path.open("w")
        self.count = 0

    def __len__(self) -> int:
        return self.count

    def __iter__(self) -> Iterator[dict[str, object]]:
        if self.spool_path is None:
            yield from self.records
            return
        if self._spool is not None:
            self._spool.flush()
        with self.spool_path.open() as source:
            for line_number, line in enumerate(source, start=1):
                if not line.strip():
                    continue
                row = json.loads(line)
                if not isinstance(row, dict):
                    raise RuntimeError(
                        f"event spool {self.spool_path}:{line_number} is not an object"
                    )
                yield row

    def append(self, row: dict[str, object]) -> None:
        if self._spool is None:
            self.records.append(row)
        else:
            self._spool.write(json.dumps(row, separators=(",", ":"), sort_keys=True))
            self._spool.write("\n")
        self.count += 1

    def finish(self) -> None:
        if self._spool is not None:
            self._spool.flush()
            self._spool.close()
            self._spool = None

    def discard_spool(self) -> None:
        self.finish()
        if self.spool_path is not None and self.spool_path.exists():
            self.spool_path.unlink()
        self.spool_path = None


@dataclass
class SimulationResult:
    summary: dict[str, object]
    assignments: list[dict[str, object]]
    events: EventRecorder
    workers: list[dict[str, object]]
    builds: list[dict[str, object]]
    generations: list[dict[str, object]]
    timeline: "TimelineRecorder"


class SparseSlotPool:
    """Lowest-numbered free slot without materializing the worker's capacity.

    A scenario may use one giant F with a million compile slots while exposing only a
    few thousand jobs.  The old ``set(range(capacity))`` representation made memory use
    proportional to declared capacity.  This pool grows only with slots actually used.
    """

    def __init__(self, capacity: int):
        self.capacity = checked_positive_int(capacity, "slot capacity")
        self.next_unused = 0
        self.released: list[int] = []
        self.in_use: set[int] = set()

    def __bool__(self) -> bool:
        return len(self.in_use) < self.capacity

    def acquire(self) -> int:
        if not self:
            raise RuntimeError("slot acquisition from a full worker")
        if self.released:
            slot = heapq.heappop(self.released)
        else:
            slot = self.next_unused
            self.next_unused += 1
        if slot >= self.capacity or slot in self.in_use:
            raise AssertionError("sparse slot pool is internally inconsistent")
        self.in_use.add(slot)
        return slot

    def release(self, slot: int) -> None:
        if slot not in self.in_use:
            raise RuntimeError(f"slot {slot} was not reserved")
        self.in_use.remove(slot)
        heapq.heappush(self.released, slot)


@dataclass
class LoadedScenario:
    document: dict[str, object]
    manifest: dict[str, object]
    path: Path
    scenario_digest: str
    work_items: dict[tuple[str, int], list[WorkItem]]
    workload_config: dict[str, dict[str, object]]
    workload_inputs: dict[str, dict[str, object]]
    route_trace: dict[int, "RouteTraceEntry"] = field(default_factory=dict)
    route_trace_path: Path | None = None
    route_trace_sha256: str | None = None
    route_trace_identity: str | None = None
    route_trace_codec: str | None = None
    route_trace_provenance: str | None = None

    @property
    def is_v2(self) -> bool:
        return self.manifest.get("schema") == "icecream-experiment-v2"


@dataclass(frozen=True)
class RouteTraceEntry:
    logical_job_id: str
    attempt_id: str
    worker: int
    tu_seq: int
    rel_seq: int
    c_store_guid: str
    physical_endpoint: str
    route_lane_id: str
    f_store_guid: str
    session_serial: int
    history_nonce: int
    c_to_f_bytes: int
    f_to_c_bytes: int


class ExactLedger:
    """Exact directional and transient-byte accounting for one simulator run."""

    def __init__(self) -> None:
        self.directional: dict[str, dict[str, int]] = {
            direction: defaultdict(int) for direction in DIRECTIONS
        }
        self.directional_by_route: dict[tuple[str, int, int, str], int] = defaultdict(
            int
        )
        self.resource_balance: dict[str, int] = defaultdict(int)
        self.resource_credited: dict[str, int] = defaultdict(int)
        self.resource_debited: dict[str, int] = defaultdict(int)
        self.resource_peak: dict[str, int] = defaultdict(int)
        self.queue_balance: dict[str, int] = defaultdict(int)
        self.queue_credited: dict[str, int] = defaultdict(int)
        self.queue_debited: dict[str, int] = defaultdict(int)
        self.queue_peak: dict[str, int] = defaultdict(int)

    @staticmethod
    def _apply_balance(
        balances: dict[str, int],
        credited: dict[str, int],
        debited: dict[str, int],
        peaks: dict[str, int],
        deltas: Mapping[str, int],
        kind: str,
    ) -> None:
        for name, delta in deltas.items():
            if not isinstance(name, str) or not name:
                raise ValueError(f"{kind} byte-delta name is empty")
            if not isinstance(delta, int) or isinstance(delta, bool):
                raise ValueError(f"{kind} byte delta {name!r} is not an integer")
            balances[name] += delta
            if balances[name] < 0:
                raise RuntimeError(
                    f"{kind} byte balance {name!r} became negative: {balances[name]}"
                )
            if delta >= 0:
                credited[name] += delta
            else:
                debited[name] -= delta
            peaks[name] = max(peaks[name], balances[name])

    def apply(
        self,
        *,
        c_to_f: int = 0,
        f_to_c: int = 0,
        account: str = "source",
        route: tuple[int, int] | None = None,
        resource: Mapping[str, int] | None = None,
        queue: Mapping[str, int] | None = None,
    ) -> None:
        for direction, value in (("c_to_f", c_to_f), ("f_to_c", f_to_c)):
            checked_nonnegative_int(value, f"{direction} byte delta")
            if not value:
                continue
            self.directional[direction][account] += value
            if route is not None:
                self.directional_by_route[
                    (direction, route[0], route[1], account)
                ] += value
        self._apply_balance(
            self.resource_balance,
            self.resource_credited,
            self.resource_debited,
            self.resource_peak,
            resource or {},
            "resource",
        )
        self._apply_balance(
            self.queue_balance,
            self.queue_credited,
            self.queue_debited,
            self.queue_peak,
            queue or {},
            "queue",
        )

    def assert_closed(self) -> None:
        open_resources = {
            name: value for name, value in self.resource_balance.items() if value
        }
        open_queues = {
            name: value for name, value in self.queue_balance.items() if value
        }
        if open_resources or open_queues:
            raise RuntimeError(
                "exact byte ledger did not close: "
                f"resources={open_resources}, queues={open_queues}"
            )

    @staticmethod
    def _balance_rows(
        balances: Mapping[str, int],
        credited: Mapping[str, int],
        debited: Mapping[str, int],
        peaks: Mapping[str, int],
    ) -> list[dict[str, object]]:
        names = sorted(set(balances) | set(credited) | set(debited) | set(peaks))
        return [
            {
                "name": name,
                "credited_bytes": credited.get(name, 0),
                "debited_bytes": debited.get(name, 0),
                "peak_bytes": peaks.get(name, 0),
                "final_bytes": balances.get(name, 0),
            }
            for name in names
        ]

    def summary(self) -> dict[str, object]:
        directions = {}
        for direction in DIRECTIONS:
            accounts = dict(sorted(self.directional[direction].items()))
            directions[direction] = {
                "accounts": accounts,
                "total_bytes": sum(accounts.values()),
            }
        routes = [
            {
                "direction": direction,
                "environment": environment,
                "worker": worker,
                "account": account,
                "bytes": byte_count,
            }
            for (direction, environment, worker, account), byte_count in sorted(
                self.directional_by_route.items()
            )
        ]
        return {
            "closure": "pass",
            "directions": directions,
            "routes": routes,
            "resources": self._balance_rows(
                self.resource_balance,
                self.resource_credited,
                self.resource_debited,
                self.resource_peak,
            ),
            "queues": self._balance_rows(
                self.queue_balance,
                self.queue_credited,
                self.queue_debited,
                self.queue_peak,
            ),
        }


class TimelineRecorder:
    """Integrate exact engine state into bounded active-time observations.

    Ten milliseconds is measured only while a compiler or a network serializer is doing
    work.  A wall-clock interval with neither is represented by one gap record, preserving
    release and propagation timers without manufacturing thousands of empty samples.
    """

    def __init__(
        self,
        interval_ns: int,
        workers: int,
        spool_path: Path | None = None,
    ):
        self.interval_ns = checked_positive_int(interval_ns, "snapshot interval_ns")
        self.workers = workers
        self.records: list[dict[str, object]] = []
        self.spool_path = None if spool_path is None else spool_path.resolve()
        self._spool = None
        if self.spool_path is not None:
            self.spool_path.parent.mkdir(parents=True, exist_ok=True)
            self._spool = self.spool_path.open("w")
        self.record_count = 0
        self.snapshot_count = 0
        self.gap_count = 0
        self.sequence = 0
        self.active_ns = Fraction(0)
        self.sample_active_start_ns = Fraction(0)
        self.sample_wall_start_ns: Fraction | None = None
        self.sample_elapsed_ns = Fraction(0)
        self.route_bits: dict[tuple[str, int, int], Fraction] = defaultdict(Fraction)
        self.fabric_bits = Fraction(0)
        self.direction_fabric_bits: dict[str, Fraction] = defaultdict(Fraction)
        self.compile_slot_ns = [Fraction(0) for _ in range(workers)]
        self.input_wait_slot_ns = [Fraction(0) for _ in range(workers)]
        self.ready_input_slot_ns = [Fraction(0) for _ in range(workers)]
        self.commit_wait_slot_ns = [Fraction(0) for _ in range(workers)]
        self.environment_wait_slot_ns = [Fraction(0) for _ in range(workers)]
        self.reserved_slot_ns = [Fraction(0) for _ in range(workers)]
        self.staging_slot_ns = [Fraction(0) for _ in range(workers)]
        self.snapshot_due = False

    def __len__(self) -> int:
        return self.record_count

    def __iter__(self) -> Iterator[dict[str, object]]:
        if self.spool_path is None:
            yield from self.records
            return
        if self._spool is not None:
            self._spool.flush()
        with self.spool_path.open() as source:
            for line_number, line in enumerate(source, start=1):
                if not line.strip():
                    continue
                row = json.loads(line)
                if not isinstance(row, dict):
                    raise RuntimeError(
                        f"timeline spool {self.spool_path}:{line_number} is not an object"
                    )
                yield row

    def _record(self, row: dict[str, object]) -> None:
        if self._spool is None:
            self.records.append(row)
        else:
            self._spool.write(json.dumps(row, separators=(",", ":"), sort_keys=True))
            self._spool.write("\n")
        self.record_count += 1
        if row["record"] == "snapshot":
            self.snapshot_count += 1
        elif row["record"] == "gap":
            self.gap_count += 1
        else:
            raise RuntimeError(f"unknown timeline record {row['record']!r}")

    def finish(self) -> None:
        if self._spool is not None:
            self._spool.flush()
            self._spool.close()
            self._spool = None

    def discard_spool(self) -> None:
        self.finish()
        if self.spool_path is not None and self.spool_path.exists():
            self.spool_path.unlink()
        self.spool_path = None

    @property
    def remaining_sample_ns(self) -> Fraction:
        return Fraction(self.interval_ns) - self.sample_elapsed_ns

    def accumulate(
        self,
        simulator: "Simulator",
        elapsed_ns: Fraction,
        rates: dict[int, Fraction],
    ) -> None:
        if elapsed_ns <= 0:
            raise ValueError("timeline accumulation requires positive elapsed time")
        if self.sample_wall_start_ns is None:
            self.sample_wall_start_ns = simulator.now
            self.sample_active_start_ns = self.active_ns
        self.sample_elapsed_ns += elapsed_ns
        self.active_ns += elapsed_ns
        for flow_sequence, rate in rates.items():
            flow = simulator.active_flows[flow_sequence]
            bits = rate * elapsed_ns
            self.route_bits[flow.endpoint] += bits
            self.fabric_bits += bits
            self.direction_fabric_bits[flow.phase.direction] += bits
        for worker in range(self.workers):
            self.compile_slot_ns[worker] += (
                simulator.worker_compiling[worker] * elapsed_ns
            )
            self.input_wait_slot_ns[worker] += (
                simulator.worker_input_wait[worker] * elapsed_ns
            )
            self.ready_input_slot_ns[worker] += (
                simulator.worker_ready_input[worker] * elapsed_ns
            )
            self.commit_wait_slot_ns[worker] += (
                simulator.worker_commit_wait[worker] * elapsed_ns
            )
            self.environment_wait_slot_ns[worker] += (
                simulator.worker_environment_wait[worker] * elapsed_ns
            )
            self.reserved_slot_ns[worker] += (
                len(simulator.compiler_slots[worker].in_use) * elapsed_ns
            )
            self.staging_slot_ns[worker] += (
                len(simulator.staging_slots[worker].in_use) * elapsed_ns
            )
        if self.sample_elapsed_ns > self.interval_ns:
            raise AssertionError("timeline sample exceeded its configured interval")

    def mark_or_emit_boundary(self, simulator: "Simulator", external: bool) -> None:
        if self.sample_elapsed_ns != self.interval_ns:
            return
        if external:
            self.snapshot_due = True
        else:
            self.emit_snapshot(simulator)

    def flush_due(self, simulator: "Simulator") -> None:
        if self.snapshot_due:
            self.emit_snapshot(simulator)

    def flush_partial(self, simulator: "Simulator") -> None:
        self.flush_due(simulator)
        if self.sample_elapsed_ns:
            self.emit_snapshot(simulator)

    def emit_snapshot(self, simulator: "Simulator") -> None:
        if not self.sample_elapsed_ns or self.sample_wall_start_ns is None:
            raise RuntimeError("cannot emit an empty active timeline sample")
        duration = self.sample_elapsed_ns
        wall_start_ns = ceil_fraction(self.sample_wall_start_ns)
        wall_end_ns = ceil_fraction(simulator.now)
        active_start_ns = ceil_fraction(self.sample_active_start_ns)
        active_end_ns = ceil_fraction(self.active_ns)
        route_metrics: list[dict[str, object]] = []
        environment_rates: dict[tuple[str, int], float] = defaultdict(float)
        worker_rates: dict[tuple[str, int], float] = defaultdict(float)
        for (direction, environment, worker), bits in sorted(self.route_bits.items()):
            average_bps = float(bits * NANOSECONDS / duration)
            environment_rates[(direction, environment)] += average_bps
            worker_rates[(direction, worker)] += average_bps
            route_capacity = int(simulator.network[direction]["bits_per_second"])
            route_metrics.append(
                {
                    "direction": direction,
                    "environment": environment,
                    "worker": worker,
                    "bits": float(bits),
                    "average_bps": average_bps,
                    "route_capacity_bps": route_capacity,
                    "route_utilization": average_bps / route_capacity,
                }
            )
        environment_metrics = []
        for environment in range(simulator.env_count):
            row: dict[str, object] = {"environment": environment}
            for direction in DIRECTIONS:
                average_bps = environment_rates[(direction, environment)]
                capacity = simulator.network[direction].get(
                    "per_environment_bits_per_second"
                )
                row[f"{direction}_average_bps"] = average_bps
                row[f"{direction}_capacity_bps"] = capacity
                row[f"{direction}_utilization"] = (
                    None if capacity is None else average_bps / int(capacity)
                )
            environment_metrics.append(row)
        worker_metrics = []
        for worker in range(self.workers):
            row = {
                "worker": worker,
                "average_compiling_slots": float(
                    self.compile_slot_ns[worker] / duration
                ),
                "average_input_wait_slots": float(
                    self.input_wait_slot_ns[worker] / duration
                ),
                "average_ready_input_slots": float(
                    self.ready_input_slot_ns[worker] / duration
                ),
                "average_commit_wait_slots": float(
                    self.commit_wait_slot_ns[worker] / duration
                ),
                "average_environment_wait_slots": float(
                    self.environment_wait_slot_ns[worker] / duration
                ),
                "average_reserved_slots": float(
                    self.reserved_slot_ns[worker] / duration
                ),
                "average_staging_slots": float(self.staging_slot_ns[worker] / duration),
            }
            for direction in DIRECTIONS:
                average_bps = worker_rates[(direction, worker)]
                capacity = simulator.network[direction].get(
                    "per_worker_bits_per_second"
                )
                row[f"{direction}_average_bps"] = average_bps
                row[f"{direction}_capacity_bps"] = capacity
                row[f"{direction}_utilization"] = (
                    None if capacity is None else average_bps / int(capacity)
                )
            worker_metrics.append(row)
        common_fabric_capacity = simulator.network.get("shared_fabric_bps")
        direction_fabrics = []
        for direction in DIRECTIONS:
            bits = self.direction_fabric_bits[direction]
            average_bps = float(bits * NANOSECONDS / duration)
            capacity = simulator.network[direction].get("fabric_bits_per_second")
            direction_fabrics.append(
                {
                    "direction": direction,
                    "bits": float(bits),
                    "average_bps": average_bps,
                    "capacity_bps": capacity,
                    "utilization": (
                        None if capacity is None else average_bps / int(capacity)
                    ),
                }
            )
        metrics = {
            "fabric_bits": float(self.fabric_bits),
            "fabric_average_bps": float(self.fabric_bits * NANOSECONDS / duration),
            "fabric_capacity_bps": common_fabric_capacity,
            "fabric_utilization": (
                None
                if common_fabric_capacity is None
                else float(
                    self.fabric_bits
                    * NANOSECONDS
                    / duration
                    / int(common_fabric_capacity)
                )
            ),
            "direction_fabrics": direction_fabrics,
            "routes": route_metrics,
            "environments": environment_metrics,
            "workers": worker_metrics,
        }
        state = simulator.timeline_state()
        self._record(
            {
                "record": "snapshot",
                "sequence": self.sequence,
                "wall_start_ns": wall_start_ns,
                "wall_end_ns": wall_end_ns,
                "wall_duration_ns": wall_end_ns - wall_start_ns,
                "active_start_ns": active_start_ns,
                "active_end_ns": active_end_ns,
                "active_duration_ns": active_end_ns - active_start_ns,
                "state": state,
                "metrics": metrics,
            }
        )
        self.sequence += 1
        self.sample_active_start_ns = self.active_ns
        self.sample_wall_start_ns = None
        self.sample_elapsed_ns = Fraction(0)
        self.route_bits.clear()
        self.fabric_bits = Fraction(0)
        self.direction_fabric_bits.clear()
        self.compile_slot_ns = [Fraction(0) for _ in range(self.workers)]
        self.input_wait_slot_ns = [Fraction(0) for _ in range(self.workers)]
        self.ready_input_slot_ns = [Fraction(0) for _ in range(self.workers)]
        self.commit_wait_slot_ns = [Fraction(0) for _ in range(self.workers)]
        self.environment_wait_slot_ns = [Fraction(0) for _ in range(self.workers)]
        self.reserved_slot_ns = [Fraction(0) for _ in range(self.workers)]
        self.staging_slot_ns = [Fraction(0) for _ in range(self.workers)]
        self.snapshot_due = False

    def emit_gap(
        self, simulator: "Simulator", start_ns: Fraction, end_ns: Fraction
    ) -> None:
        if end_ns <= start_ns:
            raise ValueError("timeline gap must have positive duration")
        if simulator.delivery_heap:
            reason = "network-propagation"
        elif simulator.environment_install_heap:
            reason = "environment-install-verify"
        elif simulator.release_heap:
            reason = "workload-release"
        else:
            reason = "timer"
        wall_start_ns = ceil_fraction(start_ns)
        wall_end_ns = ceil_fraction(end_ns)
        self._record(
            {
                "record": "gap",
                "sequence": self.sequence,
                "wall_start_ns": wall_start_ns,
                "wall_end_ns": wall_end_ns,
                "wall_duration_ns": wall_end_ns - wall_start_ns,
                "active_position_ns": ceil_fraction(self.active_ns),
                "reason": reason,
                "state": simulator.timeline_state(),
            }
        )
        self.sequence += 1


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1 << 22), b""):
            digest.update(block)
    return digest.hexdigest()


def canonical_json_sha256(document: object) -> str:
    encoded = json.dumps(
        document, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def canonical_relative_identity(
    value: str, name: str, *, allow_dot: bool = False
) -> str:
    """Return one checkout-independent POSIX identity for a v2 input path."""
    if not value or "\\" in value:
        raise ValueError(f"{name} must be a nonempty POSIX relative path")
    if value == "." and allow_dot:
        return value
    candidate = PurePosixPath(value)
    if candidate.is_absolute() or ".." in candidate.parts:
        raise ValueError(f"{name} must stay inside its declared checkout root")
    normalized = candidate.as_posix()
    if normalized in {"", "."}:
        raise ValueError(f"{name} must name a relative path")
    return normalized


def trace_row_manifest(row: TraceRow) -> dict[str, object]:
    if row.raw_sha256 is None:
        raise ValueError(f"trace row {row.logical} has no raw_sha256")
    return {
        "logical": row.logical,
        "job_id": row.job_id,
        "ii_relative": canonical_relative_identity(
            row.ii_relative, f"trace row {row.logical} ii_relative"
        ),
        "raw_bytes": row.raw_bytes,
        "raw_sha256": row.raw_sha256,
        "compile_ns": row.compile_ns,
        "compile_model": row.compile_model,
        "compile_provenance": row.compile_provenance,
    }


def workload_content_manifest(rows: Sequence[TraceRow]) -> dict[str, object]:
    """Canonical content-and-timing manifest used by scenarios and executions."""
    return {
        "schema": "icecream-workload-input-v1",
        "rows": [trace_row_manifest(row) for row in rows],
    }


def selected_v2_adapter(manifest: Mapping[str, object]) -> str | None:
    capabilities = manifest["capabilities"]
    assert isinstance(capabilities, Mapping)
    control = capabilities.get("experiment_control")
    return {
        "compile_only": "compile-only",
        "raw": "raw",
        "z3_shared_long_b1": None,
    }.get(control, str(capabilities["codec_profile"]))


def validate_json_schema(
    document: object, schema_name: str, source: Path | str
) -> None:
    """Validate a v2 contract with the checked-in Draft 2020-12 schema."""
    try:
        import jsonschema
    except ImportError as error:  # pragma: no cover - packaging/environment diagnostic
        raise RuntimeError(
            "v2 experiment validation requires the Python jsonschema package"
        ) from error
    schema_path = Path(__file__).with_name(schema_name)
    schema = json.loads(schema_path.read_text())
    validator = jsonschema.Draft202012Validator(schema)
    failures = sorted(validator.iter_errors(document), key=lambda item: list(item.path))
    if not failures:
        return
    failure = failures[0]
    location = ".".join(str(part) for part in failure.absolute_path) or "<root>"
    raise ValueError(f"{source}: schema error at {location}: {failure.message}")


def stable_hex(label: str, *parts: object, digits: int = 64) -> str:
    digest = hashlib.sha256(f"icecream-{label}-v1\0".encode("ascii"))
    for part in parts:
        encoded = str(part).encode("utf-8")
        digest.update(len(encoded).to_bytes(8, "big"))
        digest.update(encoded)
    return digest.hexdigest()[:digits]


def transaction_identity_digest(
    scenario_digest: str,
    logical_job_id: str,
    attempt_id: str,
    c_store_guid: str,
    f_store_guid: str,
    history_nonce: int,
    tu_seq: int,
    rel_seq: int,
    raw_digest: str,
    negotiated_profiles: Sequence[str],
    route_state_profiles: Sequence[str],
) -> str:
    """Return the domain-separated digest for one prepared-input transaction."""
    return stable_hex(
        "transaction",
        scenario_digest,
        logical_job_id,
        attempt_id,
        c_store_guid,
        f_store_guid,
        history_nonce,
        tu_seq,
        rel_seq,
        raw_digest,
        ",".join(negotiated_profiles),
        ",".join(route_state_profiles),
    )


def input_record_identity(transaction_digest: str, raw_digest: str) -> str:
    """Return the stable immutable-input identity for a transaction."""
    return "input-" + stable_hex(
        "input-record", transaction_digest, raw_digest, digits=32
    )


def logical_job_identity(scenario_digest: str, item: WorkItem) -> str:
    return "job-" + stable_hex(
        "logical-job",
        scenario_digest,
        item.environment,
        item.workload,
        item.build,
        item.source_job_id,
        digits=32,
    )


def normalize_v2_manifest(manifest: dict[str, object]) -> dict[str, object]:
    """Translate the mode-neutral v2 contract into the established event engine shape."""
    topology = manifest["topology"]
    workload = manifest["workload"]
    capabilities = manifest["capabilities"]
    environment = manifest["environment"]
    cache = manifest["cache"]
    assert isinstance(topology, dict)
    assert isinstance(workload, dict)
    assert isinstance(capabilities, dict)
    assert isinstance(environment, dict)
    assert isinstance(cache, dict)
    scheduler_policy = topology["scheduler_policy"]
    placement = {
        "round_robin": "round-robin",
        "rendezvous": "rendezvous",
        "dense_frontier": "rendezvous",
    }[scheduler_policy]
    if topology["assignment_source"] == "route_trace":
        placement = "trace"
    scheduler: dict[str, object] = {
        "ready_job_policy": (
            "trace"
            if topology["assignment_source"] == "route_trace"
            else "fifo-release"
        ),
        "placement_policy": placement,
    }
    if scheduler_policy == "dense_frontier" and placement == "rendezvous":
        scheduler["dense_frontier_workers"] = topology["dense_frontier_workers"]
    elif scheduler_policy == "rendezvous" and placement == "rendezvous":
        scheduler["dense_frontier_workers"] = topology["f_count"]
    jobs = []
    for source in workload["jobs"]:
        assert isinstance(source, dict)
        jobs.append(
            {
                "id": source["id"],
                "environment": source["c_index"],
                "trace": source["trace"],
                "corpus_root": source["corpus_root"],
                "manifest_digest": source["manifest_digest"],
                "compile_profile": source["compile_profile"],
                "builds": source["build_epochs"],
                "start_ns": 0,
                "build_release": source["build_release"],
                "tu_release": {"mode": "all-at-zero"},
            }
        )
    compile_profiles = {str(job["compile_profile"]) for job in jobs}
    worker_compile_profile = (
        next(iter(compile_profiles)) if len(compile_profiles) == 1 else "per-workload"
    )
    worker_template: dict[str, object] = {
        "slots": topology["f_slots"],
        "input_staging_slots": topology.get("input_staging_slots", topology["f_slots"]),
        "compile_profile": worker_compile_profile,
        "initial_cache": ("cold" if cache["initial_state"] == "cold" else "retained"),
    }
    normalized = {
        "schema": "icecream-distribution-scenario-v1",
        "name": manifest["name"],
        "seed": manifest["seed"],
        "environments": {
            "env_count": topology["c_count"],
            "job_selection": {"mode": "explicit", "jobs": jobs},
        },
        "workers": {"f_count": topology["f_count"], "template": worker_template},
        "network": topology["bandwidth"],
        "scheduler": scheduler,
        "experiment": {
            "codecs": [capabilities["codec_profile"]],
            "routing_mode": (
                "replay" if topology["assignment_source"] == "route_trace" else "online"
            ),
        },
        "v2_runtime": {
            "assignment_source": topology["assignment_source"],
            "route_trace": topology.get("route_trace"),
            "release_policy": workload["release_policy"],
            "environment": environment,
            "cache": cache,
            "capabilities": capabilities,
            "expected": manifest["expected"],
        },
    }
    return normalized


def load_execution(path: Path, scenario: LoadedScenario) -> dict[str, object]:
    path = path.resolve()
    document = json.loads(path.read_text())
    validate_json_schema(document, "execution.schema.json", path)
    if document["scenario_digest"] != scenario.scenario_digest:
        raise ValueError(
            f"{path}: execution scenario_digest differs from the loaded manifest"
        )
    if document["mode"] != "simulated":
        raise ValueError(f"{path}: run_scenario requires execution mode 'simulated'")
    expected_inputs = sorted(
        {
            str(workload["manifest_digest"])
            for workload in scenario.workload_inputs.values()
        }
    )
    if sorted(document["input_manifest_digests"]) != expected_inputs:
        raise ValueError(
            f"{path}: execution input_manifest_digests differ from loaded workload content"
        )
    return document


def read_trace(path: Path, *, require_content_identity: bool = False) -> list[TraceRow]:
    rows: list[TraceRow] = []
    with path.open(newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        required = {
            "logical",
            "job_id",
            "ii_relative",
            "raw_bytes",
            "compile_ns",
            "compile_model",
        }
        if reader.fieldnames is None or required - set(reader.fieldnames):
            raise ValueError(f"{path}: compile trace lacks {sorted(required)}")
        for expected, row in enumerate(reader):
            logical = checked_nonnegative_int(int(row["logical"]), "trace logical")
            if logical != expected:
                raise ValueError(
                    f"{path}: logical row {logical} is not contiguous at {expected}"
                )
            raw_bytes = checked_positive_int(int(row["raw_bytes"]), "trace raw_bytes")
            compile_ns = checked_positive_int(
                int(row["compile_ns"]), "trace compile_ns"
            )
            if not row["job_id"] or not row["ii_relative"] or not row["compile_model"]:
                raise ValueError(
                    f"{path}: trace row {logical} has an empty identity field"
                )
            raw_sha256 = row.get("raw_sha256") or None
            if raw_sha256 is not None and SHA256_RE.fullmatch(raw_sha256) is None:
                raise ValueError(f"{path}: trace row {logical} has invalid raw_sha256")
            if require_content_identity and raw_sha256 is None:
                raise ValueError(f"{path}: trace row {logical} lacks raw_sha256")
            compile_provenance = row.get("compile_provenance") or None
            if require_content_identity and compile_provenance not in {
                "observed",
                "modeled",
            }:
                raise ValueError(
                    f"{path}: trace row {logical} lacks observed/modeled compile_provenance"
                )
            if compile_provenance is None:
                compile_provenance = "observed"
            elif compile_provenance not in {"observed", "modeled"}:
                raise ValueError(
                    f"{path}: trace row {logical} has invalid compile_provenance"
                )
            rows.append(
                TraceRow(
                    logical,
                    row["job_id"],
                    row["ii_relative"],
                    raw_bytes,
                    compile_ns,
                    row["compile_model"],
                    raw_sha256,
                    compile_provenance,
                )
            )
    if not rows:
        raise ValueError(f"{path}: compile trace is empty")
    if len({row.job_id for row in rows}) != len(rows):
        raise ValueError(f"{path}: compile trace repeats a job_id")
    return rows


def read_release_offsets(
    config: dict[str, object], trace_rows: list[TraceRow], base: Path
) -> list[int]:
    mode = config.get("mode")
    if mode == "all-at-zero":
        return [0] * len(trace_rows)
    if mode != "trace":
        raise ValueError(f"unknown TU release mode {mode!r}")
    release_value = config.get("trace")
    if not isinstance(release_value, str) or not release_value:
        raise ValueError("TU release trace mode requires a trace path")
    release_path = (base / release_value).resolve()
    result: list[int] = []
    with release_path.open(newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        if reader.fieldnames is None or {"logical", "release_ns"} - set(
            reader.fieldnames
        ):
            raise ValueError(f"{release_path}: release trace lacks logical/release_ns")
        for expected, row in enumerate(reader):
            logical = checked_nonnegative_int(int(row["logical"]), "release logical")
            if logical != expected:
                raise ValueError(f"{release_path}: non-contiguous release row")
            result.append(checked_nonnegative_int(int(row["release_ns"]), "release_ns"))
    if len(result) != len(trace_rows):
        raise ValueError(
            f"{release_path}: release row count differs from compile trace"
        )
    if result != sorted(result):
        raise ValueError(f"{release_path}: release times move backwards")
    return result


def load_route_trace(
    path: Path,
    scenario_digest: str,
    work_items: Mapping[tuple[str, int], Sequence[WorkItem]],
    f_count: int,
    expected_codec: str,
) -> tuple[dict[int, RouteTraceEntry], str, str]:
    path = path.resolve()
    rows = [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
    if len(rows) < 3:
        raise ValueError(f"{path}: route trace is incomplete")
    for index, row in enumerate(rows, start=1):
        validate_json_schema(row, "route-trace.schema.json", f"{path}:{index}")
    header, summary = rows[0], rows[-1]
    if header["record"] != "route_trace" or summary["record"] != "route_summary":
        raise ValueError(f"{path}: route trace boundary records are invalid")
    if header["scenario_digest"] != scenario_digest:
        raise ValueError(f"{path}: route trace belongs to a different scenario")
    if header["codec_profile"] != expected_codec:
        raise ValueError(
            f"{path}: route trace codec {header['codec_profile']!r} differs from "
            f"selected adapter {expected_codec!r}"
        )
    expected_items = {item.key: item for items in work_items.values() for item in items}
    entries: dict[int, RouteTraceEntry] = {}
    seen_keys: set[tuple[str, int, int]] = set()
    by_environment: dict[int, list[int]] = defaultdict(list)
    by_route: dict[tuple[int, int], list[int]] = defaultdict(list)
    c_identity_by_environment: dict[int, str] = {}
    f_identity_by_worker: dict[int, tuple[str, str]] = {}
    identity_by_route: dict[tuple[int, int], tuple[object, ...]] = {}
    c_to_f_total = 0
    f_to_c_total = 0
    for row_number, row in enumerate(rows[1:-1], start=2):
        if row["record"] != "assignment":
            raise ValueError(f"{path}:{row_number}: expected assignment record")
        key = (row["workload"], row["build"], row["logical"])
        item = expected_items.get(key)
        if item is None or key in seen_keys:
            raise ValueError(f"{path}:{row_number}: unknown or repeated TU {key}")
        seen_keys.add(key)
        expected_job_id = logical_job_identity(scenario_digest, item)
        if row["logical_job_id"] != expected_job_id:
            raise ValueError(f"{path}:{row_number}: logical_job_id differs for {key}")
        expected_attempt = f"{expected_job_id}:attempt-0"
        if row["attempt_id"] != expected_attempt:
            raise ValueError(f"{path}:{row_number}: attempt_id differs for {key}")
        worker = checked_nonnegative_int(row["worker"], "route-trace worker")
        if worker >= f_count:
            raise ValueError(f"{path}:{row_number}: worker is outside topology")
        tu_seq = checked_nonnegative_int(row["TU_SEQ"], "route-trace TU_SEQ")
        rel_seq = checked_nonnegative_int(row["REL_SEQ"], "route-trace REL_SEQ")
        entry = RouteTraceEntry(
            row["logical_job_id"],
            row["attempt_id"],
            worker,
            tu_seq,
            rel_seq,
            row["C_STORE_GUID"],
            row["physical_endpoint"],
            row["RouteLaneId"],
            row["F_STORE_GUID"],
            checked_positive_int(row["session_serial"], "route-trace session_serial"),
            checked_nonnegative_int(row["HISTORY_NONCE"], "route-trace HISTORY_NONCE"),
            checked_nonnegative_int(row["c_to_f_bytes"], "route-trace c_to_f_bytes"),
            checked_nonnegative_int(row["f_to_c_bytes"], "route-trace f_to_c_bytes"),
        )
        entries[item.ordinal] = entry
        by_environment[item.environment].append(tu_seq)
        route = (item.environment, worker)
        by_route[route].append(rel_seq)
        c_identity = c_identity_by_environment.setdefault(
            item.environment, entry.c_store_guid
        )
        if c_identity != entry.c_store_guid:
            raise ValueError(
                f"{path}:{row_number}: C identity drifted in environment {item.environment}"
            )
        f_identity = f_identity_by_worker.setdefault(
            worker, (entry.physical_endpoint, entry.f_store_guid)
        )
        if f_identity != (entry.physical_endpoint, entry.f_store_guid):
            raise ValueError(
                f"{path}:{row_number}: F identity drifted for worker {worker}"
            )
        route_identity = (
            entry.c_store_guid,
            entry.physical_endpoint,
            entry.route_lane_id,
            entry.f_store_guid,
            entry.session_serial,
            entry.history_nonce,
        )
        established = identity_by_route.setdefault(route, route_identity)
        if established != route_identity:
            raise ValueError(
                f"{path}:{row_number}: route identity drifted on {route}; "
                "this trace declares no identity transition"
            )
        c_to_f_total += entry.c_to_f_bytes
        f_to_c_total += entry.f_to_c_bytes
    if seen_keys != set(expected_items):
        missing = sorted(set(expected_items) - seen_keys)
        raise ValueError(f"{path}: route trace misses TU {missing[:1]}")
    for environment, values in by_environment.items():
        if sorted(values) != list(range(len(values))):
            raise ValueError(f"{path}: C{environment} TU_SEQ set is not contiguous")
    for route, values in by_route.items():
        if sorted(values) != list(range(len(values))):
            raise ValueError(f"{path}: route {route} REL_SEQ set is not contiguous")
    expected_summary = {
        "record": "route_summary",
        "assignments": len(entries),
        "c_to_f_bytes": c_to_f_total,
        "f_to_c_bytes": f_to_c_total,
    }
    if summary != expected_summary:
        raise ValueError(
            f"{path}: route summary {summary!r} differs from {expected_summary!r}"
        )
    return entries, sha256(path), str(header["provenance"])


def load_scenario(
    path: Path, payload_overrides: dict[str, Path] | None = None
) -> LoadedScenario:
    path = path.resolve()
    source_document = json.loads(path.read_text())
    if not isinstance(source_document, dict):
        raise ValueError(f"{path}: scenario must be an object")
    schema = source_document.get("schema")
    selected_adapter: str | None = None
    if schema == "icecream-experiment-v2":
        validate_json_schema(source_document, "experiment.schema.json", path)
        manifest = source_document
        capabilities = manifest["capabilities"]
        expected = manifest["expected"]
        assert isinstance(capabilities, dict) and isinstance(expected, dict)
        if expected["selected_cache_wire"] != capabilities["cache_wire"]:
            raise ValueError(f"{path}: selected_cache_wire differs from capability")
        if expected["selected_codec_profile"] != capabilities["codec_profile"]:
            raise ValueError(f"{path}: selected_codec_profile differs from capability")
        component_protocols = [
            int(component["main_protocol"])
            for component in manifest["components"].values()
        ]
        negotiated_main_protocol = min(component_protocols)
        if expected["selected_main_protocol"] != negotiated_main_protocol:
            raise ValueError(
                f"{path}: selected_main_protocol differs from component negotiation "
                f"({negotiated_main_protocol})"
            )
        if expected["compile_result"] != "pass":
            raise ValueError(
                f"{path}: this simulator models successful compile completion only"
            )
        control = capabilities.get("experiment_control")
        if (
            control in {"compile_only", "raw"}
            and capabilities["codec_profile"] != "legacy"
        ):
            raise ValueError(f"{path}: basic controls require the legacy profile label")
        if (
            control == "z3_shared_long_b1"
            and capabilities["codec_profile"] != "z3_shared_long"
        ):
            raise ValueError(
                f"{path}: z3_shared_long_b1 control requires z3_shared_long"
            )
        selected_adapter = selected_v2_adapter(manifest)
        scenario_digest = canonical_json_sha256(manifest)
        document = normalize_v2_manifest(manifest)
    elif schema == "icecream-distribution-scenario-v1":
        manifest = source_document
        scenario_digest = sha256(path)
        document = source_document
    else:
        raise ValueError(f"{path}: unknown scenario schema")
    environments = document.get("environments")
    workers = document.get("workers")
    network = document.get("network")
    scheduler = document.get("scheduler")
    if not all(
        isinstance(value, dict) for value in (environments, workers, network, scheduler)
    ):
        raise ValueError(f"{path}: scenario lacks a required object")
    checked_nonnegative_int(document.get("seed"), "seed")
    env_count = checked_positive_int(environments.get("env_count"), "env_count")
    f_count = checked_positive_int(workers.get("f_count"), "f_count")
    template = workers.get("template")
    if not isinstance(template, dict):
        raise ValueError("workers.template is not an object")
    checked_positive_int(template.get("slots"), "worker slots")
    if "input_staging_slots" in template:
        checked_positive_int(
            template.get("input_staging_slots"), "worker input_staging_slots"
        )
    for direction in DIRECTIONS:
        link = network.get(direction)
        if not isinstance(link, dict):
            raise ValueError(f"network.{direction} is not an object")
        checked_positive_int(
            link.get("bits_per_second"), f"network.{direction}.bits_per_second"
        )
        checked_nonnegative_int(
            link.get("one_way_latency_ns"), f"network.{direction}.one_way_latency_ns"
        )
        checked_positive_int(
            link.get("lanes_per_endpoint"), f"network.{direction}.lanes_per_endpoint"
        )
        for optional_capacity in (
            "per_environment_bits_per_second",
            "per_worker_bits_per_second",
            "fabric_bits_per_second",
            "writer_quantum_bytes",
            "max_priority_burst_quanta",
        ):
            if optional_capacity in link:
                checked_positive_int(
                    link.get(optional_capacity),
                    f"network.{direction}.{optional_capacity}",
                )
    if "shared_fabric_bps" in network:
        checked_positive_int(
            network.get("shared_fabric_bps"), "network.shared_fabric_bps"
        )
    elif any(
        "fabric_bits_per_second" not in network[direction] for direction in DIRECTIONS
    ):
        raise ValueError(
            "network needs shared_fabric_bps or one fabric_bits_per_second per direction"
        )
    if scheduler.get("ready_job_policy") not in {
        "fifo-release",
        "environment-round-robin",
        "shortest-known",
        "trace",
    }:
        raise ValueError("unknown ready-job policy")
    if scheduler.get("placement_policy") not in {
        "round-robin",
        "fastest",
        "resident",
        "home",
        "rendezvous",
        "r4-state",
        "r5-bounded",
        "trace",
    }:
        raise ValueError("unknown placement policy")
    dense_frontier_workers = scheduler.get("dense_frontier_workers")
    if dense_frontier_workers is not None:
        dense_frontier_workers = checked_positive_int(
            dense_frontier_workers, "scheduler.dense_frontier_workers"
        )
        if scheduler.get("placement_policy") != "rendezvous":
            raise ValueError(
                "scheduler.dense_frontier_workers requires rendezvous placement"
            )
        if dense_frontier_workers > f_count:
            raise ValueError("scheduler.dense_frontier_workers exceeds workers.f_count")

    selection = environments.get("job_selection")
    if not isinstance(selection, dict) or not isinstance(selection.get("jobs"), list):
        raise ValueError("environments.job_selection.jobs is not an array")
    overrides = payload_overrides or {}
    work_items: dict[tuple[str, int], list[WorkItem]] = {}
    workload_config: dict[str, dict[str, object]] = {}
    workload_inputs: dict[str, dict[str, object]] = {}
    next_ordinal = 0
    ids: set[str] = set()
    for job in selection["jobs"]:
        if not isinstance(job, dict):
            raise ValueError("job selection row is not an object")
        workload = job.get("id")
        if not isinstance(workload, str) or not workload or workload in ids:
            raise ValueError("workload IDs must be nonempty and unique")
        ids.add(workload)
        environment = checked_nonnegative_int(
            job.get("environment"), f"{workload}.environment"
        )
        if environment >= env_count:
            raise ValueError(
                f"{workload}: environment {environment} is outside env_count"
            )
        builds = checked_positive_int(job.get("builds"), f"{workload}.builds")
        start_ns = checked_nonnegative_int(job.get("start_ns"), f"{workload}.start_ns")
        trace_value = job.get("trace")
        root_value = job.get("corpus_root")
        if not isinstance(trace_value, str) or not isinstance(root_value, str):
            raise ValueError(f"{workload}: trace/corpus_root must be strings")
        if schema == "icecream-experiment-v2":
            trace_identity = canonical_relative_identity(
                trace_value, f"{workload}.trace"
            )
            corpus_identity = canonical_relative_identity(
                root_value, f"{workload}.corpus_root", allow_dot=True
            )
        else:
            trace_identity = trace_value
            corpus_identity = root_value
        trace_path = (path.parent / trace_value).resolve()
        corpus_root = overrides.get(workload, Path(root_value))
        if not corpus_root.is_absolute():
            corpus_root = (path.parent / corpus_root).resolve()
        trace_rows = read_trace(
            trace_path, require_content_identity=schema == "icecream-experiment-v2"
        )
        manifest_digest = job.get("manifest_digest")
        if manifest_digest is not None and (
            not isinstance(manifest_digest, str)
            or SHA256_RE.fullmatch(manifest_digest) is None
        ):
            raise ValueError(f"{workload}: manifest_digest is not canonical SHA-256")
        verified_digests: dict[int, str] = {}
        if schema == "icecream-experiment-v2":
            for row in trace_rows:
                relative = canonical_relative_identity(
                    row.ii_relative, f"{workload} trace row {row.logical} ii_relative"
                )
                payload = corpus_root / relative
                if not payload.is_file():
                    raise ValueError(
                        f"{workload}: v2 payload is absent for TU {row.logical}: {relative}"
                    )
                if payload.stat().st_size != row.raw_bytes:
                    raise ValueError(
                        f"{workload}: payload size differs for TU {row.logical}"
                    )
                observed_digest = sha256(payload)
                if row.raw_sha256 != observed_digest:
                    raise ValueError(
                        f"{workload}: payload digest differs for TU {row.logical}"
                    )
                verified_digests[row.logical] = observed_digest
            content_manifest = workload_content_manifest(trace_rows)
            validate_json_schema(
                content_manifest,
                "workload-input.schema.json",
                f"{workload} canonical workload content",
            )
            observed_manifest_digest = canonical_json_sha256(content_manifest)
            if manifest_digest != observed_manifest_digest:
                raise ValueError(
                    f"{workload}: manifest_digest differs from canonical workload content"
                )
        else:
            content_manifest = None
        workload_input: dict[str, object] = {
            "trace": (
                trace_identity
                if schema == "icecream-experiment-v2"
                else str(trace_path)
            ),
            "trace_sha256": sha256(trace_path),
            "corpus_root": (
                corpus_identity
                if schema == "icecream-experiment-v2"
                else str(corpus_root)
            ),
            "trace_rows": len(trace_rows),
        }
        if manifest_digest is not None:
            workload_input["manifest_digest"] = manifest_digest
        if content_manifest is not None:
            workload_input["content_manifest"] = content_manifest
        workload_inputs[workload] = workload_input
        expected_model = job.get("compile_profile", template.get("compile_profile"))
        models = {row.compile_model for row in trace_rows}
        if models != {expected_model}:
            raise ValueError(
                f"{workload}: trace compile models {sorted(models)} do not match {expected_model!r}"
            )
        release_config = job.get("tu_release")
        build_release = job.get("build_release")
        if not isinstance(release_config, dict) or not isinstance(build_release, dict):
            raise ValueError(f"{workload}: release configuration is missing")
        release_offsets = read_release_offsets(release_config, trace_rows, path.parent)
        build_mode = build_release.get("mode")
        if build_mode not in {"after-previous", "concurrent", "fixed-interval"}:
            raise ValueError(f"{workload}: unknown build release mode {build_mode!r}")
        if build_mode == "fixed-interval":
            checked_nonnegative_int(
                build_release.get("interval_ns"), "build interval_ns"
            )
        elif "interval_ns" in build_release:
            raise ValueError(
                f"{workload}: interval_ns is valid only for fixed-interval release"
            )
        if build_mode == "after-previous":
            checked_nonnegative_int(build_release.get("gap_ns", 0), "build gap_ns")
        elif "gap_ns" in build_release:
            raise ValueError(
                f"{workload}: gap_ns is valid only for after-previous release"
            )
        workload_config[workload] = job
        for build in range(builds):
            items: list[WorkItem] = []
            for row, release_offset in zip(trace_rows, release_offsets):
                payload = corpus_root / row.ii_relative
                raw_digest = row.raw_sha256
                if schema == "icecream-experiment-v2":
                    raw_digest = verified_digests[row.logical]
                items.append(
                    WorkItem(
                        next_ordinal,
                        environment,
                        workload,
                        build,
                        row.logical,
                        row.job_id,
                        payload,
                        row.raw_bytes,
                        row.compile_ns,
                        release_offset,
                        raw_digest,
                        row.compile_provenance,
                    )
                )
                next_ordinal += 1
            work_items[(workload, build)] = items
        workload_config[workload] = {**job, "start_ns": start_ns, "builds": builds}
    if f_count < 1:
        raise AssertionError("validated f_count became empty")
    route_trace: dict[int, RouteTraceEntry] = {}
    route_trace_path: Path | None = None
    route_trace_digest: str | None = None
    route_trace_identity: str | None = None
    route_trace_codec: str | None = None
    route_trace_provenance: str | None = None
    runtime = document.get("v2_runtime", {})
    if isinstance(runtime, dict) and runtime.get("assignment_source") == "route_trace":
        route_value = runtime.get("route_trace")
        if not isinstance(route_value, str) or not route_value:
            raise ValueError("route_trace assignment requires topology.route_trace")
        route_trace_identity = canonical_relative_identity(
            route_value, "topology.route_trace"
        )
        if selected_adapter is None:
            raise ValueError("route_trace requires an implemented selected adapter")
        route_trace_path = Path(route_value)
        if not route_trace_path.is_absolute():
            route_trace_path = (path.parent / route_trace_path).resolve()
        route_trace, route_trace_digest, route_trace_provenance = load_route_trace(
            route_trace_path, scenario_digest, work_items, f_count, selected_adapter
        )
        route_trace_codec = selected_adapter
    return LoadedScenario(
        document,
        manifest,
        path,
        scenario_digest,
        work_items,
        workload_config,
        workload_inputs,
        route_trace,
        route_trace_path,
        route_trace_digest,
        route_trace_identity,
        route_trace_codec,
        route_trace_provenance,
    )


def stable_rendezvous_score(*parts: object) -> int:
    """Return one portable score without relying on Python's randomized hash."""
    digest = hashlib.sha256(b"icecream-static-rendezvous-v1\0")
    for part in parts:
        encoded = str(part).encode("utf-8")
        digest.update(len(encoded).to_bytes(4, "big"))
        digest.update(encoded)
    return int.from_bytes(digest.digest()[:8], "big")


def static_rendezvous_routes(
    scenario: LoadedScenario,
) -> tuple[dict[int, int], dict[tuple[int, str], tuple[int, ...]]]:
    """Bind every stable compile identity to one F in a dense home frontier.

    A workload is the simulator's project/build-family identity.  Its home frontier excludes
    the build number, and a TU uses ``source_job_id`` rather than chronological/logical order,
    so unchanged compile identities return to the same F on later builds.  The scenario seed
    makes the otherwise opaque placement reproducible.
    """
    document = scenario.document
    scheduler = document["scheduler"]
    if scheduler["placement_policy"] != "rendezvous":
        raise ValueError("static rendezvous routes require rendezvous placement")
    f_count = int(document["workers"]["f_count"])
    frontier_width = int(scheduler.get("dense_frontier_workers", f_count))
    profile = document["workers"]["template"]["compile_profile"]
    seed = int(document["seed"])
    home_sets: dict[tuple[int, str], tuple[int, ...]] = {}
    bindings: dict[int, int] = {}
    for items in scenario.work_items.values():
        for item in items:
            family = (item.environment, item.workload)
            homes = home_sets.get(family)
            if homes is None:
                ranked = sorted(
                    range(f_count),
                    key=lambda worker: (
                        -stable_rendezvous_score(
                            seed,
                            "home",
                            item.environment,
                            item.workload,
                            profile,
                            worker,
                        ),
                        worker,
                    ),
                )
                homes = tuple(ranked[:frontier_width])
                home_sets[family] = homes
            worker = min(
                homes,
                key=lambda candidate: (
                    -stable_rendezvous_score(
                        seed,
                        "tu",
                        item.environment,
                        item.workload,
                        profile,
                        item.source_job_id,
                        candidate,
                    ),
                    candidate,
                ),
            )
            bindings[item.ordinal] = worker
    return bindings, home_sets


class Simulator:
    def __init__(
        self,
        scenario: LoadedScenario,
        adapter: CodecAdapter,
        snapshot_interval_ns: int = DEFAULT_SNAPSHOT_NS,
        timeline_spool_path: Path | None = None,
        event_spool_path: Path | None = None,
    ):
        self.scenario = scenario
        self.adapter = adapter
        document = scenario.document
        worker_config = document["workers"]
        self.env_count = int(document["environments"]["env_count"])
        self.f_count = int(worker_config["f_count"])
        self.slots_per_f = int(worker_config["template"]["slots"])
        self.decoupled_staging = "input_staging_slots" in worker_config["template"]
        self.input_staging_slots_per_f = int(
            worker_config["template"].get("input_staging_slots", self.slots_per_f)
        )
        self.network = document["network"]
        self.scheduler = document["scheduler"]
        runtime = document.get("v2_runtime", {})
        self.v2_runtime = runtime if isinstance(runtime, dict) else {}
        capability_config = self.v2_runtime.get("capabilities", {})
        if not isinstance(capability_config, dict):
            capability_config = {}
        profile = str(capability_config.get("codec_profile", adapter.name))
        self.negotiated_profiles = [profile]
        self.route_state_profiles = [profile]
        environment_config = self.v2_runtime.get(
            "environment",
            {
                "initial_state": "resident",
                "image_digest": "",
                "transfer_bytes": 0,
                "install_verify_ns": 0,
            },
        )
        if not isinstance(environment_config, dict):
            raise ValueError("v2_runtime.environment is not an object")
        self.environment_config = environment_config
        self.environment_initial_state = str(
            environment_config.get("initial_state", "resident")
        )
        self.environment_transfer_bytes = int(
            environment_config.get("transfer_bytes", 0)
        )
        self.environment_install_verify_ns = int(
            environment_config.get("install_verify_ns", 0)
        )
        self.now = Fraction(0)
        self.release_heap: list[tuple[int, int, WorkItem]] = []
        self.ready_heap: list[tuple[tuple[int, ...], int, WorkItem]] = []
        self.ready_by_environment: dict[int, deque[WorkItem]] = defaultdict(deque)
        self.ready_environment_cycle: deque[int] = deque()
        self.trace_routing = self.scheduler["placement_policy"] == "trace"
        self.static_routing = self.scheduler["placement_policy"] in {
            "rendezvous",
            "trace",
        }
        self.static_route_workers: dict[int, int] = {}
        self.static_home_sets: dict[tuple[int, str], tuple[int, ...]] = {}
        if self.trace_routing:
            if not scenario.route_trace:
                raise ValueError("trace placement has no loaded route trace")
            self.static_route_workers = {
                ordinal: entry.worker for ordinal, entry in scenario.route_trace.items()
            }
        elif self.static_routing:
            self.static_route_workers, self.static_home_sets = static_rendezvous_routes(
                scenario
            )
        self._routing_metadata_cache: dict[str, object] | None = None
        self.static_ready_heaps: dict[
            int, list[tuple[tuple[int, ...], int, WorkItem]]
        ] = defaultdict(list)
        self.static_ready_by_environment_worker: dict[
            tuple[int, int], deque[WorkItem]
        ] = defaultdict(deque)
        self.compiler_slots: dict[int, SparseSlotPool] = {
            worker: SparseSlotPool(self.slots_per_f) for worker in range(self.f_count)
        }
        # Historical callers inspect ``free_slots``; it remains the compiler pool.
        self.free_slots = self.compiler_slots
        self.staging_slots: dict[int, SparseSlotPool] = {
            worker: SparseSlotPool(self.input_staging_slots_per_f)
            for worker in range(self.f_count)
        }
        self.ready_compiler_queues: dict[int, deque[Transaction]] = defaultdict(deque)
        self.compile_heap: list[tuple[Fraction, int, Transaction]] = []
        self.environment_install_heap: list[
            tuple[Fraction, int, int, EnvironmentEnsure]
        ] = []
        self.delivery_heap: list[tuple[Fraction, int, Flow]] = []
        self.active_flows: dict[int, Flow] = {}
        self.endpoint_queues: dict[
            tuple[str, int, int], list[tuple[int, int, Flow]]
        ] = defaultdict(list)
        self.endpoint_priority_overtakes: dict[tuple[str, int, int], int] = defaultdict(
            int
        )
        self.dialogue_active: dict[tuple[int, int], int] = defaultdict(int)
        self.dialogue_queues: dict[tuple[int, int], deque[Transaction]] = defaultdict(
            deque
        )
        self.relationship_next_assigned: dict[tuple[int, int], int] = defaultdict(int)
        self.relationship_next_committed: dict[tuple[int, int], int] = defaultdict(int)
        self.transactions: list[Transaction] = []
        self.ledger = ExactLedger()
        self.events = EventRecorder(event_spool_path)
        self.event_sequence = 0
        self.flow_sequence = 0
        self.transaction_sequence = 0
        self.environment_next_tu_seq = [0] * self.env_count
        self.prepared_tu_seq: dict[int, int] = {}
        self.round_robin_cursor = 0
        self.ready_environment_members: set[int] = set()
        self.completed = 0
        self.total_items = sum(len(items) for items in scenario.work_items.values())
        self.build_remaining = {
            key: len(items) for key, items in scenario.work_items.items()
        }
        self.worker_compile_ns = [0] * self.f_count
        self.worker_reserved_ns = [Fraction(0) for _ in range(self.f_count)]
        self.worker_raw_bytes = [0] * self.f_count
        self.worker_c_to_f = [0] * self.f_count
        self.worker_f_to_c = [0] * self.f_count
        self.worker_environment_c_to_f = [0] * self.f_count
        self.env_unreleased = [0] * self.env_count
        for items in scenario.work_items.values():
            for item in items:
                self.env_unreleased[item.environment] += 1
        self.env_ready = [0] * self.env_count
        self.env_active = [0] * self.env_count
        self.env_completed = [0] * self.env_count
        self.worker_input_wait = [0] * self.f_count
        self.worker_ready_input = [0] * self.f_count
        self.worker_compiling = [0] * self.f_count
        self.worker_commit_wait = [0] * self.f_count
        self.worker_environment_wait = [0] * self.f_count
        self.worker_dispatched = [0] * self.f_count
        self.worker_completed = [0] * self.f_count
        self.environment_status: dict[tuple[int, int], str] = {
            (environment, worker): self.environment_initial_state
            for environment in range(self.env_count)
            for worker in range(self.f_count)
        }
        self.environment_ensures: dict[tuple[int, int], EnvironmentEnsure] = {}
        self.environment_waiters: dict[tuple[int, int], deque[Transaction]] = (
            defaultdict(deque)
        )
        self.c_store_guids = [
            stable_hex("c-store", scenario.scenario_digest, environment, digits=32)
            for environment in range(self.env_count)
        ]
        self.f_store_guids = [
            stable_hex("f-store", scenario.scenario_digest, worker, digits=32)
            for worker in range(self.f_count)
        ]
        self.timeline = TimelineRecorder(
            snapshot_interval_ns,
            self.f_count,
            timeline_spool_path,
        )
        self._seed_releases()

    def _seed_releases(self) -> None:
        for workload, config in self.scenario.workload_config.items():
            mode = config["build_release"]["mode"]
            builds = int(config["builds"])
            start = int(config["start_ns"])
            if mode == "after-previous":
                self._enqueue_build(workload, 0, start)
            else:
                interval = int(config["build_release"].get("interval_ns", 0))
                for build in range(builds):
                    base = start + (build * interval if mode == "fixed-interval" else 0)
                    self._enqueue_build(workload, build, base)

    def _enqueue_build(self, workload: str, build: int, base_ns: int) -> None:
        for item in self.scenario.work_items[(workload, build)]:
            item.release_ns = base_ns + item.release_offset_ns
            heapq.heappush(self.release_heap, (item.release_ns, item.ordinal, item))

    def _actor_for_event(self, name: str, phase: Phase | None) -> str:
        if name in {"release", "route-bound"}:
            return "C_authority" if name == "release" else "scheduler"
        if name in {"dispatch", "compiler-queued"}:
            return "scheduler"
        if name.startswith("compile") or name == "transaction-complete":
            return "F_compiler"
        if name.startswith("env_") or name == "environment_ready":
            return "F_environment_store"
        if phase is not None:
            return "C_cache" if phase.direction == "c_to_f" else "F_cache"
        if name in {"input-ready", "transaction-commit", "dialogue-finish"}:
            return "F_cache"
        return "simulator"

    @staticmethod
    def _canonical_stage(name: str) -> str:
        return {
            "release": "job_release",
            "route-bound": "scheduler_select",
            "dispatch": "route_prepare",
            "dialogue-start": "c_queue",
            "input-ready": "input_commit_visible",
            "compiler-queued": "compiler_pipe",
            "compile-start": "compile",
            "compile-finish": "compile",
            "transaction-commit": "route_reconcile",
            "transaction-complete": "job_finish",
            "env_transfer": "env_transfer",
            "env_install_verify": "env_install_verify",
            "environment_ready": "environment_ready",
        }.get(name, name.replace("-", "_"))

    def _event(
        self,
        name: str,
        tx: Transaction | None = None,
        phase: Phase | None = None,
        item: WorkItem | None = None,
        flow: Flow | None = None,
        detail: str = "",
        worker: int | None = None,
        tu_seq: int | None = None,
        c_to_f_byte_delta: int = 0,
        f_to_c_byte_delta: int = 0,
        resource_byte_delta: Mapping[str, int] | None = None,
        queue_byte_delta: Mapping[str, int] | None = None,
        start_ns: Fraction | int | None = None,
        end_ns: Fraction | int | None = None,
        transition: str = "instant",
        provenance: Mapping[str, str] | None = None,
    ) -> None:
        item = tx.item if tx is not None else item
        event_worker = tx.worker if tx is not None else worker
        event_tu_seq = tx.tu_seq if tx is not None else tu_seq
        route = (
            None
            if item is None or event_worker is None
            else (item.environment, event_worker)
        )
        account = "source" if phase is None else phase.account
        self.ledger.apply(
            c_to_f=c_to_f_byte_delta,
            f_to_c=f_to_c_byte_delta,
            account=account,
            route=route,
            resource=resource_byte_delta,
            queue=queue_byte_delta,
        )
        now_ns = ceil_fraction(self.now)
        start_value = now_ns if start_ns is None else ceil_fraction(Fraction(start_ns))
        end_value = now_ns if end_ns is None else ceil_fraction(Fraction(end_ns))
        if end_value < start_value:
            raise RuntimeError("event interval moves backwards")
        trace_entry = (
            None if item is None else self.scenario.route_trace.get(item.ordinal)
        )
        if (
            trace_entry is not None
            and event_worker is not None
            and trace_entry.worker != event_worker
        ):
            raise RuntimeError("route event worker differs from its trace identity")
        logical_job_id = (
            None
            if item is None
            else logical_job_identity(self.scenario.scenario_digest, item)
        )
        attempt_id = None if logical_job_id is None else f"{logical_job_id}:attempt-0"
        if tx is not None:
            identity = {
                "logical_job_id": tx.logical_job_id,
                "attempt_id": tx.attempt_id,
                "C_STORE_GUID": tx.c_store_guid,
                "physical_endpoint": tx.physical_endpoint,
                "RouteLaneId": tx.route_lane_id,
                "F_STORE_GUID": tx.f_store_guid,
                "session_serial": tx.session_serial,
                "HISTORY_NONCE": tx.history_nonce,
                "REL_SEQ": tx.rel_seq,
                "TU_SEQ": tx.tu_seq,
                "transaction_digest": tx.transaction_digest,
                "InputRecord_identity": tx.input_record_identity,
            }
        else:
            identity = {
                "logical_job_id": logical_job_id,
                "attempt_id": attempt_id,
                "C_STORE_GUID": (
                    None
                    if item is None
                    else (
                        trace_entry.c_store_guid
                        if trace_entry is not None
                        else self.c_store_guids[item.environment]
                    )
                ),
                "physical_endpoint": (
                    None
                    if event_worker is None
                    else (
                        trace_entry.physical_endpoint
                        if trace_entry is not None
                        else f"F{event_worker}"
                    )
                ),
                "RouteLaneId": (
                    None
                    if item is None or event_worker is None
                    else (
                        trace_entry.route_lane_id
                        if trace_entry is not None
                        else f"C{item.environment}_F{event_worker}"
                    )
                ),
                "F_STORE_GUID": (
                    None
                    if event_worker is None
                    else (
                        trace_entry.f_store_guid
                        if trace_entry is not None
                        else self.f_store_guids[event_worker]
                    )
                ),
                "session_serial": (
                    None
                    if event_worker is None
                    else (trace_entry.session_serial if trace_entry is not None else 1)
                ),
                "HISTORY_NONCE": (
                    None
                    if item is None or event_worker is None
                    else (
                        trace_entry.history_nonce
                        if trace_entry is not None
                        else int(
                            stable_hex(
                                "history-nonce",
                                self.scenario.scenario_digest,
                                item.environment,
                                event_worker,
                                digits=16,
                            ),
                            16,
                        )
                    )
                ),
                "REL_SEQ": (
                    trace_entry.rel_seq
                    if trace_entry is not None and event_worker is not None
                    else None
                ),
                "TU_SEQ": event_tu_seq,
                "transaction_digest": None,
                "InputRecord_identity": None,
            }
        event_provenance = {
            "timing": "modeled",
            "byte_delta": (
                "modeled"
                if phase is not None and phase.account == "environment"
                else (
                    "observed"
                    if phase is not None
                    and self.adapter.physical
                    and (c_to_f_byte_delta or f_to_c_byte_delta)
                    else "derived"
                )
            ),
            "route_identity": (
                str(self.scenario.route_trace_provenance)
                if trace_entry is not None
                and event_worker is not None
                and self.scenario.route_trace_provenance is not None
                else "derived"
            ),
            "raw_digest": (
                "observed"
                if item is not None and item.raw_digest is not None
                else "derived"
            ),
            "compile_duration_input": (
                item.compile_provenance if item is not None else "derived"
            ),
            "resource_delta": "derived",
            "queue_delta": "derived",
        }
        if provenance is not None:
            event_provenance.update(provenance)
        self.events.append(
            {
                "sequence": self.event_sequence,
                "time_ns": ceil_fraction(self.now),
                "event": name,
                "environment": "" if item is None else item.environment,
                "workload": "" if item is None else item.workload,
                "build": "" if item is None else item.build,
                "logical": "" if item is None else item.logical,
                "worker": "" if event_worker is None else event_worker,
                "slot": "" if tx is None else tx.slot,
                "staging_slot": "" if tx is None else tx.staging_slot,
                "compiler_slot": "" if tx is None else tx.compiler_slot,
                "transaction": "" if tx is None else tx.sequence,
                "tu_seq": "" if event_tu_seq is None else event_tu_seq,
                "rel_seq": "" if tx is None else tx.rel_seq,
                "flow": "" if flow is None else flow.sequence,
                "phase": "" if phase is None else phase.name,
                "direction": "" if phase is None else phase.direction,
                "bytes": "" if phase is None else phase.byte_count,
                "detail": detail,
                "stage": self._canonical_stage(name),
                "transition": transition,
                **identity,
                "raw_digest": None if item is None else item.raw_digest,
                "negotiated_profiles": list(self.negotiated_profiles),
                "route_state_profiles": list(self.route_state_profiles),
                "actor": self._actor_for_event(name, phase),
                "start_ns": start_value,
                "end_ns": end_value,
                "duration_ns": end_value - start_value,
                "provenance": event_provenance,
                "byte_account": account,
                "c_to_f_byte_delta": c_to_f_byte_delta,
                "f_to_c_byte_delta": f_to_c_byte_delta,
                "resource_byte_delta": dict(
                    sorted((resource_byte_delta or {}).items())
                ),
                "queue_byte_delta": dict(sorted((queue_byte_delta or {}).items())),
            }
        )
        self.event_sequence += 1

    def _ready_key(self, item: WorkItem) -> tuple[int, ...]:
        policy = self.scheduler["ready_job_policy"]
        if policy == "fifo-release":
            return int(item.release_ns), item.ordinal
        if policy == "shortest-known":
            return item.compile_ns, int(item.release_ns), item.ordinal
        if policy == "trace":
            if self.trace_routing:
                return (self.scenario.route_trace[item.ordinal].rel_seq, item.ordinal)
            return (item.ordinal,)
        raise RuntimeError(f"ready policy {policy!r} has no heap key")

    def _release_ready(self) -> None:
        while self.release_heap and self.release_heap[0][0] <= self.now:
            _, _, item = heapq.heappop(self.release_heap)
            self.env_unreleased[item.environment] -= 1
            self.env_ready[item.environment] += 1
            trace_entry = self.scenario.route_trace.get(item.ordinal)
            tu_seq = (
                trace_entry.tu_seq
                if trace_entry is not None
                else self.environment_next_tu_seq[item.environment]
            )
            self.environment_next_tu_seq[item.environment] += 1
            self.prepared_tu_seq[item.ordinal] = tu_seq
            self._event(
                "release",
                item=item,
                tu_seq=tu_seq,
                resource_byte_delta={"prepared_input_bytes": item.raw_bytes},
                queue_byte_delta={"scheduler_ready_bytes": item.raw_bytes},
            )
            policy = self.scheduler["ready_job_policy"]
            if self.static_routing:
                worker = self.static_route_workers[item.ordinal]
                self._event(
                    "route-bound",
                    item=item,
                    worker=worker,
                    tu_seq=tu_seq,
                    detail=(
                        "physical-route-trace"
                        if self.trace_routing
                        else "stable-rendezvous"
                    ),
                )
                if policy == "environment-round-robin":
                    self.static_ready_by_environment_worker[
                        (item.environment, worker)
                    ].append(item)
                    if item.environment not in self.ready_environment_members:
                        self.ready_environment_cycle.append(item.environment)
                        self.ready_environment_members.add(item.environment)
                else:
                    heapq.heappush(
                        self.static_ready_heaps[worker],
                        (self._ready_key(item), item.ordinal, item),
                    )
                continue
            if policy == "environment-round-robin":
                queue = self.ready_by_environment[item.environment]
                queue.append(item)
                if item.environment not in self.ready_environment_members:
                    self.ready_environment_cycle.append(item.environment)
                    self.ready_environment_members.add(item.environment)
                continue
            key = self._ready_key(item)
            heapq.heappush(self.ready_heap, (key, item.ordinal, item))

    def _has_ready(self) -> bool:
        if self.static_routing:
            if self.scheduler["ready_job_policy"] == "environment-round-robin":
                return bool(self.ready_environment_cycle)
            return any(self.static_ready_heaps.values())
        if self.scheduler["ready_job_policy"] == "environment-round-robin":
            return bool(self.ready_environment_cycle)
        return bool(self.ready_heap)

    def _has_dispatchable_ready(self) -> bool:
        if not self._has_ready():
            return False
        if not self.static_routing:
            return True
        free_workers = set(self._free_workers())
        if self.scheduler["ready_job_policy"] == "environment-round-robin":
            return any(
                self.static_ready_by_environment_worker[(environment, worker)]
                for environment in self.ready_environment_cycle
                for worker in free_workers
            )
        return any(self.static_ready_heaps[worker] for worker in free_workers)

    def _pop_ready(self) -> WorkItem:
        if self.static_routing:
            return self._pop_static_ready()
        if self.scheduler["ready_job_policy"] != "environment-round-robin":
            item = heapq.heappop(self.ready_heap)[2]
        else:
            environment = self.ready_environment_cycle.popleft()
            queue = self.ready_by_environment[environment]
            item = queue.popleft()
            if queue:
                self.ready_environment_cycle.append(environment)
            else:
                self.ready_environment_members.remove(environment)
        self.env_ready[item.environment] -= 1
        return item

    def _pop_static_ready(self) -> WorkItem:
        free_workers = set(self._free_workers())
        if not free_workers:
            raise RuntimeError("static ready selection without a free worker")
        if self.scheduler["ready_job_policy"] != "environment-round-robin":
            candidates = [
                (queue[0][0], queue[0][1], worker)
                for worker, queue in self.static_ready_heaps.items()
                if worker in free_workers and queue
            ]
            if not candidates:
                raise RuntimeError("static ready selection has no dispatchable TU")
            _, _, worker = min(candidates)
            item = heapq.heappop(self.static_ready_heaps[worker])[2]
            self.env_ready[item.environment] -= 1
            return item

        cycle_length = len(self.ready_environment_cycle)
        for _ in range(cycle_length):
            environment = self.ready_environment_cycle.popleft()
            candidates = [
                (
                    self.static_ready_by_environment_worker[(environment, worker)][
                        0
                    ].ordinal,
                    worker,
                )
                for worker in free_workers
                if self.static_ready_by_environment_worker[(environment, worker)]
            ]
            if not candidates:
                self.ready_environment_cycle.append(environment)
                continue
            _, worker = min(candidates)
            item = self.static_ready_by_environment_worker[
                (environment, worker)
            ].popleft()
            self.env_ready[environment] -= 1
            if self.env_ready[environment]:
                self.ready_environment_cycle.append(environment)
            else:
                self.ready_environment_members.remove(environment)
            return item
        raise RuntimeError("static environment selection has no dispatchable TU")

    def _free_workers(self) -> list[int]:
        pool = self.staging_slots if self.decoupled_staging else self.compiler_slots
        return [worker for worker in range(self.f_count) if pool[worker]]

    def _reserve_worker(self, worker: int) -> tuple[int, int, int | None]:
        staging_slot = self.staging_slots[worker].acquire()
        compiler_slot = (
            None if self.decoupled_staging else self.compiler_slots[worker].acquire()
        )
        assignment_slot = staging_slot if compiler_slot is None else compiler_slot
        return assignment_slot, staging_slot, compiler_slot

    def _select_slot(self, item: WorkItem) -> tuple[int, int, int, int | None]:
        free_workers = self._free_workers()
        if not free_workers:
            raise RuntimeError("slot selection without a free worker")
        policy = self.scheduler["placement_policy"]
        if policy in {"rendezvous", "trace"}:
            worker = self.static_route_workers[item.ordinal]
            if worker not in free_workers:
                raise RuntimeError("static rendezvous target has no free staging slot")
            slot, staging_slot, compiler_slot = self._reserve_worker(worker)
            return worker, slot, staging_slot, compiler_slot
        if policy == "round-robin":
            available = set(free_workers)
            for offset in range(self.f_count):
                worker = (self.round_robin_cursor + offset) % self.f_count
                if worker in available:
                    self.round_robin_cursor = (worker + 1) % self.f_count
                    slot, staging_slot, compiler_slot = self._reserve_worker(worker)
                    return worker, slot, staging_slot, compiler_slot
            raise AssertionError("round-robin failed to find a free worker")
        if policy == "fastest":
            worker = min(
                free_workers,
                key=lambda candidate: (
                    self.adapter.preview_c_to_f(item, candidate),
                    candidate,
                ),
            )
            slot, staging_slot, compiler_slot = self._reserve_worker(worker)
            return worker, slot, staging_slot, compiler_slot
        raise NotImplementedError(
            f"placement policy {policy!r} needs its stateful adapter in the common core"
        )

    def _dispatch(self) -> None:
        while self._has_dispatchable_ready() and self._free_workers():
            item = self._pop_ready()
            worker, slot, staging_slot, compiler_slot = self._select_slot(item)
            route = (item.environment, worker)
            tu_seq = self.prepared_tu_seq[item.ordinal]
            trace_entry = self.scenario.route_trace.get(item.ordinal)
            rel_seq = (
                trace_entry.rel_seq
                if trace_entry is not None
                else self.relationship_next_assigned[route]
            )
            if rel_seq != self.relationship_next_assigned[route]:
                raise RuntimeError(
                    f"route trace dispatched REL_SEQ {rel_seq} before "
                    f"{self.relationship_next_assigned[route]} on {route}"
                )
            self.relationship_next_assigned[route] += 1
            tx = Transaction(
                self.transaction_sequence,
                item,
                worker,
                slot,
                (),
                tu_seq=tu_seq,
                rel_seq=rel_seq,
                staging_slot=staging_slot,
                compiler_slot=compiler_slot,
                dispatch_ns=self.now,
            )
            tx.logical_job_id = logical_job_identity(
                self.scenario.scenario_digest, item
            )
            tx.attempt_id = f"{tx.logical_job_id}:attempt-0"
            tx.c_store_guid = (
                trace_entry.c_store_guid
                if trace_entry is not None
                else self.c_store_guids[item.environment]
            )
            tx.physical_endpoint = (
                trace_entry.physical_endpoint
                if trace_entry is not None
                else f"F{worker}"
            )
            tx.route_lane_id = (
                trace_entry.route_lane_id
                if trace_entry is not None
                else f"C{item.environment}_F{worker}"
            )
            tx.f_store_guid = (
                trace_entry.f_store_guid
                if trace_entry is not None
                else self.f_store_guids[worker]
            )
            tx.session_serial = (
                trace_entry.session_serial if trace_entry is not None else 1
            )
            tx.history_nonce = (
                trace_entry.history_nonce
                if trace_entry is not None
                else int(
                    stable_hex(
                        "history-nonce",
                        self.scenario.scenario_digest,
                        item.environment,
                        worker,
                        digits=16,
                    ),
                    16,
                )
            )
            tx.transaction_digest = transaction_identity_digest(
                self.scenario.scenario_digest,
                tx.logical_job_id,
                tx.attempt_id,
                tx.c_store_guid,
                tx.f_store_guid,
                tx.history_nonce,
                tx.tu_seq,
                tx.rel_seq,
                item.raw_digest or "unavailable",
                self.negotiated_profiles,
                self.route_state_profiles,
            )
            tx.input_record_identity = input_record_identity(
                tx.transaction_digest,
                item.raw_digest or "unavailable",
            )
            self.adapter.bind_route(item, worker, tx.tu_seq, tx.rel_seq)
            self.transaction_sequence += 1
            self.transactions.append(tx)
            self.worker_raw_bytes[worker] += item.raw_bytes
            self.env_active[item.environment] += 1
            self.worker_input_wait[worker] += 1
            self.worker_dispatched[worker] += 1
            self._event(
                "dispatch",
                tx,
                queue_byte_delta={
                    "scheduler_ready_bytes": -item.raw_bytes,
                    "route_input_pending_bytes": item.raw_bytes,
                },
            )
            self._ensure_environment(tx)
            self._queue_or_start_dialogue(tx)

    def _ensure_environment(self, tx: Transaction) -> None:
        route = (tx.item.environment, tx.worker)
        status = self.environment_status[route]
        if status == "resident":
            return
        if status in {"transferring", "installing"}:
            return
        if status != "absent":
            raise RuntimeError(f"unknown environment state {status!r} on {route}")
        ensure = EnvironmentEnsure(route[0], route[1], tx, self.now)
        self.environment_ensures[route] = ensure
        self.environment_status[route] = "transferring"
        phase = Phase(
            "environment-image",
            "c_to_f",
            self.environment_transfer_bytes,
            priority=0,
            account="environment",
        )
        self._event(
            "env_transfer",
            tx,
            phase,
            transition="start",
        )
        self._start_flow(tx, phase)

    def _finish_environment_transfer(self, flow: Flow) -> None:
        route = (flow.transaction.item.environment, flow.transaction.worker)
        ensure = self.environment_ensures.get(route)
        if ensure is None or self.environment_status[route] != "transferring":
            raise RuntimeError(
                f"environment transfer finished without owner on {route}"
            )
        ensure.transfer_done_ns = self.now
        self._event(
            "env_transfer",
            ensure.owner,
            flow.phase,
            flow=flow,
            start_ns=ensure.start_ns,
            end_ns=self.now,
            transition="finish",
        )
        ensure.install_start_ns = self.now
        self.environment_status[route] = "installing"
        self._event(
            "env_install_verify",
            ensure.owner,
            transition="start",
        )
        ready_at = self.now + self.environment_install_verify_ns
        if ready_at == self.now:
            self._finish_environment_install(ensure)
        else:
            heapq.heappush(
                self.environment_install_heap,
                (ready_at, route[0], route[1], ensure),
            )

    def _finish_environment_install(self, ensure: EnvironmentEnsure) -> None:
        route = (ensure.environment, ensure.worker)
        if self.environment_status[route] != "installing":
            raise RuntimeError(
                f"environment install finished in wrong state on {route}"
            )
        ensure.ready_ns = self.now
        self._event(
            "env_install_verify",
            ensure.owner,
            start_ns=ensure.install_start_ns,
            end_ns=self.now,
            transition="finish",
        )
        self.environment_status[route] = "resident"
        self._event(
            "environment_ready",
            ensure.owner,
        )
        waiters = self.environment_waiters[route]
        while waiters:
            tx = waiters.popleft()
            self.worker_environment_wait[tx.worker] -= 1
            self._queue_or_begin_compile(tx)

    def _finish_environment_installs(self) -> None:
        while (
            self.environment_install_heap
            and self.environment_install_heap[0][0] <= self.now
        ):
            _, _, _, ensure = heapq.heappop(self.environment_install_heap)
            self._finish_environment_install(ensure)

    def _queue_or_start_dialogue(self, tx: Transaction) -> None:
        window = self.adapter.dialogue_window_per_route()
        route = (tx.item.environment, tx.worker)
        if window is not None and self.dialogue_active[route] >= window:
            self.dialogue_queues[route].append(tx)
            self._event("dialogue-queued", tx)
            return
        if window is not None:
            self.dialogue_active[route] += 1
        self._start_dialogue(tx)

    def _start_dialogue(self, tx: Transaction) -> None:
        if tx.dialogue_started:
            raise RuntimeError("transaction dialogue started twice")
        tx.plan = self.adapter.transaction_plan(tx.item, tx.worker)
        if tx.plan is None:
            tx.phases = tuple(self.adapter.begin(tx.item, tx.worker))
        else:
            tx.dag_tokens.update(tx.plan.initial_tokens)
        tx.dialogue_started = True
        self._event("dialogue-start", tx)
        if tx.plan is None:
            self._start_next_phase(tx)
        else:
            self._release_dag_nodes(tx)
            self._check_dag_joins(tx)

    def _finish_dialogue(self, tx: Transaction) -> None:
        self._event("dialogue-finish", tx)
        window = self.adapter.dialogue_window_per_route()
        if window is None:
            return
        route = (tx.item.environment, tx.worker)
        if self.dialogue_active[route] <= 0:
            raise RuntimeError("dialogue route count underflow")
        self.dialogue_active[route] -= 1
        queue = self.dialogue_queues[route]
        if queue:
            following = queue.popleft()
            self.dialogue_active[route] += 1
            self._start_dialogue(following)

    def _start_compile(self, tx: Transaction) -> None:
        if tx.input_ready or tx.compile_start_ns is not None:
            raise RuntimeError("transaction input became ready twice")
        tx.input_ready = True
        tx.transfer_done_ns = self.now
        self.worker_input_wait[tx.worker] -= 1
        self._event(
            "input-ready",
            tx,
            resource_byte_delta={"input_record_bytes": tx.item.raw_bytes},
            queue_byte_delta={
                "route_input_pending_bytes": -tx.item.raw_bytes,
                "compiler_input_bytes": tx.item.raw_bytes,
            },
        )
        route = (tx.item.environment, tx.worker)
        if self.environment_status[route] != "resident":
            self.environment_waiters[route].append(tx)
            self.worker_environment_wait[tx.worker] += 1
            self._event("compiler-queued", tx, detail="waiting-for-environment")
            return
        self._queue_or_begin_compile(tx)

    def _queue_or_begin_compile(self, tx: Transaction) -> None:
        if tx.compiler_slot is not None:
            self._begin_compile(tx)
            return
        self.worker_ready_input[tx.worker] += 1
        self.ready_compiler_queues[tx.worker].append(tx)
        self._event("compiler-queued", tx)
        self._start_ready_compiles(tx.worker)

    def _release_staging_slot(self, tx: Transaction) -> None:
        if tx.staging_slot is None or tx.staging_released:
            raise RuntimeError("transaction input-staging slot released twice")
        self.staging_slots[tx.worker].release(tx.staging_slot)
        tx.staging_released = True

    def _begin_compile(self, tx: Transaction) -> None:
        if tx.compile_start_ns is not None or tx.compiler_slot is None:
            raise RuntimeError("transaction has no available compiler slot")
        self._release_staging_slot(tx)
        tx.compile_start_ns = self.now
        tx.compile_finish_ns = self.now + tx.item.compile_ns
        self.worker_compile_ns[tx.worker] += tx.item.compile_ns
        self.worker_compiling[tx.worker] += 1
        self._event(
            "compile-start",
            tx,
            queue_byte_delta={"compiler_input_bytes": -tx.item.raw_bytes},
        )
        heapq.heappush(self.compile_heap, (tx.compile_finish_ns, tx.sequence, tx))

    def _start_ready_compiles(self, worker: int) -> None:
        queue = self.ready_compiler_queues[worker]
        while queue and self.compiler_slots[worker]:
            tx = queue.popleft()
            self.worker_ready_input[worker] -= 1
            tx.compiler_slot = self.compiler_slots[worker].acquire()
            self._begin_compile(tx)

    def _complete_transaction(self, tx: Transaction) -> None:
        if not tx.compile_completed or not tx.transaction_committed:
            raise RuntimeError("transaction completed before compile and commit joined")
        if not tx.staging_released or not tx.compiler_slot_released:
            raise RuntimeError(
                "transaction completed while an F resource remained reserved"
            )
        if tx.complete_ns is not None:
            raise RuntimeError("transaction completed twice")
        tx.complete_ns = self.now
        self._event(
            "transaction-complete",
            tx,
            resource_byte_delta={
                "prepared_input_bytes": -tx.item.raw_bytes,
                "input_record_bytes": -tx.item.raw_bytes,
            },
        )
        self.worker_reserved_ns[tx.worker] += self.now - tx.dispatch_ns
        self.completed += 1
        self.env_active[tx.item.environment] -= 1
        self.env_completed[tx.item.environment] += 1
        self.worker_completed[tx.worker] += 1
        key = (tx.item.workload, tx.item.build)
        self.build_remaining[key] -= 1
        if self.build_remaining[key] == 0:
            config = self.scenario.workload_config[tx.item.workload]
            next_build = tx.item.build + 1
            if config["build_release"]["mode"] == "after-previous" and next_build < int(
                config["builds"]
            ):
                gap_ns = int(config["build_release"].get("gap_ns", 0))
                self._enqueue_build(
                    tx.item.workload,
                    next_build,
                    ceil_fraction(self.now) + gap_ns,
                )

    def _commit_transaction(self, tx: Transaction) -> None:
        if tx.transaction_committed:
            raise RuntimeError("transaction committed twice")
        window = self.adapter.dialogue_window_per_route()
        if window is not None:
            route = (tx.item.environment, tx.worker)
            expected = self.relationship_next_committed[route]
            if tx.rel_seq != expected:
                raise RuntimeError(
                    f"relationship {route} attempted REL_SEQ {tx.rel_seq}, expected {expected}"
                )
        self.adapter.commit(tx.item, tx.worker)
        if window is not None:
            self.relationship_next_committed[route] += 1
        tx.transaction_committed = True
        tx.transaction_commit_ns = self.now
        self._event("transaction-commit", tx)
        self._finish_dialogue(tx)
        if tx.compile_completed:
            self.worker_commit_wait[tx.worker] -= 1
            self._complete_transaction(tx)

    def _start_flow(self, tx: Transaction, phase: Phase) -> None:
        if phase.account == "source" and phase.direction == "c_to_f":
            tx.c_to_f_bytes += phase.byte_count
            self.worker_c_to_f[tx.worker] += phase.byte_count
        elif phase.account == "source":
            tx.f_to_c_bytes += phase.byte_count
            self.worker_f_to_c[tx.worker] += phase.byte_count
        elif phase.account == "environment":
            if phase.direction != "c_to_f":
                raise RuntimeError("environment transfer must be C-to-F")
            self.worker_environment_c_to_f[tx.worker] += phase.byte_count
        flow = Flow(self.flow_sequence, tx, phase, Fraction(phase.byte_count * 8))
        self.flow_sequence += 1
        queue_delta = (
            {}
            if phase.byte_count == 0
            else {f"network_{phase.direction}_outstanding_bytes": phase.byte_count}
        )
        if phase.byte_count == 0:
            self._event(
                "flow-queued", tx, phase, flow=flow, queue_byte_delta=queue_delta
            )
            self._event("flow-start", tx, phase, flow=flow)
            latency = int(self.network[phase.direction]["one_way_latency_ns"])
            self._event("flow-sent", tx, phase, flow=flow)
            if latency:
                heapq.heappush(
                    self.delivery_heap, (self.now + latency, flow.sequence, flow)
                )
            else:
                self._event("flow-finish", tx, phase, flow=flow)
                if phase.account == "environment":
                    self._finish_environment_transfer(flow)
                elif tx.plan is None:
                    tx.phase_index += 1
                    self._start_next_phase(tx)
                else:
                    self._dag_token(tx, f"{phase.name}:sent")
                    self._dag_token(tx, f"{phase.name}:delivered")
            return
        self._event(
            "flow-queued",
            tx,
            phase,
            flow=flow,
            queue_byte_delta=queue_delta,
        )
        heapq.heappush(
            self.endpoint_queues[flow.endpoint],
            (phase.priority, flow.sequence, flow),
        )

    def _release_dag_nodes(self, tx: Transaction) -> None:
        if tx.plan is None:
            raise RuntimeError("DAG release requested for a linear transaction")
        for node in tx.plan.nodes:
            if (
                node.name in tx.dag_started
                or not set(node.dependencies) <= tx.dag_tokens
            ):
                continue
            tx.dag_started.add(node.name)
            self._event(
                "dag-node-ready",
                tx,
                Phase(node.name, node.direction, node.byte_count, node.priority),
            )
            self._start_flow(
                tx,
                Phase(node.name, node.direction, node.byte_count, node.priority),
            )

    def _check_dag_joins(self, tx: Transaction) -> None:
        if tx.plan is None:
            raise RuntimeError("DAG join requested for a linear transaction")
        if not tx.input_ready and set(tx.plan.input_ready_after) <= tx.dag_tokens:
            self._start_compile(tx)
        if not tx.transaction_committed and set(tx.plan.commit_after) <= tx.dag_tokens:
            self._commit_transaction(tx)

    def _dag_token(self, tx: Transaction, token: str) -> None:
        if token in tx.dag_tokens:
            raise RuntimeError(f"transaction DAG produced token {token!r} twice")
        tx.dag_tokens.add(token)
        self._event("dag-token", tx, phase=None, detail=token)
        self._release_dag_nodes(tx)
        self._check_dag_joins(tx)

    def _start_next_phase(self, tx: Transaction) -> None:
        if tx.phase_index == len(tx.phases):
            self._commit_transaction(tx)
            self._start_compile(tx)
            return
        phase = tx.phases[tx.phase_index]
        self._start_flow(tx, phase)

    def _endpoint_lanes(self, endpoint: tuple[str, int, int]) -> int:
        return int(self.network[endpoint[0]]["lanes_per_endpoint"])

    def _pop_endpoint_flow(self, endpoint: tuple[str, int, int]) -> tuple[Flow, bool]:
        """Select by priority while bounding how often the oldest flow is overtaken."""
        queue = self.endpoint_queues[endpoint]
        if not queue:
            raise RuntimeError("endpoint selection from an empty queue")
        oldest_index = min(range(len(queue)), key=lambda index: queue[index][1])
        limit = int(self.network[endpoint[0]].get("max_priority_burst_quanta", 8))
        forced_oldest = (
            oldest_index != 0 and self.endpoint_priority_overtakes[endpoint] >= limit
        )
        if forced_oldest:
            _, _, flow = queue[oldest_index]
            queue[oldest_index] = queue[-1]
            queue.pop()
            if queue:
                heapq.heapify(queue)
            self.endpoint_priority_overtakes[endpoint] = 0
            return flow, True
        _, selected_sequence, flow = heapq.heappop(queue)
        older_waits = any(sequence < selected_sequence for _, sequence, _ in queue)
        if older_waits:
            self.endpoint_priority_overtakes[endpoint] += 1
        else:
            self.endpoint_priority_overtakes[endpoint] = 0
        return flow, False

    def _fill_endpoint_queues(self) -> None:
        active_count: dict[tuple[str, int, int], int] = defaultdict(int)
        for flow in self.active_flows.values():
            active_count[flow.endpoint] += 1
        for endpoint in sorted(self.endpoint_queues):
            queue = self.endpoint_queues[endpoint]
            while queue and active_count[endpoint] < self._endpoint_lanes(endpoint):
                flow, forced_oldest = self._pop_endpoint_flow(endpoint)
                event = "flow-start" if flow.start_ns is None else "flow-resume"
                if flow.start_ns is None:
                    flow.start_ns = self.now
                quantum_bytes = self.network[endpoint[0]].get("writer_quantum_bytes")
                flow.quantum_remaining_bits = min(
                    flow.remaining_bits,
                    Fraction(
                        flow.remaining_bits
                        if quantum_bytes is None
                        else int(quantum_bytes) * 8
                    ),
                )
                self.active_flows[flow.sequence] = flow
                active_count[endpoint] += 1
                self._event(
                    event,
                    flow.transaction,
                    flow.phase,
                    flow=flow,
                    detail="bounded-priority-turn" if forced_oldest else "",
                )

    def _flow_rates(self) -> dict[int, Fraction]:
        """Return max-min fair bits/ns under every configured network capacity."""
        if not self.active_flows:
            return {}
        capacities: dict[tuple[object, ...], Fraction] = {}
        if "shared_fabric_bps" in self.network:
            capacities[("fabric", "common")] = Fraction(
                int(self.network["shared_fabric_bps"]), NANOSECONDS
            )
        resources: dict[int, tuple[tuple[object, ...], ...]] = {}
        for sequence, flow in self.active_flows.items():
            direction, environment, worker = flow.endpoint
            route = ("route",) + flow.endpoint
            capacities.setdefault(
                route,
                Fraction(
                    int(self.network[flow.phase.direction]["bits_per_second"]),
                    NANOSECONDS,
                ),
            )
            flow_resources: list[tuple[object, ...]] = [route]
            if "shared_fabric_bps" in self.network:
                flow_resources.append(("fabric", "common"))
            link = self.network[direction]
            if "fabric_bits_per_second" in link:
                direction_fabric = ("fabric", direction)
                capacities.setdefault(
                    direction_fabric,
                    Fraction(int(link["fabric_bits_per_second"]), NANOSECONDS),
                )
                flow_resources.append(direction_fabric)
            if "per_environment_bits_per_second" in link:
                environment_resource = ("environment", direction, environment)
                capacities.setdefault(
                    environment_resource,
                    Fraction(int(link["per_environment_bits_per_second"]), NANOSECONDS),
                )
                flow_resources.append(environment_resource)
            if "per_worker_bits_per_second" in link:
                worker_resource = ("worker", direction, worker)
                capacities.setdefault(
                    worker_resource,
                    Fraction(int(link["per_worker_bits_per_second"]), NANOSECONDS),
                )
                flow_resources.append(worker_resource)
            resources[sequence] = tuple(flow_resources)
        unresolved = set(self.active_flows)
        rates = {sequence: Fraction(0) for sequence in unresolved}
        while unresolved:
            consumer_counts: dict[tuple[object, ...], int] = defaultdict(int)
            for sequence in unresolved:
                for resource in resources[sequence]:
                    consumer_counts[resource] += 1
            candidates = [
                (capacities[resource] / consumers, resource)
                for resource, consumers in consumer_counts.items()
            ]
            if not candidates:
                raise RuntimeError("active network flow has no capacity resource")
            increment = min(value for value, _ in candidates)
            if increment <= 0:
                raise RuntimeError(
                    "network capacity was exhausted before flows resolved"
                )
            for sequence in unresolved:
                rates[sequence] += increment
            for resource, consumers in consumer_counts.items():
                capacities[resource] -= increment * consumers
            saturated = {
                resource for resource, capacity in capacities.items() if capacity == 0
            }
            resolved = {
                sequence
                for sequence in unresolved
                if any(resource in saturated for resource in resources[sequence])
            }
            if not resolved:
                raise RuntimeError("max-min network allocation made no progress")
            unresolved -= resolved
        return rates

    def _network_finish_time(self) -> Fraction | None:
        rates = self._flow_rates()
        if not rates:
            return None
        return self.now + min(
            min(
                flow.remaining_bits,
                (
                    flow.quantum_remaining_bits
                    if flow.quantum_remaining_bits is not None
                    else flow.remaining_bits
                ),
            )
            / rates[sequence]
            for sequence, flow in self.active_flows.items()
        )

    def _advance_network(self, target: Fraction) -> None:
        if target < self.now:
            raise RuntimeError("simulation clock moved backwards")
        while self.now < target:
            if not self.active_flows and not self.compile_heap:
                self.timeline.flush_partial(self)
                start = self.now
                self.timeline.emit_gap(self, start, target)
                self.now = target
                return
            elapsed = min(target - self.now, self.timeline.remaining_sample_ns)
            rates = self._flow_rates()
            self.timeline.accumulate(self, elapsed, rates)
            for sequence, flow in self.active_flows.items():
                transmitted = rates[sequence] * elapsed
                flow.remaining_bits -= transmitted
                if flow.quantum_remaining_bits is None:
                    raise RuntimeError("active flow has no writer quantum")
                flow.quantum_remaining_bits -= transmitted
                if flow.remaining_bits < 0:
                    raise RuntimeError("network flow overran its completion point")
                if flow.quantum_remaining_bits < 0:
                    raise RuntimeError("network flow overran its writer quantum")
            self.now += elapsed
            self.timeline.mark_or_emit_boundary(self, external=self.now == target)

    def _finish_serializations(self) -> None:
        finished = [
            flow for flow in self.active_flows.values() if flow.remaining_bits == 0
        ]
        yielded = [
            flow
            for flow in self.active_flows.values()
            if flow.remaining_bits > 0 and flow.quantum_remaining_bits == 0
        ]
        if not finished and not yielded:
            return
        for flow in sorted((*finished, *yielded), key=lambda value: value.sequence):
            del self.active_flows[flow.sequence]
        for flow in sorted(finished, key=lambda value: value.sequence):
            byte_count = flow.phase.byte_count
            self._event(
                "flow-sent",
                flow.transaction,
                flow.phase,
                flow=flow,
                c_to_f_byte_delta=(
                    byte_count if flow.phase.direction == "c_to_f" else 0
                ),
                f_to_c_byte_delta=(
                    byte_count if flow.phase.direction == "f_to_c" else 0
                ),
                resource_byte_delta={
                    f"network_{flow.phase.direction}_propagating_bytes": byte_count
                },
                queue_byte_delta={
                    f"network_{flow.phase.direction}_outstanding_bytes": -byte_count
                },
            )
            if (
                flow.phase.account != "environment"
                and flow.transaction.plan is not None
            ):
                self._dag_token(flow.transaction, f"{flow.phase.name}:sent")
            latency = int(self.network[flow.phase.direction]["one_way_latency_ns"])
            heapq.heappush(
                self.delivery_heap,
                (self.now + latency, flow.sequence, flow),
            )
        for flow in sorted(yielded, key=lambda value: value.sequence):
            self._event("flow-yield", flow.transaction, flow.phase, flow=flow)
            heapq.heappush(
                self.endpoint_queues[flow.endpoint],
                (flow.phase.priority, flow.sequence, flow),
            )

    def _finish_deliveries(self) -> None:
        while self.delivery_heap and self.delivery_heap[0][0] <= self.now:
            _, _, flow = heapq.heappop(self.delivery_heap)
            resource_delta = (
                {}
                if flow.phase.byte_count == 0
                else {
                    f"network_{flow.phase.direction}_propagating_bytes": -flow.phase.byte_count
                }
            )
            self._event(
                "flow-finish",
                flow.transaction,
                flow.phase,
                flow=flow,
                resource_byte_delta=resource_delta,
            )
            if flow.phase.account == "environment":
                self._finish_environment_transfer(flow)
            elif flow.transaction.plan is None:
                flow.transaction.phase_index += 1
                self._start_next_phase(flow.transaction)
            else:
                self._dag_token(flow.transaction, f"{flow.phase.name}:delivered")

    def _finish_compiles(self) -> None:
        refill_workers: set[int] = set()
        while self.compile_heap and self.compile_heap[0][0] <= self.now:
            _, _, tx = heapq.heappop(self.compile_heap)
            self._event("compile-finish", tx)
            self.worker_compiling[tx.worker] -= 1
            if tx.compiler_slot is None or tx.compiler_slot_released:
                raise RuntimeError("compiler finished without an owned compiler slot")
            self.compiler_slots[tx.worker].release(tx.compiler_slot)
            tx.compiler_slot_released = True
            refill_workers.add(tx.worker)
            tx.compile_completed = True
            if tx.transaction_committed:
                self._complete_transaction(tx)
            else:
                self.worker_commit_wait[tx.worker] += 1
                self._event("compile-await-commit", tx)
        for worker in sorted(refill_workers):
            self._start_ready_compiles(worker)

    def _next_time(self) -> Fraction:
        candidates: list[Fraction] = []
        if self.release_heap:
            candidates.append(Fraction(self.release_heap[0][0]))
        if self.compile_heap:
            candidates.append(self.compile_heap[0][0])
        if self.delivery_heap:
            candidates.append(self.delivery_heap[0][0])
        if self.environment_install_heap:
            candidates.append(self.environment_install_heap[0][0])
        network_finish = self._network_finish_time()
        if network_finish is not None:
            candidates.append(network_finish)
        future = [candidate for candidate in candidates if candidate >= self.now]
        if not future:
            raise RuntimeError(
                f"simulation stalled after {self.completed}/{self.total_items} jobs"
            )
        return min(future)

    def routing_metadata(self) -> dict[str, object]:
        if self._routing_metadata_cache is not None:
            return self._routing_metadata_cache
        metadata: dict[str, object] = {
            "placement_policy": self.scheduler["placement_policy"],
            "tu_seq_allocation": (
                "physical assignment trace"
                if self.trace_routing
                else "prepared-input release order per C"
            ),
            "rel_seq_allocation": (
                "physical assignment trace"
                if self.trace_routing
                else "route-local dispatch order"
            ),
            "binding": (
                "release-time-static" if self.static_routing else "dispatch-time"
            ),
        }
        if self.static_routing:
            assignment_counts: dict[tuple[int, str, int], int] = defaultdict(int)
            for items in self.scenario.work_items.values():
                for item in items:
                    assignment_counts[
                        (
                            item.environment,
                            item.workload,
                            self.static_route_workers[item.ordinal],
                        )
                    ] += 1
            static_metadata: dict[str, object] = {
                "algorithm": (
                    "physical-route-trace-v1"
                    if self.trace_routing
                    else "stable-rendezvous-v1"
                ),
                "assignment_source": (
                    "route_trace" if self.trace_routing else "policy"
                ),
                "assignment_counts": [
                    {
                        "environment": environment,
                        "workload": workload,
                        "worker": worker,
                        "tus": count,
                    }
                    for (environment, workload, worker), count in sorted(
                        assignment_counts.items()
                    )
                ],
            }
            if not self.trace_routing:
                static_metadata.update(
                    {
                        "algorithm": "stable-rendezvous-v1",
                        "seed": int(self.scenario.document["seed"]),
                        "stable_tu_identity": "(environment, workload, source_job_id)",
                        "build_number_in_identity": False,
                        "dense_frontier_workers": int(
                            self.scheduler.get("dense_frontier_workers", self.f_count)
                        ),
                        "home_sets": [
                            {
                                "environment": environment,
                                "workload": workload,
                                "workers": list(workers),
                            }
                            for (environment, workload), workers in sorted(
                                self.static_home_sets.items()
                            )
                        ],
                    }
                )
            else:
                static_metadata.update(
                    {
                        "route_trace": self.scenario.route_trace_identity,
                        "route_trace_sha256": self.scenario.route_trace_sha256,
                        "route_trace_codec": self.scenario.route_trace_codec,
                        "route_trace_provenance": self.scenario.route_trace_provenance,
                    }
                )
            metadata.update(static_metadata)
        self._routing_metadata_cache = metadata
        return metadata

    def timeline_state(self) -> dict[str, object]:
        active_by_route: dict[tuple[str, int, int], int] = defaultdict(int)
        queued_by_route: dict[tuple[str, int, int], int] = defaultdict(int)
        delivery_by_route: dict[tuple[str, int, int], int] = defaultdict(int)
        for flow in self.active_flows.values():
            active_by_route[flow.endpoint] += 1
        for endpoint, queue in self.endpoint_queues.items():
            queued_by_route[endpoint] += len(queue)
        for _, _, flow in self.delivery_heap:
            delivery_by_route[flow.endpoint] += 1

        c_state = []
        for environment in range(self.env_count):
            targets = []
            for worker in range(self.f_count):
                route = (environment, worker)
                active_dialogues = self.dialogue_active[route]
                queued_dialogues = len(self.dialogue_queues[route])
                forward_active = active_by_route[("c_to_f", environment, worker)]
                forward_queued = queued_by_route[("c_to_f", environment, worker)]
                forward_delivery = delivery_by_route[("c_to_f", environment, worker)]
                reverse_active = active_by_route[("f_to_c", environment, worker)]
                reverse_queued = queued_by_route[("f_to_c", environment, worker)]
                reverse_delivery = delivery_by_route[("f_to_c", environment, worker)]
                if any(
                    (
                        forward_active,
                        forward_queued,
                        forward_delivery,
                        reverse_active,
                        reverse_queued,
                        reverse_delivery,
                        active_dialogues,
                        queued_dialogues,
                    )
                ):
                    targets.append(
                        {
                            "worker": worker,
                            "next_rel_seq": self.relationship_next_assigned[route],
                            "committed_rel_seq": self.relationship_next_committed[
                                route
                            ],
                            "active_dialogues": active_dialogues,
                            "queued_dialogues": queued_dialogues,
                            "c_to_f_active_flows": forward_active,
                            "c_to_f_queued_flows": forward_queued,
                            "c_to_f_in_propagation": forward_delivery,
                            "f_to_c_active_flows": reverse_active,
                            "f_to_c_queued_flows": reverse_queued,
                            "f_to_c_in_propagation": reverse_delivery,
                        }
                    )
            c_state.append(
                {
                    "environment": environment,
                    "unreleased_tus": self.env_unreleased[environment],
                    "ready_tus": self.env_ready[environment],
                    "active_tus": self.env_active[environment],
                    "completed_tus": self.env_completed[environment],
                    "admitted_tus": (
                        self.env_active[environment] + self.env_completed[environment]
                    ),
                    "targets": targets,
                    "codec": self.adapter.timeline_c_state(environment),
                }
            )

        f_state = []
        for worker in range(self.f_count):
            namespaces = []
            for environment in range(self.env_count):
                codec_state = self.adapter.timeline_f_state(environment, worker)
                if codec_state:
                    namespaces.append(
                        {"environment": environment, "codec": codec_state}
                    )
            f_state.append(
                {
                    "worker": worker,
                    "slots": self.slots_per_f,
                    "free_slots": self.slots_per_f
                    - len(self.compiler_slots[worker].in_use),
                    "reserved_slots": len(self.compiler_slots[worker].in_use),
                    "input_staging_slots": self.input_staging_slots_per_f,
                    "free_input_staging_slots": self.input_staging_slots_per_f
                    - len(self.staging_slots[worker].in_use),
                    "occupied_input_staging_slots": len(
                        self.staging_slots[worker].in_use
                    ),
                    "input_wait_slots": self.worker_input_wait[worker],
                    "ready_input_slots": self.worker_ready_input[worker],
                    "compiling_slots": self.worker_compiling[worker],
                    "commit_wait_slots": self.worker_commit_wait[worker],
                    "environment_wait_slots": self.worker_environment_wait[worker],
                    "environment_states": [
                        {
                            "environment": environment,
                            "state": self.environment_status[(environment, worker)],
                        }
                        for environment in range(self.env_count)
                    ],
                    "dispatched_tus": self.worker_dispatched[worker],
                    "completed_tus": self.worker_completed[worker],
                    "active_flows": sum(
                        value
                        for (
                            direction,
                            environment,
                            route_worker,
                        ), value in active_by_route.items()
                        if route_worker == worker
                    ),
                    "queued_flows": sum(
                        value
                        for (
                            direction,
                            environment,
                            route_worker,
                        ), value in queued_by_route.items()
                        if route_worker == worker
                    ),
                    "in_propagation": sum(
                        value
                        for (
                            direction,
                            environment,
                            route_worker,
                        ), value in delivery_by_route.items()
                        if route_worker == worker
                    ),
                    "active_dialogues": sum(
                        self.dialogue_active[(environment, worker)]
                        for environment in range(self.env_count)
                    ),
                    "queued_dialogues": sum(
                        len(self.dialogue_queues[(environment, worker)])
                        for environment in range(self.env_count)
                    ),
                    "codec_namespaces": namespaces,
                }
            )

        routing_state = self.routing_metadata()
        return {
            "scheduler": {
                "unreleased_tus": sum(self.env_unreleased),
                "ready_tus": sum(self.env_ready),
                "active_tus": sum(self.env_active),
                "completed_tus": self.completed,
                "total_tus": self.total_items,
            },
            "routing": routing_state,
            "c": c_state,
            "f": f_state,
            "network": {
                "active_flows": len(self.active_flows),
                "queued_flows": sum(
                    len(queue) for queue in self.endpoint_queues.values()
                ),
                "in_propagation": len(self.delivery_heap),
                "active_dialogues": sum(self.dialogue_active.values()),
                "queued_dialogues": sum(
                    len(queue) for queue in self.dialogue_queues.values()
                ),
            },
        }

    def _assert_terminal_state(self) -> None:
        """Require every scheduler, route, and F-slot counter to close exactly."""
        failures = {
            "release_heap": len(self.release_heap),
            "ready_heap": len(self.ready_heap),
            "static_ready_heaps": sum(
                len(queue) for queue in self.static_ready_heaps.values()
            ),
            "static_ready_environment_queues": sum(
                len(queue) for queue in self.static_ready_by_environment_worker.values()
            ),
            "ready_environment_cycle": len(self.ready_environment_cycle),
            "ready_compiler_queues": sum(
                len(queue) for queue in self.ready_compiler_queues.values()
            ),
            "active_flows": len(self.active_flows),
            "queued_flows": sum(len(queue) for queue in self.endpoint_queues.values()),
            "deliveries": len(self.delivery_heap),
            "environment_installs": len(self.environment_install_heap),
            "environment_waiters": sum(
                len(queue) for queue in self.environment_waiters.values()
            ),
            "compiles": len(self.compile_heap),
            "active_dialogues": sum(self.dialogue_active.values()),
            "queued_dialogues": sum(
                len(queue) for queue in self.dialogue_queues.values()
            ),
            "environment_unreleased": sum(self.env_unreleased),
            "environment_ready": sum(self.env_ready),
            "environment_active": sum(self.env_active),
            "worker_input_wait": sum(self.worker_input_wait),
            "worker_ready_input": sum(self.worker_ready_input),
            "worker_compiling": sum(self.worker_compiling),
            "worker_commit_wait": sum(self.worker_commit_wait),
            "worker_environment_wait": sum(self.worker_environment_wait),
            "reserved_compiler_slots": sum(
                len(pool.in_use) for pool in self.compiler_slots.values()
            ),
            "reserved_staging_slots": sum(
                len(pool.in_use) for pool in self.staging_slots.values()
            ),
        }
        nonzero = {name: value for name, value in failures.items() if value}
        if nonzero:
            raise RuntimeError(f"completed simulation retained active state: {nonzero}")
        if (
            self.completed != self.total_items
            or sum(self.env_completed) != self.total_items
        ):
            raise RuntimeError(
                "completed simulation counters differ from the scenario TU count"
            )
        self.ledger.assert_closed()

    def _assert_relationship_order(self) -> None:
        """Require complete C identities and contiguous route-local commit order."""
        by_environment: dict[int, list[Transaction]] = defaultdict(list)
        by_relationship: dict[tuple[int, int], list[Transaction]] = defaultdict(list)
        for tx in sorted(self.transactions, key=lambda value: value.sequence):
            by_environment[tx.item.environment].append(tx)
            by_relationship[(tx.item.environment, tx.worker)].append(tx)
        for environment, transactions in by_environment.items():
            observed = [tx.tu_seq for tx in transactions]
            if sorted(observed) != list(range(len(transactions))):
                raise RuntimeError(
                    f"C{environment} TU_SEQ identity set is not contiguous: {observed[:3]}"
                )
        for route, transactions in by_relationship.items():
            rel_seq = [tx.rel_seq for tx in transactions]
            if rel_seq != list(range(len(transactions))):
                raise RuntimeError(
                    f"relationship {route} REL_SEQ order is not contiguous"
                )
            if self.relationship_next_assigned[route] != len(transactions):
                raise RuntimeError(
                    f"relationship {route} assignment cursor differs from its route"
                )
            if (
                self.adapter.dialogue_window_per_route() is not None
                and self.relationship_next_committed[route] != len(transactions)
            ):
                raise RuntimeError(
                    f"relationship {route} commit cursor differs from its route"
                )

    def _assert_event_contract(self) -> None:
        expected_sequence = 0
        c_to_f = 0
        f_to_c = 0
        resources: dict[str, int] = defaultdict(int)
        queues: dict[str, int] = defaultdict(int)
        for event in self.events:
            if event.get("sequence") != expected_sequence:
                raise RuntimeError("event sequence is not contiguous")
            expected_sequence += 1
            missing = [field for field in EVENT_IDENTITY_FIELDS if field not in event]
            if missing:
                raise RuntimeError(f"event lacks M3 fields {missing}")
            c_to_f += int(event["c_to_f_byte_delta"])
            f_to_c += int(event["f_to_c_byte_delta"])
            for name, delta in event["resource_byte_delta"].items():
                resources[name] += int(delta)
            for name, delta in event["queue_byte_delta"].items():
                queues[name] += int(delta)
        ledger_summary = self.ledger.summary()
        if c_to_f != ledger_summary["directions"]["c_to_f"]["total_bytes"]:
            raise RuntimeError("event C-to-F bytes differ from exact ledger")
        if f_to_c != ledger_summary["directions"]["f_to_c"]["total_bytes"]:
            raise RuntimeError("event F-to-C bytes differ from exact ledger")
        if any(resources.values()) or any(queues.values()):
            raise RuntimeError(
                f"event byte deltas do not close: resources={dict(resources)}, queues={dict(queues)}"
            )

    def _replay_closure(self) -> dict[str, object]:
        assignment_source = str(self.v2_runtime.get("assignment_source", "policy"))
        ledger_summary = self.ledger.summary()
        if assignment_source == "route_trace":
            expected_routes: dict[tuple[int, int, str], list[int]] = defaultdict(
                lambda: [0, 0, 0]
            )
            observed_routes: dict[tuple[int, int, str], list[int]] = defaultdict(
                lambda: [0, 0, 0]
            )
            for tx in self.transactions:
                entry = self.scenario.route_trace[tx.item.ordinal]
                if (
                    tx.logical_job_id != entry.logical_job_id
                    or tx.attempt_id != entry.attempt_id
                    or tx.worker != entry.worker
                    or tx.tu_seq != entry.tu_seq
                    or tx.rel_seq != entry.rel_seq
                    or tx.c_store_guid != entry.c_store_guid
                    or tx.physical_endpoint != entry.physical_endpoint
                    or tx.route_lane_id != entry.route_lane_id
                    or tx.f_store_guid != entry.f_store_guid
                    or tx.session_serial != entry.session_serial
                    or tx.history_nonce != entry.history_nonce
                    or tx.c_to_f_bytes != entry.c_to_f_bytes
                    or tx.f_to_c_bytes != entry.f_to_c_bytes
                ):
                    raise RuntimeError(f"route-trace closure differs for {tx.item.key}")
                expected = expected_routes[
                    (tx.item.environment, entry.worker, entry.route_lane_id)
                ]
                expected[0] += 1
                expected[1] += entry.c_to_f_bytes
                expected[2] += entry.f_to_c_bytes
                observed = observed_routes[
                    (tx.item.environment, tx.worker, tx.route_lane_id)
                ]
                observed[0] += 1
                observed[1] += tx.c_to_f_bytes
                observed[2] += tx.f_to_c_bytes
            if expected_routes != observed_routes:
                raise RuntimeError("route-trace aggregate routes differ")
            ledger_routes: dict[tuple[int, int], dict[str, dict[str, int]]] = (
                defaultdict(
                    lambda: {direction: defaultdict(int) for direction in DIRECTIONS}
                )
            )
            for row in ledger_summary["routes"]:
                ledger_routes[(row["environment"], row["worker"])][row["direction"]][
                    row["account"]
                ] += row["bytes"]
            route_rows = []
            for key, values in sorted(observed_routes.items()):
                environment, worker, route_lane_id = key
                entry = next(
                    trace
                    for tx in self.transactions
                    if tx.item.environment == environment and tx.worker == worker
                    for trace in (self.scenario.route_trace[tx.item.ordinal],)
                    if trace.route_lane_id == route_lane_id
                )
                accounts = {
                    account: {
                        "c_to_f_bytes": ledger_routes[(environment, worker)][
                            "c_to_f"
                        ].get(account, 0),
                        "f_to_c_bytes": ledger_routes[(environment, worker)][
                            "f_to_c"
                        ].get(account, 0),
                    }
                    for account in ("source", "environment", "result")
                    if ledger_routes[(environment, worker)]["c_to_f"].get(account, 0)
                    or ledger_routes[(environment, worker)]["f_to_c"].get(account, 0)
                }
                if (
                    accounts.get("source", {}).get("c_to_f_bytes", 0) != values[1]
                    or accounts.get("source", {}).get("f_to_c_bytes", 0) != values[2]
                ):
                    raise RuntimeError("route source totals differ from exact ledger")
                route_rows.append(
                    {
                        "environment": environment,
                        "worker": worker,
                        "C_STORE_GUID": entry.c_store_guid,
                        "physical_endpoint": entry.physical_endpoint,
                        "RouteLaneId": route_lane_id,
                        "F_STORE_GUID": entry.f_store_guid,
                        "session_serial": entry.session_serial,
                        "HISTORY_NONCE": entry.history_nonce,
                        "tus": values[0],
                        "accounts": accounts,
                        "c_to_f_bytes": sum(
                            account["c_to_f_bytes"] for account in accounts.values()
                        ),
                        "f_to_c_bytes": sum(
                            account["f_to_c_bytes"] for account in accounts.values()
                        ),
                    }
                )
            return {
                "status": "pass",
                "semantics": "exact-route",
                "assignment_source": "route_trace",
                "route_trace": self.scenario.route_trace_identity,
                "route_trace_sha256": self.scenario.route_trace_sha256,
                "routes": route_rows,
            }
        expected_c_to_f = sum(tx.c_to_f_bytes for tx in self.transactions)
        expected_f_to_c = sum(tx.f_to_c_bytes for tx in self.transactions)
        if isinstance(self.adapter, PhysicalLedgerAdapter):
            physical_totals = self.adapter.final["totals"]
            if (
                expected_c_to_f != physical_totals["c_to_f_bytes"]
                or expected_f_to_c != physical_totals["f_to_c_bytes"]
            ):
                raise RuntimeError(
                    "policy replay aggregate bytes differ from physical ledger"
                )
        return {
            "status": "pass",
            "semantics": "aggregate-directional",
            "assignment_source": "policy",
            "source_c_to_f_bytes": expected_c_to_f,
            "source_f_to_c_bytes": expected_f_to_c,
            "c_to_f_bytes": ledger_summary["directions"]["c_to_f"]["total_bytes"],
            "f_to_c_bytes": ledger_summary["directions"]["f_to_c"]["total_bytes"],
        }

    def _capacity_floors(self, transactions: Sequence[Transaction]) -> dict[str, int]:
        """Return optimistic C-to-F and compiler capacity lower bounds.

        These bounds intentionally omit release timing, propagation, codec CPU, dependency
        round trips, and queue order.  They answer how close a run came to the best result its
        configured byte and compiler capacities could possibly permit.
        """

        def byte_floor(byte_count: int, bits_per_second: int) -> int:
            if byte_count == 0:
                return 0
            return ceil_fraction(
                Fraction(byte_count * 8 * NANOSECONDS, bits_per_second)
            )

        c_to_f = self.network["c_to_f"]
        network_candidates = [
            byte_floor(
                sum(tx.c_to_f_bytes for tx in transactions),
                int(c_to_f.get("fabric_bits_per_second", 2**63 - 1)),
            )
        ]
        if "shared_fabric_bps" in self.network:
            network_candidates.append(
                byte_floor(
                    sum(tx.c_to_f_bytes for tx in transactions),
                    int(self.network["shared_fabric_bps"]),
                )
            )
        by_route: dict[tuple[int, int], int] = defaultdict(int)
        by_environment: dict[int, int] = defaultdict(int)
        by_worker: dict[int, int] = defaultdict(int)
        for tx in transactions:
            by_route[(tx.item.environment, tx.worker)] += tx.c_to_f_bytes
            by_environment[tx.item.environment] += tx.c_to_f_bytes
            by_worker[tx.worker] += tx.c_to_f_bytes
        network_candidates.extend(
            byte_floor(byte_count, int(c_to_f["bits_per_second"]))
            for byte_count in by_route.values()
        )
        if "per_environment_bits_per_second" in c_to_f:
            network_candidates.extend(
                byte_floor(byte_count, int(c_to_f["per_environment_bits_per_second"]))
                for byte_count in by_environment.values()
            )
        if "per_worker_bits_per_second" in c_to_f:
            network_candidates.extend(
                byte_floor(byte_count, int(c_to_f["per_worker_bits_per_second"]))
                for byte_count in by_worker.values()
            )
        c_to_f_floor_ns = max(network_candidates, default=0)
        compiler_work_ns = sum(tx.item.compile_ns for tx in transactions)
        longest_compile_ns = max(tx.item.compile_ns for tx in transactions)
        compiler_floor_ns = max(
            longest_compile_ns,
            ceil_fraction(
                Fraction(
                    compiler_work_ns,
                    self.f_count * self.slots_per_f,
                )
            ),
        )
        return {
            "capacity_c_to_f_floor_ns": c_to_f_floor_ns,
            "capacity_compiler_floor_ns": compiler_floor_ns,
            "capacity_overlap_floor_ns": max(c_to_f_floor_ns, compiler_floor_ns),
            "longest_compile_ns": longest_compile_ns,
        }

    def run(self) -> SimulationResult:
        while self.completed < self.total_items:
            self._release_ready()
            self._finish_serializations()
            self._finish_deliveries()
            self._finish_environment_installs()
            self._finish_compiles()
            self._release_ready()
            self._dispatch()
            self._fill_endpoint_queues()
            self.timeline.flush_due(self)
            if self.completed == self.total_items:
                break
            target = self._next_time()
            if target == self.now:
                raise RuntimeError("simulation produced a zero-time event loop")
            self._advance_network(target)

        self.timeline.flush_partial(self)
        self.timeline.finish()
        self.events.finish()
        self._assert_terminal_state()
        self._assert_relationship_order()
        self._assert_event_contract()

        makespan_ns = ceil_fraction(self.now)
        assignments = []
        for tx in self.transactions:
            if (
                tx.transfer_done_ns is None
                or tx.compile_start_ns is None
                or tx.compile_finish_ns is None
                or tx.transaction_commit_ns is None
                or tx.complete_ns is None
            ):
                raise RuntimeError("completed simulation has an unfinished transaction")
            assignments.append(
                {
                    "dispatch_order": tx.sequence,
                    "logical_job_id": tx.logical_job_id,
                    "attempt_id": tx.attempt_id,
                    "C_STORE_GUID": tx.c_store_guid,
                    "physical_endpoint": tx.physical_endpoint,
                    "RouteLaneId": tx.route_lane_id,
                    "F_STORE_GUID": tx.f_store_guid,
                    "session_serial": tx.session_serial,
                    "HISTORY_NONCE": tx.history_nonce,
                    "transaction_digest": tx.transaction_digest,
                    "raw_digest": tx.item.raw_digest,
                    "negotiated_profiles": ",".join(self.negotiated_profiles),
                    "route_state_profiles": ",".join(self.route_state_profiles),
                    "InputRecord_identity": tx.input_record_identity,
                    "tu_seq": tx.tu_seq,
                    "rel_seq": tx.rel_seq,
                    "environment": tx.item.environment,
                    "workload": tx.item.workload,
                    "build": tx.item.build,
                    "logical": tx.item.logical,
                    "worker": tx.worker,
                    "slot": tx.slot,
                    "staging_slot": tx.staging_slot,
                    "compiler_slot": tx.compiler_slot,
                    "release_ns": tx.item.release_ns,
                    "dispatch_ns": ceil_fraction(tx.dispatch_ns),
                    "transfer_done_ns": ceil_fraction(tx.transfer_done_ns),
                    "compile_start_ns": ceil_fraction(tx.compile_start_ns),
                    "compile_finish_ns": ceil_fraction(tx.compile_finish_ns),
                    "transaction_commit_ns": ceil_fraction(tx.transaction_commit_ns),
                    "complete_ns": ceil_fraction(tx.complete_ns),
                    "raw_bytes": tx.item.raw_bytes,
                    "compile_ns": tx.item.compile_ns,
                    "c_to_f_bytes": tx.c_to_f_bytes,
                    "f_to_c_bytes": tx.f_to_c_bytes,
                }
            )
        workers = []
        for worker in range(self.f_count):
            workers.append(
                {
                    "worker": worker,
                    "jobs": sum(tx.worker == worker for tx in self.transactions),
                    "raw_bytes": self.worker_raw_bytes[worker],
                    "c_to_f_bytes": self.worker_c_to_f[worker],
                    "f_to_c_bytes": self.worker_f_to_c[worker],
                    "environment_c_to_f_bytes": self.worker_environment_c_to_f[worker],
                    "compile_busy_ns": self.worker_compile_ns[worker],
                    "slot_reserved_ns": ceil_fraction(self.worker_reserved_ns[worker]),
                    "compile_utilization": (
                        self.worker_compile_ns[worker]
                        / (makespan_ns * self.slots_per_f)
                        if makespan_ns
                        else 0.0
                    ),
                }
            )
        transactions_by_build: dict[tuple[str, int], list[Transaction]] = defaultdict(
            list
        )
        for tx in self.transactions:
            transactions_by_build[(tx.item.workload, tx.item.build)].append(tx)
        builds = []
        previous_finish: dict[str, int] = {}
        for workload, build in sorted(
            transactions_by_build,
            key=lambda key: (
                transactions_by_build[key][0].item.environment,
                key[0],
                key[1],
            ),
        ):
            transactions = transactions_by_build[(workload, build)]
            release_ns = min(int(tx.item.release_ns) for tx in transactions)
            first_dispatch_ns = min(
                ceil_fraction(tx.dispatch_ns) for tx in transactions
            )
            last_transfer_done_ns = max(
                ceil_fraction(tx.transfer_done_ns)  # type: ignore[arg-type]
                for tx in transactions
            )
            last_compile_finish_ns = max(
                ceil_fraction(tx.compile_finish_ns)  # type: ignore[arg-type]
                for tx in transactions
            )
            last_transaction_commit_ns = max(
                ceil_fraction(tx.transaction_commit_ns)  # type: ignore[arg-type]
                for tx in transactions
            )
            finish_ns = max(
                ceil_fraction(tx.complete_ns)  # type: ignore[arg-type]
                for tx in transactions
            )
            elapsed_ns = finish_ns - release_ns
            capacity = self._capacity_floors(transactions)
            capacity_floor_ns = capacity["capacity_overlap_floor_ns"]
            prior_finish_ns = previous_finish.get(workload)
            builds.append(
                {
                    "environment": transactions[0].item.environment,
                    "workload": workload,
                    "build": build,
                    "temperature": "cold" if build == 0 else "warm",
                    "jobs": len(transactions),
                    "release_ns": release_ns,
                    "first_dispatch_ns": first_dispatch_ns,
                    "last_transfer_done_ns": last_transfer_done_ns,
                    "last_compile_finish_ns": last_compile_finish_ns,
                    "last_transaction_commit_ns": last_transaction_commit_ns,
                    "finish_ns": finish_ns,
                    "input_ready_elapsed_ns": last_transfer_done_ns - release_ns,
                    "compile_elapsed_ns": last_compile_finish_ns - release_ns,
                    "elapsed_from_release_ns": elapsed_ns,
                    "gap_from_previous_finish_ns": (
                        "" if prior_finish_ns is None else release_ns - prior_finish_ns
                    ),
                    "workers_used": len({tx.worker for tx in transactions}),
                    "raw_bytes": sum(tx.item.raw_bytes for tx in transactions),
                    "c_to_f_bytes": sum(tx.c_to_f_bytes for tx in transactions),
                    "f_to_c_bytes": sum(tx.f_to_c_bytes for tx in transactions),
                    "compiler_work_ns": sum(tx.item.compile_ns for tx in transactions),
                    **capacity,
                    "duration_over_capacity_floor": (
                        elapsed_ns / capacity_floor_ns if capacity_floor_ns else ""
                    ),
                    "capacity_floor_efficiency": (
                        capacity_floor_ns / elapsed_ns if elapsed_ns else ""
                    ),
                    "compiler_slot_utilization": (
                        sum(tx.item.compile_ns for tx in transactions)
                        / (elapsed_ns * self.f_count * self.slots_per_f)
                        if elapsed_ns
                        else ""
                    ),
                }
            )
            previous_finish[workload] = finish_ns
        transactions_by_generation: dict[int, list[Transaction]] = defaultdict(list)
        for tx in self.transactions:
            transactions_by_generation[tx.item.build].append(tx)
        generations = []
        for generation in sorted(transactions_by_generation):
            transactions = transactions_by_generation[generation]
            start_ns = min(int(tx.item.release_ns) for tx in transactions)
            first_dispatch_ns = min(
                ceil_fraction(tx.dispatch_ns) for tx in transactions
            )
            last_input_ready_ns = max(
                ceil_fraction(tx.transfer_done_ns)  # type: ignore[arg-type]
                for tx in transactions
            )
            last_compile_finish_ns = max(
                ceil_fraction(tx.compile_finish_ns)  # type: ignore[arg-type]
                for tx in transactions
            )
            last_transaction_commit_ns = max(
                ceil_fraction(tx.transaction_commit_ns)  # type: ignore[arg-type]
                for tx in transactions
            )
            stop_ns = max(
                ceil_fraction(tx.complete_ns)  # type: ignore[arg-type]
                for tx in transactions
            )
            duration_ns = stop_ns - start_ns
            capacity = self._capacity_floors(transactions)
            capacity_floor_ns = capacity["capacity_overlap_floor_ns"]
            generations.append(
                {
                    "generation": generation,
                    "temperature": "cold" if generation == 0 else "warm",
                    "environments": len({tx.item.environment for tx in transactions}),
                    "workloads": len({tx.item.workload for tx in transactions}),
                    "jobs": len(transactions),
                    "start_ns": start_ns,
                    "first_dispatch_ns": first_dispatch_ns,
                    "last_input_ready_ns": last_input_ready_ns,
                    "last_compile_finish_ns": last_compile_finish_ns,
                    "last_transaction_commit_ns": last_transaction_commit_ns,
                    "stop_ns": stop_ns,
                    "input_ready_elapsed_ns": last_input_ready_ns - start_ns,
                    "compile_elapsed_ns": last_compile_finish_ns - start_ns,
                    "transaction_commit_elapsed_ns": (
                        last_transaction_commit_ns - start_ns
                    ),
                    "duration_ns": duration_ns,
                    "raw_bytes": sum(tx.item.raw_bytes for tx in transactions),
                    "c_to_f_bytes": sum(tx.c_to_f_bytes for tx in transactions),
                    "f_to_c_bytes": sum(tx.f_to_c_bytes for tx in transactions),
                    "compiler_work_ns": sum(tx.item.compile_ns for tx in transactions),
                    **capacity,
                    "duration_over_capacity_floor": (
                        duration_ns / capacity_floor_ns if capacity_floor_ns else ""
                    ),
                    "capacity_floor_efficiency": (
                        capacity_floor_ns / duration_ns if duration_ns else ""
                    ),
                    "compiler_slot_utilization": (
                        sum(tx.item.compile_ns for tx in transactions)
                        / (duration_ns * self.f_count * self.slots_per_f)
                        if duration_ns
                        else ""
                    ),
                }
            )
        summed_generation_ns = sum(row["duration_ns"] for row in generations)
        summed_capacity_floor_ns = sum(
            row["capacity_overlap_floor_ns"] for row in generations
        )
        c_to_f = sum(tx.c_to_f_bytes for tx in self.transactions)
        f_to_c = sum(tx.f_to_c_bytes for tx in self.transactions)
        environment_c_to_f = sum(self.worker_environment_c_to_f)
        ledger_summary = self.ledger.summary()
        replay_closure = self._replay_closure()
        if (
            ledger_summary["directions"]["c_to_f"]["accounts"].get("source", 0)
            != c_to_f
        ):
            raise RuntimeError("source C-to-F score differs from exact ledger")
        if (
            ledger_summary["directions"]["f_to_c"]["accounts"].get("source", 0)
            != f_to_c
        ):
            raise RuntimeError("source F-to-C total differs from exact ledger")
        summary = {
            "schema": (
                "icecream-experiment-result-v2"
                if self.scenario.is_v2
                else "icecream-distribution-result-v1"
            ),
            "scenario": self.scenario.document["name"],
            "scenario_path": (
                self.scenario.path.name
                if self.scenario.is_v2
                else str(self.scenario.path)
            ),
            "scenario_sha256": self.scenario.scenario_digest,
            "scenario_digest": self.scenario.scenario_digest,
            "workload_inputs": self.scenario.workload_inputs,
            "codec_adapter": self.adapter.name,
            "physical_codec_result": self.adapter.physical,
            "codec_metadata": self.adapter.result_metadata(),
            "dialogue_window_per_route": self.adapter.dialogue_window_per_route(),
            "relationship_ordering": (
                "TU_SEQ is allocated in prepared-input release order per C; REL_SEQ is "
                "allocated independently in route-local dispatch order"
            ),
            "routing": self.routing_metadata(),
            "placement_policy": self.scheduler["placement_policy"],
            "ready_job_policy": self.scheduler["ready_job_policy"],
            "jobs": self.total_items,
            "environments": int(self.scenario.document["environments"]["env_count"]),
            "build_epochs": len(builds),
            "cold_builds": sum(row["temperature"] == "cold" for row in builds),
            "warm_builds": sum(row["temperature"] == "warm" for row in builds),
            "workers": self.f_count,
            "slots_per_worker": self.slots_per_f,
            "input_staging_slots_per_worker": self.input_staging_slots_per_f,
            "compiler_slot_assignment": (
                "at input readiness"
                if self.decoupled_staging
                else "reserved with scheduler assignment"
            ),
            "total_worker_slots": self.f_count * self.slots_per_f,
            "raw_bytes": sum(
                item.raw_bytes
                for items in self.scenario.work_items.values()
                for item in items
            ),
            "c_to_f_bytes": c_to_f,
            "f_to_c_bytes": f_to_c,
            "environment_c_to_f_bytes": environment_c_to_f,
            "network_c_to_f_bytes": c_to_f + environment_c_to_f,
            "network_f_to_c_bytes": f_to_c,
            "scored_outgoing_bytes": c_to_f,
            "source_transfer_score_excludes_environment": True,
            "tu_availability": (
                "all-at-zero-at-each-build-release"
                if self.scenario.is_v2
                else "scenario-defined"
            ),
            "preprocessing_in_score": False,
            "environment": {
                "initial_state": self.environment_initial_state,
                "transfer_bytes_per_used_route": self.environment_transfer_bytes,
                "install_verify_ns": self.environment_install_verify_ns,
                "ensured_routes": len(self.environment_ensures),
                "final_states": [
                    {
                        "environment": environment,
                        "worker": worker,
                        "state": state,
                    }
                    for (environment, worker), state in sorted(
                        self.environment_status.items()
                    )
                ],
            },
            "exact_byte_ledger": ledger_summary,
            "replay_closure": replay_closure,
            "makespan_ns": makespan_ns,
            "makespan_seconds": makespan_ns / NANOSECONDS,
            "summed_generation_ns": summed_generation_ns,
            "summed_generation_seconds": summed_generation_ns / NANOSECONDS,
            "summed_capacity_floor_ns": summed_capacity_floor_ns,
            "summed_capacity_floor_seconds": summed_capacity_floor_ns / NANOSECONDS,
            "summed_generation_over_capacity_floor": (
                summed_generation_ns / summed_capacity_floor_ns
                if summed_capacity_floor_ns
                else ""
            ),
            "capacity_floor_efficiency": (
                summed_capacity_floor_ns / summed_generation_ns
                if summed_generation_ns
                else ""
            ),
            "wall_minus_summed_generation_ns": makespan_ns - summed_generation_ns,
            "compiler_work_ns": sum(
                item.compile_ns
                for items in self.scenario.work_items.values()
                for item in items
            ),
            "timeline_snapshot_interval_ns": self.timeline.interval_ns,
            "timeline_active_ns": ceil_fraction(self.timeline.active_ns),
            "timeline_records": self.timeline.record_count,
            "timeline_snapshots": self.timeline.snapshot_count,
            "timeline_gaps": self.timeline.gap_count,
            "timeline_state_coverage": {
                "modelled": [
                    "TU release and scheduler-ready queues",
                    "per-C prepared-input TU_SEQ identities and independent contiguous per-F REL_SEQ orders",
                    "C-to-F placement and independent F input-staging/compiler pools",
                    "fork/join dialogue release, serialization, and propagation",
                    "writer priority with bounded serialization quanta",
                    "per-route, per-C, per-F, common-fabric, and directional-fabric max-min allocation",
                    "F input-wait, ready-input, compiler, and commit-wait occupancy",
                    "build barriers and workload release gaps",
                    "exact C-to-F and F-to-C byte events",
                    "exact transient resource/queue byte credits and final closure",
                    "resident environments plus explicit single-flight environment stress",
                    "exact route-trace or aggregate policy replay closure",
                ],
                "adapter": self.adapter.timeline_coverage(),
                "not_modelled": [
                    "preprocessor CPU and producer pipe backpressure",
                    "codec CPU and cache-thread queueing",
                    "memory and cache eviction",
                    "compile-job reference and outer cache-channel framing bytes",
                    "OS socket buffering and packet headers",
                ],
            },
        }
        if self.scenario.is_v2:
            expected = self.v2_runtime["expected"]
            summary.update(
                {
                    "selected_main_protocol": expected["selected_main_protocol"],
                    "selected_cache_wire": expected["selected_cache_wire"],
                    "selected_codec_profile": expected["selected_codec_profile"],
                    "compile_result": "pass",
                }
            )
        return SimulationResult(
            summary,
            assignments,
            self.events,
            workers,
            builds,
            generations,
            self.timeline,
        )


def write_tsv(path: Path, rows: Iterable[dict[str, object]]) -> None:
    iterator = iter(rows)
    first = next(iterator, None)
    if first is None:
        raise ValueError(f"refusing to write empty table {path}")
    with path.open("w", newline="") as output:
        writer = csv.DictWriter(
            output, fieldnames=list(first), delimiter="\t", lineterminator="\n"
        )
        writer.writeheader()
        writer.writerow(first)
        writer.writerows(iterator)


def payload_overrides(values: list[str]) -> dict[str, Path]:
    result: dict[str, Path] = {}
    for value in values:
        if "=" not in value:
            raise ValueError("--corpus-root must be WORKLOAD=PATH")
        workload, path = value.split("=", 1)
        if not workload or not path or workload in result:
            raise ValueError("invalid or repeated --corpus-root override")
        result[workload] = Path(path).resolve()
    return result


def resolved_scenario_document(scenario: LoadedScenario) -> dict[str, object]:
    if scenario.is_v2:
        return json.loads(json.dumps(scenario.manifest))
    document = json.loads(json.dumps(scenario.document))
    for job in document["environments"]["job_selection"]["jobs"]:
        job["corpus_root"] = scenario.workload_inputs[job["id"]]["corpus_root"]
    return document


def bandwidth_label(bits_per_second: int) -> str:
    for divisor, suffix in (
        (1_000_000_000_000, "T"),
        (1_000_000_000, "G"),
        (1_000_000, "M"),
        (1_000, "K"),
    ):
        if bits_per_second % divisor == 0 and bits_per_second >= divisor:
            return f"{bits_per_second // divisor}{suffix}"
    return str(bits_per_second)


def topology_label(document: dict[str, object]) -> str:
    environments = int(document["environments"]["env_count"])
    workers = int(document["workers"]["f_count"])
    slots = int(document["workers"]["template"]["slots"])
    network = document["network"]
    c_to_f = network["c_to_f"]
    environment_bps = c_to_f.get("per_environment_bits_per_second")
    if environment_bps is not None:
        capacity = f"B{bandwidth_label(int(environment_bps))}"
        fabric_value = network.get(
            "shared_fabric_bps", c_to_f.get("fabric_bits_per_second")
        )
        if fabric_value is None:
            raise ValueError("topology has no C-to-F fabric capacity")
        fabric_bps = int(fabric_value)
        fabric = (
            f"X{bandwidth_label(fabric_bps)}"
            if fabric_bps < int(environment_bps) * environments
            else ""
        )
    else:
        capacity = f"R{bandwidth_label(int(c_to_f['bits_per_second']))}"
        fabric_value = network.get(
            "shared_fabric_bps", c_to_f.get("fabric_bits_per_second")
        )
        if fabric_value is None:
            raise ValueError("topology has no C-to-F fabric capacity")
        fabric = f"X{bandwidth_label(int(fabric_value))}"
    return f"C{environments}F{workers}_{slots}{capacity}{fabric}"


def experiment_descriptor(
    scenario: LoadedScenario,
    result: SimulationResult,
    execution: dict[str, object] | None = None,
) -> dict[str, object]:
    """Build the deterministic descriptor which heads an experiment stream."""
    resolved = resolved_scenario_document(scenario)
    engine_document = scenario.document
    allocation_resources = ["direction/environment/F route"]
    if "shared_fabric_bps" in engine_document["network"]:
        allocation_resources.append("optional common fabric")
    if any(
        "fabric_bits_per_second" in engine_document["network"][direction]
        for direction in DIRECTIONS
    ):
        allocation_resources.append("per-direction fabric")
    if any(
        "per_environment_bits_per_second" in engine_document["network"][direction]
        for direction in DIRECTIONS
    ):
        allocation_resources.append("per-direction C-authority aggregate")
    if any(
        "per_worker_bits_per_second" in engine_document["network"][direction]
        for direction in DIRECTIONS
    ):
        allocation_resources.append("per-direction F aggregate")
    descriptor = {
        "record": "experiment",
        "schema": "icecream-distribution-timeline-v1",
        "scenario": result.summary["scenario"],
        "topology": topology_label(engine_document),
        "topology_dimensions": {
            "schema_semantics": (
                "v1 maps one producer agent, one logical C authority, and one C egress "
                "group to each environment"
            ),
            "producer_agents": result.summary["environments"],
            "logical_c_authorities": result.summary["environments"],
            "c_egress_groups": result.summary["environments"],
            "f_stores": result.summary["workers"],
            "compiler_slots_per_f": result.summary["slots_per_worker"],
            "input_staging_slots_per_f": result.summary[
                "input_staging_slots_per_worker"
            ],
        },
        "codec_adapter": result.summary["codec_adapter"],
        "physical_codec_result": result.summary["physical_codec_result"],
        "clock": {
            "unit": "nanosecond",
            "snapshot_interval_ns": result.summary["timeline_snapshot_interval_ns"],
            "snapshot_axis": "active time",
            "gap_rule": (
                "wall intervals with no compiler work and no network serialization "
                "are represented by one gap record; wall timers still advance"
            ),
        },
        "score": {
            "name": "modelled C-to-F bytes",
            "direction": "c_to_f",
            "field": "scored_outgoing_bytes",
            "return_bytes_retained_separately": True,
            "scope": (
                "adapter phases only; live job-reference messages and outer cache-channel "
                "framing are not yet present"
            ),
        },
        "network_allocation": {
            "algorithm": "deterministic max-min fairness",
            "resources": allocation_resources,
            "serialization_then_propagation": True,
        },
        "record_order": [
            "experiment descriptor",
            "active snapshots and compressed idle gaps",
            "final reconciliation summary",
        ],
        "snapshot_semantics": {
            "metrics": "exact interval averages from integrated flow rates",
            "snapshot_state": (
                "state after all engine transitions at the snapshot boundary"
            ),
            "gap_state": (
                "state at the gap start; no compiler or serializer is active inside the gap"
            ),
            "events": "each exact engine event appears once, on the first record ending at or after it",
        },
        "state_coverage": result.summary["timeline_state_coverage"],
        "resolved_scenario": resolved,
        "workload_inputs": result.summary["workload_inputs"],
        "simulator": {
            "source": "capability/distribution/run_scenario.py",
            "source_sha256": sha256(Path(__file__).resolve()),
        },
        "expected_summary": result.summary,
    }
    if scenario.is_v2:
        if execution is None:
            raise ValueError(
                "v2 experiment output requires a separate execution header"
            )
        descriptor = {
            "record": "execution",
            **execution,
            "schema": "icecream-execution-v2",
            "scenario_digest": scenario.scenario_digest,
            "scenario": result.summary["scenario"],
            "topology": topology_label(engine_document),
            "topology_dimensions": descriptor["topology_dimensions"],
            "codec_adapter": result.summary["codec_adapter"],
            "physical_codec_result": result.summary["physical_codec_result"],
            "clock": descriptor["clock"],
            "score": descriptor["score"],
            "network_allocation": descriptor["network_allocation"],
            "record_order": [
                "execution header with scenario digest",
                "active snapshots and compressed inactive gaps",
                "final reconciliation summary",
            ],
            "snapshot_semantics": descriptor["snapshot_semantics"],
            "state_coverage": descriptor["state_coverage"],
            "manifest_schema": scenario.manifest["schema"],
            "scenario_manifest": resolved,
            "workload_inputs": result.summary["workload_inputs"],
            "identity_contract": list(EVENT_IDENTITY_FIELDS),
            "ledger_contract": {
                "directional": "exact per-link deltas",
                "resource_and_queue": "non-negative during run and zero at closure",
                "route_replay": result.summary["replay_closure"]["semantics"],
            },
            "simulator": {
                "source": "capability/distribution/run_scenario.py",
                "source_sha256": sha256(Path(__file__).resolve()),
            },
            "expected_summary": result.summary,
        }
        if scenario.route_trace:
            descriptor["route_trace_evidence"] = {
                "path": "route-trace.jsonl",
                "sha256": scenario.route_trace_sha256,
            }
    return descriptor


def iter_event_timeline(result: SimulationResult) -> Iterator[dict[str, object]]:
    """Attach every exact event while iterating timeline records once."""
    events = iter(result.events)
    event = next(events, None)
    event_count = 0
    for source_record in result.timeline:
        record = dict(source_record)
        cutoff = int(record["wall_end_ns"])
        attached = []
        while event is not None and int(event["time_ns"]) <= cutoff:
            attached.append(event)
            event_count += 1
            event = next(events, None)
        record["events"] = attached
        record["event_sequence_start"] = "" if not attached else attached[0]["sequence"]
        record["event_sequence_end"] = "" if not attached else attached[-1]["sequence"]
        yield record
    if event is not None or event_count != len(result.events):
        raise RuntimeError(
            f"timeline attached {event_count} of {len(result.events)} engine events"
        )


def experiment_final(result: SimulationResult) -> dict[str, object]:
    return {
        "record": "summary",
        "schema": (
            "icecream-experiment-summary-v2"
            if result.summary["schema"] == "icecream-experiment-result-v2"
            else "icecream-distribution-timeline-summary-v1"
        ),
        "timeline_records": len(result.timeline),
        "event_count": len(result.events),
        "summary": result.summary,
    }


def experiment_records(
    scenario: LoadedScenario,
    result: SimulationResult,
    execution: dict[str, object] | None = None,
) -> tuple[dict[str, object], list[dict[str, object]], dict[str, object]]:
    """Materialize an experiment stream for small callers and focused tests."""
    descriptor = experiment_descriptor(scenario, result, execution)
    timeline = list(iter_event_timeline(result))
    final = experiment_final(result)
    return descriptor, timeline, final


def write_jsonl(
    path: Path,
    descriptor: dict[str, object],
    timeline: list[dict[str, object]],
    final: dict[str, object],
) -> None:
    with path.open("w") as output:
        for row in (descriptor, *timeline, final):
            output.write(json.dumps(row, separators=(",", ":"), sort_keys=True))
            output.write("\n")


def selected_snapshot_ordinals(count: int, limit: int) -> tuple[set[int], str]:
    checked_positive_int(limit, "report snapshot limit")
    if count <= limit:
        return set(range(count)), "all"
    if limit == 1:
        return {count - 1}, "last snapshot"
    return {
        index * (count - 1) // (limit - 1) for index in range(limit)
    }, "evenly spaced including first and last"


def write_experiment_stream(
    path: Path,
    scenario: LoadedScenario,
    result: SimulationResult,
    execution: dict[str, object] | None = None,
    report_limit: int = MAX_REPORT_SNAPSHOTS,
) -> tuple[dict[str, object], list[dict[str, object]], dict[str, object]]:
    """Write the canonical stream while retaining only the bounded browser view."""
    descriptor = experiment_descriptor(scenario, result, execution)
    final = experiment_final(result)
    selected, selection = selected_snapshot_ordinals(
        result.timeline.snapshot_count, report_limit
    )
    report_timeline: list[dict[str, object]] = []
    snapshot_ordinal = 0
    with path.open("w") as output:
        output.write(json.dumps(descriptor, separators=(",", ":"), sort_keys=True))
        output.write("\n")
        for row in iter_event_timeline(result):
            output.write(json.dumps(row, separators=(",", ":"), sort_keys=True))
            output.write("\n")
            if row["record"] == "gap":
                report_timeline.append(row)
            elif snapshot_ordinal in selected:
                report_timeline.append(row)
            snapshot_ordinal += row["record"] == "snapshot"
        output.write(json.dumps(final, separators=(",", ":"), sort_keys=True))
        output.write("\n")
    if snapshot_ordinal != result.timeline.snapshot_count:
        raise RuntimeError("timeline snapshot count changed while streaming output")
    report_descriptor = json.loads(json.dumps(descriptor))
    report_descriptor["report_view"] = {
        "source_records": len(result.timeline),
        "source_snapshots": result.timeline.snapshot_count,
        "embedded_records": len(report_timeline),
        "embedded_snapshots": len(selected),
        "snapshot_selection": selection,
        "all_gap_records_embedded": True,
        "canonical_detail": (
            "experiment.jsonl retains every active-time snapshot and event"
        ),
    }
    return report_descriptor, report_timeline, final


def compact_report_timeline(
    timeline: list[dict[str, object]], limit: int = MAX_REPORT_SNAPSHOTS
) -> tuple[list[dict[str, object]], dict[str, object]]:
    """Retain an evenly spaced browser view while leaving canonical JSONL untouched."""
    snapshot_positions = [
        index for index, row in enumerate(timeline) if row["record"] == "snapshot"
    ]
    selected_ordinals, selection = selected_snapshot_ordinals(
        len(snapshot_positions), limit
    )
    selected = {snapshot_positions[index] for index in selected_ordinals}
    view = [
        row
        for index, row in enumerate(timeline)
        if row["record"] == "gap" or index in selected
    ]
    metadata = {
        "source_records": len(timeline),
        "source_snapshots": len(snapshot_positions),
        "embedded_records": len(view),
        "embedded_snapshots": len(selected),
        "snapshot_selection": selection,
        "all_gap_records_embedded": True,
        "canonical_detail": (
            "experiment.jsonl retains every active-time snapshot and event"
        ),
    }
    return view, metadata


def render_report_html(
    descriptor: dict[str, object],
    timeline: list[dict[str, object]],
    final: dict[str, object],
) -> str:
    """Return a dependency-free report which also works when opened via file://."""
    if "report_view" in descriptor:
        report_timeline = timeline
        report_descriptor = descriptor
    else:
        report_timeline, report_view = compact_report_timeline(timeline)
        report_descriptor = json.loads(json.dumps(descriptor))
        report_descriptor["report_view"] = report_view
    payload = json.dumps(
        {
            "descriptor": report_descriptor,
            "timeline": report_timeline,
            "final": final,
        },
        separators=(",", ":"),
    ).replace("</", "<\\/")
    template = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Icecream distribution experiment</title>
<style>
:root{color-scheme:dark;--bg:#0b1020;--panel:#121a2d;--line:#263451;--text:#e8edf7;--muted:#94a3b8;--cf:#39d98a;--fc:#6ea8fe;--sel:#ffbe55;--queue:#d58cff}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:14px/1.45 system-ui,sans-serif}main{max-width:1500px;margin:auto;padding:24px}
h1{font-size:24px;margin:0 0 4px}h2{font-size:17px;margin:0 0 12px}.sub{color:var(--muted);margin-bottom:20px}.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:10px;margin:16px 0}.card,.panel{background:var(--panel);border:1px solid var(--line);border-radius:9px;padding:14px}.card .v{font-size:20px;font-weight:700}.card .k{color:var(--muted);font-size:12px}.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}.wide{grid-column:1/-1}canvas{display:block;width:100%;height:250px;background:#0c1425;border-radius:5px}.heat{height:auto;min-height:180px}.controls{display:flex;gap:16px;align-items:center;flex-wrap:wrap;margin:10px 0 16px}select,input{background:#0c1425;color:var(--text);border:1px solid var(--line);border-radius:5px;padding:5px}input[type=range]{width:min(700px,70vw)}table{border-collapse:collapse;width:100%}th,td{text-align:left;border-bottom:1px solid var(--line);padding:6px 8px}th{color:var(--muted)}pre{white-space:pre-wrap;overflow:auto;max-height:460px;background:#090e1a;padding:12px;border-radius:5px}.legend{color:var(--muted);font-size:12px;margin-top:7px}.dot{display:inline-block;width:9px;height:9px;border-radius:50%;margin:0 4px 0 12px}.dot:first-child{margin-left:0}@media(max-width:850px){.grid{grid-template-columns:1fr}.wide{grid-column:auto}}
</style>
</head>
<body><main>
<h1 id="title">Icecream distribution experiment</h1><div class="sub" id="subtitle"></div>
<div class="cards" id="cards"></div>
<div class="controls"><label>F <select id="worker"></select></label><label>Snapshot <input id="cursor" type="range" min="0" step="1"></label><span id="cursorLabel"></span></div>
<div class="grid">
 <section class="panel wide"><h2>Network serialization rate</h2><canvas id="bandwidth"></canvas><div class="legend"><span class="dot" style="background:var(--cf)"></span>all C→F <span class="dot" style="background:var(--fc)"></span>all F→C <span class="dot" style="background:var(--sel)"></span>selected F C→F</div></section>
 <section class="panel"><h2>Selected F pipeline occupancy</h2><canvas id="slots"></canvas><div class="legend"><span class="dot" style="background:var(--cf)"></span>compiling <span class="dot" style="background:var(--sel)"></span>waiting for input <span class="dot" style="background:var(--fc)"></span>ready input <span class="dot" style="background:var(--queue)"></span>waiting for commit <span class="dot" style="background:#ef6f6c"></span>staging occupied</div></section>
 <section class="panel"><h2>Scheduler state</h2><canvas id="queue"></canvas><div class="legend"><span class="dot" style="background:var(--queue)"></span>ready <span class="dot" style="background:var(--sel)"></span>active <span class="dot" style="background:var(--cf)"></span>completed</div></section>
 <section class="panel wide"><h2>C→F rate by F</h2><canvas class="heat" id="heatmap"></canvas><div class="legend">Rows are Fs; brighter cells carry more interval-average C→F traffic.</div></section>
 <section class="panel"><h2>Compressed idle gaps</h2><div id="gaps"></div></section>
 <section class="panel"><h2>Selected report snapshot and exact transitions</h2><pre id="detail"></pre></section>
 <section class="panel wide"><h2>Experiment definition</h2><pre id="definition"></pre></section>
</div>
</main>
<script>
const DATA=__PAYLOAD__;
const D=DATA.descriptor,S=DATA.timeline.filter(x=>x.record==='snapshot'),G=DATA.timeline.filter(x=>x.record==='gap'),SUM=DATA.final.summary;
const $=id=>document.getElementById(id), fmtN=n=>new Intl.NumberFormat('en-US',{maximumFractionDigits:2}).format(n), fmtB=n=>n>=1e9?fmtN(n/1e9)+' GB':n>=1e6?fmtN(n/1e6)+' MB':n>=1e3?fmtN(n/1e3)+' kB':fmtN(n)+' B', fmtT=n=>n>=1e9?fmtN(n/1e9)+' s':n>=1e6?fmtN(n/1e6)+' ms':n>=1e3?fmtN(n/1e3)+' µs':fmtN(n)+' ns', fmtRate=n=>n>=1e9?fmtN(n/1e9)+' Gb/s':n>=1e6?fmtN(n/1e6)+' Mb/s':fmtN(n)+' b/s';
$('title').textContent=D.scenario+' — '+D.codec_adapter;
$('subtitle').textContent=D.topology+' · active-time samples every '+fmtT(D.clock.snapshot_interval_ns)+' · report view '+fmtN(D.report_view.embedded_snapshots)+' / '+fmtN(D.report_view.source_snapshots)+' snapshots · '+(D.physical_codec_result?'physical codec result':'diagnostic adapter');
const cards=[['Outgoing score',fmtB(SUM.scored_outgoing_bytes)],['Return bytes',fmtB(SUM.f_to_c_bytes)],['Active timeline',fmtT(SUM.timeline_active_ns)],['Generation sum',fmtT(SUM.summed_generation_ns)],['Wall makespan',fmtT(SUM.makespan_ns)],['Jobs',fmtN(SUM.jobs)],['Fs / slots each',SUM.workers+' / '+fmtN(SUM.slots_per_worker)],['Trace rows',fmtN(SUM.timeline_records)]];
$('cards').innerHTML=cards.map(x=>'<div class="card"><div class="v">'+x[1]+'</div><div class="k">'+x[0]+'</div></div>').join('');
for(let w=0;w<SUM.workers;w++)$('worker').add(new Option('F'+(w+1),w));
$('cursor').max=Math.max(0,S.length-1);$('cursor').value=0;
function rate(row,dir,worker=null){return row.metrics.routes.filter(r=>r.direction===dir&&(worker===null||r.worker===worker)).reduce((a,r)=>a+r.average_bps,0)}
function workerMetric(row,w){return row.metrics.workers.find(x=>x.worker===w)||{average_compiling_slots:0,average_input_wait_slots:0,average_ready_input_slots:0,average_commit_wait_slots:0,average_staging_slots:0}}
function fit(c,h=250){const d=devicePixelRatio||1,w=Math.max(320,c.clientWidth);c.width=w*d;c.height=h*d;c.style.height=h+'px';const x=c.getContext('2d');x.setTransform(d,0,0,d,0,0);return{x,w,h}}
function axes(ctx,w,h,max,label){ctx.strokeStyle='#263451';ctx.fillStyle='#94a3b8';ctx.font='11px system-ui';ctx.beginPath();for(let i=0;i<=4;i++){let y=12+(h-32)*i/4;ctx.moveTo(45,y);ctx.lineTo(w-8,y);ctx.fillText(label(max*(1-i/4)),4,y+3)}ctx.stroke()}
function lineChart(id,series,colors,label){const c=$(id),{x,w,h}=fit(c);const vals=series.flat();const max=Math.max(1,...vals);axes(x,w,h,max,label);for(let k=0;k<series.length;k++){x.strokeStyle=colors[k];x.lineWidth=1.7;x.beginPath();series[k].forEach((v,i)=>{const px=45+(w-53)*(series[k].length===1?0:i/(series[k].length-1)),py=12+(h-32)*(1-v/max);i?x.lineTo(px,py):x.moveTo(px,py)});x.stroke()}}
function draw(){const w=+$('worker').value;lineChart('bandwidth',[S.map(r=>rate(r,'c_to_f')),S.map(r=>rate(r,'f_to_c')),S.map(r=>rate(r,'c_to_f',w))],['#39d98a','#6ea8fe','#ffbe55'],fmtRate);lineChart('slots',[S.map(r=>workerMetric(r,w).average_compiling_slots),S.map(r=>workerMetric(r,w).average_input_wait_slots),S.map(r=>workerMetric(r,w).average_ready_input_slots),S.map(r=>workerMetric(r,w).average_commit_wait_slots),S.map(r=>workerMetric(r,w).average_staging_slots)],['#39d98a','#ffbe55','#6ea8fe','#d58cff','#ef6f6c'],x=>fmtN(x));lineChart('queue',[S.map(r=>r.state.scheduler.ready_tus),S.map(r=>r.state.scheduler.active_tus),S.map(r=>r.state.scheduler.completed_tus)],['#d58cff','#ffbe55','#39d98a'],x=>fmtN(x));drawHeat();showDetail()}
function drawHeat(){const c=$('heatmap'),rowH=Math.max(7,Math.min(16,600/SUM.workers)),h=24+rowH*SUM.workers,{x,w}=fit(c,h),plotW=Math.max(1,Math.floor(w-55)),bins=Array.from({length:SUM.workers},()=>new Float64Array(plotW));let max=1;S.forEach((r,i)=>{const px=Math.min(plotW-1,Math.floor(i*plotW/Math.max(1,S.length)));for(const q of r.metrics.routes)if(q.direction==='c_to_f'){bins[q.worker][px]=Math.max(bins[q.worker][px],q.average_bps);max=Math.max(max,q.average_bps)}});x.font='10px system-ui';for(let f=0;f<SUM.workers;f++){let y=10+f*rowH;x.fillStyle='#94a3b8';x.fillText('F'+(f+1),4,y+rowH-2);for(let px=0;px<plotW;px++){let z=bins[f][px]/max;x.fillStyle='rgba(57,217,138,'+(0.04+0.96*Math.sqrt(z))+')';x.fillRect(48+px,y,1,Math.max(1,rowH-1))}}}
function showDetail(){if(!S.length){$('detail').textContent='No active snapshots';return}const i=+$('cursor').value,r=S[i],w=+$('worker').value,f=r.state.f.find(x=>x.worker===w);$('cursorLabel').textContent=(i+1)+' / '+S.length+' · active '+fmtT(r.active_end_ns)+' · wall '+fmtT(r.wall_end_ns);$('detail').textContent=JSON.stringify({interval:{wall_start_ns:r.wall_start_ns,wall_end_ns:r.wall_end_ns,active_start_ns:r.active_start_ns,active_end_ns:r.active_end_ns},scheduler:r.state.scheduler,selected_f:f,selected_f_metrics:workerMetric(r,w),selected_f_routes:r.metrics.routes.filter(x=>x.worker===w),events:r.events},null,2)}
$('worker').onchange=draw;$('cursor').oninput=showDetail;window.onresize=draw;
if(G.length){$('gaps').innerHTML='<table><thead><tr><th>Wall start</th><th>Duration</th><th>Reason</th></tr></thead><tbody>'+G.map(g=>'<tr><td>'+fmtT(g.wall_start_ns)+'</td><td>'+fmtT(g.wall_duration_ns)+'</td><td>'+g.reason+'</td></tr>').join('')+'</tbody></table>'}else $('gaps').textContent='No idle gaps.';
$('definition').textContent=JSON.stringify(D,null,2);draw();
</script></body></html>"""
    return template.replace("__PAYLOAD__", payload)


def write_route_trace(
    path: Path, scenario: LoadedScenario, result: SimulationResult
) -> None:
    profile = result.summary["codec_adapter"]
    rows: list[dict[str, object]] = [
        {
            "record": "route_trace",
            "schema": "icecream-route-trace-v1",
            "scenario_digest": scenario.scenario_digest,
            "codec_profile": profile,
            "provenance": "modeled",
        }
    ]
    c_to_f = 0
    f_to_c = 0
    for assignment in result.assignments:
        row = {
            "record": "assignment",
            "logical_job_id": assignment["logical_job_id"],
            "attempt_id": assignment["attempt_id"],
            "workload": assignment["workload"],
            "build": assignment["build"],
            "logical": assignment["logical"],
            "worker": assignment["worker"],
            "C_STORE_GUID": assignment["C_STORE_GUID"],
            "physical_endpoint": assignment["physical_endpoint"],
            "RouteLaneId": assignment["RouteLaneId"],
            "F_STORE_GUID": assignment["F_STORE_GUID"],
            "session_serial": assignment["session_serial"],
            "HISTORY_NONCE": assignment["HISTORY_NONCE"],
            "TU_SEQ": assignment["tu_seq"],
            "REL_SEQ": assignment["rel_seq"],
            "c_to_f_bytes": assignment["c_to_f_bytes"],
            "f_to_c_bytes": assignment["f_to_c_bytes"],
        }
        c_to_f += int(assignment["c_to_f_bytes"])
        f_to_c += int(assignment["f_to_c_bytes"])
        rows.append(row)
    rows.append(
        {
            "record": "route_summary",
            "assignments": len(result.assignments),
            "c_to_f_bytes": c_to_f,
            "f_to_c_bytes": f_to_c,
        }
    )
    for index, row in enumerate(rows, start=1):
        validate_json_schema(row, "route-trace.schema.json", f"{path}:{index}")
    with path.open("w") as output:
        for row in rows:
            output.write(json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n")


def validate_experiment_jsonl(path: Path) -> dict[str, int]:
    """Independently replay every v2 identity and accounting claim in a stream."""
    rows = [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
    if len(rows) < 3 or rows[0].get("record") != "execution":
        raise ValueError(f"{path}: v2 stream lacks its execution header")
    header = rows[0]
    validate_json_schema(header, "execution.schema.json", f"{path}:execution")
    manifest = header["scenario_manifest"]
    validate_json_schema(
        manifest, "experiment.schema.json", f"{path}:embedded scenario_manifest"
    )
    scenario_digest = canonical_json_sha256(manifest)
    if header["scenario_digest"] != scenario_digest:
        raise ValueError(f"{path}: embedded scenario manifest digest differs")
    if rows[-1].get("record") != "summary":
        raise ValueError(f"{path}: v2 stream lacks its final summary")
    if rows[-1].get("schema") != "icecream-experiment-summary-v2":
        raise ValueError(f"{path}: v2 stream has an unknown summary schema")
    final_summary = rows[-1].get("summary")
    if not isinstance(final_summary, dict):
        raise ValueError(f"{path}: final summary is not an object")
    if scenario_digest != final_summary.get("scenario_digest"):
        raise ValueError(f"{path}: execution/scenario digest mismatch")
    if header["expected_summary"] != final_summary:
        raise ValueError(
            f"{path}: execution expected_summary differs from final summary"
        )
    if header["identity_contract"] != list(EVENT_IDENTITY_FIELDS):
        raise ValueError(f"{path}: execution identity contract differs from validator")
    if header["workload_inputs"] != final_summary.get("workload_inputs"):
        raise ValueError(f"{path}: execution/final workload inputs differ")

    manifest_jobs = {job["id"]: job for job in manifest["workload"]["jobs"]}
    if len(manifest_jobs) != len(manifest["workload"]["jobs"]):
        raise ValueError(f"{path}: embedded manifest repeats a workload identity")
    workload_inputs = header["workload_inputs"]
    if set(workload_inputs) != set(manifest_jobs):
        raise ValueError(f"{path}: embedded workload set differs from manifest")
    content_rows: dict[tuple[str, int], dict[str, object]] = {}
    input_manifest_digests = []
    for workload, job in manifest_jobs.items():
        embedded = workload_inputs[workload]
        if not isinstance(embedded, dict):
            raise ValueError(f"{path}: workload input {workload!r} is not an object")
        content_manifest = embedded.get("content_manifest")
        validate_json_schema(
            content_manifest,
            "workload-input.schema.json",
            f"{path}:workload {workload}",
        )
        digest = canonical_json_sha256(content_manifest)
        if digest != job["manifest_digest"] or digest != embedded.get(
            "manifest_digest"
        ):
            raise ValueError(f"{path}: workload {workload!r} content digest differs")
        if (
            embedded.get("trace") != job["trace"]
            or embedded.get("corpus_root") != job["corpus_root"]
        ):
            raise ValueError(f"{path}: workload {workload!r} logical paths differ")
        canonical_relative_identity(job["trace"], f"{workload}.trace")
        canonical_relative_identity(
            job["corpus_root"], f"{workload}.corpus_root", allow_dot=True
        )
        input_rows = content_manifest["rows"]
        if embedded.get("trace_rows") != len(input_rows):
            raise ValueError(f"{path}: workload {workload!r} trace row count differs")
        if (
            not isinstance(embedded.get("trace_sha256"), str)
            or SHA256_RE.fullmatch(embedded["trace_sha256"]) is None
        ):
            raise ValueError(f"{path}: workload {workload!r} trace digest is invalid")
        if [row["logical"] for row in input_rows] != list(range(len(input_rows))):
            raise ValueError(
                f"{path}: workload {workload!r} logical rows are not contiguous"
            )
        if len({row["job_id"] for row in input_rows}) != len(input_rows):
            raise ValueError(f"{path}: workload {workload!r} repeats job identity")
        for row in input_rows:
            canonical_relative_identity(
                row["ii_relative"],
                f"{workload} trace row {row['logical']} ii_relative",
            )
            content_rows[(workload, row["logical"])] = row
        input_manifest_digests.append(digest)
    if sorted(header["input_manifest_digests"]) != sorted(set(input_manifest_digests)):
        raise ValueError(f"{path}: execution input manifest digests do not reconcile")

    expected_tus: dict[tuple[str, int, int], dict[str, object]] = {}
    retained_work_items: dict[tuple[str, int], list[WorkItem]] = {}
    ordinal_to_key: dict[int, tuple[str, int, int]] = {}
    ordinal = 0
    for job in manifest["workload"]["jobs"]:
        workload = job["id"]
        rows_for_workload = workload_inputs[workload]["content_manifest"]["rows"]
        for build in range(job["build_epochs"]):
            items: list[WorkItem] = []
            for content in rows_for_workload:
                key = (workload, build, content["logical"])
                expected_tus[key] = {
                    "environment": job["c_index"],
                    "content": content,
                }
                item = WorkItem(
                    ordinal,
                    job["c_index"],
                    workload,
                    build,
                    content["logical"],
                    content["job_id"],
                    Path(content["ii_relative"]),
                    content["raw_bytes"],
                    content["compile_ns"],
                    0,
                    content["raw_sha256"],
                    content["compile_provenance"],
                )
                items.append(item)
                ordinal_to_key[ordinal] = key
                ordinal += 1
            retained_work_items[(workload, build)] = items

    selected_main_protocol = min(
        component["main_protocol"] for component in manifest["components"].values()
    )
    expected = manifest["expected"]
    selected_claims = {
        "selected_main_protocol": selected_main_protocol,
        "selected_cache_wire": manifest["capabilities"]["cache_wire"],
        "selected_codec_profile": manifest["capabilities"]["codec_profile"],
        "compile_result": "pass",
    }
    if expected != selected_claims:
        raise ValueError(f"{path}: manifest expected outcome does not reconcile")
    for name, value in selected_claims.items():
        if final_summary.get(name) != value:
            raise ValueError(f"{path}: final {name} differs from selected outcome")
    selected_adapter = selected_v2_adapter(manifest)
    if selected_adapter is None or header["codec_adapter"] != selected_adapter:
        raise ValueError(f"{path}: execution codec adapter differs from manifest")
    if final_summary.get("codec_adapter") != selected_adapter:
        raise ValueError(f"{path}: final codec adapter differs from manifest")
    routing = final_summary.get("routing", {})
    scheduler_policy = manifest["topology"]["scheduler_policy"]
    trace_assignment = manifest["topology"]["assignment_source"] == "route_trace"
    expected_placement = (
        "trace"
        if trace_assignment
        else {
            "round_robin": "round-robin",
            "rendezvous": "rendezvous",
            "dense_frontier": "rendezvous",
        }[scheduler_policy]
    )
    expected_binding = (
        "release-time-static"
        if trace_assignment or scheduler_policy in {"rendezvous", "dense_frontier"}
        else "dispatch-time"
    )
    if (
        routing.get("placement_policy") != expected_placement
        or routing.get("binding") != expected_binding
        or final_summary.get("placement_policy") != expected_placement
    ):
        raise ValueError(f"{path}: routing policy metadata differs from manifest")
    trace_entries: dict[tuple[str, int, int], RouteTraceEntry] = {}
    trace_digest: str | None = None
    trace_provenance: str | None = None
    if manifest["topology"]["assignment_source"] == "route_trace":
        if routing.get("route_trace") != manifest["topology"]["route_trace"]:
            raise ValueError(f"{path}: route-trace logical identity differs")
        if routing.get("route_trace_codec") != selected_adapter:
            raise ValueError(f"{path}: route-trace codec differs from selected adapter")
        if routing.get("route_trace_provenance") not in {"observed", "modeled"}:
            raise ValueError(f"{path}: route-trace provenance is invalid")
        evidence = header.get("route_trace_evidence")
        if not isinstance(evidence, dict):
            raise ValueError(
                f"{path}: exact replay has no retained route-trace evidence"
            )
        evidence_identity = canonical_relative_identity(
            evidence.get("path", ""), "route_trace_evidence.path"
        )
        evidence_path = (path.parent / evidence_identity).resolve()
        if not evidence_path.is_file():
            raise ValueError(f"{path}: retained route-trace evidence is absent")
        entries_by_ordinal, trace_digest, trace_provenance = load_route_trace(
            evidence_path,
            scenario_digest,
            retained_work_items,
            manifest["topology"]["f_count"],
            selected_adapter,
        )
        trace_entries = {
            ordinal_to_key[item_ordinal]: entry
            for item_ordinal, entry in entries_by_ordinal.items()
        }
        replay_claim = final_summary.get("replay_closure", {})
        claimed_digests = {
            evidence.get("sha256"),
            routing.get("route_trace_sha256"),
            replay_claim.get("route_trace_sha256"),
        }
        if claimed_digests != {trace_digest}:
            raise ValueError(
                f"{path}: retained route-trace digest differs from exact bytes"
            )
        if (
            routing.get("route_trace_provenance") != trace_provenance
            or replay_claim.get("route_trace") != manifest["topology"]["route_trace"]
        ):
            raise ValueError(f"{path}: retained route-trace metadata differs")
    elif "route_trace_evidence" in header:
        raise ValueError(f"{path}: policy replay carries exact-route evidence")

    timeline_rows = rows[1:-1]
    if rows[-1].get("timeline_records") != len(timeline_rows):
        raise ValueError(f"{path}: final timeline record count differs")
    if final_summary.get("timeline_records") != len(timeline_rows):
        raise ValueError(f"{path}: summary timeline record count differs")
    events: list[dict[str, object]] = []
    previous_wall_end: int | None = None
    active_position = 0
    snapshot_count = 0
    gap_count = 0
    for row_number, row in enumerate(timeline_rows, start=2):
        if row.get("record") not in {"snapshot", "gap"}:
            raise ValueError(f"{path}:{row_number}: unknown timeline record")
        row_sequence = checked_nonnegative_int(
            row.get("sequence"), f"{path}:{row_number} timeline sequence"
        )
        if row_sequence != row_number - 2:
            raise ValueError(
                f"{path}:{row_number}: timeline sequence is not contiguous"
            )
        wall_start = checked_nonnegative_int(
            row.get("wall_start_ns"), f"{path}:{row_number} wall_start_ns"
        )
        wall_end = checked_nonnegative_int(
            row.get("wall_end_ns"), f"{path}:{row_number} wall_end_ns"
        )
        if (
            wall_end < wall_start
            or row.get("wall_duration_ns") != wall_end - wall_start
        ):
            raise ValueError(f"{path}:{row_number}: timeline wall interval differs")
        if previous_wall_end is None:
            if wall_start != 0:
                raise ValueError(
                    f"{path}:{row_number}: timeline does not start at zero"
                )
        elif wall_start != previous_wall_end:
            raise ValueError(
                f"{path}:{row_number}: timeline wall intervals are not contiguous"
            )
        if row["record"] == "snapshot":
            active_start = checked_nonnegative_int(
                row.get("active_start_ns"), f"{path}:{row_number} active_start_ns"
            )
            active_end = checked_nonnegative_int(
                row.get("active_end_ns"), f"{path}:{row_number} active_end_ns"
            )
            if (
                active_start != active_position
                or active_end < active_start
                or row.get("active_duration_ns") != active_end - active_start
                or active_end - active_start != wall_end - wall_start
            ):
                raise ValueError(
                    f"{path}:{row_number}: active timeline interval differs"
                )
            active_position = active_end
            snapshot_count += 1
        else:
            if row.get("active_position_ns") != active_position:
                raise ValueError(f"{path}:{row_number}: gap active position differs")
            gap_count += 1
        event_rows = row.get("events")
        if not isinstance(event_rows, list):
            raise ValueError(f"{path}:{row_number}: timeline events are not an array")
        expected_start = "" if not event_rows else event_rows[0].get("sequence")
        expected_end = "" if not event_rows else event_rows[-1].get("sequence")
        if (
            row.get("event_sequence_start") != expected_start
            or row.get("event_sequence_end") != expected_end
        ):
            raise ValueError(f"{path}:{row_number}: timeline event extent differs")
        for event in event_rows:
            event_start = event.get("start_ns")
            event_end = event.get("end_ns")
            event_time = event.get("time_ns")
            if not all(
                isinstance(value, int) and not isinstance(value, bool)
                for value in (event_start, event_end, event_time)
            ):
                raise ValueError(f"{path}:{row_number}: event time is not integral")
            lower_closed = previous_wall_end is None
            if (
                event_start < 0
                or event_end > wall_end
                or event_time > wall_end
                or (
                    event_time < wall_start
                    if lower_closed
                    else event_time <= wall_start
                )
            ):
                raise ValueError(
                    f"{path}:{row_number}: event falls outside its timeline interval"
                )
        events.extend(event_rows)
        previous_wall_end = wall_end
    if [event.get("sequence") for event in events] != list(range(len(events))):
        raise ValueError(f"{path}: event sequence is not contiguous")
    event_times = [event.get("time_ns") for event in events]
    if event_times != sorted(event_times):
        raise ValueError(f"{path}: event times move backwards")
    if rows[-1].get("event_count") != len(events):
        raise ValueError(f"{path}: final event count differs")
    if previous_wall_end != final_summary.get("makespan_ns"):
        raise ValueError(f"{path}: timeline wall extent differs from makespan")
    if (
        final_summary.get("timeline_active_ns") != active_position
        or final_summary.get("timeline_snapshots") != snapshot_count
        or final_summary.get("timeline_gaps") != gap_count
    ):
        raise ValueError(f"{path}: timeline summary counts differ")

    try:
        import jsonschema
    except ImportError as error:  # pragma: no cover - packaging/environment diagnostic
        raise RuntimeError("v2 validation requires Python jsonschema") from error
    event_schema = json.loads(Path(__file__).with_name("event.schema.json").read_text())
    event_validator = jsonschema.Draft202012Validator(event_schema)
    directional: dict[str, dict[str, int]] = {
        direction: defaultdict(int) for direction in DIRECTIONS
    }
    directional_by_route: dict[tuple[str, int, int, str], int] = defaultdict(int)
    resources: dict[str, int] = defaultdict(int)
    resource_credited: dict[str, int] = defaultdict(int)
    resource_debited: dict[str, int] = defaultdict(int)
    resource_peak: dict[str, int] = defaultdict(int)
    queues: dict[str, int] = defaultdict(int)
    queue_credited: dict[str, int] = defaultdict(int)
    queue_debited: dict[str, int] = defaultdict(int)
    queue_peak: dict[str, int] = defaultdict(int)
    resource_names: set[str] = set()
    queue_names: set[str] = set()
    route_identities: dict[tuple[int, int], tuple[object, ...]] = {}
    transaction_identities: dict[tuple[str, str], tuple[object, ...]] = {}
    transaction_digests: dict[tuple[str, str], tuple[object, object]] = {}
    dispatches_by_route: dict[tuple[int, int], int] = defaultdict(int)
    assignment_counts: dict[tuple[int, str, int], int] = defaultdict(int)
    lifecycle_counts: dict[tuple[str, int, int], dict[str, int]] = defaultdict(
        lambda: defaultdict(int)
    )
    release_times: dict[tuple[str, int, int], int] = {}
    completion_times: dict[tuple[str, int, int], int] = {}
    tu_sequences: dict[tuple[str, int, int], int] = {}
    dispatch_tu_sequences_by_environment: dict[int, list[int]] = defaultdict(list)
    dispatch_rel_sequences_by_route: dict[tuple[int, int], list[int]] = defaultdict(
        list
    )
    dispatch_transaction_sequences: list[int] = []
    source_bytes_by_tu: dict[tuple[str, int, int], dict[str, int]] = defaultdict(
        lambda: defaultdict(int)
    )

    def apply_history(
        deltas: Mapping[str, object],
        balances: dict[str, int],
        credited: dict[str, int],
        debited: dict[str, int],
        peaks: dict[str, int],
        names: set[str],
        kind: str,
    ) -> None:
        for name, raw_delta in deltas.items():
            delta = int(raw_delta)
            names.add(name)
            balances[name] += delta
            if balances[name] < 0:
                raise ValueError(f"{path}: {kind} {name} became negative")
            if delta >= 0:
                credited[name] += delta
            else:
                debited[name] -= delta
            peaks[name] = max(peaks[name], balances[name])

    for event_index, event in enumerate(events):
        failures = sorted(
            event_validator.iter_errors(event), key=lambda failure: list(failure.path)
        )
        if failures:
            failure = failures[0]
            location = ".".join(str(part) for part in failure.absolute_path) or "<root>"
            raise ValueError(
                f"{path}:event {event_index} schema error at {location}: {failure.message}"
            )
        missing = [field for field in EVENT_IDENTITY_FIELDS if field not in event]
        if missing:
            raise ValueError(f"{path}: event lacks M3 fields {missing}")
        if event["end_ns"] - event["start_ns"] != event["duration_ns"]:
            raise ValueError(f"{path}: event {event_index} duration does not reconcile")
        if event["time_ns"] != event["end_ns"]:
            raise ValueError(f"{path}: event {event_index} time/end differ")
        workload = event["workload"]
        logical = event["logical"]
        if workload != "":
            tu_key = (workload, event["build"], logical)
            expected_tu = expected_tus.get(tu_key)
            if expected_tu is None:
                raise ValueError(f"{path}: event refers to unknown workload TU")
            content = expected_tu["content"]
            assert isinstance(content, dict)
            if event["raw_digest"] != content["raw_sha256"]:
                raise ValueError(
                    f"{path}: event raw digest differs from input manifest"
                )
            if event["provenance"]["raw_digest"] != "observed":
                raise ValueError(f"{path}: event raw digest provenance is not observed")
            if (
                event["provenance"]["compile_duration_input"]
                != content["compile_provenance"]
            ):
                raise ValueError(
                    f"{path}: event compile-duration provenance differs from input"
                )
            job = manifest_jobs[workload]
            if event["environment"] != job["c_index"]:
                raise ValueError(f"{path}: event C environment differs from manifest")
            expected_logical_job_id = "job-" + stable_hex(
                "logical-job",
                scenario_digest,
                job["c_index"],
                workload,
                event["build"],
                content["job_id"],
                digits=32,
            )
            if (
                event["logical_job_id"] != expected_logical_job_id
                or event["attempt_id"] != f"{expected_logical_job_id}:attempt-0"
            ):
                raise ValueError(f"{path}: event logical/attempt identity differs")
            lifecycle_counts[tu_key][event["event"]] += 1
            if not isinstance(event["TU_SEQ"], int):
                raise ValueError(f"{path}: TU event lacks TU_SEQ")
            established_tu_seq = tu_sequences.setdefault(tu_key, event["TU_SEQ"])
            if established_tu_seq != event["TU_SEQ"]:
                raise ValueError(f"{path}: TU_SEQ drifted within one TU")
            trace_entry = trace_entries.get(tu_key)
            expected_c_store = (
                trace_entry.c_store_guid
                if trace_entry is not None
                else stable_hex("c-store", scenario_digest, job["c_index"], digits=32)
            )
            if event["C_STORE_GUID"] != expected_c_store:
                raise ValueError(f"{path}: event C identity differs from manifest")
            if trace_entry is not None and event["TU_SEQ"] != trace_entry.tu_seq:
                raise ValueError(f"{path}: event TU_SEQ differs from route trace")
            if event["event"] == "release":
                if event["start_ns"] != event["end_ns"]:
                    raise ValueError(f"{path}: TU release is not instantaneous")
                release_times[tu_key] = event["time_ns"]
            if event["event"] == "transaction-complete":
                completion_times[tu_key] = event["time_ns"]
        elif event["raw_digest"] is not None:
            raise ValueError(f"{path}: non-TU event carries a raw digest")
        if event["provenance"]["timing"] != "modeled":
            raise ValueError(f"{path}: simulated event timing is not marked modeled")

        selected_profile = manifest["capabilities"]["codec_profile"]
        if (
            selected_profile not in event["negotiated_profiles"]
            or selected_profile not in event["route_state_profiles"]
        ):
            raise ValueError(f"{path}: event profiles omit the selected codec")
        if event["phase"] == "":
            if event["direction"] != "" or event["bytes"] != "":
                raise ValueError(f"{path}: non-flow event carries a phase extent")
        else:
            if event["direction"] not in DIRECTIONS or not isinstance(
                event["bytes"], int
            ):
                raise ValueError(f"{path}: flow event phase extent is incomplete")
            expected_c_to_f_delta = (
                event["bytes"]
                if event["event"] == "flow-sent" and event["direction"] == "c_to_f"
                else 0
            )
            expected_f_to_c_delta = (
                event["bytes"]
                if event["event"] == "flow-sent" and event["direction"] == "f_to_c"
                else 0
            )
            if (
                event["c_to_f_byte_delta"] != expected_c_to_f_delta
                or event["f_to_c_byte_delta"] != expected_f_to_c_delta
            ):
                raise ValueError(f"{path}: flow phase bytes differ from byte delta")

        c_to_f = checked_nonnegative_int(
            event["c_to_f_byte_delta"], "event c_to_f_byte_delta"
        )
        f_to_c = checked_nonnegative_int(
            event["f_to_c_byte_delta"], "event f_to_c_byte_delta"
        )
        account = event["byte_account"]
        expected_byte_provenance = (
            "modeled"
            if event["phase"] != "" and account == "environment"
            else (
                "observed"
                if (c_to_f or f_to_c) and header["physical_codec_result"]
                else "derived"
            )
        )
        if event["provenance"]["byte_delta"] != expected_byte_provenance:
            raise ValueError(f"{path}: event byte provenance differs")
        if (
            event["provenance"]["resource_delta"] != "derived"
            or event["provenance"]["queue_delta"] != "derived"
        ):
            raise ValueError(f"{path}: event resource/queue provenance differs")
        directional["c_to_f"][account] += c_to_f
        directional["f_to_c"][account] += f_to_c
        if workload != "" and account == "source":
            source_bytes_by_tu[tu_key]["c_to_f"] += c_to_f
            source_bytes_by_tu[tu_key]["f_to_c"] += f_to_c
        if c_to_f or f_to_c:
            if not isinstance(event["environment"], int) or not isinstance(
                event["worker"], int
            ):
                raise ValueError(f"{path}: directional event has no physical route")
            directional_by_route[
                ("c_to_f", event["environment"], event["worker"], account)
            ] += c_to_f
            directional_by_route[
                ("f_to_c", event["environment"], event["worker"], account)
            ] += f_to_c
        if isinstance(event["worker"], int):
            route = (event["environment"], event["worker"])
            identity = tuple(
                event[name]
                for name in (
                    "C_STORE_GUID",
                    "physical_endpoint",
                    "RouteLaneId",
                    "F_STORE_GUID",
                    "session_serial",
                    "HISTORY_NONCE",
                )
            )
            if any(value is None for value in identity):
                raise ValueError(f"{path}: route event has incomplete identity")
            established = route_identities.setdefault(route, identity)
            if established != identity:
                raise ValueError(
                    f"{path}: event route identity drifted without transition"
                )
            trace_entry = trace_entries.get(tu_key) if workload != "" else None
            if trace_entry is not None:
                expected_trace_identity = (
                    trace_entry.c_store_guid,
                    trace_entry.physical_endpoint,
                    trace_entry.route_lane_id,
                    trace_entry.f_store_guid,
                    trace_entry.session_serial,
                    trace_entry.history_nonce,
                )
                if (
                    event["worker"] != trace_entry.worker
                    or identity != expected_trace_identity
                    or event["REL_SEQ"] != trace_entry.rel_seq
                    or event["TU_SEQ"] != trace_entry.tu_seq
                ):
                    raise ValueError(
                        f"{path}: event physical route differs from retained route trace"
                    )
            elif workload != "":
                expected_policy_identity = (
                    stable_hex(
                        "c-store", scenario_digest, event["environment"], digits=32
                    ),
                    f"F{event['worker']}",
                    f"C{event['environment']}_F{event['worker']}",
                    stable_hex("f-store", scenario_digest, event["worker"], digits=32),
                    1,
                    int(
                        stable_hex(
                            "history-nonce",
                            scenario_digest,
                            event["environment"],
                            event["worker"],
                            digits=16,
                        ),
                        16,
                    ),
                )
                if identity != expected_policy_identity:
                    raise ValueError(f"{path}: event policy route identity differs")
            transaction_key = (event["logical_job_id"], event["attempt_id"])
            transaction_identity = identity + (event["REL_SEQ"], event["TU_SEQ"])
            established_transaction = transaction_identities.setdefault(
                transaction_key, transaction_identity
            )
            if established_transaction != transaction_identity:
                raise ValueError(f"{path}: event transaction route identity drifted")
            if event["transaction_digest"] is not None:
                recomputed_digest = transaction_identity_digest(
                    scenario_digest,
                    event["logical_job_id"],
                    event["attempt_id"],
                    event["C_STORE_GUID"],
                    event["F_STORE_GUID"],
                    event["HISTORY_NONCE"],
                    event["TU_SEQ"],
                    event["REL_SEQ"],
                    event["raw_digest"] or "unavailable",
                    event["negotiated_profiles"],
                    event["route_state_profiles"],
                )
                recomputed_input = input_record_identity(
                    recomputed_digest, event["raw_digest"] or "unavailable"
                )
                if (
                    event["transaction_digest"] != recomputed_digest
                    or event["InputRecord_identity"] != recomputed_input
                ):
                    raise ValueError(
                        f"{path}: event transaction/InputRecord digest differs"
                    )
                digest_identity = (
                    event["transaction_digest"],
                    event["InputRecord_identity"],
                )
                established_digest = transaction_digests.setdefault(
                    transaction_key, digest_identity
                )
                if established_digest != digest_identity:
                    raise ValueError(
                        f"{path}: event transaction digest identity drifted"
                    )
            if manifest["topology"]["assignment_source"] == "route_trace":
                if event["REL_SEQ"] is None:
                    raise ValueError(f"{path}: route-trace event lacks REL_SEQ")
                expected_route_provenance = final_summary["routing"].get(
                    "route_trace_provenance"
                )
                if event["provenance"]["route_identity"] != expected_route_provenance:
                    raise ValueError(
                        f"{path}: route identity provenance differs from trace"
                    )
        elif (
            event["transaction_digest"] is not None
            or event["InputRecord_identity"] is not None
        ):
            raise ValueError(f"{path}: C-only event carries a transaction identity")
        if event["tu_seq"] != "" and event["tu_seq"] != event["TU_SEQ"]:
            raise ValueError(f"{path}: TU_SEQ aliases differ")
        if event["rel_seq"] != "" and event["rel_seq"] != event["REL_SEQ"]:
            raise ValueError(f"{path}: REL_SEQ aliases differ")
        if (
            isinstance(event["worker"], int)
            and workload != ""
            and event["event"] != "route-bound"
            and event["transaction_digest"] is None
        ):
            raise ValueError(f"{path}: transaction event lacks its digest")
        if event["event"] == "dispatch":
            dispatches_by_route[(event["environment"], event["worker"])] += 1
            assignment_counts[
                (event["environment"], event["workload"], event["worker"])
            ] += 1
            dispatch_tu_sequences_by_environment[event["environment"]].append(
                event["TU_SEQ"]
            )
            dispatch_rel_sequences_by_route[
                (event["environment"], event["worker"])
            ].append(event["REL_SEQ"])
            dispatch_transaction_sequences.append(event["transaction"])
        apply_history(
            event["resource_byte_delta"],
            resources,
            resource_credited,
            resource_debited,
            resource_peak,
            resource_names,
            "resource",
        )
        apply_history(
            event["queue_byte_delta"],
            queues,
            queue_credited,
            queue_debited,
            queue_peak,
            queue_names,
            "queue",
        )
    expected_tu_keys = set(expected_tus)
    if set(lifecycle_counts) != expected_tu_keys:
        raise ValueError(f"{path}: event TU set differs from workload manifest")
    required_lifecycle = (
        "release",
        "dispatch",
        "compile-start",
        "compile-finish",
        "transaction-commit",
        "transaction-complete",
    )
    for key in sorted(expected_tu_keys):
        counts = lifecycle_counts[key]
        for name in required_lifecycle:
            if counts.get(name, 0) != 1:
                raise ValueError(
                    f"{path}: TU {key} has {counts.get(name, 0)} {name} events"
                )
        expected_route_bindings = 1 if expected_binding == "release-time-static" else 0
        if counts.get("route-bound", 0) != expected_route_bindings:
            raise ValueError(
                f"{path}: TU {key} route-binding count differs from routing mode"
            )
    if (
        set(release_times) != expected_tu_keys
        or set(completion_times) != expected_tu_keys
    ):
        raise ValueError(f"{path}: TU release/completion cardinality differs")

    for job in manifest["workload"]["jobs"]:
        workload = job["id"]
        mode = job["build_release"]["mode"]
        previous_finish: int | None = None
        for build in range(job["build_epochs"]):
            build_keys = {
                key
                for key in expected_tu_keys
                if key[0] == workload and key[1] == build
            }
            if mode == "concurrent":
                boundary = 0
            elif mode == "fixed-interval":
                boundary = build * job["build_release"]["interval_ns"]
            elif build == 0:
                boundary = 0
            else:
                assert previous_finish is not None
                boundary = previous_finish + job["build_release"].get("gap_ns", 0)
            if {release_times[key] for key in build_keys} != {boundary}:
                raise ValueError(
                    f"{path}: v2 TU release is not at build {workload}/{build} boundary"
                )
            previous_finish = max(completion_times[key] for key in build_keys)

    if dispatch_transaction_sequences != list(range(len(expected_tu_keys))):
        raise ValueError(f"{path}: dispatch transaction sequence is not contiguous")
    if (
        len(transaction_identities) != len(expected_tu_keys)
        or len(transaction_digests) != len(expected_tu_keys)
        or set(route_identities) != set(dispatches_by_route)
    ):
        raise ValueError(f"{path}: transaction/relationship cardinality differs")
    for environment, values in dispatch_tu_sequences_by_environment.items():
        if sorted(values) != list(range(len(values))):
            raise ValueError(f"{path}: C{environment} TU_SEQ set is not contiguous")
    for route, values in dispatch_rel_sequences_by_route.items():
        if values != list(range(len(values))):
            raise ValueError(
                f"{path}: relationship {route} REL_SEQ order is not contiguous"
            )

    if trace_entries:
        for key, entry in trace_entries.items():
            observed = source_bytes_by_tu[key]
            if (
                observed.get("c_to_f", 0) != entry.c_to_f_bytes
                or observed.get("f_to_c", 0) != entry.f_to_c_bytes
            ):
                raise ValueError(
                    f"{path}: TU source bytes differ from retained route trace"
                )

    expected_job_count = len(expected_tu_keys)
    expected_build_count = sum(
        job["build_epochs"] for job in manifest["workload"]["jobs"]
    )
    expected_raw_bytes = sum(
        int(details["content"]["raw_bytes"]) for details in expected_tus.values()
    )
    expected_compile_ns = sum(
        int(details["content"]["compile_ns"]) for details in expected_tus.values()
    )
    derived_counts = {
        "jobs": expected_job_count,
        "build_epochs": expected_build_count,
        "cold_builds": len(manifest["workload"]["jobs"]),
        "warm_builds": expected_build_count - len(manifest["workload"]["jobs"]),
        "environments": manifest["topology"]["c_count"],
        "workers": manifest["topology"]["f_count"],
        "slots_per_worker": manifest["topology"]["f_slots"],
        "input_staging_slots_per_worker": manifest["topology"].get(
            "input_staging_slots", manifest["topology"]["f_slots"]
        ),
        "total_worker_slots": manifest["topology"]["f_count"]
        * manifest["topology"]["f_slots"],
        "raw_bytes": expected_raw_bytes,
        "compiler_work_ns": expected_compile_ns,
    }
    for name, expected_value in derived_counts.items():
        if final_summary.get(name) != expected_value:
            raise ValueError(f"{path}: final {name} differs from manifest/events")

    expected_assignment_counts = [
        {
            "environment": environment,
            "workload": workload,
            "worker": worker,
            "tus": count,
        }
        for (environment, workload, worker), count in sorted(assignment_counts.items())
    ]
    if (
        "assignment_counts" in routing
        and routing.get("assignment_counts") != expected_assignment_counts
    ):
        raise ValueError(f"{path}: routing assignment counts differ from events")
    if (
        manifest["topology"]["assignment_source"] == "route_trace"
        and "assignment_counts" not in routing
    ):
        raise ValueError(f"{path}: exact routing assignment counts are absent")
    if any(resources.values()) or any(queues.values()):
        raise ValueError(f"{path}: resource or queue byte ledger remains open")
    ledger = final_summary.get("exact_byte_ledger", {})
    if not isinstance(ledger, dict) or ledger.get("closure") != "pass":
        raise ValueError(f"{path}: exact byte ledger does not claim closure")
    directions = ledger.get("directions", {})
    direction_totals: dict[str, int] = {}
    for direction in DIRECTIONS:
        accounts = {
            name: count
            for name, count in sorted(directional[direction].items())
            if count
        }
        total = sum(accounts.values())
        if directions.get(direction) != {
            "accounts": accounts,
            "total_bytes": total,
        }:
            raise ValueError(f"{path}: {direction} account summary does not reconcile")
        direction_totals[direction] = total
    expected_route_rows = [
        {
            "direction": direction,
            "environment": environment,
            "worker": worker,
            "account": account,
            "bytes": byte_count,
        }
        for (direction, environment, worker, account), byte_count in sorted(
            directional_by_route.items()
        )
        if byte_count
    ]
    if ledger.get("routes") != expected_route_rows:
        raise ValueError(f"{path}: per-route directional ledger does not reconcile")

    def expected_balance_rows(
        names: set[str],
        balances: Mapping[str, int],
        credited: Mapping[str, int],
        debited: Mapping[str, int],
        peaks: Mapping[str, int],
    ) -> list[dict[str, object]]:
        return [
            {
                "name": name,
                "credited_bytes": credited.get(name, 0),
                "debited_bytes": debited.get(name, 0),
                "peak_bytes": peaks.get(name, 0),
                "final_bytes": balances.get(name, 0),
            }
            for name in sorted(names)
        ]

    if ledger.get("resources") != expected_balance_rows(
        resource_names,
        resources,
        resource_credited,
        resource_debited,
        resource_peak,
    ):
        raise ValueError(f"{path}: resource history summary does not reconcile")
    if ledger.get("queues") != expected_balance_rows(
        queue_names, queues, queue_credited, queue_debited, queue_peak
    ):
        raise ValueError(f"{path}: queue history summary does not reconcile")

    source_c_to_f = directional["c_to_f"].get("source", 0)
    source_f_to_c = directional["f_to_c"].get("source", 0)
    environment_c_to_f = directional["c_to_f"].get("environment", 0)
    if final_summary.get("c_to_f_bytes") != source_c_to_f:
        raise ValueError(f"{path}: source C-to-F summary differs")
    if final_summary.get("f_to_c_bytes") != source_f_to_c:
        raise ValueError(f"{path}: source F-to-C summary differs")
    if final_summary.get("scored_outgoing_bytes") != source_c_to_f:
        raise ValueError(f"{path}: scored outgoing bytes differ")
    if final_summary.get("environment_c_to_f_bytes") != environment_c_to_f:
        raise ValueError(f"{path}: environment C-to-F summary differs")
    if final_summary.get("network_c_to_f_bytes") != direction_totals["c_to_f"]:
        raise ValueError(f"{path}: network C-to-F summary differs")
    if final_summary.get("network_f_to_c_bytes") != direction_totals["f_to_c"]:
        raise ValueError(f"{path}: network F-to-C summary differs")

    replay = final_summary.get("replay_closure", {})
    if replay.get("status") != "pass":
        raise ValueError(f"{path}: route replay did not close")
    if replay.get("semantics") == "exact-route":
        replay_routes = replay.get("routes")
        if not isinstance(replay_routes, list):
            raise ValueError(f"{path}: exact replay routes are absent")
        trace_routes: dict[tuple[int, int], list[RouteTraceEntry]] = defaultdict(list)
        for key, entry in trace_entries.items():
            environment = int(expected_tus[key]["environment"])
            trace_routes[(environment, entry.worker)].append(entry)
        seen_routes: set[tuple[int, int]] = set()
        replay_c_to_f = 0
        replay_f_to_c = 0
        for row in replay_routes:
            route = (row["environment"], row["worker"])
            if route in seen_routes:
                raise ValueError(f"{path}: exact replay repeats a route")
            seen_routes.add(route)
            identity = route_identities.get(route)
            if (
                identity is None
                or tuple(
                    row[name]
                    for name in (
                        "C_STORE_GUID",
                        "physical_endpoint",
                        "RouteLaneId",
                        "F_STORE_GUID",
                        "session_serial",
                        "HISTORY_NONCE",
                    )
                )
                != identity
            ):
                raise ValueError(f"{path}: exact replay route identity differs")
            retained_entries = trace_routes.get(route, [])
            if not retained_entries:
                raise ValueError(f"{path}: exact replay route is absent from trace")
            retained = retained_entries[0]
            retained_identity = (
                retained.c_store_guid,
                retained.physical_endpoint,
                retained.route_lane_id,
                retained.f_store_guid,
                retained.session_serial,
                retained.history_nonce,
            )
            if (
                tuple(
                    row[name]
                    for name in (
                        "C_STORE_GUID",
                        "physical_endpoint",
                        "RouteLaneId",
                        "F_STORE_GUID",
                        "session_serial",
                        "HISTORY_NONCE",
                    )
                )
                != retained_identity
            ):
                raise ValueError(
                    f"{path}: exact replay summary identity differs from route trace"
                )
            accounts = {
                account: {
                    "c_to_f_bytes": directional_by_route.get(
                        ("c_to_f", route[0], route[1], account), 0
                    ),
                    "f_to_c_bytes": directional_by_route.get(
                        ("f_to_c", route[0], route[1], account), 0
                    ),
                }
                for account in ("source", "environment", "result")
                if directional_by_route.get(("c_to_f", route[0], route[1], account), 0)
                or directional_by_route.get(("f_to_c", route[0], route[1], account), 0)
            }
            route_c_to_f = sum(value["c_to_f_bytes"] for value in accounts.values())
            route_f_to_c = sum(value["f_to_c_bytes"] for value in accounts.values())
            if row.get("accounts") != accounts:
                raise ValueError(f"{path}: exact replay route accounts differ")
            retained_c_to_f = sum(entry.c_to_f_bytes for entry in retained_entries)
            retained_f_to_c = sum(entry.f_to_c_bytes for entry in retained_entries)
            if accounts.get("source", {"c_to_f_bytes": 0, "f_to_c_bytes": 0}) != {
                "c_to_f_bytes": retained_c_to_f,
                "f_to_c_bytes": retained_f_to_c,
            }:
                raise ValueError(
                    f"{path}: exact replay source totals differ from route trace"
                )
            if (
                row.get("c_to_f_bytes") != route_c_to_f
                or row.get("f_to_c_bytes") != route_f_to_c
            ):
                raise ValueError(f"{path}: exact replay route totals differ")
            if row.get("tus") != dispatches_by_route.get(route, 0) or row.get(
                "tus"
            ) != len(retained_entries):
                raise ValueError(f"{path}: exact replay route TU count differs")
            replay_c_to_f += route_c_to_f
            replay_f_to_c += route_f_to_c
        if seen_routes != set(dispatches_by_route) or seen_routes != set(trace_routes):
            raise ValueError(f"{path}: exact replay route set differs")
        if (
            replay_c_to_f != direction_totals["c_to_f"]
            or replay_f_to_c != direction_totals["f_to_c"]
        ):
            raise ValueError(f"{path}: exact replay aggregate totals differ")
    elif replay.get("semantics") == "aggregate-directional":
        if (
            replay.get("source_c_to_f_bytes") != source_c_to_f
            or replay.get("source_f_to_c_bytes") != source_f_to_c
        ):
            raise ValueError(f"{path}: policy replay source totals differ")
        if (
            replay.get("c_to_f_bytes") != direction_totals["c_to_f"]
            or replay.get("f_to_c_bytes") != direction_totals["f_to_c"]
        ):
            raise ValueError(f"{path}: policy replay directional totals differ")
    else:
        raise ValueError(f"{path}: replay closure semantics are unknown")
    return {
        "events": len(events),
        "c_to_f_bytes": direction_totals["c_to_f"],
        "f_to_c_bytes": direction_totals["f_to_c"],
    }


def write_result(
    scenario: LoadedScenario,
    result: SimulationResult,
    output_directory: Path,
    execution: dict[str, object] | None = None,
) -> None:
    output_directory.mkdir(parents=True, exist_ok=True)
    resolved_document = resolved_scenario_document(scenario)
    (output_directory / "resolved-scenario.json").write_text(
        json.dumps(resolved_document, indent=2) + "\n"
    )
    (output_directory / "summary.json").write_text(
        json.dumps(result.summary, indent=2) + "\n"
    )
    if scenario.is_v2:
        if execution is None:
            raise ValueError("v2 result requires execution metadata")
        (output_directory / "execution-header.json").write_text(
            json.dumps(execution, indent=2, sort_keys=True) + "\n"
        )
    write_tsv(output_directory / "assignments.tsv", result.assignments)
    write_tsv(output_directory / "events.tsv", result.events)
    write_tsv(output_directory / "workers.tsv", result.workers)
    write_tsv(output_directory / "builds.tsv", result.builds)
    write_tsv(output_directory / "generations.tsv", result.generations)
    if scenario.is_v2:
        retained_route_trace = output_directory / "route-trace.jsonl"
        if scenario.route_trace_path is not None:
            retained_route_trace.write_bytes(scenario.route_trace_path.read_bytes())
        else:
            write_route_trace(retained_route_trace, scenario, result)
    descriptor, timeline, final = write_experiment_stream(
        output_directory / "experiment.jsonl", scenario, result, execution
    )
    (output_directory / "report.html").write_text(
        render_report_html(descriptor, timeline, final)
    )
    result.timeline.discard_spool()
    result.events.discard_spool()
    if scenario.is_v2:
        validate_experiment_jsonl(output_directory / "experiment.jsonl")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("scenario", type=Path)
    parser.add_argument(
        "--codec", choices=("compile-only", "raw", "p29", "grz"), required=True
    )
    parser.add_argument(
        "--ledger",
        type=Path,
        help="required physical-ledger JSONL for p29/grz",
    )
    parser.add_argument(
        "--allow-compatible-ledger",
        action="store_true",
        help=(
            "allow a physical ledger from a different scenario file; exact payload digests "
            "and runtime worker/TU_SEQ/REL_SEQ order are still required"
        ),
    )
    parser.add_argument(
        "--corpus-root", action="append", default=[], metavar="WORKLOAD=PATH"
    )
    parser.add_argument(
        "--execution",
        type=Path,
        help="separate icecream-execution-v2 header (required by v2 manifests)",
    )
    parser.add_argument("--require-payload", action="store_true")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    scenario = load_scenario(args.scenario, payload_overrides(args.corpus_root))
    execution: dict[str, object] | None = None
    if scenario.is_v2:
        if args.execution is None:
            raise ValueError("icecream-experiment-v2 requires --execution")
        execution = load_execution(args.execution, scenario)
        capabilities = scenario.manifest["capabilities"]
        assert isinstance(capabilities, dict)
        control = capabilities.get("experiment_control")
        expected_adapter = selected_v2_adapter(scenario.manifest)
        if expected_adapter is None:
            raise ValueError(
                f"experiment control {control!r} is a declared future control, not an R4 codec"
            )
        if args.codec != expected_adapter:
            raise ValueError(
                f"manifest selects adapter {expected_adapter!r}, command selected {args.codec!r}"
            )
    elif args.execution is not None:
        raise ValueError("--execution is valid only with icecream-experiment-v2")
    if args.require_payload:
        missing = [
            str(item.payload)
            for items in scenario.work_items.values()
            for item in items
            if not item.payload.is_file()
        ]
        if missing:
            raise ValueError(
                f"{len(missing)} payloads are absent; first is {missing[0]}"
            )
    if args.codec == "compile-only":
        if args.ledger is not None or args.allow_compatible_ledger:
            raise ValueError("compile-only does not accept physical-ledger options")
        adapter: CodecAdapter = CompileOnlyAdapter()
    elif args.codec == "raw":
        if args.ledger is not None or args.allow_compatible_ledger:
            raise ValueError("raw does not accept physical-ledger options")
        adapter = RawAdapter()
    else:
        if args.ledger is None:
            raise ValueError(f"{args.codec} requires --ledger")
        adapter = PhysicalLedgerAdapter(
            args.ledger,
            scenario,
            args.codec,
            allow_compatible_scenario=args.allow_compatible_ledger,
            assignment_closure=(
                "exact_route"
                if scenario.route_trace
                else "aggregate" if scenario.is_v2 else "exact_route"
            ),
        )
    args.out.mkdir(parents=True, exist_ok=True)
    result = Simulator(
        scenario,
        adapter,
        timeline_spool_path=args.out / ".timeline-spool.jsonl",
        event_spool_path=args.out / ".event-spool.jsonl",
    ).run()
    write_result(scenario, result, args.out, execution)
    print(json.dumps(result.summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
