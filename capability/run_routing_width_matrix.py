#!/usr/bin/env python3
"""Run direction-exact cold-width and mixed-warmth routing matrices.

The score is the sum of physical C-to-F writes.  Duplex and F-to-C values are
retained for timing diagnosis but never substituted for the binding score.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import shlex
import subprocess
from dataclasses import dataclass
from pathlib import Path

from run_m5_acceptance import parse_log, read_curve, sha256


@dataclass(frozen=True)
class Corpus:
    name: str
    directory: str


CORPORA = (
    Corpus("llvm", "corpus"),
    Corpus("rocksdb", "corpus2"),
    Corpus("duckdb", "corpus3"),
    Corpus("godot", "corpus6"),
    Corpus("catch2", "corpus9"),
    Corpus("range-v3", "corpus11"),
    Corpus("eigen", "corpus12"),
    Corpus("cereal", "corpus16"),
)

CATEGORY_FIELDS = ("c_root", "c_fill", "c_control", "f_need", "f_control")


def manifest_paths(path: Path) -> list[Path]:
    return [Path(line) for line in path.read_text().splitlines() if line]


def manifest_fingerprint(path: Path) -> tuple[str, int, int]:
    """Hash manifest identity plus path/size/mtime without rereading all .ii bytes."""
    digest = hashlib.sha256(b"routing-width-manifest-v1\0")
    paths = manifest_paths(path)
    raw = 0
    for source in paths:
        encoded = str(source).encode()
        status = source.stat()
        raw += status.st_size
        digest.update(len(encoded).to_bytes(8, "little"))
        digest.update(encoded)
        digest.update(status.st_size.to_bytes(8, "little"))
        digest.update(status.st_mtime_ns.to_bytes(8, "little"))
    return digest.hexdigest(), len(paths), raw


def build_binary(source: Path, output: Path, cxx: str) -> tuple[Path, list[str]]:
    binary = output / "cap_m5"
    command = [
        cxx,
        "-O3",
        "-DNDEBUG",
        "-march=native",
        os.environ.get("ICE_CXX_STANDARD_FLAG", "-std=c++23"),
        "-DICE_LINE_CAP_LOG2=23",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-Werror",
        str(source / "cap_m5_main.cpp"),
        str(source / "cap_codec.cpp"),
        "-o",
        str(binary),
        "-lzstd",
        "-pthread",
    ]
    subprocess.run(command, check=True)
    (output / "build-command.txt").write_text(shlex.join(command) + "\n")
    return binary, command


def sum_rows(rows: list[dict[str, int]], field: str) -> int:
    return sum(row[field] for row in rows)


def validate_run(name: str, parsed: dict[str, object], rows: list[dict[str, int]]) -> None:
    if parsed["exact"] != "OK":
        raise RuntimeError(f"{name}: exact reconstruction failed")
    for field in ("frame_closure", "direction_closure", "routing_closure"):
        if parsed[field] != "OK":
            raise RuntimeError(f"{name}: {field} failed")
    if len(rows) != parsed["tus"]:
        raise RuntimeError(f"{name}: TU count does not close")
    if sum_rows(rows, "raw") != parsed["raw"]:
        raise RuntimeError(f"{name}: raw curve does not close")
    if sum_rows(rows, "wire") != parsed["actual_socket"]:
        raise RuntimeError(f"{name}: duplex curve does not close")
    if sum_rows(rows, "c_to_f") != parsed["c_to_f"]:
        raise RuntimeError(f"{name}: C-to-F curve does not close")
    if sum_rows(rows, "f_to_c") != parsed["f_to_c"]:
        raise RuntimeError(f"{name}: F-to-C curve does not close")
    for field in CATEGORY_FIELDS:
        if sum_rows(rows, field) != parsed[field]:
            raise RuntimeError(f"{name}: {field} curve does not close")
    if any(row["direction_ok"] != 1 or row["category_ok"] != 1 for row in rows):
        raise RuntimeError(f"{name}: a per-TU direction/category row failed")
    if parsed["c_root"] != parsed["c_to_f_frames"]["Root"]["bytes"]:
        raise RuntimeError(f"{name}: Root differs from physical frame ledger")
    if parsed["c_fill"] != parsed["c_to_f_frames"]["Fill"]["bytes"]:
        raise RuntimeError(f"{name}: Fill differs from physical frame ledger")
    if parsed["f_need"] != parsed["f_to_c_frames"]["Need"]["bytes"]:
        raise RuntimeError(f"{name}: Need differs from physical frame ledger")


def run_one(
    binary: Path,
    output: Path,
    name: str,
    manifest: Path,
    width: int,
    codec: str,
    assignment: str,
    timeout: int,
    resume: bool,
    repetitions: int = 1,
    latejoin_at: int | None = None,
) -> tuple[dict[str, object], list[dict[str, int]]]:
    log = output / "logs" / f"{name}.log"
    curve = output / "curves" / f"{name}.tsv"
    fingerprint, manifest_tus, manifest_raw = manifest_fingerprint(manifest)
    command = [
        str(binary),
        "--manifest",
        str(manifest),
        "--workers",
        str(width),
        "--wave",
        str(width),
        "--assignment",
        assignment,
        "--codec",
        codec,
        "--real-pipes",
        "--curve-out",
        str(curve),
    ]
    if repetitions != 1:
        command += ["--repetitions", str(repetitions)]
    if latejoin_at is not None:
        command += ["--latejoin-at", str(latejoin_at)]
    header = (
        f"COMMAND {shlex.join(command)}\n"
        f"BINARY_SHA256 {sha256(binary)}\n"
        f"MANIFEST_FINGERPRINT {fingerprint}\n"
    )
    reusable = resume and log.is_file() and curve.is_file()
    if reusable:
        reusable = log.read_text(errors="replace").startswith(header)
    if not reusable:
        completed = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout,
        )
        log.write_text(header + f"EXIT {completed.returncode}\n" + completed.stdout)
        if completed.returncode:
            raise RuntimeError(f"{name}: exited {completed.returncode}; see {log}")
    parsed = parse_log(log.read_text(errors="replace"))
    rows = read_curve(curve)
    validate_run(name, parsed, rows)
    if parsed["tus"] != manifest_tus * repetitions:
        raise RuntimeError(f"{name}: repetition/TU count mismatch")
    if parsed["raw"] != manifest_raw * repetitions:
        raise RuntimeError(f"{name}: manifest raw count mismatch")
    parsed["manifest_fingerprint"] = fingerprint
    parsed["manifest_tus"] = manifest_tus
    parsed["manifest_raw"] = manifest_raw
    parsed["log"] = str(log)
    parsed["log_sha256"] = sha256(log)
    parsed["curve"] = str(curve)
    parsed["curve_sha256"] = sha256(curve)
    return parsed, rows


def cold_row(corpus: Corpus, width: int, parsed: dict[str, object]) -> dict[str, object]:
    c_to_f = int(parsed["c_to_f"])
    return {
        "corpus": corpus.name,
        "tus": parsed["tus"],
        "raw": parsed["raw"],
        "width": width,
        "actual_socket": parsed["actual_socket"],
        "c_to_f": c_to_f,
        "f_to_c": parsed["f_to_c"],
        **{field: parsed[field] for field in CATEGORY_FIELDS},
        "c_to_f_ratio": float(parsed["raw"]) / c_to_f if c_to_f else None,
        "fill_share": int(parsed["c_fill"]) / c_to_f if c_to_f else None,
        "nominal_1gbit_ms": c_to_f * 8 / 1e6,
        "nominal_10gbit_ms": c_to_f * 8 / 1e7,
        "wall_seconds": parsed["wall_seconds"],
        "log": parsed["log"],
        "log_sha256": parsed["log_sha256"],
        "curve": parsed["curve"],
        "curve_sha256": parsed["curve_sha256"],
        "manifest_fingerprint": parsed["manifest_fingerprint"],
    }


def mixed_row(
    corpus: Corpus,
    width: int,
    manifest_tus: int,
    parsed: dict[str, object],
    rows: list[dict[str, int]],
) -> dict[str, object]:
    pass1 = rows[:manifest_tus]
    pass2 = rows[manifest_tus:]
    warm = [row for row in pass2 if row["worker"] == 0]
    cold = [row for row in pass2 if row["worker"] != 0]
    if len(pass1) != manifest_tus or len(pass2) != manifest_tus:
        raise RuntimeError(f"{corpus.name}-mixed-{width}: pass split failed")
    result: dict[str, object] = {
        "corpus": corpus.name,
        "tus_per_pass": manifest_tus,
        "width": width,
        "warm_daemons": 1,
        "cold_daemons": width - 1,
        "pass1_c_to_f": sum_rows(pass1, "c_to_f"),
        "pass2_wire": sum_rows(pass2, "wire"),
        "pass2_c_to_f": sum_rows(pass2, "c_to_f"),
        "pass2_f_to_c": sum_rows(pass2, "f_to_c"),
        "p2_warm_tus": len(warm),
        "p2_cold_tus": len(cold),
        "p2_warm_c_to_f": sum_rows(warm, "c_to_f"),
        "p2_cold_c_to_f": sum_rows(cold, "c_to_f"),
        "p2_warm_root": sum_rows(warm, "c_root"),
        "p2_warm_fill": sum_rows(warm, "c_fill"),
        "p2_warm_control": sum_rows(warm, "c_control"),
        "p2_cold_root": sum_rows(cold, "c_root"),
        "p2_cold_fill": sum_rows(cold, "c_fill"),
        "p2_cold_control": sum_rows(cold, "c_control"),
        "log": parsed["log"],
        "log_sha256": parsed["log_sha256"],
        "curve": parsed["curve"],
        "curve_sha256": parsed["curve_sha256"],
        "manifest_fingerprint": parsed["manifest_fingerprint"],
    }
    warm_tus = int(result["p2_warm_tus"])
    cold_tus = int(result["p2_cold_tus"])
    result["p2_warm_c_to_f_per_tu"] = (
        int(result["p2_warm_c_to_f"]) / warm_tus if warm_tus else None
    )
    result["p2_cold_c_to_f_per_tu"] = (
        int(result["p2_cold_c_to_f"]) / cold_tus if cold_tus else None
    )
    return result


def write_tsv(path: Path, rows: list[dict[str, object]], fields: list[str]) -> None:
    with path.open("w", newline="") as target:
        writer = csv.DictWriter(target, fieldnames=fields, delimiter="\t")
        writer.writeheader()
        writer.writerows(rows)


def add_relative_metrics(rows: list[dict[str, object]], metric: str) -> None:
    by_corpus: dict[str, dict[int, dict[str, object]]] = {}
    for row in rows:
        by_corpus.setdefault(str(row["corpus"]), {})[int(row["width"])] = row
    for corpus_rows in by_corpus.values():
        base = int(corpus_rows[1][metric])
        for row in corpus_rows.values():
            value = int(row[metric])
            row[f"{metric}_vs_width1"] = value / base
            row[f"avoidable_vs_width1"] = (value - base) / value if value else None


def write_report(
    path: Path,
    cold: list[dict[str, object]],
    mixed: list[dict[str, object]],
    widths: list[int],
) -> None:
    by_cold = {(str(row["corpus"]), int(row["width"])): row for row in cold}
    present = {str(row["corpus"]) for row in cold + mixed}
    corpus_order = [corpus for corpus in CORPORA if corpus.name in present]
    lines = [
        "# Direction-exact routing width matrix",
        "",
        "The binding score is physical C-to-F bytes. Return bytes are retained separately.",
    ]
    if cold:
        lines += [
            "",
            "## Cold width amplification",
            "",
            "| corpus | " + " | ".join(f"M={width}" for width in widths) + " |",
            "|---|" + "---:|" * len(widths),
        ]
        for corpus in corpus_order:
            cells = []
            for width in widths:
                row = by_cold[(corpus.name, width)]
                cells.append(
                    f"{int(row['c_to_f']) / 1e6:.3f} MB "
                    f"({float(row['c_to_f_vs_width1']):.2f}x)"
                )
            lines.append(f"| {corpus.name} | " + " | ".join(cells) + " |")
    if mixed:
        by_mixed = {
            (str(row["corpus"]), int(row["width"])): row for row in mixed
        }
        lines += [
            "",
            "## One warm F plus recruited cold Fs: pass-two C-to-F",
            "",
            "| corpus | " + " | ".join(f"M={width}" for width in widths) + " |",
            "|---|" + "---:|" * len(widths),
        ]
        for corpus in corpus_order:
            cells = []
            for width in widths:
                row = by_mixed[(corpus.name, width)]
                cells.append(
                    f"{int(row['pass2_c_to_f']) / 1e6:.3f} MB "
                    f"({float(row['pass2_c_to_f_vs_width1']):.2f}x)"
                )
            lines.append(f"| {corpus.name} | " + " | ".join(cells) + " |")
    path.write_text("\n".join(lines) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--corpus-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--widths", default="1,2,4,8,16,30")
    parser.add_argument("--corpora", default=",".join(c.name for c in CORPORA))
    parser.add_argument("--mode", choices=("cold", "mixed", "both"), default="both")
    parser.add_argument("--codec", choices=("z1", "z3"), default="z3")
    parser.add_argument(
        "--assignment", choices=("roundrobin", "sticky", "random"), default="roundrobin"
    )
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--timeout", type=int, default=7200)
    parser.add_argument("--resume", action="store_true")
    args = parser.parse_args()

    widths = [int(value) for value in args.widths.split(",") if value]
    if not widths or widths != sorted(set(widths)) or widths[0] != 1:
        parser.error("widths must be a unique sorted list beginning with 1")
    if any(width < 1 or width > 32 for width in widths):
        parser.error("widths must lie in [1,32]")
    selected_names = {value for value in args.corpora.split(",") if value}
    known_names = {corpus.name for corpus in CORPORA}
    if not selected_names or selected_names - known_names:
        parser.error(f"unknown corpora: {sorted(selected_names - known_names)}")
    selected = [corpus for corpus in CORPORA if corpus.name in selected_names]

    args.output.mkdir(parents=True, exist_ok=True)
    for name in ("logs", "curves"):
        (args.output / name).mkdir(exist_ok=True)
    source = Path(__file__).resolve().parent
    binary, build_command = build_binary(source, args.output, args.cxx)

    cold: list[dict[str, object]] = []
    mixed: list[dict[str, object]] = []
    for corpus in selected:
        manifest = args.corpus_root / corpus.directory / "manifest.txt"
        _, manifest_tus, _ = manifest_fingerprint(manifest)
        for width in widths:
            if args.mode in ("cold", "both"):
                name = f"cold-{corpus.name}-m{width}"
                parsed, _ = run_one(
                    binary,
                    args.output,
                    name,
                    manifest,
                    width,
                    args.codec,
                    args.assignment,
                    args.timeout,
                    args.resume,
                )
                row = cold_row(corpus, width, parsed)
                cold.append(row)
                print(
                    f"EXACT {name}: C->F={row['c_to_f']} "
                    f"Root={row['c_root']} Fill={row['c_fill']}",
                    flush=True,
                )
            if args.mode in ("mixed", "both"):
                name = f"mixed-{corpus.name}-m{width}"
                parsed, rows = run_one(
                    binary,
                    args.output,
                    name,
                    manifest,
                    width,
                    args.codec,
                    args.assignment,
                    args.timeout,
                    args.resume,
                    repetitions=2,
                    latejoin_at=manifest_tus,
                )
                row = mixed_row(corpus, width, manifest_tus, parsed, rows)
                mixed.append(row)
                print(
                    f"EXACT {name}: pass2 C->F={row['pass2_c_to_f']} "
                    f"warm/cold={row['p2_warm_c_to_f']}/{row['p2_cold_c_to_f']}",
                    flush=True,
                )

    if cold:
        add_relative_metrics(cold, "c_to_f")
        cold_fields = list(cold[0])
        write_tsv(args.output / "cold-width.tsv", cold, cold_fields)
    if mixed:
        add_relative_metrics(mixed, "pass2_c_to_f")
        mixed_fields = list(mixed[0])
        write_tsv(args.output / "mixed-warmth.tsv", mixed, mixed_fields)

    capacity_rows: list[dict[str, object]] = []
    if cold:
        by_cold = {(str(row["corpus"]), int(row["width"])): row for row in cold}
        for corpus in selected:
            for slots_per_f in (1, 4, 8, 30):
                required_width = math.ceil(30 / slots_per_f)
                if required_width not in widths:
                    continue
                row = by_cold[(corpus.name, required_width)]
                capacity_rows.append(
                    {
                        "corpus": corpus.name,
                        "slots_required": 30,
                        "slots_per_f": slots_per_f,
                        "m_min": required_width,
                        "c_to_f": row["c_to_f"],
                        "c_root": row["c_root"],
                        "c_fill": row["c_fill"],
                        "c_control": row["c_control"],
                        "nominal_1gbit_ms": row["nominal_1gbit_ms"],
                        "nominal_10gbit_ms": row["nominal_10gbit_ms"],
                    }
                )
        write_tsv(args.output / "capacity-width.tsv", capacity_rows, list(capacity_rows[0]))

    write_report(args.output / "REPORT.md", cold, mixed, widths)
    provenance = {
        "schema": 1,
        "build_command": build_command,
        "binary_sha256": sha256(binary),
        "codec": args.codec,
        "assignment": args.assignment,
        "widths": widths,
        "corpora": [corpus.name for corpus in selected],
        "cold_rows": cold,
        "mixed_rows": mixed,
        "capacity_rows": capacity_rows,
    }
    (args.output / "matrix.json").write_text(
        json.dumps(provenance, indent=2, sort_keys=True) + "\n"
    )
    retained = sorted(path for path in args.output.rglob("*") if path.is_file())
    (args.output / "SHA256SUMS").write_text(
        "".join(
            f"{sha256(path)}  {path.relative_to(args.output)}\n"
            for path in retained
            if path.name != "SHA256SUMS"
        )
    )
    print(
        f"ROUTING WIDTH MATRIX PASS: cold={len(cold)} mixed={len(mixed)} "
        f"artifacts={args.output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
