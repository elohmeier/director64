"""Pack only manifested full-game resources with the pinned target SDK."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

from director64.project import work_dir

from . import image_budget, image_pack
from .toolchain import RECIPE_LABEL, container_engine, inspect_image

# Asset compression trades ROM bytes for decompression time on every cache
# miss. Level 3 (Shrinkler) packs smallest and decompresses slowest; level 1
# (LZ4) is several times faster. Large images dominate the bytes a scene
# decompresses at runtime while holding a minority of the ROM, so a game may
# buy speed where it pays best and keep maximum compression everywhere else.
# Decompressed size selects the tier, because that is what decompression
# costs. Without a policy every image stays at level 3, as before.
#
# Measured on the console: level 3 delivers 0.7-1.3 MB/s, level 2 3.4-5.3,
# level 1 6.6-7.3. Level 2 reaches the same tick delivery as level 1 on every
# port measured while costing +26% of the promoted bytes against +63%, so it
# is the level to reach for first. Level 1 also packs a fifth larger than
# level 2 on the same images, and the ROM ceiling (rom.py) is real, so no
# port uses it.
DEFAULT_IMAGE_LEVEL = 3


def image_level(raw_bytes: int, tiers: list[dict]) -> int:
    for tier in tiers:
        if raw_bytes >= tier["min_decompressed_bytes"]:
            return tier["level"]
    return DEFAULT_IMAGE_LEVEL


def source_policy(root: Path) -> dict:
    from .project import selected_game

    try:
        policy_path = selected_game().directory / "host/source-policy.json"
    except ValueError:
        return {}
    return json.loads(policy_path.read_text()) if policy_path.is_file() else {}


# A port whose sources are all 22 kHz can spend that rate or resample below it.
# The ROM ceiling is a hard 56 MiB (see rom.py), and sound is the cheapest
# place a large port can buy the megabytes an image codec needs: ULC's output
# tracks the sample count, so 16 kHz is about a quarter smaller with a loss
# that stays inside speech and effects rather than showing on screen.
def wav_resample_hz(root: Path) -> int:
    rate = source_policy(root).get("wav_resample_hz", 0)
    if rate and not (isinstance(rate, int) and 8000 <= rate <= 48000):
        raise ValueError("invalid wav_resample_hz")
    return rate


def image_codec_tiers(root: Path) -> list[dict]:
    """Descending-threshold tiers from the selected game's source policy."""
    tiers = source_policy(root).get("image_codec_tiers", [])
    for tier in tiers:
        if tier["level"] not in (1, 2, 3) or tier["min_decompressed_bytes"] < 0:
            raise ValueError("invalid image codec tier")
    if sorted(tiers, key=lambda t: -t["min_decompressed_bytes"]) != tiers:
        raise ValueError("image codec tiers must descend by threshold")
    return tiers


# Spending a bit of each stored colour channel is the cheapest ROM a large
# port can buy, and the ROM is what pays for a faster image codec. Measured on
# eight of Mucklas's full-stage backgrounds, dropping the 5-bit channels to 4
# packs 31% smaller at level 3 and 29% at level 2; on that watercolour art the
# two are indistinguishable at stage size. Neither the stored size nor the
# layout changes, so the runtime reads the result exactly as before.
def image_color_bits(root: Path) -> int:
    bits = source_policy(root).get("image_color_bits", 5)
    if bits not in (4, 5):
        raise ValueError("image_color_bits must be 4 or 5")
    return bits


# An image with at most 256 distinct colour words is stored as indices over
# its palette (FDIC, runtime/director/ink.h): a quarter or half the bytes the
# console decompresses at a scene entry, and the same drawn pixels, since the
# RDP looks each index up in the palette on its way to the framebuffer. Only
# an FDI1 image qualifies; soft-alpha and true-colour planes stay as they are.
def image_pack_budget(root: Path) -> int:
    """ROM bytes the image pack may take (`image_pack_budget_mib`), which the
    codec budget spends on promotions above the tiers; 0 leaves the tiers."""
    budget = source_policy(root).get("image_pack_budget_mib", 0)
    if isinstance(budget, bool) or not isinstance(budget, (int, float)) or budget < 0:
        raise ValueError("image_pack_budget_mib must be a non-negative number")
    return int(budget * 1024 * 1024)


def mask_pairs(model: dict) -> dict[str, set[str]]:
    """Per image asset, the mask assets of the members that carry it: a
    bitmap member's mask is the next member of its cast, named
    "<name>_mask" (the D10 mask ink pairing the console applies)."""
    pairs: dict[str, set[str]] = {}
    for movie in model["movies"]:
        members = {(m["cast"], m["number"]): m for m in movie["members"]}
        for (cast, number), member in members.items():
            if member.get("type") != 1 or not member.get("asset"):
                continue
            mask = members.get((cast, number + 1))
            masked = (
                mask is not None
                and mask.get("type") == 1
                and mask.get("asset")
                and mask.get("name", "") == member.get("name", "") + "_mask"
            )
            pairs.setdefault(member["asset"], set()).add(mask["asset"] if masked else "")
    return pairs


def baked_masks(model: dict, inks: dict[str, set[int]]) -> dict[str, str]:
    """Which image assets get their mask baked, and with which mask: the
    assets some scene draws with mask ink, whose every member carries the
    same mask. An asset a member draws unmasked as well, or with another
    ink, keeps the console's per-pixel fallback."""
    pairs = mask_pairs(model)
    return {
        asset: next(iter(masks))
        for asset, masks in pairs.items()
        if 9 in inks.get(asset, ()) and inks[asset] == {9} and len(masks) == 1 and "" not in masks
    }


def working_sets(root: Path) -> dict:
    """The port's recorded working sets (director64 working-sets): per
    movie, the (asset, ink) pairs the stage drew and the score references,
    best first. A port without the file packs every image once and no
    scene directory, so every load goes through the pack's index."""
    from .project import selected_game

    try:
        path = selected_game().directory / "host/working-sets.json"
    except ValueError:
        return {}
    return json.loads(path.read_text()).get("movies", {}) if path.is_file() else {}


def image_indexed(root: Path) -> bool:
    indexed = source_policy(root).get("image_indexed", False)
    if not isinstance(indexed, bool):
        raise ValueError("image_indexed must be true or false")
    return indexed


TEXTURE_LIMIT, TILE = 1024, 32


def plane_pixels(width: int, height: int) -> int:
    """Pixels a stored image plane holds, padding included.

    Mirrors `bitmap_plane_pixels` in runtime/director/bitmap_tiles.h and
    `plane_pixels` in compiler/src/convert/fdi.rs; an image past the RDP's
    texture limit is stored as zero-padded 32x32 tiles."""
    if width > TEXTURE_LIMIT or height > TEXTURE_LIMIT:
        return -(-width // TILE) * -(-height // TILE) * TILE * TILE
    return width * height


def quantize_colors(raw: bytes) -> bytes:
    """Reduce an FDI image's 5-bit RGB channels to 4 bits each.

    The colour plane is big-endian RRRRRGGGGGBBBBBA. Refilling each channel's
    dropped low bit from its top bit keeps the endpoints exact (0 stays 0,
    31 stays 31), which matters because the runtime compares against pure
    white for matte ink. Bits 15, 10 and 5 all move to 11, 6 and 1, so one
    shift of the whole plane does every channel of every pixel at once and
    nothing crosses a pixel boundary.
    """
    kind = raw[:4]
    if kind not in (b"FDI1", b"FDIA"):
        return raw  # FDI2 stores RGBA8888; no port ships one.
    width = int.from_bytes(raw[8:10], "big")
    height = int.from_bytes(raw[10:12], "big")
    # The colour plane is however many pixels the stored layout holds, which
    # for an image past the texture limit is its padded tile count: the shift
    # is per pixel, so it does not care about the order, only the extent.
    end = 32 + plane_pixels(width, height) * 2
    if end > len(raw):
        raise ValueError("FDI colour plane exceeds the file")
    plane = raw[32:end]
    pixels = len(plane) // 2
    keep = int("f7bd" * pixels, 16)
    tops = int("8420" * pixels, 16)
    value = int.from_bytes(plane, "big")
    value = (value & keep) | ((value & tops) >> 4)
    return raw[:32] + value.to_bytes(len(plane), "big") + raw[end:]


def index_colors(raw: bytes) -> bytes:
    """Store an FDI1 image as FDIC when its plane holds at most 256 distinct
    RGBA5551 words: four bits per index for up to 16 colours, eight
    otherwise. A word carries its alpha bit, so a colour that occurs both
    opaque and transparent (a matte's white) is two palette entries, and
    the console's per-ink rewrite of the palette matches the per-pixel one.
    Anything else, or an image with more colours, comes back unchanged."""
    if raw[:4] != b"FDI1":
        return raw
    width = int.from_bytes(raw[8:10], "big")
    height = int.from_bytes(raw[10:12], "big")
    count = plane_pixels(width, height)
    end = 32 + count * 2
    if end > len(raw):
        raise ValueError("FDI colour plane exceeds the file")
    words = struct.unpack(f">{count}H", raw[32:end])
    palette = list(dict.fromkeys(words))
    if len(palette) > 256:
        return raw
    bits = 4 if len(palette) <= 16 else 8
    index_of = {word: i for i, word in enumerate(palette)}
    header = bytearray(raw[:32])
    header[0:4] = b"FDIC"
    header[18] = bits
    header[20:22] = len(palette).to_bytes(2, "big")
    tlut = b"".join(word.to_bytes(2, "big") for word in palette)
    offset = (32 + len(tlut) + 7) & ~7
    indices = bytes(map(index_of.__getitem__, words))
    if bits == 8:
        plane = indices
    else:
        # Pairs never straddle a row: stored rows are the image's width, or
        # 32 pixels once tile-packed, and an odd linear row ends in a zero
        # nibble, as the RDP's CI4 row stride expects.
        row = TILE if (width > TEXTURE_LIMIT or height > TEXTURE_LIMIT) else width
        rows = (indices[i : i + row] for i in range(0, len(indices), row))
        plane = b"".join(
            bytes((r[i] << 4) | (r[i + 1] if i + 1 < len(r) else 0) for i in range(0, len(r), 2))
            for r in rows
        )
    header[4:8] = (offset + len(plane)).to_bytes(4, "big")
    return bytes(header) + tlut + bytes(offset - 32 - len(tlut)) + plane + raw[end:]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def font_inputs(model_dir: Path, model: dict) -> dict:
    """Every original-font variant is keyed by font bytes AND mkfont options."""
    expected = {}
    numbers = set()
    for font in model.get("fonts", []):
        number = font["number"]
        if not isinstance(number, int) or not 1 <= number <= 255 or number in numbers:
            raise ValueError("invalid or duplicate original font number")
        numbers.add(number)
        asset = Path(font["asset"])
        if asset.parent != Path("fonts") or asset.suffix not in (".otf", ".ttf"):
            raise ValueError("invalid original font source path")
        raw = model_dir / asset
        sha = digest(raw)
        if sha != font["sha256"]:
            raise ValueError(f"modified recovered original font: {raw}")
        for variant in font["variants"]:
            size = variant["size"]
            name = f"fonts/f{number}-{size}.font64"
            if not isinstance(size, int) or not 1 <= size <= 255 or variant["asset"] != name:
                raise ValueError("invalid original font size/asset")
            if name in expected:
                raise ValueError("duplicate original font variant")
            expected[name] = {
                "raw": raw,
                "source_sha256": sha,
                "conversion": {"tool": "mkfont", "args": ["--size", str(size), "--range", "all"]},
            }
    return expected


def pack(root: Path, image: str):
    inspect = inspect_image(root, image)
    image = inspect["Id"]
    revision = inspect["Config"].get("Labels", {}).get("org.director64.libdragon-commit")
    toolchain = {
        "image": inspect["Id"],
        "revision": revision,
        "recipe": inspect["Config"].get("Labels", {}).get(RECIPE_LABEL),
    }
    model_path = root / f"{work_dir()}/director/model.json"
    model = json.loads(model_path.read_text())
    if model["problems"]:
        raise ValueError("unresolved resource conversion")
    tiers = image_codec_tiers(root)
    color_bits = image_color_bits(root)
    indexed = image_indexed(root)
    resample = wav_resample_hz(root)
    audio_flags = ["--wav-compress", "2"] + (
        ["--wav-resample", str(resample)] if resample else []
    )
    expected = {}
    # A converted video is named by the movie it came from (the browser
    # importer makes its own video of the same movie under the same name);
    # its conversion receipt ties it to that source and the pinned converter.
    video_sources = {
        record["video"]: record["source_sha256"]
        for record in model.get("externalMedia", [])
        if record.get("video")
    }
    for movie in model["movies"]:
        for member in movie["members"]:
            for name in {
                member.get("asset", ""),
                member.get("videoAudio", ""),
                *member.get("filmAssets", []),
            } - {""}:
                folder = (
                    "video"
                    if name.endswith(".h264")
                    else "audio"
                    if name.endswith(".wav64")
                    else "images"
                )
                raw = root / f"{work_dir()}/director" / ("wav" if folder == "audio" else folder)
                raw /= name.replace(".wav64", ".wav")
                sha = digest(raw)
                source = video_sources.get(name, sha) if folder == "video" else sha
                if not name.startswith(source[:24] + "."):
                    raise ValueError(f"content-addressed resource changed: {raw}")
                expected[f"{folder}/{name}"] = {"raw": raw, "source_sha256": sha}
                if folder == "images":
                    # Recorded so a policy change repacks exactly the images
                    # whose tier moved, like every other conversion option.
                    expected[f"{folder}/{name}"]["conversion"] = {
                        "tool": "mkasset",
                        "args": ["-c", str(image_level(raw.stat().st_size, tiers))],
                        "color_bits": color_bits,
                        "indexed": indexed,
                    }
                if folder == "audio":
                    expected[f"{folder}/{name}"]["conversion"] = {
                        "tool": "audioconv64",
                        "args": audio_flags,
                    }
                if folder == "video":
                    seek = raw.with_suffix(".seek")
                    expected[f"video/{seek.name}"] = {"raw": seek, "source_sha256": digest(seek)}
    # The scene directories: per movie, the (asset, ink) pairs it draws,
    # best first, each naming the asset's one blob.
    sets = working_sets(root)
    scene_rows: dict[str, dict[int, tuple[tuple[int, int], str]]] = {}
    drawn_in: dict[str, set[str]] = {}
    inks: dict[str, set[int]] = {}
    unknown = set()
    for movie_name, rows in sets.items():
        for row in rows:
            if f"images/{row['asset']}" not in expected:
                unknown.add(row["asset"])
                continue
            inks.setdefault(row["asset"], set()).add(row["ink"])
            key = image_pack.image_key(row["asset"], row["ink"])
            priority = (row.get("ticks", 0), row.get("frames", 0))
            best = scene_rows.setdefault(movie_name, {})
            if key not in best or best[key][0] < priority:
                best[key] = (priority, row["asset"])
            if row.get("ticks", 0):
                drawn_in.setdefault(row["asset"], set()).add(movie_name)
    # The mask ink, baked: the manifest records the mask as part of the
    # conversion, so a change to either image repacks the pair.
    baked = baked_masks(model, inks)
    for asset, mask in baked.items():
        if f"images/{mask}" not in expected:
            raise ValueError(f"mask image is not manifested: {mask}")
        expected[f"images/{asset}"]["conversion"]["mask"] = expected[f"images/{mask}"][
            "source_sha256"
        ]
    expected.update(font_inputs(model_path.parent, model))
    previous_path = root / f"{work_dir()}/director/packed.json"
    previous = json.loads(previous_path.read_text()) if previous_path.exists() else {}
    target = root / f"{work_dir()}/filesystem"
    staging = Path(tempfile.mkdtemp(prefix="pack-", dir=root / f"{work_dir()}/director"))
    pending = {"images": [], "audio": [], "fonts": [], "video": []}
    for folder in pending:
        (staging / folder).mkdir()
    # Compressed images live outside the ROM filesystem: the ROM carries the
    # pack, and the files are the cache the next pack is built from.
    image_cache = root / f"{work_dir()}/director/packed"
    for name, entry in sorted(expected.items()):
        old = previous.get("files", {}).get(name, {})
        packed = (image_cache if name.startswith("images/") else target) / name
        if name.startswith("video/"):
            shutil.copyfile(entry["raw"], staging / name)
        elif (
            previous.get("toolchain") == toolchain
            and packed.exists()
            and old.get("source_sha256") == entry["source_sha256"]
            and old.get("conversion") == entry.get("conversion")
            and old.get("sha256") == digest(packed)
        ):
            shutil.copyfile(packed, staging / name)
        else:
            folder = name.split("/")[0]
            pending[folder].append(
                name if folder in ("fonts", "images") else str(entry["raw"].relative_to(root))
            )
    # Every image is staged as a copy under its manifest name: a port that
    # spends a colour bit or indexes its palettes packs the reduced copy. The
    # manifest still records the recovered image's hash as the source, and
    # the conversion entry the options that produced these bytes. The codec
    # tier is the source's decompressed size, whichever representation the
    # copy ends up in.
    levels = {}
    if pending["images"]:
        reduced = staging / "reduced"
        reduced.mkdir()
        staged = []
        for name in pending["images"]:
            entry = expected[name]
            copy = reduced / Path(name).name
            data = entry["raw"].read_bytes()
            if Path(name).name in baked:
                data = image_pack.apply_mask(
                    data, expected[f"images/{baked[Path(name).name]}"]["raw"].read_bytes()
                )
            if color_bits != 5:
                data = quantize_colors(data)
            if indexed:
                data = index_colors(data)
            copy.write_bytes(data)
            staged.append(str(copy.relative_to(root)))
            levels[staged[-1]] = image_level(entry["raw"].stat().st_size, tiers)
        pending["images"] = staged
    # Images are listed per compression level; everything else keeps one list.
    by_level = {}
    for source in pending["images"]:
        by_level.setdefault(levels[source], []).append(source)
    for folder, files in pending.items():
        if folder == "images":
            continue
        listing = staging / f"{folder}.list"
        listing.write_bytes(b"".join(n.encode() + b"\0" for n in files))
    for level, sources in by_level.items():
        listing = staging / f"images-{level}.list"
        listing.write_bytes(b"".join(n.encode() + b"\0" for n in sources))
    relative = staging.relative_to(root).as_posix()
    # All dynamic paths are generated hexadecimal/numeric basenames under this
    # fixed workspace; input filenames are NUL-separated, never shell-expanded.
    jobs = min(24, os.cpu_count() or 8)
    command = " && ".join(
        [
            f"xargs -r -0 -a {relative}/images-{level}.list -n 16 -P {jobs} "
            f"mkasset -c {level} -o {relative}/images"
            for level in sorted(by_level)
        ]
        + [
            f"xargs -r -0 -a {relative}/audio.list -n 1 -P {jobs} "
            f"audioconv64 {' '.join(audio_flags)} -o {relative}/audio"
        ]
    )
    subprocess.run(
        [
            container_engine(),
            "run",
            "--rm",
            "--network=none",
            "--volume",
            f"{root}:/workdir",
            "--workdir",
            "/workdir",
            image,
            "sh",
            "-c",
            command,
        ],
        check=True,
    )
    # mkfont derives its output basename from the input. Give each requested
    # size its own staging alias; the immutable original remains manifested.
    font_sources = staging / "font-sources"
    font_sources.mkdir()
    font_groups = {}
    for name in pending["fonts"]:
        entry = expected[name]
        alias = font_sources / (Path(name).stem + entry["raw"].suffix)
        shutil.copyfile(entry["raw"], alias)
        flags = tuple(entry["conversion"]["args"])
        font_groups.setdefault(flags, []).append(str(alias.relative_to(root)))
    for flags, sources in font_groups.items():
        subprocess.run(
            [
                container_engine(),
                "run",
                "--rm",
                "--network=none",
                "--volume",
                f"{root}:/workdir",
                "--workdir",
                "/workdir",
                image,
                "mkfont",
                *flags,
                "-o",
                f"{relative}/fonts",
                *sources,
            ],
            check=True,
        )
    files = {}
    for name, entry in sorted(expected.items()):
        packed = staging / name
        if not packed.is_file() or packed.stat().st_size < 16:
            raise ValueError(f"missing/truncated packed asset: {name}")
        files[name] = {
            "source_sha256": entry["source_sha256"],
            "sha256": digest(packed),
            "bytes": packed.stat().st_size,
        }
        if "conversion" in entry:
            files[name]["conversion"] = entry["conversion"]
    # The codec budget: with the tiers' sizes known, promote the images the
    # scenes draw to a faster level while the pack stays within the port's
    # ROM budget. A promoted image is compressed again at its new level,
    # cached under promoted/ by source and level.
    budget = image_pack_budget(root)
    promoted = {}
    if budget:
        images = {}
        for name in expected:
            if name.startswith("images/"):
                level = int(expected[name]["conversion"]["args"][1])
                images[name] = {
                    "level": level,
                    "bytes": (staging / name).stat().st_size,
                    "length": expected[name]["raw"].stat().st_size,
                    "weight": len(drawn_in.get(Path(name).name, ())),
                }
        promoted = image_budget.solve(images, budget)
    promoted_dir = staging / "promoted"
    promoted_dir.mkdir()
    old_promoted = previous.get("promoted", {}) if previous.get("toolchain") == toolchain else {}
    repack = {}
    for name, level in sorted(promoted.items()):
        old = old_promoted.get(name, {})
        cached = image_cache / "promoted" / Path(name).name
        if (
            old.get("source_sha256") == expected[name]["source_sha256"]
            and old.get("level") == level
            and cached.is_file()
            and old.get("sha256") == digest(cached)
        ):
            shutil.copyfile(cached, promoted_dir / Path(name).name)
        else:
            # A cached baseline blob had no staged copy this run; stage the
            # source again, converted exactly as the baseline was.
            copy = staging / "reduced" / Path(name).name
            if not copy.exists():
                copy.parent.mkdir(exist_ok=True)
                data = expected[name]["raw"].read_bytes()
                if Path(name).name in baked:
                    data = image_pack.apply_mask(
                        data, expected[f"images/{baked[Path(name).name]}"]["raw"].read_bytes()
                    )
                if color_bits != 5:
                    data = quantize_colors(data)
                if indexed:
                    data = index_colors(data)
                copy.write_bytes(data)
            repack.setdefault(level, []).append(str(copy.relative_to(root)))
    for level, sources in sorted(repack.items()):
        listing = staging / f"promoted-{level}.list"
        listing.write_bytes(b"".join(n.encode() + b"\0" for n in sources))
        subprocess.run(
            [
                container_engine(), "run", "--rm", "--network=none",
                "--volume", f"{root}:/workdir", "--workdir", "/workdir", image, "sh", "-c",
                f"xargs -r -0 -a {relative}/promoted-{level}.list -n 16 -P {jobs} "
                f"mkasset -c {level} -o {relative}/promoted",
            ],
            check=True,
        )
    promoted_files = {}
    for name, level in sorted(promoted.items()):
        packed = promoted_dir / Path(name).name
        if not packed.is_file() or packed.stat().st_size < 16:
            raise ValueError(f"missing/truncated promoted asset: {name}")
        promoted_files[name] = {
            "source_sha256": expected[name]["source_sha256"],
            "level": level,
            "sha256": digest(packed),
            "bytes": packed.stat().st_size,
        }
    # The pack: every compressed image once (by content), indexed by its
    # authored key, and per scene the directory of what the scene draws.
    blobs, index, dims, by_digest, blob_of = {}, {}, {}, {}, {}
    for name in sorted(expected):
        if not name.startswith("images/"):
            continue
        source_file = (promoted_dir if name in promoted else staging) / (
            Path(name).name if name in promoted else name
        )
        data = source_file.read_bytes()
        asset = Path(name).name
        blob = by_digest.setdefault(hashlib.sha256(data).hexdigest(), asset)
        blobs.setdefault(blob, data)
        blob_of[asset] = blob
        index[image_pack.image_key(asset, image_pack.AUTHORED)] = blob
        if blob not in dims:
            width, height, _ = image_pack.image_dims(expected[name]["raw"].read_bytes()[:32])
            dims[blob] = (width, height)
    scenes = {}
    for movie in model["movies"]:
        rows = scene_rows.get(movie["name"], {})
        ordered = sorted(rows.items(), key=lambda kv: (-kv[1][0][0], -kv[1][0][1], kv[0]))
        # Share: per mille of the scene's most-drawn row's ticks, for the
        # console to pin what the stage draws most when not all of it fits.
        most = max((ticks for _, ((ticks, _), _) in ordered), default=0)
        scenes[movie["id"]] = [
            (key, blob_of[asset], (ticks * 1000 // most) if most else 0)
            for key, ((ticks, _), asset) in ordered
        ]
    scene_count = len(model["movies"])
    if scene_count:
        scenes.setdefault(scene_count, [])
    target.mkdir(parents=True, exist_ok=True)
    flags = {
        blob_of[asset]: image_pack.MASK_BAKED for asset in baked if asset in blob_of
    }
    pack_report = image_pack.write_pack(
        staging / "images.pack", blobs, index, dims, scenes, flags
    )
    pack_report["unknown_assets"] = sorted(unknown)
    pack_report["masks_baked"] = len(baked)
    pack_report["sha256"] = digest(staging / "images.pack")
    pack_report["budget"] = budget
    pack_report["promoted"] = {
        "images": len(promoted),
        "bytes": sum(promoted_files[n]["bytes"] - (staging / n).stat().st_size for n in promoted),
        "seconds_saved": round(
            sum(
                images[n]["weight"]
                * image_budget.seconds_saved(images[n]["length"], images[n]["level"], level)
                for n, level in promoted.items()
            ),
            3,
        )
        if promoted
        else 0.0,
    }
    pack_report["scenes"] = {
        movie["name"]: pack_report["scenes"][movie["id"]]
        for movie in model["movies"]
        if movie["id"] in pack_report["scenes"]
    }
    # Preserve the previous generated directories outside DragonFS. Stale assets
    # can neither inflate the ROM nor accidentally satisfy a missing dependency.
    backup = staging / "previous"
    backup.mkdir()
    for folder in pending:
        if folder == "images":
            continue
        if (target / folder).exists():
            (target / folder).rename(backup / folder)
        (staging / folder).rename(target / folder)
    # Per-image files never enter the ROM filesystem; the pack does.
    if (target / "images").exists():
        (target / "images").rename(backup / "images")
    image_cache.mkdir(parents=True, exist_ok=True)
    if (image_cache / "images").exists():
        (image_cache / "images").rename(backup / "images-cache")
    (staging / "images").rename(image_cache / "images")
    if (image_cache / "promoted").exists():
        (image_cache / "promoted").rename(backup / "promoted-cache")
    promoted_dir.rename(image_cache / "promoted")
    (staging / "images.pack").replace(target / "images.pack")
    manifest = {
        "version": 1,
        "model_sha256": digest(model_path),
        "toolchain": toolchain,
        "files": files,
        "promoted": promoted_files,
        "pack": pack_report,
    }
    previous_path.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Packed {len(files)} manifested resources; prior generated assets preserved in {backup}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--image",
        default=os.environ.get("DIRECTOR64_TOOLCHAIN_IMAGE", "director64-toolchain:local"),
    )
    args = parser.parse_args()
    pack(Path.cwd(), args.image)


if __name__ == "__main__":
    main()
