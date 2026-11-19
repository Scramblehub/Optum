// Binary frontend (x86-64 ELF). Rules: docs/frontends.md ("Binaries").
//
// 1. Read the ELF file (Core/src/elf.cpp) and map PLT stubs to the imports they jump to.
// 2. Sweep every code section once with capstone, keeping a compact record per instruction.
// 3. Function starts, best evidence first: symbols, .eh_frame entries, exports, the entry point,
//    init/fini arrays, pointers stored in data (relocations) and direct call targets ("exact");
//    then, for code with none of those (stripped, no unwind tables), starts after a function's
//    last instruction and its padding, unless the code before jumps there ("gap", "inferred").
// 4. Calls: direct calls and tail-call jumps to function starts; calls and jumps to PLT stubs or
//    through GOT slots are calls to imported functions.

#include "optum/frontends/binary.hpp"

#include <capstone/capstone.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "optum/elf.hpp"

namespace optum {
namespace {

namespace fs = std::filesystem;

enum class Kind : std::uint8_t { Other, Call, CallIndirect, Jump, JumpIndirect, CondJump, Return, Halt, Padding };

struct Insn {
  std::uint64_t addr = 0;
  std::uint64_t lea = 0;     // lea reg, [rip+x]: the address x (jump tables start like this)
  std::uint64_t target = 0;  // direct branches; for [rip+x] memory operands, the slot address
  std::uint8_t size = 0;
  Kind kind = Kind::Other;
  bool rip_slot = false;  // target is a memory slot (call/jmp qword ptr [rip+x])
  bool endbr = false;
};

// Library functions that never return: code after a call to them belongs to the next function.
bool noreturn_import(const std::string& name) {
  static const std::unordered_set<std::string> k = {
      "exit", "_exit", "_Exit", "abort", "__stack_chk_fail", "__assert_fail", "__fortify_fail", "__chk_fail",
      "longjmp", "siglongjmp", "__longjmp_chk", "pthread_exit", "err", "errx", "verr", "verrx", "__cxa_throw",
      "__cxa_rethrow", "_Unwind_Resume", "quick_exit", "thrd_exit"};
  return k.count(name) > 0;
}

class Disassembler {
 public:
  Disassembler() {
    if (cs_open(CS_ARCH_X86, CS_MODE_64, &h_) != CS_ERR_OK) throw IrError("capstone: cannot open x86-64");
    cs_option(h_, CS_OPT_DETAIL, CS_OPT_ON);
  }
  ~Disassembler() { cs_close(&h_); }
  Disassembler(const Disassembler&) = delete;
  Disassembler& operator=(const Disassembler&) = delete;

  // Appends one record per instruction. Undecodable bytes are skipped one at a time and recorded
  // as a halt (they end whatever function they're in).
  void sweep(std::span<const std::uint8_t> code, std::uint64_t addr, std::vector<Insn>& out) {
    cs_insn* insn = cs_malloc(h_);
    const std::uint8_t* p = code.data();
    std::size_t left = code.size();
    std::uint64_t a = addr;
    while (left > 0) {
      if (!cs_disasm_iter(h_, &p, &left, &a, insn)) {
        Insn bad;
        bad.addr = a;
        bad.size = 1;
        bad.kind = Kind::Halt;
        out.push_back(bad);
        ++p;
        --left;
        ++a;
        continue;
      }
      out.push_back(classify(*insn));
    }
    cs_free(insn, 1);
  }

 private:
  csh h_ = 0;

  static Insn classify(const cs_insn& i) {
    Insn r;
    r.addr = i.address;
    r.size = static_cast<std::uint8_t>(i.size);
    const cs_x86& x = i.detail->x86;
    auto direct = [&]() -> bool {
      if (x.op_count == 1 && x.operands[0].type == X86_OP_IMM) {
        r.target = static_cast<std::uint64_t>(x.operands[0].imm);
        return true;
      }
      if (x.op_count == 1 && x.operands[0].type == X86_OP_MEM && x.operands[0].mem.base == X86_REG_RIP &&
          x.operands[0].mem.index == X86_REG_INVALID) {
        r.target = i.address + i.size + static_cast<std::uint64_t>(x.operands[0].mem.disp);
        r.rip_slot = true;
      }
      return false;
    };
    switch (i.id) {
      case X86_INS_CALL:
        r.kind = direct() ? Kind::Call : Kind::CallIndirect;
        break;
      case X86_INS_JMP:
      case X86_INS_LJMP:
        r.kind = direct() ? Kind::Jump : Kind::JumpIndirect;
        break;
      case X86_INS_RET:
      case X86_INS_RETF:
      case X86_INS_RETFQ:
        r.kind = Kind::Return;
        break;
      case X86_INS_HLT:
      case X86_INS_UD2:
        r.kind = Kind::Halt;
        break;
      case X86_INS_NOP:
      case X86_INS_INT3:
        r.kind = Kind::Padding;
        break;
      case X86_INS_ENDBR64:
        r.endbr = true;
        break;
      case X86_INS_LEA:
        if (x.op_count == 2 && x.operands[1].type == X86_OP_MEM && x.operands[1].mem.base == X86_REG_RIP &&
            x.operands[1].mem.index == X86_REG_INVALID) {
          r.lea = i.address + i.size + static_cast<std::uint64_t>(x.operands[1].mem.disp);
        }
        break;
      default:
        break;
    }
    // Conditional jumps (jcc, loop, jrcxz): capstone puts them in CS_GRP_JUMP, with one immediate.
    if (r.kind == Kind::Other && x.op_count == 1 && x.operands[0].type == X86_OP_IMM) {
      for (std::uint8_t g = 0; g < i.detail->groups_count; ++g) {
        if (i.detail->groups[g] == CS_GRP_JUMP) {
          r.kind = Kind::CondJump;
          r.target = static_cast<std::uint64_t>(x.operands[0].imm);
        }
      }
    }
    return r;
  }
};

struct Func {
  std::uint64_t start = 0, end = 0;
  std::string name;  // from a symbol, if any
  bool is_static = false;
  const char* found = "";  // what evidence found it
  bool gap = false;
};

class BinaryAnalysis {
 public:
  BinaryAnalysis(const elf::File& f, std::string rel) : f_(f), rel_(std::move(rel)) {}

  void run() {
    map_plt();
    sweep_code();
    find_functions();
  }

  void emit(Graph& g, BinaryStats& stats) const;

 private:
  const elf::File& f_;
  std::string rel_;
  std::map<std::uint64_t, std::string> plt_;  // PLT stub start -> imported function
  std::vector<Insn> code_;                     // all code sections, in address order
  std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges_;  // code section [start, end)
  std::map<std::uint64_t, Func> funcs_;

  static bool is_plt(const std::string& name) { return name == ".plt" || name == ".plt.sec" || name == ".plt.got"; }

  bool in_code(std::uint64_t a) const {
    return std::any_of(ranges_.begin(), ranges_.end(), [a](auto r) { return a >= r.first && a < r.second; });
  }

  // Index of the instruction starting exactly at `a`, if any.
  std::optional<std::size_t> insn_at(std::uint64_t a) const {
    auto it = std::lower_bound(code_.begin(), code_.end(), a, [](const Insn& i, std::uint64_t x) { return i.addr < x; });
    if (it == code_.end() || it->addr != a) return std::nullopt;
    return static_cast<std::size_t>(it - code_.begin());
  }

  void map_plt() {
    Disassembler d;
    for (const auto& s : f_.sections) {
      if (!is_plt(s.name) || !s.executable()) continue;
      std::vector<Insn> stubs;
      d.sweep(f_.bytes(s), s.addr, stubs);
      for (const Insn& i : stubs) {
        if (i.kind != Kind::JumpIndirect || !i.rip_slot) continue;
        auto it = f_.got_imports.find(i.target);
        if (it == f_.got_imports.end()) continue;
        const std::uint64_t entry = s.addr + ((i.addr - s.addr) / 16) * 16;  // 16-byte PLT entries
        plt_.emplace(entry, it->second);
      }
    }
  }

  void sweep_code() {
    Disassembler d;
    std::vector<const elf::Section*> secs;
    for (const auto& s : f_.sections) {
      if (s.executable() && !is_plt(s.name) && s.name != ".init" && s.name != ".fini" && s.type != 8) secs.push_back(&s);
    }
    std::sort(secs.begin(), secs.end(), [](auto* a, auto* b) { return a->addr < b->addr; });
    for (const auto* s : secs) {
      d.sweep(f_.bytes(*s), s->addr, code_);
      ranges_.emplace_back(s->addr, s->addr + s->size);
    }
  }

  std::optional<std::string> import_at(const Insn& i) const {
    if (i.rip_slot) {
      if (auto it = f_.got_imports.find(i.target); it != f_.got_imports.end()) return it->second;
      return std::nullopt;
    }
    if (auto it = plt_.find(i.target); it != plt_.end()) return it->second;
    return std::nullopt;
  }

  bool terminates(const Insn& i) const {
    switch (i.kind) {
      case Kind::Return:
      case Kind::Halt:
      case Kind::Jump:
      case Kind::JumpIndirect:
        return true;
      case Kind::Call: {
        const auto imp = import_at(i);
        return imp && noreturn_import(*imp);
      }
      case Kind::CallIndirect: {
        const auto imp = import_at(i);
        return imp && noreturn_import(*imp);
      }
      default:
        return false;
    }
  }

  // gcc's switch in position-independent code: lea rT, [rip+table]; movsxd rX, [rT+rI*4];
  // add rX, rT; jmp rX. The table holds 32-bit offsets from its own start. Returns (target, jmp)
  // pairs: entries are read while they land on instruction boundaries close to the jump.
  std::vector<std::pair<std::uint64_t, std::uint64_t>> jump_table_targets() const {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
    for (std::size_t k = 0; k < code_.size(); ++k) {
      if (code_[k].kind != Kind::JumpIndirect || code_[k].rip_slot) continue;
      for (std::size_t b = k; b > 0 && k - b < 12; --b) {
        const std::uint64_t table = code_[b - 1].lea;
        if (!table) continue;
        const elf::Section* sec = f_.section_at(table);
        if (!sec || sec->executable() || sec->type == 8) break;
        const auto bytes = f_.bytes(*sec);
        for (std::uint64_t off = table - sec->addr; off + 4 <= bytes.size() && out.size() < 1u << 20; off += 4) {
          std::int32_t rel;
          std::memcpy(&rel, bytes.data() + off, 4);
          const std::uint64_t target = table + static_cast<std::uint64_t>(static_cast<std::int64_t>(rel));
          const std::uint64_t jmp = code_[k].addr;
          const std::uint64_t dist = target > jmp ? target - jmp : jmp - target;
          if (!insn_at(target) || dist > (1u << 16)) break;
          out.emplace_back(target, jmp);
        }
        break;
      }
    }
    return out;
  }

  void add(std::uint64_t a, const char* found, bool gap = false) {
    if (!in_code(a) || !insn_at(a)) return;  // only real instruction boundaries in code
    auto [it, inserted] = funcs_.try_emplace(a);
    if (inserted) {
      it->second.start = a;
      it->second.found = found;
      it->second.gap = gap;
    }
  }

  void find_functions() {
    // Names, from both symbol tables. A symbol table entry is also the best evidence of a start.
    std::map<std::uint64_t, const elf::Symbol*> named;
    auto consider = [&](const elf::Symbol& s) {
      if (!s.is_function() || !s.defined() || s.value == 0) return;
      auto& cur = named[s.value];
      // Prefer global names, and names without a GCC suffix (.localalias, .cold...).
      auto rank = [](const elf::Symbol* x) {
        return (x->bind == 0 ? 2 : 0) + (x->name.find('.') != std::string::npos ? 1 : 0);
      };
      if (!cur || rank(&s) < rank(cur)) cur = &s;
    };
    for (const auto& s : f_.symtab) consider(s);
    for (const auto& s : f_.dynsym) consider(s);
    for (const auto& [a, s] : named) {
      if (s->size > 0 || a == f_.entry) add(a, f_.symtab.empty() ? "export" : "symbol");
    }
    for (const auto& fde : f_.fdes) add(fde.start, "eh_frame");
    add(f_.entry, "entry");
    for (auto a : f_.init_array) add(a, "init_array");
    for (auto a : f_.fini_array) add(a, "init_array");
    for (auto a : f_.relative_targets) add(a, "address_taken");
    for (const Insn& i : code_) {
      if (i.kind == Kind::Call && !i.rip_slot && !plt_.count(i.target)) add(i.target, "call_target");
    }
    // Gaps: after a terminator and its padding, a new function begins, unless code of the
    // function it would split jumps there (a branch target inside the same function), or a symbol
    // or unwind entry already says where the function ends.
    std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> jumps_to;
    for (const Insn& i : code_) {
      if ((i.kind == Kind::Jump || i.kind == Kind::CondJump) && !i.rip_slot) jumps_to[i.target].push_back(i.addr);
    }
    for (const auto& [target, from] : jump_table_targets()) jumps_to[target].push_back(from);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> covered;  // known [start, end)
    for (const auto& [a, sym] : named) {
      if (sym->size) covered.emplace_back(a, a + sym->size);
    }
    for (const auto& fde : f_.fdes) covered.emplace_back(fde.start, fde.start + fde.size);
    std::sort(covered.begin(), covered.end());
    auto inside_known = [&](std::uint64_t c) {
      auto it = std::upper_bound(covered.begin(), covered.end(), std::make_pair(c, UINT64_MAX));
      for (int back = 0; back < 8 && it != covered.begin(); ++back) {
        --it;
        if (c > it->first && c < it->second) return true;
      }
      return false;
    };
    std::vector<std::uint64_t> exact;
    for (const auto& [a, fn] : funcs_) exact.push_back(a);
    // Control-flow protection (gcc -fcf-protection, Ubuntu's default): every function entry
    // starts with endbr64. If most known entries do, a guessed entry must too, unless it's only
    // jumped to from elsewhere (a .cold piece split off another function has no endbr64).
    std::size_t with_endbr = 0;
    for (std::uint64_t a : exact) with_endbr += code_[*insn_at(a)].endbr ? 1 : 0;
    const bool cet = exact.size() >= 4 && with_endbr * 10 >= exact.size() * 8;
    std::uint64_t current = 0;  // the last start with real evidence: guesses don't move it
    for (std::size_t k = 0; k < code_.size(); ++k) {
      if (auto it = funcs_.find(code_[k].addr); it != funcs_.end() && !it->second.gap) current = code_[k].addr;
      if (!terminates(code_[k])) continue;
      std::size_t n = k + 1;
      while (n < code_.size() && code_[n].kind == Kind::Padding) ++n;
      if (n >= code_.size()) break;
      const std::uint64_t c = code_[n].addr;
      if (funcs_.count(c) || !in_code(c) || inside_known(c)) continue;
      // The region the candidate would split: from the current function's start to the next
      // known start. A jump from inside it means the code continues the same function.
      auto next = std::upper_bound(exact.begin(), exact.end(), c);
      const std::uint64_t region_end = next == exact.end() ? UINT64_MAX : *next;
      bool inside = false, outside = false;
      if (auto it = jumps_to.find(c); it != jumps_to.end()) {
        for (std::uint64_t src : it->second) {
          if (src >= current && src < region_end) inside = true;
          else outside = true;
        }
      }
      if (inside) continue;
      if (cet && !code_[n].endbr && !outside) continue;  // unreachable code inside a function
      add(c, "gap", true);
    }

    for (auto& [a, fn] : funcs_) {
      if (auto it = named.find(a); it != named.end()) {
        fn.name = it->second->name;
        fn.is_static = it->second->bind == 0;
      }
    }

    // Extents: up to the next start, without trailing padding.
    for (auto it = funcs_.begin(); it != funcs_.end(); ++it) {
      auto next = std::next(it);
      std::uint64_t end = next == funcs_.end() ? UINT64_MAX : next->first;
      auto k = insn_at(it->first);
      std::uint64_t last = it->first;
      for (std::size_t i = *k; i < code_.size() && code_[i].addr < end; ++i) {
        if (code_[i].kind != Kind::Padding) last = code_[i].addr + code_[i].size;
        if (i + 1 < code_.size() && code_[i + 1].addr != code_[i].addr + code_[i].size) break;  // section end
      }
      it->second.end = last;
    }
  }

  std::uint64_t func_of(std::uint64_t a) const {
    auto it = funcs_.upper_bound(a);
    if (it == funcs_.begin()) return 0;
    --it;
    return a < it->second.end ? it->first : 0;
  }

};

std::string hex(std::uint64_t v) {
  char buf[2 + 16 + 1];
  std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
  return buf;
}

void BinaryAnalysis::emit(Graph& g, BinaryStats& stats) const {
  const std::string bin = fs::path(rel_).filename().string();
  const std::string mod_id = "bin:" + bin;
  Node mod;
  mod.id = mod_id;
  mod.kind = f_.has_interpreter ? NodeKind::Program : NodeKind::Module;
  mod.name = bin;
  mod.loc = SourceLoc{rel_, 0, 0, std::nullopt};
  mod.frontend = "binary";
  mod.attrs = {{"format", "elf64"}, {"machine", "x86-64"}};
  if (!f_.soname.empty()) mod.attrs["soname"] = f_.soname;
  g.add_node(std::move(mod));

  // Ids: the symbol name when there is one (with the address when two functions share it).
  std::unordered_map<std::string, int> name_count;
  for (const auto& [a, fn] : funcs_) {
    if (!fn.name.empty()) ++name_count[fn.name];
  }
  std::unordered_map<std::uint64_t, std::string> ids;
  // Exported functions by name: in position-independent code, calls between a library's own
  // exported functions go through its PLT, as if they were imports.
  std::unordered_map<std::string, std::uint64_t> exported;
  for (const auto& s : f_.dynsym) {
    if (s.is_function() && s.defined() && funcs_.count(s.value)) exported.emplace(s.name, s.value);
  }
  for (const auto& [a, fn] : funcs_) {
    std::string id = mod_id + "!";
    if (fn.name.empty()) id += hex(a);
    else if (name_count[fn.name] > 1) id += fn.name + "@" + hex(a);
    else id += fn.name;
    ids[a] = id;
    Node n;
    n.id = id;
    n.kind = NodeKind::Function;
    n.name = fn.name.empty() ? hex(a) : fn.name;
    n.loc = SourceLoc{rel_, 0, 0, a};
    n.frontend = "binary";
    n.confidence = fn.gap ? Confidence::Inferred : Confidence::Exact;
    n.attrs = {{"size", fn.end - a}, {"found", fn.found}};
    if (fn.is_static) n.attrs["static"] = true;
    g.add_node(std::move(n));
    Edge e;
    e.from = *g.find(mod_id);
    e.to = *g.find(id);
    e.kind = EdgeKind::Defines;
    e.frontend = "binary";
    g.add_edge(std::move(e));
  }

  // Imports: the libraries named in DT_NEEDED, and the functions called from them.
  auto library_of = [&](const std::string& fn) -> std::string {
    for (const auto& s : f_.dynsym) {
      if (s.name == fn && !s.defined() && !s.library.empty()) return s.library;
    }
    return f_.needed.size() == 1 ? f_.needed[0] : "?";
  };
  for (const std::string& lib : f_.needed) {
    Node n;
    n.id = "lib:" + lib;
    n.kind = NodeKind::Module;
    n.name = lib;
    n.frontend = "binary";
    n.attrs = {{"third_party", true}};
    g.add_node(std::move(n));
    Edge e;
    e.from = *g.find(mod_id);
    e.to = *g.find("lib:" + lib);
    e.kind = EdgeKind::Imports;
    e.frontend = "binary";
    g.add_edge(std::move(e));
  }

  for (const Insn& i : code_) {
    const bool call = i.kind == Kind::Call || (i.kind == Kind::CallIndirect && i.rip_slot);
    const bool jump = i.kind == Kind::Jump || (i.kind == Kind::JumpIndirect && i.rip_slot);
    if (!call && !jump) continue;
    const std::uint64_t from = func_of(i.addr);
    if (!from) continue;
    std::string to;
    const Confidence c = Confidence::Exact;
    nlohmann::json attrs = nlohmann::json::object();
    std::optional<std::string> imp = import_at(i);
    if (imp && exported.count(*imp)) {  // a PLT call to this binary's own exported function
      to = ids.at(exported.at(*imp));
      attrs["via"] = "plt";
      imp.reset();
    } else if (imp) {
      const std::string lib = library_of(*imp);
      to = "lib:" + lib + "!" + *imp;
      if (!g.find(to)) {
        Node n;
        n.id = to;
        n.kind = NodeKind::Function;
        n.name = *imp;
        n.frontend = "binary";
        n.attrs = {{"imported", true}};
        g.add_node(std::move(n));
        if (auto libnode = g.find("lib:" + lib)) {
          Edge d;
          d.from = *libnode;
          d.to = *g.find(to);
          d.kind = EdgeKind::Defines;
          d.frontend = "binary";
          g.add_edge(std::move(d));
        }
      }
    } else if (!i.rip_slot && funcs_.count(i.target)) {
      if (jump && (i.target == from)) continue;  // a jump to its own start is a loop, not a call
      to = ids.at(i.target);
    } else {
      continue;  // into the middle of a function, or not code we know
    }
    if (jump) attrs["via"] = attrs.contains("via") ? "plt_tail_call" : "tail_call";
    Edge e;
    e.from = *g.find(ids.at(from));
    e.to = *g.find(to);
    e.kind = EdgeKind::Calls;
    e.confidence = c;
    e.frontend = "binary";
    e.loc = SourceLoc{rel_, 0, 0, i.addr};
    e.attrs = std::move(attrs);
    g.add_edge(std::move(e));
  }
  stats.functions += funcs_.size();
  stats.instructions += code_.size();
}

}  // namespace

Graph extract_binary_files(const fs::path& root, const std::vector<fs::path>& files, BinaryStats* stats,
                           std::string* diagnostics) {
  Graph g;
  g.source = root.string();
  BinaryStats local;
  BinaryStats& st = stats ? *stats : local;
  for (const auto& rel : files) {
    const fs::path p = root / rel;
    if (!fs::is_regular_file(p) || !elf::is_elf(p)) continue;
    try {
      const elf::File f = elf::read(p);
      if (f.type != 2 && f.type != 3) {  // object files, core dumps
        ++st.skipped;
        continue;
      }
      if (f.machine != 62) {
        ++st.skipped;
        if (diagnostics) *diagnostics += rel.generic_string() + ": not x86-64, skipped\n";
        continue;
      }
      BinaryAnalysis a(f, rel.generic_string());
      a.run();
      a.emit(g, st);
      ++st.files;
    } catch (const IrError& e) {
      ++st.skipped;
      if (diagnostics) *diagnostics += std::string(e.what()) + "\n";
    }
  }
  return g;
}

}  // namespace optum
