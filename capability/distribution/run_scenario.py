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
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path
from typing import Iterable, Sequence


NANOSECONDS = 1_000_000_000
DIRECTIONS = ("c_to_f", "f_to_c")


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

    def __post_init__(self) -> None:
        if self.direction not in DIRECTIONS:
            raise ValueError(f"unknown phase direction {self.direction!r}")
        if self.byte_count < 0:
            raise ValueError("phase byte count is negative")


class CodecAdapter:
    """A stateful codec boundary owned by the common simulator."""

    name = "unset"
    physical = False

    def begin(self, item: WorkItem, worker: int) -> Sequence[Phase]:
        raise NotImplementedError

    def commit(self, item: WorkItem, worker: int) -> None:
        """Commit state only after the complete dialogue has finished."""

    def preview_c_to_f(self, item: WorkItem, worker: int) -> int:
        """Tie-break hint; never charged as a physical measurement."""
        del item, worker
        raise NotImplementedError(
            "this adapter does not provide a side-effect-free preview"
        )


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


@dataclass
class Transaction:
    sequence: int
    item: WorkItem
    worker: int
    slot: int
    phases: tuple[Phase, ...]
    phase_index: int = 0
    dispatch_ns: Fraction = Fraction(0)
    transfer_done_ns: Fraction | None = None
    compile_start_ns: Fraction | None = None
    compile_finish_ns: Fraction | None = None
    c_to_f_bytes: int = 0
    f_to_c_bytes: int = 0


@dataclass
class Flow:
    sequence: int
    transaction: Transaction
    phase: Phase
    remaining_bits: Fraction
    start_ns: Fraction | None = None

    @property
    def endpoint(self) -> tuple[str, int, int]:
        return (
            self.phase.direction,
            self.transaction.item.environment,
            self.transaction.worker,
        )


@dataclass
class SimulationResult:
    summary: dict[str, object]
    assignments: list[dict[str, object]]
    events: list[dict[str, object]]
    workers: list[dict[str, object]]
    builds: list[dict[str, object]]
    generations: list[dict[str, object]]


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
    checked_positive_int(network.get("shared_fabric_bps"), "network.shared_fabric_bps")
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
    def __init__(self, scenario: LoadedScenario, adapter: CodecAdapter):
        self.scenario = scenario
        self.adapter = adapter
        document = scenario.document
        worker_config = document["workers"]
        self.f_count = int(worker_config["f_count"])
        self.slots_per_f = int(worker_config["template"]["slots"])
        self.network = document["network"]
        self.scheduler = document["scheduler"]
        self.now = Fraction(0)
        self.release_heap: list[tuple[int, int, WorkItem]] = []
        self.ready_heap: list[tuple[tuple[int, ...], int, WorkItem]] = []
        self.ready_by_environment: dict[int, deque[WorkItem]] = defaultdict(deque)
        self.ready_environment_cycle: deque[int] = deque()
        self.free_slots: dict[int, SparseSlotPool] = {
            worker: SparseSlotPool(self.slots_per_f) for worker in range(self.f_count)
        }
        self.compile_heap: list[tuple[Fraction, int, Transaction]] = []
        self.delivery_heap: list[tuple[Fraction, int, Flow]] = []
        self.active_flows: dict[int, Flow] = {}
        self.endpoint_queues: dict[tuple[str, int, int], deque[Flow]] = defaultdict(
            deque
        )
        self.transactions: list[Transaction] = []
        self.events: list[dict[str, object]] = []
        self.event_sequence = 0
        self.flow_sequence = 0
        self.transaction_sequence = 0
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
        self, name: str, tx: Transaction | None = None, phase: Phase | None = None
    ) -> None:
        item = tx.item if tx is not None else None
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
                "phase": "" if phase is None else phase.name,
                "direction": "" if phase is None else phase.direction,
                "bytes": "" if phase is None else phase.byte_count,
            }
        )
        self.event_sequence += 1

    def _release_ready(self) -> None:
        while self.release_heap and self.release_heap[0][0] <= self.now:
            _, _, item = heapq.heappop(self.release_heap)
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
            return heapq.heappop(self.ready_heap)[2]
        environment = self.ready_environment_cycle.popleft()
        queue = self.ready_by_environment[environment]
        item = queue.popleft()
        if queue:
            self.ready_environment_cycle.append(environment)
        else:
            self.ready_environment_members.remove(environment)
        return item

    def _free_workers(self) -> list[int]:
        return [worker for worker in range(self.f_count) if self.free_slots[worker]]

    def _select_slot(self, item: WorkItem) -> tuple[int, int]:
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
                    return worker, self.free_slots[worker].acquire()
            raise AssertionError("round-robin failed to find a free worker")
        if policy == "fastest":
            worker = min(
                free_workers,
                key=lambda candidate: (
                    self.adapter.preview_c_to_f(item, candidate),
                    candidate,
                ),
            )
            return worker, self.free_slots[worker].acquire()
        raise NotImplementedError(
            f"placement policy {policy!r} needs its stateful adapter in the common core"
        )

    def _dispatch(self) -> None:
        while self._has_ready() and self._free_workers():
            item = self._pop_ready()
            worker, slot = self._select_slot(item)
            phases = tuple(self.adapter.begin(item, worker))
            tx = Transaction(
                self.transaction_sequence,
                item,
                worker,
                slot,
                phases,
                dispatch_ns=self.now,
            )
            self.transaction_sequence += 1
            self.transactions.append(tx)
            self.worker_raw_bytes[worker] += item.raw_bytes
            self._event("dispatch", tx)
            self._start_next_phase(tx)

    def _start_next_phase(self, tx: Transaction) -> None:
        if tx.phase_index == len(tx.phases):
            tx.transfer_done_ns = self.now
            self.adapter.commit(tx.item, tx.worker)
            tx.compile_start_ns = self.now
            tx.compile_finish_ns = self.now + tx.item.compile_ns
            self.worker_compile_ns[tx.worker] += tx.item.compile_ns
            self._event("compile-start", tx)
            heapq.heappush(self.compile_heap, (tx.compile_finish_ns, tx.sequence, tx))
            return
        phase = tx.phases[tx.phase_index]
        if phase.direction == "c_to_f":
            tx.c_to_f_bytes += phase.byte_count
            self.worker_c_to_f[tx.worker] += phase.byte_count
        else:
            tx.f_to_c_bytes += phase.byte_count
            self.worker_f_to_c[tx.worker] += phase.byte_count
        flow = Flow(self.flow_sequence, tx, phase, Fraction(phase.byte_count * 8))
        self.flow_sequence += 1
        if phase.byte_count == 0:
            self._event("flow-start", tx, phase)
            latency = int(self.network[phase.direction]["one_way_latency_ns"])
            self._event("flow-sent", tx, phase)
            if latency:
                heapq.heappush(
                    self.delivery_heap, (self.now + latency, flow.sequence, flow)
                )
            else:
                self._event("flow-finish", tx, phase)
                tx.phase_index += 1
                self._start_next_phase(tx)
            return
        self.endpoint_queues[flow.endpoint].append(flow)
        self._fill_endpoint_queues()

    def _endpoint_lanes(self, endpoint: tuple[str, int, int]) -> int:
        return int(self.network[endpoint[0]]["lanes_per_endpoint"])

    def _fill_endpoint_queues(self) -> None:
        active_count: dict[tuple[str, int, int], int] = defaultdict(int)
        for flow in self.active_flows.values():
            active_count[flow.endpoint] += 1
        for endpoint in sorted(self.endpoint_queues):
            queue = self.endpoint_queues[endpoint]
            while queue and active_count[endpoint] < self._endpoint_lanes(endpoint):
                flow = queue.popleft()
                flow.start_ns = self.now
                self.active_flows[flow.sequence] = flow
                active_count[endpoint] += 1
                self._event("flow-start", flow.transaction, flow.phase)

    def _flow_rates(self) -> dict[int, Fraction]:
        """Return max-min fair bits/ns under route and shared-fabric capacities."""
        if not self.active_flows:
            return {}
        capacities: dict[tuple[object, ...], Fraction] = {
            ("fabric",): Fraction(int(self.network["shared_fabric_bps"]), NANOSECONDS)
        }
        resources: dict[int, tuple[tuple[object, ...], ...]] = {}
        for sequence, flow in self.active_flows.items():
            route = ("route",) + flow.endpoint
            capacities.setdefault(
                route,
                Fraction(
                    int(self.network[flow.phase.direction]["bits_per_second"]),
                    NANOSECONDS,
                ),
            )
            resources[sequence] = (("fabric",), route)
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
            flow.remaining_bits / rates[sequence]
            for sequence, flow in self.active_flows.items()
        )

    def _advance_network(self, target: Fraction) -> None:
        if target < self.now:
            raise RuntimeError("simulation clock moved backwards")
        elapsed = target - self.now
        if elapsed and self.active_flows:
            rates = self._flow_rates()
            for sequence, flow in self.active_flows.items():
                flow.remaining_bits -= rates[sequence] * elapsed
                if flow.remaining_bits < 0:
                    raise RuntimeError("network flow overran its completion point")
        self.now = target

    def _finish_serializations(self) -> None:
        finished = [
            flow for flow in self.active_flows.values() if flow.remaining_bits == 0
        ]
        if not finished:
            return
        for flow in sorted(finished, key=lambda value: value.sequence):
            del self.active_flows[flow.sequence]
        for flow in sorted(finished, key=lambda value: value.sequence):
            self._event("flow-sent", flow.transaction, flow.phase)
            latency = int(self.network[flow.phase.direction]["one_way_latency_ns"])
            heapq.heappush(
                self.delivery_heap,
                (self.now + latency, flow.sequence, flow),
            )
        self._fill_endpoint_queues()

    def _finish_deliveries(self) -> None:
        while self.delivery_heap and self.delivery_heap[0][0] <= self.now:
            _, _, flow = heapq.heappop(self.delivery_heap)
            self._event("flow-finish", flow.transaction, flow.phase)
            flow.transaction.phase_index += 1
            self._start_next_phase(flow.transaction)

    def _finish_compiles(self) -> None:
        while self.compile_heap and self.compile_heap[0][0] <= self.now:
            _, _, tx = heapq.heappop(self.compile_heap)
            self._event("compile-finish", tx)
            self.free_slots[tx.worker].release(tx.slot)
            self.worker_reserved_ns[tx.worker] += self.now - tx.dispatch_ns
            self.completed += 1
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

    def run(self) -> SimulationResult:
        while self.completed < self.total_items:
            self._release_ready()
            self._finish_serializations()
            self._finish_deliveries()
            self._finish_compiles()
            self._release_ready()
            self._dispatch()
            if self.completed == self.total_items:
                break
            target = self._next_time()
            if target == self.now:
                raise RuntimeError("simulation produced a zero-time event loop")
            self._advance_network(target)

        makespan_ns = ceil_fraction(self.now)
        assignments = []
        for tx in self.transactions:
            if (
                tx.transfer_done_ns is None
                or tx.compile_start_ns is None
                or tx.compile_finish_ns is None
            ):
                raise RuntimeError("completed simulation has an unfinished transaction")
            assignments.append(
                {
                    "dispatch_order": tx.sequence,
                    "environment": tx.item.environment,
                    "workload": tx.item.workload,
                    "build": tx.item.build,
                    "logical": tx.item.logical,
                    "worker": tx.worker,
                    "slot": tx.slot,
                    "release_ns": tx.item.release_ns,
                    "dispatch_ns": ceil_fraction(tx.dispatch_ns),
                    "transfer_done_ns": ceil_fraction(tx.transfer_done_ns),
                    "compile_start_ns": ceil_fraction(tx.compile_start_ns),
                    "compile_finish_ns": ceil_fraction(tx.compile_finish_ns),
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
            finish_ns = max(
                ceil_fraction(tx.compile_finish_ns)  # type: ignore[arg-type]
                for tx in transactions
            )
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
                    "finish_ns": finish_ns,
                    "elapsed_from_release_ns": finish_ns - release_ns,
                    "gap_from_previous_finish_ns": (
                        "" if prior_finish_ns is None else release_ns - prior_finish_ns
                    ),
                    "workers_used": len({tx.worker for tx in transactions}),
                    "raw_bytes": sum(tx.item.raw_bytes for tx in transactions),
                    "c_to_f_bytes": sum(tx.c_to_f_bytes for tx in transactions),
                    "f_to_c_bytes": sum(tx.f_to_c_bytes for tx in transactions),
                    "compiler_work_ns": sum(tx.item.compile_ns for tx in transactions),
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
            stop_ns = max(
                ceil_fraction(tx.compile_finish_ns)  # type: ignore[arg-type]
                for tx in transactions
            )
            generations.append(
                {
                    "generation": generation,
                    "temperature": "cold" if generation == 0 else "warm",
                    "environments": len({tx.item.environment for tx in transactions}),
                    "workloads": len({tx.item.workload for tx in transactions}),
                    "jobs": len(transactions),
                    "start_ns": start_ns,
                    "first_dispatch_ns": first_dispatch_ns,
                    "stop_ns": stop_ns,
                    "duration_ns": stop_ns - start_ns,
                    "raw_bytes": sum(tx.item.raw_bytes for tx in transactions),
                    "c_to_f_bytes": sum(tx.c_to_f_bytes for tx in transactions),
                    "f_to_c_bytes": sum(tx.f_to_c_bytes for tx in transactions),
                    "compiler_work_ns": sum(tx.item.compile_ns for tx in transactions),
                }
            )
        summed_generation_ns = sum(row["duration_ns"] for row in generations)
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
            "placement_policy": self.scheduler["placement_policy"],
            "ready_job_policy": self.scheduler["ready_job_policy"],
            "jobs": self.total_items,
            "environments": int(self.scenario.document["environments"]["env_count"]),
            "build_epochs": len(builds),
            "cold_builds": sum(row["temperature"] == "cold" for row in builds),
            "warm_builds": sum(row["temperature"] == "warm" for row in builds),
            "workers": self.f_count,
            "slots_per_worker": self.slots_per_f,
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
            "wall_minus_summed_generation_ns": makespan_ns - summed_generation_ns,
            "compiler_work_ns": sum(
                item.compile_ns
                for items in self.scenario.work_items.values()
                for item in items
            ),
        }
        return SimulationResult(
            summary, assignments, self.events, workers, builds, generations
        )


def write_tsv(path: Path, rows: Iterable[dict[str, object]]) -> None:
    rows = list(rows)
    if not rows:
        raise ValueError(f"refusing to write empty table {path}")
    with path.open("w", newline="") as output:
        writer = csv.DictWriter(
            output, fieldnames=list(rows[0]), delimiter="\t", lineterminator="\n"
        )
        writer.writeheader()
        writer.writerows(rows)


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


def write_result(
    scenario: LoadedScenario, result: SimulationResult, output_directory: Path
) -> None:
    output_directory.mkdir(parents=True, exist_ok=True)
    resolved_document = json.loads(json.dumps(scenario.document))
    for job in resolved_document["environments"]["job_selection"]["jobs"]:
        job["corpus_root"] = scenario.workload_inputs[job["id"]]["corpus_root"]
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("scenario", type=Path)
    parser.add_argument("--codec", choices=("compile-only", "raw"), required=True)
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
    adapter: CodecAdapter = (
        CompileOnlyAdapter() if args.codec == "compile-only" else RawAdapter()
    )
    result = Simulator(scenario, adapter).run()
    write_result(scenario, result, args.out)
    print(json.dumps(result.summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
