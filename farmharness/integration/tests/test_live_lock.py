from __future__ import annotations

import os
from pathlib import Path

import pytest

from farmharness.integration.live_lock import LiveRunLockError, live_run_lock


def test_live_lock_is_exclusive_and_released(tmp_path: Path) -> None:
    path = tmp_path / "live.lock"
    with live_run_lock(path):
        with pytest.raises(LiveRunLockError, match="another process owns"):
            with live_run_lock(path):
                pass
    with live_run_lock(path):
        assert path.is_file()
        assert path.stat().st_uid == os.geteuid()


def test_live_lock_refuses_relative_and_symlink_paths(tmp_path: Path) -> None:
    with pytest.raises(LiveRunLockError, match="absolute"):
        with live_run_lock(Path("relative.lock")):
            pass

    target = tmp_path / "target"
    target.write_text("", encoding="ascii")
    link = tmp_path / "link"
    link.symlink_to(target)
    with pytest.raises(LiveRunLockError, match="cannot open"):
        with live_run_lock(link):
            pass


def test_live_lock_refuses_hardlinked_inode(tmp_path: Path) -> None:
    path = tmp_path / "live.lock"
    alias = tmp_path / "alias"
    path.write_text("", encoding="ascii")
    os.link(path, alias)
    with pytest.raises(LiveRunLockError, match="not one private"):
        with live_run_lock(path):
            pass
