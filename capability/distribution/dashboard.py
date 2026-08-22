#!/usr/bin/env python3
"""Offline dashboard renderer for icecream distribution experiment JSONL.

The JSONL stream is the source of truth.  This module deliberately has no runtime
dependencies beyond the Python standard library and emits a file://-safe report.
"""

from __future__ import annotations

import argparse
import json
from collections import deque
from pathlib import Path
from typing import Iterable


MAX_REPORT_SNAPSHOTS = 2_000


class EventAccumulator:
    """Bounded event sample plus exact aggregate counters for one JSONL pass."""

    def __init__(self, sample_limit: int = 250) -> None:
        self.sample_limit = sample_limit
        self.event_count = 0
        self.first: list[dict[str, object]] = []
        self.last: deque[dict[str, object]] = deque(maxlen=sample_limit)
        self.phases: dict[str, dict[str, object]] = {}
        self.builds: dict[str, dict[str, object]] = {}
        self.terminals: dict[str, dict[str, object]] = {}

    def add(self, event: dict[str, object]) -> None:
        self.event_count += 1
        if len(self.first) < self.sample_limit:
            self.first.append(event)
        else:
            self.last.append(event)
        phase = str(event.get("phase") or "(none)") + " / " + str(event.get("direction") or "")
        p = self.phases.setdefault(phase, {"phase": phase, "filter_phase": str(event.get("phase") or ""), "events": 0, "bytes": 0, "semantics": "flow-queued phase extents"})
        if event.get("event") == "flow-queued":
            p["events"] += 1
            p["bytes"] += int(event.get("bytes") or 0)
        build = str(event.get("build") if event.get("build") != "" else "(none)")
        b = self.builds.setdefault(build, {"build": build, "events": 0, "first": int(event.get("time_ns") or 0), "last": int(event.get("time_ns") or 0)})
        b["events"] += 1
        b["first"] = min(b["first"], int(event.get("time_ns") or 0))
        b["last"] = max(b["last"], int(event.get("time_ns") or 0))
        endpoint = build + " / " + ("C" if event.get("worker") == "" else "F" + str(int(event.get("worker")) + 1))
        if endpoint not in self.terminals or int(event.get("time_ns") or 0) >= self.terminals[endpoint]["time"]:
            self.terminals[endpoint] = {"endpoint": endpoint, "time": int(event.get("time_ns") or 0), "event": event.get("event", ""), "phase": event.get("phase", ""), "detail": event.get("detail", "")}

    def finish(self) -> dict[str, object]:
        sampled = self.first + list(self.last)
        return {"event_count": self.event_count, "phases": list(self.phases.values()), "builds": list(self.builds.values()), "terminals": list(self.terminals.values()), "events": sampled, "sampled_events": len(sampled), "event_sample_limit": self.sample_limit * 2, "source_event_count": self.event_count, "event_sampling": "first and last bounded event sample" if self.event_count > len(sampled) else "all retained events"}


def _selected_ordinals(count: int, limit: int) -> set[int]:
    if count <= limit:
        return set(range(count))
    if limit <= 1:
        return {count - 1}
    return {i * (count - 1) // (limit - 1) for i in range(limit)}


def compact_timeline(rows: list[dict[str, object]], limit: int = MAX_REPORT_SNAPSHOTS) -> tuple[list[dict[str, object]], dict[str, object]]:
    """Bound snapshots for HTML, retaining every explicit idle gap."""
    positions = [i for i, row in enumerate(rows) if row.get("record") == "snapshot"]
    selected = _selected_ordinals(len(positions), limit)
    chosen = {positions[i] for i in selected}
    view = [_slim_row(row) for i, row in enumerate(rows) if row.get("record") == "gap" or i in chosen]
    return view, {
        "source_records": len(rows),
        "source_snapshots": len(positions),
        "embedded_records": len(view),
        "embedded_snapshots": len(selected),
        "snapshot_selection": "all" if len(positions) <= limit else "evenly spaced including first and last",
        "all_gap_records_embedded": True,
        "canonical_detail": "experiment.jsonl retains every active-time snapshot and event",
    }


def _slim_row(row: dict[str, object]) -> dict[str, object]:
    """Project a snapshot to fields used by the browser; codec state is intentionally omitted."""
    if row.get("record") != "snapshot":
        return {k: row.get(k) for k in ("record", "sequence", "wall_start_ns", "wall_end_ns", "wall_duration_ns", "active_position_ns", "reason") if k in row}
    display = row.get("noncanonical_display") if isinstance(row.get("noncanonical_display"), dict) else row
    metrics = display.get("metrics") if isinstance(display.get("metrics"), dict) else {}
    state = display.get("state") if isinstance(display.get("state"), dict) else {}
    keep_state = {
        "scheduler": state.get("scheduler", {}),
        "network": state.get("network", {}),
        "c": [
            {k: item.get(k) for k in ("environment", "unreleased_tus", "ready_tus", "active_tus", "completed_tus", "admitted_tus", "targets")}
            for item in state.get("c", []) if isinstance(item, dict)
        ],
        "f": [
            {k: item.get(k) for k in ("worker", "slots", "free_slots", "reserved_slots", "input_staging_slots", "free_input_staging_slots", "occupied_input_staging_slots", "input_wait_slots", "ready_input_slots", "compiling_slots", "commit_wait_slots", "dispatched_tus", "completed_tus", "active_flows", "queued_flows", "in_propagation", "active_dialogues", "queued_dialogues")}
            for item in state.get("f", []) if isinstance(item, dict)
        ],
    }
    keep_metrics = {k: metrics.get(k) for k in ("fabric_average_bps", "fabric_capacity_bps", "fabric_utilization", "direction_fabrics", "routes", "environments", "workers") if k in metrics}
    return {k: row.get(k) for k in ("record", "sequence", "wall_start_ns", "wall_end_ns", "wall_duration_ns", "active_start_ns", "active_end_ns", "active_duration_ns", "event_sequence_start", "event_sequence_end") if k in row} | {"state": keep_state, "metrics": keep_metrics, "events": []}


def read_experiment(path: Path, limit: int = MAX_REPORT_SNAPSHOTS) -> tuple[dict[str, object], list[dict[str, object]], dict[str, object]]:
    """Stream an existing canonical JSONL twice; never materialize its full timeline."""
    descriptor: dict[str, object] | None = None
    final: dict[str, object] | None = None
    snapshot_count = 0
    gap_count = 0
    accumulator = EventAccumulator()
    with path.open(encoding="utf-8") as source:
        for line_number, line in enumerate(source, 1):
            if not line.strip():
                continue
            row = json.loads(line)
            if not isinstance(row, dict):
                raise ValueError(f"{path}:{line_number}: JSONL row is not an object")
            record = row.get("record")
            if record in ("experiment", "execution"):
                if descriptor is not None:
                    raise ValueError(f"{path}: repeated experiment descriptor")
                descriptor = row
            elif record == "summary":
                final = row
            elif record in ("snapshot", "gap"):
                if record == "snapshot":
                    snapshot_count += 1
                else:
                    gap_count += 1
                for event in row.get("events", []):
                    if isinstance(event, dict):
                        accumulator.add(event)
            else:
                raise ValueError(f"{path}:{line_number}: unknown record {record!r}")
    if descriptor is None or final is None:
        raise ValueError(f"{path}: expected experiment descriptor and summary rows")
    selected = _selected_ordinals(snapshot_count, limit)
    view: list[dict[str, object]] = []
    ordinal = 0
    with path.open(encoding="utf-8") as source:
        for line in source:
            if not line.strip():
                continue
            row = json.loads(line)
            if row.get("record") == "gap" or row.get("record") == "snapshot" and ordinal in selected:
                view.append(_slim_row(row))
            if row.get("record") == "snapshot":
                ordinal += 1
    metadata = {
        "source_records": snapshot_count + gap_count,
        "source_snapshots": snapshot_count,
        "embedded_records": len(view),
        "embedded_snapshots": len(selected),
        "snapshot_selection": "all" if snapshot_count <= limit else "evenly spaced including first and last",
        "all_gap_records_embedded": True,
        "canonical_detail": "experiment.jsonl retains every active-time snapshot and event",
        "source_event_count": accumulator.event_count,
        "embedded_event_count": accumulator.finish()["sampled_events"],
        "event_sampling": "first and last 250 events" if accumulator.event_count > len(accumulator.first) else "all retained events",
    }
    report_descriptor = json.loads(json.dumps(descriptor))
    report_descriptor["report_view"] = metadata
    report_descriptor["dashboard_aggregates"] = accumulator.finish()
    return report_descriptor, view, final


def _payload(descriptor: dict[str, object], timeline: Iterable[dict[str, object]], final: dict[str, object]) -> str:
    payload_descriptor = json.loads(json.dumps(descriptor))
    aggregates = payload_descriptor.pop("dashboard_aggregates", {})
    events = aggregates.pop("events", []) if isinstance(aggregates, dict) else []
    return json.dumps({"descriptor": payload_descriptor, "timeline": list(timeline), "final": final, "events": events, "aggregates": aggregates}, separators=(",", ":")).replace("</", "<\\/")


def render_experiment_html(descriptor: dict[str, object], timeline: list[dict[str, object]], final: dict[str, object], *, compact: bool = True) -> str:
    """Render a rich, self-contained experiment report.

    ``timeline`` may already be the bounded writer view.  Direct callers can set
    ``compact=True`` to apply the same bound while preserving all gaps.
    """
    if compact and not descriptor.get("report_view"):
        events = [event for row in timeline for event in row.get("events", []) if isinstance(event, dict)]
        sample = events if len(events) <= 250 else events[:250] + events[-250:]
        descriptor = json.loads(json.dumps(descriptor))
        descriptor["dashboard_aggregates"] = {"event_count": len(events), "events": sample, "sampled_events": len(sample), "event_sample_limit": 500}
        timeline, report_view = compact_timeline(timeline)
        descriptor["report_view"] = report_view
    elif descriptor.get("report_view"):
        # write_result supplies an already-selected view.  It still contains full
        # simulator state, so project it here without reparsing the canonical stream.
        descriptor = json.loads(json.dumps(descriptor))
        if not descriptor.get("dashboard_aggregates"):
            events = [event for row in timeline for event in row.get("events", []) if isinstance(event, dict)]
            canonical_event_count = int(final.get("event_count") or len(events))
            sample = events if len(events) <= 250 else events[:250] + events[-250:]
            descriptor["dashboard_aggregates"] = {"event_count": canonical_event_count, "source_event_count": canonical_event_count, "events": sample, "sampled_events": len(sample), "event_sample_limit": 500, "event_sampling": "embedded report-view events only; regenerate from experiment.jsonl for complete aggregates"}
            descriptor["report_view"] = {**descriptor.get("report_view", {}), "source_event_count": canonical_event_count, "embedded_event_count": min(len(events), 500), "event_sampling": "embedded report-view events only; regenerate from experiment.jsonl for complete aggregates"}
        timeline = [_slim_row(row) for row in timeline]
    payload = _payload(descriptor, timeline, final)
    return _template().replace("__PAYLOAD__", payload)


def render_experiment_file(source: Path, output: Path, limit: int = MAX_REPORT_SNAPSHOTS) -> None:
    descriptor, timeline, final = read_experiment(source, limit)
    output.write_text(render_experiment_html(descriptor, timeline, final, compact=False), encoding="utf-8")


def _template() -> str:
    return Path(__file__).with_name("dashboard_template.html").read_text(encoding="utf-8")

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Render an offline dashboard from experiment.jsonl")
    parser.add_argument("experiment_jsonl", type=Path)
    parser.add_argument("--out", type=Path, help="HTML output (default: alongside JSONL as report.html)")
    parser.add_argument("--max-snapshots", type=int, default=MAX_REPORT_SNAPSHOTS)
    args = parser.parse_args(argv)
    if args.max_snapshots <= 0:
        parser.error("--max-snapshots must be positive")
    output = args.out or args.experiment_jsonl.with_name("report.html")
    output.parent.mkdir(parents=True, exist_ok=True)
    render_experiment_file(args.experiment_jsonl, output, args.max_snapshots)
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
