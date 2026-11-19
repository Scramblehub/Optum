def stock_warnings(products, threshold):
    """Family C: shop.pricing.stock_alerts with small edits (a new condition, a changed message)."""
    alerts = []
    for product in products:
        level = product["stock"]
        if level <= 0:
            alerts.append((product["sku"], "out of stock"))
        elif level < threshold:
            alerts.append((product["sku"], "running low: " + str(level)))
        if product.get("discontinued") and level > 0:
            alerts.append((product["sku"], "discontinued with stock"))
        if product.get("backordered"):
            alerts.append((product["sku"], "backordered"))
    alerts.sort()
    return alerts
