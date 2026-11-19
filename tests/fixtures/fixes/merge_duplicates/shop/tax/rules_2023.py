"""REFUSE: this year's VAT rules. The same code as the other year's, but each year stays as filed."""


def vat_due(net_amount, category):
    if category == "standard":
        rate = 0.20
    elif category == "reduced":
        rate = 0.05
    elif category == "zero":
        rate = 0.0
    else:
        raise ValueError("unknown VAT category: " + str(category))
    due = round(net_amount * rate, 2)
    return {"net": net_amount, "rate": rate, "due": due, "gross": round(net_amount + due, 2)}
