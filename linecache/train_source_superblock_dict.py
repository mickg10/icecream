#!/usr/bin/env python3
"""Train charged zstd dictionaries on disjoint-project source superblocks.

The trainer reads only the supplied training manifests.  It extracts project source paths from
preprocessor markers, groups consecutive source lines into small immutable superblock samples, and
trains one zstd dictionary per requested package size.  The target manifest is deliberately not an
input to this program.
"""

from __future__ import annotations

import argparse
import json
import mmap
import os
from pathlib import Path
import random
import re
import time

import zstandard as zstd


MARKER_PATH = re.compile(br'(?m)^# [0-9]+ "([^"\n]+)"')


def project_source_paths(manifest: Path) -> tuple[list[Path], dict[str, int]]:
    paths: set[Path] = set()
    tus = 0
    markers = 0
    missing_tus = 0
    with manifest.open("r", encoding="utf-8") as stream:
        for line in stream:
            name = line.rstrip("\r\n")
            if not name:
                continue
            tu_path = Path(name)
            tus += 1
            try:
                with tu_path.open("rb") as source:
                    if os.fstat(source.fileno()).st_size == 0:
                        continue
                    with mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as mapped:
                        for match in MARKER_PATH.finditer(mapped):
                            markers += 1
                            raw = match.group(1)
                            if raw.startswith((b"/usr", b"/lib", b"<")):
                                continue
                            paths.add(Path(os.fsdecode(raw)))
            except OSError:
                missing_tus += 1
    existing = sorted(path for path in paths if path.is_file())
    return existing, {
        "tus": tus,
        "markers": markers,
        "project_paths": len(paths),
        "existing_project_paths": len(existing),
        "missing_tus": missing_tus,
    }


def source_superblocks(paths: list[Path], byte_budget: int, lines_per_block: int,
                       max_block_bytes: int, seed: int) -> tuple[list[bytes], dict[str, int]]:
    ordered = list(paths)
    random.Random(seed).shuffle(ordered)
    samples: list[bytes] = []
    sampled_bytes = 0
    files_read = 0
    lines_read = 0
    for path in ordered:
        if sampled_bytes >= byte_budget:
            break
        try:
            lines = path.read_bytes().splitlines(keepends=True)
        except OSError:
            continue
        files_read += 1
        lines_read += len(lines)
        block = bytearray()
        block_lines = 0
        for line in lines:
            if block and (block_lines >= lines_per_block or len(block) + len(line) > max_block_bytes):
                sample = bytes(block)
                if len(sample) >= 8:
                    samples.append(sample)
                    sampled_bytes += len(sample)
                block.clear()
                block_lines = 0
                if sampled_bytes >= byte_budget:
                    break
            block.extend(line)
            block_lines += 1
        if sampled_bytes >= byte_budget:
            break
        if len(block) >= 8:
            sample = bytes(block)
            samples.append(sample)
            sampled_bytes += len(sample)
    return samples, {
        "files_read": files_read,
        "lines_read": lines_read,
        "samples": len(samples),
        "sample_bytes": sampled_bytes,
    }


def run(args: argparse.Namespace) -> int:
    started = time.perf_counter()
    all_samples: list[bytes] = []
    corpora: list[dict[str, object]] = []
    per_corpus_budget = args.sample_mib_per_corpus * 1024 * 1024
    for index, manifest_name in enumerate(args.train_manifest):
        manifest = Path(manifest_name)
        paths, path_stats = project_source_paths(manifest)
        samples, sample_stats = source_superblocks(
            paths, per_corpus_budget, args.lines_per_block, args.max_block_bytes,
            args.seed + index,
        )
        all_samples.extend(samples)
        corpora.append({"manifest": str(manifest), **path_stats, **sample_stats})
    if not all_samples:
        raise SystemExit("no source-superblock samples found")

    outputs: list[dict[str, object]] = []
    prefix = Path(args.output_prefix)
    prefix.parent.mkdir(parents=True, exist_ok=True)
    for kib in args.dict_kib:
        trained = zstd.train_dictionary(kib * 1024, all_samples).as_bytes()
        output = Path(f"{prefix}-{kib}k.dict")
        output.write_bytes(trained)
        outputs.append({"requested_kib": kib, "bytes": len(trained), "path": str(output)})

    report = {
        "experiment": "disjoint_project_source_superblock_zstd_dictionary",
        "training_manifests": [str(Path(name)) for name in args.train_manifest],
        "target_manifest_read": False,
        "lines_per_block": args.lines_per_block,
        "max_block_bytes": args.max_block_bytes,
        "sample_mib_per_corpus": args.sample_mib_per_corpus,
        "seed": args.seed,
        "corpora": corpora,
        "total_samples": len(all_samples),
        "total_sample_bytes": sum(map(len, all_samples)),
        "dictionaries": outputs,
        "seconds": time.perf_counter() - started,
        "zstandard_version": zstd.__version__,
    }
    report_path = Path(f"{prefix}-report.json")
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    result.add_argument("--train-manifest", action="append", required=True)
    result.add_argument("--dict-kib", nargs="+", type=int, default=(32, 64, 128, 256))
    result.add_argument("--sample-mib-per-corpus", type=int, default=64)
    result.add_argument("--lines-per-block", type=int, default=32)
    result.add_argument("--max-block-bytes", type=int, default=8192)
    result.add_argument("--seed", type=int, default=0x1CE50)
    result.add_argument("--output-prefix", required=True)
    return result


if __name__ == "__main__":
    raise SystemExit(run(parser().parse_args()))
