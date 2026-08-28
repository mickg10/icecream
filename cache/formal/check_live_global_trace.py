#!/usr/bin/env python3
"""Replay production GlobalResourceTrace JSONL emitted by P50ServerEndpoint."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


ACTIONS = {
    "NAMESPACE_ADMITTED",
    "NAMESPACE_TOUCHED",
    "TU_STARTED",
    "TU_FINISHED",
    "ARENA_INSTALLING",
    "ARENA_RETRY_INSTALLING",
    "ARENA_PRESENT",
    "ARENA_PINNED",
    "ARENA_UNPINNED",
    "ARENA_RELEASED",
    "INSTALL_CRASHED",
    "CONTENT_CONFLICT_FATAL",
    "NAMESPACE_EVICTED",
    "GENERATION_ADVANCED",
    "GENERATION_WRAP_STOPPED",
    "C_GUID_FLIPPED",
}
FIELDS = {
    "action",
    "c_store_guid",
    "previous_c_store_guid",
    "generation",
    "key64",
    "slot",
    "bytes",
    "lru",
    "content_digest",
}
REQUIRED_ENDPOINT_ACTIONS = {
    "NAMESPACE_ADMITTED",
    "NAMESPACE_TOUCHED",
    "TU_STARTED",
    "TU_FINISHED",
    "ARENA_INSTALLING",
    "ARENA_PRESENT",
    "ARENA_RELEASED",
    "NAMESPACE_EVICTED",
}
ZERO128 = "0" * 32
MAX_GENERATION = (1 << 10) - 1
ORDINAL_MASK = (1 << 49) - 1


def object_pairs_no_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON field {key!r}")
        result[key] = value
    return result


def valid_hex128(value: Any) -> bool:
    return (
        isinstance(value, str)
        and len(value) == 32
        and all(character in "0123456789abcdef" for character in value)
    )


def require_uint(row: dict[str, Any], field: str, line: int) -> int:
    value = row[field]
    if not isinstance(value, int) or isinstance(value, bool) or value < 0 or value >= 1 << 64:
        raise ValueError(f"line {line}: {field} is not an unsigned 64-bit integer")
    return value


def check(path: Path, require_endpoint_lifecycle: bool = True) -> None:
    namespaces: dict[str, dict[str, Any]] = {}
    slots: dict[int, tuple[str, int]] = {}
    observed: set[str] = set()
    last_lru = 0
    rows = path.read_text(encoding="utf-8").splitlines()
    if not rows:
        raise ValueError("production global trace is empty")

    def namespace(guid: str, line: int) -> dict[str, Any]:
        current = namespaces.get(guid)
        if current is None:
            raise ValueError(f"line {line}: action references an unknown namespace")
        return current

    def require_live(current: dict[str, Any], line: int) -> None:
        if not current["live"]:
            raise ValueError(f"line {line}: action references an absent namespace")

    for line, raw in enumerate(rows, 1):
        if not raw:
            raise ValueError(f"line {line}: blank production trace record")
        row = json.loads(raw, object_pairs_hook=object_pairs_no_duplicates)
        if not isinstance(row, dict) or set(row) != FIELDS:
            raise ValueError(f"line {line}: production trace fields differ from the closed schema")
        action = row["action"]
        if action not in ACTIONS:
            raise ValueError(f"line {line}: unknown production global action {action!r}")
        guid = row["c_store_guid"]
        previous_guid = row["previous_c_store_guid"]
        digest = row["content_digest"]
        if not valid_hex128(guid) or not valid_hex128(previous_guid) or not valid_hex128(digest):
            raise ValueError(f"line {line}: malformed 128-bit production identity")
        generation = require_uint(row, "generation", line)
        key = require_uint(row, "key64", line)
        slot = require_uint(row, "slot", line)
        byte_count = require_uint(row, "bytes", line)
        lru = require_uint(row, "lru", line)
        if generation > MAX_GENERATION:
            raise ValueError(f"line {line}: generation exceeds the Key64 layout")
        observed.add(action)

        if action == "NAMESPACE_ADMITTED":
            if guid == ZERO128 or previous_guid != ZERO128:
                raise ValueError(f"line {line}: invalid admitted namespace identity")
            current = namespaces.get(guid)
            if current is None:
                current = {
                    "live": False,
                    "active": False,
                    "stopped": False,
                    "generation": generation,
                    "lru": 0,
                    "objects": {},
                }
                namespaces[guid] = current
            if current["live"] or current["stopped"] or current["generation"] != generation:
                raise ValueError(f"line {line}: namespace admission changed lifecycle identity")
            current["live"] = True

        elif action == "C_GUID_FLIPPED":
            if guid == ZERO128 or previous_guid == ZERO128 or guid in namespaces:
                raise ValueError(f"line {line}: GUID flip did not create a fresh namespace")
            previous = namespace(previous_guid, line)
            if not previous["stopped"]:
                raise ValueError(f"line {line}: GUID flip preceded generation stop")
            namespaces[guid] = {
                "live": False,
                "active": False,
                "stopped": False,
                "generation": 0,
                "lru": 0,
                "objects": {},
            }

        else:
            current = namespace(guid, line)
            if action != "GENERATION_ADVANCED" and generation != current["generation"]:
                raise ValueError(f"line {line}: action generation changed namespace identity")

            if action == "NAMESPACE_TOUCHED":
                require_live(current, line)
                if lru <= last_lru:
                    raise ValueError(f"line {line}: global LRU clock did not increase")
                current["lru"] = lru
                last_lru = lru

            elif action == "TU_STARTED":
                require_live(current, line)
                if current["active"]:
                    raise ValueError(f"line {line}: overlapping global TU")
                current["active"] = True

            elif action == "TU_FINISHED":
                if not current["active"] or any(
                    obj["state"] == "INSTALLING" for obj in current["objects"].values()
                ):
                    raise ValueError(f"line {line}: TU finished with invalid global state")
                current["active"] = False
                for obj in current["objects"].values():
                    if obj["state"] == "PINNED":
                        obj["state"] = "PRESENT"

            elif action in {"ARENA_INSTALLING", "ARENA_RETRY_INSTALLING"}:
                require_live(current, line)
                if not current["active"] or key == 0 or byte_count == 0:
                    raise ValueError(f"line {line}: invalid production install identity")
                if ((key >> 49) & MAX_GENERATION) != generation or (key & ORDINAL_MASK) == 0:
                    raise ValueError(f"line {line}: Key64 does not bind the namespace generation")
                obj = current["objects"].get(key)
                if obj is not None and obj["state"] != "ABSENT":
                    raise ValueError(f"line {line}: install did not start from ABSENT")
                crashed = bool(obj and obj.get("crashed"))
                if (action == "ARENA_RETRY_INSTALLING") != crashed:
                    raise ValueError(f"line {line}: retry/crash identity mismatch")
                if slot in slots:
                    raise ValueError(f"line {line}: staging slot has competing owners")
                current["objects"][key] = {
                    "state": "INSTALLING",
                    "slot": slot,
                    "bytes": byte_count,
                    "digest": digest,
                    "crashed": False,
                }
                slots[slot] = (guid, key)

            elif action == "ARENA_PRESENT":
                obj = current["objects"].get(key)
                if obj is None or obj["state"] != "INSTALLING":
                    raise ValueError(f"line {line}: PRESENT lacks INSTALLING")
                if slots.get(slot) != (guid, key) or obj["slot"] != slot:
                    raise ValueError(f"line {line}: PRESENT lost its staging slot")
                if obj["digest"] != digest or obj["bytes"] != byte_count:
                    raise ValueError(f"line {line}: staged content changed before PRESENT")
                obj["state"] = "PRESENT"
                del slots[slot]

            elif action == "INSTALL_CRASHED":
                obj = current["objects"].get(key)
                if obj is None or obj["state"] != "INSTALLING" or slots.get(slot) != (guid, key):
                    raise ValueError(f"line {line}: crash lost INSTALLING ownership")
                if obj["bytes"] != byte_count:
                    raise ValueError(f"line {line}: crash changed staged byte accounting")
                current["objects"][key] = {"state": "ABSENT", "crashed": True}
                del slots[slot]

            elif action == "ARENA_PINNED":
                obj = current["objects"].get(key)
                if not current["active"] or obj is None or obj["state"] != "PRESENT":
                    raise ValueError(f"line {line}: PINNED lacks active PRESENT object")
                obj["state"] = "PINNED"

            elif action == "ARENA_UNPINNED":
                obj = current["objects"].get(key)
                if obj is None or obj["state"] != "PINNED":
                    raise ValueError(f"line {line}: UNPINNED lacks PINNED object")
                obj["state"] = "PRESENT"

            elif action == "ARENA_RELEASED":
                obj = current["objects"].get(key)
                if obj is None or obj["state"] not in {"PRESENT", "PINNED"}:
                    raise ValueError(f"line {line}: RELEASED lacks a resident object")
                if obj["bytes"] != byte_count:
                    raise ValueError(f"line {line}: release changed resident byte accounting")
                current["objects"][key] = {"state": "ABSENT", "crashed": False}

            elif action == "CONTENT_CONFLICT_FATAL":
                obj = current["objects"].get(key)
                if obj is None or obj["state"] not in {"PRESENT", "PINNED"} or obj["digest"] == digest:
                    raise ValueError(f"line {line}: conflict is not same-key/different-content")

            elif action == "NAMESPACE_EVICTED":
                require_live(current, line)
                if current["active"] or any(
                    obj["state"] in {"INSTALLING", "PINNED"}
                    for obj in current["objects"].values()
                ):
                    raise ValueError(f"line {line}: eviction crossed live global work")
                eligible_lru = [
                    other["lru"]
                    for other in namespaces.values()
                    if other["live"] and not other["active"]
                    and not any(
                        obj["state"] in {"INSTALLING", "PINNED"}
                        for obj in other["objects"].values()
                    )
                ]
                if eligible_lru and current["lru"] != min(eligible_lru):
                    raise ValueError(f"line {line}: eviction did not select global LRU")
                if lru != current["lru"]:
                    raise ValueError(f"line {line}: eviction changed the LRU witness")
                current["live"] = False
                current["objects"] = {}

            elif action == "GENERATION_ADVANCED":
                if current["live"] or current["stopped"] or generation == 0:
                    raise ValueError(f"line {line}: invalid generation advance")
                # The emitted row carries the new generation, so compare with
                # the value before applying the action.
                if generation != current["generation"] + 1:
                    raise ValueError(f"line {line}: generation did not advance by one")
                current["generation"] = generation

            elif action == "GENERATION_WRAP_STOPPED":
                if current["live"] or current["stopped"] or generation == 0:
                    raise ValueError(f"line {line}: invalid production generation stop")
                current["stopped"] = True

    if slots:
        raise ValueError("production trace ended with owned staging slots")
    if require_endpoint_lifecycle:
        missing = sorted(REQUIRED_ENDPOINT_ACTIONS - observed)
        if missing:
            raise ValueError(f"production endpoint trace omitted required actions: {missing}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument(
        "--allow-partial",
        action="store_true",
        help="replay a partial production trace without the endpoint lifecycle census",
    )
    arguments = parser.parse_args()
    check(arguments.trace, require_endpoint_lifecycle=not arguments.allow_partial)
    print("check_live_global_trace.py: production endpoint trace passed")


if __name__ == "__main__":
    main()
