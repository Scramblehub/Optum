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
import collections
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
    if src.startswith("@repo/"):
        return "repo__" + os.path.basename(src)[:-2]
    return src[:-2].replace("/", "__")


def repo_path(x):
    """'@repo/...' means the repo root (stub headers and libraries shared by projects)."""
    return x.replace("@repo/", REPO + "/")


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


def text_sections(path):
    """Section index -> name, for the code sections a function can live in (not .init/.fini/.plt*)."""
    out = {}
    for line in run(["readelf", "-S", "-W", path]).splitlines():
        m = re.match(r"\s*\[\s*(\d+)\]\s+(\S+)\s+\S+\s+\S+\s+\S+\s+\S+\s+\S+\s+(\S*X\S*)\s", line)
        if m and not m.group(2).startswith((".init", ".fini", ".plt")):
            out[m.group(1)] = m.group(2)
    return out


def dwarf_decls(debug_path):
    """Each function's declaring file and line, from the debug info (DW_AT_decl_file), read with
    binutils' readelf: by code address, and by name for declarations without one. addr2line's
    answer is the line table's, which for an inline function from a header can be its caller's."""
    def plain(text):  # readelf -W writes each value's form first: "(addr) 0x1149", "(udata) 1"
        return re.sub(r"\((?:addr|udata|data\d|sdata|line_strp|strp|string|sec_offset|ref\d|ref_udata|strx\d?|addrx\d?|implicit_const|flag_present|flag)\)\s*", "", text)

    tables, files = {}, None
    for line in run(["readelf", "--debug-dump=line", "-W", debug_path]).splitlines():
        line = plain(line)
        m = re.match(r"\s*Offset:\s+(0x[0-9a-f]+|0)\s*$", line)  # readelf prints the first as plain "0"
        if m:
            files, dirs = [], []
            tables[int(m.group(1), 16)] = (dirs, files)
            mode = None
            continue
        if files is None:
            continue
        if "The Directory Table" in line:
            mode = "dirs"
        elif "The File Name Table" in line:
            mode = "files"
        elif "Line Number Statements" in line:
            mode = None
        elif mode:
            m = re.match(r"\s*(\d+)\s+(?:(\d+)\s+)?\(.*\):\s+(.*)$", line) or re.match(r"\s*(\d+)\s+(?:(\d+)\s+)?(\S.*)$", line)
            if m and not line.strip().startswith("Entry"):
                (dirs if mode == "dirs" else files).append((m.group(3).strip(), int(m.group(2) or 0)))
    by_addr, by_offset, cu_table = {}, {}, None
    cur = None
    for line in run(["readelf", "--debug-dump=info", "-W", debug_path]).splitlines():
        m = re.match(r"\s*<(\d+)><([0-9a-f]+)>: Abbrev Number: \d+ \((\w+)\)", line)
        if m:
            cur = {"tag": m.group(3), "off": int(m.group(2), 16), "table": cu_table}
            if m.group(3) == "DW_TAG_subprogram":
                by_offset[cur["off"]] = cur
            continue
        if cur is None:
            continue
        m = re.match(r"\s*<[0-9a-f]+>\s+(DW_AT_\w+)\s*:\s*(.*)$", plain(line))
        if not m:
            continue
        attr, val = m.groups()
        if attr == "DW_AT_stmt_list" and cur["tag"] == "DW_TAG_compile_unit":
            cu_table = tables.get(int(val, 16))
        elif attr == "DW_AT_name":
            cur["name"] = val.split(": ")[-1].strip()
        elif attr == "DW_AT_decl_file":
            cur["file"] = int(val.split()[0], 0)  # decimal, or hex for wide forms
        elif attr == "DW_AT_decl_line":
            cur["line"] = int(val.split()[0], 0)
        elif attr == "DW_AT_low_pc":
            cur["low"] = int(val, 16)
        elif attr == "DW_AT_declaration":
            cur["declaration"] = True
        elif attr in ("DW_AT_abstract_origin", "DW_AT_specification"):
            cur["origin"] = int(re.search(r"<0x([0-9a-f]+)>", val).group(1), 16)
    def resolve(d):
        name, fileno, line, table, hops = d.get("name"), d.get("file"), d.get("line", 0), d["table"], 0
        while (name is None or fileno is None) and "origin" in d and hops < 8:
            d = by_offset.get(d["origin"], {})
            if not d:
                break
            name = name or d.get("name")
            if fileno is None:
                fileno, line, table = d.get("file"), d.get("line", 0), d["table"]
            hops += 1
        if fileno is None or not table:
            return None
        dirs, files = table
        if fileno >= len(files):
            return None
        fname, dirno = files[fileno]
        path = os.path.normpath(os.path.join(dirs[dirno][0] if dirno < len(dirs) else "", fname))
        return name, path, line
    # The name fallback is only safe for names defined in one file anywhere in the debug info
    # (two files' static helpers of one name, one of them inlined away, must not be confused).
    by_name, files_of = collections.defaultdict(set), collections.defaultdict(set)
    for d in by_offset.values():
        r = resolve(d)
        if not r:
            continue
        if not d.get("declaration"):  # prototypes aren't definitions
            files_of[r[0]].add(r[1])
        if "low" in d:
            by_addr[d["low"]] = r
        elif "origin" not in d and not d.get("declaration"):
            by_name[r[0]].add((r[1], r[2]))
    by_name = {n: v for n, v in by_name.items() if len(files_of[n]) == 1}
    return by_addr, {n: (n, *next(iter(v))) for n, v in by_name.items() if len(v) == 1}  # (name, path, line)


def functions_of(debug_path, project_root):
    # readelf --syms lists .dynsym and .symtab: a shared library's exports appear in both.
    # Size-0 function symbols in code sections are real functions too (frame_dummy and other C
    # runtime code); only .init/.fini/.plt pieces are left out.
    code = text_sections(debug_path)
    seen, funcs = set(), []
    for sym in symtab(debug_path):
        if sym["type"] != "FUNC" or sym["ndx"] == "UND" or (sym["name"], sym["value"]) in seen:
            continue
        if sym["size"] > 0 or sym["ndx"] in code:
            seen.add((sym["name"], sym["value"]))
            funcs.append(sym)
    if not funcs:
        return []
    out = run(["addr2line", "-f", "-e", debug_path] + [hex(f["value"]) for f in funcs]).splitlines()
    by_addr, by_name = dwarf_decls(debug_path)
    result = []
    for f, (_, loc) in zip(funcs, zip(out[0::2], out[1::2])):
        file, _, line = loc.rpartition(":")
        file = os.path.normpath(file) if file not in ("", "??") else ""
        line = int(line.split()[0]) if line[:1].isdigit() else 0
        # The declaring file from the debug info, where it has one (see dwarf_decls).
        decl = by_addr.get(f["value"]) or by_name.get(f["name"].partition(".")[0])
        if decl and file:
            file, line = decl[1], decl[2]
        # In the project only if the file is really there: the C runtime's crtstuff.c also has a
        # relative name in its debug info.
        in_project = bool(file) and not os.path.isabs(file) and not file.startswith("..") and \
            os.path.isfile(os.path.join(project_root, file))
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

    static_names = collections.defaultdict(set)
    for (file, name), sid in statics.items():
        static_names[name].add(sid)

    def resolve(title, tu):
        if ":" in title:
            file, name = title.rsplit(":", 1)
            hit = statics.get((file, name)) or statics.get((os.path.basename(file), name))
            if not hit and len(static_names[name]) == 1:  # declared in a header, compiled into this file
                hit = next(iter(static_names[name]))
            return hit
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
    # GCC applies the last matching prefix map: the project's own (more specific) goes last.
    cflags = [opt, "-g", "-w", "-fPIC", "-fcallgraph-info", f"-ffile-prefix-map={REPO}=@repo",
              f"-ffile-prefix-map={root}=."] + [repo_path(c) for c in project.get("cflags", [])]
    srcs = sorted({s for b in project["binaries"] for s in sources_of(project, b)})
    for src in srcs:
        obj = os.path.join(objdir, obj_stem(src) + ".o")
        run(["gcc", "-c"] + cflags + ["-dumpdir", objdir + "/", "-o", obj, repo_path(src)], cwd=root)

    entries = []
    built = {}  # binary name -> its debug path, for '@bin:NAME' in later binaries' libs
    for b in project["binaries"]:
        objs = [os.path.join(objdir, obj_stem(s) + ".o") for s in sources_of(project, b)]
        paths = {v: os.path.join(base, v, b["name"]) for v in ("debug", "symbols", "stripped", "bare")}
        for p in paths.values():
            os.makedirs(os.path.dirname(p), exist_ok=True)
        link = ["gcc", opt, "-g"] + (["-shared"] if b["kind"] == "shared" else []) + objs + ["-o", paths["debug"]]
        libs = [built[x[5:]] if x.startswith("@bin:") else x for x in b.get("libs", [])]
        run(link + libs)
        built[b["name"]] = paths["debug"]
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
        if "expected" in b:  # the hand-checked source graph (for APIs and tables reached)
            truth["source_graph"] = os.path.relpath(os.path.join(root, b["expected"]), REPO)
        tpath = os.path.join(base, "truth", b["name"] + ".json")
        os.makedirs(os.path.dirname(tpath), exist_ok=True)
        with open(tpath, "w") as f:
            json.dump(truth, f, indent=1)
        entries.append({"project": project["name"], "binary": b["name"], "opt": opt, "kind": b["kind"],
                        "stub": bool(b.get("stub")),
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
        if not proj and not e.get("stub"):  # stub libraries (tests/corpus/stubs) have no project code
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
