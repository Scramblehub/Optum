"""Check graph JSON files against docs/ir.schema.json.

Usage: validate_schema.py SCHEMA [--expect-fail] FILE...
Exits 0 when every file matches the expectation, 1 otherwise.
"""
import json
import sys

import jsonschema


def main(argv: list[str]) -> int:
    schema_path, rest = argv[0], argv[1:]
    expect_fail = rest[:1] == ["--expect-fail"]
    files = rest[1:] if expect_fail else rest
    with open(schema_path) as f:
        validator = jsonschema.Draft202012Validator(json.load(f))
    bad = 0
    for path in files:
        with open(path) as f:
            errors = list(validator.iter_errors(json.load(f)))
        if bool(errors) != expect_fail:
            bad += 1
            detail = errors[0].message if errors else "passed, but should have failed"
            print(f"{path}: {detail}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
