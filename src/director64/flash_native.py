"""Native descriptors for the retained SWF dictionary, without executing AVM1."""

from .aot import cstring


def array(lines, ctype, name, entries):
    if not entries:
        return "NULL"
    lines.append(f"static const {ctype} {name}[]={{" + ",".join(entries) + "};")
    return name


def actions(lines, name, values):
    entries = []
    for i, a in enumerate(values):
        code = a["code"]
        if not code or len(code) > 1048576 or code[-1] != 0 or any(not 0 <= n <= 255 for n in code):
            raise ValueError("invalid Flash action bytes")
        data = array(lines, "uint8_t", f"{name}_{i}", list(map(str, code)))
        entries.append(
            "{" + f"{a['frame']},{a.get('event', 0)},{len(code)},{data},{a.get('key', 0)}" + "}"
        )
    return array(lines, "dg_flash_action_t", name, entries)


def frame(lines, name, values):
    entries, previous = [], 0
    for i, layer in enumerate(values):
        channel = layer["channel"]
        if not previous < channel <= 65535:
            raise ValueError("Flash layer depth outside profile or unordered")
        previous = channel
        spec = {
            "x": layer["x"],
            "y": layer["y"],
            "width": layer["width"],
            "height": layer["height"],
            "ink": 8,
            "blend": 100,
            "type": 16,
            "stretch": 1,
            "loc_z": channel,
            "flags": 48,
            "fore": 1,
            "back_rgb": 0xFFFFFF,
        }
        members = [
            f".channel={channel}",
            ".value={" + ",".join(f".{k}={v}" for k, v in spec.items()) + "}",
            f".asset={cstring(layer['asset']) if layer.get('asset') else 'NULL'}",
            f".mask_asset={cstring(layer['maskAsset']) if layer.get('maskAsset') else 'NULL'}",
            f".name={cstring(layer['name']) if layer.get('name') else 'NULL'}",
            f".origin_x={layer['originX']}",
            f".origin_y={layer['originY']}",
            f".affine={int(bool(layer.get('affine')))}",
            ".matrix={"
            + ",".join(str(round(v * 65536)) for v in layer.get("matrix", [0] * 6))
            + "}",
        ]
        for key, ckey in [
            ("clipId", "clip_id"),
            ("buttonId", "button_id"),
            ("fieldId", "field_id"),
            ("clipDepth", "clip_depth"),
            ("placementGeneration", "placement_generation"),
        ]:
            members.append(f".{ckey}={layer.get(key, 0)}")
        members += [
            f".has_cxform={int('cxform' in layer)}",
            ".cxform={" + ",".join(map(str, layer.get("cxform", [0] * 8))) + "}",
            f".action_count={len(layer.get('actions', []))}",
            f".actions={actions(lines, f'{name}_actions_{i}', layer.get('actions', []))}",
        ]
        entries.append("{" + ",".join(members) + "}")
    ptr = array(lines, "dg_film_layer_t", name, entries)
    return "{" + f"{len(entries)},{ptr}" + "}"


def frames(lines, name, values):
    entries = [frame(lines, f"{name}_{i}", f) for i, f in enumerate(values)]
    return array(lines, "dg_film_frame_t", name, entries)


def runtime(lines, name, value):
    if not value:
        return ["0"] * 4 + ["NULL"] * 4 + ["0", "NULL"]
    clips, buttons, sounds, exports = [], [], [], []

    def fixed(v):
        return "{" + ",".join(str(round(n * 65536)) for n in v) + "}"

    for c in value.get("clips", []):
        prefix = f"{name}_clip_{c['id']}"
        f = frames(lines, prefix + "_frames", c["frames"])
        a = actions(lines, prefix + "_actions", c["actions"])
        labels = array(
            lines,
            "dg_label_t",
            prefix + "_labels",
            ["{" + f"{cstring(label['name'])},{label['frame']}" + "}" for label in c["labels"]],
        )
        events = []
        for i, e in enumerate(c["soundEvents"]):
            envelopes = array(
                lines,
                "dg_flash_envelope_t",
                f"{prefix}_envelopes_{i}",
                ["{" + ",".join(map(str, v)) + "}" for v in e["envelopes"]],
            )
            events.append(
                "{"
                + ",".join(
                    map(
                        str,
                        [
                            e["frame"],
                            e["sound"],
                            e["loops"],
                            e["flags"],
                            len(e["envelopes"]),
                            e["inPoint"],
                            e["outPoint"],
                            envelopes,
                        ],
                    )
                )
                + "}"
            )
        events = array(lines, "dg_flash_sound_event_t", prefix + "_sounds", events)
        clips.append(
            "{"
            + ",".join(
                map(
                    str,
                    [
                        c["id"],
                        c["frameCount"],
                        len(c["actions"]),
                        len(c["labels"]),
                        len(c["soundEvents"]),
                        fixed(c["localBounds"]),
                        f,
                        a,
                        labels,
                        events,
                    ],
                )
            )
            + "}"
        )
    for b in value.get("buttons", []):
        prefix = f"{name}_button_{b['id']}"
        states = [frame(lines, f"{prefix}_state_{i}", f) for i, f in enumerate(b["states"])]
        a = actions(lines, prefix + "_actions", b["actions"])
        buttons.append(
            "{"
            + ",".join(
                map(
                    str,
                    [
                        b["id"],
                        len(b["actions"]),
                        int(b["trackAsMenu"]),
                        fixed(b["localBounds"]),
                        "{" + ",".join(states) + "}",
                        a,
                    ],
                )
            )
            + "}"
        )
    for s in value.get("sounds", []):
        sounds.append("{" + f"{s['id']},{cstring(s['asset'])},{s['samples']},{s['rate']}" + "}")
    for e in value.get("exports", []):
        exports.append("{" + f"{e['id']},{cstring(e['name'])}" + "}")
    values = [clips, buttons, sounds, exports]
    pointers = [
        array(lines, f"dg_flash_{kind}_t", f"{name}_{kind}s", entries)
        for kind, entries in zip(["clip", "button", "sound", "export"], values, strict=True)
    ]
    initializers = []
    for i, init in enumerate(value.get("initializers", [])):
        code = init["code"]
        pointer = array(lines, "uint8_t", f"{name}_init_code_{i}", list(map(str, code)))
        initializers.append(
            "{" + f"{init['clipId']}," + "{" + f"{init['frame']},0,{len(code)},{pointer},0" + "}}"
        )
    ip = array(lines, "dg_flash_initializer_t", f"{name}_initializers", initializers)
    return [str(len(v)) for v in values] + pointers + [str(len(initializers)), ip]


def assets(value):
    """Include offstage/exported definitions: scripts can instantiate them."""
    if isinstance(value, dict):
        for key in ("asset", "maskAsset"):
            if value.get(key):
                yield value[key]
        for child in value.values():
            yield from assets(child)
    elif isinstance(value, list):
        for child in value:
            yield from assets(child)
