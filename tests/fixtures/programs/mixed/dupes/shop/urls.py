def make_slug(text, max_length=0):
    """REPORTED: the project's own copy of vendor/textkit/slug.py's slugify (import it instead)."""
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
