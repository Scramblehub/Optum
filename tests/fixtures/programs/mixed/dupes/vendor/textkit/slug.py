"""A vendored library (vendor/): its own copies are left out."""


def slugify(text, max_length=0):
    text = text.strip().lower()
    out = []
    for ch in text:
        if ch.isalnum():
            out.append(ch)
        elif ch in " -_." and out and out[-1] != "-":
            out.append("-")
    while out and out[-1] == "-":
        out.pop()
    result = "".join(out)
    return result[:max_length] if max_length else result


def slugify_legacy(text, max_length=0):
    """A copy inside the vendored library: not the project's to change."""
    text = text.strip().lower()
    out = []
    for ch in text:
        if ch.isalnum():
            out.append(ch)
        elif ch in " -_." and out and out[-1] != "-":
            out.append("-")
    while out and out[-1] == "-":
        out.pop()
    result = "".join(out)
    return result[:max_length] if max_length else result
