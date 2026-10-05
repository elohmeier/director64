"""Explicit port corrections for the pinned Mucklas source, before native lowering."""

import copy

from director64.lingo import LingoError

SOURCE = "ad4c7afb4b19339c45c2a69756b522b59ba4869199740fd528527f190a806925"
SCRIPT = "d2db226e23a14304b8953395fdbe7684f6109f2bdd386609d3db07ce1d49e906"


def apply(program: dict, source_sha256: str) -> dict:
    """Use BR's keyed spacing table at its two authored symbolic getAt sites.

    Bytecode extcall offsets 546/579 confirm getAt was authored. ScummVM's
    getAt accepts numeric indices only. This is a selected-port correction,
    not a claim that Director generally accepts symbolic getAt arguments.
    """
    result = copy.deepcopy(program)
    matches = [
        h
        for h in result["handlers"]
        if (h["movie"], h["cast"], h["member"], h["name"]) == ("BR.DXR", "Internal", 4, "ritabana")
    ]
    if source_sha256 != SOURCE or len(matches) != 1 or matches[0]["source_sha256"] != SCRIPT:
        raise LingoError("Mucklas BR compatibility source identity changed; review required")
    count = 0

    def replace(node):
        nonlocal count
        if node == ["call", "getat", [["variable", "avstlist"], ["variable", "a"]]]:
            node[1] = "getaprop"
            count += 1
        elif isinstance(node, dict):
            for value in node.values():
                replace(value)
        elif isinstance(node, list):
            for value in node:
                replace(value)

    replace(matches[0]["body"])
    if count != 2:
        raise LingoError("Mucklas BR compatibility call sites changed; review required")
    result["compatibility_fixes"] = [
        {
            "id": "br-symbolic-spacing",
            "movie": "BR.DXR",
            "member": 4,
            "handler": "ritabana",
            "source_sha256": SCRIPT,
            "bytecode_offsets": [546, 579],
            "calls_changed": count,
            "from": "getat",
            "to": "getaprop",
            "original_projector_verified": False,
        }
    ]
    return result
