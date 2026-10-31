"""A second real converter process must not remove a running converter's output."""

import os
import select
import subprocess
import sys
from pathlib import Path

import pytest

WORKER = """
import sys
from pathlib import Path
from types import SimpleNamespace
from director64.build_lock import conversion
game = SimpleNamespace(work=Path(sys.argv[1]), slug='synthetic-game')
print('waiting', flush=True)
with conversion(game, timeout=float(sys.argv[2])):
    print('acquired', flush=True)
    sys.stdin.readline()
"""

ANDROID_READER = """
import sys
from pathlib import Path
from types import SimpleNamespace
from director64 import android
game = SimpleNamespace(work=Path(sys.argv[1]), slug='synthetic-game')
def snapshot(*args, **kwargs):
    path = game.work / 'converted-data'
    path.write_text('complete-version')
    print('converted', flush=True)
    sys.stdin.readline()
    assert path.read_text() == 'complete-version'
    print('snapshot-complete', flush=True)
    return path
android._build = snapshot
android.build(game, 1, ['x86_64'], False)
"""

SNAPSHOT_WRITER = WORKER.replace(
    "    print('acquired', flush=True)",
    "    (game.work / 'converted-data').write_text('writer-version')\n"
    "    print('acquired', flush=True)",
)


def line(process, timeout=5):
    assert select.select([process.stdout], [], [], timeout)[0], "lock worker did not respond"
    return process.stdout.readline().decode().strip()


def worker(path, timeout=5, script=WORKER):
    source = str(Path(__file__).parents[1] / "src")
    return subprocess.Popen([sys.executable, "-u", "-c", script, str(path), str(timeout)],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            bufsize=0, env=os.environ | {"PYTHONPATH": source})


def test_two_processes_serialize_the_same_output_and_release_after_exit(tmp_path):
    first, second = worker(tmp_path), None
    try:
        assert line(first) == "waiting"
        assert line(first) == "acquired"
        second = worker(tmp_path)
        assert line(second) == "waiting"
        assert not select.select([second.stdout], [], [], 0.2)[0]
        assert second.poll() is None
        first.stdin.write(b"done\n")
        first.stdin.flush()
        assert first.wait(timeout=5) == 0
        assert line(second) == "acquired"
        second.stdin.write(b"done\n")
        second.stdin.flush()
        assert second.wait(timeout=5) == 0
    finally:
        for child in (first, second):
            if child is not None and child.poll() is None:
                child.kill()
                child.wait(timeout=5)


def test_competing_process_times_out_without_entering_conversion(tmp_path):
    first, second = worker(tmp_path), None
    try:
        assert line(first) == "waiting"
        assert line(first) == "acquired"
        second = worker(tmp_path, timeout=0.1)
        assert line(second) == "waiting"
        assert second.wait(timeout=5) != 0
        assert second.stdout.read() == b""
        assert b"another conversion still holds" in second.stderr.read()
    finally:
        for child in (first, second):
            if child is not None and child.poll() is None:
                child.kill()
                child.wait(timeout=5)


@pytest.mark.parametrize("timeout", [-1, float("nan"), float("inf"), float("-inf")])
def test_lock_timeout_must_be_finite_and_nonnegative(tmp_path, timeout):
    from types import SimpleNamespace

    from director64.build_lock import conversion

    with pytest.raises(ValueError, match="negative"), conversion(
            SimpleNamespace(work=tmp_path, slug="fixture"), timeout=timeout):
        pytest.fail("invalid timeout acquired a lock")


def test_android_reader_keeps_conversion_lock_until_snapshot_is_complete(tmp_path):
    reader, writer = worker(tmp_path, script=ANDROID_READER), None
    try:
        assert line(reader) == "converted"
        writer = worker(tmp_path, script=SNAPSHOT_WRITER)
        assert line(writer) == "waiting"
        assert not select.select([writer.stdout], [], [], 0.2)[0]
        assert writer.poll() is None
        assert (tmp_path / "converted-data").read_text() == "complete-version"
        reader.stdin.write(b"finish-snapshot\n")
        reader.stdin.flush()
        assert line(reader) == "snapshot-complete"
        assert reader.wait(timeout=5) == 0
        assert line(writer) == "acquired"
        assert (tmp_path / "converted-data").read_text() == "writer-version"
        writer.stdin.write(b"done\n")
        writer.stdin.flush()
        assert writer.wait(timeout=5) == 0
    finally:
        for child in (reader, writer):
            if child is not None and child.poll() is None:
                child.kill()
                child.wait(timeout=5)
