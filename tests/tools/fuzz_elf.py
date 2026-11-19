#!/usr/bin/env python3
"""Robustness check for the ELF reader and binary frontend: binaries in a repo are untrusted input.

Mutates real ELF files (byte flips, truncation, corrupted header and section fields) and runs
`optum graph` on each. A clean error or a skipped file is fine; a crash, hang or sanitizer report
is a failure.

Usage: fuzz_elf.py --optum OPTUM --seed-binary FILE [--seed-binary FILE ...] [--cases N] [--seed S]
"""
import argparse
import concurrent.futures
import os
import random
import struct
import subprocess
import sys
import tempfile


def sections(b):
    """(name, offset, size) of each section, read from the section headers."""
    try:
        shoff, = struct.unpack_from("<Q", b, 40)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", b, 58)
        heads = [struct.unpack_from("<IIQQQQ", b, shoff + i * shentsize) for i in range(shnum)]
        stroff = heads[shstrndx][4]
        out = []
        for name, _type, _flags, _addr, off, size in heads:
            end = b.index(0, stroff + name)
            out.append((bytes(b[stroff + name:end]).decode("ascii", "replace"), off, size))
        return out
    except (struct.error, ValueError, IndexError):
        return []


def mutate(data, rng):
    b = bytearray(data)
    debug = [s for s in sections(b) if s[0].startswith(".debug_") and s[2] > 0 and s[1] + s[2] <= len(b)]
    choice = rng.randrange(7 if debug else 5)
    if choice == 0:  # random byte flips
        for _ in range(rng.randint(1, 64)):
            b[rng.randrange(len(b))] = rng.randrange(256)
    elif choice == 1:  # truncate
        b = b[: rng.randrange(1, len(b))]
    elif choice == 2:  # corrupt an ELF header field (offsets, counts, sizes)
        off = rng.choice([24, 32, 40, 54, 56, 58, 60, 62])
        width = 8 if off in (24, 32, 40) else 2
        b[off:off + width] = rng.randbytes(width)
    elif choice >= 5:  # corrupt bytes inside debug info (the DWARF reader's input)
        _name, off, size = rng.choice(debug)
        for _ in range(rng.randint(1, 16)):
            b[off + rng.randrange(size)] = rng.randrange(256)
    else:  # corrupt a section header field
        shoff, shentsize, shnum = struct.unpack_from("<QHH", b, 40)[0], *struct.unpack_from("<HH", b, 58)
        if shnum and shoff + shnum * 64 <= len(b):
            h = shoff + rng.randrange(shnum) * 64 + rng.choice([4, 8, 16, 24, 32, 40, 44, 56])
            b[h:h + 4] = rng.randbytes(4)
    return bytes(b)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--optum", required=True)
    ap.add_argument("--seed-binary", action="append", required=True)
    ap.add_argument("--cases", type=int, default=200)
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    seeds = [open(p, "rb").read() for p in args.seed_binary]
    bad = 0
    with tempfile.TemporaryDirectory() as d:
        # The mutants come from one random sequence, so make them all first (same cases as ever),
        # then run them side by side: each `optum graph` run is independent.
        paths = []
        for case in range(args.cases):
            paths.append(os.path.join(d, f"mutant{case}"))
            with open(paths[-1], "wb") as f:
                f.write(mutate(rng.choice(seeds), rng))

        def run(path):
            try:
                return subprocess.run([args.optum, "graph", path, "-o", os.devnull], capture_output=True, text=True, timeout=30)
            except subprocess.TimeoutExpired:
                return None

        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 1) as pool:
            results = list(pool.map(run, paths))
        for case, (path, r) in enumerate(zip(paths, results)):
            if r is None:
                print(f"case {case}: timeout")
                bad += 1
                continue
            crashed = r.returncode < 0 or "Sanitizer" in r.stderr or "runtime error" in r.stderr or "terminate called" in r.stderr
            if crashed:
                bad += 1
                keep = os.path.join(os.getcwd(), f"fuzz_crash_{case}.bin")
                open(keep, "wb").write(open(path, "rb").read())
                print(f"case {case}: crashed (exit {r.returncode}), kept as {keep}\n{r.stderr[-1500:]}")
    print(f"{args.cases} mutated binaries: {'no crashes' if not bad else f'{bad} failures'}")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
