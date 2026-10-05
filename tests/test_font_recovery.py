"""Font source/variant identity contracts. The OpenType writer and the PFR1
decoder are tested with the converter (compiler/src/convert/cff.rs, pfr.rs)."""

import pytest

from director64.full_assets import font_inputs


def test_manifested_font_variants_include_size_in_conversion_identity(tmp_path):
    import hashlib

    model_dir = tmp_path / "director"
    source = model_dir / "fonts/font.otf"
    source.parent.mkdir(parents=True)
    source.write_bytes(b"OTTO recovered font")
    font = {
        "number": 1,
        "asset": "fonts/font.otf",
        "sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "variants": [
            {"size": 12, "asset": "fonts/f1-12.font64"},
            {"size": 15, "asset": "fonts/f1-15.font64"},
        ],
    }
    result = font_inputs(model_dir, {"fonts": [font]})
    a, b = result["fonts/f1-12.font64"], result["fonts/f1-15.font64"]
    assert a["source_sha256"] == b["source_sha256"]
    assert a["conversion"]["args"] == ["--size", "12", "--range", "all"]
    assert a["conversion"] != b["conversion"]
    source.write_bytes(b"modified")
    with pytest.raises(ValueError, match="modified"):
        font_inputs(model_dir, {"fonts": [font]})


def test_source_text_styles_generate_deduplicated_d8_metadata(tmp_path):
    from director64.director import generate

    initial = {
        "fontName": "Pettson *",
        "fontId": 1,
        "size": 12,
        "align": 1,
        "ascent": 11,
        "descent": 6,
        "leading": 0,
        "lineHeight": 17,
        "color": 0,
    }
    insertion = {**initial, "size": 15, "ascent": 14, "descent": 7, "lineHeight": 21}
    generate(
        {
            "problems": [],
            "movies": [
                {
                    "name": "LO.DXR",
                    "id": 1,
                    "tempo": 30,
                    "directorVersion": 800,
                    "casts": [{"name": "Internal", "number": 1, "file": "LO.DXR"}],
                    "members": [
                        {
                            "cast": 1,
                            "number": n,
                            "type": 3,
                            "text": "Ä",
                            "textStyle": initial,
                            "textInsertStyle": insertion,
                        }
                        for n in [1, 2]
                    ],
                }
            ],
        },
        tmp_path,
    )
    text = (tmp_path / "lo_dxr_scene.c").read_text()
    assert text.count("static const dg_text_style_t") == 2
    assert text.count("&text_style_0,&text_style_1") == 2
    assert '.font_name="Pettson *",.font_id=1,.size=12,.align=1' in text
    assert ".ascent=14,.descent=7,.leading=0,.line_height=21" in text
