#!/usr/bin/env python3
"""Level-1 checker for the bounded S3 global-resource action vocabulary.

The checker intentionally owns only the global extension.  Relationship
transaction rows continue to use check_trace.py.  This keeps a missing
global action, stale staging slot, partial namespace eviction, or generation
wrap from being silently ignored by the older per-route checker.
"""

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
    "INSTALL_CRASHED",
    "CONTENT_CONFLICT_FATAL",
    "NAMESPACE_EVICTED",
    "GENERATION_ADVANCED",
    "GENERATION_WRAP_STOPPED",
    "C_GUID_FLIPPED",
}

MAX_GENERATION = 1
MAX_AGGREGATE_BYTES = 6
MAX_NAMESPACE_BYTES = 5
MAX_STAGING_BYTES = 5
MAX_TOTAL_BYTES = 11
SLOTS = {"slot0", "slot1"}
NAMESPACES = {"n0", "n1"}
KEYS = {"k0", "k1"}
GUIDS = {"guid0", "guid1", "guid2"}
CANONICAL = {
    "n0": {"k0": "content0", "k1": "content1"},
    "n1": {"k0": "content1", "k1": "content0"},
}
OBJECT_BYTES = {"k0": 2, "k1": 3}


def require(row: dict[str, Any], field: str, index: int) -> Any:
    if field not in row:
        raise ValueError(f"line {index}: missing global field {field!r}")
    return row[field]


def check(path: Path) -> None:
    state: dict[str, Any] = {
        n: {
            "live": False,
            "evicted": False,
            "admission_count": 0,
            "guid": "guid0" if n == "n0" else "guid2",
            "history": {"guid0"} if n == "n0" else {"guid2"},
            "generation": 0,
            "stopped": False,
            "lru": 0,
            "active": False,
            "tu_used": False,
            "fresh_pending": False,
            "objects": {k: {"state": "ABSENT", "content": None} for k in KEYS},
            "crashed": set(),
            "attempts": {k: 0 for k in KEYS},
            "conflicts": {k: False for k in KEYS},
        }
        for n in NAMESPACES
    }
    slots: dict[str, tuple[str, str] | None] = {slot: None for slot in SLOTS}
    clock = 0
    fatal = False

    def ns_bytes(n: str) -> int:
        return sum(
            OBJECT_BYTES[k]
            for k, obj in state[n]["objects"].items()
            if obj["state"] in {"PRESENT", "PINNED"}
        )

    def total_bytes() -> int:
        return sum(ns_bytes(n) for n in NAMESPACES)

    def staging_bytes(n: str) -> int:
        return sum(
            OBJECT_BYTES[k]
            for k, obj in state[n]["objects"].items()
            if obj["state"] == "INSTALLING"
        )

    def total_staging_bytes() -> int:
        return sum(staging_bytes(n) for n in NAMESPACES)

    def installing(n: str) -> set[str]:
        return {
            k for k, obj in state[n]["objects"].items() if obj["state"] == "INSTALLING"
        }

    def pinned(n: str) -> set[str]:
        return {
            k for k, obj in state[n]["objects"].items() if obj["state"] == "PINNED"
        }

    def eligible(n: str) -> bool:
        return (
            state[n]["live"]
            and not state[n]["active"]
            and not installing(n)
            and not pinned(n)
        )

    def check_caps(index: int) -> None:
        if total_bytes() > MAX_AGGREGATE_BYTES:
            raise ValueError(f"line {index}: aggregate byte cap exceeded")
        if total_staging_bytes() > MAX_STAGING_BYTES:
            raise ValueError(f"line {index}: staging byte cap exceeded")
        if total_bytes() + total_staging_bytes() > MAX_TOTAL_BYTES:
            raise ValueError(f"line {index}: total simultaneous byte cap exceeded")
        for n in NAMESPACES:
            if ns_bytes(n) > MAX_NAMESPACE_BYTES:
                raise ValueError(f"line {index}: namespace byte cap exceeded for {n}")

    for index, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not raw.strip():
            continue
        row = json.loads(raw)
        action = row.get("action")
        if action not in ACTIONS:
            raise ValueError(f"line {index}: unknown global action {action!r}")
        namespace = require(row, "namespace", index)
        if namespace not in NAMESPACES:
            raise ValueError(f"line {index}: unknown namespace {namespace!r}")
        current = state[namespace]

        if action == "NAMESPACE_ADMITTED":
            if current["live"]:
                raise ValueError(f"line {index}: namespace already admitted")
            if current["stopped"]:
                raise ValueError(f"line {index}: admission after generation wrap stop")
            if current["generation"] >= MAX_GENERATION:
                raise ValueError(f"line {index}: admission at terminal generation")
            guid = require(row, "guid", index)
            generation = require(row, "generation", index)
            if guid != current["guid"]:
                raise ValueError(f"line {index}: admission GUID does not match namespace")
            if generation != current["generation"]:
                raise ValueError(f"line {index}: admission generation does not match namespace")
            current["live"] = True
            current["evicted"] = False
            current["admission_count"] += 1
            current["fresh_pending"] = current["admission_count"] == 2

        elif action == "NAMESPACE_TOUCHED":
            if not current["live"]:
                raise ValueError(f"line {index}: touched absent namespace")
            next_clock = require(row, "lru", index)
            if not isinstance(next_clock, int) or next_clock <= clock:
                raise ValueError(f"line {index}: LRU clock did not increase")
            clock = next_clock
            current["lru"] = next_clock

        elif action == "TU_STARTED":
            if not current["live"] or current["active"] or current["tu_used"]:
                raise ValueError(f"line {index}: invalid TU start")
            current["active"] = True
            current["tu_used"] = True
            current["fresh_pending"] = False

        elif action == "TU_FINISHED":
            if not current["active"] or installing(namespace):
                raise ValueError(f"line {index}: invalid TU finish")
            current["active"] = False
            for obj in current["objects"].values():
                if obj["state"] == "PINNED":
                    obj["state"] = "PRESENT"

        elif action in {"ARENA_INSTALLING", "ARENA_RETRY_INSTALLING"}:
            key = require(row, "key", index)
            slot = require(row, "slot", index)
            content = require(row, "content_digest", index)
            guid = require(row, "guid", index)
            generation = require(row, "generation", index)
            if key not in KEYS or slot not in SLOTS or content not in CANONICAL[namespace].values():
                raise ValueError(f"line {index}: invalid install identity")
            if guid != current["guid"] or generation != current["generation"]:
                raise ValueError(f"line {index}: arena identity does not match namespace")
            if not current["live"] or current["objects"][key]["state"] != "ABSENT":
                raise ValueError(f"line {index}: install requires ABSENT live object")
            if not current["active"]:
                raise ValueError(f"line {index}: install requires active TU")
            if slots[slot] is not None:
                raise ValueError(f"line {index}: staging slot is already owned")
            if total_staging_bytes() + OBJECT_BYTES[key] > MAX_STAGING_BYTES:
                raise ValueError(f"line {index}: staging byte cap exceeded")
            if total_bytes() + total_staging_bytes() + OBJECT_BYTES[key] > MAX_TOTAL_BYTES:
                raise ValueError(f"line {index}: total simultaneous byte cap exceeded")
            if action == "ARENA_RETRY_INSTALLING":
                if key not in current["crashed"] or current["attempts"][key] != 1:
                    raise ValueError(f"line {index}: retry without crashed INSTALLING state")
            elif current["attempts"][key] != 0:
                raise ValueError(f"line {index}: repeated initial install")
            slots[slot] = (namespace, key)
            current["objects"][key] = {"state": "INSTALLING", "content": content}
            current["attempts"][key] += 1

        elif action == "ARENA_PRESENT":
            key = require(row, "key", index)
            slot = require(row, "slot", index)
            content = require(row, "content_digest", index)
            guid = require(row, "guid", index)
            generation = require(row, "generation", index)
            if key not in KEYS or slot not in SLOTS:
                raise ValueError(f"line {index}: invalid PRESENT identity")
            if current["objects"][key]["state"] != "INSTALLING":
                raise ValueError(f"line {index}: PRESENT without INSTALLING")
            if guid != current["guid"] or generation != current["generation"]:
                raise ValueError(f"line {index}: arena identity does not match namespace")
            if slots[slot] != (namespace, key):
                raise ValueError(f"line {index}: PRESENT lost staging ownership")
            if current["objects"][key]["content"] != content:
                raise ValueError(f"line {index}: staged content changed before PRESENT")
            if content != CANONICAL[namespace][key]:
                raise ValueError(f"line {index}: immutable arena content changed")
            current["objects"][key] = {"state": "PRESENT", "content": content}
            slots[slot] = None
            check_caps(index)

        elif action == "ARENA_PINNED":
            key = require(row, "key", index)
            if key not in KEYS or not current["active"]:
                raise ValueError(f"line {index}: PINNED without active TU")
            if current["objects"][key]["state"] != "PRESENT":
                raise ValueError(f"line {index}: PINNED requires PRESENT")
            current["objects"][key]["state"] = "PINNED"

        elif action == "ARENA_UNPINNED":
            key = require(row, "key", index)
            if key not in KEYS or current["objects"][key]["state"] != "PINNED":
                raise ValueError(f"line {index}: invalid unpin")
            current["objects"][key]["state"] = "PRESENT"

        elif action == "INSTALL_CRASHED":
            key = require(row, "key", index)
            slot = require(row, "slot", index)
            if key not in KEYS or slot not in SLOTS:
                raise ValueError(f"line {index}: invalid crash identity")
            if current["objects"][key]["state"] != "INSTALLING":
                raise ValueError(f"line {index}: crash without INSTALLING")
            if slots[slot] != (namespace, key):
                raise ValueError(f"line {index}: crash does not own staging slot")
            current["objects"][key] = {"state": "ABSENT", "content": None}
            current["crashed"].add(key)
            slots[slot] = None

        elif action == "CONTENT_CONFLICT_FATAL":
            key = require(row, "key", index)
            content = require(row, "content_digest", index)
            if key not in KEYS or content == current["objects"][key]["content"]:
                raise ValueError(f"line {index}: conflict is not same-key/different-content")
            if current["objects"][key]["state"] not in {"PRESENT", "PINNED"}:
                raise ValueError(f"line {index}: conflict lacks immutable existing object")
            if current["conflicts"][key]:
                raise ValueError(f"line {index}: repeated content conflict")
            current["conflicts"][key] = True
            fatal = True

        elif action == "NAMESPACE_EVICTED":
            if not eligible(namespace):
                raise ValueError(f"line {index}: eviction requires an idle unpinned namespace")
            eligible_names = [n for n in NAMESPACES if eligible(n)]
            if any(state[n]["lru"] < current["lru"] for n in eligible_names):
                raise ValueError(f"line {index}: eviction victim is not whole-namespace LRU")
            for obj in current["objects"].values():
                obj["state"] = "ABSENT"
                obj["content"] = None
            current["live"] = False
            current["evicted"] = True
            current["crashed"] = set()
            current["active"] = False
            current["tu_used"] = False
            current["fresh_pending"] = False
            current["attempts"] = {k: 0 for k in KEYS}
            current["conflicts"] = {k: False for k in KEYS}
            check_caps(index)

        elif action == "GENERATION_ADVANCED":
            generation = require(row, "generation", index)
            if current["live"] or current["stopped"]:
                raise ValueError(f"line {index}: generation advanced while admitted/stopped")
            if current["generation"] >= MAX_GENERATION:
                raise ValueError(f"line {index}: generation wrapped instead of stopping")
            if generation != current["generation"] + 1:
                raise ValueError(f"line {index}: unexpected generation advance")
            current["generation"] = generation

        elif action == "GENERATION_WRAP_STOPPED":
            generation = require(row, "generation", index)
            if current["live"] or current["stopped"] or generation != MAX_GENERATION:
                raise ValueError(f"line {index}: invalid generation-wrap stop")
            current["stopped"] = True

        elif action == "C_GUID_FLIPPED":
            guid = require(row, "guid", index)
            if guid not in GUIDS or not current["stopped"]:
                raise ValueError(f"line {index}: GUID flip without admission stop")
            if guid in current["history"]:
                raise ValueError(f"line {index}: GUID was reused after wrap")
            if any(
                other != namespace and (
                    state[other]["guid"] == guid or guid in state[other]["history"]
                )
                for other in NAMESPACES
            ):
                raise ValueError(f"line {index}: GUID aliases another namespace")
            current["guid"] = guid
            current["history"].add(guid)
            current["generation"] = 0
            current["stopped"] = False

        if action not in {"ARENA_INSTALLING", "ARENA_RETRY_INSTALLING", "ARENA_PRESENT"}:
            for slot, owner in slots.items():
                if owner is not None:
                    n, k = owner
                    if state[n]["objects"][k]["state"] != "INSTALLING":
                        raise ValueError(f"line {index}: stale staging owner in {slot}")
        check_caps(index)

    if not fatal:
        raise ValueError("trace did not exercise same-key/different-content fatality")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    args = parser.parse_args()
    check(args.trace)
    print(f"check_global_trace.py: {args.trace} passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
