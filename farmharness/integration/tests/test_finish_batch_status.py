from __future__ import annotations

import shlex
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]


def _finish_batch_function() -> str:
    source = (ROOT / "unittests/p50compilee2e-run.sh").read_text(encoding="utf-8")
    start = source.index("    finish_batch() {")
    end = source.index("    publish_gate_marker() {", start)
    return source[start:end]


def _run_extracted_finish_batch(tmp_path: Path, python_stub: str) -> subprocess.CompletedProcess[str]:
    active_batch = tmp_path / "empty-batch.tsv"
    active_batch.write_text("", encoding="utf-8")
    prelude = f"""\
set -eu
work={shlex.quote(str(tmp_path))}
run_label=regression
job_entries=
job_pids=
batch_job_pids=
batch_allow_failures=0
ordinal=0
batch_start_ns=1
batch_end_ns=2
external_mode=0
emit_rows=0
active_batch_file={shlex.quote(str(active_batch))}
relationship_count=1
slots_per_f=30
real_scheduler_restart_w30=0
w30_f_loss=0
compiler_loss_w30=0
suite=C1F1/100000
python3() {{ {python_stub}; }}
"""
    script = prelude + _finish_batch_function() + "\nfinish_batch || exit 23\n"
    return subprocess.run(
        ("/bin/sh", "-c", script),
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )


def test_finish_batch_propagates_metrics_failure_even_under_or_list(tmp_path: Path) -> None:
    failed = _run_extracted_finish_batch(tmp_path, "cat >/dev/null; return 17")
    assert failed.returncode == 23, failed.stdout + failed.stderr
    assert "FAIL: batch metrics validation failed (regression)" in failed.stderr
    assert "S8_BATCH_COMPLETE" not in failed.stdout


def test_finish_batch_reports_complete_after_successful_metrics(tmp_path: Path) -> None:
    passed = _run_extracted_finish_batch(
        tmp_path,
        "cat >/dev/null; printf 'makespan_ns=1\\n'; return 0",
    )
    assert passed.returncode == 0, passed.stdout + passed.stderr
    assert "S8_BATCH_COMPLETE run=regression count=0" in passed.stdout
