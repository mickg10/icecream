"""Focused immutable-input gates for the S8 salvage producer."""

from __future__ import annotations

import hashlib
from pathlib import Path

import pytest

from s8_live_salvage import SalvageError, _load_timing, _product_log


RAW = Path(
    "/tanksmall/scratch/ictmp/experiments/icecream/"
    "s8-rocksdb-zstd-tu-cold-full-6c72c2da-20260830T125215Z/"
    "live-output-full2/C1F1/icecream/C1F1-100000/20260830T125215Z/ZSTD_TU"
)


def _copy_evidence(tmp_path: Path, *names: str) -> Path:
    root = tmp_path / "raw"
    evidence = root / "product-evidence"
    evidence.mkdir(parents=True)
    for name in names:
        source = RAW / name if name.startswith("timing_") else (RAW / name if "/" in name else RAW / "product-evidence" / name)
        target = root / Path(name).name if name.startswith("timing_") else evidence / Path(name).name
        target.write_bytes(source.read_bytes())
    return root


@pytest.mark.skipif(not RAW.is_dir(), reason="terminal raw package is not mounted")
def test_deletion_of_timing_is_fail_closed(tmp_path: Path) -> None:
    root = _copy_evidence(tmp_path, "timing_full-2.jsonl")
    (root / "timing_full-2.jsonl").unlink()
    with pytest.raises(SalvageError, match="unavailable"):
        _load_timing(root, "full-2")


@pytest.mark.skipif(not RAW.is_dir(), reason="terminal raw package is not mounted")
def test_mutated_product_log_is_rejected_and_source_hash_is_unchanged(tmp_path: Path) -> None:
    source = RAW / "product-evidence" / "product-output.log"
    before = hashlib.sha256(source.read_bytes()).hexdigest()
    root = _copy_evidence(tmp_path, "product-output.log")
    changed = bytearray((root / "product-evidence" / "product-output.log").read_bytes())
    marker = changed.index(b"S8_BATCH_TU")
    changed[marker] = ord("X")
    (root / "product-evidence" / "product-output.log").write_bytes(changed)
    with pytest.raises(SalvageError, match="product_log:"):
        _product_log(root)
    assert hashlib.sha256(source.read_bytes()).hexdigest() == before
