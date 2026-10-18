import hashlib
import json
from pathlib import Path

from director64 import full_assets
from director64.project import work_dir


def test_event_audio_uses_exact_pcm_while_director_audio_keeps_its_codec(tmp_path, monkeypatch):
    directory = tmp_path / work_dir() / "director"
    (directory / "wav").mkdir(parents=True)
    names = []
    for raw in [b"synthetic event PCM", b"synthetic ordinary PCM"]:
        name = hashlib.sha256(raw).hexdigest()[:24]
        (directory / "wav" / f"{name}.wav").write_bytes(raw)
        names.append(f"{name}.wav64")
    (directory / "model.json").write_text(
        json.dumps(
            {
                "problems": [],
                "fonts": [],
                "movies": [
                    {
                        "id": 1,
                        "name": "TEST",
                        "members": [
                            {"flashRuntime": {"sounds": [{"asset": names[0]}]}},
                            {"asset": names[1]},
                        ],
                    }
                ],
            }
        )
    )
    monkeypatch.setattr(full_assets, "source_policy", lambda root: {"wav_resample_hz": 22050})
    monkeypatch.setattr(full_assets, "working_sets", lambda root: {})
    monkeypatch.setattr(
        full_assets, "inspect_image", lambda *args: {"Id": "test-sdk", "Config": {"Labels": {}}}
    )
    commands = []

    def convert(command, **kwargs):
        shell = command[command.index("-c") + 1]
        for part in shell.split(" && "):
            words = part.split()
            if "audioconv64" not in words:
                continue
            listing = tmp_path / words[words.index("-a") + 1]
            output = tmp_path / words[words.index("-o") + 1]
            output.mkdir(parents=True, exist_ok=True)
            for source in listing.read_bytes().split(b"\0"):
                if source:
                    source = Path(source.decode())
                    commands.append((source.with_suffix(".wav64").name, words))
                    (output / source.with_suffix(".wav64").name).write_bytes(
                        b"converted " + (tmp_path / source).read_bytes()
                    )

    monkeypatch.setattr(full_assets.subprocess, "run", convert)
    full_assets.pack(tmp_path, "test")
    by_name = dict(commands)
    assert by_name[names[0]][by_name[names[0]].index("--wav-compress") + 1] == "0"
    assert "--wav-resample" not in by_name[names[0]]
    assert by_name[names[1]][by_name[names[1]].index("--wav-compress") + 1] == "2"
    assert by_name[names[1]][by_name[names[1]].index("--wav-resample") + 1] == "22050"
    manifest = json.loads((directory / "packed.json").read_text())
    assert manifest["files"][f"audio/{names[0]}"]["conversion"]["args"] == ["--wav-compress", "0"]
    full_assets.pack(tmp_path, "test")
    assert len(commands) == 2
