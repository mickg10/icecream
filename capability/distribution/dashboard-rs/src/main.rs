use serde_json::{json, Map, Value};
use std::collections::{BTreeMap, VecDeque};
use std::env;
use std::fs::{self, File};
use std::io::{self, BufRead, BufReader, Write};
use std::path::{Path, PathBuf};

const MAX_REPORT_SNAPSHOTS: usize = 2_000;
const TEMPLATE: &str = include_str!("../../dashboard_template.html");

#[derive(Debug)]
struct EventAccumulator {
    count: u64,
    first: Vec<Value>,
    last: VecDeque<Value>,
    phases: BTreeMap<String, PhaseAgg>,
    builds: BTreeMap<String, BuildAgg>,
    terminals: BTreeMap<String, TerminalAgg>,
}

#[derive(Debug, Default)]
struct PhaseAgg {
    filter_phase: String,
    events: u64,
    bytes: u64,
}
#[derive(Debug, Default)]
struct BuildAgg {
    events: u64,
    first: u64,
    last: u64,
}
#[derive(Debug, Default)]
struct TerminalAgg {
    time: u64,
    event: String,
    phase: String,
    detail: String,
}

impl EventAccumulator {
    fn new() -> Self {
        Self {
            count: 0,
            first: Vec::with_capacity(250),
            last: VecDeque::with_capacity(250),
            phases: BTreeMap::new(),
            builds: BTreeMap::new(),
            terminals: BTreeMap::new(),
        }
    }

    fn add(&mut self, event: &Value) {
        self.count += 1;
        if self.first.len() < 250 {
            self.first.push(event.clone());
        } else {
            if self.last.len() == 250 {
                self.last.pop_front();
            }
            self.last.push_back(event.clone());
        }
        let phase_name = format!(
            "{} / {}",
            text(event, "phase", "(none)"),
            text(event, "direction", "")
        );
        let phase = self.phases.entry(phase_name).or_insert_with(|| PhaseAgg {
            filter_phase: text(event, "phase", ""),
            ..Default::default()
        });
        // One phase extent emits queued/start/sent/finish events.  Only queued is
        // the canonical byte-bearing extent for the phase table.
        if text(event, "event", "") == "flow-queued" {
            phase.events += 1;
            phase.bytes += number(event, "bytes");
        }
        let build_text = text(event, "build", "");
        let build = if build_text.is_empty() {
            "(none)".to_string()
        } else {
            build_text
        };
        let time = number(event, "time_ns");
        let b = self.builds.entry(build.clone()).or_insert(BuildAgg {
            first: time,
            last: time,
            ..Default::default()
        });
        b.events += 1;
        b.first = b.first.min(time);
        b.last = b.last.max(time);
        let endpoint = format!(
            "{} / {}",
            build,
            if text(event, "worker", "").is_empty() {
                "C".to_string()
            } else {
                format!("F{}", number(event, "worker") + 1)
            }
        );
        let terminal = self.terminals.entry(endpoint).or_default();
        if time >= terminal.time {
            terminal.time = time;
            terminal.event = text(event, "event", "");
            terminal.phase = text(event, "phase", "");
            terminal.detail = text(event, "detail", "");
        }
    }

    fn value(&self) -> Value {
        let mut events = self.first.clone();
        events.extend(self.last.iter().cloned());
        let phases: Vec<Value> = self.phases.iter().map(|(name, p)| json!({"phase": name, "filter_phase": p.filter_phase, "events": p.events, "bytes": p.bytes, "semantics": "flow-queued phase extents"})).collect();
        let builds: Vec<Value> = self.builds.iter().map(|(build, b)| json!({"build": build, "events": b.events, "first": b.first, "last": b.last})).collect();
        let terminals: Vec<Value> = self.terminals.iter().map(|(endpoint, t)| json!({"endpoint": endpoint, "time": t.time, "event": t.event, "phase": t.phase, "detail": t.detail})).collect();
        json!({"event_count": self.count, "source_event_count": self.count, "phases": phases, "builds": builds, "terminals": terminals, "events": events, "sampled_events": self.first.len() + self.last.len(), "event_sample_limit": 500, "event_sampling": if self.count > (self.first.len() + self.last.len()) as u64 { "first and last bounded event sample" } else { "all retained events" }})
    }
}

fn text(row: &Value, key: &str, fallback: &str) -> String {
    row.get(key)
        .and_then(Value::as_str)
        .map(str::to_string)
        .or_else(|| {
            row.get(key).and_then(|v| {
                if v.is_number() {
                    Some(v.to_string())
                } else {
                    None
                }
            })
        })
        .unwrap_or_else(|| fallback.to_string())
}
fn number(row: &Value, key: &str) -> u64 {
    row.get(key)
        .and_then(Value::as_u64)
        .or_else(|| row.get(key).and_then(Value::as_f64).map(|v| v as u64))
        .unwrap_or(0)
}
fn record(row: &Value) -> Result<&str, String> {
    row.get("record")
        .and_then(Value::as_str)
        .ok_or_else(|| "JSONL row has no string record".to_string())
}

fn selected_ordinals(count: usize, limit: usize) -> Vec<usize> {
    if count <= limit {
        return (0..count).collect();
    }
    if limit == 1 {
        return vec![count - 1];
    }
    (0..limit).map(|i| i * (count - 1) / (limit - 1)).collect()
}

fn parse_line(line: io::Result<String>, line_number: usize) -> Result<Value, String> {
    let line = line.map_err(|e| format!("line {line_number}: {e}"))?;
    serde_json::from_str(&line).map_err(|e| format!("line {line_number}: invalid JSON: {e}"))
}

// Pass two only needs the record discriminator for omitted snapshots.  The full
// serde parse is reserved for selected snapshots and gaps, while pass one still
// validates every row completely.
fn record_hint(line: &str) -> Result<&str, String> {
    let marker = line
        .find("\"record\"")
        .ok_or_else(|| "JSONL row has no record field".to_string())?;
    let after = &line[marker + 8..];
    let colon = after
        .find(':')
        .ok_or_else(|| "record field has no colon".to_string())?;
    let value = after[colon + 1..].trim_start();
    if !value.starts_with('"') {
        return Err("record field is not a string".into());
    }
    let end = value[1..]
        .find('"')
        .ok_or_else(|| "unterminated record field".to_string())?
        + 1;
    Ok(&value[1..end])
}

fn first_pass(path: &Path) -> Result<(Value, Value, usize, usize, EventAccumulator), String> {
    let file = File::open(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let mut descriptor = None;
    let mut final_row = None;
    let mut snapshots = 0;
    let mut gaps = 0;
    let mut events = EventAccumulator::new();
    let mut saw_summary = false;
    for (index, line) in BufReader::new(file).lines().enumerate() {
        let row = parse_line(line, index + 1)?;
        let kind = record(&row)?;
        if saw_summary {
            return Err(format!(
                "line {}: summary must be the final JSONL row",
                index + 1
            ));
        }
        match kind {
            "execution" => {
                if descriptor.is_some() || index != 0 {
                    return Err("execution header must be the first JSONL row".into());
                }
                if row.get("schema").and_then(Value::as_str) != Some("icecream-execution-v2") {
                    return Err("first row is not an icecream-execution-v2 header".into());
                }
                descriptor = Some(row);
            }
            "summary" => {
                if final_row.is_some() {
                    return Err("repeated summary row".into());
                }
                final_row = Some(row);
                saw_summary = true;
            }
            "snapshot" => {
                if row.get("sequence").and_then(Value::as_u64) != Some((snapshots + gaps) as u64) {
                    return Err(format!(
                        "line {}: timeline sequence is not contiguous",
                        index + 1
                    ));
                }
                for field in [
                    "sequence",
                    "wall_start_ns",
                    "wall_end_ns",
                    "wall_duration_ns",
                    "active_start_ns",
                    "active_end_ns",
                    "active_duration_ns",
                    "noncanonical_display",
                    "events",
                ] {
                    if row.get(field).is_none() {
                        return Err(format!(
                            "line {}: snapshot lacks required field {field}",
                            index + 1
                        ));
                    }
                }
                if !row
                    .get("noncanonical_display")
                    .is_some_and(Value::is_object)
                    || !row.get("events").is_some_and(Value::is_array)
                {
                    return Err(format!(
                        "line {}: snapshot display/events have wrong shape",
                        index + 1
                    ));
                }
                snapshots += 1;
                if let Some(list) = row.get("events").and_then(Value::as_array) {
                    for event in list {
                        if !event.is_object() {
                            return Err(format!("line {}: event is not an object", index + 1));
                        }
                        events.add(event);
                    }
                }
            }
            "gap" => {
                if row.get("sequence").and_then(Value::as_u64) != Some((snapshots + gaps) as u64) {
                    return Err(format!(
                        "line {}: timeline sequence is not contiguous",
                        index + 1
                    ));
                }
                for field in [
                    "sequence",
                    "wall_start_ns",
                    "wall_end_ns",
                    "wall_duration_ns",
                    "active_position_ns",
                    "reason",
                    "noncanonical_display",
                    "events",
                ] {
                    if row.get(field).is_none() {
                        return Err(format!(
                            "line {}: gap lacks required field {field}",
                            index + 1
                        ));
                    }
                }
                if ![
                    "network-propagation",
                    "environment-install-verify",
                    "workload-release",
                    "timer",
                ]
                .contains(&text(&row, "reason", "").as_str())
                {
                    return Err(format!("line {}: unsupported gap reason", index + 1));
                }
                if !row
                    .get("noncanonical_display")
                    .is_some_and(Value::is_object)
                    || !row.get("events").is_some_and(Value::is_array)
                {
                    return Err(format!(
                        "line {}: gap display/events have wrong shape",
                        index + 1
                    ));
                }
                gaps += 1;
                if let Some(list) = row.get("events").and_then(Value::as_array) {
                    for event in list {
                        if !event.is_object() {
                            return Err(format!("line {}: event is not an object", index + 1));
                        }
                        events.add(event);
                    }
                }
            }
            other => return Err(format!("unknown record {other:?}")),
        }
    }
    match (descriptor, final_row) {
        (Some(d), Some(f)) => {
            let expected = f
                .get("event_count")
                .and_then(Value::as_u64)
                .ok_or_else(|| "summary has no integer event_count".to_string())?;
            if expected != events.count {
                return Err(format!(
                    "summary event_count {expected} does not match observed {count}",
                    count = events.count
                ));
            }
            if f.get("schema").and_then(Value::as_str) != Some("icecream-experiment-summary-v2") {
                return Err("summary is not an icecream-experiment-summary-v2 row".into());
            }
            if let Some(n) = f.get("timeline_records").and_then(Value::as_u64) {
                if n != (snapshots + gaps) as u64 {
                    return Err(format!(
                        "summary timeline_records {n} does not match observed {}",
                        snapshots + gaps
                    ));
                }
            }
            Ok((d, f, snapshots, gaps, events))
        }
        _ => Err("JSONL requires experiment descriptor and summary rows".into()),
    }
}

fn object_field(row: &Value, key: &str) -> Map<String, Value> {
    row.get(key)
        .and_then(Value::as_object)
        .cloned()
        .unwrap_or_default()
}
fn slim_snapshot(row: &Value) -> Value {
    // R4 calls the display-only projection `noncanonical_display`.  Keep this
    // projection deliberately small: the retained JSONL is the evidence; the
    // report only needs bounded chart state and must not copy codec namespaces.
    let display = object_field(row, "noncanonical_display");
    let state = object_field(&Value::Object(display.clone()), "state");
    let metrics = object_field(&Value::Object(display), "metrics");
    let scheduler = state.get("scheduler").cloned().unwrap_or_else(|| json!({}));
    let network = state.get("network").cloned().unwrap_or_else(|| json!({}));
    let c = state
        .get("c")
        .and_then(Value::as_array)
        .map(|items| {
            items
                .iter()
                .map(|item| {
                    let mut out = Map::new();
                    for key in [
                        "environment",
                        "unreleased_tus",
                        "ready_tus",
                        "active_tus",
                        "completed_tus",
                        "admitted_tus",
                    ] {
                        if let Some(v) = item.get(key) {
                            out.insert(key.to_string(), v.clone());
                        }
                    }
                    Value::Object(out)
                })
                .collect::<Vec<_>>()
        })
        .unwrap_or_default();
    let f = state
        .get("f")
        .and_then(Value::as_array)
        .map(|items| {
            items
                .iter()
                .map(|item| {
                    let mut out = Map::new();
                    for key in [
                        "worker",
                        "slots",
                        "free_slots",
                        "reserved_slots",
                        "input_staging_slots",
                        "free_input_staging_slots",
                        "occupied_input_staging_slots",
                        "input_wait_slots",
                        "ready_input_slots",
                        "compiling_slots",
                        "commit_wait_slots",
                        "dispatched_tus",
                        "completed_tus",
                        "active_flows",
                        "queued_flows",
                        "in_propagation",
                        "active_dialogues",
                        "queued_dialogues",
                    ] {
                        if let Some(v) = item.get(key) {
                            out.insert(key.to_string(), v.clone());
                        }
                    }
                    Value::Object(out)
                })
                .collect::<Vec<_>>()
        })
        .unwrap_or_default();
    let mut selected_metrics = Map::new();
    for key in [
        "fabric_average_bps",
        "fabric_capacity_bps",
        "fabric_utilization",
        "direction_fabrics",
        "workers",
    ] {
        if let Some(v) = metrics.get(key) {
            selected_metrics.insert(key.to_string(), v.clone());
        }
    }
    // Keep one compact route point per worker/direction.  The original route
    // list may contain many per-lane records and is not needed for the chart.
    let mut routes = Vec::new();
    if let Some(items) = metrics.get("routes").and_then(Value::as_array) {
        for item in items {
            let mut out = Map::new();
            for key in ["direction", "worker", "average_bps"] {
                if let Some(v) = item.get(key) {
                    out.insert(key.to_string(), v.clone());
                }
            }
            if !out.is_empty() {
                routes.push(Value::Object(out));
            }
        }
    }
    selected_metrics.insert("routes".into(), Value::Array(routes));
    json!({"record":"snapshot", "sequence":row.get("sequence"), "wall_start_ns":row.get("wall_start_ns"), "wall_end_ns":row.get("wall_end_ns"), "wall_duration_ns":row.get("wall_duration_ns"), "active_start_ns":row.get("active_start_ns"), "active_end_ns":row.get("active_end_ns"), "active_duration_ns":row.get("active_duration_ns"), "event_sequence_start":row.get("event_sequence_start"), "event_sequence_end":row.get("event_sequence_end"), "state":{"scheduler":scheduler,"network":network,"c":c,"f":f}, "metrics":selected_metrics, "events":[]})
}

fn slim_gap(row: &Value) -> Value {
    let mut output = Map::new();
    for key in [
        "record",
        "sequence",
        "wall_start_ns",
        "wall_end_ns",
        "wall_duration_ns",
        "active_position_ns",
    ] {
        if let Some(value) = row.get(key) {
            output.insert(key.to_string(), value.clone());
        }
    }
    output.insert(
        "reason".into(),
        row.get("reason").cloned().unwrap_or_else(|| json!("")),
    );
    output.insert("events".into(), json!([]));
    Value::Object(output)
}

fn second_pass(path: &Path, selected: &[usize]) -> Result<(String, usize), String> {
    let file = File::open(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let mut ordinal = 0;
    let mut output = String::from("[");
    let mut output_count = 0;
    for (index, line) in BufReader::new(file).lines().enumerate() {
        let raw = line.map_err(|e| format!("line {}: {e}", index + 1))?;
        match record_hint(&raw)? {
            "snapshot" => {
                if selected.binary_search(&ordinal).is_ok() {
                    let row: Value = serde_json::from_str(&raw)
                        .map_err(|e| format!("line {}: invalid JSON: {e}", index + 1))?;
                    if output_count > 0 {
                        output.push(',');
                    }
                    output.push_str(
                        &serde_json::to_string(&slim_snapshot(&row)).map_err(|e| e.to_string())?,
                    );
                    output_count += 1;
                }
                ordinal += 1;
            }
            "gap" => {
                let row: Value = serde_json::from_str(&raw)
                    .map_err(|e| format!("line {}: invalid JSON: {e}", index + 1))?;
                if output_count > 0 {
                    output.push(',');
                }
                output
                    .push_str(&serde_json::to_string(&slim_gap(&row)).map_err(|e| e.to_string())?);
                output_count += 1;
            }
            "execution" | "summary" => {}
            other => return Err(format!("unknown record {other:?}")),
        }
    }
    output.push(']');
    Ok((output, output_count))
}

fn render(input: &Path, output: &Path, max_snapshots: usize) -> Result<(u64, u64), String> {
    if max_snapshots == 0 {
        return Err("--max-snapshots must be positive".into());
    }
    if input.exists()
        && output.exists()
        && fs::canonicalize(input).ok() == fs::canonicalize(output).ok()
    {
        return Err("--out must not overwrite experiment.jsonl".into());
    }
    let (mut descriptor, final_row, snapshot_count, gap_count, accumulator) = first_pass(input)?;
    let selected = selected_ordinals(snapshot_count, max_snapshots);
    let (timeline_json, embedded_records) = second_pass(input, &selected)?;
    let mut aggregates = accumulator.value();
    let events = aggregates
        .get_mut("events")
        .map(Value::take)
        .unwrap_or_else(|| json!([]));
    if let Some(object) = aggregates.as_object_mut() {
        object.remove("events");
    }
    let report_view = json!({"source_records": snapshot_count + gap_count, "source_snapshots": snapshot_count, "embedded_records": embedded_records, "embedded_snapshots": selected.len(), "snapshot_selection": if snapshot_count <= max_snapshots { "all" } else { "evenly spaced including first and last" }, "all_gap_records_embedded": true, "canonical_detail": "experiment.jsonl retains every active-time snapshot and event", "source_event_count": accumulator.count, "embedded_event_count": aggregates.get("sampled_events"), "event_sampling": aggregates.get("event_sampling")});
    descriptor["report_view"] = report_view;
    descriptor["dashboard_aggregates"] = aggregates.clone();
    let mut payload_descriptor = descriptor;
    if let Some(object) = payload_descriptor.as_object_mut() {
        object.remove("dashboard_aggregates");
    }
    let parent = output.parent().unwrap_or_else(|| Path::new("."));
    fs::create_dir_all(parent).map_err(|e| format!("{}: {e}", parent.display()))?;
    let temp = parent.join(format!(
        ".{}.tmp-{}",
        output
            .file_name()
            .and_then(|n| n.to_str())
            .unwrap_or("report.html"),
        std::process::id()
    ));
    let descriptor_json = serde_json::to_string(&payload_descriptor).map_err(|e| e.to_string())?;
    let final_json = serde_json::to_string(&final_row).map_err(|e| e.to_string())?;
    let events_json = serde_json::to_string(&events).map_err(|e| e.to_string())?;
    let aggregates_json = serde_json::to_string(&aggregates).map_err(|e| e.to_string())?;
    let marker = "__PAYLOAD__";
    let marker_position = TEMPLATE
        .find(marker)
        .ok_or_else(|| "dashboard template has no __PAYLOAD__ marker".to_string())?;
    let payload_prefix = format!("{{\"descriptor\":{descriptor_json},\"timeline\":{timeline_json},\"final\":{final_json},\"events\":{events_json},\"aggregates\":{aggregates_json}}}").replace("</", "<\\/");
    let mut file = File::create(&temp).map_err(|e| format!("{}: {e}", temp.display()))?;
    file.write_all(&TEMPLATE.as_bytes()[..marker_position])
        .map_err(|e| e.to_string())?;
    file.write_all(payload_prefix.as_bytes())
        .map_err(|e| e.to_string())?;
    file.write_all(&TEMPLATE.as_bytes()[marker_position + marker.len()..])
        .map_err(|e| e.to_string())?;
    file.sync_all().ok();
    fs::rename(&temp, output).map_err(|e| format!("{}: {e}", output.display()))?;
    Ok((
        fs::metadata(output).map_err(|e| e.to_string())?.len(),
        accumulator.count,
    ))
}

fn html_escape(value: &str) -> String {
    value
        .replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
        .replace('\'', "&#39;")
}

fn summary_value(summary: &Value, key: &str) -> String {
    summary
        .get(key)
        .map(|value| match value {
            Value::String(s) => s.clone(),
            _ => value.to_string(),
        })
        .unwrap_or_else(|| "(missing)".into())
}

fn atomic_html(output: &Path, html: &str) -> Result<u64, String> {
    let parent = output.parent().unwrap_or_else(|| Path::new("."));
    fs::create_dir_all(parent).map_err(|e| format!("{}: {e}", parent.display()))?;
    let temp = parent.join(format!(
        ".{}.tmp-{}",
        output
            .file_name()
            .and_then(|n| n.to_str())
            .unwrap_or("report.html"),
        std::process::id()
    ));
    let mut file = File::create(&temp).map_err(|e| format!("{}: {e}", temp.display()))?;
    file.write_all(html.as_bytes()).map_err(|e| e.to_string())?;
    file.sync_all().map_err(|e| e.to_string())?;
    fs::rename(&temp, output).map_err(|e| format!("{}: {e}", output.display()))?;
    Ok(html.len() as u64)
}

fn compare(input_a: &Path, input_b: &Path, output: &Path) -> Result<u64, String> {
    let (_a, a_final, a_snapshots, a_gaps, a_events) = first_pass(input_a)?;
    let (_b, b_final, b_snapshots, b_gaps, b_events) = first_pass(input_b)?;
    let sa = a_final
        .get("summary")
        .ok_or("first comparison stream has no summary")?;
    let sb = b_final
        .get("summary")
        .ok_or("second comparison stream has no summary")?;
    let fields = [
        ("scenario", "scenario"),
        ("main protocol", "selected_main_protocol"),
        ("cache wire", "selected_cache_wire"),
        ("codec profile", "selected_codec_profile"),
        ("scored C→F bytes", "scored_outgoing_bytes"),
        ("raw bytes", "raw_bytes"),
        ("F→C bytes", "f_to_c_bytes"),
        ("makespan ns", "makespan_ns"),
        ("active ns", "timeline_active_ns"),
        ("jobs", "jobs"),
    ];
    let mut rows = String::new();
    for (label, key) in fields {
        let av = summary_value(sa, key);
        let bv = summary_value(sb, key);
        rows.push_str(&format!(
            "<tr><th>{}</th><td>{}</td><td>{}</td></tr>",
            html_escape(label),
            html_escape(&av),
            html_escape(&bv)
        ));
    }
    let html = format!(
        r#"<!doctype html><meta charset="utf-8"><title>Icecream comparison</title>
<style>body{{font:14px system-ui;background:#101820;color:#e8eef5;margin:2rem;max-width:1100px}}h1{{font-size:1.5rem}}table{{border-collapse:collapse;width:100%;background:#172432}}th,td{{padding:.55rem;border:1px solid #33485d;text-align:left}}th{{color:#b9c9d8}}.note{{color:#b9c9d8}}code{{overflow-wrap:anywhere}}</style>
<h1>Simulated versus physical experiment</h1><p class="note">Validated source streams; values are retained summary fields, with source paths shown below. Missing values are not inferred.</p>
<table><thead><tr><th>metric</th><th>simulated</th><th>physical</th></tr></thead><tbody>{rows}</tbody></table>
<h2>Sources and coverage</h2><ul><li>simulated: <code>{pa}</code>; {asnap} snapshots, {agap} gaps, {aevent} events</li><li>physical: <code>{pb}</code>; {bsnap} snapshots, {bgap} gaps, {bevent} events</li></ul>
<p class="note">Open each source JSONL with the canonical retained-stream tools for full event, codec, cache, lease, cursor, and provenance detail.</p>"#,
        rows = rows,
        pa = html_escape(&input_a.display().to_string()),
        pb = html_escape(&input_b.display().to_string()),
        asnap = a_snapshots,
        agap = a_gaps,
        aevent = a_events.count,
        bsnap = b_snapshots,
        bgap = b_gaps,
        bevent = b_events.count
    );
    atomic_html(output, &html)
}

fn collect_jsonl(root: &Path, out: &mut Vec<PathBuf>) -> Result<(), String> {
    let entries = fs::read_dir(root).map_err(|e| format!("{}: {e}", root.display()))?;
    for entry in entries {
        let path = entry.map_err(|e| e.to_string())?.path();
        if path.is_dir() {
            collect_jsonl(&path, out)?;
        } else if path.extension().and_then(|x| x.to_str()) == Some("jsonl") {
            out.push(path);
        }
    }
    Ok(())
}

fn index(root: &Path, output: &Path) -> Result<u64, String> {
    let mut files = Vec::new();
    collect_jsonl(root, &mut files)?;
    files.sort();
    let mut rows = String::new();
    for path in files {
        let relative = path.strip_prefix(root).unwrap_or(&path);
        match first_pass(&path) {
            Ok((header, final_row, snapshots, gaps, events)) => {
                let summary = final_row
                    .get("summary")
                    .cloned()
                    .unwrap_or_else(|| json!({}));
                let report = path.with_file_name("report.html");
                let report_cell = if report.exists() {
                    let rel_report = report.strip_prefix(root).unwrap_or(&report);
                    format!(
                        "<a href=\"{}\">report</a>",
                        html_escape(&rel_report.to_string_lossy())
                    )
                } else {
                    "(not rendered)".into()
                };
                rows.push_str(&format!("<tr><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td></tr>",
                    html_escape(&relative.to_string_lossy()), html_escape(&summary_value(&header, "scenario")),
                    html_escape(&summary_value(&summary, "selected_main_protocol")), html_escape(&summary_value(&summary, "selected_codec_profile")),
                    html_escape(&summary_value(&summary, "scored_outgoing_bytes")), html_escape(&summary_value(&summary, "makespan_ns")), snapshots, gaps, report_cell));
                let _ = events;
            }
            Err(error) => rows.push_str(&format!(
                "<tr><td>{}</td><td colspan=\"8\">invalid source: {}</td></tr>",
                html_escape(&relative.to_string_lossy()),
                html_escape(&error)
            )),
        }
    }
    let html = format!(
        r#"<!doctype html><meta charset="utf-8"><title>Icecream experiment index</title>
<style>body{{font:14px system-ui;background:#101820;color:#e8eef5;margin:2rem}}table{{border-collapse:collapse;width:100%}}th,td{{padding:.45rem;border:1px solid #33485d;text-align:left}}th{{color:#b9c9d8;background:#172432}}a{{color:#8fc7ff}}</style>
<h1>Icecream experiment index</h1><p>Root: <code>{root}</code>. Each row was parsed and contract-checked; missing report links mean rendering has not been requested.</p>
<table><thead><tr><th>source</th><th>scenario</th><th>P</th><th>codec</th><th>C→F bytes</th><th>makespan ns</th><th>snapshots</th><th>gaps</th><th>report</th></tr></thead><tbody>{rows}</tbody></table>"#,
        root = html_escape(&root.display().to_string()),
        rows = rows
    );
    atomic_html(output, &html)
}

fn usage() -> ! {
    eprintln!("usage: icecream-dashboard render EXPERIMENT_JSONL --out REPORT [--max-snapshots N]");
    eprintln!("       icecream-dashboard compare SIMULATED_JSONL PHYSICAL_JSONL --out REPORT");
    eprintln!("       icecream-dashboard index RESULTS_ROOT --out INDEX_HTML");
    std::process::exit(2);
}
fn main() {
    let args: Vec<String> = env::args().skip(1).collect();
    if args.is_empty() {
        usage();
    }
    let command = if matches!(args[0].as_str(), "render" | "compare" | "index") {
        args[0].as_str()
    } else {
        "render"
    };
    let offset = if command == "render" && args[0] != "render" {
        0
    } else {
        1
    };
    if command == "compare" {
        if args.len() < 4 {
            usage();
        }
        let a = PathBuf::from(&args[1]);
        let b = PathBuf::from(&args[2]);
        let mut output = None;
        let mut i = 3;
        while i < args.len() {
            if args[i] == "--out" {
                i += 1;
                if i >= args.len() {
                    usage();
                }
                output = Some(PathBuf::from(&args[i]));
            } else {
                usage();
            }
            i += 1;
        }
        let output = output.unwrap_or_else(|| PathBuf::from("comparison.html"));
        match compare(&a, &b, &output) {
            Ok(bytes) => println!("{} ({} bytes HTML)", output.display(), bytes),
            Err(error) => {
                eprintln!("error: {error}");
                std::process::exit(1);
            }
        }
        return;
    }
    if command == "index" {
        if args.len() < 2 {
            usage();
        }
        let root = PathBuf::from(&args[1]);
        let mut output = None;
        let mut i = 2;
        while i < args.len() {
            if args[i] == "--out" {
                i += 1;
                if i >= args.len() {
                    usage();
                }
                output = Some(PathBuf::from(&args[i]));
            } else {
                usage();
            }
            i += 1;
        }
        let output = output.unwrap_or_else(|| root.join("index.html"));
        match index(&root, &output) {
            Ok(bytes) => println!("{} ({} bytes HTML)", output.display(), bytes),
            Err(error) => {
                eprintln!("error: {error}");
                std::process::exit(1);
            }
        }
        return;
    }
    let input = PathBuf::from(&args[offset]);
    let mut output = None;
    let mut limit = MAX_REPORT_SNAPSHOTS;
    let mut i = offset + 1;
    while i < args.len() {
        match args[i].as_str() {
            "--out" => {
                i += 1;
                if i >= args.len() {
                    usage();
                }
                output = Some(PathBuf::from(&args[i]));
            }
            "--max-snapshots" => {
                i += 1;
                if i >= args.len() {
                    usage();
                }
                limit = args[i].parse().unwrap_or_else(|_| usage());
            }
            _ => usage(),
        }
        i += 1;
    }
    let output = output.unwrap_or_else(|| input.with_file_name("report.html"));
    match render(&input, &output, limit) {
        Ok((bytes, events)) => println!(
            "{} ({} bytes HTML; {} events aggregated)",
            output.display(),
            bytes,
            events
        ),
        Err(error) => {
            eprintln!("error: {error}");
            std::process::exit(1);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn selection_keeps_boundaries() {
        assert_eq!(selected_ordinals(10_000, 3), vec![0, 4999, 9999]);
        assert_eq!(selected_ordinals(2, 2000), vec![0, 1]);
    }
    #[test]
    fn accumulator_does_not_duplicate_small_trace() {
        let mut a = EventAccumulator::new();
        a.add(&json!({"event":"x"}));
        a.add(&json!({"event":"y"}));
        assert_eq!(a.value()["sampled_events"], 2);
    }
    #[test]
    fn phase_bytes_use_queued_only() {
        let mut a = EventAccumulator::new();
        a.add(&json!({"event":"flow-queued","phase":"dict","direction":"c_to_f","bytes":11}));
        a.add(&json!({"event":"flow-finish","phase":"dict","direction":"c_to_f","bytes":11}));
        assert_eq!(a.value()["phases"][0]["bytes"], 11);
    }
    #[test]
    fn render_smoke_is_bounded_and_atomic() {
        let root =
            std::env::temp_dir().join(format!("icecream-dashboard-test-{}", std::process::id()));
        let input = root.join("experiment.jsonl");
        let output = root.join("nested/report.html");
        fs::create_dir_all(&root).unwrap();
        let descriptor = json!({"record":"execution","schema":"icecream-execution-v2","scenario":"test","topology_dimensions":{"f_stores":1,"logical_c_authorities":1}});
        let snapshot = json!({"record":"snapshot","sequence":0,"wall_start_ns":0,"wall_end_ns":1,"wall_duration_ns":1,"active_start_ns":0,"active_end_ns":1,"active_duration_ns":1,"event_sequence_start":0,"event_sequence_end":0,"noncanonical_display":{"state":{"scheduler":{},"network":{},"c":[],"f":[]},"metrics":{"routes":[],"workers":[]}},"events":[{"event":"flow-queued","phase":"dict","direction":"c_to_f","bytes":3,"time_ns":1,"build":0,"worker":0}]});
        let summary = json!({"record":"summary","schema":"icecream-experiment-summary-v2","event_count":1,"timeline_records":1,"summary":{"workers":1,"environments":1,"build_epochs":1}});
        fs::write(
            &input,
            format!("{}\n{}\n{}\n", descriptor, snapshot, summary),
        )
        .unwrap();
        let (bytes, events) = render(&input, &output, 1).unwrap();
        assert!(bytes > 1000);
        assert_eq!(events, 1);
        assert!(output.is_file());
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn accepted_fixture_compare_index_and_reproducibility() {
        let source = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../samples/r4-minimum/sample-experiment.jsonl");
        let root = std::env::temp_dir().join(format!(
            "icecream-dashboard-accepted-{}",
            std::process::id()
        ));
        fs::create_dir_all(root.join("runs")).unwrap();
        let copied = root.join("runs/fixture.jsonl");
        fs::copy(&source, &copied).unwrap();
        let one = root.join("one.html");
        let two = root.join("two.html");
        render(&source, &one, MAX_REPORT_SNAPSHOTS).unwrap();
        render(&source, &two, MAX_REPORT_SNAPSHOTS).unwrap();
        assert_eq!(fs::read(&one).unwrap(), fs::read(&two).unwrap());
        compare(&source, &copied, &root.join("compare.html")).unwrap();
        index(&root.join("runs"), &root.join("index.html")).unwrap();
        assert!(fs::read_to_string(root.join("compare.html"))
            .unwrap()
            .contains("Simulated versus physical"));
        assert!(fs::read_to_string(root.join("index.html"))
            .unwrap()
            .contains("fixture.jsonl"));
        let html = fs::read_to_string(one).unwrap();
        assert!(!html.contains("http://") && !html.contains("https://"));
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn invalid_source_is_rejected_before_rendering() {
        let root =
            std::env::temp_dir().join(format!("icecream-dashboard-invalid-{}", std::process::id()));
        fs::create_dir_all(&root).unwrap();
        let input = root.join("bad.jsonl");
        fs::write(&input, "{\"record\":\"execution\",\"schema\":\"icecream-execution-v2\"}\n{\"record\":\"snapshot\"}\n").unwrap();
        assert!(render(&input, &root.join("bad.html"), 10).is_err());
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn large_synthetic_projection_stays_bounded() {
        let root =
            std::env::temp_dir().join(format!("icecream-dashboard-large-{}", std::process::id()));
        fs::create_dir_all(&root).unwrap();
        let input = root.join("large.jsonl");
        let mut stream = String::new();
        stream.push_str(&json!({"record":"execution","schema":"icecream-execution-v2","scenario":"synthetic","topology_dimensions":{"f_stores":8,"logical_c_authorities":1}}).to_string());
        stream.push('\n');
        for i in 0..3_000u64 {
            stream.push_str(&json!({"record":"snapshot","sequence":i,"wall_start_ns":i,"wall_end_ns":i+1,"wall_duration_ns":1,"active_start_ns":i,"active_end_ns":i+1,"active_duration_ns":1,"event_sequence_start":"","event_sequence_end":"","noncanonical_display":{"state":{"scheduler":{"ready_tus":i%7,"active_tus":1,"completed_tus":i},"network":{},"c":[],"f":[]},"metrics":{"routes":[],"workers":[]}},"events":[]}).to_string());
            stream.push('\n');
        }
        stream.push_str(&json!({"record":"summary","schema":"icecream-experiment-summary-v2","event_count":0,"timeline_records":3_000,"summary":{"scenario":"synthetic","workers":8,"environments":1,"build_epochs":1,"scored_outgoing_bytes":0,"makespan_ns":3_000}}).to_string());
        stream.push('\n');
        fs::write(&input, stream).unwrap();
        let output = root.join("large.html");
        render(&input, &output, 2_000).unwrap();
        assert!(fs::metadata(output).unwrap().len() < 5_000_000);
        let _ = fs::remove_dir_all(root);
    }
}
