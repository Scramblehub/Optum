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
        self.assertEqual(pricing.with_tax(100), 120.0)
        self.assertEqual(totals.old_with_tax(100), 125.0)

    def test_labels(self):
        self.assertEqual(labels.label(1, "ann"), totals.old_label(1, "ann"))

    def test_rows(self):
        self.assertEqual(report.format_row({"a": 1}), export.format_line({"a": 1}))


if __name__ == "__main__":
    unittest.main()
