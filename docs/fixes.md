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
functions and methods of top-level classes. Callers don't change: the old name now is the kept one.

- A **function** copy becomes an import of the kept one, under the copy's name.
- A **method** copy is deleted when its class directly subclasses the kept method's class (one
  base): the class inherits it. Otherwise it becomes `name = Kept.name` in the class body (a Python
  method is a plain function, so it works as this class's method, `self` and all), with
  `from mod import Kept` added if the kept class is in another module.

```python
from shop.pricing import invoice_total as old_invoice_total  # merged by optum: an exact copy of shop.pricing.invoice_total
shipping_cost_v2 = shipping_cost  # (in the same file: an assignment after the kept function)

class Price(Money):
    pass  # as_text is inherited from shop.money.Money.as_text (merged by optum)

class Refund:
    as_text = Money.as_text  # merged by optum: an exact copy of shop.money.Money.as_text
```

The kept copy is a function over a method, then the one in the shortest module path (the most
central), then the one defined first in its file, then the first by id.

### When it refuses

A copy is merged only if all of these hold. Otherwise it is listed as refused, with the reason.

1. **The same code:** equal syntax trees (Python's `ast`; names and docstrings aside), no
   decorators, no `global` or `nonlocal`; both functions, or both methods of top-level classes.
   Methods also must not use `super()`, `__class__` or private `__names`: they mean the class the
   code is written in, so a copy elsewhere means something else. In one file, the kept class (or
   method) must come first.
2. **The same globals, meaning the same things.** Identical text can call different code: each
   module's own `TAX_RATE`, `helper` or `join`. Every global the body reads (Python's `symtable`)
   must be a builtin in both modules, the same import, or the same constant. A name defined in
   each module is refused even when the definitions look alike.
3. **No import cycle:** the kept function's module doesn't import the copy's module, counting the
   imports earlier merges of the same run add (two merges can make a cycle together: found on
   sympy, where one merge made `theanocode` import `aesaracode` and two others the reverse).
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
has 12 exact copies and a unittest suite. 6 are safe to merge: three functions (across modules, in
one file) and three copies of a method (in a subclass, in an unrelated class in the same file, in
a class in another module). 6 are traps: a pair in `shop/alpha.py` and `shop/beta.py` that would
import each other if both merged, the same code reading a `RATE` that's 0.20 in one module
and 0.25 in the other, a copy whose kept module imports it (a cycle), a copy calling a `helper`
defined in each module, `vat_due` in `shop/tax/rules_2023.py` and `rules_2024.py` (regulated: each
year stays as filed), and a method copy using `super()`.
[check_fix.py](../tests/tools/check_fix.py) runs three ways in a fresh copy:

| Test | Checks |
|---|---|
| `apply` | 6 merged, 6 refused with the right reasons; each rewrite as expected; the tests pass before, after, and again afterwards |
| `dry` | the diff is printed and no file changes |
| `revert` | a test the merge breaks (`__name__`) fails after the change, and every file is restored |

On the Python standard library (a dry run), it merges nothing and refuses all 8 exact copies,
correctly. Six are methods (not handled yet). `posixpath.abspath` and `ntpath._abspath_fallback`
are the same text, but `isabs`, `join` and `normpath` are POSIX in one and Windows in the other:
merging them would have been a real bug. `sndhdr.testall` and `imghdr.testall` each call their own
module's `what`.

### Not yet

Copies in sibling classes could share a new base class or mixin rather than point at one of
them; near copies need a merged version, not one kept; other languages.

On nine real projects ([note](notes/2026-11-19-real-repos.md)) it found 1 safe merge, in sympy,
applied with sympy's `printing` tests passing before and after (1,112 tests); every other exact
copy was refused for a right reason, mostly methods.
