"""Source-free checks of the original/port image comparison (director64 compare)."""

import numpy as np

from director64.compare import BLOCK, best_offset, closest, overlaps, panel, regions

H, W = 96, 128


def scene(t: int, *, sheet: bool = True, phase: int = 0) -> np.ndarray:
    """A background, a bar moving with time and a static white 'sheet'."""
    frame = np.zeros((H, W, 3), np.uint8)
    frame[:, :] = (30, 40, 50)
    x = (t * 6 + phase) % (W - 16)
    frame[10:20, x:x + 16] = (200, 120, 40)
    if sheet:
        frame[60:84, 90:110] = (255, 255, 255)
    return frame


def recording(count: int, shift: int = 0, **kwargs) -> np.ndarray:
    return np.stack([scene(t + shift, **kwargs) for t in range(count)])


def persistent(port: np.ndarray, original: np.ndarray, offset: int, reach: int) -> np.ndarray:
    counts = np.zeros((H // BLOCK, W // BLOCK), np.int32)
    frames = 0
    for i in range(len(port)):
        lo, hi = i + offset - reach, i + offset + reach + 1
        if lo < 0 or hi > len(original):
            continue
        difference, _ = closest(port[i], original[lo:hi])
        bad = difference > 48
        counts += bad.reshape(H // BLOCK, BLOCK, W // BLOCK, BLOCK).mean(axis=(1, 3)) > 0.5
        frames += 1
    return counts >= 0.8 * frames


def test_offset_is_recovered():
    original = recording(60)
    port = recording(50, shift=4)  # the port starts 4 frames into the original
    assert best_offset(port, original, 0, range(-3, 10)) == 4


def test_phase_jitter_and_motion_are_not_differences():
    original = recording(60)
    port = recording(50, shift=4)
    # A one-frame lag inside the window must not be reported either.
    lagging = recording(50, shift=3)
    assert not persistent(port, original, 4, 2).any()
    assert not persistent(lagging, original, 4, 2).any()


def test_missing_static_content_is_one_region():
    original = recording(60)
    port = recording(50, shift=4, sheet=False)
    found = regions(persistent(port, original, 4, 2), minimum=4)
    assert len(found) == 1
    left, top, right, bottom, blocks = found[0]
    # The sheet spans 90..110 x 60..84; half-covered edge blocks do not count.
    assert 88 <= left <= 92 and top == 60 and 108 <= right <= 112 and bottom == 84
    assert blocks * BLOCK * BLOCK >= 16 * 24
    assert overlaps([95, 70, 100, 75], found[0]) and not overlaps([0, 0, 10, 10], found[0])


def test_panel_shapes():
    frame = scene(0)
    mask = np.zeros((H, W), bool)
    assert panel(frame, frame, mask, None).shape == (H, W * 3 + 12, 3)
    crop = panel(frame, frame, mask, (60, 40, 80, 60, 4))
    # A 20-pixel box with 40-pixel margins, clipped to the 96-pixel stage.
    assert crop.shape == (96, 3 * 100 + 12, 3)
