"""The extension cache must not hide edits to its pinned reconstruction inputs."""

import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("changed", ["upstream.patch", "extension.commit"])
def test_changed_pin_input_rejected_before_cache_reuse(tmp_path, changed):
    task = tmp_path / ".mise-tasks/projectorrays-build"
    task.parent.mkdir()
    shutil.copyfile(ROOT / ".mise-tasks/projectorrays-build", task)
    source = ROOT / "tools/projectorrays"
    tools = tmp_path / "tools/projectorrays"
    tools.mkdir(parents=True)
    pin = json.loads((source / "pin.json").read_text())
    for name in ["upstream.patch", "extension.commit"]:
        shutil.copyfile(source / name, tools / name)
    (tools / "pin.json").write_text(json.dumps(pin))
    cache = tmp_path / "build/projectorrays" / pin["extension_commit"]
    cache.mkdir(parents=True)
    (tools / changed).write_bytes((tools / changed).read_bytes() + b"changed\n")
    run = subprocess.run([sys.executable, str(task)], capture_output=True, text=True)
    assert run.returncode != 0
    assert f"{changed} changed" in run.stderr
    assert "Traceback" not in run.stderr
    assert not (tmp_path / "build/host").exists()


def test_commit_object_and_patch_match_pins():
    source = ROOT / "tools/projectorrays"
    pin = json.loads((source / "pin.json").read_text())
    raw = (source / pin["commit_object"]).read_bytes()
    # Git's object ID hashes the type/length header and exact object content.
    object_bytes = f"commit {len(raw)}\0".encode() + raw
    assert hashlib.sha1(object_bytes).hexdigest() == pin["extension_commit"]
    assert hashlib.sha256(raw).hexdigest() == pin["commit_object_sha256"]
    patch = (source / pin["integration_patch"]).read_bytes()
    assert hashlib.sha256(patch).hexdigest() == pin["patch_sha256"]
    assert raw.startswith(f"tree {pin['extension_tree']}\n".encode())
    assert f"parent {pin['upstream_commit']}\n".encode() in raw
