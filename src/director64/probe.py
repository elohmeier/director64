"""Reading the web probe's line protocol (platforms/web/node/probe.mjs)."""

from __future__ import annotations

from typing import IO


def state_line(stdout: IO[str]) -> str:
    """The next state line, past the movie-entry lines ("ENTER ...") the probe
    prints between them; "" once the probe has exited."""
    while line := stdout.readline():
        if line.startswith("{"):
            return line
    return ""
