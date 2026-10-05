"""Replace this disc's print composition with a fixed-document phone handoff."""

import copy

from director64.lingo import LingoError

SOURCE = "f31970b980f096a343a9aec5f10d52931709fbdda3cf09e5a80023791e402af2"
SCRIPT = "0687160c839bd70d5c6a07c1ddb955f06de6b93c23bc24f2b499f8bcf9724901"


def apply(program, source_sha256):
    result = copy.deepcopy(program)
    matches = [
        h
        for h in result["handlers"]
        if (h["movie"], h["cast"], h["member"], h["name"])
        == ("CURSOR.CXT", "External", 90, "drucken")
    ]
    if source_sha256 != SOURCE or len(matches) != 1 or matches[0]["source_sha256"] != SCRIPT:
        raise LingoError("Löwenzahn print source identity changed; review required")
    handler = matches[0]
    if handler["parameters"] != ["me", "logobreite"]:
        raise LingoError("Löwenzahn print parameters changed")
    handler["body"] = [
        {
            "op": "call",
            "line": handler["line"],
            "value": [
                "call",
                "director64_print",
                [["variable", "mbuch"], ["variable", "mkapitel"]],
            ],
        }
    ]
    result["compatibility_fixes"] = [
        {
            "id": "phone-printing",
            "movie": "CURSOR.CXT",
            "member": 90,
            "handler": "drucken",
            "source_sha256": SCRIPT,
            "from": "PrintOMatic page composition",
            "to": "fixed PDF QR handoff",
            "original_projector_verified": False,
        }
    ]
    return result
