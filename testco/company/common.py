"""Settings shared by the company's scripts: where its tools live and how to log in.

These are the company's own scripts (the glue Optum analyses and, in stage 1, replaces); they
call the tools by their internal names, through the recording proxy.
"""
import os

SHOP = os.environ.get("SHOP_URL", "http://shop.testco.internal:8080/wp-json/wc/v3")
SHOP_AUTH = ("admin", "testcoShopApiPassword00")
ACCOUNTING = os.environ.get("ACCOUNTING_URL", "http://accounting.testco.internal:8069/jsonrpc")
ACCOUNTING_DB, ACCOUNTING_LOGIN, ACCOUNTING_PASSWORD = "testco", "admin", "admin"
PAYMENTS = os.environ.get("PAYMENTS_URL", "http://payments.testco.internal:12111")
PAYMENTS_KEY = "sk_test_123"
HOOKS = os.environ.get("HOOKS_URL", "http://hooks.testco.internal:3000")
