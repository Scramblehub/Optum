def helper(x):
    return str(x).strip()


def format_line(row):
    cells = []
    for key in sorted(row):
        value = helper(row[key])
        if not value:
            value = "-"
        cells.append(key + "=" + value)
    return " | ".join(cells) + " (" + str(len(cells)) + " cells)"
