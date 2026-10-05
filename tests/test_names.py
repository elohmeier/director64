"""The name vocabulary: one list, two generated files, and every runtime compare in it."""

from director64 import names


def test_generated_files_are_current():
    table = names.load()
    assert names.HEADER.read_text() == names.render_header(table)
    assert names.TABLE.read_text() == names.render_table(table)


def test_every_runtime_literal_is_in_the_vocabulary():
    table = names.load()
    missing = names.runtime_literals() - set(table)
    assert not missing, sorted(missing)


def test_ids_follow_sorted_order():
    table = names.load()
    assert list(table.values()) == list(range(len(table)))
    assert list(table) == sorted(table)
