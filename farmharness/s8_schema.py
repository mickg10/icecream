"""Shared immutable S8 cell and split contract."""

from __future__ import annotations


CORPORA = ("fmt", "RocksDB", "DuckDB", "LLVM-1238")
PROFILES = ("ZSTD_TU", "ZSTD_ROUTE", "P29", "GRZ_RESIDUAL")
REGIMES = ("cold", "warm")
CALIBRATION_CORPORA = frozenset(("fmt", "RocksDB"))
HELD_OUT_CORPORA = frozenset(("DuckDB", "LLVM-1238"))
SPLITS = {
    **{corpus: "calibration" for corpus in CALIBRATION_CORPORA},
    **{corpus: "held_out_validation" for corpus in HELD_OUT_CORPORA},
}
DECLARED_CELLS = tuple(
    {"corpus": corpus, "profile": profile, "regime": regime}
    for corpus in CORPORA
    for profile in PROFILES
    for regime in REGIMES
)
CURRENT_SEMANTICS = "s8-current-semantics-v1"
