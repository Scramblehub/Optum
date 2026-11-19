"""Reads the items table: same package as writer.py, so not a second tool."""


def list_items(db):
    return db.execute("SELECT name FROM items").fetchall()
