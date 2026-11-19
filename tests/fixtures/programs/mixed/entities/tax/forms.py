"""Decoys: regulated forms (tax). They share an email with the customer, but stay as they are."""
from dataclasses import dataclass


@dataclass
class W9Form:
    name: str
    business_name: str
    tin: str
    address: str
    email: str


def store(db, form):
    db.execute("CREATE TABLE IF NOT EXISTS tax_forms (id INTEGER PRIMARY KEY, name TEXT, business_name TEXT, "
               "tin TEXT, address TEXT, email TEXT UNIQUE)")
