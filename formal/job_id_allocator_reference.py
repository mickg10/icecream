#!/usr/bin/env python3
"""Deterministic tiny-domain oracle for JobIdAllocator.tla.

The C++ allocator unit test and TLC runs are authoritative.  This script fixes
the intended mutation traces and model-to-harness event names.
"""
from __future__ import annotations

from dataclasses import asdict, dataclass, field
import argparse
import json
from typing import Optional


@dataclass
class Allocator:
    max_id: int
    cursor: int = 0
    live: set[int] = field(default_factory=set)
    issued_total: int = 0
    released_total: int = 0
    exhaustion_count: int = 0
    unknown_release_count: int = 0

    def cyclic(self):
        for step in range(1, self.max_id + 1):
            yield ((self.cursor + step - 1) % self.max_id) + 1

    def allocate(
        self,
        visible: Optional[set[int]] = None,
        *,
        allow_zero: bool = False,
    ) -> Optional[int]:
        view = self.live if visible is None else visible
        if allow_zero and self.cursor == self.max_id and len(view) < self.max_id:
            value = 0
        else:
            value = next(
                (candidate for candidate in self.cyclic() if candidate not in view),
                None,
            )
        if value is None:
            self.exhaustion_count += 1
            return None
        self.cursor = value
        self.live.add(value)
        self.issued_total += 1
        return value

    def release(self, value: int) -> bool:
        if value not in self.live:
            self.unknown_release_count += 1
            return False
        self.live.remove(value)
        self.released_total += 1
        return True

    def conservation(self) -> bool:
        return self.issued_total == len(self.live) + self.released_total


@dataclass
class Result:
    name: str
    outcome: str
    property: Optional[str]
    trace: list[str]
    final: dict


def fixed_case() -> Result:
    allocator = Allocator(3, cursor=3)
    owners: dict[str, int] = {}
    trace: list[str] = []
    for owner in ("r0", "l0", "r1"):
        value = allocator.allocate()
        assert value is not None
        owners[owner] = value
        trace.append(f"Allocate({owner})={value}")
    value = allocator.allocate()
    trace.append(f"Allocate(r2)={value}")
    assert value is None
    assert allocator.live == {1, 2, 3}
    assert allocator.conservation()
    assert allocator.release(owners["l0"])
    trace.append("Release(l0=2)")
    value = allocator.allocate()
    assert value == 2
    owners["r2"] = value
    trace.append("Allocate(r2)=2")
    assert 0 not in allocator.live
    assert len(allocator.live) == 3
    assert allocator.conservation()
    return Result(
        "JobIdAllocatorFixed",
        "PASS",
        None,
        trace,
        {"allocator": asdict(allocator), "owners": owners},
    )


def zero_case() -> Result:
    allocator = Allocator(3, cursor=3)
    value = allocator.allocate(allow_zero=True)
    assert value == 0
    return Result(
        "JobIdZeroWrapMutant",
        "EXPECTED_COUNTEREXAMPLE",
        "NoZero",
        ["Allocate(r0)=0"],
        asdict(allocator),
    )


def remote_only_case() -> Result:
    allocator = Allocator(1)
    owners: dict[str, int] = {}
    owners["l0"] = allocator.allocate()
    owners["r0"] = allocator.allocate(visible=set())
    assert owners["l0"] == owners["r0"] == 1
    return Result(
        "JobIdRemoteOnlyCollisionMutant",
        "EXPECTED_COUNTEREXAMPLE",
        "GlobalLiveUniqueness",
        ["Allocate(l0)=1", "Allocate(r0)=1 using remote-only view"],
        {"allocator": asdict(allocator), "owners": owners},
    )


def unknown_done_case() -> Result:
    terminal_ids = [0]
    return Result(
        "JobIdUnknownLocalDoneMutant",
        "EXPECTED_COUNTEREXAMPLE",
        "UnknownDoneNoTerminalZero",
        ["UnknownLocalDone(k)", "map::operator[] -> global_id=0", "EmitTerminal(0)"],
        {"terminal_ids": terminal_ids},
    )


def duplicate_case() -> Result:
    allocator = Allocator(3)
    owners: dict[str, int] = {"l0": allocator.allocate()}
    old = owners["l0"]
    new = allocator.allocate()
    assert old == 1 and new == 2
    owners["l0"] = new
    mapped = set(owners.values())
    assert allocator.live != mapped
    return Result(
        "JobIdDuplicateLocalBeginMutant",
        "EXPECTED_COUNTEREXAMPLE",
        "AllocatorMapAgreement",
        ["LocalBegin(k)=1", "DuplicateLocalBegin(k)=2", "old id orphaned"],
        {"allocator": asdict(allocator), "owners": owners, "mapped": sorted(mapped)},
    )


def disconnect_case() -> Result:
    allocator = Allocator(3)
    owners: dict[str, int] = {"l0": allocator.allocate()}
    del owners["l0"]
    assert allocator.live
    return Result(
        "JobIdDisconnectLeakMutant",
        "EXPECTED_COUNTEREXAMPLE",
        "AllocatorMapAgreement",
        ["LocalBegin(l0)=1", "DaemonDisconnect", "mapping destroyed without release"],
        {"allocator": asdict(allocator), "owners": owners},
    )


def batch_case() -> Result:
    allocator = Allocator(3)
    assert allocator.allocate() == 1
    assert allocator.allocate() == 2
    free = [candidate for candidate in allocator.cyclic() if candidate not in allocator.live]
    assert free == [3]
    staged = allocator.allocate()
    assert staged == 3
    return Result(
        "JobIdPartialBatchMutant",
        "EXPECTED_COUNTEREXAMPLE",
        "ExhaustionNoPartialPublication",
        [
            "Allocate(existing0)=1",
            "Allocate(existing1)=2",
            "BatchRequest(count=2)",
            "PartiallyPublish(3)",
            "Exhaust",
        ],
        {
            "allocator": asdict(allocator),
            "batch_failed": True,
            "batch_published": [staged],
        },
    )


def reuse_case() -> Result:
    allocator = Allocator(1, cursor=1, live={1}, issued_total=1)
    assert allocator.release(1)
    new = allocator.allocate()
    assert new == 1
    assert allocator.conservation()
    return Result(
        "JobIdReleasedReuse",
        "PASS",
        None,
        ["OldOwnerLive(1)", "Release(old=1)", "Allocate(new)=1"],
        asdict(allocator),
    )


def run_all() -> list[Result]:
    return [
        fixed_case(),
        zero_case(),
        remote_only_case(),
        unknown_done_case(),
        duplicate_case(),
        disconnect_case(),
        batch_case(),
        reuse_case(),
    ]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    results = run_all()
    if args.json:
        print(json.dumps([asdict(result) for result in results], indent=2, sort_keys=True))
    else:
        for result in results:
            print(f"{result.outcome:27} {result.name}")
            if result.property:
                print(f"  property: {result.property}")
            print("  trace: " + " -> ".join(result.trace))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
