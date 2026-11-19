"""Decoys: the same names by coincidence, and fields in common by coincidence (no shared id)."""
from dataclasses import dataclass


@dataclass
class Customer:  # decoy: called Customer, but a marketing segment, with no id or email to tie it
    name: str
    segment: str
    lifetime_value: float
    phone: str


@dataclass
class Hotel:  # decoy: city and phone in common with the restaurants table, nothing that identifies
    name: str
    city: str
    phone: str
    stars: int


def init(db):
    db.execute("CREATE TABLE IF NOT EXISTS restaurants (id INTEGER PRIMARY KEY, name TEXT, city TEXT, "
               "phone TEXT, cuisine TEXT)")
