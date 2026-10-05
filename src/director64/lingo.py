"""Strict source AST for ahead-of-time compilation of recovered Lingo scripts.

The parser is the Rust converter's (compiler/src/lingo.rs), the one the
browser importer runs too; this module is its host-side interface for the
pipeline, the recovery report and the tests. Unsupported syntax is an error
with movie/member/line identity, not an ignored statement.
"""

from __future__ import annotations

import argparse
import atexit
import json
import subprocess
import threading
from pathlib import Path


class LingoError(ValueError):
    pass


def _binary() -> Path:
    from .aot import generator_command, repository_root

    return Path(generator_command(repository_root())[0])


class _Server:
    """One long-lived `director64-aot lingo-server` per interpreter."""

    def __init__(self):
        self.process = None
        self.lock = threading.Lock()

    def close(self):
        if self.process is not None:
            self.process.stdin.close()
            self.process.wait(timeout=10)
            self.process.stdout.close()
            self.process = None

    def call(self, request: dict):
        with self.lock:
            if self.process is None or self.process.poll() is not None:
                self.process = subprocess.Popen(
                    [_binary(), "lingo-server"],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    text=True,
                    encoding="utf-8",
                )
                atexit.register(self.close)
            self.process.stdin.write(json.dumps(request) + "\n")
            self.process.stdin.flush()
            line = self.process.stdout.readline()
        if not line:
            raise LingoError("the Lingo parser stopped")
        reply = json.loads(line)
        if "error" in reply:
            raise LingoError(reply["error"])
        return reply["ok"]


_server = _Server()


def expression(text: str) -> list:
    return _server.call({"expression": text})


class Statements:
    """A statement block from (line, text) pairs."""

    def __init__(self, lines: list[tuple[int, str]]):
        self.lines = lines

    def block(self) -> list[dict]:
        return _server.call({"block": [[line, text] for line, text in self.lines]})


def parse_movie(source: str, movie: str) -> list[dict]:
    return _server.call({"movie": [source, movie]})


def walk(value):
    if isinstance(value, dict):
        for child in value.values():
            yield from walk(child)
    elif isinstance(value, list):
        yield value
        for child in value:
            yield from walk(child)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    completed = subprocess.run(
        [_binary(), "lingo", args.directory, args.output], capture_output=True, text=True
    )
    if completed.returncode:
        raise LingoError(completed.stderr.strip().removeprefix("director64-aot: "))
    print(completed.stdout.strip())


if __name__ == "__main__":
    main()
