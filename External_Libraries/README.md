# External libraries

Third-party code, pinned as shallow git submodules so every build uses the same versions.
After cloning: `git submodule update --init --depth 1`.

| Library | Version | License | Used for |
|---|---|---|---|
| [nlohmann/json](https://github.com/nlohmann/json) | v3.11.3 | MIT | Reading and writing the JSON graph format |
| [CLI11](https://github.com/CLIUtils/CLI11) | v2.4.2 | BSD-3-Clause | The `optum` command line |
| [Catch2](https://github.com/catchorg/Catch2) | v3.7.1 | BSL-1.0 | Tests |
| [tree-sitter](https://github.com/tree-sitter/tree-sitter) | v0.27.2 | MIT | Parsing runtime (ABI 13–15) |
| [tree-sitter-python](https://github.com/tree-sitter/tree-sitter-python) | v0.25.0 | MIT | Python grammar (ABI 15) |
| [tree-sitter-javascript](https://github.com/tree-sitter/tree-sitter-javascript) | v0.25.0 | MIT | JavaScript + JSX grammar (ABI 15) |
| [tree-sitter-typescript](https://github.com/tree-sitter/tree-sitter-typescript) | v0.23.2 | MIT | TypeScript and TSX grammars (ABI 14) |
| [tree-sitter-c](https://github.com/tree-sitter/tree-sitter-c) | v0.24.2 | MIT | C grammar (ABI 15) |

Planned: a SQL grammar, capstone (`exp/frontend-binary`).
A grammar's ABI version must be one the runtime supports; check `LANGUAGE_VERSION` in its `src/parser.c`.

To upgrade one: `git -C External_Libraries/<name> fetch --depth 1 origin tag <tag> && git -C External_Libraries/<name> checkout <tag>`, then update this table and commit.
