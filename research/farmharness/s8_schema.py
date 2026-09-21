"""Shared immutable S8 cell and split contract."""

from __future__ import annotations


CORPORA = ("fmt", "RocksDB", "DuckDB", "LLVM-1238")
# The four values above are the immutable S8 accuracy contract.  These
# additional project labels are deliberately a separate benchmarking scope;
# they must never be added to CORPORA or DECLARED_CELLS.
EXPANDED_ONLY_CORPORA = (
    "abseil+protobuf", "OpenCV", "Godot", "spdlog", "Catch2",
    "nlohmann-json", "range-v3",
)
ALL_CORPORA = CORPORA + EXPANDED_ONLY_CORPORA
PROFILES = ("ZSTD_TU", "ZSTD_ROUTE", "P29", "GRZ_RESIDUAL")
# RAW_II is an explicit whole-legacy control arm.  It is intentionally kept
# outside PROFILES: the latter is the four-profile compressed calibration
# contract consumed by the predictive engine and the 32-cell audit.
CONTROL_PROFILES = ("RAW_II",)
REGIMES = ("cold", "warm")
TOPOLOGIES = ("C1F1", "C1F20")
DEPTH_CLASSES = ("100", "200", "full", "repeat-full", "legacy")
CALIBRATION_CORPORA = frozenset(("fmt", "RocksDB"))
HELD_OUT_CORPORA = frozenset(("DuckDB", "LLVM-1238"))
SPLITS = {
    **{corpus: "calibration" for corpus in CALIBRATION_CORPORA},
    **{corpus: "held_out_validation" for corpus in HELD_OUT_CORPORA},
}
# Expanded corpora have no calibration or held-out accuracy semantics.  This
# descriptive label is intentionally absent from SPLITS so canonical loss,
# accuracy, and the 32-cell audit cannot consume an expanded corpus.
EXPANDED_DESCRIPTIVE_SPLIT = "expanded_descriptive"
ALL_SPLITS = {
    **SPLITS,
    **{corpus: EXPANDED_DESCRIPTIVE_SPLIT for corpus in EXPANDED_ONLY_CORPORA},
}
DECLARED_CELLS = tuple(
    {"corpus": corpus, "profile": profile, "regime": regime}
    for corpus in CORPORA
    for profile in PROFILES
    for regime in REGIMES
)
CURRENT_SEMANTICS = "s8-current-semantics-v1"
