import pytest
from director64_findus_workshop.full_release import matches_release


def test_release_requires_identical_engine_assets_overlays_and_sdk():
    source = {"engine.c": "source-hash"}
    rom = {"dfs_sha256": "dfs-hash", "toolchain": {"image": "sdk-hash"}}
    snapshot = dict(rom, status="passing", source_sha256=source)
    assert matches_release(snapshot, source, rom)


@pytest.mark.parametrize(
    "change",
    [
        {"source_sha256": {}},
        {"source_sha256": {"engine.c": "stale"}},
        {"dfs_sha256": "stale"},
        {"toolchain": {"image": "different-sdk"}},
        {"status": "failed"},
    ],
)
def test_release_rejects_stale_or_failed_evidence(change):
    source = {"engine.c": "source-hash"}
    rom = {"dfs_sha256": "dfs-hash", "toolchain": {"image": "sdk-hash"}}
    snapshot = dict(rom, status="passing", source_sha256=source) | change
    assert not matches_release(snapshot, source, rom)
