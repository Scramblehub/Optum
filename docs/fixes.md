# Automatic fixes

Optum's findings ([diagnose.md](diagnose.md)) say what's wasted. A fix acts on one, and only when
it is safe and the project's tests agree. There is one fix so far, as the [Phase 2
plan](plans/phase2.md) set out: merging exact duplicate Python functions.

## Merging exact duplicates

```sh
python3 Core/fixers/merge_duplicates.py path/to/project --optum build/release/Core/optum            # the diff
python3 Core/fixers/merge_duplicates.py path/to/project --optum build/release/Core/optum \
    --test "python3 -m pytest -q" --apply --report fix.json                                        # change it
```

It reads `optum diagnose <dir> -f json` and takes the **exact** copies among top-level Python
functions. Each copy's `def` is replaced by an import of the kept one, under the copy's name, so
callers don't change: the old name now is the kept function.

```python
from shop.pricing import invoice_total as old_invoice_total  # merged by optum: an exact copy of shop.pricing.invoice_total
shipping_cost_v2 = shipping_cost  # (in the same file: an assignment after the kept function)
```

The kept copy is the one in the shortest module path (the most central), then the first by id.

### When it refuses

A copy is merged only if all of these hold. Otherwise it is listed as refused, with the reason.

1. **The same code:** equal syntax trees (Python's `ast`; the names of the functions and docstrings
   aside), both top-level functions, no decorators, no `global` or `nonlocal`.
2. **The same globals, meaning the same things.** Identical text can call different code: each
   module's own `TAX_RATE`, `helper` or `join`. Every global the body reads (Python's `symtable`)
   must be a builtin in both modules, the same import, or the same constant. A name defined in
   each module is refused even when the definitions look alike.
3. **No import cycle:** the kept function's module doesn't import the copy's module.
4. **Not regulated code.** Tax, legal and medical code is often duplicated on purpose: this
   year's VAT rules next to last year's, each kept as it was filed. Copies whose module path or
   name has a word like `tax`, `vat`, `w9`, `1099`, `legal`, `contract`, `consent`, `medical`,
   `patient`, `hipaa` are refused, unless `--allow-regulated` (the same words as the same-entity
   analysis, [diagnose.md](diagnose.md#the-same-data-in-several-places)).
5. **The tests agree** (with `--test`): they must pass before the change, and after it; if they
   fail after, every file is restored.

Without `--apply`, it only prints the diff.

### What changes for the program

The copy's name now refers to the kept function, so `old_invoice_total.__name__` is
`"invoice_total"`, and `__module__` is the kept function's module. Code that looks functions up by
`__name__` (registries, some serialisers) can notice; that's what the test gate is for (the
`revert` test checks it).

### How it's checked

The sample project [tests/fixtures/fixes/merge_duplicates](../tests/fixtures/fixes/merge_duplicates)
has 6 exact copy pairs and a unittest suite. 2 are safe to merge (one across modules, one in the same
file); 4 are traps: the same code reading a `RATE` that's 0.20 in one module and 0.25 in the other,
a copy whose kept module imports it (a cycle), a copy calling a `helper` defined in each module,
and `vat_due` in `shop/tax/rules_2023.py` and `rules_2024.py` (regulated: each year stays as filed).
[check_fix.py](../tests/tools/check_fix.py) runs three ways in a fresh copy:

| Test | Checks |
|---|---|
| `apply` | 2 merged, 4 refused with the right reasons; the tests pass before, after, and again afterwards |
| `dry` | the diff is printed and no file changes |
| `revert` | a test the merge breaks (`__name__`) fails after the change, and every file is restored |

On the Python standard library (a dry run), it merges nothing and refuses all 8 exact copies,
correctly. Six are methods (not handled yet). `posixpath.abspath` and `ntpath._abspath_fallback`
are the same text, but `isabs`, `join` and `normpath` are POSIX in one and Windows in the other:
merging them would have been a real bug. `sndhdr.testall` and `imghdr.testall` each call their own
module's `what`.

### Not yet

Methods (a shared base class or a mixin would be the fix), near copies (they need a merged
version, not one kept), and other languages.

On nine real projects ([note](notes/2026-11-19-real-repos.md)) it found 1 safe merge, in sympy,
applied with sympy's `printing` tests passing before and after (1,112 tests); every other exact
copy was refused for a right reason, mostly methods.
