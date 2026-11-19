"""Planted unused code, and decoys that look unused but aren't."""

__all__ = ["_exported_helper"]  # a name listed in __all__ is public API, even with "_"

HANDLERS = {}


def register(fn):
    HANDLERS[fn.__name__] = fn
    return fn


@register
def _on_refund(amount):  # decoy: registered by its decorator, called through HANDLERS
    return -amount


def _format_total(total):  # decoy: used as a value (a dispatch table)
    return f"{total:.2f}"


FORMATTERS = {"total": _format_total}


def _legacy_rounding(x):  # PLANTED: nothing uses it
    return round(x, 1)


def _old_tax_table():  # PLANTED: nothing uses it
    return {"standard": 0.2, "reduced": 0.05}


def _exported_helper():  # decoy: listed in __all__
    return 1


def _only_tested(x):  # PLANTED (test-only): only tests/test_invoices.py calls it
    return x * 2


class Invoice:
    def __init__(self, total):  # decoy: a dunder
        self.total = total

    def _recompute(self):  # PLANTED: a private method nothing calls
        return self.total * 1.0

    def _by_name(self):  # decoy: looked up by name with getattr
        return "by name"

    def describe(self):  # decoy: public API
        return getattr(self, "_by_name")()

    def _helper_used(self):  # decoy: called by a method
        return self.total

    def amount(self):
        return self._helper_used()

    def visit_line(self, line):  # decoy: a framework hook, called by name
        return line


def public_entry(total):  # decoy: public, may be called from outside the project
    inv = Invoice(total)
    return FORMATTERS["total"](inv.amount()) + inv.describe()
