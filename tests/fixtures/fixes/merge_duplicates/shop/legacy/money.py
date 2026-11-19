"""An old money class with a copied method (another module)."""


class LegacyAmount:
    def __init__(self, cents, currency):
        self.cents = cents
        self.currency = currency

    def as_text(self):
        """MERGE (another module): becomes as_text = Money.as_text, with an import of Money."""
        sign = "-" if self.cents < 0 else ""
        whole, part = divmod(abs(self.cents), 100)
        if self.currency in ("JPY", "KRW"):
            return sign + self.currency + " " + str(abs(self.cents))
        return sign + self.currency + " " + str(whole) + "." + str(part).rjust(2, "0")
