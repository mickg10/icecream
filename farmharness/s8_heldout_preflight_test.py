from __future__ import annotations

import json
from pathlib import Path

import pytest

import s8_heldout_preflight as preflight


def _write(path: Path, value: str | bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(value.encode() if isinstance(value, str) else value)


def _fixture(root: Path) -> None:
    for corpus, config in preflight.CORPORA.items():
        source_root = root / config["source_root"]
        corpus_root = root / config["corpus_root"]
        source_relative = "tools/utils/test_platform.cpp" if corpus == "DuckDB" else "llvm/lib/Analysis/AliasAnalysis.cpp"
        source = source_root / source_relative
        _write(source, "int heldout_fixture() { return 0; }\n")
        representative = corpus_root / config["representative_name"]
        _write(representative, b"# 1 \"fixture\"\nint heldout_fixture();\n")
        _write(corpus_root / "manifest.txt", str(representative) + "\n")
        matrix_manifest = root / config["matrix_manifest"]
        _write(matrix_manifest, (str(representative) + "\n") * 4)
        metadata = {"corpus_id": "corpus3" if corpus == "DuckDB" else "corpus",
                    "project": "DuckDB" if corpus == "DuckDB" else "LLVM",
                    "git_commit": "a" * 9, "TU": 689 if corpus == "DuckDB" else 1238,
                    "source_checkouts": [str(source_root)]}
        _write(corpus_root / "METADATA.json", json.dumps(metadata) + "\n")
        compile_db = root / config["compile_db"]
        _write(compile_db, json.dumps([{
            "directory": str(source.parent), "file": str(source),
            "command": f"/usr/bin/c++ -std=c++17 -c {source} -o unit.o",
        }]) + "\n")


def test_preflight_binds_both_corpora_and_repeated_matrix_cells(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    _fixture(tmp_path)
    monkeypatch.setattr(preflight, "_git_identity",
                        lambda root: {"commit": "a" * 40, "tree": "b" * 40})
    value = preflight.preflight(tmp_path, Path(__file__).resolve().parents[1])
    assert value["status"] == "PASS"
    assert value["corpora"]["DuckDB"]["retained_manifest"]["entries"] == 1
    assert value["corpora"]["DuckDB"]["matrix_manifest"]["entries"] == 4
    assert value["corpora"]["DuckDB"]["matrix_manifest"]["repeat_factor"] == 4
    assert "ICECC_P50_C1F1_SOURCE_ROOT=" + value["corpora"]["DuckDB"]["corpus_root"] in value["corpora"]["DuckDB"]["runner_template"]
    assert value["corpora"]["LLVM-1238"]["compile_database"]["source"].endswith(
        "llvm/lib/Analysis/AliasAnalysis.cpp")
    assert value["runner"]["profiles"] == ["ZSTD_TU", "ZSTD_ROUTE", "P29", "GRZ_RESIDUAL"]
    assert value["runner"]["warm_values"] == [0, 1]


def test_preflight_rejects_matrix_with_wrong_repeat_factor(tmp_path: Path) -> None:
    root = tmp_path / "corpus"
    item = root / "one.ii"
    _write(item, b"fixture")
    manifest = root / "matrix.txt"
    _write(manifest, (str(item) + "\n") * 3)
    with pytest.raises(preflight.PreflightError, match="repeat_factor_invalid"):
        preflight._paths_from_manifest(manifest, root, "matrix", repeat_factor=4)


def test_cli_is_read_only_without_output_path(tmp_path: Path,
                                              monkeypatch: pytest.MonkeyPatch,
                                              capsys: pytest.CaptureFixture[str]) -> None:
    _fixture(tmp_path)
    monkeypatch.setattr(preflight, "_git_identity",
                        lambda root: {"commit": "a" * 40, "tree": "b" * 40})
    assert preflight.main(["--root", str(tmp_path), "--repo",
                           str(Path(__file__).resolve().parents[1])]) == 0
    output = json.loads(capsys.readouterr().out)
    assert output["schema"] == preflight.SCHEMA
    assert not list(tmp_path.glob("*.json"))
