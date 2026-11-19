"""Planted clones (see expected_diagnosis.json) and decoys."""


def invoice_total(lines, tax_rate, discount_code):
    """Family A, original: an exact copy is in legacy/totals.py."""
    subtotal = 0.0
    for line in lines:
        if line.get("refunded"):
            continue
        subtotal += line["price"] * line["quantity"]
    if discount_code == "WELCOME10":
        subtotal = subtotal * 0.9
    elif discount_code == "VIP":
        subtotal = subtotal * 0.8
    tax = round(subtotal * tax_rate, 2)
    return {"subtotal": round(subtotal, 2), "tax": tax, "total": round(subtotal + tax, 2)}


def parse_shipping_address(raw):
    """Family B, original: a copy with renamed names and values is in shop/addresses.py."""
    parts = [p.strip() for p in raw.split(",")]
    if len(parts) < 3:
        raise ValueError("incomplete address: " + raw)
    street, city, postcode = parts[0], parts[1], parts[2]
    country = parts[3] if len(parts) > 3 else "GB"
    if not postcode.replace(" ", "").isalnum():
        raise ValueError("bad postcode: " + postcode)
    return {"street": street, "city": city.title(), "postcode": postcode.upper(), "country": country}


def stock_alerts(products, threshold):
    """Family C, original: an edited copy is in legacy/stock.py."""
    alerts = []
    for product in products:
        level = product["stock"]
        if level <= 0:
            alerts.append((product["sku"], "out of stock"))
        elif level < threshold:
            alerts.append((product["sku"], "low: " + str(level)))
        if product.get("discontinued") and level > 0:
            alerts.append((product["sku"], "discontinued with stock"))
    alerts.sort()
    return alerts


def order_summary(orders):
    """Decoy: the same idioms as stock_alerts (a loop, conditions, appends), different logic."""
    by_status = {}
    for order in orders:
        status = order.get("status", "new")
        if status not in by_status:
            by_status[status] = []
        by_status[status].append(order["id"])
        if order.get("priority"):
            by_status.setdefault("priority", []).append(order["id"])
    return {k: sorted(v) for k, v in by_status.items()}


def get_name(item):  # decoy: tiny look-alike helpers, below the size limit
    return item["name"]


def get_sku(item):
    return item["sku"]
