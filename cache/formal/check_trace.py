#!/usr/bin/env python3
"""Check the canonical Protocol-50 JSONL action vocabulary and core ordering."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


ACTIONS = {
    "SESSION_OPENED",
    "SESSION_REPLACED",
    "SESSION_DISCONNECTED",
    "HISTORY_RESET",
    "TX_BEGIN",
    "TX_ABORTED",
    "BODY_COMPLETE",
    "NEED_RECORDED",
    "OBJECT_APPLIED",
    "INPUT_MATERIALIZED",
    "INPUT_COMMITTED",
    "COMMIT_ACCEPTED",
    "ACTIVE_REPLAYED",
    "LOST_COMMIT_ACCEPTED",
}
UINT64_MAX = (1 << 64) - 1


def check(path: Path) -> None:
    relationships: dict[tuple[str, str], dict[str, Any]] = {}

    def new_relationship() -> dict[str, Any]:
        return {
            # Route pipelining (P50_ROUTE_PIPELINING.md step 7): C's scalar
            # "active" generalizes to the bounded ACTIVE_TX FIFO
            # (active_queue_), front = head = the oldest not-yet-retired TX.
            # Every check below is written so that a one-member queue
            # reproduces the original scalar assertion exactly, which is
            # the K=1 identity argument (design invariant 12): a K=1 trace
            # carries no queue fields, the queue holds at most one member,
            # and each generalized assertion degenerates to today's.
            "c": {
                "cursor": None,
                "queue": [],
                # Identities removed from the queue by a *fielded* TX_ABORTED
                # (route-pipelining universal invalidation, design invariant
                # 6): a subsequent HISTORY_RESET may legitimately discard
                # still-unacknowledged durable F commits, which at K=1 is
                # exactly the "skipped an unacknowledged durable commit"
                # bug this checker rejects.
                "abandoned_pipelined": [],
                # Consecutive fielded TX_ABORTED records since the last
                # queue push/retire/reset: abandon_active drains the whole
                # queue front-to-back with positions 0..n-1, so this
                # counter validates a drain batch's order and completeness.
                "aborted_batch": 0,
            },
            "f": {
                "connected": False,
                "session": 0,
                "last_session_serial": 0,
                "reset_used_in_session": False,
                "route": None,
                "last_history_nonce": None,
                "pending": None,
                "body": False,
                "need": False,
                "requested": set(),
                "remaining": set(),
                "materialized": False,
                "last_commit": None,
                # F may hold more than one durable-but-unaccepted commit
                # while C still holds the matching TXs as queue members;
                # they retire strictly front-first on COMMIT_ACCEPTED /
                # LOST_COMMIT_ACCEPTED.  commit_digests carries each
                # committed TX's post-state digest in the same order.  At
                # K=1 both hold at most one entry, so "front of unacked" is
                # today's "last_commit" scalar.
                "unacked": [],
                "commit_digests": [],
                "commit_unacknowledged": False,
                "commit_disconnected": False,
                "installed": {},
            },
        }

    def identity(row: dict[str, Any]) -> tuple[Any, ...]:
        return (
            row["history_nonce"],
            row["rel_seq"],
            row["tu_seq"],
            row["transaction_digest"],
            row["raw_digest"],
        )

    def clear_f_pending(f: dict[str, Any]) -> None:
        f.update(
            {
                "pending": None,
                "body": False,
                "need": False,
                "requested": set(),
                "remaining": set(),
                "materialized": False,
            }
        )

    def current_f_session(row: dict[str, Any], f: dict[str, Any]) -> bool:
        return (
            row["actor"] == "F"
            and f["connected"]
            and row["session_serial"] == f["session"]
        )

    # Route-pipelining additive fields.  A C row carrying queue_depth means
    # the route runs (or ran) at K>1; absence means K=1 or a pre-pipelining
    # trace.  Every generalized check below is written so that a one-member
    # queue reproduces the original scalar assertion exactly, so old
    # fixtures pass unchanged; the depth field is only consulted where the
    # K bound itself must be enforced (TX_BEGIN admission).
    def check_reconciliation_witness(
        index: int, c: dict[str, Any], f: dict[str, Any]
    ) -> None:
        if not f["commit_unacknowledged"]:
            return
        unacked = f["unacked"]
        # F commits strictly in rel_seq order and C retires strictly
        # front-first, so F's unacknowledged commits are exactly the
        # oldest members of C's queue, in order -- except commits whose C
        # transaction was already *abandoned* (universal invalidation,
        # design invariant 6): those are being discarded wholesale and a
        # HISTORY_RESET at a fresh nonce retires them below.  At K=1
        # nothing is ever abandoned-pipelined, so every check below sees
        # the original scalar shape.
        live_unacked = [
            entry
            for entry in unacked
            if entry not in c["abandoned_pipelined"]
        ]
        # Every *live* durable commit must still name a C queue member (C
        # has not accepted it and has not invalidated it).  Abandoned
        # entries have deliberately lost their C identity.
        if live_unacked and not c["queue"]:
            raise ValueError(
                f"line {index}: durable F commit lost its C reconciliation identity"
            )
        if f["pending"] is not None and f["pending"] not in c["queue"]:
            raise ValueError(
                f"line {index}: F pending transaction is not a C queue member"
            )
        if len(live_unacked) > len(c["queue"]) or live_unacked != c["queue"][: len(live_unacked)]:
            raise ValueError(
                f"line {index}: durable F commit is not C's queue head"
            )
        if live_unacked and f["last_commit"] != unacked[-1]:
            raise ValueError(
                f"line {index}: durable F commit does not match C active"
            )
        if c["cursor"] is None or f["route"] is None:
            raise ValueError(
                f"line {index}: durable F commit lacks route cursors"
            )
        # "Ahead by at most K, never behind" (design invariant 9): C's queue
        # head sits exactly at C's confirmed cursor, so F's cursor (one past
        # its newest durable commit) is exactly len(unacked) ahead -- the
        # abandoned entries counted too, since F's cursor really did advance
        # past the commits C is discarding.  At K=1 (one queue member, one
        # unacked entry) this is exactly the original "exactly one REL_SEQ
        # ahead".
        if (
            f["route"][0] != c["cursor"][0]
            or f["route"][1] != c["cursor"][1] + len(unacked)
        ):
            raise ValueError(
                f"line {index}: durable F commit is not at most K REL_SEQ ahead"
            )
        # The oldest *live* (non-abandoned) unacknowledged commit sits
        # exactly at C's confirmed cursor (it is the queue head).  At K=1
        # this is the original "c active == c cursor" check; in the
        # poison shape (every unacked entry abandoned) there is no live
        # head to pin and the HISTORY_RESET boundary gate below takes
        # over.
        if live_unacked and (
            live_unacked[0][0] != c["cursor"][0]
            or live_unacked[0][1] != c["cursor"][1]
        ):
            raise ValueError(
                f"line {index}: durable F commit active identity missed C cursor"
            )

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
            if (
                not isinstance(value, int)
                or isinstance(value, bool)
                or value < 0
                or value > UINT64_MAX
            ):
                raise ValueError(f"line {index}: {field} is outside u64")

        relationship = relationships.setdefault(
            (row["c_store_guid"], row["f_store_guid"]), new_relationship()
        )
        c = relationship["c"]
        f = relationship["f"]
        tx = identity(row)

        if action in {"SESSION_OPENED", "SESSION_REPLACED"}:
            replacing = action == "SESSION_REPLACED"
            serial = row["session_serial"]
            if actor != "F" or serial == 0 or f["connected"] != replacing:
                raise ValueError(f"line {index}: invalid F session open/replace")
            if serial <= f["last_session_serial"]:
                raise ValueError(
                    f"line {index}: F session serial was reused or did not increase"
                )
            if replacing and f["commit_unacknowledged"]:
                f["commit_disconnected"] = True
            f["connected"] = True
            f["session"] = serial
            f["last_session_serial"] = serial
            f["reset_used_in_session"] = False
            clear_f_pending(f)

        elif action == "SESSION_DISCONNECTED":
            if not current_f_session(row, f):
                raise ValueError(
                    f"line {index}: disconnect without current F session"
                )
            f["connected"] = False
            if f["commit_unacknowledged"]:
                f["commit_disconnected"] = True
            clear_f_pending(f)

        elif action == "HISTORY_RESET":
            if (
                not current_f_session(row, f)
                or f["pending"] is not None
                # Generalized from "c active is not None": a route-pipelined
                # C may hold up to K outstanding TXs; a reset is legal only
                # once every one of them has been resolved (retired by an
                # accepted commit or dropped by a TX_ABORTED).  Universal
                # invalidation (design invariant 6) means a pipelined
                # abort drains the whole queue before the reset; a reset
                # with survivors is the same wrong boundary as at K=1.
                or c["queue"]
                # A pipelined route may reset while F still holds durable
                # commits C has deliberately invalidated (the aborted
                # TXs) -- but only if every unacknowledged commit names an
                # abandoned queue member (poison-then-cold-reset, design
                # §3 P3/P5).  At K=1 nothing is ever in abandoned_pipelined
                # so this stays exactly today's blanket rejection.
                or (
                    f["commit_unacknowledged"]
                    and any(
                        entry not in c["abandoned_pipelined"]
                        for entry in f["unacked"]
                    )
                )
            ):
                raise ValueError(
                    f"line {index}: F history reset at the wrong boundary"
                )
            if f["reset_used_in_session"]:
                raise ValueError(
                    f"line {index}: second HISTORY_RESET in one F session"
                )
            next_nonce = row["history_nonce"]
            if (
                f["last_history_nonce"] is not None
                and next_nonce <= f["last_history_nonce"]
            ):
                raise ValueError(
                    f"line {index}: HISTORY_NONCE was reused or did not increase"
                )
            f["reset_used_in_session"] = True
            f["last_history_nonce"] = next_nonce
            f["route"] = (next_nonce, 0, row["state_digest"])
            c["cursor"] = (next_nonce, 0, row["state_digest"])
            c["queue"].clear()
            c["abandoned_pipelined"].clear()
            c["aborted_batch"] = 0
            f["last_commit"] = None
            f["unacked"].clear()
            f["commit_digests"].clear()
            f["commit_unacknowledged"] = False
            f["commit_disconnected"] = False

        elif action == "TX_BEGIN":
            if actor == "C":
                # Additive route-pipelining fields (queue_position /
                # queue_depth) are optional: a K=1 (or pre-pipelining) row
                # carries neither and takes today's scalar path verbatim.
                position = row.get("queue_position")
                depth = row.get("queue_depth")
                if (position is None) != (depth is None):
                    raise ValueError(
                        f"line {index}: C TX_BEGIN carries only one queue field"
                    )
                if depth is not None and (
                    not isinstance(depth, int)
                    or isinstance(depth, bool)
                    or depth < 1
                ):
                    raise ValueError(
                        f"line {index}: C TX_BEGIN has an invalid queue depth"
                    )
                if position is not None and (
                    not isinstance(position, int)
                    or isinstance(position, bool)
                    or position < 0
                ):
                    raise ValueError(
                        f"line {index}: C TX_BEGIN has an invalid queue position"
                    )
                if position is not None:
                    # Pipelined admission: position is the 0-based index this
                    # TX takes in C's ACTIVE_TX queue, so it must equal the
                    # current length and stay below the route's depth K.
                    if position != len(c["queue"]) or position >= depth:
                        raise ValueError(
                            f"line {index}: C TX_BEGIN missed its queue slot"
                        )
                    # A successor begin is legal while F still requires
                    # reconciliation of its predecessor (that is the
                    # point of pipelining), but the reconciliation identity
                    # must still be a live queue member.
                    if f["pending"] is not None and f["pending"] not in c["queue"]:
                        raise ValueError(
                            f"line {index}: C began while F owns a foreign pending transaction"
                        )
                elif c["queue"]:
                    raise ValueError(
                        f"line {index}: second C active transaction"
                    )
                if position is None and (
                    f["pending"] is not None or f["commit_unacknowledged"]
                ):
                    raise ValueError(
                        f"line {index}: C began while F state still requires reconciliation"
                    )
                if row["rel_seq"] == UINT64_MAX:
                    raise ValueError(
                        f"line {index}: C REL_SEQ exhausted before TX_BEGIN"
                    )
                cursor = c["cursor"]
                if cursor is None or row["history_nonce"] != cursor[0]:
                    if row["rel_seq"] != 0:
                        raise ValueError(
                            f"line {index}: new C history did not start at zero"
                        )
                    c["cursor"] = (
                        row["history_nonce"],
                        row["rel_seq"],
                        row["state_digest"],
                    )
                elif position is not None:
                    # Pipelined successor: rel_seq advances by one per queue
                    # slot from the confirmed cursor.  At position 0 the
                    # pre-state digest is still C's confirmed cursor digest
                    # (exactly today's check); at position > 0 it is the
                    # predecessor's *predicted* post-digest, which the
                    # trace does not carry, so only the rel_seq chain is
                    # checkable here -- the digest itself is verified by F's
                    # own TX_BEGIN cursor check below, which is unchanged.
                    if row["rel_seq"] != cursor[1] + position or (
                        position == 0 and row["state_digest"] != cursor[2]
                    ):
                        raise ValueError(
                            f"line {index}: C TX_BEGIN missed its cursor"
                        )
                elif (row["rel_seq"], row["state_digest"]) != (
                    cursor[1],
                    cursor[2],
                ):
                    raise ValueError(
                        f"line {index}: C TX_BEGIN missed its cursor"
                    )
                c["queue"].append(tx)
                c["aborted_batch"] = 0
            else:
                if (
                    not current_f_session(row, f)
                    or f["route"] is None
                    or f["pending"] is not None
                ):
                    raise ValueError(
                        f"line {index}: F TX_BEGIN at the wrong boundary"
                    )
                # Generalized from "c active == tx": F begins one TX at a
                # time, strictly in rel_seq order, but under pipelining the
                # TX it begins may be any member of C's outstanding queue,
                # not just the head.  At K=1 the queue holds at most the
                # one head member, so this is today's scalar assertion.
                if tx not in c["queue"]:
                    raise ValueError(
                        f"line {index}: F TX_BEGIN does not match C active"
                    )
                # Route pipelining: a successor connection is *pre-admitted*
                # by C (its TX is already a queue member before F releases
                # it), so a SESSION_REPLACED that opened this session was
                # ordinary pipelining, not a lost acknowledgement -- the
                # predecessor's own connection is still the live ack channel.
                # At K=1 this clear can never fire: C cannot begin while a
                # commit is unacknowledged, so no F TX_BEGIN ever names a
                # queued TX while commit_disconnected is set.
                if f["commit_disconnected"]:
                    f["commit_disconnected"] = False
                if (
                    row["history_nonce"],
                    row["rel_seq"],
                    row["state_digest"],
                ) != f["route"]:
                    raise ValueError(
                        f"line {index}: F TX_BEGIN missed its cursor"
                    )
                clear_f_pending(f)
                f["pending"] = tx

        elif action == "ACTIVE_REPLAYED":
            if (
                not current_f_session(row, f)
                or f["route"] is None
                or f["pending"] is not None
                # Same queue-membership generalization as F TX_BEGIN: the
                # replayed TX is one of C's outstanding queue members.
                or tx not in c["queue"]
                or (
                    row["history_nonce"],
                    row["rel_seq"],
                    row["state_digest"],
                )
                != f["route"]
            ):
                raise ValueError(
                    f"line {index}: replay does not match C/F state"
                )
            clear_f_pending(f)
            f["pending"] = tx

        elif action == "TX_ABORTED":
            # Generalized from "c active == tx": a pipelined route aborts
            # exactly the TXs it still holds outstanding, one record per
            # queue member, front-to-back (CRoute::abandon_active).
            # Removing by identity (not head-only) also covers the P3 S2
            # shape where C drops only a successor that F never saw.  At
            # K=1 the queue holds at most the head member, so this is
            # today's scalar assertion.
            if actor != "C" or tx not in c["queue"]:
                raise ValueError(
                    f"line {index}: abort does not match C active"
                )
            position = row.get("queue_position")
            depth = row.get("queue_depth")
            if (position is None) != (depth is None):
                raise ValueError(
                    f"line {index}: C TX_ABORTED carries only one queue field"
                )
            if position is not None:
                # Pipelined abort: CRoute::abandon_active emits one record
                # per outstanding member, front-to-back, BEFORE clearing the
                # queue, so a drain batch is positions 0..n-1 in order and
                # always covers the whole queue.  Validate the record against
                # the still-queued front (pop it) and require the position to
                # be the batch's consecutive counter.  A batch that ends
                # early leaves a survivor in the queue, which the
                # reconciliation witness / HISTORY_RESET boundary checks
                # below reject.
                if (
                    not isinstance(position, int)
                    or isinstance(position, bool)
                    or not isinstance(depth, int)
                    or isinstance(depth, bool)
                    or position < 0
                    or depth < 1
                    or position >= depth
                    or position != c["aborted_batch"]
                    or not c["queue"]
                    or c["queue"][0] != tx
                ):
                    raise ValueError(
                        f"line {index}: pipelined abort does not match its queue slot"
                    )
                # A successor abort is legal while F still holds the
                # predecessor's durable-but-unaccepted commit (universal
                # invalidation, design §3 P3/P5) or while F is mid-TX on the
                # predecessor (the successor's connection is simply never
                # released -- parked connections emit no F records).
                # F's *own* pending transaction is the predecessor's, so
                # only the aborted member itself must not be F-pending.
                c["queue"].pop(0)
                c["abandoned_pipelined"].append(tx)
                c["aborted_batch"] += 1
            else:
                if f["pending"] is not None:
                    raise ValueError(
                        f"line {index}: C abort while F still owns pending overlay"
                    )
                if f["commit_unacknowledged"]:
                    raise ValueError(
                        f"line {index}: C abort after durable F commit before acceptance"
                    )
                c["queue"].remove(tx)
                c["aborted_batch"] = 0

        elif action == "NEED_RECORDED":
            keys = row["need_keys"]
            if (
                not current_f_session(row, f)
                or f["pending"] != tx
                or not f["body"]
                or f["need"]
                or keys != sorted(set(keys))
                or row["remaining_need"] != len(keys)
            ):
                raise ValueError(
                    f"line {index}: Need precedes exact BODY/current session"
                )
            f["need"] = True
            f["requested"] = set(keys)
            f["remaining"] = set(keys)

        elif action == "BODY_COMPLETE":
            if (
                not current_f_session(row, f)
                or f["pending"] != tx
                or f["body"]
            ):
                raise ValueError(
                    f"line {index}: BODY does not match F pending/current session"
                )
            f["body"] = True

        elif action == "OBJECT_APPLIED":
            key = row["key64"]
            if (
                not current_f_session(row, f)
                or f["pending"] != tx
                or not f["need"]
                or key not in f["requested"]
            ):
                raise ValueError(
                    f"line {index}: object is outside active Need/current session"
                )
            old_content = f["installed"].get(key)
            if old_content is not None and old_content != row["content_digest"]:
                raise ValueError(
                    f"line {index}: Key64 changed immutable content"
                )
            f["installed"][key] = row["content_digest"]
            first = key in f["remaining"]
            if first == row["duplicate"]:
                raise ValueError(
                    f"line {index}: duplicate marker disagrees with Need state"
                )
            f["remaining"].discard(key)
            if row["remaining_need"] != len(f["remaining"]):
                raise ValueError(
                    f"line {index}: object changed exact Need incorrectly"
                )

        elif action == "INPUT_MATERIALIZED":
            if (
                not current_f_session(row, f)
                or f["pending"] != tx
                or not (f["body"] and f["need"])
                or f["remaining"]
                or f["materialized"]
            ):
                raise ValueError(
                    f"line {index}: input materialized before exact closure/current session"
                )
            f["materialized"] = True

        elif action == "INPUT_COMMITTED":
            if (
                not current_f_session(row, f)
                or f["pending"] != tx
                or not f["materialized"]
            ):
                raise ValueError(
                    f"line {index}: F commit lacks exact materialization/current session"
                )
            if row["rel_seq"] == UINT64_MAX:
                raise ValueError(f"line {index}: F REL_SEQ exhausted")
            f["last_commit"] = tx
            # Route pipelining: F may hold several durable commits that C has
            # not accepted yet; they retire strictly front-first.  At K=1 the
            # FIFO holds at most the one entry that last_commit names.
            f["unacked"].append(tx)
            f["commit_digests"].append(row["state_digest"])
            f["commit_unacknowledged"] = True
            f["commit_disconnected"] = False
            clear_f_pending(f)
            f["route"] = (
                row["history_nonce"],
                row["rel_seq"] + 1,
                row["state_digest"],
            )

        elif action in {"COMMIT_ACCEPTED", "LOST_COMMIT_ACCEPTED"}:
            position = row.get("queue_position")
            depth = row.get("queue_depth")
            if (position is None) != (depth is None):
                raise ValueError(
                    f"line {index}: C acceptance carries only one queue field"
                )
            if position is not None and (
                not isinstance(position, int)
                or isinstance(position, bool)
                or not isinstance(depth, int)
                or isinstance(depth, bool)
                or position < 0
                or depth < 1
                or position >= depth
                # retire_locked_commits only ever emits the retiring head,
                # so a fielded acceptance must name slot 0.
                or position != 0
            ):
                raise ValueError(
                    f"line {index}: C acceptance does not name the retiring queue head"
                )
            if (
                actor != "C"
                # Generalized from "c active == tx": C accepts a commit
                # strictly front-first (retire_locked_commits), so the
                # accepted TX must be the head of C's outstanding queue.
                # At K=1 the queue holds at most the head member.
                or not c["queue"]
                or c["queue"][0] != tx
                # ... and the head of F's unacknowledged commits (the
                # oldest durable commit C has not accepted yet), replacing
                # today's scalar f["last_commit"] == tx.  A successor
                # retiring before its head is the exact bug the design's
                # touch-up (b) forbids.  The accepted digest is compared
                # against this TX's *own* commit digest, not F's newest
                # route digest -- at K=1 (one outstanding commit) these are
                # the same value, so today's f["route"][2] comparison is
                # preserved exactly.
                or not f["unacked"]
                or f["unacked"][0] != tx
                or f["route"] is None
                or row["state_digest"] != f["commit_digests"][0]
            ):
                raise ValueError(
                    f"line {index}: C acceptance lacks matching F commit"
                )
            if (
                not f["commit_unacknowledged"]
                or (
                    action == "COMMIT_ACCEPTED"
                    and f["commit_disconnected"]
                )
                or (
                    action == "LOST_COMMIT_ACCEPTED"
                    and not f["commit_disconnected"]
                )
            ):
                raise ValueError(
                    f"line {index}: wrong normal/lost acceptance path"
                )
            if row["rel_seq"] == UINT64_MAX:
                raise ValueError(f"line {index}: C REL_SEQ exhausted")
            c["queue"].pop(0)
            c["aborted_batch"] = 0
            if tx in c["abandoned_pipelined"]:
                c["abandoned_pipelined"].remove(tx)
            c["cursor"] = (
                row["history_nonce"],
                row["rel_seq"] + 1,
                row["state_digest"],
            )
            f["unacked"].pop(0)
            f["commit_digests"].pop(0)
            f["commit_unacknowledged"] = bool(f["unacked"])
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
