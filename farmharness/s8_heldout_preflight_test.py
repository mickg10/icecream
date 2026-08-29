from __future__ import annotations

import json
import shlex
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


def _product_build(root: Path) -> Path:
    build = root / "product-build"
    for relative in preflight.PRODUCT_BINARIES:
        path = build / relative
        _write(path, (str(relative) + "\n").encode())
        path.chmod(0o755)
    _write(build / preflight.PRODUCT_CONFIG,
           "#define ICECC_P50_WITH_LIBBSC 1\n")
    _write(build / preflight.PRODUCT_CACHE_MAKEFILE,
           "LIBBSC_CFLAGS = -DICECC_P50_WITH_LIBBSC\n"
           "LIBBSC_LIBS = /opt/libbsc.a\n")
    return build


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
    assert Path(value["corpora"]["DuckDB"]["compile_database"]["source"]).is_absolute()


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


def test_product_build_binding_is_authenticated_and_renders_all_cells(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    _fixture(tmp_path)
    build = _product_build(tmp_path)
    monkeypatch.setattr(preflight, "_git_identity",
                        lambda root: {"commit": "a" * 40, "tree": "b" * 40})
    repo = Path(__file__).resolve().parents[1]
    value = preflight.preflight(tmp_path, repo, build)
    repeat = preflight.preflight(tmp_path, repo, build)
    assert value == repeat
    binding = value["runner"]["product_build"]
    assert binding["status"] == "READY"
    assert set(binding["required_binaries"]) == {
        str(path) for path in preflight.PRODUCT_BINARIES
    }
    assert binding["git"] == {"commit": "a" * 40, "tree": "b" * 40}
    commands = binding["ready_cell_commands"]
    assert len(commands) == 16
    assert len({tuple(item["cell"].values()) for item in commands}) == 16
    for item in commands:
        assert item["command"] == shlex.join(item["argv"])
        assert "$" not in item["command"]
        assert item["argv"][-1] == str(repo / preflight.RUNNER)
        compile_source = value["corpora"][item["cell"]["corpus"]][
            "compile_database"]["source"]
        compile_arg = f"ICECC_P50_C1F1_COMPILE_SOURCE={compile_source}"
        assert compile_arg in item["argv"]
        assert Path(compile_source).is_absolute()


def test_product_build_binding_rejects_missing_binary_and_bad_grz_config(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    _fixture(tmp_path)
    build = _product_build(tmp_path)
    monkeypatch.setattr(preflight, "_git_identity",
                        lambda root: {"commit": "a" * 40, "tree": "b" * 40})
    (build / preflight.PRODUCT_BINARIES[0]).unlink()
    with pytest.raises(preflight.PreflightError, match="product_build.binary:.*unavailable"):
        preflight.preflight(tmp_path, Path(__file__).resolve().parents[1], build)

    bad_root = tmp_path / "bad-grz"
    _fixture(bad_root)
    build = _product_build(bad_root)
    (build / preflight.PRODUCT_CONFIG).write_text("/* no GRZ */\n", encoding="utf-8")
    with pytest.raises(preflight.PreflightError, match="libbsc_define_missing"):
        preflight.preflight(bad_root, Path(__file__).resolve().parents[1], build)

    bad_make_root = tmp_path / "bad-make"
    _fixture(bad_make_root)
    build = _product_build(bad_make_root)
    (build / preflight.PRODUCT_CACHE_MAKEFILE).write_text(
        "LIBBSC_CFLAGS =\nLIBBSC_LIBS =\n", encoding="utf-8")
    with pytest.raises(preflight.PreflightError, match="libbsc_make_inputs_invalid"):
        preflight.preflight(bad_make_root, Path(__file__).resolve().parents[1], build)


def test_product_build_binding_rejects_incompatible_runner(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    _fixture(tmp_path)
    build = _product_build(tmp_path)
    monkeypatch.setattr(preflight, "_git_identity",
                        lambda root: {"commit": "a" * 40, "tree": "b" * 40})
    runner = {
        "path": str(Path(__file__).resolve().parents[1] / preflight.RUNNER),
        "sha256": "c" * 64,
        "source_contract": {"sha256": "d" * 64},
        "profiles": ["ZSTD_TU"], "warm_values": [0, 1],
        "contract_check": "PASS",
    }
    monkeypatch.setattr(preflight, "_runner_contract", lambda repo: runner)
    with pytest.raises(preflight.PreflightError,
                       match="product_build.runner_compatibility_invalid"):
        preflight.preflight(tmp_path, Path(__file__).resolve().parents[1], build)
