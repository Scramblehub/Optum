#pragma once
// Developer aid: the tree-sitter syntax tree of a source file, as an S-expression.
// Used when writing or debugging a built-in frontend (`optum syntax-tree <file>`).

#include <filesystem>
#include <string>

namespace optum {

// Grammar chosen by extension: .py, .js/.jsx/.mjs/.cjs, .ts, .tsx, .c/.h. Throws IrError otherwise.
std::string syntax_tree(const std::filesystem::path& file);

}  // namespace optum
