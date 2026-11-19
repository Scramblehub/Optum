#include "optum/syntax_tree.hpp"

#include <cstdlib>

#include "frontends/common.hpp"
#include "optum/ir.hpp"
#include "ts_util.hpp"

extern "C" const TSLanguage* tree_sitter_python();
extern "C" const TSLanguage* tree_sitter_javascript();
extern "C" const TSLanguage* tree_sitter_typescript();
extern "C" const TSLanguage* tree_sitter_tsx();

namespace optum {

std::string syntax_tree(const std::filesystem::path& file) {
  const std::string ext = file.extension().string();
  const TSLanguage* lang = nullptr;
  if (ext == ".py") lang = tree_sitter_python();
  else if (ext == ".js" || ext == ".jsx" || ext == ".mjs" || ext == ".cjs") lang = tree_sitter_javascript();
  else if (ext == ".ts" || ext == ".mts" || ext == ".cts") lang = tree_sitter_typescript();
  else if (ext == ".tsx") lang = tree_sitter_tsx();
  else throw IrError("no grammar for '" + ext + "' files");
  const std::string src = frontends::read_file(file);
  ts::ParserPtr parser(ts_parser_new());
  ts_parser_set_language(parser.get(), lang);
  ts::TreePtr tree(ts_parser_parse_string(parser.get(), nullptr, src.data(), static_cast<uint32_t>(src.size())));
  char* s = ts_node_string(ts_tree_root_node(tree.get()));
  std::string out = s;
  std::free(s);
  return out + "\n";
}

}  // namespace optum
