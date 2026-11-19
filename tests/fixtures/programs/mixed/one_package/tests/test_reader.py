"""Test code reaches what the code under test reaches: not waste."""


def test_items(db):
    return db.execute("SELECT count(*) FROM items").fetchone()
