"""Old copies."""
import math

from shop import labels

RATE = 0.25  # not the RATE of shop.pricing


def old_invoice_total(lines, discount_code):
    """MERGE: an exact copy of shop.pricing.invoice_total (only builtins as globals)."""
    subtotal = 0.0
    for line in lines:
        if line.get("refunded"):
            continue
        subtotal += line["price"] * line["quantity"]
    if discount_code == "WELCOME10":
        subtotal = subtotal * 0.9
    elif discount_code == "VIP":
        subtotal = subtotal * 0.8
    return round(subtotal, 2)


def old_with_surcharge(amount):
    """REFUSE: the same code as shop.pricing.with_surcharge, but RATE means 0.25 here."""
    taxed = amount * (1 + RATE)
    if taxed < 0:
        raise ValueError("negative amount: " + str(amount))
    if taxed > 10000:
        taxed = round(taxed)
    if isinstance(amount, bool):
        raise TypeError("amount must be a number, not " + type(amount).__name__)
    taxed = round(taxed, 2)
    return taxed


def old_label(order_id, customer):
    """REFUSE: an exact copy of shop.labels.label, but shop.labels imports this module (a cycle)."""
    text = "Order " + str(order_id) + " for " + customer.strip().title()
    if len(text) > 40:
        text = text[:37] + "..."
    text = text.replace("  ", " ")
    width = max(20, len(text))
    return text.ljust(width) + "|" + str(math.floor(width / 2))
