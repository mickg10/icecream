from __future__ import annotations

import json
import sys
from pathlib import Path, PurePosixPath

import pytest

from farmharness.integration.firefox_corpus_authority import (
    FirefoxCorpusAuthorityError,
    MUTATION_AFTER,
    MUTATION_BEFORE,
    build_firefox_authority,
    select_compile_valid,
    load_compile_passes,
    load_corrected_trace,
)


def _write_trace(path: Path, relatives: list[str]) -> None:
    path.write_text(
        "logical\tii_relative\n"
        + "".join(f"{index}\t{relative}\n" for index, relative in enumerate(relatives)),
        encoding="utf-8",
    )


def _write_audit(path: Path, sources: list[Path], passes: list[bool]) -> None:
    path.write_text(
        json.dumps(
            [
                {
                    "exit_code": 0 if passed else 1,
                    "output_sha256": "a" * 64 if passed else None,
                    "path": str(source),
                    "timed_out": False,
                }
                for source, passed in zip(sources, passes, strict=True)
            ]
        ),
        encoding="utf-8",
    )


def test_selection_is_first_compile_valid_corrected_trace_order(tmp_path: Path) -> None:
    root = tmp_path / "corpus"
    relatives = [f"source/gecko/{name}.ii" for name in ("a", "b", "c")]
    sources = [root / relative for relative in relatives]
    for source in sources:
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_text("int x;\n", encoding="utf-8")
    trace = tmp_path / "trace.tsv"
    audit = tmp_path / "rows.json"
    _write_trace(trace, relatives)
    _write_audit(audit, sources, [True, False, True])

    entries = load_corrected_trace(trace, root)
    selected = select_compile_valid(entries, load_compile_passes((audit,)), 2)

    assert [entry.logical for entry in selected] == [0, 2]


def test_authority_generates_exact_mutation_and_validates_both_turns(
    tmp_path: Path,
) -> None:
    root = tmp_path / "corpus"
    marker = PurePosixPath("/source/gecko")
    relatives = ["source/gecko/a.ii", "source/gecko/b.ii", "source/gecko/c.ii"]
    sources = [root / relative for relative in relatives]
    payloads = [
        b'void f(){mozalloc_abort("alloc overflow");}\n',
        b"int untouched;\n",
        b'void g(){mozalloc_abort("alloc overflow");}\n',
    ]
    for source, payload in zip(sources, payloads, strict=True):
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_bytes(payload)
    trace = tmp_path / "trace.tsv"
    audit = tmp_path / "rows.json"
    _write_trace(trace, relatives)
    _write_audit(audit, sources, [True, True, True])
    compiler = tmp_path / "compiler"
    compiler.write_text(
        f"#!{sys.executable}\n"
        "import pathlib,sys\n"
        "args=sys.argv[1:]\n"
        "source=pathlib.Path(args[args.index('-c')+1])\n"
        "target=pathlib.Path(args[args.index('-o')+1])\n"
        "target.write_bytes(source.read_bytes())\n",
        encoding="utf-8",
    )
    compiler.chmod(0o755)

    result = build_firefox_authority(
        trace_path=trace,
        corpus_root=root,
        audit_rows=(audit,),
        output_root=tmp_path / "authority",
        authority_marker=marker,
        compiler=compiler,
        compiler_arguments=("-O2",),
        count=2,
        jobs=2,
        timeout_s=10,
    )

    assert result["mutation"] == {
        "affected": 1,
        "after": 'mozalloc_abort("Xalloc overflow")',
        "before": 'mozalloc_abort("alloc overflow")',
        "unaffected": 1,
    }
    assert result["compile_validation"]["turn_a_pass"] == 2
    assert result["compile_validation"]["turn_b_pass"] == 2
    b_paths = Path(result["turn_b_manifest"]["path"]).read_text().splitlines()
    assert Path(b_paths[0]).read_bytes() == payloads[0].replace(
        MUTATION_BEFORE, MUTATION_AFTER, 1
    )
    assert Path(b_paths[1]).read_bytes() == payloads[1]
    assert not (tmp_path / "authority" / ".objects").exists()


def test_authority_refuses_insufficient_compile_valid_inputs(tmp_path: Path) -> None:
    root = tmp_path / "corpus"
    relative = "source/gecko/a.ii"
    source = root / relative
    source.parent.mkdir(parents=True)
    source.write_text("int x;\n", encoding="utf-8")
    trace = tmp_path / "trace.tsv"
    audit = tmp_path / "rows.json"
    _write_trace(trace, [relative])
    _write_audit(audit, [source], [False])

    with pytest.raises(FirefoxCorpusAuthorityError, match="only 0"):
        build_firefox_authority(
            trace_path=trace,
            corpus_root=root,
            audit_rows=(audit,),
            output_root=tmp_path / "authority",
            authority_marker=PurePosixPath("source/gecko"),
            compiler=Path(sys.executable).resolve(),
            compiler_arguments=(),
            count=1,
        )


def test_authority_refuses_zero_exit_without_an_object(tmp_path: Path) -> None:
    root = tmp_path / "corpus"
    relative = "source/gecko/a.ii"
    source = root / relative
    source.parent.mkdir(parents=True)
    source.write_bytes(MUTATION_BEFORE + b";\n")
    trace = tmp_path / "trace.tsv"
    audit = tmp_path / "rows.json"
    _write_trace(trace, [relative])
    _write_audit(audit, [source], [True])
    compiler = tmp_path / "compiler"
    compiler.write_text(f"#!{sys.executable}\n", encoding="utf-8")
    compiler.chmod(0o755)
    output_root = tmp_path / "authority"

    with pytest.raises(FirefoxCorpusAuthorityError, match="2/2 inputs"):
        build_firefox_authority(
            trace_path=trace,
            corpus_root=root,
            audit_rows=(audit,),
            output_root=output_root,
            authority_marker=PurePosixPath("source/gecko"),
            compiler=compiler,
            compiler_arguments=(),
            count=1,
            jobs=1,
            timeout_s=10,
        )

    assert (output_root / "compile-validation.json").is_file()
    assert not (output_root / "authority.json").exists()
