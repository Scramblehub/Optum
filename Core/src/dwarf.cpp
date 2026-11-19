#include "optum/dwarf.hpp"

#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <unordered_map>

#include "optum/ir.hpp"

namespace optum::dwarf {
namespace {

// Bounds-checked cursor over one section's bytes.
class Cursor {
 public:
  Cursor(std::span<const std::uint8_t> d, std::uint64_t pos = 0) : d_(d), pos_(pos) {}
  std::uint64_t pos() const { return pos_; }
  void seek(std::uint64_t p) {
    if (p > d_.size()) fail();
    pos_ = p;
  }
  bool at_end() const { return pos_ >= d_.size(); }
  std::uint64_t size() const { return d_.size(); }

  template <typename T>
  T get() {
    if (pos_ > d_.size() || sizeof(T) > d_.size() - pos_) fail();
    T v;
    std::memcpy(&v, d_.data() + pos_, sizeof(T));
    pos_ += sizeof(T);
    return v;
  }
  std::uint64_t uint(int bytes) {
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v |= std::uint64_t(get<std::uint8_t>()) << (8 * i);
    return v;
  }
  std::uint64_t uleb() {
    std::uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      const auto b = get<std::uint8_t>();
      v |= std::uint64_t(b & 0x7f) << shift;
      if (!(b & 0x80)) return v;
    }
    fail();
  }
  std::int64_t sleb() {
    std::int64_t v = 0;
    int shift = 0;
    std::uint8_t b;
    do {
      b = get<std::uint8_t>();
      if (shift < 64) v |= std::int64_t(b & 0x7f) << shift;
      shift += 7;
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40)) v |= -(std::int64_t(1) << shift);
    return v;
  }
  std::string cstr() {
    if (pos_ >= d_.size()) fail();
    const void* end = std::memchr(d_.data() + pos_, 0, d_.size() - pos_);
    if (!end) fail();
    std::string s(reinterpret_cast<const char*>(d_.data() + pos_),
                  static_cast<const std::uint8_t*>(end) - (d_.data() + pos_));
    pos_ += s.size() + 1;
    return s;
  }
  void cstr_skip() {
    if (pos_ >= d_.size()) fail();
    const void* end = std::memchr(d_.data() + pos_, 0, d_.size() - pos_);
    if (!end) fail();
    pos_ = static_cast<std::uint64_t>(static_cast<const std::uint8_t*>(end) - d_.data()) + 1;
  }
  void skip(std::uint64_t n) {
    if (n > d_.size() - std::min<std::uint64_t>(pos_, d_.size())) fail();
    pos_ += n;
  }
  // A string at an offset (for .debug_str / .debug_line_str), without moving.
  std::string cstr_at(std::uint64_t off) const {
    Cursor c(d_, off);
    return c.cstr();
  }

  [[noreturn]] static void fail() { throw IrError("malformed DWARF debug info"); }

 private:
  std::span<const std::uint8_t> d_;
  std::uint64_t pos_;
};

struct Attr {
  std::uint64_t name, form;
  std::int64_t implicit = 0;
};
struct Abbrev {
  std::uint64_t tag = 0;
  bool children = false;
  std::vector<Attr> attrs;
};

// The value of one attribute, as far as we need it.
struct Value {
  std::uint64_t u = 0;    // numbers, offsets, indexes
  std::string s;          // inline strings
  bool is_string = false;
  bool is_ref = false;    // a reference to another DIE (u: offset in .debug_info)
  bool needs_str_index = false, needs_addr_index = false, needs_rnglist_index = false;
  std::uint64_t form = 0;
};

struct Sections {
  std::span<const std::uint8_t> info, abbrev, str, line_str, line, str_offsets, addr, rnglists, ranges;
};

struct Unit {
  std::uint64_t offset = 0;  // in .debug_info
  std::uint16_t version = 0;
  std::uint8_t addr_size = 8;
  int offset_size = 4;  // 8 for 64-bit DWARF
  std::uint64_t str_offsets_base = 8, addr_base = 8, rnglists_base = 0;
  std::uint64_t low_pc = 0;  // base address for range lists
  std::string comp_dir;
  std::vector<std::string> files;  // the line table's file names, full paths
};

struct Die {  // a subprogram, before origins are followed
  std::uint64_t offset = 0;
  std::size_t unit = 0;
  std::string name;
  std::int64_t decl_file = -1;
  std::uint32_t line = 0;
  std::uint64_t origin = 0;  // abstract_origin or specification, a .debug_info offset
  bool declaration = false;  // DW_AT_declaration: a prototype, possibly in a caller's file
  std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;  // [start, end)
};

Value read_form(Cursor& c, std::uint64_t form, const Unit& u, std::int64_t implicit) {
  Value v;
  v.form = form;
  const int off = u.offset_size;
  switch (form) {
    case 0x01: v.u = c.uint(u.addr_size); break;                         // addr
    case 0x03: c.skip(c.get<std::uint16_t>()); break;                   // block2
    case 0x04: c.skip(c.get<std::uint32_t>()); break;                   // block4
    case 0x05: v.u = c.get<std::uint16_t>(); break;                     // data2
    case 0x06: v.u = c.get<std::uint32_t>(); break;                     // data4
    case 0x07: v.u = c.get<std::uint64_t>(); break;                     // data8
    case 0x08: v.s = c.cstr(); v.is_string = true; break;               // string
    case 0x09: case 0x18: c.skip(c.uleb()); break;                      // block, exprloc
    case 0x0a: c.skip(c.get<std::uint8_t>()); break;                    // block1
    case 0x0b: v.u = c.get<std::uint8_t>(); break;                      // data1
    case 0x0c: v.u = c.get<std::uint8_t>(); break;                      // flag
    case 0x0d: v.u = static_cast<std::uint64_t>(c.sleb()); break;       // sdata
    case 0x0e: v.u = c.uint(off); v.is_string = true; break;            // strp (resolved later)
    case 0x0f: v.u = c.uleb(); break;                                   // udata
    case 0x10: v.u = c.uint(u.version <= 2 ? u.addr_size : off); v.is_ref = true; break;  // ref_addr
    case 0x11: v.u = u.offset + c.get<std::uint8_t>(); v.is_ref = true; break;   // ref1
    case 0x12: v.u = u.offset + c.get<std::uint16_t>(); v.is_ref = true; break;  // ref2
    case 0x13: v.u = u.offset + c.get<std::uint32_t>(); v.is_ref = true; break;  // ref4
    case 0x14: v.u = u.offset + c.get<std::uint64_t>(); v.is_ref = true; break;  // ref8
    case 0x15: v.u = u.offset + c.uleb(); v.is_ref = true; break;                // ref_udata
    case 0x16: return read_form(c, c.uleb(), u, implicit);                       // indirect
    case 0x17: v.u = c.uint(off); break;                                // sec_offset
    case 0x19: v.u = 1; break;                                          // flag_present
    case 0x1a: v.u = c.uleb(); v.needs_str_index = true; break;         // strx
    case 0x1b: v.u = c.uleb(); v.needs_addr_index = true; break;        // addrx
    case 0x1c: c.skip(4); break;                                        // ref_sup4
    case 0x1d: c.skip(off); break;                                      // strp_sup
    case 0x1e: c.skip(16); break;                                       // data16
    case 0x1f: v.u = c.uint(off); v.is_string = true; break;            // line_strp (resolved later)
    case 0x20: c.skip(8); break;                                        // ref_sig8
    case 0x21: v.u = static_cast<std::uint64_t>(implicit); break;       // implicit_const
    case 0x22: v.u = c.uleb(); break;                                   // loclistx
    case 0x23: v.u = c.uleb(); v.needs_rnglist_index = true; break;     // rnglistx
    case 0x24: c.skip(8); break;                                        // ref_sup8
    case 0x25: v.u = c.uint(1); v.needs_str_index = true; break;        // strx1..4
    case 0x26: v.u = c.uint(2); v.needs_str_index = true; break;
    case 0x27: v.u = c.uint(3); v.needs_str_index = true; break;
    case 0x28: v.u = c.uint(4); v.needs_str_index = true; break;
    case 0x29: v.u = c.uint(1); v.needs_addr_index = true; break;       // addrx1..4
    case 0x2a: v.u = c.uint(2); v.needs_addr_index = true; break;
    case 0x2b: v.u = c.uint(3); v.needs_addr_index = true; break;
    case 0x2c: v.u = c.uint(4); v.needs_addr_index = true; break;
    default: Cursor::fail();
  }
  return v;
}

// Advances past one attribute value without building a Value: most DIEs in a big binary
// (variables, types, inlined calls) are not subprograms and their attributes are never read.
// Mirrors read_form's sizes, including its failure on unknown forms.
void skip_form(Cursor& c, std::uint64_t form, const Unit& u) {
  const std::uint64_t off = static_cast<std::uint64_t>(u.offset_size);
  switch (form) {
    case 0x19: case 0x21: break;                                              // flag_present, implicit_const
    case 0x0b: case 0x0c: case 0x11: case 0x25: case 0x29: c.skip(1); break;  // data1, flag, ref1, strx1, addrx1
    case 0x05: case 0x12: case 0x26: case 0x2a: c.skip(2); break;             // data2, ref2, strx2, addrx2
    case 0x27: case 0x2b: c.skip(3); break;                                   // strx3, addrx3
    case 0x06: case 0x13: case 0x1c: case 0x28: case 0x2c: c.skip(4); break;  // data4, ref4, ref_sup4, strx4, addrx4
    case 0x07: case 0x14: case 0x20: case 0x24: c.skip(8); break;             // data8, ref8, ref_sig8, ref_sup8
    case 0x1e: c.skip(16); break;                                             // data16
    case 0x01: c.skip(u.addr_size); break;                                    // addr
    case 0x0e: case 0x17: case 0x1d: case 0x1f: c.skip(off); break;           // strp, sec_offset, strp_sup, line_strp
    case 0x10: c.skip(u.version <= 2 ? u.addr_size : off); break;             // ref_addr
    case 0x0d: c.sleb(); break;                                               // sdata
    case 0x0f: case 0x15: case 0x1a: case 0x1b: case 0x22: case 0x23: c.uleb(); break;  // udata, ref_udata, strx, addrx, loclistx, rnglistx
    case 0x08: c.cstr_skip(); break;                                          // string
    case 0x03: c.skip(c.get<std::uint16_t>()); break;                         // block2
    case 0x04: c.skip(c.get<std::uint32_t>()); break;                         // block4
    case 0x09: case 0x18: c.skip(c.uleb()); break;                            // block, exprloc
    case 0x0a: c.skip(c.get<std::uint8_t>()); break;                          // block1
    case 0x16: skip_form(c, c.uleb(), u); break;                              // indirect
    default: Cursor::fail();
  }
}

// The attributes read_unit looks at; the unit DIE (`first`) has a few more.
bool wanted_attr(std::uint64_t name, bool first) {
  switch (name) {
    case 0x03: case 0x11: case 0x12: case 0x55: case 0x3a: case 0x3b: case 0x31: case 0x47: case 0x3c: return true;
    case 0x1b: case 0x10: case 0x72: case 0x73: case 0x74: return first;
    default: return false;
  }
}

class Reader {
 public:
  explicit Reader(const Sections& s) : s_(s) {}

  std::vector<Function> run() {
    std::unordered_map<std::uint64_t, std::vector<Abbrev>> abbrev_cache;
    Cursor info(s_.info);
    while (!info.at_end()) {
      Unit u;
      u.offset = info.pos();
      std::uint64_t len = info.get<std::uint32_t>();
      if (len == 0xffffffff) {
        len = info.get<std::uint64_t>();
        u.offset_size = 8;
      }
      const std::uint64_t end = info.pos() + len;
      if (len > s_.info.size() || end > s_.info.size()) Cursor::fail();
      u.version = info.get<std::uint16_t>();
      std::uint64_t abbrev_off = 0;
      std::uint8_t unit_type = 1;
      if (u.version >= 5) {
        unit_type = info.get<std::uint8_t>();
        u.addr_size = info.get<std::uint8_t>();
        abbrev_off = info.uint(u.offset_size);
        if (unit_type == 2 || unit_type == 6) info.skip(8 + static_cast<std::uint64_t>(u.offset_size));  // type units
        else if (unit_type == 4 || unit_type == 5) info.skip(8);  // skeleton / split: DWO id
      } else if (u.version >= 2) {
        abbrev_off = info.uint(u.offset_size);
        u.addr_size = info.get<std::uint8_t>();
      } else {
        Cursor::fail();
      }
      if (u.addr_size != 8 && u.addr_size != 4) Cursor::fail();
      if (unit_type == 1 || unit_type == 3) {  // compile and partial units
        auto it = abbrev_cache.find(abbrev_off);
        if (it == abbrev_cache.end()) it = abbrev_cache.emplace(abbrev_off, abbrevs(abbrev_off)).first;
        units_.push_back(u);
        read_unit(info, end, it->second, units_.size() - 1);
      }
      info.seek(end);
    }
    return finish();
  }

 private:
  const Sections& s_;
  std::vector<Unit> units_;
  std::vector<Die> dies_;
  std::unordered_map<std::uint64_t, std::size_t> by_offset_;  // DIE offset -> index in dies_

  std::vector<Abbrev> abbrevs(std::uint64_t off) const {
    std::vector<Abbrev> out;
    Cursor c(s_.abbrev, off);
    while (true) {
      const std::uint64_t code = c.uleb();
      if (code == 0) break;
      if (code > 100000) Cursor::fail();
      if (out.size() <= code) out.resize(code + 1);
      Abbrev& a = out[code];
      a.tag = c.uleb();
      a.children = c.get<std::uint8_t>() != 0;
      while (true) {
        Attr at{c.uleb(), c.uleb(), 0};
        if (at.name == 0 && at.form == 0) break;
        if (at.form == 0x21) at.implicit = c.sleb();
        a.attrs.push_back(at);
      }
    }
    return out;
  }

  std::string str(const Value& v, const Unit& u) const {
    if (!v.is_string && !v.needs_str_index) return "";
    if (v.form == 0x08) return v.s;
    if (v.form == 0x1f) return Cursor(s_.line_str).cstr_at(v.u);
    if (v.needs_str_index) {
      Cursor c(s_.str_offsets, u.str_offsets_base + v.u * static_cast<std::uint64_t>(u.offset_size));
      return Cursor(s_.str).cstr_at(c.uint(u.offset_size));
    }
    return Cursor(s_.str).cstr_at(v.u);
  }

  std::uint64_t addr(const Value& v, const Unit& u) const {
    if (!v.needs_addr_index) return v.u;
    Cursor c(s_.addr, u.addr_base + v.u * u.addr_size);
    return c.uint(u.addr_size);
  }

  void read_unit(Cursor& c, std::uint64_t end, const std::vector<Abbrev>& abbrevs, std::size_t unit_index) {
    int depth = 0;
    bool first = true;
    while (c.pos() < end) {
      const std::uint64_t die_off = c.pos();
      const std::uint64_t code = c.uleb();
      if (code == 0) {  // end of a sibling list
        if (--depth < 0) break;
        continue;
      }
      if (code >= abbrevs.size() || abbrevs[code].tag == 0) Cursor::fail();
      const Abbrev& a = abbrevs[code];
      Unit& u = units_[unit_index];
      if (!first && a.tag != 0x2e) {  // not a subprogram: nothing in it is kept
        for (const Attr& at : a.attrs) skip_form(c, at.form, u);
        if (a.children) ++depth;
        continue;
      }
      Die d;
      d.offset = die_off;
      d.unit = unit_index;
      std::optional<Value> low_v, comp_dir_v;
      std::optional<std::uint64_t> high, ranges, stmt;
      bool high_is_offset = false;
      Value name_v, ranges_v;
      for (const Attr& at : a.attrs) {
        if (!wanted_attr(at.name, first)) {  // skip the rest (linkage names, types, frame bases, ...)
          skip_form(c, at.form, u);
          continue;
        }
        const Value v = read_form(c, at.form, u, at.implicit);
        switch (at.name) {
          case 0x03: name_v = v; break;                                              // name
          case 0x11: low_v = v; break;                                               // low_pc
          case 0x12: high = v.u; high_is_offset = at.form != 0x01 && !v.needs_addr_index; break;  // high_pc
          case 0x55: ranges = v.u; ranges_v = v; break;                              // ranges
          case 0x3a: d.decl_file = static_cast<std::int64_t>(v.u); break;            // decl_file
          case 0x3b: d.line = static_cast<std::uint32_t>(v.u); break;                // decl_line
          case 0x31: case 0x47: if (v.is_ref) d.origin = v.u; break;                 // abstract_origin, specification
          case 0x3c: d.declaration = v.u != 0; break;                                // declaration
          case 0x1b: if (first) comp_dir_v = v; break;                               // comp_dir
          case 0x10: if (first) stmt = v.u; break;                                   // stmt_list
          case 0x72: if (first) u.str_offsets_base = v.u; break;                     // str_offsets_base
          case 0x73: if (first) u.addr_base = v.u; break;                            // addr_base
          case 0x74: if (first) u.rnglists_base = v.u; break;                        // rnglists_base
          default: break;
        }
      }
      // Indexed forms (strx, addrx) need the unit's bases, which may come later in the unit DIE.
      std::optional<std::uint64_t> low;
      if (low_v) low = addr(*low_v, u);
      if (first) {
        // The unit DIE: its attributes (bases, comp_dir, base address) apply to what follows.
        if (comp_dir_v) u.comp_dir = str(*comp_dir_v, u);
        if (low) u.low_pc = *low;
            if (stmt) u.files = line_files(*stmt, u);
        first = false;
      } else if (a.tag == 0x2e) {  // DW_TAG_subprogram
        d.name = str(name_v, u);
        if (low && high) d.ranges.emplace_back(*low, high_is_offset ? *low + *high : *high);
        else if (ranges) d.ranges = range_list(ranges_v, u);
        by_offset_[die_off] = dies_.size();
        dies_.push_back(std::move(d));
      }
      if (a.children) ++depth;
    }
  }

  // Range lists: DWARF 5 .debug_rnglists (entries) or DWARF 4 .debug_ranges (address pairs).
  std::vector<std::pair<std::uint64_t, std::uint64_t>> range_list(const Value& v, const Unit& u) const {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
    std::uint64_t base = u.low_pc;
    if (u.version >= 5) {
      std::uint64_t off = v.u;
      if (v.needs_rnglist_index) {
        Cursor ix(s_.rnglists, u.rnglists_base + v.u * static_cast<std::uint64_t>(u.offset_size));
        off = u.rnglists_base + ix.uint(u.offset_size);
      }
      Cursor c(s_.rnglists, off);
      for (int guard = 0; guard < 100000; ++guard) {
        const auto kind = c.get<std::uint8_t>();
        auto ax = [&](std::uint64_t i) {
          Cursor a(s_.addr, u.addr_base + i * u.addr_size);
          return a.uint(u.addr_size);
        };
        switch (kind) {
          case 0: return out;                                                          // end_of_list
          case 1: base = ax(c.uleb()); break;                                          // base_addressx
          case 2: { auto s = ax(c.uleb()), e = ax(c.uleb()); out.emplace_back(s, e); break; }  // startx_endx
          case 3: { auto s = ax(c.uleb()); out.emplace_back(s, s + c.uleb()); break; }  // startx_length
          case 4: { auto s = c.uleb(), e = c.uleb(); out.emplace_back(base + s, base + e); break; }  // offset_pair
          case 5: base = c.uint(u.addr_size); break;                                   // base_address
          case 6: { auto s = c.uint(u.addr_size), e = c.uint(u.addr_size); out.emplace_back(s, e); break; }  // start_end
          case 7: { auto s = c.uint(u.addr_size); out.emplace_back(s, s + c.uleb()); break; }  // start_length
          default: Cursor::fail();
        }
      }
      return out;
    }
    Cursor c(s_.ranges, v.u);
    for (int guard = 0; guard < 100000; ++guard) {
      const std::uint64_t s = c.uint(u.addr_size), e = c.uint(u.addr_size);
      if (s == 0 && e == 0) break;
      if (s == (u.addr_size == 8 ? ~0ull : 0xffffffffull)) base = e;
      else out.emplace_back(base + s, base + e);
    }
    return out;
  }

  // The file names of a line table (full paths, with comp_dir and the directory applied).
  std::vector<std::string> line_files(std::uint64_t off, const Unit& u) const {
    namespace fs = std::filesystem;
    std::vector<std::string> files;
    Cursor c(s_.line, off);
    std::uint64_t len = c.get<std::uint32_t>();
    int osz = 4;
    if (len == 0xffffffff) {
      len = c.get<std::uint64_t>();
      osz = 8;
    }
    const auto version = c.get<std::uint16_t>();
    if (version >= 5) c.skip(2);  // address_size, segment_selector_size
    c.uint(osz);                  // header_length
    c.skip(version >= 4 ? 4 : 3);  // min_inst_length, [max_ops], default_is_stmt, line_base
    c.skip(1);                     // line_range
    const auto opcode_base = c.get<std::uint8_t>();
    c.skip(opcode_base ? opcode_base - 1u : 0u);
    auto join = [&](const std::string& dir, const std::string& name) {
      fs::path p(name);
      if (p.is_relative()) p = fs::path(dir) / p;
      if (p.is_relative() && !u.comp_dir.empty()) p = fs::path(u.comp_dir) / p;
      return p.lexically_normal().generic_string();
    };
    if (version >= 5) {
      auto entries = [&](std::vector<std::pair<std::string, std::uint64_t>>& out) {
        const auto nformats = c.get<std::uint8_t>();
        std::vector<std::pair<std::uint64_t, std::uint64_t>> formats;
        for (int i = 0; i < nformats; ++i) {
          const std::uint64_t type = c.uleb();  // two statements: argument order is unspecified
          const std::uint64_t form = c.uleb();
          formats.emplace_back(type, form);
        }
        const std::uint64_t count = c.uleb();
        if (count > 1000000) Cursor::fail();
        Unit lu = u;
        lu.offset_size = osz;
        for (std::uint64_t i = 0; i < count; ++i) {
          std::string path;
          std::uint64_t dir = 0;
          for (const auto& [type, form] : formats) {
            const Value v = read_form(c, form, lu, 0);
            if (type == 1) path = str(v, lu);  // DW_LNCT_path
            if (type == 2) dir = v.u;          // DW_LNCT_directory_index
          }
          out.emplace_back(path, dir);
        }
      };
      std::vector<std::pair<std::string, std::uint64_t>> dirs, names;
      entries(dirs);
      entries(names);
      for (const auto& [name, d] : names) files.push_back(join(d < dirs.size() ? dirs[d].first : "", name));
    } else {
      std::vector<std::string> dirs{u.comp_dir};
      for (std::string d = c.cstr(); !d.empty(); d = c.cstr()) dirs.push_back(d);
      files.emplace_back();  // DWARF 4 file numbers start at 1
      for (std::string name = c.cstr(); !name.empty(); name = c.cstr()) {
        const std::uint64_t d = c.uleb();
        c.uleb();  // mtime
        c.uleb();  // length
        files.push_back(join(d < dirs.size() ? dirs[d] : "", name));
      }
    }
    return files;
  }

  std::vector<Function> finish() const {
    std::vector<Function> out;
    for (const Die& d : dies_) {
      // Clones and out-of-line instances name their original: follow origins for the name,
      // file and line.
      std::string name = d.name;
      std::int64_t file = d.decl_file;
      std::size_t file_unit = d.unit;
      std::uint32_t line = d.line;
      std::uint64_t origin = d.origin;
      for (int hops = 0; hops < 8 && origin && (name.empty() || file < 0); ++hops) {
        auto it = by_offset_.find(origin);
        if (it == by_offset_.end()) break;
        const Die& o = dies_[it->second];
        if (name.empty()) name = o.name;
        if (file < 0) {
          file = o.decl_file;
          file_unit = o.unit;
          line = o.line;
        }
        origin = o.origin;
      }
      if (name.empty()) continue;
      const auto& files = units_[file_unit].files;
      const std::string path = file >= 0 && static_cast<std::size_t>(file) < files.size() ? files[file] : "";
      if (d.ranges.empty()) {
        // A definition without an address; prototypes (DW_AT_declaration) aren't definitions.
        if (!path.empty() && !d.origin && !d.declaration) out.push_back({0, name, path, line, false, false});
        continue;
      }
      bool first = true;
      for (const auto& [lo, hi] : d.ranges) {
        if (hi <= lo) continue;
        out.push_back({lo, name, path, line, first, true});
        first = false;
      }
    }
    return out;
  }
};

}  // namespace

std::vector<Function> functions(const elf::File& f) {
  auto sec = [&](const char* name) -> std::span<const std::uint8_t> {
    const elf::Section* s = f.section(name);
    if (!s) return {};
    if (s->flags & 0x800) throw IrError(f.path.string() + ": compressed debug info isn't supported yet");  // SHF_COMPRESSED
    return f.bytes(*s);
  };
  Sections s{sec(".debug_info"), sec(".debug_abbrev"), sec(".debug_str"), sec(".debug_line_str"),
             sec(".debug_line"),  sec(".debug_str_offsets"), sec(".debug_addr"), sec(".debug_rnglists"),
             sec(".debug_ranges")};
  if (s.info.empty() || s.abbrev.empty()) return {};
  try {
    return Reader(s).run();
  } catch (const IrError&) {
    throw IrError(f.path.string() + ": malformed DWARF debug info");
  }
}

}  // namespace optum::dwarf
