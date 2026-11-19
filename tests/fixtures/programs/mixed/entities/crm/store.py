import sqlite3

SCHEMA = """CREATE TABLE IF NOT EXISTS customers (
    id INTEGER PRIMARY KEY, email TEXT UNIQUE, first_name TEXT, last_name TEXT, phone TEXT, created_at TEXT)"""


def init(db):
    db.execute(SCHEMA)  # customer, place 2 of 4
    db.execute("CREATE TABLE IF NOT EXISTS orders (id INTEGER PRIMARY KEY, customer_email TEXT, "
               "total_cents INTEGER, currency TEXT, shipping_address TEXT, created_at TEXT)")  # order, place 2 of 3
    db.execute("CREATE TABLE IF NOT EXISTS products (id INTEGER PRIMARY KEY, name TEXT, created_at TEXT, "
               "price_cents INTEGER, sku TEXT)")  # decoy: shares only id, name, created_at with AuditEvent
    db.execute("CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT)")  # decoy


def connect():
    return sqlite3.connect("crm.db")
