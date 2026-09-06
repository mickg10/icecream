"""Host-wide serialization for every live controlled-farm cell."""

from __future__ import annotations

import contextlib
import errno
import fcntl
import os
import stat
from pathlib import Path
from typing import Iterator


# This path is a compatibility contract with s8_campaign_driver.py and its
# protected launcher.  It is intentionally host-global and does not follow
# TMPDIR into a per-run namespace.
LIVE_LOCK_PATH = Path("/tmp/icecream-s8-live-run.lock")


class LiveRunLockError(RuntimeError):
    """A live cell cannot exclusively own the controlled farm."""


def _open_lock(path: Path) -> int:
    if not path.is_absolute():
        raise LiveRunLockError("live farm lock path must be absolute")
    for ancestor in path.parents:
        if ancestor.is_symlink():
            raise LiveRunLockError("live farm lock has a symlink ancestor")
    flags = os.O_RDWR | os.O_CREAT | os.O_CLOEXEC
    flags |= getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags, 0o600)
    except OSError as exc:
        raise LiveRunLockError(f"cannot open live farm lock: {exc}") from exc
    try:
        observed = os.fstat(descriptor)
        if (
            not stat.S_ISREG(observed.st_mode)
            or observed.st_nlink != 1
            or observed.st_uid != os.geteuid()
        ):
            raise LiveRunLockError(
                "live farm lock is not one private, owner-controlled regular file"
            )
        named = path.stat(follow_symlinks=False)
        if (named.st_dev, named.st_ino) != (observed.st_dev, observed.st_ino):
            raise LiveRunLockError("live farm lock pathname changed while opening")
        return descriptor
    except BaseException:
        os.close(descriptor)
        raise


@contextlib.contextmanager
def live_run_lock(path: Path = LIVE_LOCK_PATH) -> Iterator[None]:
    """Own the S8-compatible live slot, refusing rather than waiting."""

    descriptor = _open_lock(Path(path))
    locked = False
    try:
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            locked = True
        except OSError as exc:
            if exc.errno in (errno.EACCES, errno.EAGAIN):
                raise LiveRunLockError("another process owns the live farm lock") from exc
            raise LiveRunLockError(f"cannot acquire live farm lock: {exc}") from exc
        observed = os.fstat(descriptor)
        named = Path(path).stat(follow_symlinks=False)
        if (named.st_dev, named.st_ino) != (observed.st_dev, observed.st_ino):
            raise LiveRunLockError("live farm lock pathname changed after acquisition")
        yield
    finally:
        if locked:
            fcntl.flock(descriptor, fcntl.LOCK_UN)
        os.close(descriptor)
