"""MERGE weighted_sum here (kept); REFUSE keywords (merging it too would make a cycle)."""


def weighted_sum(values):
    total = 0
    for i, value in enumerate(values):
        if value is None:
            continue
        weight = 1.0 + i / max(1, len(values))
        total += value * weight
    if total > 1000:
        total = 1000.0
    return round(total, 3)


def helper_a():
    return 1


def helper_b():
    return 2


def keywords(text, limit):
    out = []
    for word in text.split():
        if len(word) < 3 or word.isdigit():
            continue
        word = word.strip(".,;:!?").lower()
        if word and word not in out:
            out.append(word)
    out.sort(key=len)
    return out[:limit]
