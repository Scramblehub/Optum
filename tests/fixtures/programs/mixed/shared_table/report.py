"""Diagnose sample: a nightly report. Shares only the customers table with signup.js."""
import sqlite3

import requests


def send_report(db):
    emails = db.execute("SELECT email FROM customers").fetchall()
    return requests.post("https://mail.example.com/api/send", json={"to": "ops", "count": len(emails)}, timeout=10)


if __name__ == "__main__":
    send_report(sqlite3.connect(":memory:"))
