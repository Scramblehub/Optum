#!/usr/bin/env python3
"""Build the binary reference corpus and its answer keys (docs/plans/binary.md, stage 2).

For every project in tests/corpus/projects.json and every optimisation level:
  - compile each source once with gcc (-g, -fPIC, -fcallgraph-info), then link each binary;
  - keep four forms of every binary: debug/ (as built), symbols/ (strip --strip-debug),
    stripped/ (strip --strip-all) and bare/ (stripped, and without the .eh_frame unwind tables,
    which otherwise give away every function boundary: the case of other compilers, of
    -fno-asynchronous-unwind-tables builds and of much Windows code);
  - write truth/<binary>.json, the answer key, read from the debug build:
      functions  every function in the binary: address, size, static or not, its source file and
                 line, and its source id in the C frontend's form (c:<file>#<name>). Pieces GCC
                 split off or cloned (name.cold, name.part.0, name.isra.0...) have "fragment" set
                 and their parent function's source id
      calls      the direct calls the compiler emitted at this level (from -fcallgraph-info),
                 between the binary's own functions, as source ids; inlined calls are gone
      imports    shared libraries needed and the functions imported from them

Usage:
  build_corpus.py --out DIR [--with-sqlite] [--only PROJECT] [--jobs N]
  build_corpus.py --out DIR --check        sanity-check a built corpus

Uses gcc, strip, objcopy, readelf and addr2line. Optional projects (sqlite) are downloaded once into
DIR/_downloads and checked against a pinned SHA3-256.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SPEC = os.path.join(REPO, "tests", "corpus", "projects.json")


def run(cmd, cwd=None):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)}\n{r.stderr[-2000:]}")
    return r.stdout


def opt_name(opt):
    return opt.lstrip("-")


def fetch(project, out):
    d = project["download"]
    cache = os.path.join(out, "_downloads")
    os.makedirs(cache, exist_ok=True)
    archive = os.path.join(cache, os.path.basename(d["url"]))
    if not os.path.exists(archive):
        print(f"downloading {d['url']}", file=sys.stderr)
        urllib.request.urlretrieve(d["url"], archive + ".part")
        os.replace(archive + ".part", archive)
    digest = hashlib.sha3_256(open(archive, "rb").read()).hexdigest()
    if digest != d["sha3_256"]:
        os.remove(archive)
        raise RuntimeError(f"{archive}: SHA3-256 {digest} does not match the pinned {d['sha3_256']}")
    root = os.path.join(cache, d["strip_dir"])
    if not os.path.isdir(root):
        with zipfile.ZipFile(archive) as z:
            z.extractall(cache)
    return root


def sources_of(project, binary):
    out = []
    for s in binary["sources"]:
        out += project["library"] if s == "@library" else [s]
    return out


def obj_stem(src):
    return src[:-2].replace("/", "__")


# ------------------------------------------------------------------ answer keys

SYM = re.compile(r"^\s*\d+:\s+([0-9a-f]+)\s+(\d+)\s+(\w+)\s+(\w+)\s+\w+\s+(\S+)\s+(\S+)")


def symtab(binary_path, dynamic=False, table=None):
    """Symbols from readelf. --syms lists both .dynsym and .symtab; `table` keeps one of them."""
    rows = []
    current = ""
    for line in run(["readelf", "--dyn-syms" if dynamic else "--syms", "-W", binary_path]).splitlines():
        t = re.match(r"Symbol table '([^']+)'", line)
        if t:
            current = t.group(1)
            continue
        m = SYM.match(line)
        if m and (table is None or current == table):
            value, size, typ, bind, ndx, name = m.groups()
            rows.append({"value": int(value, 16), "size": int(size), "type": typ, "bind": bind, "ndx": ndx,
                         "name": name.split("@")[0]})
    return rows


def functions_of(debug_path, project_root):
    # readelf --syms lists .dynsym and .symtab: a shared library's exports appear in both.
    seen, funcs = set(), []
    for sym in symtab(debug_path):
        if sym["type"] == "FUNC" and sym["ndx"] != "UND" and sym["size"] > 0 and (sym["name"], sym["value"]) not in seen:
            seen.add((sym["name"], sym["value"]))
            funcs.append(sym)
    if not funcs:
        return []
    out = run(["addr2line", "-f", "-e", debug_path] + [hex(f["value"]) for f in funcs]).splitlines()
    result = []
    for f, (_, loc) in zip(funcs, zip(out[0::2], out[1::2])):
        file, _, line = loc.rpartition(":")
        file = os.path.normpath(file) if file not in ("", "??") else ""
        line = int(line.split()[0]) if line[:1].isdigit() else 0
        in_project = bool(file) and not os.path.isabs(file) and not file.startswith("..")
        # GCC splits and clones functions when optimising: gz_compress.cold, f.part.0, f.isra.0,
        # f.constprop.0. They're separate code in the binary but belong to one source function.
        base, dot, suffix = f["name"].partition(".")
        entry = {"name": f["name"], "address": hex(f["value"]), "size": f["size"], "static": f["bind"] == "LOCAL",
                 "file": file if in_project else "", "line": line if in_project else 0}
        if dot:
            entry["fragment"] = suffix
        entry["source_id"] = f"c:{file}#{base}" if in_project else ""
        entry["origin"] = "project" if in_project else "runtime"  # crt / libc startup code
        result.append(entry)
    return sorted(result, key=lambda e: int(e["address"], 16))


CI_GRAPH = re.compile(r'graph: \{ title: "([^"]+)"')
CI_EDGE = re.compile(r'edge: \{ sourcename: "([^"]+)" targetname: "([^"]+)"')


def calls_of(ci_files, functions):
    """The compiler's direct calls, as source ids. A static is titled "file.c:name" in .ci files."""
    by_name = {}
    statics = {}
    for f in functions:
        if f["origin"] != "project":
            continue
        if f["static"]:
            statics[(f["file"], f["name"])] = f["source_id"]
            statics[(os.path.basename(f["file"]), f["name"])] = f["source_id"]
        else:
            by_name[f["name"]] = f["source_id"]

    def resolve(title, tu):
        if ":" in title:
            file, name = title.rsplit(":", 1)
            return statics.get((file, name)) or statics.get((os.path.basename(file), name))
        return by_name.get(title) or statics.get((tu, title)) or statics.get((os.path.basename(tu), title))

    calls, external, indirect = set(), set(), 0
    for ci in ci_files:
        text = open(ci).read()
        m = CI_GRAPH.search(text)
        tu = m.group(1) if m else ""
        for src, dst in CI_EDGE.findall(text):
            a = resolve(src, tu)
            if not a:
                continue
            if dst == "__indirect_call":
                indirect += 1
                continue
            b = resolve(dst, tu)
            if b:
                if a != b:  # a function calling its own .cold / .part piece isn't a source-level call
                    calls.add((a, b))
            else:
                external.add(dst.rsplit(":", 1)[-1].lstrip("*"))
    return sorted(calls), sorted(external), indirect


def imports_of(path):
    needed = re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", run(["readelf", "-d", "-W", path]))
    syms = sorted({s["name"] for s in symtab(path, dynamic=True) if s["ndx"] == "UND" and s["type"] == "FUNC"})
    return {"needed": needed, "functions": syms}


# ------------------------------------------------------------------ build

def build_level(project, root, opt, out):
    """Compile every source of the project at one level, then link, strip and describe each binary."""
    base = os.path.join(out, project["name"], opt_name(opt))
    objdir = os.path.join(base, "obj")
    os.makedirs(objdir, exist_ok=True)
    cflags = [opt, "-g", "-w", "-fPIC", "-fcallgraph-info", f"-ffile-prefix-map={root}=."] + project.get("cflags", [])
    srcs = sorted({s for b in project["binaries"] for s in sources_of(project, b)})
    for src in srcs:
        obj = os.path.join(objdir, obj_stem(src) + ".o")
        run(["gcc", "-c"] + cflags + ["-dumpdir", objdir + "/", "-o", obj, src], cwd=root)

    entries = []
    for b in project["binaries"]:
        objs = [os.path.join(objdir, obj_stem(s) + ".o") for s in sources_of(project, b)]
        paths = {v: os.path.join(base, v, b["name"]) for v in ("debug", "symbols", "stripped", "bare")}
        for p in paths.values():
            os.makedirs(os.path.dirname(p), exist_ok=True)
        link = ["gcc", opt, "-g"] + (["-shared"] if b["kind"] == "shared" else []) + objs + ["-o", paths["debug"]]
        run(link + b.get("libs", []))
        run(["strip", "--strip-debug", "-o", paths["symbols"], paths["debug"]])
        run(["strip", "--strip-all", "-o", paths["stripped"], paths["debug"]])
        run(["objcopy", "--remove-section=.eh_frame", "--remove-section=.eh_frame_hdr", paths["stripped"], paths["bare"]])

        functions = functions_of(paths["debug"], root)
        ci_files = [os.path.join(objdir, obj_stem(s) + ".ci") for s in sources_of(project, b)]
        calls, external, indirect = calls_of([c for c in ci_files if os.path.exists(c)], functions)
        truth = {
            "project": project["name"], "binary": b["name"], "kind": b["kind"], "opt": opt,
            "paths": {k: os.path.relpath(v, out) for k, v in paths.items()},
            "source_root": os.path.relpath(root, REPO) if root.startswith(REPO) else root,
            "functions": functions,
            "calls": [{"from": a, "to": c} for a, c in calls],
            "external_calls": external,
            "indirect_call_sites": indirect,
            "imports": imports_of(paths["debug"]),
        }
        tpath = os.path.join(base, "truth", b["name"] + ".json")
        os.makedirs(os.path.dirname(tpath), exist_ok=True)
        with open(tpath, "w") as f:
            json.dump(truth, f, indent=1)
        entries.append({"project": project["name"], "binary": b["name"], "opt": opt, "kind": b["kind"],
                        "truth": os.path.relpath(tpath, out), "functions": len(functions),
                        "project_functions": sum(1 for x in functions if x["origin"] == "project"),
                        "calls": len(calls), "indirect_call_sites": indirect})
    return entries


def build(args):
    spec = json.load(open(SPEC))
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    jobs = []
    for p in spec["projects"]:
        if args.only and p["name"] != args.only:
            continue
        if p.get("optional") and not getattr(args, p["optional"].lstrip("-").replace("-", "_")):
            continue
        root = fetch(p, out) if "download" in p else os.path.join(REPO, p["root"])
        if not os.path.isdir(root):
            raise RuntimeError(f"{p['name']}: {root} missing (git submodule update --init --depth 1)")
        for opt in spec["opt_levels"]:
            jobs.append((p, root, opt))
    manifest = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for entries in pool.map(lambda j: build_level(j[0], j[1], j[2], out), jobs):
            manifest += entries
    manifest.sort(key=lambda e: (e["project"], e["binary"], spec["opt_levels"].index(e["opt"])))
    with open(os.path.join(out, "manifest.json"), "w") as f:
        json.dump({"spec": os.path.relpath(SPEC, REPO), "binaries": manifest}, f, indent=1)
    print(f"{len(manifest)} binaries x 4 forms in {out}")
    for e in manifest:
        print(f"  {e['project']:8} {e['binary']:14} {e['opt']:4} functions {e['functions']:5} "
              f"(project {e['project_functions']:5})  calls {e['calls']:5}  indirect sites {e['indirect_call_sites']}")


def check(args):
    """The answer keys are consistent: every binary has project functions with source ids, the
    symbols form keeps the same symbol table, the stripped form has none."""
    out = os.path.abspath(args.out)
    manifest = json.load(open(os.path.join(out, "manifest.json")))["binaries"]
    problems = []
    for e in manifest:
        t = json.load(open(os.path.join(out, e["truth"])))
        where = f"{e['project']}/{e['opt']}/{e['binary']}"
        proj = [f for f in t["functions"] if f["origin"] == "project"]
        if not proj:
            problems.append(f"{where}: no project functions")
        if any(not f["source_id"] for f in proj):
            problems.append(f"{where}: project functions without a source id")
        def funcs(form):
            return {(s["name"], s["value"]) for s in symtab(os.path.join(out, t["paths"][form]), table=".symtab")
                    if s["type"] == "FUNC"}
        if funcs("debug") != funcs("symbols"):
            problems.append(f"{where}: symbols form lost function symbols")
        if symtab(os.path.join(out, t["paths"]["stripped"]), table=".symtab"):
            problems.append(f"{where}: stripped form still has a .symtab")  # .dynsym stays: the loader needs it
        sections = run(["readelf", "-S", "-W", os.path.join(out, t["paths"]["bare"])])
        if ".eh_frame" in sections or ".symtab" in sections:
            problems.append(f"{where}: bare form still has .eh_frame or .symtab")
        if e["project"] == "zlib" and not t["calls"]:
            problems.append(f"{where}: no calls in the answer key")
    for p in problems:
        print(p)
    print(f"checked {len(manifest)} binaries: {'ok' if not problems else str(len(problems)) + ' problems'}")
    return 1 if problems else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", required=True)
    ap.add_argument("--with-sqlite", action="store_true", help="also build sqlite (downloaded; slow)")
    ap.add_argument("--only", help="build one project")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--check", action="store_true", help="sanity-check an already built corpus")
    args = ap.parse_args()
    for tool in ("gcc", "strip", "objcopy", "readelf", "addr2line"):
        if not shutil.which(tool):
            sys.exit(f"build_corpus: {tool} not found")
    if args.check:
        sys.exit(check(args))
    build(args)


if __name__ == "__main__":
    main()
