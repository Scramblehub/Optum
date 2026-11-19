def read_address(text):
    """Family B: shop.pricing.parse_shipping_address with every name and value renamed."""
    fields = [f.strip() for f in text.split(";")]
    if len(fields) < 3:
        raise ValueError("address too short: " + text)
    road, town, zipcode = fields[0], fields[1], fields[2]
    nation = fields[3] if len(fields) > 3 else "US"
    if not zipcode.replace("-", "").isalnum():
        raise ValueError("invalid zip: " + zipcode)
    return {"road": road, "town": town.title(), "zip": zipcode.upper(), "nation": nation}
