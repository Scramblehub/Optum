from billing.invoices import _only_tested


def test_only_tested():
    assert _only_tested(2) == 4
