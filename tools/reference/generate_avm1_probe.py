#!/usr/bin/env python3
"""Generate synthetic SWFs for original AVM1 semantics; no retail input.

Each movie exposes `report` for Director's getVariable and writes the same
result through trace() in an original standalone debugger. Keep outputs in
ignored build/. A completed report distinguishes a tolerated operation from
an action-list abort. Record the loaded player version alongside results.
"""
from pathlib import Path
import argparse
import json
import struct
from generate_filter_probe import rect, tag


def push(value):
    if value is None:
        data = b"\x03"  # undefined
    elif isinstance(value, str):
        data = b"\0" + value.encode("utf-8") + b"\0"
    elif isinstance(value, bool):
        data = b"\x05" + bytes([value])
    else:
        data = b"\x07" + struct.pack("<i", value)
    return b"\x96" + struct.pack("<H", len(data)) + data


NULL = b"\x96\x01\0\x02"
def get(name): return push(name) + b"\x1c"
def assign(name, value): return push(name) + value + b"\x1d"


def cases():
    result = {}
    for name, owner in (("undefined-write", push(None)), ("null-write", NULL),
                        ("number-write", push(3)), ("string-write", push("x"))):
        result[name] = owner + push("value") + push(17) + b"\x4f" + assign("report", push("after"))
    result["undefined-call"] = push(0) + push(None) + push("missing") + b"\x52\x17" + assign("report", push("after"))
    result["undefined-number"] = assign("report", push(None) + b"\x4a")
    result["undefined-string"] = assign("report", push("[") + push(None) + b"\x4b\x47" + push("]") + b"\x47")
    result["undefined-property"] = assign("report", push(None) + push("x") + b"\x4e\x44")
    return result


def unicode_cases():
    """SWF6+ uses UTF-8 literals and UTF-16 String indices."""
    text = "Grüße😀!"

    def method(owner, name, *args):
        return b"".join(push(v) for v in reversed(args)) + push(len(args)) + push(owner) + push(name) + b"\x52"

    expressions = {
        "unicode-length": push(text) + push("length") + b"\x4e",
        "unicode-char-code": method(text, "charCodeAt", 2),
        "unicode-char": method(text, "charAt", 2),
        "unicode-substring": method(text, "substring", 2, 5),
        "unicode-substr": method(text, "substr", 2, 3),
        "unicode-upper": method(text, "toUpperCase"),
        "unicode-lower-sigma": method("ΟΣ", "toLowerCase"),
        "unicode-lower-dotted-i": method("İ", "toLowerCase"),
        "unicode-upper-ligature": method("ﬃ", "toUpperCase"),
        "unicode-high-surrogate": method(text, "charCodeAt", 5),
        "unicode-low-surrogate": method(text, "charCodeAt", 6),
    }
    return {name: assign("report", expression) for name, expression in expressions.items()}


def super_cases():
    def action(op, data): return bytes([op]) + struct.pack("<H", len(data)) + data
    def function(name, body, preload=False):
        header = name.encode() + b"\0\0\0"
        if preload: header += b"\x02\x10\0"
        return action(0x8e if preload else 0x9b, header + struct.pack("<H", len(body))) + body
    def prop(owner, key): return owner + push(key) + b"\x4e"
    def setprop(owner, key, value): return owner + push(key) + value + b"\x4f"
    def prototype(name): return prop(get(name), "prototype")
    def new(name): return push(0) + push(name) + b"\x40"
    def method(owner, key): return push(0) + owner + push(key) + b"\x52"
    def append(marker): return assign("constructors", get("constructors") + push(marker) + b"\x47")
    parent_call = push(0) + push("super") + b"\x3d\x17"
    prefix = assign("constructors", push(""))
    prefix += function("Base", append("B") + b"\0")
    prefix += function("Child", parent_call + append("C") + b"\0")
    prefix += function("Grand", parent_call + append("G") + b"\0")
    prefix += setprop(get("Child"), "prototype", new("Base"))
    prefix += setprop(get("Grand"), "prototype", new("Child"))
    prefix += function("baseMethod", prop(get("this"), "value") + b"\x3e")
    prefix += function("childMethod", method(get("super"), "method") + push(10) + b"\x47\x3e")
    prefix += function("grandMethod", method(get("super"), "method") + push(20) + b"\x47\x3e")
    for name, method_name in [("Base", "baseMethod"), ("Child", "childMethod"), ("Grand", "grandMethod")]:
        prefix += setprop(prototype(name), "method", get(method_name))
    prefix += assign("instance", new("Grand")) + setprop(get("instance"), "value", push(7))
    result = {
        "super-method": prefix + assign("report", method(get("instance"), "method")),
        "super-inherited-method": prefix + prototype("Grand") + push("method") + b"\x3a\x17" + assign("report", method(get("instance"), "method")),
        "super-constructor": prefix + assign("constructors", push("")) + new("Grand") + b"\x17" + assign("report", get("constructors")),
    }
    reset = setprop(prototype("Child"), "constructor", get("Child"))
    reset += setprop(prototype("Grand"), "constructor", get("Grand"))
    result["super-constructor-reset"] = prefix + reset + assign("constructors", push("")) + new("Grand") + b"\x17" + assign("report", get("constructors"))
    result["super-type"] = prefix + function("superType", get("super") + b"\x44\x3e") + setprop(prototype("Child"), "type", get("superType")) + assign("report", method(get("instance"), "type"))
    result["super-data"] = prefix + setprop(prototype("Base"), "data", push(5)) + setprop(prototype("Child"), "data", push(9)) + function("superData", prop(get("super"), "data") + b"\x3e") + setprop(prototype("Child"), "read", get("superData")) + assign("report", method(get("instance"), "read"))
    register1 = b"\x96\x02\0\x04\x01"
    result["super-preload"] = prefix + function("superPreload", method(register1, "method") + b"\x3e", True) + setprop(prototype("Child"), "preloaded", get("superPreload")) + assign("report", method(get("instance"), "preloaded"))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    manifest = []
    for version in (5, 6, 7):
        for name, actions in (cases() | ((unicode_cases() | super_cases()) if version >= 6 else {})).items():
            label = f"{name}-v{version}"
            code = assign("report", push("before")) + actions
            code += push(f"CASE {label} ") + get("report") + b"\x47\x26"
            code += push("AVM1_PROBE_DONE") + b"\x26\x07\0"
            body = rect(0, 0, 640, 640) + b"\0\x0c\x01\0" + tag(12, code) + tag(1) + tag(0)
            data = b"FWS" + bytes([version]) + struct.pack("<I", len(body) + 8) + body
            filename = label + ".swf"
            (args.output / filename).write_bytes(data)
            manifest.append({"file": filename, "report": "before until action completes"})
    (args.output / "inputs.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
