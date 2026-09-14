from __future__ import annotations

import hashlib
import json
import os
import shlex
import signal
import subprocess
import sys
import time
import uuid
from pathlib import Path

import pytest

from farmharness.integration.firefox_corpus_promotion import (
    FirefoxCorpusPromotionError,
    validate_and_verify_bodies,
    validate_corpus_promotion,
)
from farmharness.integration.firefox_root_header_authority import (
    AUTHORITY_SCHEMA,
    CAPTURE_SCHEMA,
    RootHeaderAuthorityError,
    _publish_directory_noreplace,
    capture_root_header_authority,
    build_root_header_authority,
    run_compile_validation,
    run_retained_true_path_revalidation,
    validate_root_header_authority,
    main,
)
from farmharness.integration.firefox_root_header_authority import (
    NAMESPACE_ARGV,
    NAMESPACE_TIMEOUT_ARGV,
    _compile_commands,
    _difference_summary,
    _first_diagnostic,
    _manifest,
    _preprocess_argv,
    _trace,
)
from farmharness.integration.schema_validation import canonical_bytes, load_json


COUNT = 1_000
AFFECTED = 761
def _digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _digest_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _token_pids(token: str) -> set[int]:
    needle = token.encode()
    found: set[int] = set()
    for entry in Path("/proc").glob("[0-9]*"):
        try:
            pid = int(entry.name)
            if needle in (entry / "cmdline").read_bytes():
                found.add(pid)
        except (FileNotFoundError, PermissionError, ValueError):
            continue
    return found


def _init_source(tmp_path: Path) -> tuple[Path, Path, str]:
    root = tmp_path / "source"
    header = root / "include" / "root.h"
    header.parent.mkdir(parents=True)
    header.write_text("#define ROOT 1\n", encoding="utf-8")
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    subprocess.run(["git", "-C", str(root), "config", "user.email", "test@example.invalid"], check=True)
    subprocess.run(["git", "-C", str(root), "config", "user.name", "S80 Test"], check=True)
    subprocess.run(["git", "-C", str(root), "add", str(header)], check=True)
    subprocess.run(["git", "-C", str(root), "commit", "-qm", "root"], check=True)
    commit = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip()
    return root, header, commit


def _make_inputs(
    tmp_path: Path, *, affected: int = AFFECTED
) -> dict[str, Path | dict[str, object]]:
    tmp_path.mkdir(parents=True, exist_ok=True)
    source_root = tmp_path / "source"
    header_before = source_root / "memory" / "mozalloc.h"
    header_before.parent.mkdir(parents=True)
    header_before.write_bytes(b"#define ICE_ROOT 1\nint alloc();\n")
    subprocess.run(["git", "init", "-q", str(source_root)], check=True)
    subprocess.run(["git", "-C", str(source_root), "config", "user.email", "test@example.invalid"], check=True)
    subprocess.run(["git", "-C", str(source_root), "config", "user.name", "S80 Test"], check=True)
    subprocess.run(["git", "-C", str(source_root), "add", str(header_before)], check=True)
    subprocess.run(["git", "-C", str(source_root), "commit", "-qm", "root header"], check=True)
    source_commit = subprocess.check_output(
        ["git", "-C", str(source_root), "rev-parse", "HEAD"], text=True
    ).strip()
    header_after = tmp_path / "edited-mozalloc.h"
    header_after.write_bytes(b"#define ICE_ROOT X1\nint alloc();\n")
    edit_diff = tmp_path / "root-header.diff"
    edit_diff.write_text(
        f"--- {header_before}\n+++ {header_after}\n"
        "@@ -1 +1 @@\n-#define ICE_ROOT 1\n+#define ICE_ROOT X1\n",
        encoding="utf-8",
    )

    actual_root = tmp_path / "actual"
    a_root = tmp_path / "A"
    b_root = tmp_path / "B"
    actual_root.mkdir()
    a_root.mkdir()
    b_root.mkdir()
    trace_rows: list[str] = ["logical\tii_relative\tactual_input\n"]
    compile_rows: list[dict[str, object]] = []
    a_paths: list[Path] = []
    b_paths: list[Path] = []
    for index in range(COUNT):
        relative = f"obj/file-{index:04d}.ii"
        actual = actual_root / f"input-{index:04d}.cpp"
        a_path = a_root / relative
        b_path = b_root / relative
        actual.write_text(f"int input_{index}();\n", encoding="utf-8")
        a_path.parent.mkdir(parents=True, exist_ok=True)
        b_path.parent.mkdir(parents=True, exist_ok=True)
        body = f"int body_{index};\n".encode()
        a_path.write_bytes(body)
        b_path.write_bytes(body if index >= affected else b"X" + body)
        trace_rows.append(f"{index}\t{relative}\t{actual}\n")
        copy_script = (
            "import pathlib,sys; "
            "source=pathlib.Path(sys.argv[1]); "
            "target=pathlib.Path(sys.argv[sys.argv.index('-o')+1]); "
            "target.write_bytes(source.read_bytes())"
        )
        compile_rows.append(
            {
                "arguments": [sys.executable, "-c", copy_script, str(actual)],
                "directory": str(tmp_path),
                "file": str(actual),
            }
        )
        a_paths.append(a_path)
        b_paths.append(b_path)
    trace = tmp_path / "selection-trace.tsv"
    trace.write_text("".join(trace_rows), encoding="utf-8")
    compile_commands = tmp_path / "compile_commands.json"
    compile_commands.write_bytes(canonical_bytes(compile_rows))
    a_manifest = tmp_path / "turnA.manifest"
    b_manifest = tmp_path / "turnB.manifest"
    a_manifest.write_text("".join(f"{path}\n" for path in a_paths), encoding="utf-8")
    b_manifest.write_text("".join(f"{path}\n" for path in b_paths), encoding="utf-8")
    validation_rows: list[dict[str, object]] = []
    compiler_path = Path(sys.executable).resolve()
    compiler_sha = _digest(compiler_path)
    command_hashes = {
        row["file"]: hashlib.sha256(canonical_bytes(row)).hexdigest()
        for row in compile_rows
    }
    for index in range(COUNT):
        for turn, path in (("A", a_paths[index]), ("B", b_paths[index])):
            object_path = tmp_path / "receipts" / "objects" / turn / f"{index:04d}.o"
            raw_command = compile_rows[index]
            validation_rows.append(
                {
                    "actual_input": str(actual_root / f"input-{index:04d}.cpp"),
                    "command_sha256": command_hashes[str(actual_root / f"input-{index:04d}.cpp")],
                    "argv": [str(compiler_path), *raw_command["arguments"][1:-1], str(path), "-o", str(object_path)],
                    "compiler_path": str(compiler_path),
                    "compiler_sha256": compiler_sha,
                    "elapsed_ms": 1.0,
                    "error": "",
                    "exit_code": 0,
                    "index": index,
                    "input_sha256": _digest(path),
                    "object_path": str(object_path),
                    "output_sha256": "d" * 64,
                    "resolution_basis": "exact-file",
                    "resolution_row_sha256": command_hashes[str(actual_root / f"input-{index:04d}.cpp")],
                    "stderr_first_diagnostic": "",
                    "stderr_sha256": hashlib.sha256(b"").hexdigest(),
                    "stdout_sha256": hashlib.sha256(b"").hexdigest(),
                    "timed_out": False,
                    "turn": turn,
                    "working_directory": str(tmp_path),
                }
            )
    compile_validation = tmp_path / "compile-validation.json"
    compile_validation.write_bytes(
        canonical_bytes(
            {
                "compile_commands_sha256": _digest(compile_commands),
                "count": COUNT,
                "executor": {"jobs": 2, "timeout_s": 30.0},
                "rows": validation_rows,
                "schema": "icefarm-firefox-root-compile-validation-v1",
                "source_error": "",
                "source_evidence": {
                    "commit": source_commit,
                    "git_end": {"head": source_commit, "status": "", "status_sha256": hashlib.sha256(b"").hexdigest()},
                    "git_start": {"head": source_commit, "status": "", "status_sha256": hashlib.sha256(b"").hexdigest()},
                    "header_end": {"committed_sha256": _digest(header_before), "current_sha256": _digest(header_before), "path": str(header_before), "relative": "memory/mozalloc.h", "tracked": True},
                    "header_start": {"committed_sha256": _digest(header_before), "current_sha256": _digest(header_before), "path": str(header_before), "relative": "memory/mozalloc.h", "tracked": True},
                    "root": str(source_root),
                },
            }
        )
    )
    provenance: dict[str, object] = {
        "edit_diff": str(edit_diff),
        "edited_header": str(header_after),
        "mode": "true-path-bind",
        "true_path_bind": {
            "mount_argv": ["mount", "--bind", str(header_after), str(header_before)],
            "namespace_argv": ["unshare", "-Urm", "--propagation", "unchanged"],
            "target": str(header_before),
        },
    }
    return {
        "a_manifest": a_manifest,
        "b_manifest": b_manifest,
        "compile_commands": compile_commands,
        "compile_validation": compile_validation,
        "edit_diff": edit_diff,
        "header_after": header_after,
        "header_before": header_before,
        "output": tmp_path / "authority",
        "provenance": provenance,
        "source_root": source_root,
        "source_commit": source_commit,
        "trace": trace,
    }


def _build(inputs: dict[str, Path | dict[str, object]]) -> dict[str, object]:
    return build_root_header_authority(
        output_root=inputs["output"],  # type: ignore[arg-type]
        source_root=inputs["source_root"],  # type: ignore[arg-type]
        source_commit=inputs["source_commit"],  # type: ignore[arg-type]
        root_include=str(inputs["header_before"]),
        header_before=inputs["header_before"],  # type: ignore[arg-type]
        header_after=inputs["header_after"],  # type: ignore[arg-type]
        edit_diff=inputs["edit_diff"],  # type: ignore[arg-type]
        trace_path=inputs["trace"],  # type: ignore[arg-type]
        compile_commands_path=inputs["compile_commands"],  # type: ignore[arg-type]
        turn_a_manifest=inputs["a_manifest"],  # type: ignore[arg-type]
        turn_b_manifest=inputs["b_manifest"],  # type: ignore[arg-type]
        compile_validation=inputs["compile_validation"],  # type: ignore[arg-type]
        provenance=inputs["provenance"],  # type: ignore[arg-type]
    )


def _build_with_law(
    inputs: dict[str, Path | dict[str, object]], affected: int
) -> dict[str, object]:
    return build_root_header_authority(
        output_root=inputs["output"],  # type: ignore[arg-type]
        source_root=inputs["source_root"],  # type: ignore[arg-type]
        source_commit=inputs["source_commit"],  # type: ignore[arg-type]
        root_include=str(inputs["header_before"]),
        header_before=inputs["header_before"],  # type: ignore[arg-type]
        header_after=inputs["header_after"],  # type: ignore[arg-type]
        edit_diff=inputs["edit_diff"],  # type: ignore[arg-type]
        trace_path=inputs["trace"],  # type: ignore[arg-type]
        compile_commands_path=inputs["compile_commands"],  # type: ignore[arg-type]
        turn_a_manifest=inputs["a_manifest"],  # type: ignore[arg-type]
        turn_b_manifest=inputs["b_manifest"],  # type: ignore[arg-type]
        compile_validation=inputs["compile_validation"],  # type: ignore[arg-type]
        provenance=inputs["provenance"],  # type: ignore[arg-type]
        expected_affected=affected,
        expected_unaffected=COUNT - affected,
    )


def _promotion_corpus(
    inputs: dict[str, Path | dict[str, object]], authority: dict[str, object]
) -> dict[str, object]:
    pair = load_json(Path(authority["pair_index"]["path"]))  # type: ignore[index]
    normalized = hashlib.sha256(
        ("\n".join(row["path"] for row in pair["pairs"]) + "\n").encode()
    ).hexdigest()
    compiler = Path(sys.executable).resolve()
    return {
        "authority_receipt": {
            "path": str(inputs["output"] / "authority.json"),  # type: ignore[operator]
            "sha256": _digest(inputs["output"] / "authority.json"),  # type: ignore[operator]
        },
        "compiler_recipes": {
            "python-test": {
                "arguments": [],
                "binary_sha256": _digest(compiler),
                "executable": str(compiler),
            }
        },
        "normalized_manifest_sha256": normalized,
        "pair_index_sha256": authority["pair_index"]["sha256"],  # type: ignore[index]
        "root_header_body_law": authority["body_law"],
        "root": str(inputs["source_root"]),
        "turn_a_manifest": authority["turn_a_manifest"]["path"],  # type: ignore[index]
        "turn_b_manifest": authority["turn_b_manifest"]["path"],  # type: ignore[index]
        "tus": COUNT,
    }


def test_true_path_authority_proves_order_body_law_and_both_turns(tmp_path: Path) -> None:
    inputs = _make_inputs(tmp_path)
    authority = _build(inputs)
    assert authority["schema"] == AUTHORITY_SCHEMA
    assert authority["generation"]["source_commit"] == inputs["source_commit"]  # type: ignore[index]
    assert authority["generation"]["generator"].endswith("firefox_root_header_authority")  # type: ignore[index]
    assert authority["body_law"] == {"affected": 761, "unaffected": 239}
    assert authority["compile_validation"]["turn_a_pass"] == COUNT  # type: ignore[index]
    assert authority["compile_validation"]["turn_b_pass"] == COUNT  # type: ignore[index]
    selection = load_json(Path(authority["selection"]["path"]))  # type: ignore[index]
    assert selection["rows"][0]["index"] == 0
    assert selection["rows"][-1]["index"] == COUNT - 1
    assert selection["rows"][0]["ii_relative"] == "obj/file-0000.ii"
    assert validate_root_header_authority(inputs["output"] / "authority.json")["schema"] == AUTHORITY_SCHEMA  # type: ignore[operator]


def test_alternative_body_law_is_explicitly_bound_end_to_end(tmp_path: Path) -> None:
    inputs = _make_inputs(tmp_path, affected=617)
    authority = _build_with_law(inputs, 617)
    assert authority["body_law"] == {"affected": 617, "unaffected": 383}
    validated = validate_root_header_authority(
        inputs["output"] / "authority.json"  # type: ignore[operator]
    )
    assert validated["body_law"] == authority["body_law"]


def test_preprocess_preserves_original_relative_input_spelling(tmp_path: Path) -> None:
    actual = tmp_path / "obj" / "unit.cpp"
    actual.parent.mkdir()
    actual.write_text("int unit;\n", encoding="utf-8")
    candidate = {
        "actual_input": str(actual),
        "argv": ["/bin/cc", "-c", "unit.cpp", "-o", "unit.o"],
        "compiler_path": "/bin/cc",
        "directory": str(actual.parent),
        "input_operand": "unit.cpp",
        "input_position": 2,
    }

    assert _preprocess_argv(candidate, actual) == ["/bin/cc", "unit.cpp", "-E"]
    replacement = tmp_path / "replacement.ii"
    assert _preprocess_argv(candidate, replacement) == [
        "/bin/cc",
        str(replacement),
        "-E",
    ]


def test_failure_diagnostics_prefer_error_and_bound_first_difference() -> None:
    stderr = b"clang: warning: unused option\nunit.cpp:7:3: error: broken token\n"
    assert _first_diagnostic(stderr) == "unit.cpp:7:3: error: broken token"
    summary = _difference_summary(b"0123456789abcdefLEFT", b"0123456789abcdefRIGHT")
    assert summary.startswith(
        "first_difference=16;retained_bytes=20;reproduced_bytes=21;"
    )
    assert "retained_hex=" in summary
    assert "reproduced_hex=" in summary


def test_revalidation_rejects_tampered_body_and_no_overwrite(tmp_path: Path) -> None:
    inputs = _make_inputs(tmp_path)
    _build(inputs)
    b_path = Path(inputs["b_manifest"].read_text(encoding="utf-8").splitlines()[0])  # type: ignore[union-attr]
    original = b_path.read_bytes()
    b_path.write_bytes(b"tampered\n")
    with pytest.raises(RootHeaderAuthorityError, match=r"SHA-256 mismatch|pair\[0\]"):
        validate_root_header_authority(inputs["output"] / "authority.json")  # type: ignore[operator]
    b_path.write_bytes(original)
    authority_path = inputs["output"] / "authority.json"  # type: ignore[operator]
    authority = load_json(authority_path)
    authority["generation"]["source_commit"] = "0" * 40
    authority_path.write_bytes(canonical_bytes(authority))
    with pytest.raises(RootHeaderAuthorityError, match="generation.source_commit"):
        validate_root_header_authority(authority_path)
    with pytest.raises(RootHeaderAuthorityError, match="output already exists"):
        _build(inputs)
    dangling = tmp_path / "dangling-authority"
    dangling.symlink_to(tmp_path / "missing-target", target_is_directory=True)
    inputs["output"] = dangling
    with pytest.raises(RootHeaderAuthorityError, match="output already exists"):
        _build(inputs)


def test_refuses_direct_mutation_and_invalid_true_path_provenance(tmp_path: Path) -> None:
    inputs = _make_inputs(tmp_path)
    provenance = inputs["provenance"]
    assert isinstance(provenance, dict)
    provenance["mode"] = "direct-ii-mutation"
    with pytest.raises(RootHeaderAuthorityError, match="true-path"):
        _build(inputs)
    provenance["mode"] = "true-path-bind"
    provenance["true_path_bind"]["target"] = "/wrong/header"  # type: ignore[index]
    inputs["output"] = tmp_path / "authority-2"
    with pytest.raises(RootHeaderAuthorityError, match="does not equal root header"):
        _build(inputs)


def test_refuses_missing_compile_command_and_wrong_trace_order(tmp_path: Path) -> None:
    inputs = _make_inputs(tmp_path)
    compile_rows = load_json(inputs["compile_commands"])  # type: ignore[arg-type]
    compile_rows.pop()
    inputs["compile_commands"].write_bytes(canonical_bytes(compile_rows))  # type: ignore[union-attr]
    with pytest.raises(RootHeaderAuthorityError, match="got 0"):
        _build(inputs)

    inputs = _make_inputs(tmp_path / "order")
    lines = inputs["a_manifest"].read_text(encoding="utf-8").splitlines()  # type: ignore[union-attr]
    lines[0], lines[1] = lines[1], lines[0]
    inputs["a_manifest"].write_text("\n".join(lines) + "\n", encoding="utf-8")  # type: ignore[union-attr]
    with pytest.raises(RootHeaderAuthorityError, match="does not contain the trace suffix"):
        _build(inputs)


def test_refuses_non_single_header_edit(tmp_path: Path) -> None:
    inputs = _make_inputs(tmp_path)
    inputs["header_after"].write_bytes(b"#define ICE_ROOT XX1\nint alloc();\n")  # type: ignore[union-attr]
    with pytest.raises(RootHeaderAuthorityError, match="insert exactly one byte"):
        _build(inputs)


def test_retained_mode_revalidates_namespace_rows_and_authority(tmp_path: Path) -> None:
    inputs = _make_inputs(tmp_path)
    b_paths = [Path(line) for line in inputs["b_manifest"].read_text(encoding="utf-8").splitlines()]  # type: ignore[union-attr]

    def fake_invoke(argv: list[str], timeout_s: float) -> subprocess.CompletedProcess[bytes]:
        assert argv[:4] == ["unshare", "-Urm", "--propagation", "unchanged"]
        assert argv[4:7] == ["--pid", "--fork", "--kill-child"]
        payload = json.loads(argv[-1])
        actual = Path(payload["compile_argv"][-2] if payload["compile_argv"][-1] == "-E" else payload["compile_argv"][-1])
        index = int(actual.stem.split("-")[-1])
        mountinfo = f"36 25 0:32 / {payload['root_include']} rw - bind bind rw\n".encode()
        Path(payload["mountinfo_path"]).write_bytes(mountinfo)
        source = Path(payload["edited_header"])
        source_sha = _digest(source)
        stat = source.stat()
        evidence = {
            "bind_device": stat.st_dev,
            "bind_inode": stat.st_ino,
            "bind_source_sha256": source_sha,
            "bind_target_sha256": source_sha,
            "mountinfo_row_sha256": _digest_bytes(mountinfo.rstrip(b"\n")),
            "source_device": stat.st_dev,
            "source_inode": stat.st_ino,
            "target_device": stat.st_dev,
            "target_inode": stat.st_ino,
        }
        return subprocess.CompletedProcess(
            argv,
            0,
            stdout=b_paths[index].read_bytes(),
            stderr=(
                b"ICEFARM-MOUNTINFO-SHA256:" + hashlib.sha256(mountinfo).hexdigest().encode() + b"\n"
                + b"ICEFARM-BIND-EVIDENCE:" + json.dumps(evidence, sort_keys=True, separators=(",", ":")).encode() + b"\n"
            ),
        )

    receipt_path = tmp_path / "retained-execution.json"
    run_retained_true_path_revalidation(
        source_root=inputs["source_root"],  # type: ignore[arg-type]
        source_commit=inputs["source_commit"],  # type: ignore[arg-type]
        root_include=inputs["header_before"],  # type: ignore[arg-type]
        header_after=inputs["header_after"],  # type: ignore[arg-type]
        trace_path=inputs["trace"],  # type: ignore[arg-type]
        compile_commands_path=inputs["compile_commands"],  # type: ignore[arg-type]
        turn_a_manifest=inputs["a_manifest"],  # type: ignore[arg-type]
        turn_b_manifest=inputs["b_manifest"],  # type: ignore[arg-type]
        output_path=receipt_path,
        jobs=4,
        timeout_s=10,
        invoke=fake_invoke,
    )
    inputs["provenance"]["mode"] = "retained-true-path-revalidation"  # type: ignore[index]
    inputs["provenance"]["execution_receipt"] = {"path": str(receipt_path), "sha256": _digest(receipt_path)}  # type: ignore[index]
    authority = _build(inputs)
    assert validate_root_header_authority(inputs["output"] / "authority.json")["schema"] == AUTHORITY_SCHEMA  # type: ignore[operator]

    corpus = _promotion_corpus(inputs, authority)
    promotion = validate_corpus_promotion(corpus)
    assert promotion.mutation_kind == "single-insert"
    assert promotion.inserted_byte == ord("X")
    assert len(validate_and_verify_bodies(corpus).pair_rows) == COUNT
    with pytest.raises(
        FirefoxCorpusPromotionError,
        match="corpus.root_header_body_law",
    ):
        validate_corpus_promotion(
            {key: value for key, value in corpus.items() if key != "root_header_body_law"}
        )
    with pytest.raises(FirefoxCorpusPromotionError, match="authority.body_law"):
        validate_corpus_promotion(
            {
                **corpus,
                "root_header_body_law": {"affected": 760, "unaffected": 240},
            }
        )
    with pytest.raises(FirefoxCorpusPromotionError, match="normalized_manifest_sha256"):
        validate_corpus_promotion({**corpus, "normalized_manifest_sha256": "0" * 64})

    authority_path = inputs["output"] / "authority.json"  # type: ignore[operator]
    authority_bytes = authority_path.read_bytes()

    b_path = Path(inputs["b_manifest"].read_text(encoding="utf-8").splitlines()[0])  # type: ignore[union-attr]
    b_bytes = b_path.read_bytes()
    b_path.write_bytes(b"Y" + b_bytes[1:])
    with pytest.raises(
        FirefoxCorpusPromotionError,
        match=r"exact permitted insertion|body SHA-256",
    ):
        validate_and_verify_bodies(corpus)
    b_path.write_bytes(b_bytes)

    selection_path = Path(authority["selection"]["path"])  # type: ignore[index]
    selection_bytes = selection_path.read_bytes()
    tampered_selection = load_json(selection_path)
    tampered_selection["turn_a_root"] += "-forged"
    selection_path.write_bytes(canonical_bytes(tampered_selection))
    tampered_authority = load_json(authority_path)
    tampered_authority["selection"]["sha256"] = _digest(selection_path)
    authority_path.write_bytes(canonical_bytes(tampered_authority))
    tampered_corpus = {
        **corpus,
        "authority_receipt": {
            "path": str(authority_path),
            "sha256": _digest(authority_path),
        },
    }
    with pytest.raises(FirefoxCorpusPromotionError, match="selection"):
        validate_corpus_promotion(tampered_corpus)
    selection_path.write_bytes(selection_bytes)
    authority_path.write_bytes(authority_bytes)

    trace_path = Path(inputs["trace"])
    trace_bytes = trace_path.read_bytes()
    trace_lines = trace_bytes.decode().splitlines()
    first = trace_lines[1].split("\t")
    second = trace_lines[2].split("\t")
    first[2], second[2] = second[2], first[2]
    trace_lines[1], trace_lines[2] = "\t".join(first), "\t".join(second)
    trace_path.write_text("\n".join(trace_lines) + "\n", encoding="utf-8")
    tampered_selection = load_json(selection_path)
    tampered_selection["trace_sha256"] = _digest(trace_path)
    selection_path.write_bytes(canonical_bytes(tampered_selection))
    tampered_authority = load_json(authority_path)
    tampered_authority["generation"]["trace_sha256"] = _digest(trace_path)
    tampered_authority["trace"]["sha256"] = _digest(trace_path)
    tampered_authority["selection"]["sha256"] = _digest(selection_path)
    authority_path.write_bytes(canonical_bytes(tampered_authority))
    tampered_corpus["authority_receipt"]["sha256"] = _digest(authority_path)
    with pytest.raises(FirefoxCorpusPromotionError, match="trace/pair mapping"):
        validate_corpus_promotion(tampered_corpus)
    trace_path.write_bytes(trace_bytes)
    selection_path.write_bytes(selection_bytes)
    authority_path.write_bytes(authority_bytes)

    validation_path = Path(authority["compile_validation"]["path"])  # type: ignore[index]
    validation_bytes = validation_path.read_bytes()
    tampered_validation = load_json(validation_path)
    tampered_validation["rows"][0]["argv"].insert(1, "--forged")
    validation_path.write_bytes(canonical_bytes(tampered_validation))
    tampered_authority = load_json(authority_path)
    tampered_authority["compile_validation"]["sha256"] = _digest(validation_path)
    authority_path.write_bytes(canonical_bytes(tampered_authority))
    tampered_corpus["authority_receipt"]["sha256"] = _digest(authority_path)
    with pytest.raises(
        FirefoxCorpusPromotionError,
        match="does not reproduce the selected compile command",
    ):
        validate_corpus_promotion(tampered_corpus)
    validation_path.write_bytes(validation_bytes)
    authority_path.write_bytes(authority_bytes)

    tampered_authority = load_json(authority_path)
    tampered_authority["trace"]["sha256"] = "0" * 64
    authority_path.write_bytes(canonical_bytes(tampered_authority))
    tampered_corpus = {
        **corpus,
        "authority_receipt": {
            "path": str(authority_path),
            "sha256": _digest(authority_path),
        },
    }
    with pytest.raises(FirefoxCorpusPromotionError, match="authority.trace"):
        validate_corpus_promotion(tampered_corpus)
    authority_path.write_bytes(authority_bytes)

    receipt = load_json(inputs["output"] / "retained-execution.json")  # type: ignore[operator]
    row = receipt["rows"][0]
    mountinfo_path = Path(row["mountinfo_path"])
    wrong_mountinfo = "36 25 0:32 / /wrong-target rw - bind bind rw\n".encode()
    mountinfo_path.write_bytes(wrong_mountinfo)
    row["mountinfo_sha256"] = _digest_bytes(wrong_mountinfo)
    row["mountinfo_row_sha256"] = _digest_bytes(wrong_mountinfo.rstrip(b"\n"))
    stderr_path = Path(row["stderr_path"])
    evidence = {
        "bind_device": row["bind_device"], "bind_inode": row["bind_inode"],
        "bind_source_sha256": row["bind_source_sha256"], "bind_target_sha256": row["bind_target_sha256"],
        "mountinfo_row_sha256": row["mountinfo_row_sha256"], "source_device": row["source_device"],
        "source_inode": row["source_inode"], "target_device": row["target_device"], "target_inode": row["target_inode"],
    }
    stderr = (
        b"ICEFARM-MOUNTINFO-SHA256:" + row["mountinfo_sha256"].encode() + b"\n"
        + b"ICEFARM-BIND-EVIDENCE:" + json.dumps(evidence, sort_keys=True, separators=(",", ":")).encode() + b"\n"
    )
    stderr_path.write_bytes(stderr)
    row["stderr_sha256"] = _digest_bytes(stderr)
    row["stderr_first_diagnostic"] = stderr.splitlines()[0].decode()
    (inputs["output"] / "retained-execution.json").write_bytes(canonical_bytes(receipt))  # type: ignore[operator]
    authority_path = inputs["output"] / "authority.json"  # type: ignore[operator]
    authority = load_json(authority_path)
    authority["provenance"]["execution_receipt"]["sha256"] = _digest(inputs["output"] / "retained-execution.json")  # type: ignore[index,operator]
    authority_path.write_bytes(canonical_bytes(authority))
    with pytest.raises(RootHeaderAuthorityError, match="target-row"):
        validate_root_header_authority(authority_path)
    valid_mountinfo = f"36 25 0:32 / {inputs['header_before']} rw - bind bind rw\n".encode()
    mountinfo_path.write_bytes(valid_mountinfo)
    row["mountinfo_sha256"] = _digest_bytes(valid_mountinfo)
    row["mountinfo_row_sha256"] = _digest_bytes(valid_mountinfo.rstrip(b"\n"))
    evidence["mountinfo_row_sha256"] = row["mountinfo_row_sha256"]
    stderr = (
        b"ICEFARM-MOUNTINFO-SHA256:" + row["mountinfo_sha256"].encode() + b"\n"
        + b"ICEFARM-BIND-EVIDENCE:" + json.dumps(evidence, sort_keys=True, separators=(",", ":")).encode() + b"\n"
    )
    stderr_path.write_bytes(stderr)
    row["stderr_sha256"] = _digest_bytes(stderr)
    row["stderr_first_diagnostic"] = stderr.splitlines()[0].decode()
    row["bind_target_sha256"] = "0" * 64
    (inputs["output"] / "retained-execution.json").write_bytes(canonical_bytes(receipt))  # type: ignore[operator]
    authority["provenance"]["execution_receipt"]["sha256"] = _digest(inputs["output"] / "retained-execution.json")  # type: ignore[index,operator]
    authority_path.write_bytes(canonical_bytes(authority))
    with pytest.raises(RootHeaderAuthorityError, match="stderr bind identity"):
        validate_root_header_authority(authority_path)


def test_real_namespace_timeout_kills_descendants() -> None:
    assert tuple(NAMESPACE_TIMEOUT_ARGV[:4]) == NAMESPACE_ARGV
    assert tuple(NAMESPACE_TIMEOUT_ARGV[4:]) == ("--pid", "--fork", "--kill-child")
    probe = subprocess.run(
        [*NAMESPACE_TIMEOUT_ARGV, sys.executable, "-c", "pass"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=5,
    )
    if probe.returncode != 0:
        diagnostic = (probe.stdout + probe.stderr).decode("utf-8", "replace")
        if any(text in diagnostic.lower() for text in ("operation not permitted", "permission denied", "unshare failed")):
            pytest.skip(f"user/PID namespace capability unavailable: {diagnostic.strip()}")
        pytest.fail(f"namespace capability probe failed unexpectedly: {diagnostic.strip()}")

    token = f"icefarm-s80-timeout-{uuid.uuid4().hex}"
    helper = (
        "import subprocess,sys,time; "
        "token=sys.argv[1]; "
        "subprocess.Popen([sys.executable,'-c','import time; time.sleep(30)',token]); "
        "time.sleep(30)"
    )
    process = subprocess.Popen(
        [*NAMESPACE_TIMEOUT_ARGV, sys.executable, "-c", helper, token],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    survivors: set[int] = set()
    try:
        with pytest.raises(subprocess.TimeoutExpired):
            process.communicate(timeout=0.5)
        process.kill()
        process.wait(timeout=5)
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            survivors = _token_pids(token)
            if not survivors:
                break
            time.sleep(0.05)
    finally:
        for pid in _token_pids(token):
            try:
                os.kill(pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass
        try:
            process.kill()
        except ProcessLookupError:
            pass
        process.wait(timeout=5)
    remaining = _token_pids(token)
    assert not survivors, f"namespace timeout leaked token processes: {sorted(survivors)}"
    assert not remaining, f"namespace timeout cleanup left token processes: {sorted(remaining)}"


def test_directory_publication_refuses_forced_destination_race(tmp_path: Path) -> None:
    source = tmp_path / "staging"
    destination = tmp_path / "authority"
    source.mkdir()
    (source / "authority.json").write_text("{}", encoding="utf-8")
    _publish_directory_noreplace(source, destination)
    second = tmp_path / "second"
    second.mkdir()
    with pytest.raises(RootHeaderAuthorityError, match="output appeared"):
        _publish_directory_noreplace(second, destination)


def test_capture_cli_owns_producer_receipts_and_requires_capture_inputs(monkeypatch: pytest.MonkeyPatch) -> None:
    import farmharness.integration.firefox_root_header_authority as module

    called: dict[str, object] = {}

    def fake_capture(**kwargs: object) -> dict[str, object]:
        called.update(kwargs)
        return {"schema": CAPTURE_SCHEMA}

    monkeypatch.setattr(module, "capture_root_header_authority", fake_capture)
    output = "/tmp/i/s80-authority"
    rc = main([
        "--capture", "--source-root", "/tmp/source", "--source-commit", "a" * 40,
        "--root-include", "/tmp/source/root.h", "--header-after", "/tmp/edited.h",
        "--edit-diff", "/tmp/root.diff", "--trace", "/tmp/trace.tsv",
        "--compile-commands", "/tmp/compile_commands.json", "--turn-a", "/tmp/A.manifest",
        "--turn-b", "/tmp/B.manifest", "--output", output,
        "--capture-receipt", "/tmp/i/s80-capture.json",
        "--expected-affected", "617", "--expected-unaffected", "383",
    ])
    assert rc == 0
    assert called["output_root"] == Path(output)
    assert "compile_validation" not in called
    assert called["capture_receipt_path"] == Path("/tmp/i/s80-capture.json")
    assert called["expected_affected"] == 617
    assert called["expected_unaffected"] == 383


def test_capture_failure_retains_terminal_evidence_without_authority(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    def fail_compile(**_kwargs: object) -> None:
        raise RootHeaderAuthorityError("producer failed")

    monkeypatch.setattr(
        "farmharness.integration.firefox_root_header_authority.run_compile_validation",
        fail_compile,
    )
    output = tmp_path / "authority"
    receipt = tmp_path / "capture.json"
    with pytest.raises(RootHeaderAuthorityError, match="producer failed"):
        capture_root_header_authority(
            output_root=output,
            capture_receipt_path=receipt,
            source_root=tmp_path,
            source_commit="a" * 40,
            root_include=tmp_path / "root.h",
            header_before=tmp_path / "root.h",
            header_after=tmp_path / "edited.h",
            edit_diff=tmp_path / "root.diff",
            trace_path=tmp_path / "trace.tsv",
            compile_commands_path=tmp_path / "commands.json",
            turn_a_manifest=tmp_path / "A.manifest",
            turn_b_manifest=tmp_path / "B.manifest",
        )
    assert not output.exists()
    failure = load_json(receipt)
    assert failure["schema"] == CAPTURE_SCHEMA
    assert failure["status"] == "FAIL"
    assert Path(failure["failure_root"]).is_dir()


def test_capture_stops_before_retained_phase_on_nonzero_compile(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    def failed_compile(**kwargs: object) -> dict[str, object]:
        Path(kwargs["output_path"]).write_bytes(b'{"stage":"compile"}')
        return {
            "rows": [
                {
                    "error": "",
                    "exit_code": 1,
                    "index": 17,
                    "output_sha256": None,
                    "timed_out": False,
                    "turn": "B",
                }
            ]
        }

    def retained_must_not_run(**_kwargs: object) -> None:
        pytest.fail("retained phase must not run after compile failure")

    monkeypatch.setattr(
        "farmharness.integration.firefox_root_header_authority.run_compile_validation",
        failed_compile,
    )
    monkeypatch.setattr(
        "farmharness.integration.firefox_root_header_authority.run_retained_true_path_revalidation",
        retained_must_not_run,
    )
    output = tmp_path / "authority"
    receipt = tmp_path / "capture.json"
    with pytest.raises(
        RootHeaderAuthorityError,
        match=r"compile_validation.rows\[17,B\]: not a definitive pass",
    ):
        capture_root_header_authority(
            output_root=output,
            capture_receipt_path=receipt,
            source_root=tmp_path,
            source_commit="a" * 40,
            root_include=tmp_path / "root.h",
            header_before=tmp_path / "root.h",
            header_after=tmp_path / "edited.h",
            edit_diff=tmp_path / "root.diff",
            trace_path=tmp_path / "trace.tsv",
            compile_commands_path=tmp_path / "commands.json",
            turn_a_manifest=tmp_path / "A.manifest",
            turn_b_manifest=tmp_path / "B.manifest",
        )
    assert not output.exists()
    failure = load_json(receipt)
    assert failure["status"] == "FAIL"
    assert Path(failure["failure_root"], "compile-validation.json").is_file()


def test_capture_receipt_race_leaves_only_complete_internal_capture(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    import farmharness.integration.firefox_root_header_authority as module

    def fake_compile(**kwargs: object) -> None:
        Path(kwargs["output_path"]).write_bytes(b'{"stage":"compile"}')

    def fake_retained(**kwargs: object) -> None:
        Path(kwargs["output_path"]).write_bytes(b'{"stage":"retained"}')

    def fake_build(**kwargs: object) -> None:
        output = Path(kwargs["output_root"])
        assert Path(kwargs["published_root"]).name == "authority"
        output.mkdir()
        (output / "authority.json").write_bytes(b"{}")
        for name in ("compile-validation.json", "retained-execution.json"):
            source = Path(kwargs["compile_validation"]) if name.startswith("compile") else None
            if source is None:
                source = output.parent / "retained-execution.json"
            (output / name).write_bytes(source.read_bytes())

    def fail_receipt(_path: Path, _value: object) -> None:
        raise RootHeaderAuthorityError("receipt appeared during publication")

    monkeypatch.setattr(module, "run_compile_validation", fake_compile)
    monkeypatch.setattr(module, "run_retained_true_path_revalidation", fake_retained)
    monkeypatch.setattr(module, "build_root_header_authority", fake_build)

    def validate_published(path: Path) -> None:
        assert path.parent.name == "authority"

    monkeypatch.setattr(module, "validate_root_header_authority", validate_published)
    monkeypatch.setattr(module, "_publish_json_noreplace", fail_receipt)
    output = tmp_path / "authority"
    receipt = tmp_path / "capture.json"
    with pytest.raises(RootHeaderAuthorityError, match="receipt appeared"):
        capture_root_header_authority(
            output_root=output,
            capture_receipt_path=receipt,
            source_root=tmp_path,
            source_commit="a" * 40,
            root_include=tmp_path / "root.h",
            header_before=tmp_path / "root.h",
            header_after=tmp_path / "edited.h",
            edit_diff=tmp_path / "root.diff",
            trace_path=tmp_path / "trace.tsv",
            compile_commands_path=tmp_path / "commands.json",
            turn_a_manifest=tmp_path / "A.manifest",
            turn_b_manifest=tmp_path / "B.manifest",
        )
    assert (output / "authority.json").is_file()
    internal = load_json(output / "capture-receipt.json")
    assert internal["schema"] == CAPTURE_SCHEMA
    assert internal["status"] == "PASS"
    assert not receipt.exists()


def test_capture_final_validation_failure_never_publishes_pass_receipt(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    import farmharness.integration.firefox_root_header_authority as module

    def fake_compile(**kwargs: object) -> None:
        Path(kwargs["output_path"]).write_bytes(b'{"stage":"compile"}')

    def fake_retained(**kwargs: object) -> None:
        Path(kwargs["output_path"]).write_bytes(b'{"stage":"retained"}')

    def fake_build(**kwargs: object) -> None:
        output = Path(kwargs["output_root"])
        output.mkdir()
        (output / "authority.json").write_bytes(b"{}")
        for name, source in (
            ("compile-validation.json", Path(kwargs["compile_validation"])),
            ("retained-execution.json", output.parent / "retained-execution.json"),
        ):
            (output / name).write_bytes(source.read_bytes())

    def fail_validation(_path: Path) -> None:
        raise RootHeaderAuthorityError("published authority validation failed")

    monkeypatch.setattr(module, "run_compile_validation", fake_compile)
    monkeypatch.setattr(module, "run_retained_true_path_revalidation", fake_retained)
    monkeypatch.setattr(module, "build_root_header_authority", fake_build)
    monkeypatch.setattr(module, "validate_root_header_authority", fail_validation)
    output = tmp_path / "authority"
    receipt = tmp_path / "capture.json"
    with pytest.raises(RootHeaderAuthorityError, match="published authority validation"):
        capture_root_header_authority(
            output_root=output,
            capture_receipt_path=receipt,
            source_root=tmp_path,
            source_commit="a" * 40,
            root_include=tmp_path / "root.h",
            header_before=tmp_path / "root.h",
            header_after=tmp_path / "edited.h",
            edit_diff=tmp_path / "root.diff",
            trace_path=tmp_path / "trace.tsv",
            compile_commands_path=tmp_path / "commands.json",
            turn_a_manifest=tmp_path / "A.manifest",
            turn_b_manifest=tmp_path / "B.manifest",
        )
    assert (output / "authority.json").is_file()
    assert not (output / "capture-receipt.json").exists()
    assert not receipt.exists()


def test_accepts_retained_three_line_edit_metadata(tmp_path: Path) -> None:
    inputs = _make_inputs(tmp_path)
    inputs["edit_diff"].write_text(  # type: ignore[union-attr]
        f"{inputs['header_before']}:1\n-#define ICE_ROOT 1\n+#define ICE_ROOT X1\n",
        encoding="utf-8",
    )
    _build(inputs)


def test_argv_only_executor_binds_each_input_and_retains_failure(tmp_path: Path) -> None:
    source_root, root_include, source_commit = _init_source(tmp_path)
    actual_root = tmp_path / "actual"
    a_root = tmp_path / "A"
    b_root = tmp_path / "B"
    actual_root.mkdir()
    a_root.mkdir()
    b_root.mkdir()
    script = (
        "import pathlib,sys; "
        "source=pathlib.Path(sys.argv[1]); "
        "target=pathlib.Path(sys.argv[sys.argv.index('-o')+1]); "
        "target.write_bytes(source.read_bytes())"
    )
    trace = tmp_path / "trace.tsv"
    trace.write_text(
        "logical\tii_relative\tactual_input\n"
        + "".join(
            f"{index}\tfile-{index}.ii\t{actual_root / f'input-{index}.cpp'}\n"
            for index in range(2)
        ),
        encoding="utf-8",
    )
    commands = []
    a_paths, b_paths = [], []
    for index in range(2):
        actual = actual_root / f"input-{index}.cpp"
        a_path, b_path = a_root / f"file-{index}.ii", b_root / f"file-{index}.ii"
        actual.write_text(f"actual {index}\n", encoding="utf-8")
        a_path.write_text(f"A {index}\n", encoding="utf-8")
        b_path.write_text(f"B {index}\n", encoding="utf-8")
        a_paths.append(a_path)
        b_paths.append(b_path)
        if index == 1:
            commands.append({"command": " ".join(shlex.quote(item) for item in [sys.executable, "-c", script, str(actual)]), "directory": str(tmp_path), "file": str(actual)})
        else:
            commands.append({"arguments": [sys.executable, "-c", script, str(actual)], "directory": str(tmp_path), "file": str(actual)})
    compile_commands = tmp_path / "compile_commands.json"
    compile_commands.write_bytes(canonical_bytes(commands))
    a_manifest = tmp_path / "A.manifest"
    b_manifest = tmp_path / "B.manifest"
    a_manifest.write_text("".join(f"{path}\n" for path in a_paths), encoding="utf-8")
    b_manifest.write_text("".join(f"{path}\n" for path in b_paths), encoding="utf-8")
    receipt_path = tmp_path / "compile-validation.json"
    receipt = run_compile_validation(
        source_root=source_root,
        source_commit=source_commit,
        root_include=root_include,
        trace_path=trace,
        compile_commands_path=compile_commands,
        turn_a_manifest=a_manifest,
        turn_b_manifest=b_manifest,
        output_path=receipt_path,
        count=2,
        jobs=2,
        timeout_s=10,
    )
    assert len(receipt["rows"]) == 4  # type: ignore[arg-type]
    assert all(row["exit_code"] == 0 for row in receipt["rows"])  # type: ignore[index]
    assert all(row["argv"][0] == str(Path(sys.executable).resolve()) for row in receipt["rows"])  # type: ignore[index]
    assert all("shell" not in row["argv"] for row in receipt["rows"])  # type: ignore[index]
    with pytest.raises(RootHeaderAuthorityError, match="already exists"):
        run_compile_validation(
            source_root=source_root,
            source_commit=source_commit,
            root_include=root_include,
            trace_path=trace,
            compile_commands_path=compile_commands,
            turn_a_manifest=a_manifest,
            turn_b_manifest=b_manifest,
            output_path=receipt_path,
            count=2,
            jobs=2,
            timeout_s=10,
        )


def test_executor_writes_failed_rows_instead_of_claiming_success(tmp_path: Path) -> None:
    source_root, root_include, source_commit = _init_source(tmp_path)
    actual = tmp_path / "actual.cpp"
    actual.write_text("actual\n", encoding="utf-8")
    a_path, b_path = tmp_path / "a.ii", tmp_path / "b.ii"
    a_path.write_text("A\n", encoding="utf-8")
    b_path.write_text("B\n", encoding="utf-8")
    trace = tmp_path / "trace.tsv"
    trace.write_text(f"logical\tii_relative\tactual_input\n0\ta.ii\t{actual}\n", encoding="utf-8")
    failed = tmp_path / "compile_commands.json"
    failed.write_bytes(canonical_bytes([{"arguments": [sys.executable, "-c", "import sys;sys.exit(7)", str(actual)], "directory": str(tmp_path), "file": str(actual)}]))
    a_manifest, b_manifest = tmp_path / "a.manifest", tmp_path / "b.manifest"
    a_manifest.write_text(f"{a_path}\n", encoding="utf-8")
    b_manifest.write_text(f"{b_path}\n", encoding="utf-8")
    receipt_path = tmp_path / "failed.json"
    receipt = run_compile_validation(
        source_root=source_root,
        source_commit=source_commit,
        root_include=root_include,
        trace_path=trace,
        compile_commands_path=failed,
        turn_a_manifest=a_manifest,
        turn_b_manifest=b_manifest,
        output_path=receipt_path,
        count=1,
        jobs=1,
        timeout_s=10,
    )
    assert [row["exit_code"] for row in receipt["rows"]] == [7, 7]  # type: ignore[index]
    assert receipt_path.is_file()


def test_retained_first_thousand_trace_inputs_resolve_to_one_argv_command() -> None:
    trace = Path("/tanksmall/scratch/ictmp/lo-s4-e50.G5KsGG/capability/distribution/firefox-corrected.compile-trace.tsv")
    compile_commands = Path("/tanksmall/scratch/ictmp/src3/gecko-dev/obj-cc/compile_commands.json")
    a_manifest = Path("/tanksmall/scratch/ictmp/experiments/icecream/p29v1-final-matrix-root-20260903/turnA.manifest")
    if not trace.is_file() or not compile_commands.is_file():
        pytest.skip("retained production fixture is unavailable")
    rows = _trace(trace, 1000)
    a_paths = _manifest(a_manifest, 1000, "retained.turn_a_manifest")
    specs = _compile_commands(compile_commands, rows, a_paths, True)
    assert len(specs) == 1000
    assert all(spec["actual_input"] == row["actual_input"] for spec, row in zip(specs.values(), rows, strict=True))
    assert specs[rows[15]["actual_input"]]["resolution_basis"].endswith("preprocess-a")


def test_adversarial_authority_and_source_checks(tmp_path: Path) -> None:
    inputs = _make_inputs(tmp_path)
    _build(inputs)

    authority_path = inputs["output"] / "authority.json"  # type: ignore[operator]
    authority = load_json(authority_path)
    authority["root_edit"]["before_sha256"] = "0" * 64
    authority_path.write_bytes(canonical_bytes(authority))
    with pytest.raises(RootHeaderAuthorityError, match="root_edit"):
        validate_root_header_authority(authority_path)

    inputs = _make_inputs(tmp_path / "mount")
    inputs["provenance"]["true_path_bind"]["mount_argv"][2] = "/wrong/header"  # type: ignore[index]
    with pytest.raises(RootHeaderAuthorityError, match="mount --bind"):
        _build(inputs)

    inputs = _make_inputs(tmp_path / "diff")
    inputs["edit_diff"].write_text("--- wrong\n+++ wrong\n@@\n-old\n+new\n", encoding="utf-8")  # type: ignore[union-attr]
    with pytest.raises(RootHeaderAuthorityError, match="exact before header"):
        _build(inputs)

    inputs = _make_inputs(tmp_path / "dirty")
    (inputs["source_root"] / "untracked").write_text("dirty\n", encoding="utf-8")  # type: ignore[operator]
    inputs["output"] = tmp_path / "dirty-authority"
    with pytest.raises(RootHeaderAuthorityError, match="not clean"):
        _build(inputs)

    inputs = _make_inputs(tmp_path / "wrong-head")
    inputs["output"] = tmp_path / "wrong-head-authority"
    inputs["source_commit"] = "0" * 40
    with pytest.raises(RootHeaderAuthorityError, match="expected"):
        _build(inputs)

    inputs = _make_inputs(tmp_path / "law")
    inputs["output"] = tmp_path / "law-authority"
    with pytest.raises(RootHeaderAuthorityError, match="body_law"):
        build_root_header_authority(
            output_root=inputs["output"],  # type: ignore[arg-type]
            source_root=inputs["source_root"],  # type: ignore[arg-type]
            source_commit=inputs["source_commit"],  # type: ignore[arg-type]
            root_include=str(inputs["header_before"]),
            header_before=inputs["header_before"],  # type: ignore[arg-type]
            header_after=inputs["header_after"],  # type: ignore[arg-type]
            edit_diff=inputs["edit_diff"],  # type: ignore[arg-type]
            trace_path=inputs["trace"],  # type: ignore[arg-type]
            compile_commands_path=inputs["compile_commands"],  # type: ignore[arg-type]
            turn_a_manifest=inputs["a_manifest"],  # type: ignore[arg-type]
            turn_b_manifest=inputs["b_manifest"],  # type: ignore[arg-type]
            compile_validation=inputs["compile_validation"],  # type: ignore[arg-type]
            provenance=inputs["provenance"],  # type: ignore[arg-type]
            expected_affected=760,
            expected_unaffected=240,
        )

    inputs = _make_inputs(tmp_path / "forged-receipt")
    _build(inputs)
    authority_path = inputs["output"] / "authority.json"  # type: ignore[operator]
    authority = load_json(authority_path)
    receipt_path = inputs["output"] / "compile-validation.json"  # type: ignore[operator]
    receipt = load_json(receipt_path)
    receipt["rows"][0]["argv"] = ["/forged/compiler", "--success"]
    receipt_path.write_bytes(canonical_bytes(receipt))
    authority["compile_validation"]["sha256"] = _digest(receipt_path)
    authority_path.write_bytes(canonical_bytes(authority))
    with pytest.raises(RootHeaderAuthorityError, match="reconstructed argv"):
        validate_root_header_authority(authority_path)

    inputs = _make_inputs(tmp_path / "extra")
    _build(inputs)
    authority_path = inputs["output"] / "authority.json"  # type: ignore[operator]
    authority = load_json(authority_path)
    authority["generation"]["unexpected"] = True
    authority_path.write_bytes(canonical_bytes(authority))
    with pytest.raises(RootHeaderAuthorityError, match="generation"):
        validate_root_header_authority(authority_path)
