from copy import deepcopy

import pytest

from director64.lingo import parse_movie
from director64.recovery import validate_handlers


def fixture():
    source = {
        "files": [
            {
                "name": "SHARED.CXT",
                "members": [{"id": "member-hash", "library_id": 1024, "member_id": 3}],
                "libraries": [{"library_id": 1024, "name": "External"}],
                "scripts": [{"member_id": "member-hash", "handlers": [{"name": "new"}]}],
            }
        ],
    }
    program = {
        "handlers": parse_movie(
            "-- cast: External; member: 3; type: MovieScript; name: actor\n"
            "property position\non new me\n  position = point(1, 2)\n  return me\nend\n",
            "SHARED.CXT",
        )
    }
    return source, program


def test_script_identity_agreement_accepts_native_object_lowering():
    source, program = fixture()
    assert validate_handlers(source, program) == []


@pytest.mark.parametrize(
    "field,value",
    [
        ("cast", "Internal"),
        ("member", 4),
        ("name", "start"),
        ("movie", "OTHER.CXT"),
    ],
)
def test_same_handler_count_cannot_hide_changed_bindings(field, value):
    source, program = fixture()
    modified = deepcopy(program)
    modified["handlers"][0][field] = value
    with pytest.raises(ValueError, match="identities"):
        validate_handlers(source, modified)


def test_missing_or_duplicated_handlers_are_rejected():
    source, program = fixture()
    for handlers in ([], program["handlers"] * 2):
        with pytest.raises(ValueError, match="identities"):
            validate_handlers(source, {"handlers": handlers})
