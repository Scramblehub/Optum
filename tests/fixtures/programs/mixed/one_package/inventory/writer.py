"""Writes the items table."""


def add_item(db, name):
    db.execute("INSERT INTO items (name) VALUES (?)", (name,))
