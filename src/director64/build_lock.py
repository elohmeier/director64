"""Serialize callers that convert the same game/edition into shared output."""

from __future__ import annotations

import fcntl
import math
import os
import sys
import time
from contextlib import contextmanager

from .project import GameSpec


@contextmanager
def conversion(game: GameSpec, *, timeout: float = 900):
    """Hold a stable-inode process lock; never unlink it while waiters exist."""
    if not math.isfinite(timeout) or timeout < 0:
        raise ValueError("conversion lock timeout must be finite and nonnegative")
    game.work.mkdir(parents=True, exist_ok=True)
    path = game.work / ".conversion.lock"
    deadline, announced = time.monotonic() + timeout, False
    with path.open("a+") as lock:
        while True:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if not announced:
                    print(f"Waiting for another conversion of {game.slug} ({path})",
                          file=sys.stderr)
                    announced = True
                if time.monotonic() >= deadline:
                    raise ValueError(f"another conversion still holds {path}; "
                                     f"waited {timeout:g} seconds") from None
                time.sleep(min(0.05, max(0, deadline - time.monotonic())))
        try:
            lock.seek(0)
            lock.truncate()
            lock.write(f"pid={os.getpid()}\n")
            lock.flush()
            yield
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)
