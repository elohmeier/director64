from copy import deepcopy

import pytest
from director64_findus_workshop.full_accountability import build


def fixture():
    script = {
        "id": "s1",
        "member_id": "m1",
        "content_sha256": "hash",
        "lingo": "on boot\nend\n",
        "handlers": [{"name": "boot", "id": "h1"}],
    }
    source = {
        "files": [
            {
                "name": "START.DXR",
                "libraries": [{"library_id": 1, "name": "Internal"}],
                "members": [{"id": "m1", "library_id": 1, "member_id": 5}],
                "scripts": [script],
            }
        ],
        "supplemental_projector": {"scripts": [deepcopy(script)]},
    }
    program = {
        "handlers": [
            {"movie": "START.DXR", "name": "boot", "cast": "Internal", "member": 5, "body": []}
        ]
    }
    aot = {"movies": [{"movie": "START.DXR", "handlers": 1, "file": "start_dxr.c"}]}
    return source, program, aot


def test_maps_native_handlers_and_equivalent_launcher():
    report = build(*fixture())
    assert report["counts"]["media_scripts"] == 1
    assert report["projector"][0]["equivalent_start_script"] == "s1"
    assert report["native_execution_verified"] is False


def test_missing_native_handler_or_changed_launcher_fails():
    source, program, aot = fixture()
    program["handlers"][0]["member"] = 6
    with pytest.raises(ValueError, match="mapping"):
        build(source, program, aot)
    source, program, aot = fixture()
    source["supplemental_projector"]["scripts"][0]["lingo"] += "different"
    with pytest.raises(ValueError, match="startup differs"):
        build(source, program, aot)
