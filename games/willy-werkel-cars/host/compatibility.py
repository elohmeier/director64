"""Explicit, source-pinned dispositions for platform-specific desktop code."""

import copy

SOURCE = "938be20a50e2825f5acf4788aabfdc145b94d1a1fb61b7264a4a81156428afac"
WINDOW = "9cb4f829e9e23a5271fd9568ca2e71bd5f0eee9d18368bc95c1437a710e7ec0d"
OPENER = "b8cdef14b9e36e5fb60da419e8af19c7fc6841c965fa318a7eda2bb109224500"
PRINT = "408f357aa657c52dfd018fbd3759b070f44a55ba66a9dbb86ae33d2677d388ac"


def apply(program, source_sha256):
    if source_sha256 != SOURCE:
        raise ValueError("Willy compatibility source changed")
    result = copy.deepcopy(program)
    handlers = [
        h
        for h in result["handlers"]
        if (h["movie"], h["member"], h["name"]) == ("06.DXR", 2, "closewindow")
    ]
    if len(handlers) != 1 or handlers[0]["source_sha256"] != WINDOW:
        raise ValueError("Willy external chooser source changed")
    body = next(n for n in handlers[0]["body"] if n["op"] == "if")["yes"]
    tell = body[0]
    if tell["op"] != "tell" or tell["target"] != ["variable", "mymiaw"]:
        raise ValueError("Willy external chooser context changed")
    # Keep a fail-closed native trap for external PC file selection. This path
    # is separate from the six in-game profiles and car slots in the database.
    body[0:1] = [
        {"op": "call", "line": tell["line"], "value": ["call", "tell_window", [tell["target"]]]},
        *tell["body"],
    ]
    openers = [
        h
        for h in result["handlers"]
        if (h["movie"], h["member"], h["name"]) == ("06.DXR", 2, "openwindow")
    ]
    if len(openers) != 1 or openers[0]["source_sha256"] != OPENER:
        raise ValueError("Willy external chooser opener changed")
    # The opener deactivates every save-screen button before creating the
    # desktop MIAW; without a window the alert recovery would leave the screen
    # dead. Trap the whole handler before any deactivation: the click raises
    # the recoverable unsupported-chooser alert and the screen stays usable.
    opener_line = openers[0]["body"][1]["line"]
    openers[0]["body"] = [
        {"op": "call", "line": opener_line, "value": ["call", "open_window_trap", []]}
    ]
    printers = [h for h in result["handlers"]
                if (h["movie"], h["member"], h["name"]) == ("08.DXR", 38, "print")]
    if len(printers) != 1 or printers[0]["source_sha256"] != PRINT:
        raise ValueError("Willy certificate print source changed")
    printers[0]["body"] = [{
        "op": "call", "line": printers[0]["body"][0]["line"],
        "value": ["call", "certificate_print_notice", []],
    }]
    result["compatibility_fixes"] = [
        {
            "id": "certificate-print-notice",
            "movie": "08.DXR",
            "member": 38,
            "handler": "print",
            "source_sha256": PRINT,
            "disposition": "dismissible-platform-notice",
            "original_projector_verified": False,
        },
        {
            "id": "external-file-chooser-trap",
            "movie": "06.DXR",
            "member": 2,
            "handler": "closewindow",
            "source_sha256": WINDOW,
            "disposition": "explicit-runtime-trap",
            "original_projector_verified": False,
        },
        {
            "id": "external-file-chooser-opener-trap",
            "movie": "06.DXR",
            "member": 2,
            "handler": "openwindow",
            "source_sha256": OPENER,
            "disposition": "recoverable-runtime-trap",
            "original_projector_verified": False,
        },
    ]
    return result
