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
