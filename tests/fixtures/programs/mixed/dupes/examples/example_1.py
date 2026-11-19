"""Example 1: examples are standalone on purpose, so their copies are left out."""


def demo_total(lines, tax_rate, discount_code):
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
