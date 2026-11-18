# External libraries

Third-party code, pinned as shallow git submodules so every build uses the same versions.
After cloning: `git submodule update --init --depth 1`.

| Library | Version | License | Used for |
|---|---|---|---|
| [nlohmann/json](https://github.com/nlohmann/json) | v3.11.3 | MIT | Reading and writing the JSON graph format |
| [CLI11](https://github.com/CLIUtils/CLI11) | v2.4.2 | BSD-3-Clause | The `optum` command line |
| [Catch2](https://github.com/catchorg/Catch2) | v3.7.1 | BSL-1.0 | Tests |

Planned: tree-sitter and its grammars (`feat/frontend-python`), capstone (`exp/frontend-binary`).

To upgrade one: `git -C External_Libraries/<name> fetch --depth 1 origin tag <tag> && git -C External_Libraries/<name> checkout <tag>`, then update this table and commit.
