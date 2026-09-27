#!/usr/bin/env python3
"""Prove production payload short-write continuation from Linux strace."""

import argparse
import re
import sys
import unittest
from dataclasses import dataclass


PROFILES = ("ZSTD_TU", "P29V1", "ZSTD_ROUTE")
LINE_RE = re.compile(r"^\s*(\d+)\s+(\d+\.\d+)\s+(.*)$")
SEND_RE = re.compile(
    r'^sendto\((\d+)(?:<TCP:\[([^]]+)\]>)?, "((?:\\.|[^"\\])*)", '
    r'(\d+), .* = (-1 (?:EAGAIN|EINTR) \([^)]*\)|\d+)$'
)
WRITE_RE = re.compile(r'^write\(2(?:<[^>]*>)?, "((?:\\.|[^"\\])*)", (\d+)\) = (\d+)$')


def c_string_bytes(value: str) -> bytes:
    out = bytearray()
    index = 0
    while index < len(value):
        if value[index] != "\\":
            out.extend(value[index].encode("utf-8"))
            index += 1
            continue
        index += 1
        if index == len(value):
            raise ValueError("trailing strace escape")
        if value[index] in "01234567":
            match = re.match(r"[0-7]{1,3}", value[index:])
            out.append(int(match.group(0), 8))
            index += len(match.group(0))
        elif value[index] == "x":
            match = re.match(r"[0-9a-fA-F]{1,2}", value[index + 1:])
            if not match:
                raise ValueError("invalid hexadecimal strace escape")
            out.append(int(match.group(0), 16))
            index += 1 + len(match.group(0))
        else:
            escaped = {
                "a": 7, "b": 8, "f": 12, "n": 10, "r": 13,
                "t": 9, "v": 11, "\\": 92, '"': 34,
            }
            if value[index] not in escaped:
                raise ValueError(f"unsupported strace escape \\{value[index]}")
            out.append(escaped[value[index]])
            index += 1
    return bytes(out)


@dataclass(frozen=True)
class Event:
    pid: int
    time: float
    fd: int
    tuple: str | None
    payload: bytes
    length: int
    result: str


def parse(trace: str):
    events = []
    markers = {}
    unfinished = {}
    for line in trace.splitlines():
        matched = LINE_RE.match(line)
        if not matched:
            continue
        pid, time_text, call = matched.groups()
        pid, timestamp = int(pid), float(time_text)
        resumed = re.match(r"^<\.\.\. (sendto|write) resumed>\) = (.*)$", call)
        if resumed:
            syscall = resumed.group(1)
            prior = unfinished.pop((pid, syscall), None)
            if prior is not None:
                original_time, original = prior
                call = f"{original}) = {resumed.group(2)}"
                if syscall == "write":
                    timestamp = original_time
        send = SEND_RE.match(call)
        if send:
            fd, tuple_text, raw, length, result = send.groups()
            events.append(Event(pid, timestamp, int(fd), tuple_text,
                                c_string_bytes(raw), int(length), result))
            continue
        if " <unfinished ...>" in call:
            syscall = ("sendto" if "sendto(" in call else
                       "write" if "write(2" in call else None)
            if syscall is not None:
                unfinished[(pid, syscall)] = (
                    timestamp, call.split(" <unfinished ...>", 1)[0])
            continue
        write = WRITE_RE.match(call)
        if write:
            raw, requested, actual = write.groups()
            payload = c_string_bytes(raw)
            if len(payload) != int(requested) or int(actual) != int(requested):
                continue
            for marker in payload.decode("utf-8", errors="replace").splitlines():
                if not marker.startswith("P51_D03_PAYLOAD_"):
                    continue
                profile = re.search(r"\bprofile=([A-Z0-9_]+)", marker)
                if profile:
                    markers.setdefault(profile.group(1), []).append((timestamp, marker))
    return events, markers


def validate(trace: str, test_log: str, exit_code: int):
    if exit_code != 0:
        raise ValueError(f"selector exit was {exit_code}")
    if "P51_D03_KERNEL_PAYLOAD_SHORTWRITE_SELECTOR PASS" not in test_log:
        raise ValueError("payload selector completion marker is absent")
    events, markers = parse(trace)
    reports = []
    used_tuples = set()
    for profile in PROFILES:
        profile_markers = markers.get(profile, [])
        def exact(tag):
            return [(time, line) for time, line in profile_markers
                    if tag in line]
        arms = exact("P51_D03_PAYLOAD_ARM")
        gates = exact("P51_D03_PAYLOAD_GATE ")
        releases = exact("P51_D03_PAYLOAD_GATE_RELEASE")
        cases = exact("P51_D03_PAYLOAD_CASE")
        for label, rows in (("ARM", arms), ("gate", gates),
                            ("release", releases), ("CASE", cases)):
            if len(rows) != 1:
                raise ValueError(f"{profile}: expected one {label} trace marker")
        arm_time, arm_line = arms[0]
        gate_time, gate_line = gates[0]
        release_time, _ = releases[0]
        case_time, _ = cases[0]
        if not arm_time < gate_time < release_time < case_time:
            raise ValueError(f"{profile}: gate marker timestamps are not ordered")
        case = f"P51_D03_PAYLOAD_CASE profile={profile} exact=1 receipts=2 commits=2 credits=1 PASS"
        if test_log.count(case) != 1:
            raise ValueError(f"{profile}: exact commit/receipt/credit marker missing")
        tuple_match = re.search(r"\btuple=([^ ]+)", arm_line)
        gate_component = re.search(r"\bcomponent=(R2_BODY|R2_FILL)\b", gate_line)
        gate_tu = re.search(r"\btu_seq=(\d+)\b", gate_line)
        gate_size = re.search(r"\bbytes=(\d+)\b", gate_line)
        expected_component = "R2_FILL" if profile == "P29V1" else "R2_BODY"
        if (not tuple_match or not gate_component or not gate_tu or not gate_size or
                gate_component.group(1) != expected_component or
                int(gate_tu.group(1)) != 1):
            raise ValueError(f"{profile}: missing socket or gated-job identity")
        expected_tuple = tuple_match.group(1)
        tuple_events = [event for event in events if event.tuple == expected_tuple]
        if not tuple_events:
            raise ValueError(f"{profile}: marked sender TCP tuple has no syscalls")
        sender_fd = tuple_events[0].fd
        socket_events = [event for event in tuple_events if event.fd == sender_fd]
        if expected_tuple in used_tuples:
            raise ValueError(f"{profile}: TCP tuple reused across profiles")
        used_tuples.add(expected_tuple)
        hellos = [event for event in socket_events
                  if event.payload[:1] == b"\x0a" and
                  not event.result.startswith("-1") and event.time < gate_time]
        if not hellos:
            raise ValueError(f"{profile}: marked socket lacks C-to-F LINK_HELLO")

        # Rebuild the stream from positive syscall return counts only. Failed
        # calls contribute no bytes even when strace prints their full suffix.
        stream = bytearray()
        positioned = []
        for event in socket_events:
            start = len(stream)
            positioned.append((event, start))
            if event.result.startswith("-1"):
                continue
            accepted = int(event.result)
            if len(event.payload) != event.length or accepted > event.length:
                raise ValueError(f"{profile}: strace truncated syscall bytes")
            stream.extend(event.payload[:accepted])

        components = []
        cursor = 0
        while cursor + 4 <= len(stream):
            message_type = stream[cursor]
            payload_bytes = int.from_bytes(stream[cursor + 1:cursor + 4], "big")
            end = cursor + 4 + payload_bytes
            if message_type in (14, 15) and payload_bytes:
                components.append((cursor, cursor + 4, end, payload_bytes))
            if end > len(stream):
                break
            cursor = end

        gated_bytes = int(gate_size.group(1))
        frame_type = 15 if expected_component == "R2_FILL" else 14
        matching_components = [component for component in components
                               if stream[component[0]] == frame_type and
                               component[3] == gated_bytes]
        if len(matching_components) != 1:
            raise ValueError(f"{profile}: gate component/size does not identify one frame")
        component_index = [component for component in components
                           if stream[component[0]] == frame_type].index(
                               matching_components[0])
        _, payload_start, component_end, component_size = matching_components[0]
        if component_index != int(gate_tu.group(1)):
            raise ValueError(f"{profile}: gated component ordinal differs from decoded frame order")
        shorts = [(event, start, int(event.result))
                  for event, start in positioned
                  if not event.result.startswith("-1") and
                  0 < int(event.result) < event.length and
                  start >= payload_start and
                  start + int(event.result) > payload_start and
                  start + int(event.result) < component_end and event.time < release_time]
        if not shorts:
            raise ValueError(f"{profile}: no positive short syscall reached a nonzero component-payload offset")
        short_event, short_start, short_result = shorts[0]
        suffix_offset = short_start + short_result
        suffix = short_event.payload[short_result:]
        # Asio may issue a fresh bounded write_some request at the same stream
        # offset, larger than the tail of the prior short call. Prove that its
        # requested prefix is exactly the prior unaccepted suffix; do not
        # require the syscall request itself to have the suffix's exact length.
        body_eagain = [(event, start) for event, start in positioned
                       if event.result.startswith("-1 EAGAIN") and
                       gate_time < event.time < release_time and
                       event.fd == short_event.fd and event.tuple == short_event.tuple and
                       start == suffix_offset and len(event.payload) >= len(suffix) and
                       event.payload[:len(suffix)] == suffix]
        retries = [(event, start) for event, start in positioned
                   if event.time > short_event.time and event.fd == short_event.fd and
                   event.tuple == short_event.tuple and start == suffix_offset and
                   len(event.payload) >= len(suffix) and
                   event.payload[:len(suffix)] == suffix and
                   not event.result.startswith("-1") and int(event.result) > 0 and
                   int(event.result) <= len(suffix)]
        if not retries:
            raise ValueError(f"{profile}: no exact successful suffix continuation")
        resumed = retries[0][0]
        if component_end > len(stream):
            raise ValueError(f"{profile}: gated component was not fully reconstructed from accepted syscall bytes")
        reports.append(
            f"{profile}: tuple={expected_tuple} component={expected_component} bytes={component_size} "
            f"short={short_result}/{short_event.length} accepted-offset="
            f"{short_start + short_result - payload_start} "
            f"resume={int(resumed.result)}/{resumed.length} exact-suffix=1 "
            f"same-offset-eagain={int(bool(body_eagain))}")
    return reports


class PayloadTraceTests(unittest.TestCase):
    @staticmethod
    def escaped(payload):
        return "".join(f"\\{byte:03o}" for byte in payload)

    def fixture(self, *, no_short=False, no_retry=False, wrong_fd=False,
                eintr_only=False, wrong_offset=False, duplicate_suffix=False,
                altered_suffix=False, resume_before_release=False):
        trace, log = [], ["P51_D03_KERNEL_PAYLOAD_SHORTWRITE_SELECTOR PASS"]
        for index, profile in enumerate(PROFILES):
            fd = 12
            sock = f"127.0.0.1:{40000+index}->127.0.0.1:{41000+index}"
            def emit(time, call, pid=fd):
                trace.append(f"{pid} {time:.6f} {call}")
            def write_marker(time, marker):
                payload = (marker + "\n").encode()
                emit(time, f'write(2, "{payload[:-1].decode()}\\n", {len(payload)}) = {len(payload)}')
                log.append(marker)
            arm = f"P51_D03_PAYLOAD_ARM profile={profile} tuple={sock} sndbuf=4096"
            component = "R2_FILL" if profile == "P29V1" else "R2_BODY"
            gate = f"P51_D03_PAYLOAD_GATE profile={profile} component={component} tu_seq=1 bytes=100 offset=0"
            release = f"P51_D03_PAYLOAD_GATE_RELEASE profile={profile}"
            case = f"P51_D03_PAYLOAD_CASE profile={profile} exact=1 receipts=2 commits=2 credits=1 PASS"
            write_marker(index + .01, arm)
            write_marker(index + .02, gate)
            release_time = index + .08
            write_marker(release_time, release)
            write_marker(index + .09, case)
            hello = bytes([10, 0, 0, 1, ord("x")])
            emit(index + .015, f'sendto({fd}<TCP:[{sock}]>, "{self.escaped(hello)}", 5, MSG_NOSIGNAL, NULL, 0) = 5')
            frame_type = 15 if profile == "P29V1" else 14
            first_body = bytes([frame_type, 0, 0, 3, ord("a"), ord("b"), ord("c")])
            emit(index + .018, f'sendto({fd}<TCP:[{sock}]>, "{self.escaped(first_body)}", 7, MSG_NOSIGNAL, NULL, 0) = 7')
            body = bytes([frame_type, 0, 0, 100]) + bytes(range(100))
            emit(index + .025, f'sendto({fd}<TCP:[{sock}]>, "{self.escaped(body[:4])}", 4, MSG_NOSIGNAL, NULL, 0) = 4')
            if no_short:
                emit(index + .03, f'sendto({fd}<TCP:[{sock}]>, "{self.escaped(body[4:])}", 100, MSG_NOSIGNAL, NULL, 0) = 100')
                continue
            emit(index + .03, f'sendto({fd}<TCP:[{sock}]>, "{self.escaped(body[4:])}", 100, MSG_NOSIGNAL, NULL, 0) = 40')
            if eintr_only:
                emit(index + .085, f'sendto({fd}<TCP:[{sock}]>, "{self.escaped(body[44:])}", 60, MSG_NOSIGNAL, NULL, 0) = -1 EINTR (Interrupted system call)')
                continue
            retry_start = (39 if duplicate_suffix else
                           45 if wrong_offset else 44)
            retry_fd = fd + 1 if wrong_fd else fd
            retry_payload = bytearray(body[retry_start:])
            if altered_suffix and retry_payload:
                retry_payload[0] ^= 1
            retry_time = index + (.04 if resume_before_release else .085)
            # A fresh write_some may request more than the old call's suffix.
            requested = len(retry_payload)
            if not duplicate_suffix and not wrong_offset and not altered_suffix:
                retry_payload = bytearray(body[44:]) + b"extra"
                requested = len(retry_payload)
            emit(retry_time, f'sendto({retry_fd}<TCP:[{sock}]>, "{self.escaped(retry_payload)}", {requested}, MSG_NOSIGNAL, NULL, 0) = {min(requested, 60)}', retry_fd)
            if no_retry:
                trace.pop()
        return "\n".join(trace), "\n".join(log)

    def test_accepts_all_profiles(self):
        self.assertEqual(len(validate(*self.fixture(), 0)), 3)

    def test_rejects_missing_positive_short(self):
        with self.assertRaisesRegex(ValueError, "no positive short"):
            validate(*self.fixture(no_short=True), 0)

    def test_rejects_eintr_as_resume(self):
        with self.assertRaisesRegex(ValueError, "no exact successful suffix continuation"):
            validate(*self.fixture(eintr_only=True), 0)

    def test_decodes_standard_strace_control_escapes(self):
        self.assertEqual(c_string_bytes(r"\a\b\f\v\n\r\t\\\""),
                         bytes((7, 8, 12, 11, 10, 13, 9, 92, 34)))

    def test_rejects_missing_resume(self):
        with self.assertRaisesRegex(ValueError, "no exact successful suffix continuation"):
            validate(*self.fixture(no_retry=True), 0)

    def test_rejects_other_fd_resume(self):
        with self.assertRaisesRegex(ValueError, "no exact successful suffix continuation"):
            validate(*self.fixture(wrong_fd=True), 0)

    def test_rejects_wrong_resume_offset(self):
        with self.assertRaisesRegex(ValueError, "no exact successful suffix continuation"):
            validate(*self.fixture(wrong_offset=True), 0)

    def test_accepts_continuation_before_or_after_release(self):
        self.assertEqual(len(validate(*self.fixture(resume_before_release=True), 0)), 3)
        self.assertEqual(len(validate(*self.fixture(), 0)), 3)

    def test_rejects_duplicated_suffix(self):
        with self.assertRaisesRegex(ValueError, "no exact successful suffix continuation"):
            validate(*self.fixture(duplicate_suffix=True), 0)

    def test_rejects_altered_suffix(self):
        with self.assertRaisesRegex(ValueError, "no exact successful suffix continuation"):
            validate(*self.fixture(altered_suffix=True), 0)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--trace")
    parser.add_argument("--test-log")
    parser.add_argument("--exit-code", type=int)
    args = parser.parse_args()
    if args.self_test:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(PayloadTraceTests)
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
        print(f"payload short-write validation failed: {error}", file=sys.stderr)
        return 1
    print("P51_D03_KERNEL_PAYLOAD_SHORTWRITE_TRACE_CHECK PASS profiles=3")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
