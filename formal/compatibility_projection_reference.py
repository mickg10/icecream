#!/usr/bin/env python3
"""Deterministic reference for the P43/P48 compatibility projection.

This is a trace oracle for mutation tests. TLC plus the cross-linked OLD/NEW
codec fixture remain authoritative.
"""
from __future__ import annotations

from dataclasses import asdict, dataclass, replace
from itertools import zip_longest
import argparse
import json
from typing import Optional


@dataclass(frozen=True)
class Message:
    kind: str
    job: int = 0
    client: int = 0
    command_summary: str = ""
    fulljob: bool = False
    local_reason: str = ""
    cmdline: str = ""
    local_flags: int = 0
    client_count: Optional[int] = None


TRACE = (
    Message("GET_CS", client=11, command_summary="cc -c x.c"),
    Message("USE_CS", job=100, client=11),
    Message("JOB_BEGIN", job=100, client=11, client_count=9),
    Message("JOB_DONE", job=100, client=11, client_count=9),
    Message(
        "JOB_LOCAL_BEGIN",
        client=12,
        fulljob=True,
        local_reason="policy",
        cmdline="cc -E x.c",
        local_flags=1,
    ),
    Message("JOB_LOCAL_DONE", client=12),
    Message("JOB_TIMING", job=100, client=11),
    Message("STATS", client_count=9),
)


def correct_project(message: Message) -> Optional[Message]:
    if message.kind == "JOB_TIMING":
        return None
    if message.kind == "GET_CS":
        return replace(message, command_summary="")
    if message.kind == "JOB_LOCAL_BEGIN":
        return replace(
            message,
            fulljob=False,
            local_reason="",
            cmdline="",
            local_flags=0,
        )
    if message.kind == "STATS":
        # client_count is an in-memory member, not a wire field at P43 or P48.
        return replace(message, client_count=None)
    return message


def candidate_project(
    message: Message,
    *,
    emit_job_timing: bool = False,
    keep_command_summary: bool = False,
    keep_local_fields: bool = False,
    keep_stats_client_count: bool = False,
) -> Optional[Message]:
    if message.kind == "JOB_TIMING":
        return message if emit_job_timing else None
    if message.kind == "GET_CS":
        return message if keep_command_summary else replace(message, command_summary="")
    if message.kind == "JOB_LOCAL_BEGIN":
        if keep_local_fields:
            return message
        return replace(
            message,
            fulljob=False,
            local_reason="",
            cmdline="",
            local_flags=0,
        )
    if message.kind == "STATS":
        return message if keep_stats_client_count else replace(message, client_count=None)
    return message


def projected_trace(projector) -> tuple[Message, ...]:
    output: list[Message] = []
    for message in TRACE:
        projected = projector(message)
        if projected is not None:
            output.append(projected)
    return tuple(output)


@dataclass
class Result:
    name: str
    outcome: str
    processed: int
    property: Optional[str] = None
    candidate: Optional[dict] = None
    expected: Optional[dict] = None


def run_case(name: str, property_name: str, **mutants: bool) -> Result:
    expected = projected_trace(correct_project)
    candidate = projected_trace(lambda message: candidate_project(message, **mutants))
    if candidate == expected:
        return Result(name, "PASS", len(TRACE))

    for index, pair in enumerate(zip_longest(candidate, expected), start=1):
        actual, wanted = pair
        if actual != wanted:
            return Result(
                name,
                "EXPECTED_COUNTEREXAMPLE",
                index,
                property_name,
                None if actual is None else asdict(actual),
                None if wanted is None else asdict(wanted),
            )
    raise AssertionError("different traces without a differing element")


def run_all() -> list[Result]:
    fixed = projected_trace(correct_project)
    # Pure projection checks, mirrored by the TLA+ invariants.
    for message in fixed:
        again = correct_project(message)
        assert again == message, ("projection is not idempotent", message, again)
    original_by_kind = {message.kind: message for message in TRACE}
    for message in fixed:
        original = original_by_kind[message.kind]
        assert message.job == original.job
        assert message.client == original.client

    return [
        run_case("CompatibilityProjectionFixed", "ProjectionPrefix"),
        run_case(
            "P43EmitJobTimingMutant",
            "NoUnknownKind",
            emit_job_timing=True,
        ),
        run_case(
            "P43CommandSummaryMutant",
            "ProjectionPrefix",
            keep_command_summary=True,
        ),
        run_case(
            "P43LocalFieldsMutant",
            "ProjectionPrefix",
            keep_local_fields=True,
        ),
        run_case(
            "P43StatsClientCountMutant",
            "ProjectionPrefix",
            keep_stats_client_count=True,
        ),
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
            print(f"{result.outcome:27} {result.name} ({result.processed} messages)")
            if result.property:
                print(f"  property: {result.property}")
                print(f"  candidate: {result.candidate}")
                print(f"  expected:  {result.expected}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
