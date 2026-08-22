# JSONL experiment dashboard specification

## Purpose and artifacts

Each simulator run produces a directory containing the canonical
`experiment.jsonl`, the bounded `report.html`, and machine-readable summaries and
TSVs.  `experiment.jsonl` is immutable evidence: it contains the descriptor,
active-time snapshots and explicit idle gaps, followed by the final summary.
`report.html` is a disposable offline visualization generated from that evidence.
It has no CDN, fetch, server, or browser extension dependency and works from
`file://`.

The standalone Rust and Python renderers use the single checked-in
[dashboard_template.html](dashboard_template.html) asset. The Rust crate embeds
it at build time and `dashboard.py` reads it at runtime. `run_scenario.py` still
has its older inline convenience report; use the standalone launcher when a
canonical bounded dashboard is required.

The canonical standalone regeneration command is the Rust launcher:

```bash
capability/distribution/render_dashboard.sh /path/to/experiment.jsonl \
  --out /path/to/report.html
```

The same launcher also provides bounded comparison and catalog views:

```text
render  experiment.jsonl --out report.html
compare simulated.jsonl physical.jsonl --out comparison.html
index   results-root --out index.html
```

Each source is checked by the accepted R4 `validate_experiment.py` boundary
before output. Its bounded multi-pass adapter executes the exact accepted
validator body and changes only JSONL row storage; this preserves every R4
schema, identity, evidence, replay and accounting check without retaining the
full display timeline. Comparison and index pages retain
missing fields as `(missing)` or `not rendered`; they do not estimate values from
unrelated fields.

The Rust reader uses line-by-line `serde_json`, measured bounded selection, and
atomic temp/rename output. The Python module is retained as a compatible
standalone path and for tests; it is not the preferred reader for multi-gigabyte
Firefox traces.

## Canonical record contract

The first row has `record=execution`, schema
`icecream-execution-v2`, scenario and topology identity, codec
adapter/physical-result provenance, resolved scenario, workload inputs, clock,
network allocation and state-coverage semantics.  Timeline rows have either
`record=snapshot` or `record=gap`.

Snapshots contain wall and active-time interval boundaries, interval-integrated
network metrics, scheduler/C/F state after boundary transitions, and exact events
attached to the first ending record at or after each event.  Gaps contain wall
duration, active position, reason, and boundary state where available.  The final
row has `record=summary`, `event_count`, and the complete simulator summary.

The JSONL remains the only source for exact event ordering and full codec state.
The browser projection may omit codec namespaces and event rows, but must label
those omissions and show the canonical path.

## Browser projection and fidelity

The HTML embeds an adaptive, evenly spaced subset of at most 2,000 snapshots,
always including the first and last snapshot, plus every gap. The adaptive cap
uses the measured encoded size of every projected snapshot and a two-megabyte
timeline target; it does not trust descriptor dimensions as a size estimate.
The complete final HTML is measured before atomic replacement and must not
exceed 5,000,000 bytes. Snapshot projection retains chart fields,
worker occupancy, C scheduler counters, route/environment rates and interval
boundaries. Optional per-F cache/lease/cursor state is capped at 512 encoded
bytes per selected F point and carries an explicit truncation marker; large codec
namespace objects are not embedded. Events are sampled
as first/last bounded samples (currently 250 each) and the page exposes source and
embedded counts.  Standalone generation computes complete phase, build and
terminal aggregates while streaming; these aggregates are not inferred from the
sampled event rows.

Phase byte totals count one canonical `flow-queued` extent per phase.  Queued,
start, sent and finish transitions are not summed as bytes because they describe
the same extent.  The UI labels this semantic explicitly.

## Views and interactions

The dashboard contains:

* scenario/codec identity, resolved provenance, topology dimensions and headline
  byte/time/job/build metrics;
* an active-time bandwidth timeline with C→F, F→C and selected-F lines;
* zoom, pan, reset and hover detail on the timeline;
* per-F occupancy and per-C scheduler/state plots;
* endpoint bandwidth rates/utilization where capacities are present;
* build-epoch and generation/terminal-stage navigation;
* phase/event breakdown, exact event table, and compressed gap table;
* snapshot detail showing interval, scheduler, selected C/F state, routes and
  attached events;
* filters for C, F, build and phase.  C/F options come from descriptor/summary
  dimensions, not only from sampled events.

Active-flow counts are kept in state/detail views rather than sharing a scale with
bits-per-second.  A terminal-stage table is explicitly a terminal-event summary;
it is not called a critical path unless a future trace supplies dependency/path
reconstruction data.

## Empty, small and large traces

An empty active timeline renders a useful identity/provenance page and a clear
“no active snapshots” message.  A one-snapshot trace renders without divide-by-zero
or canvas errors.  Small traces retain all rows.  Large traces retain boundaries,
all gaps, and bounded state/chart data; omitted detail is disclosed with source
counts and the canonical JSONL path.  Event tables cap visible sampled rows and
never imply that a sample is complete.

## Suite index and comparison

The suite report remains the comparison index.  Its matrix links to each run's
`scenario/codec/report.html` and keeps exact matrix, generation, physical-phase
and runner-timing TSVs as the authoritative comparison data.  A suite dashboard
must not inline every run's trace; navigation is by link to each bounded report.

The standalone `compare` command has a deliberately narrow contract: its first
input must declare `mode=simulated`, its second must declare `mode=physical`, and
their canonical scenario name/digest/manifest, input-manifest digests and
workload inputs must be identical. Other pairs are refused instead of being
relabeled. Column and source labels come from the two actual execution modes.

Canonical evidence `path` fields remain untouched. For offline navigation the
renderer adds a display-only `report_href`, rebased from the source run directory
to the chosen report directory. This works when a report is beside its run, in a
sibling directory, or outside the indexed result root.

## Accessibility and performance budgets

The report uses semantic headings, table headers, labels, readable contrast,
keyboard-operable selects/buttons, and text tables as fallbacks for every chart.
It must remain usable without network access. Target embedded snapshot count is
≤2,000, event sample ≤500, and the complete generated HTML is ≤5,000,000 bytes,
bounded by projected state rather than canonical trace length.

## Acceptance gates

1. `python3 -m unittest capability.distribution.test_run_scenario` passes,
   including existing JSONL/report compatibility checks.
2. Standalone rendering succeeds on an existing JSONL and preserves canonical
   JSONL byte-for-byte.
3. A large synthetic stream proves bounded snapshot/event embedding and a bounded
   HTML size.
4. A small and zero-snapshot stream renders with no exception.
5. The report contains no external URLs/scripts and opens as a local file.
6. Suite report links remain relative and valid.
7. `cargo test --manifest-path capability/distribution/dashboard-rs/Cargo.toml` passes.

## End-to-end accepted C1F20 benchmark

The independent rereview's authoritative-valid 226,191,505-byte C1F20 stream has
10,000 realistic snapshots, 20 F state rows, 20 worker metric rows, both route
directions for every F, and 30 exact events. The corrected public release
`render` command, including accepted R4 validation, measured:

```text
wall:                 25.01 s
maximum RSS:          47,972 KiB
HTML:                 2,073,253 bytes
SHA-256:              9eb14fb3cac7a9ae82a7f8746c5bdcac2c3d3027a5a974c15265ec1fe5aa8dfd
```

The measurement ran the already-built release CLI under `/usr/bin/time -v`;
compilation was excluded. Validation alone reported the exact retained totals
(`C→F=37`, `F→C=0`, `events=30`) at 47,920 KiB peak RSS in 16.46 seconds. The
large-stream gate applies to actual final bytes, not an estimate.
