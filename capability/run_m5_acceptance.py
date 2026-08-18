#!/usr/bin/env python3
"""One-command protocol-50 M5 scenario and evidence launcher.

The launcher builds the exact C/F socket harness, generates deterministic
header/generated-file A/B/A fixtures, runs each requested scenario to
completion, and refuses to publish a row unless the physical frame ledger and
per-TU curve both close to the literal socket total.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import shlex
import subprocess
import sys
from dataclasses import asdict, dataclass
from pathlib import Path


CORPORA = (
    ("llvm", "corpus"),
    ("rocksdb", "corpus2"),
    ("duckdb", "corpus3"),
    ("abseil", "corpus4"),
    ("opencv", "corpus5"),
    ("godot", "corpus6"),
    ("fmt", "corpus7"),
    ("spdlog", "corpus8"),
    ("catch2", "corpus9"),
    ("nlohmann-json", "corpus10"),
    ("range-v3", "corpus11"),
    ("eigen", "corpus12"),
    ("re2", "corpus13"),
    ("leveldb", "corpus14"),
    ("simdjson", "corpus15"),
    ("cereal", "corpus16"),
)
MARKER = re.compile(rb'^#\s+\d+\s+"([^"]+)"')
CORPUS_FINGERPRINTS: dict[tuple[str, int | None], str] = {}


@dataclass(frozen=True)
class RunSpec:
    name: str
    scope: str
    manifest: str
    codec: str = "z1"
    extra: tuple[str, ...] = ()
    max_files: int | None = None
    phase_names: tuple[str, ...] = ()
    phase_lengths: tuple[int, ...] = ()
    expected_failures: int = 0
    expected_prepared_aborts: int = 0
    minimum_relationship_gbps: float | None = None
    minimum_stage_gbps: float | None = None
    minimum_complete_gbps: float | None = None
    one_pass: bool = False
    wire_equivalent_to: str | None = None


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def kv_fields(line: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for token in line.split()[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            result[key] = value
    return result


def parse_log(text: str) -> dict[str, object]:
    result: dict[str, object] = {"components": {}, "frames": {}}
    for line in text.splitlines():
        if line.startswith("RESULT "):
            result.update(kv_fields(line))
        elif line.startswith("FRAME_LEDGER "):
            fields = kv_fields(line)
            result["frame_closure"] = fields.pop("closure")
            result["frame_total"] = int(fields.pop("total"))
            result["frames"] = {
                name: {
                    "bytes": int(value.split("/")[0]),
                    "count": int(value.split("/")[1]),
                }
                for name, value in fields.items()
            }
        elif line.startswith("CURVE_SUMMARY "):
            fields = kv_fields(line)
            result["c50"] = float(fields["C50"])
            result["c50_tu"] = int(fields["C50_TU"])
            result["h200"] = None if fields["H200"] == "none" else float(fields["H200"])
            result["h200_tu"] = (
                None if fields["H200_TU"] == "none" else int(fields["H200_TU"])
            )
            result["second_half"] = float(fields["SECOND_HALF"])
        elif line.startswith("CACHE "):
            fields = kv_fields(line)
            result["cache_regions"], result["cache_region_bytes"] = map(
                int, fields["regions"].split("/")
            )
            result["cache_public"], result["cache_public_bytes"] = map(
                int, fields["public"].split("/")
            )
            result["cache_blocks"], result["cache_block_bytes"] = map(
                int, fields["blocks"].split("/")
            )
            removals = list(map(int, fields["removals"].split("/")))
            (
                result["region_removals"],
                result["public_removals"],
                result["block_removals"],
            ) = removals
            result["compactions"] = int(fields["compactions"])
        elif line.startswith("THROUGHPUT "):
            fields = kv_fields(line)
            result["c_transform_gbps"] = float(fields["C_transform"])
            result["f_decode_cpu_gbps"] = float(fields["F_decode_cpu"])
            result["relationship_gbps"] = float(fields["relationship"])
            result["wall_seconds"] = float(fields["wall"].removesuffix("s"))
            result["c_peak_mib"] = float(fields.get("C_peak", "0MiB").removesuffix("MiB"))
            result["f_peak_mib"] = float(fields["F_peak"].removesuffix("MiB"))
        elif line.startswith("PIPE_PATH "):
            fields = kv_fields(line)
            result["pipe_mode"] = fields["mode"]
            result["c_pipe_to_wire_gbps"] = float(fields["C_pipe_to_wire"])
            result["f_wire_to_compiler_pipe_gbps"] = float(
                fields["F_wire_to_compiler_pipe"]
            )
            result["f_aggregate_gbps"] = float(fields["F_aggregate"])
            result["f_pipe_write_gbps"] = float(fields["F_pipe_write"])
            result["complete_gbps"] = float(fields["complete"])
            result["compiler_bytes"] = int(fields["compiler_bytes"])
            result["compiler_tus"] = int(fields["compiler_tus"])
            result["compiler_measured_bytes"] = int(fields["compiler_measured_bytes"])
            result["compiler_summary_bytes"] = int(fields["compiler_summary_bytes"])
            result["compiler_summary_tus"] = int(fields["compiler_summary_tus"])
            result["summary_lost_workers"] = int(fields["summary_lost_workers"])
        elif line.startswith("TRANSACTIONS "):
            fields = kv_fields(line)
            result["transaction_closure"] = fields["closure"]
            result["prepared_accepted"] = int(fields["prepared_accepted"])
            result["decode_rejected"] = int(fields["decode_rejected"])
            result["transaction_committed"] = int(fields["committed"])
            result["transaction_aborted"] = int(fields["aborted"])
        elif line.startswith("PREP_RATE "):
            fields = kv_fields(line)
            result["c_source_pipe_gbps"] = float(fields["source_pipe"])
            result["c_interning_gbps"] = float(fields["interning"])
            result["c_factorization_gbps"] = float(fields["factorization"])
        elif line.startswith("COMPONENT "):
            match = re.match(r"COMPONENT\s+(\S+)\s+(.*)", line)
            if match:
                result["components"][match.group(1)] = kv_fields("X " + match.group(2))
    required = {
        "exact",
        "actual_socket",
        "raw",
        "tus",
        "failures",
        "frame_closure",
        "frame_total",
        "c50",
        "c50_tu",
        "h200",
        "h200_tu",
        "second_half",
        "c_transform_gbps",
        "f_decode_cpu_gbps",
        "relationship_gbps",
        "c_peak_mib",
        "f_peak_mib",
        "pipe_mode",
        "c_pipe_to_wire_gbps",
        "f_wire_to_compiler_pipe_gbps",
        "f_aggregate_gbps",
        "f_pipe_write_gbps",
        "complete_gbps",
        "compiler_bytes",
        "compiler_tus",
        "compiler_measured_bytes",
        "compiler_summary_bytes",
        "compiler_summary_tus",
        "summary_lost_workers",
        "transaction_closure",
        "prepared_accepted",
        "decode_rejected",
        "transaction_committed",
        "transaction_aborted",
        "c_source_pipe_gbps",
        "c_interning_gbps",
        "c_factorization_gbps",
    }
    missing = sorted(required - result.keys())
    if missing:
        raise ValueError(f"M5 log lacks fields: {', '.join(missing)}")
    for name in ("workers", "wave", "tus", "raw", "actual_socket", "failures"):
        result[name] = int(result[name])
    result["ratio"] = float(result["ratio"])
    return result


def read_curve(path: Path) -> list[dict[str, int]]:
    with path.open(newline="") as source:
        rows = [
            {key: int(value) for key, value in row.items()}
            for row in csv.DictReader(source, delimiter="\t")
        ]
    for expected, row in enumerate(rows):
        if row["logical"] != expected:
            raise ValueError(f"{path}: non-contiguous logical row {row['logical']}")
    return rows


def latency_percentile_ms(rows: list[dict[str, int]], percentile: float) -> float:
    values = sorted(row["latency_ns"] for row in rows)
    if not values:
        return 0.0
    index = max(0, min(len(values) - 1, int(percentile * len(values) + 0.999999) - 1))
    return values[index] / 1e6


def wire_curve_signature(path: Path) -> list[tuple[int, ...]]:
    ignored = {"latency_ns"}
    return [
        tuple(value for key, value in row.items() if key not in ignored)
        for row in read_curve(path)
    ]


def phase_summary(
    rows: list[dict[str, int]], names: tuple[str, ...], lengths: tuple[int, ...]
) -> list[dict[str, object]]:
    if not names:
        return []
    if len(names) != len(lengths) or sum(lengths) != len(rows):
        raise ValueError("phase declaration does not cover the complete curve")
    result: list[dict[str, object]] = []
    offset = 0
    for name, length in zip(names, lengths, strict=True):
        selected = rows[offset : offset + length]
        raw = sum(row["raw"] for row in selected)
        wire = sum(row["wire"] for row in selected)
        result.append(
            {
                "name": name,
                "begin_tu": offset + 1,
                "end_tu": offset + length,
                "raw": raw,
                "wire": wire,
                "ratio": raw / wire if wire else None,
            }
        )
        offset += length
    return result


def build(source: Path, output: Path, cxx: str) -> tuple[Path, Path, list[list[str]]]:
    binary = output / "cap_m5"
    stream_binary = output / "cap_m5_stream"
    header_test = output / "cap_header_test"
    transport_test = output / "cap_transport_test"
    m2_test = output / "cap_m2_test"
    state_test = output / "cap_m5_state_test"
    m4_test = output / "cap_m4_test"
    common = [
        cxx,
        "-O3",
        "-DNDEBUG",
        "-march=native",
        "-std=c++17",
        "-DICE_LINE_CAP_LOG2=23",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-Werror",
    ]
    commands = [
        common
        + [
            str(source / "cap_m5_main.cpp"),
            str(source / "cap_codec.cpp"),
            "-o",
            str(binary),
            "-lzstd",
            "-pthread",
        ],
        common
        + [
            str(source / "cap_m5_stream_main.cpp"),
            str(source / "cap_codec.cpp"),
            "-o",
            str(stream_binary),
            "-lzstd",
            "-pthread",
        ],
        common
        + [
            str(source / "cap_header_test.cpp"),
            "-o",
            str(header_test),
        ],
        common
        + [
            str(source / "cap_transport_test.cpp"),
            "-o",
            str(transport_test),
        ],
        common
        + [
            str(source / "cap_m2_test.cpp"),
            str(source / "cap_codec.cpp"),
            "-o",
            str(m2_test),
            "-lzstd",
            "-pthread",
        ],
        common
        + [
            str(source / "cap_m5_state_test.cpp"),
            str(source / "cap_codec.cpp"),
            "-o",
            str(state_test),
            "-lzstd",
            "-pthread",
        ],
        common
        + [
            str(source / "cap_m4_test.cpp"),
            str(source / "cap_codec.cpp"),
            "-o",
            str(m4_test),
            "-lzstd",
            "-pthread",
        ],
    ]
    for command in commands:
        subprocess.run(command, check=True)
    for test in (header_test, transport_test, m2_test, state_test, m4_test):
        completed = subprocess.run(
            test, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
        )
        (output / "logs" / f"{test.name}.log").write_text(completed.stdout)
        if completed.returncode:
            raise RuntimeError(f"{test.name} failed")
    return binary, stream_binary, commands


def manifest_paths(path: Path, limit: int | None = None) -> list[Path]:
    values = [Path(line) for line in path.read_text().splitlines() if line]
    return values if limit is None else values[:limit]


def corpus_fingerprint(manifest: str, limit: int | None) -> str:
    key = (manifest, limit)
    retained = CORPUS_FINGERPRINTS.get(key)
    if retained is not None:
        return retained
    digest = hashlib.sha256(b"protocol-50-m5-corpus-v1\0")
    for path in manifest_paths(Path(manifest), limit):
        encoded = str(path).encode()
        digest.update(len(encoded).to_bytes(8, "little"))
        digest.update(encoded)
        size = path.stat().st_size
        digest.update(size.to_bytes(8, "little"))
        with path.open("rb") as source:
            for block in iter(lambda: source.read(1 << 20), b""):
                digest.update(block)
    retained = digest.hexdigest()
    CORPUS_FINGERPRINTS[key] = retained
    return retained


def marker_lines(data: bytes):
    current = b""
    for line in data.splitlines(keepends=True):
        match = MARKER.match(line)
        if match:
            current = match.group(1)
        else:
            yield current, line


def choose_fixture_lines(
    paths: list[Path],
) -> tuple[tuple[bytes, bytes], tuple[bytes, bytes]]:
    header: dict[tuple[bytes, bytes], int] = {}
    generated: dict[tuple[bytes, bytes], int] = {}
    for ordinal, path in enumerate(paths):
        seen_header: set[tuple[bytes, bytes]] = set()
        seen_generated: set[tuple[bytes, bytes]] = set()
        for source_path, line in marker_lines(path.read_bytes()):
            if (
                source_path
                and 24 <= len(line) <= 512
                and line.endswith(b"\n")
                and not line.endswith(b"\\\n")
                and line.strip()
                and (b".h" in source_path or b".inc" in source_path)
            ):
                seen_header.add((source_path, line))
            numeric = sum(byte in b"0123456789abcdefABCDEF" for byte in line)
            if (
                source_path
                and 32 <= len(line) <= 4096
                and line.count(b",") >= 3
                and numeric >= 12
                and (b"0x" in line or b"0X" in line)
            ):
                seen_generated.add((source_path, line))
        bit = 1 << ordinal
        for key in seen_header:
            header[key] = header.get(key, 0) | bit
        for key in seen_generated:
            generated[key] = generated.get(key, 0) | bit
    if not header:
        raise RuntimeError("no shared header-line fixture candidate")
    header_key = max(
        header,
        key=lambda key: (header[key].bit_count(), len(key[1]), key[0], key[1]),
    )
    if not generated:
        raise RuntimeError("no generated-line fixture candidate")
    generated_key = max(
        generated,
        key=lambda key: (
            generated[key].bit_count(),
            key[1].count(b","),
            len(key[1]),
            key[0],
        ),
    )
    return header_key, generated_key


def generated_replacement(line: bytes) -> bytes:
    start = line.find(b"0x")
    if start < 0:
        start = line.find(b"0X")
    if start < 0 or start + 2 >= len(line):
        raise ValueError("generated fixture lacks a hexadecimal value")
    mutable = bytearray(line)
    position = start + 2
    mutable[position] = ord("1") if mutable[position] != ord("1") else ord("2")
    return bytes(mutable)


def write_variant(
    paths: list[Path],
    directory: Path,
    replacements: tuple[tuple[bytes, bytes, bytes], ...],
) -> tuple[list[Path], dict[str, int]]:
    directory.mkdir(parents=True, exist_ok=True)
    result: list[Path] = []
    changed_files = 0
    changed_lines = 0
    for ordinal, source in enumerate(paths):
        data = source.read_bytes()
        output = bytearray()
        current = b""
        local_changes = 0
        for line in data.splitlines(keepends=True):
            marker = MARKER.match(line)
            if marker:
                current = marker.group(1)
                output += line
                continue
            replacement = line
            for expected_path, before, after in replacements:
                if current == expected_path and line == before:
                    replacement = after
                    local_changes += 1
                    break
            output += replacement
        if local_changes:
            target = directory / f"{ordinal:08d}.ii"
            target.write_bytes(output)
            result.append(target.resolve())
            changed_files += 1
            changed_lines += local_changes
        else:
            result.append(source.resolve())
    return result, {"changed_files": changed_files, "changed_lines": changed_lines}


def write_manifest(path: Path, groups: tuple[list[Path], ...]) -> None:
    path.write_text("".join(f"{item}\n" for group in groups for item in group))


def evolution_fixtures(
    corpus_root: Path, output: Path, limit: int
) -> tuple[list[RunSpec], dict[str, object]]:
    base = manifest_paths(corpus_root / "corpus6" / "manifest.txt", limit)
    header, generated = choose_fixture_lines(base)
    header_after = header[1][:-1] + b" \n"
    generated_after = generated_replacement(generated[1])
    fixture_root = output / "fixtures"
    header_paths, header_stats = write_variant(
        base, fixture_root / "header", ((header[0], header[1], header_after),)
    )
    generated_paths, generated_stats = write_variant(
        base,
        fixture_root / "generated",
        ((generated[0], generated[1], generated_after),),
    )
    branch_paths, branch_stats = write_variant(
        base,
        fixture_root / "branch",
        (
            (header[0], header[1], header_after),
            (generated[0], generated[1], generated_after),
        ),
    )
    manifests = fixture_root / "manifests"
    manifests.mkdir(parents=True, exist_ok=True)
    definitions = {
        "header-edit": ((base, header_paths), ("A", "header-B")),
        "generated-edit": ((base, generated_paths), ("A", "generated-B")),
        "header-revert": ((base, header_paths, base), ("A1", "header-B", "A2")),
        "branch-a-b-a": ((base, branch_paths, base), ("A1", "branch-B", "A2")),
    }
    specs: list[RunSpec] = []
    for name, (groups, phases) in definitions.items():
        manifest = manifests / f"{name}.txt"
        write_manifest(manifest, groups)
        specs.append(
            RunSpec(
                f"evolution-{name}",
                f"godot-first-{limit}",
                str(manifest),
                phase_names=phases,
                phase_lengths=tuple(len(group) for group in groups),
            )
        )
    metadata: dict[str, object] = {
        "source_manifest": str(corpus_root / "corpus6" / "manifest.txt"),
        "tus_per_phase": len(base),
        "header": {
            "path": header[0].decode(errors="replace"),
            "before_sha256": hashlib.sha256(header[1]).hexdigest(),
            "after_sha256": hashlib.sha256(header_after).hexdigest(),
            **header_stats,
        },
        "generated": {
            "path": generated[0].decode(errors="replace"),
            "before_sha256": hashlib.sha256(generated[1]).hexdigest(),
            "after_sha256": hashlib.sha256(generated_after).hexdigest(),
            **generated_stats,
        },
        "branch": branch_stats,
    }
    (fixture_root / "fixture-metadata.json").write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n"
    )
    return specs, metadata


def smoke_specs(
    corpus_root: Path, output: Path, evolution_tus: int
) -> tuple[list[RunSpec], dict[str, object]]:
    fmt = str(corpus_root / "corpus7" / "manifest.txt")
    common = {"scope": "fmt-first-20", "manifest": fmt, "max_files": 20}
    specs = [
        RunSpec("cold-standard", **common),
        RunSpec("cold-standard-replay", **common, wire_equivalent_to="cold-standard"),
        RunSpec("cache50-bit0", **common, extra=("--cache50", "0")),
        RunSpec("cache50-bit1", **common, extra=("--cache50", "1")),
        RunSpec("order-reverse", **common, extra=("--order", "reverse")),
        RunSpec(
            "order-shuffle-1", **common, extra=("--order", "shuffle", "--seed", "11")
        ),
        RunSpec(
            "order-shuffle-2", **common, extra=("--order", "shuffle", "--seed", "29")
        ),
        RunSpec(
            "order-shuffle-3", **common, extra=("--order", "shuffle", "--seed", "47")
        ),
        RunSpec("order-novelty-max", **common, extra=("--order", "novelty-max")),
        RunSpec(
            "snapshot-raw50",
            **common,
            extra=(
                "--workers",
                "2",
                "--wave",
                "2",
                "--snapshot50-prefix",
                str(output / "snapshots" / "fmt20"),
            ),
        ),
        RunSpec(
            "bounded-all-stores",
            **common,
            extra=(
                "--workers",
                "2",
                "--wave",
                "2",
                "--region-bytes",
                "65536",
                "--public-bytes",
                "65536",
                "--block-bytes",
                "2048",
            ),
        ),
        RunSpec(
            "worker-restart",
            **common,
            extra=("--workers", "4", "--wave", "4", "--restart-at", "9"),
        ),
        RunSpec(
            "worker-latejoin",
            **common,
            extra=("--workers", "4", "--wave", "4", "--latejoin-at", "7"),
        ),
        RunSpec(
            "cache50-worker-restart",
            **common,
            extra=(
                "--cache50",
                "0",
                "--workers",
                "4",
                "--wave",
                "4",
                "--restart-at",
                "9",
            ),
        ),
        RunSpec(
            "cache50-worker-latejoin",
            **common,
            extra=(
                "--cache50",
                "1",
                "--workers",
                "4",
                "--wave",
                "4",
                "--latejoin-at",
                "7",
            ),
        ),
        RunSpec(
            "bounded-worker-restart",
            **common,
            extra=(
                "--workers",
                "4",
                "--wave",
                "4",
                "--restart-at",
                "9",
                "--region-bytes",
                "65536",
                "--public-bytes",
                "65536",
                "--block-bytes",
                "2048",
            ),
        ),
        RunSpec(
            "bounded-worker-latejoin",
            **common,
            extra=(
                "--workers",
                "4",
                "--wave",
                "4",
                "--latejoin-at",
                "7",
                "--region-bytes",
                "65536",
                "--public-bytes",
                "65536",
                "--block-bytes",
                "2048",
            ),
        ),
        RunSpec(
            "bounded-snapshot",
            **common,
            extra=(
                "--workers",
                "2",
                "--wave",
                "2",
                "--snapshot50-prefix",
                str(output / "snapshots" / "fmt20-bounded"),
                "--region-bytes",
                "65536",
                "--public-bytes",
                "65536",
                "--block-bytes",
                "2048",
            ),
        ),
        RunSpec(
            "fill-reject-retry",
            **common,
            extra=("--workers", "4", "--wave", "4", "--corrupt-tu", "5"),
            expected_failures=1,
            expected_prepared_aborts=2,
        ),
        RunSpec(
            "host-concurrency-24",
            "fmt-complete",
            fmt,
            extra=("--workers", "24", "--wave", "24"),
        ),
    ]
    for workers in (1, 4, 8, 16, 32):
        for assignment in ("sticky", "roundrobin", "random"):
            specs.append(
                RunSpec(
                    f"mesh-{workers}f-{assignment}",
                    **common,
                    extra=(
                        "--workers",
                        str(workers),
                        "--wave",
                        str(workers),
                        "--assignment",
                        assignment,
                    ),
                )
            )
        if workers > 1:
            specs.append(
                RunSpec(
                    f"mesh-{workers}f-failover",
                    **common,
                    extra=(
                        "--workers",
                        str(workers),
                        "--wave",
                        str(workers),
                        "--assignment",
                        "failover",
                        "--failover-at",
                        "8",
                    ),
                )
            )
    specs.extend(
        (
            RunSpec(
                "onepass-typed-grow",
                **common,
                extra=("--workers", "4", "--wave", "4"),
                one_pass=True,
                wire_equivalent_to="mesh-4f-roundrobin",
            ),
            RunSpec(
                "onepass-typed-bounded",
                **common,
                extra=(
                    "--workers",
                    "4",
                    "--wave",
                    "4",
                    "--region-bytes",
                    "65536",
                    "--public-bytes",
                    "65536",
                    "--block-bytes",
                    "2048",
                ),
                one_pass=True,
            ),
        )
    )
    evolution, metadata = evolution_fixtures(corpus_root, output, evolution_tus)
    specs.extend(evolution)
    return specs, metadata


def full_specs(corpus_root: Path, output: Path, selected: set[str]) -> list[RunSpec]:
    specs: list[RunSpec] = []
    for corpus, directory in CORPORA:
        if corpus not in selected:
            continue
        manifest = str(corpus_root / directory / "manifest.txt")
        for codec in ("z1", "z3"):
            specs.append(
                RunSpec(
                    f"full-cold-{corpus}-{codec}",
                    corpus,
                    manifest,
                    codec=codec,
                )
            )
        for bit in (0, 1):
            specs.append(
                RunSpec(
                    f"full-cache50-{corpus}-bit{bit}",
                    corpus,
                    manifest,
                    extra=("--cache50", str(bit)),
                )
            )
        specs.append(
            RunSpec(
                f"full-snapshot-{corpus}",
                corpus,
                manifest,
                extra=(
                    "--snapshot50-prefix",
                    str(output / "snapshots" / f"full-{corpus}"),
                ),
            )
        )
        for order, seed in (
            ("reverse", None),
            ("shuffle", 11),
            ("shuffle", 29),
            ("shuffle", 47),
            ("novelty-max", None),
        ):
            suffix = order if seed is None else f"{order}-{seed}"
            extra = ("--order", order) + (() if seed is None else ("--seed", str(seed)))
            specs.append(
                RunSpec(
                    f"full-order-{corpus}-{suffix}",
                    corpus,
                    manifest,
                    extra=extra,
                )
            )
    specs.extend(onepass_specs(corpus_root, selected))
    return specs


def onepass_specs(corpus_root: Path, selected: set[str]) -> list[RunSpec]:
    if "duckdb" not in selected:
        return []
    manifest = str(corpus_root / "corpus3" / "manifest.txt")
    specs = [
        RunSpec(
            f"throughput-duckdb-{codec}-8f",
            "throughput",
            manifest,
            codec=codec,
            extra=("--workers", "8", "--wave", "8"),
            minimum_relationship_gbps=1.0,
            minimum_stage_gbps=1.0,
            minimum_complete_gbps=1.0,
            one_pass=True,
        )
        for codec in ("z1", "z3")
    ]
    specs.extend(
        RunSpec(
            f"scale-duckdb-z3-{workers}f",
            "scaling",
            manifest,
            codec="z3",
            extra=(
                "--workers",
                str(workers),
                "--wave",
                str(workers),
            ),
            one_pass=True,
        )
        for workers in (1, 4, 16, 32)
    )
    return specs


def run_one(
    binary: Path,
    stream_binary: Path,
    output: Path,
    spec: RunSpec,
    timeout: int,
    resume: bool,
) -> dict[str, object]:
    log = output / "logs" / f"{spec.name}.log"
    curve = output / "curves" / f"{spec.name}.tsv"
    selected_binary = stream_binary if spec.one_pass else binary
    command = [
        str(selected_binary),
        "--manifest",
        spec.manifest,
        "--codec",
        spec.codec,
        "--curve-out",
        str(curve),
        "--real-pipes",
    ]
    if spec.max_files is not None:
        command += ["--max-files", str(spec.max_files)]
    command += spec.extra
    command_record = f"COMMAND {shlex.join(command)}\n"
    binary_record = f"BINARY_SHA256 {sha256(selected_binary)}\n"
    corpus_record = (
        f"CORPUS_SHA256 {corpus_fingerprint(spec.manifest, spec.max_files)}\n"
    )
    reusable = False
    if resume and log.is_file() and curve.is_file():
        retained = log.read_text(errors="replace")
        reusable = retained.startswith(command_record + binary_record + corpus_record)
    if not reusable:
        completed = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout,
        )
        log.write_text(
            command_record
            + binary_record
            + corpus_record
            + f"EXIT {completed.returncode}\n{completed.stdout}"
        )
        if completed.returncode:
            raise RuntimeError(f"{spec.name} exited {completed.returncode}; see {log}")
    text = log.read_text(errors="replace")
    parsed = parse_log(text)
    curve_rows = read_curve(curve)
    if parsed["exact"] != "OK" or parsed["frame_closure"] != "OK":
        raise RuntimeError(f"{spec.name}: exact/frame closure failed")
    if parsed["transaction_closure"] != "OK":
        raise RuntimeError(f"{spec.name}: transaction closure failed")
    if parsed["frame_total"] != parsed["actual_socket"]:
        raise RuntimeError(f"{spec.name}: frame total differs from socket total")
    if sum(frame["bytes"] for frame in parsed["frames"].values()) != parsed["frame_total"]:
        raise RuntimeError(f"{spec.name}: frame categories do not close")
    if sum(row["raw"] for row in curve_rows) != parsed["raw"]:
        raise RuntimeError(f"{spec.name}: curve raw does not close")
    if sum(row["wire"] for row in curve_rows) != parsed["actual_socket"]:
        raise RuntimeError(f"{spec.name}: curve wire does not close")
    if len(curve_rows) != parsed["tus"] or parsed["failures"] != spec.expected_failures:
        raise RuntimeError(f"{spec.name}: TU/failure count mismatch")
    if parsed["transaction_committed"] != parsed["tus"]:
        raise RuntimeError(f"{spec.name}: committed transaction count does not close")
    if parsed["prepared_accepted"] != (
        parsed["transaction_committed"] + parsed["transaction_aborted"]
    ):
        raise RuntimeError(f"{spec.name}: prepared transaction count does not close")
    if parsed["transaction_aborted"] != spec.expected_prepared_aborts:
        raise RuntimeError(f"{spec.name}: prepared-abort count mismatch")
    if parsed["decode_rejected"] != spec.expected_failures:
        raise RuntimeError(f"{spec.name}: decode-rejection count mismatch")
    if parsed["pipe_mode"] != "real":
        raise RuntimeError(f"{spec.name}: real compiler-pipe path was not used")
    if parsed["compiler_bytes"] != parsed["raw"]:
        raise RuntimeError(f"{spec.name}: compiler-pipe bytes do not close")
    if parsed["compiler_measured_bytes"] != parsed["raw"]:
        raise RuntimeError(f"{spec.name}: compiler consumer bytes do not close")
    if parsed["compiler_tus"] != parsed["tus"]:
        raise RuntimeError(f"{spec.name}: compiler-pipe TU count does not close")
    if parsed["summary_lost_workers"]:
        if (
            parsed["compiler_summary_bytes"] > parsed["raw"]
            or parsed["compiler_summary_tus"] > parsed["tus"]
        ):
            raise RuntimeError(f"{spec.name}: partial compiler summary exceeds Ack ledger")
    elif (
        parsed["compiler_summary_bytes"] != parsed["raw"]
        or parsed["compiler_summary_tus"] != parsed["tus"]
    ):
        raise RuntimeError(f"{spec.name}: complete compiler summary does not close")
    parsed["latency_p50_ms"] = latency_percentile_ms(curve_rows, 0.50)
    parsed["latency_p95_ms"] = latency_percentile_ms(curve_rows, 0.95)
    parsed["latency_p99_ms"] = latency_percentile_ms(curve_rows, 0.99)
    parsed["latency_max_ms"] = latency_percentile_ms(curve_rows, 1.00)
    parsed.update(asdict(spec))
    parsed["relationship_speed_pass"] = (
        spec.minimum_relationship_gbps is None
        or parsed["relationship_gbps"] >= spec.minimum_relationship_gbps
    )
    stage_rates = (
        parsed["c_source_pipe_gbps"],
        parsed["c_interning_gbps"],
        parsed["c_factorization_gbps"],
        parsed["c_transform_gbps"],
        parsed["f_decode_cpu_gbps"],
        parsed["f_pipe_write_gbps"],
    )
    parsed["stage_speed_pass"] = (
        spec.minimum_stage_gbps is None or min(stage_rates) >= spec.minimum_stage_gbps
    )
    parsed["complete_speed_pass"] = (
        spec.minimum_complete_gbps is None
        or parsed["complete_gbps"] >= spec.minimum_complete_gbps
    )
    parsed["speed_pass"] = (
        parsed["relationship_speed_pass"]
        and parsed["stage_speed_pass"]
        and parsed["complete_speed_pass"]
    )
    parsed["command"] = command
    parsed["curve"] = str(curve)
    parsed["curve_sha256"] = sha256(curve)
    parsed["log"] = str(log)
    parsed["log_sha256"] = sha256(log)
    parsed["corpus_sha256"] = corpus_record.split()[1]
    parsed["phases"] = phase_summary(curve_rows, spec.phase_names, spec.phase_lengths)
    return parsed


def write_outputs(
    output: Path,
    rows: list[dict[str, object]],
    builds: list[list[str]],
    fixture: dict[str, object],
) -> None:
    report = output / "m5-acceptance.json"
    report.write_text(
        json.dumps(
            {
                "schema": 1,
                "experiment": "protocol-50 M5 acceptance",
                "build_commands": builds,
                "fixture": fixture,
                "rows": rows,
            },
            indent=2,
            sort_keys=True,
        )
        + "\n"
    )
    table = output / "m5-acceptance.tsv"
    columns = (
        "name",
        "scope",
        "codec",
        "order",
        "assignment",
        "workers",
        "wave",
        "tus",
        "raw",
        "actual_socket",
        "ratio",
        "failures",
        "prepared_accepted",
        "decode_rejected",
        "transaction_committed",
        "transaction_aborted",
        "c50",
        "c50_tu",
        "h200",
        "h200_tu",
        "second_half",
        "latency_p50_ms",
        "latency_p95_ms",
        "latency_p99_ms",
        "latency_max_ms",
        "relationship_gbps",
        "c_source_pipe_gbps",
        "c_interning_gbps",
        "c_factorization_gbps",
        "c_transform_gbps",
        "f_decode_cpu_gbps",
        "c_peak_mib",
        "f_peak_mib",
        "pipe_mode",
        "c_pipe_to_wire_gbps",
        "f_wire_to_compiler_pipe_gbps",
        "f_aggregate_gbps",
        "f_pipe_write_gbps",
        "complete_gbps",
        "compiler_bytes",
        "compiler_tus",
        "compiler_measured_bytes",
        "compiler_summary_bytes",
        "compiler_summary_tus",
        "summary_lost_workers",
        "minimum_relationship_gbps",
        "minimum_stage_gbps",
        "minimum_complete_gbps",
        "relationship_speed_pass",
        "stage_speed_pass",
        "complete_speed_pass",
        "speed_pass",
        "one_pass",
        "region_removals",
        "public_removals",
        "block_removals",
        "compactions",
        "curve",
        "curve_sha256",
        "log",
        "log_sha256",
        "corpus_sha256",
        "wire_equivalent_to",
        "wire_equivalent_pass",
    )
    with table.open("w", newline="") as target:
        writer = csv.DictWriter(
            target, fieldnames=columns, delimiter="\t", extrasaction="ignore"
        )
        writer.writeheader()
        writer.writerows(rows)
    retained = [
        report,
        table,
        *sorted(path for path in (output / "fixtures").rglob("*") if path.is_file()),
        *sorted(path for path in output.glob("cap_*") if path.is_file()),
        *sorted((output / "logs").glob("*.log")),
        *sorted((output / "curves").glob("*.tsv")),
        *sorted(path for path in (output / "snapshots").glob("*") if path.is_file()),
    ]
    (output / "SHA256SUMS").write_text(
        "".join(
            f"{sha256(path)}  {path.relative_to(output)}\n"
            for path in retained
            if path.is_file()
        )
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--corpus-root", type=Path, default=Path("/tanksmall/scratch/ictmp")
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--suite", choices=("smoke", "rate", "full"), default="full"
    )
    parser.add_argument("--corpora", default=",".join(name for name, _ in CORPORA))
    parser.add_argument("--evolution-tus", type=int, default=24)
    parser.add_argument("--timeout", type=int, default=3600)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--resume", action="store_true")
    args = parser.parse_args()
    known = {name for name, _ in CORPORA}
    selected = {name for name in args.corpora.split(",") if name}
    if not selected or selected - known:
        parser.error(f"unknown corpus selection: {sorted(selected - known)}")
    if args.evolution_tus < 4:
        parser.error("--evolution-tus must be at least 4")
    args.output.mkdir(parents=True, exist_ok=True)
    for name in ("logs", "curves", "snapshots"):
        (args.output / name).mkdir(exist_ok=True)
    source = Path(__file__).resolve().parent
    binary, stream_binary, builds = build(source, args.output, args.cxx)
    if args.suite == "rate":
        specs, fixture = onepass_specs(args.corpus_root, selected), {}
    else:
        specs, fixture = smoke_specs(
            args.corpus_root, args.output, args.evolution_tus
        )
    if args.suite == "full":
        specs += full_specs(args.corpus_root, args.output, selected)
    rows: list[dict[str, object]] = []
    row_by_name: dict[str, dict[str, object]] = {}
    for ordinal, spec in enumerate(specs, 1):
        row = run_one(
            binary, stream_binary, args.output, spec, args.timeout, args.resume
        )
        row["wire_equivalent_pass"] = None
        if spec.wire_equivalent_to is not None:
            reference = row_by_name.get(spec.wire_equivalent_to)
            if reference is None:
                raise RuntimeError(
                    f"{spec.name}: missing wire reference {spec.wire_equivalent_to}"
                )
            if (
                row["actual_socket"] != reference["actual_socket"]
                or row["frames"] != reference["frames"]
                or row["components"] != reference["components"]
                or wire_curve_signature(Path(str(row["curve"])))
                != wire_curve_signature(Path(str(reference["curve"])))
            ):
                raise RuntimeError(
                    f"{spec.name}: physical wire differs from {spec.wire_equivalent_to}"
                )
            row["wire_equivalent_pass"] = True
        rows.append(row)
        row_by_name[spec.name] = row
        write_outputs(args.output, rows, builds, fixture)
        print(
            f"EXACT {ordinal}/{len(specs)} {spec.name}: {row['actual_socket']} B "
            f"{row['ratio']:.3f}x relationship={row['relationship_gbps']:.3f} "
            f"GB/s complete={row['complete_gbps']:.3f} GB/s",
            flush=True,
        )
    speed_failures = [row["name"] for row in rows if not row["speed_pass"]]
    if speed_failures:
        print(
            "M5 ACCEPTANCE FAIL: required complete/stage rate below floor for "
            + ", ".join(speed_failures)
        )
        return 1
    print(f"M5 ACCEPTANCE PASS: {len(rows)} rows; artifacts={args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
