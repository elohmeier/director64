"""Corroborate recovered scripts and score bytes without claiming execution."""

from __future__ import annotations

import json
from collections import Counter

from .aot import native_blockers
from .iso import file_sha256
from .lingo import parse_movie


def validate_handlers(source: dict, program: dict) -> list[dict]:
    expected = Counter()
    for file in source["files"]:
        members = {m["id"]: m for m in file["members"]}
        casts = {c["library_id"]: c["name"] for c in file["libraries"]}
        for script in file["scripts"]:
            member = members[script["member_id"]]
            for handler in script["handlers"]:
                expected[
                    (
                        file["name"],
                        casts[member["library_id"]].casefold(),
                        member["member_id"],
                        handler["name"].casefold(),
                    )
                ] += 1
    actual = Counter(
        (h["movie"], h["cast"].casefold(), h["member"], h["name"]) for h in program["handlers"]
    )
    if expected != actual:
        raise ValueError("recovered AST handler identities differ from source bytecode")
    return [
        {key: h[key] for key in ("movie", "cast", "member", "name")} | {"requires": blockers}
        for h in program["handlers"]
        if (blockers := native_blockers(h))
    ]


def report(game):
    analysis = game.work / "analysis"
    paths = {
        "source": analysis / "source/manifest.json",
        "scores": analysis / "score-recovery/manifest.json",
        "program": game.work / "aot/program.json",
    }
    source, scores, program = (json.loads(path.read_text()) for path in paths.values())
    blockers = validate_handlers(source, program)
    if scores["source_manifest_sha256"] != file_sha256(paths["source"]):
        raise ValueError("score recovery belongs to a different source audit")
    # Launcher code is evidence in its own namespace. It is not implicitly
    # replaced by the entry movie or treated as already executed on the N64.
    launcher = source["supplemental_projector"]
    members = {m["id"]: m for m in launcher["members"]}
    casts = {c["library_id"]: c["name"] for c in launcher["libraries"]}
    launcher_handlers = []
    for script in launcher["scripts"]:
        member = members[script["member_id"]]
        text = (
            f"-- cast: {casts[member['library_id']]}; member: {member['member_id']}; "
            f"type: {script['script_type']}; name: launcher\n" + script["lingo"]
        )
        launcher_handlers.extend(parse_movie(text, launcher["name"]))
    validate_handlers({"files": [launcher]}, {"handlers": launcher_handlers})
    (analysis / "launcher-program.json").write_text(
        json.dumps({"handlers": launcher_handlers, "native_execution_verified": False}) + "\n"
    )
    xtras = Counter(
        m["xtra"]["symbol"] for f in source["files"] for m in f["members"] if m.get("xtra")
    )
    requirements = Counter(reason for h in blockers for reason in h["requires"])
    layouts = Counter()
    tempos = Counter()
    max_sprite = 0
    for file in scores["files"]:
        for resource in file["resources"]:
            if resource["fourcc"] != "VWSC":
                continue
            path = analysis / "score-recovery" / resource["blob"]
            if file_sha256(path) != resource["output_sha256"]:
                raise ValueError("structural score evidence changed")
            score = json.loads(path.read_text())
            fields = score["fields"]
            layouts[f"{fields['frames_version']}/{fields['channel_record_size']}"] += 1
            for frame in score["frames"]:
                for channel in frame["changed_channels"]:
                    index = channel["index"]
                    version = file["director_version"]
                    main_channels = 2 if version == 500 else 6
                    if index >= main_channels:
                        max_sprite = max(max_sprite, index - main_channels + 1)
                    elif version == 500 and index == 0:
                        raw = bytes.fromhex(channel["record_hex"])
                        tempos[f"D5/{raw[21]}"] += 1
                    elif index == 1 and version != 500:
                        raw = bytes.fromhex(channel["record_hex"])
                        tempos[f"{raw[6]}/{int.from_bytes(raw[4:6], 'big')}"] += 1
    value = {
        "schema_version": 1,
        "game": game.slug,
        "source_sha256": game.source["sha256"],
        "stage": "source-recovery",
        "native_execution_verified": False,
        "original_projector_compared": False,
        "n64_hardware_validated": False,
        "input_sha256": {key: file_sha256(path) for key, path in paths.items()},
        "source_counts": source["summary"],
        "score_counts": scores["summary"],
        "score_layouts_format_and_record_bytes": dict(sorted(layouts.items())),
        "max_recovered_sprite_channel": max_sprite,
        "max_cast_libraries": max(len(f["cast_links"]) for f in source["files"]),
        "tempo_opcode_and_operand_observations": dict(sorted(tempos.items())),
        "launcher_handlers": len(launcher_handlers),
        "xtras": dict(sorted(xtras.items())),
        "native_lowering_requirements": dict(sorted(requirements.items())),
        "handlers_requiring_lowering": blockers,
    }
    target = analysis / "recovery.json"
    target.write_text(json.dumps(value, indent=2) + "\n")
    print(
        f"Recovered source/bytecode identities agree: {len(program['handlers'])} media handlers, "
        f"{len(launcher_handlers)} launcher handlers; {len(blockers)} require native lowering"
    )
    print(target)
    return value
