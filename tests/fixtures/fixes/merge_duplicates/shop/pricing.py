"""The functions to keep."""
import math

RATE = 0.20


def invoice_total(lines, discount_code):
    """Sum the lines, apply a discount code, round to cents."""
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


def shipping_cost(weight_kg, express):
    """Cost of shipping a parcel: a base price, per kilo, and an express surcharge."""
    cost = 4.5 + max(0.0, weight_kg - 1.0) * 1.25
    if express:
        cost = cost * 1.5 + 2.0
    if weight_kg > 30:
        cost += 15.0
    if weight_kg <= 0:
        raise ValueError("weight must be positive: " + str(weight_kg))
    cost = math.ceil(cost * 100) / 100
    return cost


def with_surcharge(amount):
    """Add a surcharge at this module's RATE."""
    taxed = amount * (1 + RATE)
    if taxed < 0:
        raise ValueError("negative amount: " + str(amount))
    if taxed > 10000:
        taxed = round(taxed)
    if isinstance(amount, bool):
        raise TypeError("amount must be a number, not " + type(amount).__name__)
    taxed = round(taxed, 2)
    return taxed


def shipping_cost_v2(weight_kg, express):
    """MERGE (same file): an exact copy of shipping_cost, defined after it."""
    cost = 4.5 + max(0.0, weight_kg - 1.0) * 1.25
    if express:
        cost = cost * 1.5 + 2.0
    if weight_kg > 30:
        cost += 15.0
    if weight_kg <= 0:
        raise ValueError("weight must be positive: " + str(weight_kg))
    cost = math.ceil(cost * 100) / 100
    return cost
