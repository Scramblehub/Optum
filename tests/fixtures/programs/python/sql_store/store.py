"""SQL through sqlite3: table reads and writes from literal queries, including a join."""
import sqlite3


def connect(path=":memory:"):
    db = sqlite3.connect(path)
    db.execute("CREATE TABLE IF NOT EXISTS customers (id INTEGER PRIMARY KEY, email TEXT)")
    db.execute(
        "CREATE TABLE IF NOT EXISTS orders (id INTEGER PRIMARY KEY, customer_id INTEGER, total REAL)"
    )
    return db


def add_customer(db, email):
    cur = db.execute("INSERT INTO customers (email) VALUES (?)", (email,))
    return cur.lastrowid


def add_order(db, customer_id, total):
    db.execute("INSERT INTO orders (customer_id, total) VALUES (?, ?)", (customer_id, total))


def revenue_by_customer(db):
    return db.execute(
        "SELECT c.email, SUM(o.total) FROM orders o "
        "JOIN customers c ON c.id = o.customer_id GROUP BY c.email"
    ).fetchall()


def delete_customer(db, customer_id):
    db.execute("DELETE FROM orders WHERE customer_id = ?", (customer_id,))
    db.execute("DELETE FROM customers WHERE id = ?", (customer_id,))


if __name__ == "__main__":
    db = connect()
    cid = add_customer(db, "a@example.com")
    add_order(db, cid, 9.5)
    print(revenue_by_customer(db))
