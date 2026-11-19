# Binary lifting: APIs and tables reached by compiled code (2026-11-18)

Stage 4 of the [binary plan](../plans/binary.md).

## The test

The C `http_glue` sample does a familiar job: it fetches orders from a shop API, creates and closes
deals in a CRM, and logs to sqlite. It's compiled into the corpus at five levels and in four forms
(debug, symbols, stripped, bare), linked against stub `libcurl.so.4` and `libsqlite3.so.0` that have
the real sonames and ABI values. Its hand-checked source graph names what it must reach:

| Reached | Kind | How the URL is written in the source |
|---|---|---|
| `http:shop.example.com/api/orders` | reads (GET) | `#define` string + `"?since=today"` |
| `http:crm.example.com/api/deals` | sends_to (POST) | `static const char *` |
| `http:crm.example.com/api/deals/{}` | sends_to (PATCH) | `snprintf` into a stack buffer |
| `sql:orders` | writes | literal SQL |

## Result

**20 of 20 binaries reach exactly these, with the right methods,** including `bare` (no symbols,
no unwind tables) at every level. At `-O0`, where nothing is inlined, each is reached from the same
function as in the source. At `-O2` everything is inlined into one function, and grouping libcurl
options by `perform` keeps the three requests apart.

Function and call scores on the corpus didn't change (`bare`: functions 97.0% / 98.2%, calls 98.9%
/ 94.4%).

## How the values are found

At `-O2`:

```
lea  rdx, [rip+0xd91]     ; the URL string in .rodata
mov  esi, 0x2712          ; CURLOPT_URL (10002)
mov  rdi, rbx             ; the handle
call curl_easy_setopt@plt
```

At `-O0`, the same values pass through stack slots, and the `static const char *` arrives as a load
from `.data`, which is resolved through its `R_X86_64_RELATIVE` relocation. The `snprintf` call
leaves a template in its stack buffer (`https://crm.example.com/api/deals/{}`), and the later
`CURLOPT_URL` reads it back.

## Also in this branch

- **Unversioned imports** (sqlite, most libraries other than glibc) were `lib:?` whenever a binary
  needs more than one library. They're now resolved by looking up the needed libraries the way the
  dynamic linker does.
- **Fuzzing** now includes the glue binary, so the lifter runs on mutated input too: 400 extra
  mutants locally, no crashes.

## Not covered

- Control flow: values are followed in address order.
- Libraries other than libcurl, sqlite, libpq and MySQL.
- Calls through function pointers.
- Strings built at runtime other than with `snprintf`/`sprintf`/`strcpy`.
