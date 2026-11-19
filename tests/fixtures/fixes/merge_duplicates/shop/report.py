"""REFUSE: format_row calls helper(), which is defined here and in shop/export.py."""


def helper(x):
    return str(x).strip()


def format_row(row):
    cells = []
    for key in sorted(row):
        value = helper(row[key])
        if not value:
            value = "-"
        cells.append(key + "=" + value)
    return " | ".join(cells) + " (" + str(len(cells)) + " cells)"
