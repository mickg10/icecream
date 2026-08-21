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
from collections import defaultdict, deque
from dataclasses import dataclass, field
from fractions import Fraction
from pathlib import Path
from typing import Iterable, Iterator, Sequence


NANOSECONDS = 1_000_000_000
DIRECTIONS = ("c_to_f", "f_to_c")
DEFAULT_SNAPSHOT_NS = 10_000_000
MAX_REPORT_SNAPSHOTS = 2_000


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

    def __post_init__(self) -> None:
        if self.direction not in DIRECTIONS:
            raise ValueError(f"unknown phase direction {self.direction!r}")
        if self.byte_count < 0:
            raise ValueError("phase byte count is negative")
        checked_nonnegative_int(self.priority, "phase priority")


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
        if len(initial) != len(self.initial_tokens) or any(not token for token in initial):
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
                raise ValueError(f"transaction DAG {label} has unknown tokens {unknown}")
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

    def transaction_plan(
        self, item: WorkItem, worker: int
    ) -> TransactionPlan | None:
        """Return a fork/join plan, or ``None`` to use the linear phase adapter."""
        del item, worker
        return None

    def commit(self, item: WorkItem, worker: int) -> None:
        """Commit state only after the complete dialogue has finished."""

    def bind_route(
        self, item: WorkItem, worker: int, tu_seq: int, rel_seq: int
    ) -> None:
        """Validate the global prepared identity and ordered F projection."""
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

    def timeline_f_state(
        self, environment: int, worker: int
    ) -> dict[str, object]:
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
        payload_digest_cache: dict[
            Path, tuple[tuple[int, int, int, int, int], str]
        ]
        | None = None,
    ):
        self.path = path.resolve()
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
        if not isinstance(reconstruction, dict) or reconstruction.get("status") != "pass":
            raise ValueError(f"{self.path}: reconstruction result is not pass")
        ledger_scenario_sha256 = descriptor.get("scenario_sha256")
        replay_scenario_sha256 = sha256(scenario.path)
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
        payload_digests: dict[
            Path, tuple[tuple[int, int, int, int, int], str]
        ] = (
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
            if (
                scenario_key not in scenario.work_items
                or logical >= len(scenario.work_items[scenario_key])
            ):
                raise ValueError(
                    f"{self.path}:{row_number}: TU identity is outside the scenario"
                )
            item = scenario.work_items[scenario_key][logical]
            worker = checked_nonnegative_int(row.get("worker"), "ledger worker")
            if worker >= int(scenario.document["workers"]["f_count"]):
                raise ValueError(f"{self.path}:{row_number}: worker is outside scenario")
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
                raise ValueError(f"{self.path}:{row_number}: raw_sha256 is not canonical")
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
                    dependencies = () if previous is None else (f"{previous}:delivered",)
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
                raise ValueError(f"{self.path}:{row_number}: state_after is not an object")
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
                raise ValueError(f"{self.path}:{row_number}: repeated TU identity {key}")
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
            item.key: item
            for items in scenario.work_items.values()
            for item in items
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
                raise ValueError(f"{self.path}: payload is absent for {key}: {item.payload}")
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
        if entry.worker != worker:
            raise RuntimeError(
                f"physical ledger assigned {item.key} to F{entry.worker}, simulator chose F{worker}"
            )
        route = (item.environment, worker)
        if entry.route_sequence != self.committed_by_route[route]:
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

    def transaction_plan(
        self, item: WorkItem, worker: int
    ) -> TransactionPlan | None:
        return self._activate(item, worker).plan

    def commit(self, item: WorkItem, worker: int) -> None:
        if item.key not in self.active:
            raise RuntimeError(f"physical ledger TU {item.key} committed without begin")
        entry = self.entries[item.key]
        route = (item.environment, worker)
        if entry.worker != worker or entry.route_sequence != self.committed_by_route[route]:
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
        if entry.worker != worker:
            return 1 << 62
        return sum(
            phase.byte_count for phase in entry.phases if phase.direction == "c_to_f"
        )

    def timeline_c_state(self, environment: int) -> dict[str, object]:
        return {
            "committed_tus": self.committed_by_environment[environment],
            "committed_c_to_f_bytes": self.committed_bytes_by_environment[environment],
        }

    def timeline_f_state(
        self, environment: int, worker: int
    ) -> dict[str, object]:
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
            "ledger": str(self.path),
            "ledger_sha256": sha256(self.path),
            "scenario_binding": self.scenario_binding,
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
    dialogue_started: bool = False
    plan: TransactionPlan | None = None
    dag_tokens: set[str] = field(default_factory=set)
    dag_started: set[str] = field(default_factory=set)
    input_ready: bool = False
    transaction_committed: bool = False
    compile_completed: bool = False

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
    path: Path
    work_items: dict[tuple[str, int], list[WorkItem]]
    workload_config: dict[str, dict[str, object]]
    workload_inputs: dict[str, dict[str, object]]


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
                "average_reserved_slots": float(
                    self.reserved_slot_ns[worker] / duration
                ),
                "average_staging_slots": float(
                    self.staging_slot_ns[worker] / duration
                ),
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
                "wall_start_ns": ceil_fraction(self.sample_wall_start_ns),
                "wall_end_ns": ceil_fraction(simulator.now),
                "wall_duration_ns": ceil_fraction(
                    simulator.now - self.sample_wall_start_ns
                ),
                "active_start_ns": ceil_fraction(self.sample_active_start_ns),
                "active_end_ns": ceil_fraction(self.active_ns),
                "active_duration_ns": ceil_fraction(duration),
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
        elif simulator.release_heap:
            reason = "workload-release"
        else:
            reason = "timer"
        self._record(
            {
                "record": "gap",
                "sequence": self.sequence,
                "wall_start_ns": ceil_fraction(start_ns),
                "wall_end_ns": ceil_fraction(end_ns),
                "wall_duration_ns": ceil_fraction(end_ns - start_ns),
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


def read_trace(path: Path) -> list[TraceRow]:
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
            rows.append(
                TraceRow(
                    logical,
                    row["job_id"],
                    row["ii_relative"],
                    raw_bytes,
                    compile_ns,
                    row["compile_model"],
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


def load_scenario(
    path: Path, payload_overrides: dict[str, Path] | None = None
) -> LoadedScenario:
    path = path.resolve()
    document = json.loads(path.read_text())
    if (
        not isinstance(document, dict)
        or document.get("schema") != "icecream-distribution-scenario-v1"
    ):
        raise ValueError(f"{path}: unknown scenario schema")
    environments = document.get("environments")
    workers = document.get("workers")
    network = document.get("network")
    scheduler = document.get("scheduler")
    if not all(
        isinstance(value, dict) for value in (environments, workers, network, scheduler)
    ):
        raise ValueError(f"{path}: scenario lacks a required object")
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
        "fabric_bits_per_second" not in network[direction]
        for direction in DIRECTIONS
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
        trace_path = (path.parent / trace_value).resolve()
        corpus_root = overrides.get(workload, Path(root_value))
        if not corpus_root.is_absolute():
            corpus_root = (path.parent / corpus_root).resolve()
        trace_rows = read_trace(trace_path)
        workload_inputs[workload] = {
            "trace": str(trace_path),
            "trace_sha256": sha256(trace_path),
            "corpus_root": str(corpus_root),
            "trace_rows": len(trace_rows),
        }
        expected_model = template.get("compile_profile")
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
                items.append(
                    WorkItem(
                        next_ordinal,
                        environment,
                        workload,
                        build,
                        row.logical,
                        row.job_id,
                        corpus_root / row.ii_relative,
                        row.raw_bytes,
                        row.compile_ns,
                        release_offset,
                    )
                )
                next_ordinal += 1
            work_items[(workload, build)] = items
        workload_config[workload] = {**job, "start_ns": start_ns, "builds": builds}
    if f_count < 1:
        raise AssertionError("validated f_count became empty")
    return LoadedScenario(document, path, work_items, workload_config, workload_inputs)


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
        self.decoupled_staging = (
            "input_staging_slots" in worker_config["template"]
        )
        self.input_staging_slots_per_f = int(
            worker_config["template"].get("input_staging_slots", self.slots_per_f)
        )
        self.network = document["network"]
        self.scheduler = document["scheduler"]
        self.now = Fraction(0)
        self.release_heap: list[tuple[int, int, WorkItem]] = []
        self.ready_heap: list[tuple[tuple[int, ...], int, WorkItem]] = []
        self.ready_by_environment: dict[int, deque[WorkItem]] = defaultdict(deque)
        self.ready_environment_cycle: deque[int] = deque()
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
        self.delivery_heap: list[tuple[Fraction, int, Flow]] = []
        self.active_flows: dict[int, Flow] = {}
        self.endpoint_queues: dict[
            tuple[str, int, int], list[tuple[int, int, Flow]]
        ] = defaultdict(
            list
        )
        self.endpoint_priority_overtakes: dict[
            tuple[str, int, int], int
        ] = defaultdict(int)
        self.dialogue_active: dict[tuple[int, int], int] = defaultdict(int)
        self.dialogue_queues: dict[tuple[int, int], deque[Transaction]] = defaultdict(
            deque
        )
        self.relationship_next_assigned: dict[tuple[int, int], int] = defaultdict(int)
        self.relationship_next_committed: dict[tuple[int, int], int] = defaultdict(int)
        self.transactions: list[Transaction] = []
        self.events = EventRecorder(event_spool_path)
        self.event_sequence = 0
        self.flow_sequence = 0
        self.transaction_sequence = 0
        self.environment_next_tu_seq = [0] * self.env_count
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
        self.worker_dispatched = [0] * self.f_count
        self.worker_completed = [0] * self.f_count
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

    def _event(
        self,
        name: str,
        tx: Transaction | None = None,
        phase: Phase | None = None,
        item: WorkItem | None = None,
        flow: Flow | None = None,
        detail: str = "",
    ) -> None:
        item = tx.item if tx is not None else item
        self.events.append(
            {
                "sequence": self.event_sequence,
                "time_ns": ceil_fraction(self.now),
                "event": name,
                "environment": "" if item is None else item.environment,
                "workload": "" if item is None else item.workload,
                "build": "" if item is None else item.build,
                "logical": "" if item is None else item.logical,
                "worker": "" if tx is None else tx.worker,
                "slot": "" if tx is None else tx.slot,
                "staging_slot": "" if tx is None else tx.staging_slot,
                "compiler_slot": "" if tx is None else tx.compiler_slot,
                "transaction": "" if tx is None else tx.sequence,
                "tu_seq": "" if tx is None else tx.tu_seq,
                "rel_seq": "" if tx is None else tx.rel_seq,
                "flow": "" if flow is None else flow.sequence,
                "phase": "" if phase is None else phase.name,
                "direction": "" if phase is None else phase.direction,
                "bytes": "" if phase is None else phase.byte_count,
                "detail": detail,
            }
        )
        self.event_sequence += 1

    def _release_ready(self) -> None:
        while self.release_heap and self.release_heap[0][0] <= self.now:
            _, _, item = heapq.heappop(self.release_heap)
            self.env_unreleased[item.environment] -= 1
            self.env_ready[item.environment] += 1
            self._event("release", item=item)
            policy = self.scheduler["ready_job_policy"]
            if policy == "environment-round-robin":
                queue = self.ready_by_environment[item.environment]
                queue.append(item)
                if item.environment not in self.ready_environment_members:
                    self.ready_environment_cycle.append(item.environment)
                    self.ready_environment_members.add(item.environment)
                continue
            if policy == "fifo-release":
                key = (int(item.release_ns), item.ordinal)
            elif policy == "shortest-known":
                key = (item.compile_ns, int(item.release_ns), item.ordinal)
            elif policy == "trace":
                key = (item.ordinal,)
            else:
                raise AssertionError(f"unvalidated ready-job policy {policy!r}")
            heapq.heappush(self.ready_heap, (key, item.ordinal, item))

    def _has_ready(self) -> bool:
        if self.scheduler["ready_job_policy"] == "environment-round-robin":
            return bool(self.ready_environment_cycle)
        return bool(self.ready_heap)

    def _pop_ready(self) -> WorkItem:
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

    def _free_workers(self) -> list[int]:
        pool = self.staging_slots if self.decoupled_staging else self.compiler_slots
        return [worker for worker in range(self.f_count) if pool[worker]]

    def _reserve_worker(self, worker: int) -> tuple[int, int, int | None]:
        staging_slot = self.staging_slots[worker].acquire()
        compiler_slot = (
            None
            if self.decoupled_staging
            else self.compiler_slots[worker].acquire()
        )
        assignment_slot = (
            staging_slot if compiler_slot is None else compiler_slot
        )
        return assignment_slot, staging_slot, compiler_slot

    def _select_slot(self, item: WorkItem) -> tuple[int, int, int, int | None]:
        free_workers = self._free_workers()
        if not free_workers:
            raise RuntimeError("slot selection without a free worker")
        policy = self.scheduler["placement_policy"]
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
        while self._has_ready() and self._free_workers():
            item = self._pop_ready()
            worker, slot, staging_slot, compiler_slot = self._select_slot(item)
            route = (item.environment, worker)
            tu_seq = self.environment_next_tu_seq[item.environment]
            self.environment_next_tu_seq[item.environment] += 1
            rel_seq = self.relationship_next_assigned[route]
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
            self.adapter.bind_route(item, worker, tx.tu_seq, tx.rel_seq)
            self.transaction_sequence += 1
            self.transactions.append(tx)
            self.worker_raw_bytes[worker] += item.raw_bytes
            self.env_active[item.environment] += 1
            self.worker_input_wait[worker] += 1
            self.worker_dispatched[worker] += 1
            self._event("dispatch", tx)
            self._queue_or_start_dialogue(tx)

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
        self._event("input-ready", tx)
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
        self._event("compile-start", tx)
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
            raise RuntimeError("transaction completed while an F resource remained reserved")
        if tx.complete_ns is not None:
            raise RuntimeError("transaction completed twice")
        tx.complete_ns = self.now
        self._event("transaction-complete", tx)
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
            if config["build_release"][
                "mode"
            ] == "after-previous" and next_build < int(config["builds"]):
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
        if phase.direction == "c_to_f":
            tx.c_to_f_bytes += phase.byte_count
            self.worker_c_to_f[tx.worker] += phase.byte_count
        else:
            tx.f_to_c_bytes += phase.byte_count
            self.worker_f_to_c[tx.worker] += phase.byte_count
        flow = Flow(self.flow_sequence, tx, phase, Fraction(phase.byte_count * 8))
        self.flow_sequence += 1
        if phase.byte_count == 0:
            self._event("flow-queued", tx, phase, flow=flow)
            self._event("flow-start", tx, phase, flow=flow)
            latency = int(self.network[phase.direction]["one_way_latency_ns"])
            self._event("flow-sent", tx, phase, flow=flow)
            if latency:
                heapq.heappush(
                    self.delivery_heap, (self.now + latency, flow.sequence, flow)
                )
            else:
                self._event("flow-finish", tx, phase, flow=flow)
                if tx.plan is None:
                    tx.phase_index += 1
                    self._start_next_phase(tx)
                else:
                    self._dag_token(tx, f"{phase.name}:sent")
                    self._dag_token(tx, f"{phase.name}:delivered")
            return
        self._event("flow-queued", tx, phase, flow=flow)
        heapq.heappush(
            self.endpoint_queues[flow.endpoint],
            (phase.priority, flow.sequence, flow),
        )

    def _release_dag_nodes(self, tx: Transaction) -> None:
        if tx.plan is None:
            raise RuntimeError("DAG release requested for a linear transaction")
        for node in tx.plan.nodes:
            if node.name in tx.dag_started or not set(node.dependencies) <= tx.dag_tokens:
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
        if (
            not tx.transaction_committed
            and set(tx.plan.commit_after) <= tx.dag_tokens
        ):
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

    def _pop_endpoint_flow(
        self, endpoint: tuple[str, int, int]
    ) -> tuple[Flow, bool]:
        """Select by priority while bounding how often the oldest flow is overtaken."""
        queue = self.endpoint_queues[endpoint]
        if not queue:
            raise RuntimeError("endpoint selection from an empty queue")
        oldest_index = min(range(len(queue)), key=lambda index: queue[index][1])
        limit = int(
            self.network[endpoint[0]].get("max_priority_burst_quanta", 8)
        )
        forced_oldest = (
            oldest_index != 0
            and self.endpoint_priority_overtakes[endpoint] >= limit
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
                    Fraction(
                        int(link["per_environment_bits_per_second"]), NANOSECONDS
                    ),
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
                flow.quantum_remaining_bits
                if flow.quantum_remaining_bits is not None
                else flow.remaining_bits,
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
            self._event(
                "flow-sent", flow.transaction, flow.phase, flow=flow
            )
            if flow.transaction.plan is not None:
                self._dag_token(
                    flow.transaction, f"{flow.phase.name}:sent"
                )
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
            self._event(
                "flow-finish", flow.transaction, flow.phase, flow=flow
            )
            if flow.transaction.plan is None:
                flow.transaction.phase_index += 1
                self._start_next_phase(flow.transaction)
            else:
                self._dag_token(
                    flow.transaction, f"{flow.phase.name}:delivered"
                )

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
        network_finish = self._network_finish_time()
        if network_finish is not None:
            candidates.append(network_finish)
        future = [candidate for candidate in candidates if candidate >= self.now]
        if not future:
            raise RuntimeError(
                f"simulation stalled after {self.completed}/{self.total_items} jobs"
            )
        return min(future)

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
                            "committed_rel_seq": self.relationship_next_committed[route],
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
                        self.env_active[environment]
                        + self.env_completed[environment]
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
                    "dispatched_tus": self.worker_dispatched[worker],
                    "completed_tus": self.worker_completed[worker],
                    "active_flows": sum(
                        value
                        for (direction, environment, route_worker), value in active_by_route.items()
                        if route_worker == worker
                    ),
                    "queued_flows": sum(
                        value
                        for (direction, environment, route_worker), value in queued_by_route.items()
                        if route_worker == worker
                    ),
                    "in_propagation": sum(
                        value
                        for (direction, environment, route_worker), value in delivery_by_route.items()
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

        return {
            "scheduler": {
                "unreleased_tus": sum(self.env_unreleased),
                "ready_tus": sum(self.env_ready),
                "active_tus": sum(self.env_active),
                "completed_tus": self.completed,
                "total_tus": self.total_items,
            },
            "c": c_state,
            "f": f_state,
            "network": {
                "active_flows": len(self.active_flows),
                "queued_flows": sum(len(queue) for queue in self.endpoint_queues.values()),
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
            "ready_environment_cycle": len(self.ready_environment_cycle),
            "ready_compiler_queues": sum(
                len(queue) for queue in self.ready_compiler_queues.values()
            ),
            "active_flows": len(self.active_flows),
            "queued_flows": sum(len(queue) for queue in self.endpoint_queues.values()),
            "deliveries": len(self.delivery_heap),
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
        if self.completed != self.total_items or sum(self.env_completed) != self.total_items:
            raise RuntimeError(
                "completed simulation counters differ from the scenario TU count"
            )

    def _assert_projection_order(self) -> None:
        """Require every F relationship to be an order-preserving C projection."""
        by_environment: dict[int, list[Transaction]] = defaultdict(list)
        by_relationship: dict[tuple[int, int], list[Transaction]] = defaultdict(list)
        for tx in sorted(self.transactions, key=lambda value: value.sequence):
            by_environment[tx.item.environment].append(tx)
            by_relationship[(tx.item.environment, tx.worker)].append(tx)
        for environment, transactions in by_environment.items():
            observed = [tx.tu_seq for tx in transactions]
            if observed != list(range(len(transactions))):
                raise RuntimeError(
                    f"C{environment} TU_SEQ admission order is not contiguous: {observed[:3]}"
                )
        for route, transactions in by_relationship.items():
            rel_seq = [tx.rel_seq for tx in transactions]
            tu_seq = [tx.tu_seq for tx in transactions]
            if rel_seq != list(range(len(transactions))):
                raise RuntimeError(
                    f"relationship {route} REL_SEQ projection is not contiguous"
                )
            if tu_seq != sorted(tu_seq):
                raise RuntimeError(
                    f"relationship {route} changed the C admission order"
                )
            if self.relationship_next_assigned[route] != len(transactions):
                raise RuntimeError(
                    f"relationship {route} assignment cursor differs from its projection"
                )
            if (
                self.adapter.dialogue_window_per_route() is not None
                and self.relationship_next_committed[route] != len(transactions)
            ):
                raise RuntimeError(
                    f"relationship {route} commit cursor differs from its projection"
                )

    def _capacity_floors(
        self, transactions: Sequence[Transaction]
    ) -> dict[str, int]:
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
                byte_floor(
                    byte_count, int(c_to_f["per_environment_bits_per_second"])
                )
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
        self._assert_projection_order()

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
                    "transaction_commit_ns": ceil_fraction(
                        tx.transaction_commit_ns
                    ),
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
        summary = {
            "schema": "icecream-distribution-result-v1",
            "scenario": self.scenario.document["name"],
            "scenario_path": str(self.scenario.path),
            "scenario_sha256": sha256(self.scenario.path),
            "workload_inputs": self.scenario.workload_inputs,
            "codec_adapter": self.adapter.name,
            "physical_codec_result": self.adapter.physical,
            "codec_metadata": self.adapter.result_metadata(),
            "dialogue_window_per_route": self.adapter.dialogue_window_per_route(),
            "relationship_ordering": (
                "TU_SEQ is contiguous per C admission order; REL_SEQ is the contiguous "
                "order-preserving projection on each (C,F) relationship"
            ),
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
            "scored_outgoing_bytes": c_to_f,
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
                    "per-C TU_SEQ admission and contiguous per-F REL_SEQ projections",
                    "C-to-F placement and independent F input-staging/compiler pools",
                    "fork/join dialogue release, serialization, and propagation",
                    "writer priority with bounded serialization quanta",
                    "per-route, per-C, per-F, common-fabric, and directional-fabric max-min allocation",
                    "F input-wait, ready-input, compiler, and commit-wait occupancy",
                    "build barriers and workload release gaps",
                    "exact C-to-F and F-to-C byte events",
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
    scenario: LoadedScenario, result: SimulationResult
) -> dict[str, object]:
    """Build the deterministic descriptor which heads an experiment stream."""
    resolved = resolved_scenario_document(scenario)
    allocation_resources = ["direction/environment/F route"]
    if "shared_fabric_bps" in resolved["network"]:
        allocation_resources.append("optional common fabric")
    if any(
        "fabric_bits_per_second" in resolved["network"][direction]
        for direction in DIRECTIONS
    ):
        allocation_resources.append("per-direction fabric")
    if any(
        "per_environment_bits_per_second" in resolved["network"][direction]
        for direction in DIRECTIONS
    ):
        allocation_resources.append("per-direction C-authority aggregate")
    if any(
        "per_worker_bits_per_second" in resolved["network"][direction]
        for direction in DIRECTIONS
    ):
        allocation_resources.append("per-direction F aggregate")
    descriptor = {
        "record": "experiment",
        "schema": "icecream-distribution-timeline-v1",
        "scenario": result.summary["scenario"],
        "topology": topology_label(resolved),
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
            "snapshot_interval_ns": result.summary[
                "timeline_snapshot_interval_ns"
            ],
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
            "source": str(Path(__file__).resolve()),
            "source_sha256": sha256(Path(__file__).resolve()),
        },
        "expected_summary": result.summary,
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
        record["event_sequence_start"] = (
            "" if not attached else attached[0]["sequence"]
        )
        record["event_sequence_end"] = "" if not attached else attached[-1]["sequence"]
        yield record
    if event is not None or event_count != len(result.events):
        raise RuntimeError(
            f"timeline attached {event_count} of {len(result.events)} engine events"
        )


def experiment_final(result: SimulationResult) -> dict[str, object]:
    return {
        "record": "summary",
        "schema": "icecream-distribution-timeline-summary-v1",
        "timeline_records": len(result.timeline),
        "event_count": len(result.events),
        "summary": result.summary,
    }


def experiment_records(
    scenario: LoadedScenario, result: SimulationResult
) -> tuple[dict[str, object], list[dict[str, object]], dict[str, object]]:
    """Materialize an experiment stream for small callers and focused tests."""
    descriptor = experiment_descriptor(scenario, result)
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
    report_limit: int = MAX_REPORT_SNAPSHOTS,
) -> tuple[dict[str, object], list[dict[str, object]], dict[str, object]]:
    """Write the canonical stream while retaining only the bounded browser view."""
    descriptor = experiment_descriptor(scenario, result)
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
    template = r'''<!doctype html>
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
</script></body></html>'''
    return template.replace("__PAYLOAD__", payload)


def write_result(
    scenario: LoadedScenario, result: SimulationResult, output_directory: Path
) -> None:
    output_directory.mkdir(parents=True, exist_ok=True)
    resolved_document = resolved_scenario_document(scenario)
    (output_directory / "resolved-scenario.json").write_text(
        json.dumps(resolved_document, indent=2) + "\n"
    )
    (output_directory / "summary.json").write_text(
        json.dumps(result.summary, indent=2) + "\n"
    )
    write_tsv(output_directory / "assignments.tsv", result.assignments)
    write_tsv(output_directory / "events.tsv", result.events)
    write_tsv(output_directory / "workers.tsv", result.workers)
    write_tsv(output_directory / "builds.tsv", result.builds)
    write_tsv(output_directory / "generations.tsv", result.generations)
    descriptor, timeline, final = write_experiment_stream(
        output_directory / "experiment.jsonl", scenario, result
    )
    (output_directory / "report.html").write_text(
        render_report_html(descriptor, timeline, final)
    )
    result.timeline.discard_spool()
    result.events.discard_spool()


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
    parser.add_argument("--require-payload", action="store_true")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    scenario = load_scenario(args.scenario, payload_overrides(args.corpus_root))
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
        )
    args.out.mkdir(parents=True, exist_ok=True)
    result = Simulator(
        scenario,
        adapter,
        timeline_spool_path=args.out / ".timeline-spool.jsonl",
        event_spool_path=args.out / ".event-spool.jsonl",
    ).run()
    write_result(scenario, result, args.out)
    print(json.dumps(result.summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
