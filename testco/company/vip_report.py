#!/usr/bin/env python3
"""The VIP list: customers with five or more orders, for the hand-kept spreadsheet.

clean_address and normalize_email were copied from nightly_sync.py when this was written.
"""
import collections

import requests

from common import SHOP, SHOP_AUTH


def clean_address(street, city, postcode):
    """One address line, tidied: single spaces, title-case city, upper-case postcode."""
    parts = []
    for value in (street, city, postcode):
        value = " ".join(str(value or "").split())
        if not value:
            continue
        parts.append(value)
    if len(parts) >= 2:
        parts[1] = parts[1].title()
    if len(parts) == 3:
        parts[2] = parts[2].upper()
    return ", ".join(parts)


def normalize_email(email):
    """Lower case, no spaces, and Gmail-style dots kept (they matter for most providers)."""
    email = (email or "").strip().lower()
    if email.count("@") != 1:
        return None
    local, domain = email.split("@")
    if not local or "." not in domain:
        return None
    return local + "@" + domain


def vips(min_orders=5):
    counts, who = collections.Counter(), {}
    page = 1
    while True:
        batch = requests.get(SHOP + "/orders", params={"per_page": 100, "page": page, "status": "completed"},
                             auth=SHOP_AUTH, timeout=60).json()
        if not batch:
            break
        for o in batch:
            email = normalize_email(o["billing"]["email"])
            counts[email] += 1
            who[email] = (f"{o['billing']['first_name']} {o['billing']['last_name']}",
                          clean_address(o["billing"]["address_1"], o["billing"]["city"], o["billing"]["postcode"]))
        page += 1
    return [(email, *who[email], n) for email, n in counts.most_common() if n >= min_orders]


if __name__ == "__main__":
    for email, name, address, n in vips():
        print(f"{name}\t{email}\t{address}\t{n} orders")
