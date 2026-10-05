import io
import zipfile

import pytest
from test_iso import image, record

from director64.iso import file_sha256, inventory
from director64.media import extract_zip, zip_entries


def test_associated_iso_streams_are_separate():
    data = image()
    start = 20 * 2048 + 68
    ordinary = record(b"TEST.TXT;1", 21, 4)
    associated = record(b"TEST.TXT;1", 22, 4, flags=4)
    data[start : start + len(ordinary) + len(associated)] = ordinary + associated
    assert {e.path for e in inventory(io.BytesIO(data))} == {"TEST.TXT", "__associated__/TEST.TXT"}


@pytest.mark.parametrize("name", ["../escape", "/escape", "C:/escape", "a/../b", "a\\b"])
def test_zip_rejects_unsafe_paths(name):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as z:
        z.writestr(name, "test")
    with zipfile.ZipFile(buffer) as z, pytest.raises(ValueError, match="unsafe"):
        zip_entries(z)


def test_zip_preserves_roots_and_verifies_cached_files(tmp_path):
    source = tmp_path / "fixture.zip"
    with zipfile.ZipFile(source, "w") as z:
        z.writestr("Game/MOVIES/SCENE.DXR", b"synthetic")
    digest = file_sha256(source)
    output = tmp_path / "output"
    extract_zip(source, output, digest)
    extract_zip(source, output, digest)
    (output / "Game/MOVIES/SCENE.DXR").write_bytes(b"changed")
    with pytest.raises(ValueError, match="differs"):
        extract_zip(source, output, digest)
