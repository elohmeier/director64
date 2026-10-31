"""Independent observations must expose visible and save-content differences."""

import os
import sys

from director64.parity import difference, normalized, run_episode


def test_normalization_keeps_case_in_displayed_text_and_save_files():
    state = {"globals": {"gname": "Anna", "glist": '[#Name:"Anna"]'},
             "saves": {"player.txt": "Anna"}, "frame_sha256": "ABC", "heap_bytes": 1}
    assert normalized(state) == {k: v for k, v in state.items() if k != "heap_bytes"}
    for field, value in (("globals", {"gname": "ANNA", "glist": '[#Name:"Anna"]'}),
                         ("saves", {"player.txt": "ANNA"}), ("frame_sha256", "abc")):
        changed = state | {field: value}
        assert normalized(changed) != normalized(state)
        assert field in difference(state, changed)


def probe_file(tmp_path, name, *, label="Anna", fail=False):
    path = tmp_path / name
    state = {"movie": "START", "frame": 1, "globals": {"label": label}, "sprites": []}
    path.write_text(f"""#!{sys.executable}
import json, sys
state = {state!r}
print(json.dumps(state), flush=True)
for line in sys.stdin:
    if {fail!r}:
        state['error'] = 'synthetic failure'
    print(json.dumps(state), flush=True)
    if {fail!r}: sys.exit(1)
""")
    path.chmod(0o755)
    return path


def test_parity_compares_the_initial_state_before_input(tmp_path):
    a = probe_file(tmp_path, "a", label="Anna")
    b = probe_file(tmp_path, "b", label="ANNA")
    report = run_episode(7, ["START"], 2, {"pad": False}, a, b, tmp_path, dict(os.environ))
    assert report["divergence"]["command"] == "boot"
    assert "globals.label" in report["divergence"]["diff"]
    assert (tmp_path / "7-commands.txt").is_file()


def test_a_failing_command_is_replayed_without_a_zip_length_error(tmp_path):
    a = probe_file(tmp_path, "a", fail=True)
    b = probe_file(tmp_path, "b", fail=True)
    report = run_episode(7, ["START"], 2, {"pad": False}, a, b, tmp_path, dict(os.environ))
    assert report["outcome"]["kind"] == "script-error"
    assert report["divergence"] is None
    assert len((tmp_path / "7-commands.txt").read_text().splitlines()) == 1
