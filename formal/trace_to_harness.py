#!/usr/bin/env python3
"""
Validate TLC JSON counterexample traces and compile their essential transition
sequence into deterministic harness instructions.

TLC's ``-dumpTrace json`` output is a JSON array of state records.  It does not
carry action labels, so this adapter classifies adjacent state pairs with a
small declarative manifest.  No Python expressions are evaluated: manifests
only contain path/value/delta predicates.

A manifest can prove that an expected counterexample is discriminating by
requiring:

* a named essential event subsequence;
* final-state predicates representing the intended property violation;
* optional forbidden events;
* an optional lasso marker parsed from the retained TLC text log.

The emitted JSON is intentionally generic.  C++ integration fixtures consume
the named ``harness_steps`` and map them to pipes, eventfds, or control-message
barriers.  The adapter does not claim a TLC run passed; it rejects empty,
malformed, ambiguous, or incorrectly ordered traces.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Mapping, Sequence


SCHEMA_VERSION = 1
_MISSING = object()


class TraceValidationError(RuntimeError):
    """A trace or manifest failed a load-bearing validation."""


@dataclass(frozen=True)
class ClassifiedEvent:
    transition_index: int
    name: str
    changed_paths: tuple[str, ...]


def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _read_json(path: Path) -> Any:
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise TraceValidationError(f"cannot read {path}: {exc}") from exc
    try:
        return json.loads(raw), _sha256_bytes(raw)
    except json.JSONDecodeError as exc:
        raise TraceValidationError(
            f"{path}: invalid JSON at line {exc.lineno}, column {exc.colno}: {exc.msg}"
        ) from exc


def _normalize_states(document: Any) -> list[dict[str, Any]]:
    """Accept TLC's array plus a few explicit wrapper shapes used by fixtures."""
    states: Any
    if isinstance(document, list):
        states = document
    elif isinstance(document, dict):
        for key in ("states", "trace", "counterexample"):
            if key in document:
                states = document[key]
                break
        else:
            raise TraceValidationError(
                "trace object must contain one of: states, trace, counterexample"
            )
        if isinstance(states, dict):
            # Some wrappers use {"states": [...]} under "counterexample".
            states = states.get("states", states)
    else:
        raise TraceValidationError("trace root must be a JSON array or object")

    if not isinstance(states, list):
        raise TraceValidationError("normalized trace is not a JSON array")
    if not states:
        raise TraceValidationError("trace contains zero states")
    normalized: list[dict[str, Any]] = []
    for idx, state in enumerate(states, start=1):
        if not isinstance(state, dict):
            raise TraceValidationError(f"state {idx} is not a JSON object")
        normalized.append(state)
    return normalized


_PATH_TOKEN = re.compile(
    r"""
    (?:
        ^|\.                             # dot-separated object key
    )
    (?P<key>[^.\[\]]+)
    |
    \[(?P<index>[0-9]+)\]                # zero-based array index
""",
    re.VERBOSE,
)


def _path_tokens(path: str) -> list[str | int]:
    if not isinstance(path, str) or not path:
        raise TraceValidationError("condition path must be a non-empty string")
    if path.startswith("/"):
        # RFC 6901 JSON Pointer.
        result: list[str | int] = []
        for raw in path.split("/")[1:]:
            token = raw.replace("~1", "/").replace("~0", "~")
            result.append(int(token) if token.isdigit() else token)
        return result

    tokens: list[str | int] = []
    consumed = 0
    for match in _PATH_TOKEN.finditer(path):
        if match.start() != consumed:
            raise TraceValidationError(f"invalid path syntax: {path!r}")
        consumed = match.end()
        key = match.group("key")
        if key is not None:
            tokens.append(key)
        else:
            tokens.append(int(match.group("index")))
    if consumed != len(path) or not tokens:
        raise TraceValidationError(f"invalid path syntax: {path!r}")
    return tokens


def _get_path(document: Any, path: str, default: Any = _MISSING) -> Any:
    value = document
    try:
        for token in _path_tokens(path):
            if isinstance(token, int):
                if not isinstance(value, list):
                    raise KeyError(token)
                value = value[token]
            else:
                if not isinstance(value, dict):
                    raise KeyError(token)
                value = value[token]
        return value
    except (KeyError, IndexError):
        if default is not _MISSING:
            return default
        raise TraceValidationError(f"path {path!r} is absent from a state")


def _number(value: Any, *, context: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise TraceValidationError(f"{context} requires a numeric value, got {value!r}")
    return float(value)


def _condition_matches(
    condition: Mapping[str, Any],
    previous: Mapping[str, Any],
    current: Mapping[str, Any],
    *,
    strict_paths: bool = False,
) -> bool:
    allowed = {
        "path",
        "from",
        "to",
        "eq",
        "from_in",
        "to_in",
        "changed",
        "unchanged",
        "delta",
        "delta_gt",
        "delta_ge",
        "delta_lt",
        "delta_le",
        "from_len",
        "to_len",
        "len_delta",
        "exists",
    }
    unknown = set(condition) - allowed
    if unknown:
        raise TraceValidationError(f"unknown condition keys: {sorted(unknown)}")
    path = condition.get("path")
    if not isinstance(path, str):
        raise TraceValidationError("every condition requires a string path")

    before = _get_path(previous, path, _MISSING)
    after = _get_path(current, path, _MISSING)
    if strict_paths and (before is _MISSING or after is _MISSING):
        raise TraceValidationError(f"required path {path!r} is absent")
    if condition.get("exists") is True:
        return after is not _MISSING
    if condition.get("exists") is False:
        return after is _MISSING
    if before is _MISSING or after is _MISSING:
        return False

    if "from" in condition and before != condition["from"]:
        return False
    if "to" in condition and after != condition["to"]:
        return False
    if "eq" in condition and after != condition["eq"]:
        return False
    if "from_in" in condition:
        choices = condition["from_in"]
        if not isinstance(choices, list) or before not in choices:
            return False
    if "to_in" in condition:
        choices = condition["to_in"]
        if not isinstance(choices, list) or after not in choices:
            return False
    if condition.get("changed") is True and before == after:
        return False
    if condition.get("changed") is False and before != after:
        return False
    if condition.get("unchanged") is True and before != after:
        return False

    if "from_len" in condition:
        if not hasattr(before, "__len__") or len(before) != condition["from_len"]:
            return False
    if "to_len" in condition:
        if not hasattr(after, "__len__") or len(after) != condition["to_len"]:
            return False
    if "len_delta" in condition:
        if not hasattr(before, "__len__") or not hasattr(after, "__len__"):
            return False
        if len(after) - len(before) != condition["len_delta"]:
            return False

    if any(
        key in condition
        for key in ("delta", "delta_gt", "delta_ge", "delta_lt", "delta_le")
    ):
        delta = _number(after, context=f"{path} after") - _number(
            before, context=f"{path} before"
        )
        if "delta" in condition and delta != float(condition["delta"]):
            return False
        if "delta_gt" in condition and not delta > float(condition["delta_gt"]):
            return False
        if "delta_ge" in condition and not delta >= float(condition["delta_ge"]):
            return False
        if "delta_lt" in condition and not delta < float(condition["delta_lt"]):
            return False
        if "delta_le" in condition and not delta <= float(condition["delta_le"]):
            return False

    return True


def _event_matches(
    definition: Mapping[str, Any],
    previous: Mapping[str, Any],
    current: Mapping[str, Any],
) -> bool:
    if not isinstance(definition.get("name"), str) or not definition["name"]:
        raise TraceValidationError("every event definition needs a non-empty name")
    all_conditions = definition.get("all", [])
    any_conditions = definition.get("any", [])
    none_conditions = definition.get("none", [])
    if not all(
        isinstance(group, list)
        for group in (all_conditions, any_conditions, none_conditions)
    ):
        raise TraceValidationError(
            f"event {definition['name']}: all/any/none must be arrays"
        )
    if not all(
        isinstance(c, dict)
        for group in (all_conditions, any_conditions, none_conditions)
        for c in group
    ):
        raise TraceValidationError(
            f"event {definition['name']}: conditions must be objects"
        )
    if not all(
        _condition_matches(c, previous, current) for c in all_conditions
    ):
        return False
    if any_conditions and not any(
        _condition_matches(c, previous, current) for c in any_conditions
    ):
        return False
    if any(
        _condition_matches(c, previous, current) for c in none_conditions
    ):
        return False
    return bool(all_conditions or any_conditions)


def _flatten_changed_paths(
    before: Any,
    after: Any,
    *,
    prefix: str = "",
    limit: int = 64,
) -> tuple[str, ...]:
    changed: list[str] = []

    def walk(left: Any, right: Any, path: str) -> None:
        if len(changed) >= limit:
            return
        if isinstance(left, dict) and isinstance(right, dict):
            for key in sorted(set(left) | set(right)):
                child = f"{path}.{key}" if path else str(key)
                if key not in left or key not in right:
                    changed.append(child)
                else:
                    walk(left[key], right[key], child)
            return
        if isinstance(left, list) and isinstance(right, list):
            if len(left) != len(right):
                changed.append(path or "$")
                return
            for idx, (lval, rval) in enumerate(zip(left, right)):
                walk(lval, rval, f"{path}[{idx}]")
            return
        if left != right:
            changed.append(path or "$")

    walk(before, after, prefix)
    if len(changed) >= limit:
        changed.append("<change-list-truncated>")
    return tuple(changed)


def _classify(
    states: Sequence[Mapping[str, Any]],
    event_definitions: Sequence[Mapping[str, Any]],
    *,
    event_mode: str,
    require_all: bool,
) -> tuple[list[ClassifiedEvent], list[dict[str, Any]]]:
    if event_mode not in {"exclusive", "first", "all"}:
        raise TraceValidationError(
            "event_mode must be one of: exclusive, first, all"
        )
    events: list[ClassifiedEvent] = []
    unclassified: list[dict[str, Any]] = []
    for idx in range(1, len(states)):
        previous = states[idx - 1]
        current = states[idx]
        matches = [
            definition["name"]
            for definition in event_definitions
            if _event_matches(definition, previous, current)
        ]
        changed = _flatten_changed_paths(previous, current)
        if not matches:
            if previous != current:
                unclassified.append(
                    {
                        "transition_index": idx,
                        "changed_paths": list(changed),
                    }
                )
            continue
        if event_mode == "exclusive" and len(matches) != 1:
            raise TraceValidationError(
                f"transition {idx}: ambiguous event classification {matches}; "
                f"changed paths: {list(changed)}"
            )
        selected = matches[:1] if event_mode in {"exclusive", "first"} else matches
        for name in selected:
            events.append(
                ClassifiedEvent(
                    transition_index=idx,
                    name=name,
                    changed_paths=changed,
                )
            )
    if require_all and unclassified:
        first = unclassified[0]
        raise TraceValidationError(
            "unclassified non-stuttering transition "
            f"{first['transition_index']}: {first['changed_paths']}"
        )
    return events, unclassified


def _find_subsequence(sequence: Sequence[str], required: Sequence[str]) -> list[int]:
    positions: list[int] = []
    cursor = 0
    for wanted in required:
        while cursor < len(sequence) and sequence[cursor] != wanted:
            cursor += 1
        if cursor >= len(sequence):
            raise TraceValidationError(
                f"required event subsequence missing {wanted!r}; "
                f"classified sequence is {list(sequence)!r}"
            )
        positions.append(cursor)
        cursor += 1
    return positions


def _find_contiguous(sequence: Sequence[str], required: Sequence[str]) -> list[int]:
    if not required:
        return []
    width = len(required)
    for start in range(0, len(sequence) - width + 1):
        if list(sequence[start : start + width]) == list(required):
            return list(range(start, start + width))
    raise TraceValidationError(
        f"required contiguous sequence {list(required)!r} not found in "
        f"{list(sequence)!r}"
    )


_LASSO_RE = re.compile(r"\bBack\s+to\s+state\s+([0-9]+)\b", re.IGNORECASE)
_STUTTER_RE = re.compile(r"\bState\s+([0-9]+)\s*:\s*Stuttering\b", re.IGNORECASE)


def _parse_lasso(log_path: Path | None) -> dict[str, Any] | None:
    if log_path is None:
        return None
    try:
        text = log_path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        raise TraceValidationError(f"cannot read TLC log {log_path}: {exc}") from exc
    matches = list(_LASSO_RE.finditer(text))
    if matches:
        return {
            "kind": "back-edge",
            "target_state_ordinal": int(matches[-1].group(1)),
        }
    matches = list(_STUTTER_RE.finditer(text))
    if matches:
        return {
            "kind": "stuttering",
            "state_ordinal": int(matches[-1].group(1)),
        }
    return None


def _validate_final(
    states: Sequence[Mapping[str, Any]],
    conditions: Sequence[Mapping[str, Any]],
) -> None:
    final = states[-1]
    for condition in conditions:
        # Final predicates compare the final state with itself; use "to"/"eq"
        # or exists/length predicates.  "from" is allowed and has the same
        # value, useful for shared condition fragments.
        if not _condition_matches(condition, final, final, strict_paths=True):
            raise TraceValidationError(
                f"final-state predicate failed: {dict(condition)!r}"
            )


def _resolve_harness_steps(
    harness_steps: Sequence[Mapping[str, Any]],
    events: Sequence[ClassifiedEvent],
) -> list[dict[str, Any]]:
    by_name: dict[str, list[ClassifiedEvent]] = {}
    for event in events:
        by_name.setdefault(event.name, []).append(event)

    resolved: list[dict[str, Any]] = []
    for raw in harness_steps:
        if not isinstance(raw, dict):
            raise TraceValidationError("harness_steps entries must be objects")
        after = raw.get("after")
        emit = raw.get("emit")
        occurrence = raw.get("occurrence", 1)
        if not isinstance(after, str) or not isinstance(emit, str):
            raise TraceValidationError(
                "harness step requires string fields 'after' and 'emit'"
            )
        if not isinstance(occurrence, int) or occurrence < 1:
            raise TraceValidationError("harness step occurrence must be >= 1")
        candidates = by_name.get(after, [])
        if len(candidates) < occurrence:
            raise TraceValidationError(
                f"harness step {emit!r}: event {after!r} occurrence "
                f"{occurrence} is absent"
            )
        event = candidates[occurrence - 1]
        item = {
            "after_event": after,
            "event_transition_index": event.transition_index,
            "emit": emit,
        }
        for key in ("actor", "action", "notes", "arguments"):
            if key in raw:
                item[key] = raw[key]
        resolved.append(item)
    return resolved


def validate_trace(
    trace_path: Path,
    manifest_path: Path,
    *,
    tlc_log_path: Path | None = None,
) -> dict[str, Any]:
    trace_document, trace_sha = _read_json(trace_path)
    manifest, manifest_sha = _read_json(manifest_path)
    if not isinstance(manifest, dict):
        raise TraceValidationError("manifest root must be a JSON object")
    if manifest.get("schema") != SCHEMA_VERSION:
        raise TraceValidationError(
            f"unsupported manifest schema {manifest.get('schema')!r}; "
            f"expected {SCHEMA_VERSION}"
        )
    scenario = manifest.get("scenario")
    if not isinstance(scenario, str) or not scenario:
        raise TraceValidationError("manifest requires a non-empty scenario")

    states = _normalize_states(trace_document)
    minimum = manifest.get("min_states", 2)
    if not isinstance(minimum, int) or minimum < 1:
        raise TraceValidationError("min_states must be a positive integer")
    if len(states) < minimum:
        raise TraceValidationError(
            f"trace has {len(states)} states, fewer than required {minimum}"
        )

    definitions = manifest.get("events", [])
    if not isinstance(definitions, list) or not all(
        isinstance(item, dict) for item in definitions
    ):
        raise TraceValidationError("events must be an array of objects")
    names = [item.get("name") for item in definitions]
    if len(names) != len(set(names)):
        raise TraceValidationError("event names must be unique")

    events, unclassified = _classify(
        states,
        definitions,
        event_mode=manifest.get("event_mode", "exclusive"),
        require_all=bool(manifest.get("require_all_transitions_classified", False)),
    )
    sequence = [event.name for event in events]

    required = manifest.get("required_subsequence", [])
    if not isinstance(required, list) or not all(
        isinstance(item, str) for item in required
    ):
        raise TraceValidationError("required_subsequence must be an array of strings")
    if manifest.get("required_contiguous", False):
        subsequence_positions = _find_contiguous(sequence, required)
    else:
        subsequence_positions = _find_subsequence(sequence, required)

    forbidden = manifest.get("forbidden_events", [])
    if not isinstance(forbidden, list) or not all(
        isinstance(item, str) for item in forbidden
    ):
        raise TraceValidationError("forbidden_events must be an array of strings")
    present_forbidden = sorted(set(sequence) & set(forbidden))
    if present_forbidden:
        raise TraceValidationError(
            f"forbidden classified events present: {present_forbidden}"
        )

    final_conditions = manifest.get("final_all", [])
    if not isinstance(final_conditions, list) or not all(
        isinstance(item, dict) for item in final_conditions
    ):
        raise TraceValidationError("final_all must be an array of condition objects")
    _validate_final(states, final_conditions)

    lasso = _parse_lasso(tlc_log_path)
    if manifest.get("require_lasso", False) and lasso is None:
        raise TraceValidationError(
            "manifest requires a lasso, but no 'Back to state' or stuttering "
            "marker was found in the TLC log"
        )

    harness_steps = manifest.get("harness_steps", [])
    if not isinstance(harness_steps, list):
        raise TraceValidationError("harness_steps must be an array")
    resolved_steps = _resolve_harness_steps(harness_steps, events)

    return {
        "schema": SCHEMA_VERSION,
        "scenario": scenario,
        "property": manifest.get("property"),
        "expected_result": manifest.get("expected_result", "counterexample"),
        "trace": {
            "path": str(trace_path),
            "sha256": trace_sha,
            "state_count": len(states),
        },
        "manifest": {
            "path": str(manifest_path),
            "sha256": manifest_sha,
        },
        "classified_events": [
            {
                "transition_index": event.transition_index,
                "name": event.name,
                "changed_paths": list(event.changed_paths),
            }
            for event in events
        ],
        "event_sequence": sequence,
        "required_subsequence": required,
        "required_subsequence_event_positions": subsequence_positions,
        "unclassified_transitions": unclassified,
        "harness_steps": resolved_steps,
        "lasso": lasso,
        "metadata": manifest.get("metadata", {}),
    }


def _manifest_trace_path(manifest_path: Path, manifest: Mapping[str, Any]) -> Path:
    raw = manifest.get("trace")
    if not isinstance(raw, str) or not raw:
        raise TraceValidationError(
            f"{manifest_path}: check-all manifest requires a non-empty 'trace' path"
        )
    path = Path(raw)
    return path if path.is_absolute() else manifest_path.parent / path


def _manifest_log_path(
    manifest_path: Path, manifest: Mapping[str, Any]
) -> Path | None:
    raw = manifest.get("tlc_log")
    if raw is None:
        return None
    if not isinstance(raw, str) or not raw:
        raise TraceValidationError(
            f"{manifest_path}: tlc_log must be a non-empty string"
        )
    path = Path(raw)
    return path if path.is_absolute() else manifest_path.parent / path


def _check_all(directory: Path) -> list[dict[str, Any]]:
    manifests = sorted(directory.rglob("*.manifest.json"))
    if not manifests:
        raise TraceValidationError(f"{directory}: no *.manifest.json files found")
    results: list[dict[str, Any]] = []
    for manifest_path in manifests:
        manifest, _ = _read_json(manifest_path)
        if not isinstance(manifest, dict):
            raise TraceValidationError(f"{manifest_path}: manifest is not an object")
        trace_path = _manifest_trace_path(manifest_path, manifest)
        log_path = _manifest_log_path(manifest_path, manifest)
        results.append(
            validate_trace(
                trace_path,
                manifest_path,
                tlc_log_path=log_path,
            )
        )
    return results


def _write_output(result: Any, output: Path | None) -> None:
    payload = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if output is None:
        sys.stdout.write(payload)
        return
    try:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(payload, encoding="utf-8")
    except OSError as exc:
        raise TraceValidationError(f"cannot write {output}: {exc}") from exc


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Validate TLC -dumpTrace json output against a declarative "
            "essential-event manifest and emit deterministic harness steps."
        )
    )
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--trace", type=Path, help="TLC JSON trace")
    mode.add_argument(
        "--check-all",
        type=Path,
        metavar="DIR",
        help="validate every *.manifest.json under DIR",
    )
    parser.add_argument("--manifest", type=Path, help="manifest for --trace")
    parser.add_argument(
        "--tlc-log",
        type=Path,
        help="retained TLC text log (required by lasso manifests)",
    )
    parser.add_argument(
        "--output", type=Path, help="write result JSON instead of stdout"
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    try:
        if args.trace is not None:
            if args.manifest is None:
                parser.error("--manifest is required with --trace")
            result: Any = validate_trace(
                args.trace,
                args.manifest,
                tlc_log_path=args.tlc_log,
            )
        else:
            if args.manifest is not None or args.tlc_log is not None:
                parser.error("--manifest/--tlc-log are not used with --check-all")
            result = {
                "schema": SCHEMA_VERSION,
                "results": _check_all(args.check_all),
            }
        _write_output(result, args.output)
        return 0
    except TraceValidationError as exc:
        print(f"trace validation failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
