#!/usr/bin/env python3
"""Check the canonical Protocol-50 JSONL action vocabulary and core ordering."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


ACTIONS = {
    "SESSION_OPENED", "SESSION_REPLACED", "SESSION_DISCONNECTED", "HISTORY_RESET",
    "TX_BEGIN", "TX_ABORTED", "DICT_COMPLETE", "BODY_COMPLETE", "NEED_RECORDED",
    "OBJECT_APPLIED", "INPUT_MATERIALIZED", "INPUT_COMMITTED", "COMMIT_ACCEPTED",
    "ACTIVE_REPLAYED", "LOST_COMMIT_ACCEPTED",
}
UINT64_MAX = (1 << 64) - 1


def check(path: Path) -> None:
    relationships: dict[tuple[str, str], dict] = {}

    def new_relationship() -> dict:
        return {
            "c": {"cursor": None, "active": None},
            "f": {
                "connected": False, "session": 0, "route": None,
                "pending": None, "dict": False, "body": False,
                "need": False, "requested": set(), "remaining": set(),
                "materialized": False, "last_commit": None,
                "commit_unacknowledged": False, "commit_disconnected": False,
                "installed": {},
            },
        }

    def identity(row: dict) -> tuple:
        return (row["history_nonce"], row["rel_seq"], row["tu_seq"],
                row["transaction_digest"], row["raw_digest"])

    def clear_f_pending(f: dict) -> None:
        f.update({"pending": None, "dict": False, "body": False,
                  "need": False, "requested": set(), "remaining": set(),
                  "materialized": False})

    def check_reconciliation_witness(index: int, c: dict, f: dict) -> None:
        if not f["commit_unacknowledged"]:
            return
        if c["active"] is None or f["pending"] is not None:
            raise ValueError(
                f"line {index}: durable F commit lost its C reconciliation identity")
        if f["last_commit"] != c["active"]:
            raise ValueError(
                f"line {index}: durable F commit does not match C active")
        if c["cursor"] is None or f["route"] is None:
            raise ValueError(
                f"line {index}: durable F commit lacks route cursors")
        if (f["route"][0] != c["cursor"][0] or
                f["route"][1] != c["cursor"][1] + 1):
            raise ValueError(
                f"line {index}: durable F commit is not exactly one REL_SEQ ahead")

    for index, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        row = json.loads(line)
        action = row.get("action")
        if action not in ACTIONS:
            raise ValueError(f"line {index}: unknown action {action!r}")
        actor = row.get("actor")
        if actor not in {"C", "F"}:
            raise ValueError(f"line {index}: unknown actor {actor!r}")
        for field in ("session_serial", "history_nonce", "rel_seq", "tu_seq"):
            value = row.get(field)
            if not isinstance(value, int) or value < 0 or value > UINT64_MAX:
                raise ValueError(f"line {index}: {field} is outside u64")
        relationship = relationships.setdefault(
            (row["c_store_guid"], row["f_store_guid"]), new_relationship())
        c = relationship["c"]
        f = relationship["f"]
        tx = identity(row)
        if action in {"SESSION_OPENED", "SESSION_REPLACED"}:
            replacing = action == "SESSION_REPLACED"
            if actor != "F" or not row["session_serial"] or f["connected"] != replacing:
                raise ValueError(f"line {index}: invalid F session open/replace")
            if replacing and f["commit_unacknowledged"]:
                f["commit_disconnected"] = True
            f["connected"] = True
            f["session"] = row["session_serial"]
            clear_f_pending(f)
        elif action == "SESSION_DISCONNECTED":
            if actor != "F" or not f["connected"] or row["session_serial"] != f["session"]:
                raise ValueError(f"line {index}: disconnect without current F session")
            f["connected"] = False
            if f["commit_unacknowledged"]:
                f["commit_disconnected"] = True
            clear_f_pending(f)
        elif action == "HISTORY_RESET":
            if (actor != "F" or not f["connected"] or f["pending"] is not None or
                    c["active"] is not None or f["commit_unacknowledged"]):
                raise ValueError(f"line {index}: F history reset at the wrong boundary")
            if f["route"] is not None and row["history_nonce"] == f["route"][0]:
                raise ValueError(f"line {index}: HISTORY_NONCE was reused")
            f["route"] = (row["history_nonce"], 0, row["state_digest"])
            c["cursor"] = (row["history_nonce"], 0, row["state_digest"])
            f["last_commit"] = None
            f["commit_unacknowledged"] = False
            f["commit_disconnected"] = False
        elif action == "TX_BEGIN":
            if actor == "C":
                if c["active"] is not None:
                    raise ValueError(f"line {index}: second C active transaction")
                if f["pending"] is not None or f["commit_unacknowledged"]:
                    raise ValueError(
                        f"line {index}: C began while F state still requires reconciliation")
                if row["rel_seq"] == UINT64_MAX:
                    raise ValueError(f"line {index}: C REL_SEQ exhausted before TX_BEGIN")
                cursor = c["cursor"]
                if cursor is None or row["history_nonce"] != cursor[0]:
                    if row["rel_seq"] != 0:
                        raise ValueError(f"line {index}: new C history did not start at zero")
                    c["cursor"] = (row["history_nonce"], row["rel_seq"],
                                   row["state_digest"])
                elif (row["rel_seq"], row["state_digest"]) != (cursor[1], cursor[2]):
                    raise ValueError(f"line {index}: C TX_BEGIN missed its cursor")
                c["active"] = tx
            else:
                if not f["connected"] or f["route"] is None or f["pending"] is not None:
                    raise ValueError(f"line {index}: F TX_BEGIN at the wrong boundary")
                if c["active"] != tx:
                    raise ValueError(f"line {index}: F TX_BEGIN does not match C active")
                if (row["history_nonce"], row["rel_seq"], row["state_digest"]) != f["route"]:
                    raise ValueError(f"line {index}: F TX_BEGIN missed its cursor")
                clear_f_pending(f)
                f["pending"] = tx
        elif action == "ACTIVE_REPLAYED":
            if (actor != "F" or not f["connected"] or f["route"] is None or
                    f["pending"] is not None or c["active"] != tx or
                    (row["history_nonce"], row["rel_seq"], row["state_digest"]) != f["route"]):
                raise ValueError(f"line {index}: replay does not match C/F state")
            clear_f_pending(f)
            f["pending"] = tx
        elif action == "TX_ABORTED":
            if actor != "C" or c["active"] != tx:
                raise ValueError(f"line {index}: abort does not match C active")
            if f["pending"] is not None:
                raise ValueError(f"line {index}: abort skipped F pending overlay")
            if f["commit_unacknowledged"]:
                raise ValueError(
                    f"line {index}: abort skipped durable F commit reconciliation")
            c["active"] = None
        elif action == "DICT_COMPLETE":
            if actor != "F" or f["pending"] != tx or f["dict"]:
                raise ValueError(f"line {index}: DICT does not match F pending")
            f["dict"] = True
        elif action == "NEED_RECORDED":
            keys = row["need_keys"]
            if (actor != "F" or f["pending"] != tx or not f["dict"] or f["need"] or
                    keys != sorted(set(keys)) or row["remaining_need"] != len(keys)):
                raise ValueError(f"line {index}: Need precedes exact DICT")
            f["need"] = True
            f["requested"] = set(keys)
            f["remaining"] = set(keys)
        elif action == "BODY_COMPLETE":
            if actor != "F" or f["pending"] != tx or f["body"]:
                raise ValueError(f"line {index}: BODY does not match F pending")
            f["body"] = True
        elif action == "OBJECT_APPLIED":
            key = row["key64"]
            if (actor != "F" or f["pending"] != tx or not f["need"] or
                    key not in f["requested"]):
                raise ValueError(f"line {index}: object is outside recorded Need")
            old_content = f["installed"].get(key)
            if old_content is not None and old_content != row["content_digest"]:
                raise ValueError(f"line {index}: Key64 changed immutable content")
            f["installed"][key] = row["content_digest"]
            first = key in f["remaining"]
            if first == row["duplicate"]:
                raise ValueError(f"line {index}: duplicate marker disagrees with Need state")
            f["remaining"].discard(key)
            if row["remaining_need"] != len(f["remaining"]):
                raise ValueError(f"line {index}: object changed exact Need incorrectly")
        elif action == "INPUT_MATERIALIZED":
            if (actor != "F" or f["pending"] != tx or
                    not (f["dict"] and f["body"] and f["need"]) or
                    f["remaining"] or f["materialized"]):
                raise ValueError(f"line {index}: input materialized before exact closure")
            f["materialized"] = True
        elif action == "INPUT_COMMITTED":
            if actor != "F" or f["pending"] != tx or not f["materialized"]:
                raise ValueError(f"line {index}: F commit lacks exact materialization")
            if row["rel_seq"] == UINT64_MAX:
                raise ValueError(f"line {index}: F REL_SEQ exhausted")
            f["last_commit"] = tx
            f["commit_unacknowledged"] = True
            f["commit_disconnected"] = False
            clear_f_pending(f)
            f["route"] = (row["history_nonce"], row["rel_seq"] + 1,
                          row["state_digest"])
        elif action in {"COMMIT_ACCEPTED", "LOST_COMMIT_ACCEPTED"}:
            if (actor != "C" or c["active"] != tx or f["last_commit"] != tx or
                    f["route"] is None or row["state_digest"] != f["route"][2]):
                raise ValueError(f"line {index}: C acceptance lacks matching F commit")
            if (not f["commit_unacknowledged"] or
                    (action == "COMMIT_ACCEPTED" and f["commit_disconnected"]) or
                    (action == "LOST_COMMIT_ACCEPTED" and not f["commit_disconnected"])):
                raise ValueError(f"line {index}: wrong normal/lost acceptance path")
            if row["rel_seq"] == UINT64_MAX:
                raise ValueError(f"line {index}: C REL_SEQ exhausted")
            c["active"] = None
            c["cursor"] = (row["history_nonce"], row["rel_seq"] + 1,
                           row["state_digest"])
            f["commit_unacknowledged"] = False
            f["commit_disconnected"] = False
        check_reconciliation_witness(index, c, f)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    args = parser.parse_args()
    check(args.trace)
    print(f"check_trace.py: {args.trace} passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
