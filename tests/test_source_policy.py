"""Transport exact ISO aliases into the conversion without changing source IDs."""

import json
from types import SimpleNamespace

import pytest

from director64 import convert, iso


@pytest.mark.parametrize("media_root", ["LONGROOT/DATA", "/LONGROOT/DATA/"])
def test_prepared_policy_scopes_joliet_names_to_media_root(tmp_path, monkeypatch, media_root):
    host = tmp_path / "host"
    host.mkdir()
    original = {"director_version": 850, "source_path_aliases": {"stale": "old"}}
    (host / "source-policy.json").write_text(json.dumps(original))
    media = tmp_path / "disc.iso"
    media.write_bytes(b"synthetic inventory stub")
    work = tmp_path / "work"
    work.mkdir()
    aliases = {
        "Long Root/Data/Levelwaage.dir": "LONGROOT/DATA/LEVELWAA.DIR",
        "Long Root/Data/Tamagotchi/00_a_Begruessung.dcr": "LONGROOT/DATA/TAMAGOTC/00ABEGRU.DCR",
        "Other/Outside.dir": "OTHER/OUTSIDE.DIR",
    }
    monkeypatch.setattr(iso, "inventory_report", lambda _: {"aliases": aliases})
    game = SimpleNamespace(
        directory=tmp_path, media=media, work=work, source={"kind": "iso9660"},
        data={"port": {"media_root": media_root}},
    )
    policy = json.loads(convert.prepared_policy(game, work, work / "extracted").read_text())
    assert policy == {"director_version": 850, "source_path_aliases": {
        "Levelwaage.dir": "LEVELWAA.DIR",
        "Tamagotchi/00_a_Begruessung.dcr": "TAMAGOTC/00ABEGRU.DCR",
    }}
    assert json.loads((host / "source-policy.json").read_text()) == original
