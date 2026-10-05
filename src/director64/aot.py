"""The converter's compiler stage, as the pipeline and the tests reach it.

The bytecode compiler itself is the Rust crate under compiler/
(`director64-aot`, docs/roadmap.md Track C); this module runs it, prepares
the program it reads, and keeps the source-analysis helpers that the
recovery report and the score generator share with it.
"""

from __future__ import annotations

import argparse
import copy
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from .lingo import LingoError


def native_blockers(handler: dict) -> list[str]:
    """Report recovered forms that still require explicit native lowering.

    Every recovered tell form now lowers: the stage inline, window tells
    through the controller-window services, and the remaining sprite-valued
    targets through the sprite timeline services.
    """
    del handler
    return []


def normalize_handler(handler: dict) -> dict:
    """Lower collection loops and cases while evaluating their selectors once.

    Hidden locals cannot collide with Lingo identifiers. They also keep objects
    alive across native yields. The D8 bytecode caches both list and loop count.
    """
    result = copy.deepcopy(handler)
    serial = 0

    def temporary(kind):
        nonlocal serial
        serial += 1
        return ["variable", f"${kind}{serial}"]

    def block(nodes):
        output = []
        for node in nodes:
            line = node["line"]
            for key in ("yes", "no", "body"):
                if key in node:
                    node[key] = block(node[key])
            if node["op"] == "tell" and node["target"] == ["the", "stage"]:
                output.extend(
                    [
                        {"op": "call", "line": line, "value": ["call", "tell_stage", []]},
                        *node["body"],
                        {"op": "call", "line": line, "value": ["call", "tell_end", []]},
                    ]
                )
            elif node["op"] == "tell" and node["target"][:2] == ["call", "window"]:
                output.extend(
                    [
                        {
                            "op": "call",
                            "line": line,
                            "value": ["call", "tell_window", node["target"][2]],
                        },
                        *node["body"],
                        {"op": "call", "line": line, "value": ["call", "tell_end", []]},
                    ]
                )
            elif node["op"] == "tell":
                # Remaining tells target sprites, as sprite(n) references or
                # sprite values; they drive that sprite's flattened film
                # timeline, and handler sends retarget to its behaviors.
                channel = temporary("tellsprite")

                def retarget(value, channel=channel):
                    if not isinstance(value, list):
                        return value
                    if value == ["the", "frame"]:
                        return ["call", "sprite_frame", [channel]]
                    if value == ["the", "lastframe"]:
                        return ["call", "sprite_lastframe", [channel]]
                    return [retarget(item) for item in value]

                def resprite(statements, channel=channel):
                    result = []
                    for inner in statements:
                        inner = {
                            key: resprite(value)
                            if key in ("yes", "no", "body")
                            else retarget(value)
                            if isinstance(value, list)
                            else value
                            for key, value in inner.items()
                        }
                        value = inner.get("value")
                        if inner["op"] == "call" and value[1] == "go" and len(value[2]) == 1:
                            inner["value"] = ["call", "sprite_go", [channel, value[2][0]]]
                        elif (
                            inner["op"] == "call"
                            and value[0] == "call"
                            and value[1]
                            not in (
                                "sendsprite",
                                "sprite_go",
                                "sprite_frame",
                                "sprite_lastframe",
                            )
                        ):
                            inner["value"] = [
                                "call",
                                "sendsprite",
                                [channel, ["symbol", value[1]], *value[2]],
                            ]
                        result.append(inner)
                    return result

                target = (
                    node["target"][2]
                    if node["target"][:2] == ["reference", "sprite"]
                    else node["target"]
                )
                output.extend(
                    [
                        {"op": "set", "line": line, "target": channel, "value": retarget(target)},
                        *resprite(node["body"]),
                    ]
                )
            elif node["op"] == "delete" and node["target"][0] == "variable":
                # Deleting a variable disposes its instance: assign VOID.
                node.update(op="set", value=["variable", "void"])
                output.append(node)
            elif node["op"] == "unrecovered":
                # The pinned decompiler marked untranslatable bytecode; lower
                # to the explicit alert-and-continue runtime failure.
                output.append(
                    {
                        "op": "call",
                        "line": line,
                        "value": [
                            "call",
                            "undecompiled_bytecode",
                            [["string", node["opcode"]]],
                        ],
                    }
                )
            elif node["op"] == "foreach":
                seq, bound, index = (temporary(kind) for kind in ("sequence", "count", "index"))
                output.extend(
                    [
                        {"op": "set", "line": line, "target": seq, "value": node["sequence"]},
                        {
                            "op": "set",
                            "line": line,
                            "target": bound,
                            "value": ["call", "count", [seq]],
                        },
                        {
                            "op": "for",
                            "line": line,
                            "variable": index[1],
                            "first": ["number", 1],
                            "last": bound,
                            "step": 1,
                            "body": [
                                {
                                    "op": "set",
                                    "line": line,
                                    "target": ["variable", node["variable"]],
                                    "value": ["call", "getat", [seq, index]],
                                },
                                *node["body"],
                            ],
                        },
                    ]
                )
            elif node["op"] == "case":
                selector = temporary("case")
                tail = []
                for branch in reversed(node["branches"]):
                    body = block(branch["body"])
                    if branch["values"] is None:
                        tail = body
                        continue
                    conditions = [["binary", "=", selector, value] for value in branch["values"]]
                    condition = conditions[0]
                    for extra in conditions[1:]:
                        condition = ["binary", "or", condition, extra]
                    tail = [
                        {"op": "if", "line": line, "condition": condition, "yes": body, "no": tail}
                    ]
                output.append(
                    {"op": "set", "line": line, "target": selector, "value": node["selector"]}
                )
                output.extend(tail)
            else:
                output.append(node)
        return output

    result["body"] = block(result["body"])
    return result


def text_hash(name: str) -> int:
    """FNV-1a over the ASCII-folded UTF-8 name, matching `lv_text_hash` in
    runtime/lingo/lingo_runtime.c: the key of every generated name index.

    The runtime folds A-Z only (tolower in the C locale leaves bytes above
    127 alone), so folding A-Z is the whole rule. The hash only has to agree
    with itself: the runtime still compares every candidate's text.
    """
    digest = 2166136261
    for byte in name.encode():
        if 65 <= byte <= 90:
            byte += 32
        digest = ((digest ^ byte) * 16777619) & 0xFFFFFFFF
    return digest


def cstring(value: str) -> str:
    # Octal-escape UTF-8 bytes: C hex escapes can consume a following hex digit.
    return (
        '"'
        + "".join(
            chr(b) if 32 <= b < 127 and b not in (34, 92) else f"\\{b:03o}" for b in value.encode()
        )
        + '"'
    )


# Bytecode contract shared with runtime/lingo/lingo_bytecode.h;
# tests/test_aot_bytecode.py compares the two tables. Operand kinds are u8,
# i8, u16, i32 and pc (a state index the assembler resolves to a u16 byte
# offset), encoded big-endian so the bytes are the same on the host probe
# and the console. Every op below "jump" pushes, pops or stores over the
# running frame's temps exactly as the lx_* service of the same name; every
# op from "jump" on ends a state and returns its flow.


RUST_GENERATOR = Path("compiler/target/release/director64-aot")


def generator_command(root: Path) -> list[str]:
    """The bytecode compiler the pipeline runs: the Rust crate's binary, which
    `mise run setup` (or `mise run compiler`) builds."""
    binary = root / RUST_GENERATOR
    if not binary.is_file():
        raise ValueError(f"{binary} is not built; run mise run compiler")
    return [str(binary), "--names", str(root / "runtime/lingo/names.txt")]


def repository_root() -> Path:
    return Path(__file__).resolve().parents[2]


def generate(program: dict, output: Path) -> dict:
    """Compile a parsed program into bytecode units under `output` and return
    the manifest the compiler wrote (tests build their fixtures this way)."""
    output.mkdir(parents=True, exist_ok=True)
    root = repository_root()
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as handle:
        json.dump(program, handle)
        path = Path(handle.name)
    try:
        completed = subprocess.run(
            [*generator_command(root), str(path), str(output)],
            capture_output=True,
            text=True,
            check=False,
        )
    finally:
        path.unlink(missing_ok=True)
    if completed.returncode:
        raise LingoError(completed.stderr.strip() or completed.stdout.strip())
    return json.loads((output / "manifest.json").read_text())


def executable_program(game) -> Path:
    """Include an embedded projector as a native startup unit when selected.

    Keep the recovered media AST unchanged, and record the original launcher
    identity on each copied handler for source accountability.
    """
    path = game.work / "aot/program.json"
    movie = game.data["port"].get("launcher_movie")
    compatibility = game.data["port"].get("native_compatibility", False)
    if not movie and not compatibility:
        return path
    program = json.loads(path.read_text())
    if compatibility:
        program = game.host("compatibility").apply(program, game.source["sha256"])
    if movie:
        launcher = json.loads((game.work / "analysis/launcher-program.json").read_text())
        if any(h["movie"] == movie for h in program["handlers"]):
            raise LingoError("embedded launcher conflicts with a media movie")
        for handler in launcher["handlers"]:
            handler["source_movie"] = handler["movie"]
            handler["movie"] = movie
            program["handlers"].append(handler)
    output = game.work / "aot/native-program.json"
    output.write_text(json.dumps(program, indent=2) + "\n")
    return output


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("program", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    command = [*generator_command(repository_root()), str(args.program), str(args.output)]
    sys.exit(subprocess.run(command, check=False, env=os.environ).returncode)


if __name__ == "__main__":
    main()
