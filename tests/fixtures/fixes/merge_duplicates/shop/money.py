"""Methods copied between classes."""


class Money:
    def __init__(self, cents, currency):
        self.cents = cents
        self.currency = currency

    def as_text(self):
        """Format like 'GBP 12.50', with a minus sign for negative amounts."""
        sign = "-" if self.cents < 0 else ""
        whole, part = divmod(abs(self.cents), 100)
        if self.currency in ("JPY", "KRW"):
            return sign + self.currency + " " + str(abs(self.cents))
        return sign + self.currency + " " + str(whole) + "." + str(part).rjust(2, "0")


class Price(Money):
    def as_text(self):
        """MERGE (subclass): an exact copy of Money.as_text; Price inherits it, so the copy goes."""
        sign = "-" if self.cents < 0 else ""
        whole, part = divmod(abs(self.cents), 100)
        if self.currency in ("JPY", "KRW"):
            return sign + self.currency + " " + str(abs(self.cents))
        return sign + self.currency + " " + str(whole) + "." + str(part).rjust(2, "0")


class Refund:
    def __init__(self, cents, currency):
        self.cents = -cents
        self.currency = currency

    def as_text(self):
        """MERGE (unrelated class, same file): becomes as_text = Money.as_text."""
        sign = "-" if self.cents < 0 else ""
        whole, part = divmod(abs(self.cents), 100)
        if self.currency in ("JPY", "KRW"):
            return sign + self.currency + " " + str(abs(self.cents))
        return sign + self.currency + " " + str(whole) + "." + str(part).rjust(2, "0")


class Base:
    def describe(self, prefix):
        return prefix + ": " + type(self).__name__


class Item(Base):
    def describe(self, prefix):
        """REFUSE: super() means Item's place in the hierarchy; a copy in Box would mean Box's."""
        text = super().describe(prefix)
        if len(text) > 30:
            text = text[:27] + "..."
        parts = text.split(":")
        return parts[0].upper() + ":" + ":".join(parts[1:]) + " (" + str(len(parts)) + ")"


class Box(Base):
    def describe(self, prefix):
        text = super().describe(prefix)
        if len(text) > 30:
            text = text[:27] + "..."
        parts = text.split(":")
        return parts[0].upper() + ":" + ":".join(parts[1:]) + " (" + str(len(parts)) + ")"
