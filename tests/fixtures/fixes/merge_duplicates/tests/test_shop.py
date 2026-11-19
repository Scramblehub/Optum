import unittest

from shop import export, labels, pricing, report
from shop.legacy import totals

LINES = [{"price": 10.0, "quantity": 2}, {"price": 5.0, "quantity": 1, "refunded": True}]


class ShopTest(unittest.TestCase):
    def test_totals(self):
        self.assertEqual(pricing.invoice_total(LINES, "VIP"), 16.0)
        self.assertEqual(totals.old_invoice_total(LINES, "WELCOME10"), 18.0)

    def test_shipping(self):
        self.assertEqual(pricing.shipping_cost(2.0, True), 10.63)
        self.assertEqual(pricing.shipping_cost_v2(2.0, False), 5.75)

    def test_tax_differs(self):
        self.assertEqual(pricing.with_surcharge(100), 120.0)
        self.assertEqual(totals.old_with_surcharge(100), 125.0)

    def test_labels(self):
        self.assertEqual(labels.label(1, "ann"), totals.old_label(1, "ann"))

    def test_rows(self):
        self.assertEqual(report.format_row({"a": 1}), export.format_line({"a": 1}))



class MoneyTest(unittest.TestCase):
    def test_money(self):
        from shop.legacy.money import LegacyAmount
        from shop.money import Box, Item, Money, Price, Refund
        self.assertEqual(Money(1250, "GBP").as_text(), "GBP 12.50")
        self.assertEqual(Price(-305, "EUR").as_text(), "-EUR 3.05")
        self.assertEqual(Refund(700, "JPY").as_text(), "-JPY 700")
        self.assertEqual(LegacyAmount(5, "USD").as_text(), "USD 0.05")
        self.assertEqual(Item().describe("x"), "X: Item (2)")
        self.assertEqual(Box().describe("y"), "Y: Box (2)")



class CycleTest(unittest.TestCase):
    def test_both(self):
        from shop import alpha, beta
        self.assertEqual(alpha.weighted_sum([1, None, 2]), beta.weighted_sum([1, None, 2]))
        self.assertEqual(alpha.keywords("the cat sat on the mat", 3), beta.keywords("the cat sat on the mat", 3))


if __name__ == "__main__":
    unittest.main()
