"""Source-pinned lowerings for the two MX 2004 JavaScript-dialect handlers.

The pinned decompiler reports both as ``undecompiled-bytecode-unk26``: their
Lscr bytecode is the JavaScript stub ``44 01 44 02 26`` (pushcons, pushcons,
opcode 0x26). The compiled function payloads sit in the same chunks' literal
pools and identify the sources completely:

- ``TasksScripts.cxt`` chunk 228 (member 9) holds ``Function``/``int2hex``
  with parameter ``num``, a call to ``toString`` and the integer 16 —
  ``function int2hex(num) { return num.toString(16) }``. Its callers build
  the exercises' percent-encoded UTF-8 text ("%" & int2hex(n1) & …).
- ``ZMScripts.cxt`` chunk 566 (member 58) holds ``Function``/
  ``clearGarbage``/``_system``/``gc`` —
  ``function clearGarbage() { _system.gc() }``.

Both lower to the runtime's ``js_*`` natives, which implement exactly those
JavaScript semantics (lowercase hex; collection is automatic).
"""

import copy

SOURCE = "4c87e10dcc99dfb68882b51c9daceb7ce4761b482a2a43bf620093289353e0f4"
INT2HEX = "400f1d944fee13df345a3ccb97a597363c5edde995cd689359145e1e8185abac"
CLEARGARBAGE = "515abd3ad032882fdf3ff3d1d8e2468de62f28da773fa5cec0ae1b127a475822"


def apply(program, source_sha256):
    if source_sha256 != SOURCE:
        raise ValueError("Deutsch compatibility source changed")
    result = copy.deepcopy(program)
    fixes = []
    for spec in (
        {
            "id": "javascript-int2hex",
            "movie": "TASKSCRIPTS.CXT",
            "member": 9,
            "name": "int2hex",
            "sha": INT2HEX,
            "parameters": ["num"],
            "body": [
                {
                    "op": "return",
                    "line": 30,
                    "value": ["call", "js_int2hex", [["variable", "num"]]],
                }
            ],
            "disposition": "recovered-javascript-function",
        },
        {
            "id": "javascript-cleargarbage",
            "movie": "ZMSCRIPTS.CXT",
            "member": 58,
            "name": "cleargarbage",
            "sha": CLEARGARBAGE,
            "parameters": [],
            "body": [
                {
                    "op": "call",
                    "line": 7405,
                    "value": ["call", "js_cleargarbage", []],
                }
            ],
            "disposition": "recovered-javascript-function",
        },
    ):
        matches = [
            h
            for h in result["handlers"]
            if (h["movie"], h["member"], h["name"])
            == (spec["movie"], spec["member"], spec["name"])
        ]
        if len(matches) != 1 or matches[0]["source_sha256"] != spec["sha"]:
            raise ValueError(f"Deutsch {spec['name']} source changed")
        body = matches[0]["body"]
        if (
            len(body) != 1
            or body[0].get("op") != "unrecovered"
            or body[0].get("opcode") != "unk26"
        ):
            raise ValueError(f"Deutsch {spec['name']} bytecode shape changed")
        if matches[0]["parameters"]:
            raise ValueError(f"Deutsch {spec['name']} parameters changed")
        matches[0]["parameters"] = spec["parameters"]
        matches[0]["body"] = spec["body"]
        fixes.append(
            {
                "id": spec["id"],
                "movie": spec["movie"],
                "member": spec["member"],
                "handler": spec["name"],
                "source_sha256": spec["sha"],
                "disposition": spec["disposition"],
                "original_projector_verified": False,
            }
        )
    result["compatibility_fixes"] = fixes
    return result
