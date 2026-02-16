#!/usr/bin/env python3
import json
import os
import re
import sys
from pathlib import Path


def read_text(path: Path) -> str:
    try:
        return path.read_text(errors="replace")
    except FileNotFoundError:
        return ""


def read_jsonl(path: Path):
    rows = []
    with path.open("r", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    return rows


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: verify.py <out_dir>", file=sys.stderr)
        return 2

    out_dir = Path(sys.argv[1])
    if not out_dir.is_dir():
        print(f"verify.py: missing out_dir {out_dir}", file=sys.stderr)
        return 2

    failures = []

    # Collect scheduler mapping of nodeName->ip from listcs.
    sched_listcs = read_text(out_dir / "scheduler.listcs.txt")
    node_to_ip = {}
    for line in sched_listcs.splitlines():
        m = re.match(r"^ (build[1-4]) \(([^:]+):\d+\) ", line)
        if m:
            node_to_ip[m.group(1)] = m.group(2)

    if len(node_to_ip) != 4:
        failures.append(f"scheduler.listcs.txt did not show 4 build servers (got {len(node_to_ip)})")

    build_servers_used = {k: False for k in ("build1", "build2", "build3", "build4")}

    # Check worker logs indicate remote use.
    for worker in ("worker1", "worker2"):
        wdir = out_dir / worker
        exitcode_txt = read_text(wdir / "build.exitcode.txt").strip()
        if exitcode_txt != "0":
            failures.append(f"{worker} build.exitcode.txt != 0 (got {exitcode_txt or '<missing>'})")

        icecc_log = read_text(wdir / "icecc.log")
        hosts = set(re.findall(r"Have to use host ([^:]+):\d+", icecc_log))
        if not hosts:
            failures.append(f"{worker} icecc.log had no 'Have to use host' lines (remote compile missing?)")
        if node_to_ip:
            ips = set(node_to_ip.values())
            for ip in hosts:
                if ip in ips:
                    for node, node_ip in node_to_ip.items():
                        if node_ip == ip:
                            build_servers_used[node] = True

    # Check build servers were actually used via state JSONL.
    for build in ("build1", "build2", "build3", "build4"):
        state_path = out_dir / build / "iceccd.state.jsonl"
        rows = read_jsonl(state_path) if state_path.exists() else []
        max_used = 0
        for row in rows:
            try:
                used = int(row.get("slots", {}).get("used", 0))
            except (TypeError, ValueError):
                used = 0
            max_used = max(max_used, used)
        if max_used <= 0:
            failures.append(f"{build} never showed slots.used>0 in iceccd.state.jsonl")

    unused = [b for b, used in build_servers_used.items() if not used]
    if unused:
        failures.append(f"workers did not appear to use: {', '.join(unused)} (based on scheduler ip mapping + icecc logs)")

    if failures:
        print("FAIL", file=sys.stderr)
        for f in failures:
            print(f"- {f}", file=sys.stderr)
        return 1

    print("OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
