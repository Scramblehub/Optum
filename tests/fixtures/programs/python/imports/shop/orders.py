import json

from shop import pricing as p
from shop.pricing import price_with_tax


def place_order(items):
    subtotal = sum(i["price"] for i in items)
    subtotal = p.discount(subtotal, 10)
    return json.dumps({"total": price_with_tax(subtotal)})
