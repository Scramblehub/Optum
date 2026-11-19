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

#include "../common.hpp"
#include "optum/dwarf.hpp"
#include "optum/elf.hpp"
#include "optum/http.hpp"
#include "optum/sql.hpp"

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

// ===================================================================== lifting
//
// Just enough to read the arguments of library calls: values in registers and stack slots,
// followed through one function in address order (control flow is ignored, which is right for
// the straight-line setup code that calls libraries). Values are constants, addresses (of string
// literals, or of pointers stored in data), and stack addresses (local buffers).

struct Val {
  enum class K : std::uint8_t { Unknown, Imm, Addr, Stack };
  K k = K::Unknown;
  std::uint64_t v = 0;        // Imm: the value. Addr: the address. Stack: the offset (as signed)
  unsigned base = 0;          // Stack: the base register (rsp or rbp)
};

// The 64-bit register a register name is part of (esi -> rsi); X86_REG_INVALID if not tracked.
unsigned canon(unsigned r) {
  switch (r) {
    case X86_REG_RAX: case X86_REG_EAX: case X86_REG_AX: case X86_REG_AL: return X86_REG_RAX;
    case X86_REG_RBX: case X86_REG_EBX: case X86_REG_BX: case X86_REG_BL: return X86_REG_RBX;
    case X86_REG_RCX: case X86_REG_ECX: case X86_REG_CX: case X86_REG_CL: return X86_REG_RCX;
    case X86_REG_RDX: case X86_REG_EDX: case X86_REG_DX: case X86_REG_DL: return X86_REG_RDX;
    case X86_REG_RSI: case X86_REG_ESI: case X86_REG_SI: case X86_REG_SIL: return X86_REG_RSI;
    case X86_REG_RDI: case X86_REG_EDI: case X86_REG_DI: case X86_REG_DIL: return X86_REG_RDI;
    case X86_REG_RBP: case X86_REG_EBP: return X86_REG_RBP;
    case X86_REG_RSP: case X86_REG_ESP: return X86_REG_RSP;
    case X86_REG_R8: case X86_REG_R8D: case X86_REG_R8W: case X86_REG_R8B: return X86_REG_R8;
    case X86_REG_R9: case X86_REG_R9D: case X86_REG_R9W: case X86_REG_R9B: return X86_REG_R9;
    case X86_REG_R10: case X86_REG_R10D: case X86_REG_R10W: case X86_REG_R10B: return X86_REG_R10;
    case X86_REG_R11: case X86_REG_R11D: case X86_REG_R11W: case X86_REG_R11B: return X86_REG_R11;
    case X86_REG_R12: case X86_REG_R12D: case X86_REG_R12W: case X86_REG_R12B: return X86_REG_R12;
    case X86_REG_R13: case X86_REG_R13D: case X86_REG_R13W: case X86_REG_R13B: return X86_REG_R13;
    case X86_REG_R14: case X86_REG_R14D: case X86_REG_R14W: case X86_REG_R14B: return X86_REG_R14;
    case X86_REG_R15: case X86_REG_R15D: case X86_REG_R15W: case X86_REG_R15B: return X86_REG_R15;
    default: return X86_REG_INVALID;
  }
}

constexpr unsigned kArgRegs[6] = {X86_REG_RDI, X86_REG_RSI, X86_REG_RDX, X86_REG_RCX, X86_REG_R8, X86_REG_R9};

// A library call with its first six arguments, as far as they could be followed.
struct LibCall {
  std::uint64_t addr = 0;
  std::string name;
  Val args[6];
  std::optional<std::string> str[6];  // the argument as text, where it points at one
};

// printf conversions (%s, %d, %08lx...) become "{}"; "%%" is a percent sign.
std::string printf_template(const std::string& fmt) {
  std::string out;
  for (std::size_t i = 0; i < fmt.size(); ++i) {
    if (fmt[i] != '%') {
      out += fmt[i];
      continue;
    }
    if (i + 1 < fmt.size() && fmt[i + 1] == '%') {
      out += '%';
      ++i;
      continue;
    }
    std::size_t j = i + 1;
    while (j < fmt.size() && std::strchr("-+ #0123456789.*hlLqjzt", fmt[j])) ++j;
    out += "{}";
    i = j;
  }
  return out;
}

class Lifter {
 public:
  Lifter(const elf::File& f, const std::map<std::uint64_t, std::string>& plt) : f_(f), plt_(plt) {
    cs_open(CS_ARCH_X86, CS_MODE_64, &h_);
    cs_option(h_, CS_OPT_DETAIL, CS_OPT_ON);
  }
  ~Lifter() { cs_close(&h_); }
  Lifter(const Lifter&) = delete;
  Lifter& operator=(const Lifter&) = delete;

  std::vector<LibCall> run(std::uint64_t start, std::uint64_t end) {
    regs_.clear();
    slots_.clear();
    buffers_.clear();
    calls_.clear();
    const elf::Section* sec = f_.section_at(start);
    if (!sec || end <= start) return {};
    const auto bytes = f_.bytes(*sec);
    const std::uint64_t off = start - sec->addr;
    if (off >= bytes.size()) return {};
    const std::uint8_t* p = bytes.data() + off;
    std::size_t left = static_cast<std::size_t>(std::min<std::uint64_t>(end - start, bytes.size() - off));
    std::uint64_t a = start;
    cs_insn* insn = cs_malloc(h_);
    while (left > 0 && cs_disasm_iter(h_, &p, &left, &a, insn)) step(*insn);
    cs_free(insn, 1);
    return std::move(calls_);
  }

 private:
  const elf::File& f_;
  const std::map<std::uint64_t, std::string>& plt_;
  csh h_ = 0;
  std::map<unsigned, Val> regs_;
  std::map<std::pair<unsigned, std::int64_t>, Val> slots_;            // stack slots
  std::map<std::pair<unsigned, std::int64_t>, std::string> buffers_;  // stack buffers holding text
  std::vector<LibCall> calls_;

  Val reg(unsigned r) const {
    if (r == X86_REG_RSP || r == X86_REG_RBP) return {Val::K::Stack, 0, r};
    auto it = regs_.find(r);
    return it == regs_.end() ? Val{} : it->second;
  }

  // A memory operand as a stack slot (base rsp/rbp, no index), if it is one.
  static std::optional<std::pair<unsigned, std::int64_t>> slot(const cs_x86_op& op) {
    const unsigned b = canon(op.mem.base);
    if (op.type != X86_OP_MEM || op.mem.index != X86_REG_INVALID || (b != X86_REG_RSP && b != X86_REG_RBP)) {
      return std::nullopt;
    }
    return std::make_pair(b, op.mem.disp);
  }

  std::optional<std::string> text_of(const Val& v) const {
    if (v.k == Val::K::Addr) return f_.string_at(v.v);
    if (v.k == Val::K::Stack) {
      if (auto it = buffers_.find({v.base, static_cast<std::int64_t>(v.v)}); it != buffers_.end()) return it->second;
    }
    return std::nullopt;
  }

  std::optional<std::string> import_of(const cs_insn& i) const {
    const cs_x86& x = i.detail->x86;
    if (x.op_count != 1) return std::nullopt;
    const cs_x86_op& op = x.operands[0];
    if (op.type == X86_OP_IMM) {
      if (auto it = plt_.find(static_cast<std::uint64_t>(op.imm)); it != plt_.end()) return it->second;
    } else if (op.type == X86_OP_MEM && op.mem.base == X86_REG_RIP && op.mem.index == X86_REG_INVALID) {
      const std::uint64_t slot_addr = i.address + i.size + static_cast<std::uint64_t>(op.mem.disp);
      if (auto it = f_.got_imports.find(slot_addr); it != f_.got_imports.end()) return it->second;
    }
    return std::nullopt;
  }

  void on_library_call(std::uint64_t addr, const std::string& name) {
    LibCall c;
    c.addr = addr;
    c.name = name;
    for (int k = 0; k < 6; ++k) {
      c.args[k] = reg(kArgRegs[k]);
      c.str[k] = text_of(c.args[k]);
    }
    // Writers of local buffers: what the buffer holds afterwards (printf formats as templates).
    auto into = [&](int buf, int src, bool format) {
      if (c.args[buf].k != Val::K::Stack || !c.str[src]) return;
      buffers_[{c.args[buf].base, static_cast<std::int64_t>(c.args[buf].v)}] = format ? printf_template(*c.str[src]) : *c.str[src];
    };
    if (name == "snprintf") into(0, 2, true);
    else if (name == "sprintf") into(0, 1, true);
    else if (name == "__snprintf_chk") into(0, 4, true);  // _FORTIFY_SOURCE versions
    else if (name == "__sprintf_chk") into(0, 3, true);
    else if (name == "strcpy" || name == "__strcpy_chk" || name == "stpcpy") into(0, 1, false);
    calls_.push_back(std::move(c));
  }

  void clobber_after_call() {
    for (unsigned r : {X86_REG_RAX, X86_REG_RCX, X86_REG_RDX, X86_REG_RSI, X86_REG_RDI, X86_REG_R8, X86_REG_R9,
                       X86_REG_R10, X86_REG_R11}) {
      regs_.erase(r);
    }
  }

  void step(const cs_insn& i) {
    const cs_x86& x = i.detail->x86;
    if (i.id == X86_INS_CALL || i.id == X86_INS_JMP) {
      if (auto name = import_of(i)) on_library_call(i.address, *name);
      if (i.id == X86_INS_CALL) clobber_after_call();
      return;
    }
    if ((i.id == X86_INS_MOV || i.id == X86_INS_MOVABS) && x.op_count == 2) {
      const cs_x86_op &dst = x.operands[0], &src = x.operands[1];
      if (dst.type == X86_OP_REG) {
        const unsigned d = canon(dst.reg);
        if (d == X86_REG_INVALID || d == X86_REG_RSP) return;
        if (src.type == X86_OP_IMM) regs_[d] = {Val::K::Imm, static_cast<std::uint64_t>(src.imm), 0};
        else if (src.type == X86_OP_REG) regs_[d] = reg(canon(src.reg));
        else if (auto sl = slot(src)) regs_[d] = slots_.count(*sl) ? slots_[*sl] : Val{};
        else if (src.type == X86_OP_MEM && src.mem.base == X86_REG_RIP && src.mem.index == X86_REG_INVALID && dst.size == 8) {
          // A pointer stored in data (static const char *URL = "..."): load what it holds.
          const auto ptr = f_.pointer_at(i.address + i.size + static_cast<std::uint64_t>(src.mem.disp));
          regs_[d] = ptr ? Val{Val::K::Addr, *ptr, 0} : Val{};
        } else {
          regs_.erase(d);
        }
        return;
      }
      if (auto sl = slot(dst)) {
        if (src.type == X86_OP_REG) slots_[*sl] = reg(canon(src.reg));
        else if (src.type == X86_OP_IMM) slots_[*sl] = {Val::K::Imm, static_cast<std::uint64_t>(src.imm), 0};
        else slots_.erase(*sl);
      }
      return;
    }
    if (i.id == X86_INS_LEA && x.op_count == 2 && x.operands[0].type == X86_OP_REG) {
      const unsigned d = canon(x.operands[0].reg);
      const cs_x86_op& m = x.operands[1];
      if (d == X86_REG_INVALID || d == X86_REG_RSP) return;
      if (m.mem.base == X86_REG_RIP && m.mem.index == X86_REG_INVALID) {
        regs_[d] = {Val::K::Addr, i.address + i.size + static_cast<std::uint64_t>(m.mem.disp), 0};
      } else if (auto sl = slot(m)) {
        regs_[d] = {Val::K::Stack, static_cast<std::uint64_t>(sl->second), sl->first};
      } else {
        regs_.erase(d);
      }
      return;
    }
    if (i.id == X86_INS_XOR && x.op_count == 2 && x.operands[0].type == X86_OP_REG && x.operands[1].type == X86_OP_REG &&
        canon(x.operands[0].reg) == canon(x.operands[1].reg)) {
      regs_[canon(x.operands[0].reg)] = {Val::K::Imm, 0, 0};
      return;
    }
    // Anything else that writes a register makes it unknown.
    for (std::uint8_t k = 0; k < x.op_count; ++k) {
      const cs_x86_op& op = x.operands[k];
      if (op.type == X86_OP_REG && (op.access & CS_AC_WRITE)) regs_.erase(canon(op.reg));
      if (auto sl = slot(op); sl && (op.access & CS_AC_WRITE)) slots_.erase(*sl);
    }
  }
};

// What a function's library calls reach: HTTP endpoints (libcurl) and SQL tables.
struct Reach {
  std::string node_id;  // http:... or sql:...
  EdgeKind kind = EdgeKind::Reads;
  std::string method;   // HTTP only
  std::uint64_t addr = 0;
};

std::vector<Reach> reach_of(const std::vector<LibCall>& calls) {
  std::vector<Reach> out;
  // libcurl: each curl_easy_perform uses the URL and the method options set since the previous
  // one. Grouping by perform (not by function) keeps requests apart when gcc inlines several
  // functions into one.
  std::optional<std::string> url;
  std::uint64_t url_addr = 0;
  std::string method;
  auto flush = [&] {
    if (url) {
      const std::string id = http_node_id(*url);
      const std::string m = method.empty() ? "GET" : method;
      if (!id.empty()) {
        out.push_back({id, (m == "GET" || m == "HEAD" || m == "OPTIONS") ? EdgeKind::Reads : EdgeKind::SendsTo, m, url_addr});
      }
    }
    url.reset();
    method.clear();
  };
  for (const LibCall& c : calls) {
    if (c.name == "curl_easy_setopt" && c.args[1].k == Val::K::Imm) {
      switch (static_cast<std::uint32_t>(c.args[1].v)) {
        case 10002: url = c.str[2]; url_addr = c.addr; break;                    // CURLOPT_URL
        case 47: case 10015: case 10165: case 10269: if (method.empty()) method = "POST"; break;
        case 46: case 54: method = "PUT"; break;                                // UPLOAD, PUT
        case 44: method = "HEAD"; break;                                        // NOBODY
        case 10036: if (c.str[2] && c.str[2]->find("{}") == std::string::npos) method = frontends::to_upper(*c.str[2]); break;
        default: break;
      }
    } else if (c.name == "curl_easy_perform") {
      flush();
    }
    static const std::unordered_map<std::string, int> kSql = {
        {"sqlite3_exec", 1}, {"sqlite3_prepare", 1}, {"sqlite3_prepare_v2", 1}, {"sqlite3_prepare_v3", 1},
        {"PQexec", 1},       {"PQexecParams", 1},    {"PQprepare", 2},          {"PQsendQuery", 1},
        {"mysql_query", 1},  {"mysql_real_query", 1}};
    if (auto it = kSql.find(c.name); it != kSql.end() && c.str[it->second]) {
      const SqlTables t = sql_tables(*c.str[it->second]);
      for (const auto& r : t.reads) out.push_back({sql_store_id(r), EdgeKind::Reads, "", c.addr});
      for (const auto& w : t.writes) out.push_back({sql_store_id(w), EdgeKind::Writes, "", c.addr});
    }
  }
  flush();
  return out;
}

// Which library exports a function, for imports without a symbol version (sqlite, most non-glibc
// libraries). Looks for each DT_NEEDED library like the dynamic linker would: the RUNPATH/RPATH
// entries ($ORIGIN = the binary's directory), then the system library directories. Each library's
// exports are read once per run.
class LibraryIndex {
 public:
  std::string library_of(const std::string& symbol, const elf::File& f) {
    for (const std::string& lib : f.needed) {
      const auto* exports = exports_of(lib, f);
      if (exports && exports->count(symbol)) return lib;
    }
    return "";
  }

 private:
  std::map<std::string, std::optional<std::unordered_set<std::string>>> cache_;  // path -> exports

  const std::unordered_set<std::string>* exports_of(const std::string& lib, const elf::File& f) {
    const std::string dir = fs::absolute(f.path).parent_path().string();
    std::vector<std::string> search;
    for (std::string p : f.runpath) {
      if (auto at = p.find("$ORIGIN"); at != std::string::npos) p.replace(at, 7, dir);
      search.push_back(p);
    }
    for (const char* sys : {"/lib/x86_64-linux-gnu", "/usr/lib/x86_64-linux-gnu", "/lib64", "/usr/lib64", "/lib",
                            "/usr/lib", "/usr/local/lib"}) {
      search.emplace_back(sys);
    }
    for (const std::string& d : search) {
      const fs::path candidate = fs::path(d) / lib;
      std::error_code ec;
      if (!fs::is_regular_file(candidate, ec)) continue;
      const std::string key = fs::weakly_canonical(candidate, ec).string();
      auto [it, inserted] = cache_.try_emplace(key);
      if (inserted) {
        try {
          const elf::File l = elf::read(candidate);
          std::unordered_set<std::string> names;
          for (const auto& s : l.dynsym) {
            if (s.defined() && (s.is_function() || s.type == 10)) names.insert(s.name);  // FUNC, GNU_IFUNC
          }
          it->second = std::move(names);
        } catch (const IrError&) {
          it->second.reset();  // not a readable library: keep looking
        }
      }
      if (it->second) return &*it->second;
    }
    return nullptr;
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
  BinaryAnalysis(const elf::File& f, std::string rel, fs::path root) : f_(f), rel_(std::move(rel)), root_(std::move(root)) {}

  void run(std::string* diagnostics) {
    map_plt();
    sweep_code();
    find_functions();
    read_debug_info(diagnostics);
  }

  void emit(Graph& g, BinaryStats& stats, LibraryIndex& libs) const;
  nlohmann::json disassembly() const;

 private:
  const elf::File& f_;
  std::string rel_;
  fs::path root_;
  std::map<std::uint64_t, std::string> plt_;  // PLT stub start -> imported function
  struct Source {
    std::string path;  // as the source frontends name it (relative to the analysed directory if possible)
    std::string name;
    std::uint32_t line = 0;
  };
  std::unordered_map<std::uint64_t, Source> source_;  // function start -> its source, from debug info

  // Debug info gives functions their source ids (decision 0005): c:<file>#<name>.
  void read_debug_info(std::string* diagnostics) {
    std::vector<dwarf::Function> fns;
    try {
      fns = dwarf::functions(f_);
    } catch (const IrError& e) {
      if (diagnostics) *diagnostics += std::string(e.what()) + " (analysed without it)\n";
      return;
    }
    const fs::path base = fs::absolute(root_).lexically_normal();
    auto source_path = [&](const std::string& file) {
      fs::path p(file);
      if (p.is_absolute()) {
        const fs::path rel = p.lexically_relative(base);
        if (!rel.empty() && *rel.begin() != "..") p = rel;
      }
      std::string path = p.lexically_normal().generic_string();
      return path.starts_with("./") ? path.substr(2) : path;
    };
    // Names defined in more than one file (two files' static helpers) can't be matched by name.
    std::unordered_map<std::string, std::unordered_set<std::string>> files_of;
    for (const auto& d : fns) {
      if (!d.file.empty()) files_of[d.name].insert(d.file);
    }
    std::unordered_map<std::string, std::optional<Source>> declared;  // name -> definition without code
    for (const auto& d : fns) {
      if (d.file.empty()) continue;
      if (!d.has_code) {
        if (files_of[d.name].size() == 1) declared.try_emplace(d.name, Source{source_path(d.file), d.name, d.line});
        continue;
      }
      if (!funcs_.count(d.start)) continue;
      const Source src{source_path(d.file), d.name, d.line};
      auto [it, inserted] = source_.try_emplace(d.start, src);
      if (!inserted && d.is_main_range) it->second = src;
    }
    // A symbol whose code has no debug entry, but whose name has exactly one declaration.
    for (const auto& [a, fn] : funcs_) {
      if (source_.count(a) || fn.name.empty()) continue;
      if (auto it = declared.find(fn.name); it != declared.end() && it->second) source_.emplace(a, *it->second);
    }
  }
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

void BinaryAnalysis::emit(Graph& g, BinaryStats& stats, LibraryIndex& libs) const {
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
  std::unordered_set<std::string> added;
  // Exported functions by name: in position-independent code, calls between a library's own
  // exported functions go through its PLT, as if they were imports.
  std::unordered_map<std::string, std::uint64_t> exported;
  for (const auto& s : f_.dynsym) {
    if (s.is_function() && s.defined() && funcs_.count(s.value)) exported.emplace(s.name, s.value);
  }
  for (const auto& [a, fn] : funcs_) {
    std::string id = mod_id + "!";
    const auto src = source_.find(a);
    if (src != source_.end()) {
      // The source id; the symbol's name when there is one keeps GCC's .cold / .part suffixes.
      id = "c:" + src->second.path + "#" + (fn.name.empty() ? src->second.name : fn.name);
    } else if (fn.name.empty()) {
      id += hex(a);
    } else if (name_count[fn.name] > 1) {
      id += fn.name + "@" + hex(a);
    } else {
      id += fn.name;
    }
    ids[a] = id;
    Node n;
    n.id = id;
    n.kind = NodeKind::Function;
    n.name = fn.name.empty() ? (src != source_.end() ? src->second.name : hex(a)) : fn.name;
    n.loc = src != source_.end() ? SourceLoc{src->second.path, src->second.line, 0, a} : SourceLoc{rel_, 0, 0, a};
    n.frontend = "binary";
    n.confidence = fn.gap ? Confidence::Inferred : Confidence::Exact;
    n.attrs = {{"size", fn.end - a}, {"found", fn.found}, {"address", hex(a)}, {"binary", rel_}};
    if (fn.is_static) n.attrs["static"] = true;
    // Copies of one source function (a static inline in a header, emitted in several files)
    // share an id and become one node.
    if (!added.insert(id).second) continue;
    g.add_node(std::move(n));
    Edge e;
    e.from = *g.find(mod_id);
    e.to = *g.find(id);
    e.kind = EdgeKind::Defines;
    e.frontend = "binary";
    g.add_edge(std::move(e));
  }

  // Imports: the libraries named in DT_NEEDED, and the functions called from them.
  // The library an import comes from: its symbol version names it; else the library among
  // DT_NEEDED that exports it (looked up on this machine); else the only DT_NEEDED; else "?".
  auto library_of = [&](const std::string& fn) -> std::string {
    for (const auto& s : f_.dynsym) {
      if (s.name == fn && !s.defined() && !s.library.empty()) return s.library;
    }
    if (std::string lib = libs.library_of(fn, f_); !lib.empty()) return lib;
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
  // APIs and tables reached: lift the functions that call libraries we understand.
  static const std::unordered_set<std::string> kGlue = {
      "curl_easy_setopt", "curl_easy_perform", "sqlite3_exec", "sqlite3_prepare", "sqlite3_prepare_v2",
      "sqlite3_prepare_v3", "PQexec", "PQexecParams", "PQprepare", "PQsendQuery", "mysql_query", "mysql_real_query"};
  std::set<std::uint64_t> glue_funcs;
  for (const Insn& i : code_) {
    if (i.kind != Kind::Call && i.kind != Kind::Jump && !i.rip_slot) continue;
    if (auto imp = import_at(i); imp && kGlue.count(*imp)) {
      if (const std::uint64_t fn = func_of(i.addr)) glue_funcs.insert(fn);
    }
  }
  if (!glue_funcs.empty()) {
    Lifter lifter(f_, plt_);
    for (std::uint64_t fn : glue_funcs) {
      for (const Reach& r : reach_of(lifter.run(fn, funcs_.at(fn).end))) {
        if (!g.find(r.node_id)) {
          if (r.node_id.starts_with("http:")) g.add_node(frontends::http_external_node(r.node_id, "binary"));
          else g.add_node(frontends::sql_store_node(r.node_id, r.node_id.substr(4), "binary"));
        }
        Edge e;
        e.from = *g.find(ids.at(fn));
        e.to = *g.find(r.node_id);
        e.kind = r.kind;
        e.confidence = r.node_id.find("{}") == std::string::npos ? Confidence::Exact : Confidence::Inferred;
        e.frontend = "binary";
        e.loc = SourceLoc{rel_, 0, 0, r.addr};
        if (!r.method.empty()) e.attrs = {{"method", r.method}};
        g.add_edge(std::move(e));
      }
    }
  }
  stats.functions += funcs_.size();
  stats.instructions += code_.size();
}

}  // namespace

nlohmann::json BinaryAnalysis::disassembly() const {
  csh h = 0;
  if (cs_open(CS_ARCH_X86, CS_MODE_64, &h) != CS_ERR_OK) throw IrError("capstone: cannot open x86-64");
  cs_option(h, CS_OPT_DETAIL, CS_OPT_ON);
  cs_insn* insn = cs_malloc(h);
  nlohmann::json out = nlohmann::json::array();
  for (const auto& [start, fn] : funcs_) {
    nlohmann::json j = {{"start", hex(start)}, {"size", fn.end - start}, {"found", fn.found}};
    if (!fn.name.empty()) j["name"] = fn.name;
    nlohmann::json insns = nlohmann::json::array();
    const elf::Section* sec = f_.section_at(start);
    if (sec && fn.end > start) {
      const auto bytes = f_.bytes(*sec);
      const std::uint64_t off = start - sec->addr;
      const std::uint8_t* p = bytes.data() + off;
      std::size_t left = static_cast<std::size_t>(std::min<std::uint64_t>(fn.end - start, bytes.size() - off));
      std::uint64_t a = start;
      while (left > 0 && cs_disasm_iter(h, &p, &left, &a, insn)) {
        const cs_x86& x = insn->detail->x86;
        nlohmann::json ops = nlohmann::json::array();
        for (std::uint8_t k = 0; k < x.op_count; ++k) {
          const cs_x86_op& op = x.operands[k];
          if (op.type == X86_OP_REG) {
            ops.push_back({{"k", "reg"}, {"r", cs_reg_name(h, op.reg)}, {"s", op.size}});
          } else if (op.type == X86_OP_IMM) {
            ops.push_back({{"k", "imm"}, {"v", op.imm}, {"s", op.size}});
          } else if (op.type == X86_OP_MEM) {
            nlohmann::json m = {{"k", "mem"}, {"s", op.size}, {"disp", op.mem.disp}, {"scale", op.mem.scale}};
            m["base"] = op.mem.base == X86_REG_INVALID ? "" : cs_reg_name(h, op.mem.base);
            m["index"] = op.mem.index == X86_REG_INVALID ? "" : cs_reg_name(h, op.mem.index);
            ops.push_back(std::move(m));
          }
        }
        // Bytes, with fields that depend on where code and data were placed masked out:
        // rip-relative displacements and relative branch targets.
        std::string b;
        static const char* kHex = "0123456789abcdef";
        const bool rip = std::any_of(x.operands, x.operands + x.op_count,
                                     [](const cs_x86_op& o) { return o.type == X86_OP_MEM && o.mem.base == X86_REG_RIP; });
        const bool rel_branch = cs_insn_group(h, insn, CS_GRP_JUMP) || cs_insn_group(h, insn, CS_GRP_CALL);
        for (std::uint16_t i = 0; i < insn->size; ++i) {
          const bool masked = (rip && x.encoding.disp_size && i >= x.encoding.disp_offset &&
                               i < x.encoding.disp_offset + x.encoding.disp_size) ||
                              (rel_branch && x.encoding.imm_size && i >= x.encoding.imm_offset &&
                               i < x.encoding.imm_offset + x.encoding.imm_size);
          b += masked ? "??" : std::string{kHex[insn->bytes[i] >> 4], kHex[insn->bytes[i] & 15]};
        }
        nlohmann::json ij = {{"m", insn->mnemonic}, {"o", std::move(ops)}, {"b", std::move(b)}};
        // Text the instruction points at (lea r, [rip+x] to a string literal): it survives
        // recompilation at another optimisation level when the code around it doesn't.
        for (std::uint8_t k = 0; k < x.op_count; ++k) {
          const cs_x86_op& op = x.operands[k];
          if (op.type == X86_OP_MEM && op.mem.base == X86_REG_RIP && op.mem.index == X86_REG_INVALID) {
            const std::uint64_t at = insn->address + insn->size + static_cast<std::uint64_t>(op.mem.disp);
            if (auto text = f_.string_at(at, 200); text && text->size() >= 4) ij["str"] = *text;
          }
        }
        // Where a call or jump goes.
        if (rel_branch || insn->id == X86_INS_CALL || insn->id == X86_INS_JMP) {
          Insn rec;
          rec.addr = insn->address;
          rec.size = static_cast<std::uint8_t>(insn->size);
          if (x.op_count == 1 && x.operands[0].type == X86_OP_IMM) rec.target = static_cast<std::uint64_t>(x.operands[0].imm);
          if (x.op_count == 1 && x.operands[0].type == X86_OP_MEM && x.operands[0].mem.base == X86_REG_RIP) {
            rec.target = insn->address + insn->size + static_cast<std::uint64_t>(x.operands[0].mem.disp);
            rec.rip_slot = true;
          }
          if (auto imp = import_at(rec)) ij["to_import"] = *imp;
          else if (!rec.rip_slot && rec.target >= start && rec.target < fn.end) ij["to_offset"] = rec.target - start;
          else if (!rec.rip_slot && funcs_.count(rec.target)) ij["to_function"] = hex(rec.target);
        }
        insns.push_back(std::move(ij));
      }
    }
    j["insns"] = std::move(insns);
    out.push_back(std::move(j));
  }
  cs_free(insn, 1);
  cs_close(&h);
  return out;
}

nlohmann::json disassemble_binary(const fs::path& file) {
  const elf::File f = elf::read(file);
  if (f.machine != 62 || (f.type != 2 && f.type != 3)) throw IrError(file.string() + ": not an x86-64 executable or shared library");
  BinaryAnalysis a(f, file.filename().string(), file.parent_path());
  a.run(nullptr);
  return {{"binary", file.string()}, {"functions", a.disassembly()}};
}

Graph extract_binary_files(const fs::path& root, const std::vector<fs::path>& files, BinaryStats* stats,
                           std::string* diagnostics) {
  Graph g;
  g.source = root.string();
  BinaryStats local;
  BinaryStats& st = stats ? *stats : local;
  LibraryIndex libs;
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
      BinaryAnalysis a(f, rel.generic_string(), root);
      a.run(diagnostics);
      a.emit(g, st, libs);
      ++st.files;
    } catch (const IrError& e) {
      ++st.skipped;
      if (diagnostics) *diagnostics += std::string(e.what()) + "\n";
    }
  }
  return g;
}

}  // namespace optum
