#!/usr/bin/env python3
"""Validate the opt-in sender EAGAIN witness against strace and test output."""

import argparse
import re
import sys
import unittest
from dataclasses import dataclass


PROFILES = ("ZSTD_TU", "P29V1", "ZSTD_ROUTE")
PREFIX = "P51_D03_EAGAIN_"
LINE_RE = re.compile(r"^\s*(\d+)\s+(\d+\.\d+)\s+(.*)$")
WRITE_RE = re.compile(r'^write\(2(?:<[^>]*>)?, "((?:\\.|[^"\\])*)", (\d+)\) = (\d+)$')
WRITE_UNFINISHED_RE = re.compile(
    r'^write\(2(?:<[^>]*>)?, "((?:\\.|[^"\\])*)", (\d+) <unfinished \.\.\.>$'
)
WRITE_RESUMED_RE = re.compile(r'^<\.\.\. write resumed>\) = (\d+)$')
SEND_RE = re.compile(
    r'^sendto\((\d+)(?:<TCP:\[([^]]+)\]>)?, "((?:\\.|[^"\\])*)", '
    r'(\d+), .* = (-1 EAGAIN \([^)]*\)|\d+)$'
)


def c_string_bytes(value: str) -> bytes:
    out = bytearray()
    i = 0
    while i < len(value):
        ch = value[i]
        if ch != "\\":
            out.extend(ch.encode("utf-8"))
            i += 1
            continue
        i += 1
        if i >= len(value):
            raise ValueError("trailing escape in strace C string")
        if value[i] in "01234567":
            digits = re.match(r"[0-7]{1,3}", value[i:]).group(0)
            out.append(int(digits, 8))
            i += len(digits)
        elif value[i] == "x":
            i += 1
            digits = re.match(r"[0-9a-fA-F]+", value[i:])
            if not digits:
                raise ValueError("empty hexadecimal escape in strace C string")
            out.append(int(digits.group(0), 16) & 0xFF)
            i += len(digits.group(0))
        else:
            escapes = {"a": 7, "b": 8, "f": 12, "n": 10, "r": 13,
                       "t": 9, "v": 11, "\\": 92, '"': 34, "?": 63}
            if value[i] not in escapes:
                raise ValueError(f"unsupported strace escape \\{value[i]}")
            out.append(escapes[value[i]])
            i += 1
    return bytes(out)


@dataclass(frozen=True)
class Event:
    pid: int
    time: float
    kind: str
    fd: int
    payload: bytes
    length: int
    result: str
    tuple: str | None = None


def parse_trace(trace: str) -> tuple[list[Event], dict[str, list[tuple[float, int, str]]]]:
    events: list[Event] = []
    markers: dict[str, list[tuple[float, int, str]]] = {}
    pending_writes: dict[int, tuple[float, bytes, int]] = {}

    def record_markers(time_f: float, pid_i: int, payload: bytes,
                       requested: int, actual: int) -> None:
        if len(payload) != requested or actual != requested:
            return
        text = payload.decode("utf-8", errors="replace")
        for marker_line in text.splitlines():
            if marker_line.startswith(PREFIX):
                marker = marker_line[len(PREFIX) :].split(" ", 1)[0]
                profile_match = re.search(r"\bprofile=([A-Z0-9_]+)", marker_line)
                profile = profile_match.group(1) if profile_match else ""
                if profile:
                    markers.setdefault(profile, []).append((time_f, pid_i, marker_line))

    for line in trace.splitlines():
        if "<unfinished ...>" in line and "sendto(" in line:
            raise ValueError("strace split a sendto syscall; use a complete non-interleaved syscall trace")
        if "<... sendto resumed>" in line:
            raise ValueError("strace resumed a split sendto syscall; use a complete non-interleaved syscall trace")
        matched = LINE_RE.match(line)
        if not matched:
            continue
        pid, timestamp, call = matched.groups()
        pid_i, time_f = int(pid), float(timestamp)
        resumed = WRITE_RESUMED_RE.match(call)
        if resumed:
            pending = pending_writes.pop(pid_i, None)
            if pending is None:
                raise ValueError("strace resumed a write without its matching unfinished call")
            _, payload, requested = pending
            record_markers(time_f, pid_i, payload, requested,
                           int(resumed.group(1)))
            continue
        write_unfinished = WRITE_UNFINISHED_RE.match(call)
        if write_unfinished:
            raw, requested = write_unfinished.groups()
            if pid_i in pending_writes:
                raise ValueError("strace reported nested unfinished writes for one thread")
            pending_writes[pid_i] = (time_f, c_string_bytes(raw), int(requested))
            continue
        write = WRITE_RE.match(call)
        if write:
            raw, requested, actual = write.groups()
            if PREFIX not in raw:
                continue
            record_markers(time_f, pid_i, c_string_bytes(raw),
                           int(requested), int(actual))
            continue
        send = SEND_RE.match(call)
        if send:
            fd, socket_tuple, raw, length, result = send.groups()
            events.append(Event(pid_i, time_f, "sendto", int(fd),
                                c_string_bytes(raw), int(length), result,
                                socket_tuple))
    if pending_writes:
        raise ValueError("strace ended with unfinished write syscall(s)")
    return events, markers


def validate(trace: str, test_log: str, exit_code: int) -> list[str]:
    if exit_code != 0:
        raise ValueError(f"selector exit was {exit_code}, expected 0")
    if "P51_D03_KERNEL_EAGAIN_CANDIDATE_SELECTOR PASS" not in test_log:
        raise ValueError("successful selector completion marker is absent")
    for profile in PROFILES:
        expected = f"P51_D03_EAGAIN_CASE profile={profile} exact=1 receipts=2 commits=2 credits=1 PASS"
        if test_log.count(expected) != 1:
            raise ValueError(f"expected exactly one successful {profile} case marker")

    events, markers = parse_trace(trace)
    results: list[str] = []
    used_tuples: set[str] = set()
    for profile in PROFILES:
        profile_markers = markers.get(profile, [])
        arms = [(time, pid, line) for time, pid, line in profile_markers
                if "P51_D03_EAGAIN_ARM" in line]
        cases = [(time, pid, line) for time, pid, line in profile_markers
                 if "P51_D03_EAGAIN_CASE" in line]
        if len(arms) != 1 or len(cases) != 1:
            raise ValueError(f"{profile}: expected exactly one ARM and CASE trace marker")
        arm_time, _, arm_line = arms[0]
        case_time, _, _ = cases[0]
        if case_time <= arm_time:
            raise ValueError(f"{profile}: malformed or reversed ARM/CASE marker")
        tuple_match = re.search(r"\btuple=([^ ]+)\b", arm_line)
        if not tuple_match:
            raise ValueError(f"{profile}: ARM marker lacks the observed socket tuple")
        expected_tuple = tuple_match.group(1)
        body_header = [event for event in events
                       if event.time > arm_time and
                       event.time < case_time and event.length == 4 and
                       event.payload[:1] == bytes([14])]
        failures = [event for event in body_header
                    if event.result.startswith("-1 EAGAIN")]
        if not failures:
            raise ValueError(f"{profile}: expected an actual 4-byte R2_BODY EAGAIN, found none")
        if any(event.length != 4 or len(event.payload) != 4 for event in failures):
            raise ValueError(f"{profile}: EAGAIN candidate was not a complete 4-byte frame header")
        failed_tuples = {event.tuple for event in failures}
        if len(failed_tuples) != 1 or expected_tuple not in failed_tuples:
            if len(failed_tuples) != 1:
                raise ValueError(f"{profile}: EAGAINs span multiple TCP tuples")
            raise ValueError(f"{profile}: EAGAIN tuple differs from observed socket tuple")
        failed = failures[-1]
        if not failed.tuple or failed.tuple in used_tuples:
            raise ValueError(f"{profile}: missing or reused TCP tuple")
        # Type 10 is LINK_HELLO. This ties the syscall to the C->F sender
        # socket, rather than accepting a same-shaped F->C peer write.
        hello = [event for event in events
                 if event.fd == failed.fd and event.tuple == failed.tuple and
                 event.time < failed.time and event.length >= 1 and
                 event.payload[:1] == bytes([10]) and not event.result.startswith("-1")]
        if not hello:
            raise ValueError(f"{profile}: EAGAIN tuple has no preceding C->F LINK_HELLO")
        retries = [event for event in body_header
                   if event.time > failed.time and event.length == 4 and
                   len(event.payload) == 4 and event.fd == failed.fd and
                   event.tuple == failed.tuple and event.payload == failed.payload and
                   event.result == str(event.length)]
        if not retries:
            raise ValueError(f"{profile}: no successful identical same-socket retry after EAGAIN")
        used_tuples.add(failed.tuple)
        results.append(f"{profile}: C->F R2_BODY fd={failed.fd} tuple={failed.tuple} EAGAIN->retry")
    return results


class TraceCheckerTests(unittest.TestCase):
    def fixture(self, *, wrong_direction=False, no_eagain=False,
                no_retry=False, reused_tuple=False, wrong_marker_tuple=False):
        lines = []
        logs = []
        for index, profile in enumerate(PROFILES):
            fd = 11
            local_port = 40000 if reused_tuple else 40000 + index
            remote_port = 41000 if reused_tuple else 41000 + index
            sock = f"127.0.0.1:{remote_port}->127.0.0.1:{local_port}" if wrong_direction else f"127.0.0.1:{local_port}->127.0.0.1:{remote_port}"
            def line(t, call):
                lines.append(f"{fd} {t:.6f} {call}")
            marked_tuple = "127.0.0.1:40099->127.0.0.1:41099" if wrong_marker_tuple else sock
            arm = f"P51_D03_EAGAIN_ARM profile={profile} tuple={marked_tuple} sndbuf=4608"
            line(index + 0.01, f'write(2, "{arm}\\n", {len(arm) + 1}) = {len(arm) + 1}')
            marker = f"P51_D03_EAGAIN_CASE profile={profile} exact=1 receipts=2 commits=2 credits=1 PASS"
            line(index + 0.09, f'write(2, "{marker}\\n", {len(marker) + 1}) = {len(marker) + 1}')
            hello = r'"\013\0\0\324"' if wrong_direction else r'"\n\0\0\265"'
            line(index + 0.02, f'sendto({fd}<TCP:[{sock}]>, {hello}, 4, MSG_NOSIGNAL, NULL, 0) = 4')
            header = r'"\16\10\0\25"'
            outcome = "= 4" if no_eagain else "= -1 EAGAIN (Resource temporarily unavailable)"
            line(index + 0.04, f'sendto({fd}<TCP:[{sock}]>, {header}, 4, MSG_NOSIGNAL, NULL, 0) {outcome}')
            if not no_retry:
                line(index + 0.05, f'sendto({fd}<TCP:[{sock}]>, {header}, 4, MSG_NOSIGNAL, NULL, 0) = 4')
            logs.append(marker)
        logs.append("P51_D03_KERNEL_EAGAIN_CANDIDATE_SELECTOR PASS")
        return "\n".join(lines), "\n".join(logs)

    def test_valid_three_profile_witness(self):
        trace, log = self.fixture()
        self.assertEqual(len(validate(trace, log, 0)), 3)

    def test_decodes_strace_c_escapes(self):
        self.assertEqual(c_string_bytes(r"\v\x41\101"), b"\x0bAA")

    def test_accepts_resumed_marker_write(self):
        trace, log = self.fixture()
        marker = "P51_D03_EAGAIN_CASE profile=ZSTD_TU exact=1 receipts=2 commits=2 credits=1 PASS"
        complete = f'write(2, "{marker}\\n", {len(marker) + 1}) = {len(marker) + 1}'
        unfinished = f'write(2, "{marker}\\n", {len(marker) + 1} <unfinished ...>'
        resumed = f'<... write resumed>) = {len(marker) + 1}'
        trace = trace.replace(complete, unfinished + "\n11 0.095000 " + resumed)
        self.assertEqual(len(validate(trace, log, 0)), 3)

    def test_accepts_fd_reuse_with_distinct_profile_tuples(self):
        trace, log = self.fixture()
        self.assertEqual(len(validate(trace, log, 0)), 3)

    def test_rejects_failed_selector(self):
        trace, log = self.fixture()
        with self.assertRaisesRegex(ValueError, "selector exit"):
            validate(trace, log, 1)

    def test_rejects_no_eagain(self):
        trace, log = self.fixture(no_eagain=True)
        with self.assertRaisesRegex(ValueError, "actual 4-byte R2_BODY EAGAIN"):
            validate(trace, log, 0)

    def test_rejects_missing_retry(self):
        trace, log = self.fixture(no_retry=True)
        with self.assertRaisesRegex(ValueError, "successful identical"):
            validate(trace, log, 0)

    def test_accepts_repeated_eagain_before_retry(self):
        trace, log = self.fixture()
        trace = trace.replace(
            'sendto(11<TCP:[127.0.0.1:40000->127.0.0.1:41000]>, "\\16\\10\\0\\25", 4, MSG_NOSIGNAL, NULL, 0) = 4',
            'sendto(11<TCP:[127.0.0.1:40000->127.0.0.1:41000]>, "\\16\\10\\0\\25", 4, MSG_NOSIGNAL, NULL, 0) = -1 EAGAIN (Resource temporarily unavailable)\n'
            '11 0.060000 sendto(11<TCP:[127.0.0.1:40000->127.0.0.1:41000]>, "\\16\\10\\0\\25", 4, MSG_NOSIGNAL, NULL, 0) = 4')
        self.assertEqual(len(validate(trace, log, 0)), 3)

    def test_rejects_reused_tuple_across_profiles(self):
        trace, log = self.fixture(reused_tuple=True)
        with self.assertRaisesRegex(ValueError, "reused TCP tuple"):
            validate(trace, log, 0)

    def test_rejects_mismatched_observed_socket_tuple(self):
        trace, log = self.fixture(wrong_marker_tuple=True)
        with self.assertRaisesRegex(ValueError, "differs from observed socket tuple"):
            validate(trace, log, 0)

    def test_rejects_wrong_direction_without_sender_hello(self):
        trace, log = self.fixture(wrong_direction=True)
        # A tuple whose LINK_HELLO is in reverse endpoint order does not
        # carry the client-originated R2 write under test.
        with self.assertRaisesRegex(ValueError, "preceding C->F LINK_HELLO"):
            validate(trace, log, 0)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--trace")
    parser.add_argument("--test-log")
    parser.add_argument("--exit-code", type=int)
    args = parser.parse_args()
    if args.self_test:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(TraceCheckerTests)
        return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1
    if not args.trace or not args.test_log or args.exit_code is None:
        parser.error("--trace, --test-log, and --exit-code are required")
    try:
        with open(args.trace, encoding="utf-8") as stream:
            trace = stream.read()
        with open(args.test_log, encoding="utf-8") as stream:
            test_log = stream.read()
        for result in validate(trace, test_log, args.exit_code):
            print(result)
    except (OSError, ValueError) as error:
        print(f"EAGAIN trace validation failed: {error}", file=sys.stderr)
        return 1
    print("P51_D03_KERNEL_EAGAIN_TRACE_CHECK PASS profiles=3")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
