#include "optum/elf.hpp"

#include <cstring>
#include <fstream>

#include "optum/ir.hpp"

namespace optum::elf {
namespace {

// Bounds-checked little-endian reads over the whole file.
class Reader {
 public:
  Reader(const std::vector<std::uint8_t>& d, const std::filesystem::path& p) : d_(d), path_(p) {}

  template <typename T>
  T get(std::uint64_t off) const {
    if (off > d_.size() || sizeof(T) > d_.size() - off) fail("read past the end of the file");
    T v;
    std::memcpy(&v, d_.data() + off, sizeof(T));
    return v;  // x86-64 hosts are little-endian, as the files we accept
  }

  std::string cstr(std::uint64_t off) const {
    if (off >= d_.size()) fail("string offset out of range");
    const void* end = std::memchr(d_.data() + off, 0, d_.size() - off);
    if (!end) fail("unterminated string");
    return std::string(reinterpret_cast<const char*>(d_.data() + off),
                       static_cast<const std::uint8_t*>(end) - (d_.data() + off));
  }

  void check_range(std::uint64_t off, std::uint64_t size) const {
    if (off > d_.size() || size > d_.size() - off) fail("section extends past the end of the file");
  }

  [[noreturn]] void fail(const std::string& msg) const { throw IrError(path_.string() + ": " + msg); }

 private:
  const std::vector<std::uint8_t>& d_;
  const std::filesystem::path& path_;
};

// DWARF pointer encodings used by .eh_frame (DW_EH_PE_*).
std::uint64_t read_encoded(const Reader& r, std::uint64_t& off, std::uint8_t enc, std::uint64_t field_addr) {
  if (enc == 0xff) return 0;  // omit
  std::uint64_t v = 0;
  const std::uint64_t at = off;
  switch (enc & 0x0f) {
    case 0x00: v = r.get<std::uint64_t>(off); off += 8; break;                       // absptr
    case 0x02: v = r.get<std::uint16_t>(off); off += 2; break;                       // udata2
    case 0x03: v = r.get<std::uint32_t>(off); off += 4; break;                       // udata4
    case 0x04: v = r.get<std::uint64_t>(off); off += 8; break;                       // udata8
    case 0x0a: v = static_cast<std::uint64_t>(r.get<std::int16_t>(off)); off += 2; break;  // sdata2
    case 0x0b: v = static_cast<std::uint64_t>(r.get<std::int32_t>(off)); off += 4; break;  // sdata4
    case 0x0c: v = r.get<std::uint64_t>(off); off += 8; break;                       // sdata8
    default: r.fail("unsupported .eh_frame pointer encoding");
  }
  (void)at;
  if ((enc & 0x70) == 0x10) v += field_addr;  // pcrel: relative to the field itself
  return v;
}

std::uint64_t uleb(const Reader& r, std::uint64_t& off) {
  std::uint64_t v = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    const auto b = r.get<std::uint8_t>(off++);
    v |= std::uint64_t(b & 0x7f) << shift;
    if (!(b & 0x80)) return v;
  }
  r.fail("LEB128 value too long");
}

void parse_eh_frame(const Reader& r, const Section& s, std::vector<Fde>& out) {
  std::unordered_map<std::uint64_t, std::uint8_t> cie_encoding;  // CIE offset -> FDE pointer encoding
  std::uint64_t off = s.offset;
  const std::uint64_t end = s.offset + s.size;
  while (off + 4 <= end) {
    const std::uint64_t entry = off;
    std::uint64_t len = r.get<std::uint32_t>(off);
    off += 4;
    if (len == 0) break;  // terminator
    if (len == 0xffffffff) {
      len = r.get<std::uint64_t>(off);
      off += 8;
    }
    const std::uint64_t body = off;
    if (len > end - body) r.fail(".eh_frame entry past the end of the section");
    const std::uint64_t next = body + len;
    const auto id = r.get<std::uint32_t>(off);
    off += 4;
    if (id == 0) {  // a CIE: read its augmentation for the FDE pointer encoding
      off += 1;     // version
      const std::string aug = r.cstr(off);
      off += aug.size() + 1;
      uleb(r, off);  // code alignment
      uleb(r, off);  // data alignment (sleb, but only skipped)
      uleb(r, off);  // return address register
      std::uint8_t enc = 0;
      if (!aug.empty() && aug[0] == 'z') {
        uleb(r, off);  // augmentation data length
        for (char c : aug.substr(1)) {
          if (c == 'R') enc = r.get<std::uint8_t>(off++);
          else if (c == 'L') off += 1;
          else if (c == 'P') {
            const auto penc = r.get<std::uint8_t>(off++);
            read_encoded(r, off, penc & 0x7f, s.addr + (off - s.offset));
          } else if (c != 'S' && c != 'B') break;
        }
      }
      cie_encoding[entry] = enc;
    } else {  // an FDE: its CIE is `id` bytes back from the id field
      const std::uint64_t cie = body - id;
      const auto it = cie_encoding.find(cie);
      const std::uint8_t enc = it == cie_encoding.end() ? 0x1b : it->second;
      const std::uint64_t start = read_encoded(r, off, enc, s.addr + (off - s.offset));
      const std::uint64_t size = read_encoded(r, off, enc & 0x0f, 0);
      if (size > 0) out.push_back({start, size});
    }
    off = next;
  }
}

}  // namespace

const Section* File::section(std::string_view name) const {
  for (const auto& s : sections) {
    if (s.name == name) return &s;
  }
  return nullptr;
}

const Section* File::section_at(std::uint64_t addr) const {
  for (const auto& s : sections) {
    if (s.addr && addr >= s.addr && addr - s.addr < s.size && (s.flags & 0x2)) return &s;  // SHF_ALLOC
  }
  return nullptr;
}

std::span<const std::uint8_t> File::bytes(const Section& s) const {
  if (s.type == 8 || s.offset > data.size() || s.size > data.size() - s.offset) return {};  // SHT_NOBITS
  return {data.data() + s.offset, static_cast<std::size_t>(s.size)};
}

std::optional<std::uint64_t> File::pointer_at(std::uint64_t addr) const {
  if (auto it = relative_at.find(addr); it != relative_at.end()) return it->second;
  const Section* s = section_at(addr);
  if (!s || s->executable() || s->type == 8) return std::nullopt;
  const auto b = bytes(*s);
  const std::uint64_t off = addr - s->addr;
  if (off + 8 > b.size()) return std::nullopt;
  std::uint64_t v;
  std::memcpy(&v, b.data() + off, 8);
  return v;
}

std::optional<std::string> File::string_at(std::uint64_t addr, std::size_t max) const {
  const Section* s = section_at(addr);
  if (!s || s->executable() || s->type == 8) return std::nullopt;
  const auto b = bytes(*s);
  std::string out;
  for (std::uint64_t off = addr - s->addr; off < b.size() && out.size() < max; ++off) {
    const auto c = b[off];
    if (c == 0) return out;
    if (c < 0x09 || (c > 0x0d && c < 0x20)) return std::nullopt;  // control bytes: not text
    out.push_back(static_cast<char>(c));
  }
  return std::nullopt;  // unterminated, or longer than max
}

bool is_elf(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  char m[4] = {};
  return in.read(m, 4) && m[0] == 0x7f && m[1] == 'E' && m[2] == 'L' && m[3] == 'F';
}

File read(const std::filesystem::path& p) {
  File f;
  f.path = p;
  {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw IrError(p.string() + ": cannot open");
    // One sized read: copying through an istreambuf_iterator is a byte-at-a-time loop, which took
    // about a minute on a 150 MB sanitizer-built binary.
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    in.seekg(0);
    if (size > 0) {
      f.data.resize(static_cast<std::size_t>(size));
      in.read(reinterpret_cast<char*>(f.data.data()), size);
      f.data.resize(static_cast<std::size_t>(in.gcount()));
    }
  }
  const Reader r(f.data, p);
  if (f.data.size() < 64 || r.get<std::uint32_t>(0) != 0x464c457f) r.fail("not an ELF file");
  if (f.data[4] != 2) r.fail("not a 64-bit ELF file");
  if (f.data[5] != 1) r.fail("not a little-endian ELF file");
  f.type = r.get<std::uint16_t>(16);
  f.machine = r.get<std::uint16_t>(18);
  f.entry = r.get<std::uint64_t>(24);
  const auto shoff = r.get<std::uint64_t>(40);
  const auto shentsize = r.get<std::uint16_t>(58);
  const auto shnum = r.get<std::uint16_t>(60);
  const auto shstrndx = r.get<std::uint16_t>(62);
  const auto phoff = r.get<std::uint64_t>(32);
  const auto phentsize = r.get<std::uint16_t>(54);
  const auto phnum = r.get<std::uint16_t>(56);
  for (std::uint16_t i = 0; i < phnum && phentsize >= 56; ++i) {
    if (r.get<std::uint32_t>(phoff + std::uint64_t(i) * phentsize) == 3) f.has_interpreter = true;  // PT_INTERP
  }
  if (shnum == 0) return f;  // no section headers: nothing more to read
  if (shentsize < 64) r.fail("bad section header size");

  for (std::uint16_t i = 0; i < shnum; ++i) {
    const std::uint64_t h = shoff + std::uint64_t(i) * shentsize;
    Section s;
    s.type = r.get<std::uint32_t>(h + 4);
    s.flags = r.get<std::uint64_t>(h + 8);
    s.addr = r.get<std::uint64_t>(h + 16);
    s.offset = r.get<std::uint64_t>(h + 24);
    s.size = r.get<std::uint64_t>(h + 32);
    s.link = r.get<std::uint32_t>(h + 40);
    s.info = r.get<std::uint32_t>(h + 44);
    s.entsize = r.get<std::uint64_t>(h + 56);
    s.name = std::to_string(r.get<std::uint32_t>(h));  // resolved below
    f.sections.push_back(std::move(s));
  }
  if (shstrndx >= f.sections.size()) r.fail("bad section name table index");
  const Section& names = f.sections[shstrndx];
  r.check_range(names.offset, names.size);
  for (auto& s : f.sections) s.name = r.cstr(names.offset + std::stoull(s.name));
  for (const auto& s : f.sections) {
    if (s.type != 8) r.check_range(s.offset, s.size);  // everything but SHT_NOBITS lies in the file
  }

  auto strtab_of = [&](const Section& s) -> const Section& {
    if (s.link >= f.sections.size()) r.fail("bad section link");
    return f.sections[s.link];
  };
  auto read_symbols = [&](const Section& s, std::vector<Symbol>& out) {
    const Section& str = strtab_of(s);
    for (std::uint64_t off = s.offset; off + 24 <= s.offset + s.size; off += 24) {
      Symbol sym;
      sym.name = r.cstr(str.offset + r.get<std::uint32_t>(off));
      const auto info = r.get<std::uint8_t>(off + 4);
      sym.type = info & 0xf;
      sym.bind = info >> 4;
      sym.shndx = r.get<std::uint16_t>(off + 6);
      sym.value = r.get<std::uint64_t>(off + 8);
      sym.size = r.get<std::uint64_t>(off + 16);
      out.push_back(std::move(sym));
    }
  };
  for (const auto& s : f.sections) {
    if (s.type == 2) read_symbols(s, f.symtab);   // SHT_SYMTAB
    if (s.type == 11) read_symbols(s, f.dynsym);  // SHT_DYNSYM
  }

  // Symbol versions: which library each imported symbol comes from.
  const Section* versym = nullptr;
  std::unordered_map<std::uint16_t, std::string> version_file;
  for (const auto& s : f.sections) {
    if (s.type == 0x6fffffff) versym = &s;  // SHT_GNU_versym
    if (s.type == 0x6ffffffe) {             // SHT_GNU_verneed
      const Section& str = strtab_of(s);
      std::uint64_t vn = s.offset;
      for (std::uint32_t i = 0; i < s.info && vn < s.offset + s.size; ++i) {
        const std::string file = r.cstr(str.offset + r.get<std::uint32_t>(vn + 4));
        const auto cnt = r.get<std::uint16_t>(vn + 2);
        std::uint64_t aux = vn + r.get<std::uint32_t>(vn + 8);
        for (std::uint16_t k = 0; k < cnt; ++k) {
          version_file[r.get<std::uint16_t>(aux + 6) & 0x7fff] = file;
          const auto next = r.get<std::uint32_t>(aux + 12);
          if (!next) break;
          aux += next;
        }
        const auto next = r.get<std::uint32_t>(vn + 12);
        if (!next) break;
        vn += next;
      }
    }
  }
  if (versym) {
    for (std::size_t i = 0; i < f.dynsym.size() && 2 * i + 2 <= versym->size; ++i) {
      const auto v = static_cast<std::uint16_t>(r.get<std::uint16_t>(versym->offset + 2 * i) & 0x7fff);
      if (auto it = version_file.find(v); it != version_file.end()) f.dynsym[i].library = it->second;
    }
  }

  for (const auto& s : f.sections) {
    if (s.type == 6) {  // SHT_DYNAMIC
      const Section& str = strtab_of(s);
      for (std::uint64_t off = s.offset; off + 16 <= s.offset + s.size; off += 16) {
        const auto tag = r.get<std::int64_t>(off);
        const auto val = r.get<std::uint64_t>(off + 8);
        if (tag == 0) break;
        if (tag == 1) f.needed.push_back(r.cstr(str.offset + val));
        if (tag == 14) f.soname = r.cstr(str.offset + val);
        if (tag == 29 || tag == 15) {  // DT_RUNPATH, DT_RPATH: ':'-separated
          std::string list = r.cstr(str.offset + val);
          for (std::size_t a = 0, b; a <= list.size(); a = b + 1) {
            b = list.find(':', a);
            if (b == std::string::npos) b = list.size();
            if (b > a) f.runpath.push_back(list.substr(a, b - a));
          }
        }
      }
    }
    if (s.type == 4) {  // SHT_RELA
      // PIE binaries carry a RELATIVE entry per stored pointer (1.6 million in a sanitizer build):
      // size the containers once instead of rehashing as they grow.
      std::size_t relatives = 0;
      for (std::uint64_t off = s.offset; off + 24 <= s.offset + s.size; off += 24) {
        if (static_cast<std::uint32_t>(r.get<std::uint64_t>(off + 8) & 0xffffffff) == 8) ++relatives;
      }
      f.relative_targets.reserve(f.relative_targets.size() + relatives);
      f.relative_at.reserve(f.relative_at.size() + relatives);
      for (std::uint64_t off = s.offset; off + 24 <= s.offset + s.size; off += 24) {
        const auto where = r.get<std::uint64_t>(off);
        const auto info = r.get<std::uint64_t>(off + 8);
        const auto addend = r.get<std::int64_t>(off + 16);
        const auto type = static_cast<std::uint32_t>(info & 0xffffffff);
        const auto sym = static_cast<std::size_t>(info >> 32);
        if ((type == 7 || type == 6) && sym < f.dynsym.size()) {  // JUMP_SLOT, GLOB_DAT
          f.got_imports[where] = f.dynsym[sym].name;
        } else if (type == 8) {  // RELATIVE: a pointer stored in data
          f.relative_targets.push_back(static_cast<std::uint64_t>(addend));
          f.relative_at[where] = static_cast<std::uint64_t>(addend);
        }
      }
    }
  }

  auto pointers = [&](const char* name, std::vector<std::uint64_t>& out) {
    const Section* s = f.section(name);
    if (!s || s->type == 8) return;
    for (std::uint64_t off = 0; off + 8 <= s->size; off += 8) {
      const auto v = r.get<std::uint64_t>(s->offset + off);
      if (v) out.push_back(v);
    }
  };
  pointers(".init_array", f.init_array);
  pointers(".fini_array", f.fini_array);

  if (const Section* eh = f.section(".eh_frame"); eh && eh->type != 8) parse_eh_frame(r, *eh, f.fdes);
  return f;
}

}  // namespace optum::elf
