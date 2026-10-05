"""Compile recovered scene metadata into native, per-movie overlay data."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

from director64.project import work_dir

from .aot import cstring, text_hash

# Mirrors DG_SPAN in runtime/director/director.h: the mask bit that marks a
# delta as the start of a new sprite span rather than a change within one.
DG_SPAN = 0x8000


def member_name_hash(name: str) -> int:
    """The member-name index key: the runtime's one text hash, shared with
    the handler index (see `text_hash`)."""
    return text_hash(name)


def score_tempo(record: bytes, version: int = 6, *, extended_d6: bool = False) -> int:
    """Normalize a versioned FPS command; D8 delay is a separate native field."""
    opcode = record[6]
    if opcode == 0:
        return 0  # No command: retain the most recent score rate.
    if (version >= 7 or extended_d6) and opcode == 247:
        return 0
    if opcode != 246:
        raise ValueError(f"unsupported Director {version} tempo opcode {opcode}")
    rate = int.from_bytes(record[4:6], "big")
    if not 1 <= rate <= (999 if version >= 7 else 120):
        raise ValueError(f"unsupported Director {version} frame rate {rate}")
    return rate


def validate_script_casts(model: dict, program: dict) -> None:
    """Reject split identities between recovered code and resource bindings."""
    movies = {m["name"].casefold(): m for m in model["movies"]}
    for handler in program["handlers"]:
        movie = movies[handler["movie"].casefold()]
        casts = {c["name"].casefold(): c for c in movie["casts"]}
        cast = casts.get(handler["cast"].casefold())
        if cast is None or cast["file"].casefold() != handler["movie"].casefold():
            raise ValueError(
                f"{handler['movie']}:{handler['cast']}:{handler['member']}: "
                "script cast does not match local resource namespace"
            )


def generate(model: dict, output: Path) -> None:
    if model["problems"]:
        raise ValueError("resource conversion is incomplete")
    movies = model["movies"]
    by_name = {m["name"]: m for m in movies}
    output.mkdir(parents=True, exist_ok=True)

    def reference(movie, cast, member):
        if not member:
            return 0
        if not 1 <= cast <= len(movie["casts"]):
            raise ValueError(f"{movie['name']}: invalid cast {cast}:{member}")
        lib = movie["casts"][cast - 1]
        target = by_name[lib["file"]]
        target_cast = cast if target is movie else 1
        return target["id"] << 20 | target_cast << 16 | member

    font_metrics = model.get("fontMetrics", {})
    font_aliases = {}
    for font in model.get("fonts", []):
        for alias in font.get("aliases", []):
            font_aliases[alias.casefold()] = font["number"]
    for movie in movies:
        score = movie.get("score", {})
        declared_version = movie.get("directorVersion", 6)
        if declared_version >= 100:
            declared_version //= 100
        version = (
            5
            if declared_version == 5
            else (10 if declared_version >= 10 else 7 if declared_version == 7 else 8)
            if score.get("recordSize", score.get("record_size", 24)) == 48
            or score.get("version", 0) >= 13
            or declared_version >= 7
            else 6
        )
        stem = movie["name"].replace(".", "_").lower()
        lines = ['#include "director.h"', f"extern const lv_movie_t aot_{stem};"]
        text_styles = {}

        def text_style(style, text_styles=text_styles, lines=lines):
            if not style:
                return "NULL"
            key = json.dumps(style, sort_keys=True)
            if key not in text_styles:
                name = f"text_style_{len(text_styles)}"
                text_styles[key] = name
                values = {
                    "font_name": cstring(style["fontName"]),
                    "font_id": style["fontId"],
                    "size": style["size"],
                    "align": style["align"],
                    "ascent": style["ascent"],
                    "descent": style["descent"],
                    "leading": style["leading"],
                    "line_height": style["lineHeight"],
                    "color": style["color"],
                }
                metrics = style.get("controllerMetrics")
                if metrics is None and style["fontId"]:
                    # Measured per-variant metrics from the packed font64 set,
                    # attached by the game's asset postprocessor.
                    metrics = font_metrics.get(f"{style['fontId']}:{style['size']}")
                if metrics:
                    advances = metrics["advances"]
                    pairs = metrics["kerning"]
                    lines.append(
                        f"static const uint8_t {name}_advances[]={{"
                        + ",".join(map(str, advances)) + "};"
                    )
                    lines.append(
                        f"static const int16_t {name}_kerning[]={{"
                        + (",".join(str(n) for pair in pairs for n in pair) or "0") + "};"
                    )
                    values.update(advances=f"{name}_advances", kerning=f"{name}_kerning",
                                  kerning_count=len(pairs), advance_count=len(advances))
                    if not values["ascent"] and not values["line_height"] and "ascent" in metrics:
                        # Zeroed STXT vertical metrics mean "use the face's
                        # own"; the measured build supplies them at the
                        # authored size.
                        values["ascent"] = metrics["ascent"]
                        values["descent"] = metrics["descent"]
                        values["line_height"] = (
                            metrics["ascent"] + metrics["descent"] + values["leading"]
                        )
                lines.append(
                    f"static const dg_text_style_t {name}={{"
                    + ",".join(f".{k}={v}" for k, v in values.items())
                    + "};"
                )
            return "&" + text_styles[key]

        def flash_field_style(field, movie=movie):
            """A converted Flash edit field renders through a text style built
            from its recorded descriptor and the embedded font's metrics."""
            size = max(1, round(field.get("fontHeight") or 12))
            leading = round(field.get("leading") or 0)
            font_id = font_aliases.get((field.get("fontName") or "").casefold(), 0)
            metrics = font_metrics.get(f"{font_id}:{size}") if font_id else None
            color = field.get("color") or [0, 0, 0, 255]
            return {
                "fontName": field.get("fontName") or "",
                "fontId": font_id,
                "size": size,
                # SWF field alignment: 0 left, 1 right, 2 center → renderer
                # order left, center, right.
                "align": {0: 0, 1: 2, 2: 1}.get(field.get("align") or 0, 0),
                "ascent": metrics["ascent"] if metrics else size,
                "descent": metrics["descent"] if metrics else 0,
                "leading": leading,
                "lineHeight": size + leading,
                "color": color[0] * 65536 + color[1] * 256 + color[2],
            }

        members, member_names = [], []
        for m in sorted(movie["members"], key=lambda m: (m["cast"], m["number"])):
            member_names.append(m.get("name", ""))
            film = m.get("filmAssets", [])
            film_name = f"film_{m['cast']}_{m['number']}" if film else "NULL"
            if film:
                lines.append(
                    f"static const char *const {film_name}[]={{"
                    + ",".join(cstring(n) for n in film)
                    + "};"
                )
            flash_labels = (m.get("flashTimeline") or {}).get("labels", [])
            flash_labels_name = "NULL"
            if version >= 10 and flash_labels:
                flash_labels_name = f"flash_labels_{m['cast']}_{m['number']}"
                lines.append(
                    f"static const dg_label_t {flash_labels_name}[]={{"
                    + ",".join(
                        "{" + f"{cstring(label['name'])},{label['frame']}" + "}"
                        for label in flash_labels
                    )
                    + "};"
                )
            flash_fields = []
            if version >= 10:
                seen = set()
                for field in m.get("flashFields", []):
                    binding = (field.get("name") or field.get("variable") or "").casefold()
                    if not binding or binding in seen:
                        continue
                    seen.add(binding)
                    flash_fields.append(field)
            flash_fields_name = "NULL"
            if flash_fields:
                flash_fields_name = f"flash_fields_{m['cast']}_{m['number']}"
                entries = []
                for field in flash_fields:
                    bounds = field["bounds"]
                    initial = field.get("text") or ""
                    if field.get("html") and initial:
                        # The authored initial markup reduces to its runs; the
                        # descriptor style carries the face/size/alignment.
                        initial = re.sub(r"</p>\s*", "\r", initial)
                        initial = re.sub(r"<[^>]*>", "", initial).rstrip("\r")
                    entries.append(
                        "{"
                        + ",".join(
                            [
                                f".name={cstring(field.get('name') or field.get('variable'))}",
                                f".variable={cstring(field.get('variable') or '')}",
                                f".text={cstring(initial)}",
                                f".x={round(bounds['left'])}",
                                f".y={round(bounds['top'])}",
                                f".width={round(bounds['width'])}",
                                f".height={round(bounds['height'])}",
                                f".margin_left={round(field.get('leftMargin') or 0)}",
                                f".margin_right={round(field.get('rightMargin') or 0)}",
                                f".indent={round(field.get('indent') or 0)}",
                                f".align={ {0: 0, 1: 2, 2: 1}.get(field.get('align') or 0, 0)}",
                                f".word_wrap={int(bool(field.get('wordWrap')))}",
                                f".multiline={int(bool(field.get('multiline')))}",
                                f".leading={round(field.get('leading') or 0)}",
                                f".style={text_style(flash_field_style(field))}",
                            ]
                        )
                        + "}"
                    )
                lines.append(
                    f"static const dg_flash_field_t {flash_fields_name}[]={{"
                    + ",".join(entries)
                    + "};"
                )
            cues_name, cue_points = "NULL", m.get("cuePoints") or []
            if model.get("extendedD6") and cue_points:
                cues_name = f"cues_{m['cast']}_{m['number']}"
                lines.append(
                    f"static const dg_cue_t {cues_name}[]={{"
                    + ",".join(
                        "{" + str(c["milliseconds"]) + "," + cstring(c["name"]) + "}"
                        for c in cue_points
                    )
                    + "};"
                )
            film_sounds = "NULL"
            if version == 5 and m.get("filmSounds"):
                film_sounds = f"film_sounds_{m['cast']}_{m['number']}"
                values = [
                    reference(movie, pair["cast"], pair["member"]) if pair else 0
                    for frame in m["filmSounds"]
                    for pair in frame
                ]
                lines.append(
                    f"static const uint32_t {film_sounds}[]={{" + ",".join(map(str, values)) + "};"
                )
            values = [
                reference(movie, m["cast"], m["number"]),
                m["number"],
                m["cast"],
                m["type"],
                m.get("width", 0),
                m.get("height", 0),
                m.get("regX", 0),
                m.get("regY", 0),
            ]
            texts = [cstring(m.get(k, "")) for k in ("name", "asset", "text")]
            binary_text = bool(model.get("extendedD6") and "\0" in m.get("text", ""))
            text_bytes = (
                bytes.fromhex(m["textHex"]) if binary_text else m.get("text", "").encode("utf-8")
            )
            if binary_text or len(text_bytes) > 4095:
                text_name = f"text_{m['cast']}_{m['number']}"
                lines.append(
                    f"static const unsigned char {text_name}[]={{"
                    + ",".join(map(str, text_bytes))
                    + ",0};"
                )
                texts[2] = f"(const char *){text_name}"
            sound = [m.get(k, 0) for k in ("frames", "rate", "loopStart", "loopEnd")]
            sound += [m.get(k, 0) for k in ("shape", "pattern", "filled", "lineWidth")]
            sound += [m.get("looping", 0), len(film), m.get("filmLoop", 0)]
            styles = (
                [text_style(m.get("textStyle")), text_style(m.get("textInsertStyle"))]
                if version != 6 or model.get("extendedD6")
                else []
            )
            members.append(
                "{"
                + ",".join(
                    [
                        *(str(v) for v in values),
                        *texts,
                        *(str(v) for v in sound),
                        film_name,
                        str(m.get("lineDirection", 0)),
                        *(
                            [
                                str(int(m.get("editable", False))),
                                str(len(text_bytes) if binary_text else 0),
                                str(m.get("sourceBytes", 0)),
                                str(len(cue_points)),
                                cues_name,
                            ]
                            if model.get("extendedD6")
                            else []
                        ),
                        *styles,
                        *(
                            [
                                cstring(m.get("videoAudio", "")),
                                str(m.get("videoFlags", 0)),
                                film_sounds,
                            ]
                            if version == 5
                            else []
                        ),
                        *(
                            # Members born from Flash/vectorShape Xtras answer
                            # their authored type to scripts even when playback
                            # is converted or deferred. The recorded dynamic
                            # surface (labels, fields) follows.
                            [
                                str(
                                    {"flash": 1, "vectorShape": 2}.get(
                                        (m.get("xtra") or {}).get("symbol"), 0
                                    )
                                ),
                                str(len(flash_labels)),
                                str(len(flash_fields)),
                                flash_labels_name,
                                flash_fields_name,
                            ]
                            if version >= 10
                            else []
                        ),
                    ]
                )
                + "}"
            )
        lines.append("static const dg_member_t members[]={" + (",\n".join(members) or "{0}") + "};")
        # Name searches used to stride the whole member table, which is tens of
        # kilobytes of ~80-byte records against the console's 8 KiB data cache.
        # This is the same table ordered by a folded-name hash; `sorted` is
        # stable, so equal hashes keep the (cast, number) order a linear search
        # would have reported. dg_member.name is matched case-insensitively, and
        # the runtime still confirms every hit with the full comparison.
        index = sorted(
            ((member_name_hash(name), position) for position, name in enumerate(member_names)),
            key=lambda entry: entry[0],
        )
        lines.append(
            "static const dg_member_index_t member_index[]={"
            + (",".join("{" + f"{h}u,{position}" + "}" for h, position in index) or "{0}")
            + "};"
        )
        palette = movie.get("palette", [0xFFFFFF] * 255 + [0])
        if len(palette) != 256:
            raise ValueError("scene palette must have 256 colors")
        lines.append("static const uint32_t palette[]={" + ",".join(map(str, palette)) + "};")
        casts = [
            "{"
            + f"{cstring(c['name'])},{by_name[c['file']]['id']},"
            + str(c["number"] if c["file"] == movie["name"] else 1)
            + "}"
            for c in movie["casts"]
        ]
        lines.append("static const dg_cast_t casts[]={" + ",".join(casts) + "};")
        deltas, frames = [], []
        behavior_tables = {}
        for frame in movie.get("score", {}).get("frames", []):
            frames.append("{" + f"{len(deltas)},{len(frame['channels'])}" + "}")
            for channel in frame["channels"]:
                b = bytes(channel["bytes"])
                c = channel["channel"]
                expected = 48 if version >= 7 else 24
                if len(b) != expected or len(channel["changed"]) != expected:
                    raise ValueError(
                        f"{movie['name']}: invalid Director {version} score record size"
                    )
                if not 0 <= c < (1006 if version >= 10 else 806 if version >= 7 else 126):
                    raise ValueError(f"{movie['name']}: score channel {c} exceeds profile")

                def u16(n, data=b):
                    return int.from_bytes(data[n : n + 2], "big")

                def i16(n, data=b):
                    return int.from_bytes(data[n : n + 2], "big", signed=True)

                member = (
                    reference(movie, u16(4 if c >= 6 else 0), u16(6 if c >= 6 else 2))
                    if c not in (1, 5)
                    else 0
                )
                scripts = channel["behaviors"]
                if len(scripts) > 1 and not model.get("extendedD6"):
                    raise ValueError("multiple behaviors require additional dispatch slots")
                if any(s.get("parameters") for s in scripts) and not model.get("extendedD6"):
                    raise ValueError("parameterized behavior requires extended runtime")
                script = (
                    reference(movie, scripts[0]["cast"], scripts[0]["member"]) if scripts else 0
                )
                if c == 0 and not script:
                    script = member
                values = (
                    [
                        member,
                        script,
                        i16(14),
                        i16(12),
                        i16(18),
                        i16(16),
                        b[1] & 63,
                        (255 - b[21]) * 100 // 255,
                        b[0],
                        b[20],
                        b[2],
                        b[3],
                        b[22],
                        int(bool(b[1] & 128)),
                        int(bool(b[1] & 64)),
                    ]
                    if c >= 6
                    else [
                        member,
                        script,
                        0,
                        0,
                        0,
                        0,
                        0,
                        0,
                        (
                            b[6]
                            if version == 5
                            else score_tempo(b, version, extended_d6=bool(model.get("extendedD6")))
                        )
                        if c == 1 and version < 7
                        else 0,
                        0,
                        0,
                        0,
                        0,
                        0,
                        0,
                    ]
                )
                if version != 6 or model.get("extendedD6"):
                    # Positional, in dg_spec_t order: loc_z, tempo, delay,
                    # the two colors, rotation and skew.
                    values += [
                        c - 5 if c >= 6 else 0,
                        (b[6] if 1 <= b[6] <= 120 else 0)
                        if c == 1 and version == 5
                        else score_tempo(b, version, extended_d6=bool(model.get("extendedD6")))
                        if c == 1
                        else 0,
                        (256 - b[6] if b[6] >= 196 else 0)
                        if c == 1 and version == 5
                        else u16(4)
                        if c == 1 and b[6] == 247
                        else 0,
                        palette[b[2]]
                        if c >= 6 and version < 7
                        else (b[2] << 16 | b[24] << 8 | b[26])
                        if c >= 6
                        else 0,
                        palette[b[3]]
                        if c >= 6 and version < 7
                        else (b[3] << 16 | b[25] << 8 | b[27])
                        if c >= 6
                        else 0,
                        int.from_bytes(b[28:32], "big", signed=True)
                        if c >= 6 and version >= 7
                        else 0,
                        int.from_bytes(b[32:36], "big", signed=True)
                        if c >= 6 and version >= 7
                        else 0,
                    ]
                if model.get("extendedD6"):
                    behavior_name = "NULL"
                    if scripts:
                        key = json.dumps(scripts, sort_keys=True)
                        if key not in behavior_tables:
                            behavior_name = f"behaviors_{len(behavior_tables)}"
                            behavior_tables[key] = behavior_name
                            entries = [
                                "{"
                                + str(reference(movie, s["cast"], s["member"]))
                                + ","
                                + cstring(s.get("parameters", ""))
                                + "}"
                                for s in scripts
                            ]
                            lines.append(
                                f"static const dg_behavior_t {behavior_name}[]={{"
                                + ",".join(entries)
                                + "};"
                            )
                        behavior_name = behavior_tables[key]
                    values += [behavior_name, len(scripts)]
                mask = 0
                for flag, indexes in [
                    (1, range(4, 8)),
                    (2, range(12, 16)),
                    (4, range(18, 20)),  # width and height have separate D6 ownership
                    (8, [1]),  # ink, stretch and trails share the score byte
                    (16, [21]),
                    (32, [0]),
                    (64, range(16, 18)),
                    (128, [2]),
                    (256, [3]),
                    (512, [22]),  # includes the blend-enable flag
                    (1024, [20]),
                ]:
                    if any(channel["changed"][i] for i in indexes):
                        mask |= flag
                if version >= 7:
                    for flag, indexes in [
                        (128, [20, 24, 26]),
                        (256, [20, 25, 27]),
                        (2048, range(28, 32)),
                        (4096, range(32, 36)),
                    ]:
                        if any(channel["changed"][i] for i in indexes):
                            mask |= flag
                # Director rewrites a channel's whole record where a new
                # sprite span begins and writes only what moved inside one.
                # That is the only thing separating three consecutive
                # one-frame sprites from a single sprite held for three
                # frames, and it decides who gets beginSprite: Willy's login
                # screen drops a SendBH on each of its Still/Talk/Wait
                # frames to announce the label it just entered.
                if c >= 6 and all(channel["changed"]):
                    mask |= DG_SPAN
                deltas.append("{" + f"{c},{mask}," + "{" + ",".join(map(str, values)) + "}}")
        labels = movie.get("score", {}).get("labels", [])
        lines.append("static const dg_frame_t frames[]={" + (",".join(frames) or "{0}") + "};")
        lines.append("static const dg_delta_t deltas[]={" + (",\n".join(deltas) or "{0}") + "};")
        lines.append(
            "static const dg_label_t labels[]={"
            + (
                ",".join(
                    "{" + f"{cstring(label['name'])},{label['frame']}" + "}" for label in labels
                )
                or "{0}"
            )
            + "};"
        )
        lines.append(
            f"const dg_movie_t dg_{stem}={{&aot_{stem},{movie['id']},{movie['tempo']},"
            f"{len(casts)},{len(members)},{len(frames)},{len(labels)},"
            "casts,members,member_index,frames,deltas,labels,palette"
            + (
                f",{movie.get('stageColorRGB', palette[movie.get('stageColor', 0) & 255])}"
                if version != 6 or model.get("extendedD6")
                else ""
            )
            + "};"
        )
        (output / f"{stem}_scene.c").write_text("\n".join(lines) + "\n")
    declarations = [
        f"extern const dg_movie_t dg_{m['name'].replace('.', '_').lower()};" for m in movies
    ]
    pointers = ["&dg_" + m["name"].replace(".", "_").lower() for m in movies]
    (output / "registry.c").write_text(
        '#include "director.h"\n'
        + "\n".join(declarations)
        + "\nconst dg_movie_t *const dg_registry[]={"
        + ",".join(pointers)
        + "};\n"
        + f"const unsigned dg_registry_count={len(movies)};\n"
    )
    (output / "movie_names.inc").write_text(
        ",\n".join(cstring(m["name"].replace(".", "_").lower()) for m in movies) + "\n"
    )
    # Each movie's distinct cast files, so a scene can order its overlay loads
    # by size without first loading the entering movie's own overlay to read
    # the list back out of it. Authored order, zero-terminated, indexed by
    # movie id; the authored list is what the runtime reads from `casts`,
    # and a script's later castLib swap loads its target separately.
    files: list[int] = []
    first: list[int] = []
    if [m["id"] for m in movies] != list(range(1, len(movies) + 1)):
        raise ValueError("movie ids must index the registry")
    for movie in movies:
        first.append(len(files))
        seen: set[int] = set()
        for cast in movie["casts"]:
            file = by_name[cast["file"]]["id"]
            if file and file != movie["id"] and file not in seen:
                seen.add(file)
                files.append(file)
        files.append(0)
    # A movie several others share as a cast library is worth keeping loaded
    # across a scene change: the authored flow separates every pair of real
    # scenes with a tiny transition movie, so draining there makes the next
    # scene reload the libraries it just released. A stage movie's own overlay
    # is referenced by nobody and stays a one-scene load.
    shared: list[int] = []
    for movie in movies:
        users = {
            other["id"]
            for other in movies
            for cast in other["casts"]
            if by_name[cast["file"]]["id"] == movie["id"] and other["id"] != movie["id"]
        }
        shared.append(1 if len(users) >= 2 else 0)
    (output / "movie_casts.inc").write_text(
        "static const uint16_t movie_cast_files[]={" + ",".join(map(str, files)) + "};\n"
        "static const uint16_t movie_cast_first[]={" + ",".join(map(str, first)) + "};\n"
        "static const uint8_t movie_shared_cast[]={" + ",".join(map(str, shared)) + "};\n"
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--program", type=Path, default=Path(f"{work_dir()}/aot/program.json"))
    args = parser.parse_args()
    model = json.loads(args.model.read_text())
    validate_script_casts(model, json.loads(args.program.read_text()))
    generate(model, args.output)


if __name__ == "__main__":
    main()
