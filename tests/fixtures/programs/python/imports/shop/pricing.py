TAX = 0.2


def price_with_tax(amount):
    return round(amount * (1 + TAX), 2)


def discount(amount, pct):
    return amount * (1 - pct / 100)
