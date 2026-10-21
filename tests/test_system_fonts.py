"""Original-font transport must preserve and verify the entire installer."""

import hashlib
import io

import pytest

from director64 import system_fonts


def resource(monkeypatch):
    raw = b"MZ synthetic installer, never executed"
    digest = hashlib.sha256(raw).hexdigest()
    monkeypatch.setattr(
        system_fonts,
        "INSTALLERS",
        {
            digest: {"file": "example32.exe", "url": "https://example.invalid/example32.exe"},
        },
    )
    return raw, digest


def test_download_and_native_media_transport_keep_original_verified_bytes(tmp_path, monkeypatch):
    raw, digest = resource(monkeypatch)
    calls = []

    def download(request, timeout):
        calls.append(request.full_url)
        return io.BytesIO(raw)

    monkeypatch.setattr(system_fonts.urllib.request, "urlopen", download)
    path = "director64-system-fonts/example32.exe"
    policy = {"system_font_resources": [{"path": path, "sha256": digest}]}
    media = tmp_path / "extracted/DATA"
    system_fonts.prepare(tmp_path, media, policy)
    assert (media / path).read_bytes() == raw
    assert system_fonts.installer(tmp_path, digest).name == "example32.exe"
    assert len(calls) == 1
    (media / path).write_bytes(b"corrupt")
    with pytest.raises(ValueError, match="resource hash mismatch"):
        system_fonts.prepare(tmp_path, media, policy)


def test_corrupt_download_and_cache_fail_before_conversion(tmp_path, monkeypatch):
    raw, digest = resource(monkeypatch)
    monkeypatch.setattr(
        system_fonts.urllib.request, "urlopen", lambda *a, **kw: io.BytesIO(b"wrong")
    )
    with pytest.raises(ValueError, match="downloaded.*hash mismatch"):
        system_fonts.installer(tmp_path, digest)
    target = tmp_path / "build/system-fonts" / digest / "example32.exe"
    assert not target.exists()
    target.write_bytes(raw + b"corrupt")
    with pytest.raises(ValueError, match="cached.*hash mismatch"):
        system_fonts.installer(tmp_path, digest)


def test_disc_font_outside_game_folder_preserves_original_bytes(tmp_path, monkeypatch):
    raw = b"synthetic original font from the disc's installer folder"
    digest = hashlib.sha256(raw).hexdigest()
    disc = tmp_path / "extracted"
    original = disc / "FONTS/ORIGINAL.TTF"
    original.parent.mkdir(parents=True)
    original.write_bytes(raw)
    policy = {"system_font_resources": [{
        "path": "director64-system-fonts/original.ttf",
        "disc_path": "FONTS/ORIGINAL.TTF", "sha256": digest,
    }]}
    monkeypatch.setattr(system_fonts, "installer", lambda *a: pytest.fail("disc font downloaded"))
    media = disc / "GAME"
    system_fonts.prepare(tmp_path, media, policy, disc=disc)
    target = media / policy["system_font_resources"][0]["path"]
    assert target.read_bytes() == original.read_bytes() == raw
    target.unlink()
    original.write_bytes(b"corrupt")
    with pytest.raises(ValueError, match="disc resource hash mismatch"):
        system_fonts.prepare(tmp_path, media, policy, disc=disc)
    assert not target.exists()
    with pytest.raises(ValueError, match="disc extraction is missing"):
        system_fonts.prepare(tmp_path, media, policy)


@pytest.mark.parametrize(
    "path", ["../font.exe", "/font.exe", "a/../../font.exe", "C:/font.exe", "a\\font.exe", ""]
)
def test_native_resources_cannot_escape_media(tmp_path, path):
    with pytest.raises(ValueError, match="media-relative"):
        system_fonts.prepare(
            tmp_path,
            tmp_path / "media",
            {
                "system_font_resources": [{"path": path, "sha256": "unused"}],
            },
        )


@pytest.mark.parametrize("path", ["../font.ttf", "/font.ttf", "C:/font.ttf", "a\\font.ttf", ""])
def test_disc_font_source_cannot_escape_extraction(tmp_path, path):
    with pytest.raises(ValueError, match="media-relative"):
        system_fonts.prepare(tmp_path, tmp_path / "media", {"system_font_resources": [{
            "path": "fonts/original.ttf", "disc_path": path, "sha256": "unused",
        }]}, disc=tmp_path / "extracted")
