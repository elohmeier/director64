import hashlib

import pytest

from director64.mpeg_recovery import prepare


def test_padding_recovery_is_bounded_pinned_and_retains_unselected_bytes():
    source = b"\0\0\1\xba" + bytes(8) + b"damaged payload" + b"\0\0\1\xba" + bytes(8)
    first, last = 12, len(source) - 12
    expected = (
        source[:first]
        + b"\0\0\1\xbe"
        + (last - first - 6).to_bytes(2, "big")
        + b"\xff" * (last - first - 6)
        + source[last:]
    )
    spec = {
        "method": "padding-and-decoder-concealment-v1",
        "bytes": len(source),
        "sha256": hashlib.sha256(source).hexdigest(),
        "output_sha256": hashlib.sha256(expected).hexdigest(),
        "padding_ranges": [[first, last]],
    }
    result, receipt = prepare(source, spec)
    assert result == expected and not receipt["restoration_claim"]
    assert receipt["decoder_concealment"]
    with pytest.raises(ValueError, match="source differs"):
        prepare(source + b"x", spec)
    with pytest.raises(ValueError, match="hash differs"):
        prepare(source, spec | {"output_sha256": "0" * 64})
    with pytest.raises(ValueError, match="invalid packet range"):
        prepare(source, spec | {"padding_ranges": [[first, last], [first, last]]})
