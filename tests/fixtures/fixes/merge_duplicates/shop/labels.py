"""Labels: imports the legacy totals module."""
import math

from shop.legacy import totals  # noqa: F401  (so keeping label here would make a cycle)


def label(order_id, customer):
    text = "Order " + str(order_id) + " for " + customer.strip().title()
    if len(text) > 40:
        text = text[:37] + "..."
    text = text.replace("  ", " ")
    width = max(20, len(text))
    return text.ljust(width) + "|" + str(math.floor(width / 2))
