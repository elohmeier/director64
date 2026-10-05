from director64 import image_budget


def image(level, size, length, weight):
    return {"level": level, "bytes": size, "length": length, "weight": weight}


def test_estimates_follow_the_measured_ratios():
    assert image_budget.estimated_bytes(1000, 3, 2) == 1260
    assert image_budget.estimated_bytes(1260, 2, 3) == 1000
    assert image_budget.estimated_bytes(1000, 3, 1) == 1630
    assert image_budget.seconds_saved(1_000_000, 3, 2) > image_budget.seconds_saved(1_000_000, 2, 1)


def test_promotes_the_best_seconds_per_byte_first_within_the_budget():
    images = {
        # Packs well and is drawn everywhere: the first promotion.
        "flat.fdi": image(3, 1000, 100_000, 4),
        # Packs badly: many bytes per second saved.
        "noise.fdi": image(3, 50_000, 100_000, 4),
        # Never drawn: never promoted.
        "unseen.fdi": image(3, 100, 100_000, 0),
        # Already at level 2, drawn: only a level-1 candidate.
        "big.fdi": image(2, 10_000, 200_000, 1),
    }
    total = sum(i["bytes"] for i in images.values())
    assert image_budget.solve(images, total) == {}
    assert image_budget.solve(images, total + 300) == {"flat.fdi": 2}
    result = image_budget.solve(images, total + 300 + 13_000)
    assert result == {"flat.fdi": 2, "noise.fdi": 2}
    everything = image_budget.solve(images, total * 3)
    assert {everything[n] for n in ("flat.fdi", "noise.fdi", "big.fdi")} == {1}
    assert "unseen.fdi" not in everything


def test_level_one_is_taken_only_after_every_level_two_promotion():
    images = {
        "a.fdi": image(3, 1000, 100_000, 1),
        "b.fdi": image(2, 1000, 100_000, 5),
    }
    # Budget for one step: the level-3 image moves first even though the
    # level-2 image is drawn more often.
    result = image_budget.solve(images, 2000 + 300)
    assert result == {"a.fdi": 2}
