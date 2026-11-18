# Building the staged GammaThink components

Run from this directory. Required tools are CMake 3.24 or newer, Ninja,
a C++20 compiler, pkg-config and Threads. External development packages are
`gmpxx`, `libcrypto`, `libzim`, `libxml-2.0`, `libcurl`, `json-c`, `sqlite3`
and `libzstd`. Supply their pkg-config directories through `PKG_CONFIG_PATH`
if they are not already discoverable. Nothing is downloaded or installed.

```sh
cmake -S . -B build -G Ninja
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

On this macOS host, expose Homebrew ICU with `PKG_CONFIG_PATH=/opt/homebrew/opt/icu4c/lib/pkgconfig` before configuration. Other hosts should use their installed package paths.

The default configuration has `GAMMATHINK_WITH_MODEL_RUNTIME=OFF`.
It builds `axiom`, `gammathink`, `knowledge-inspect`, `knowledge-catalogue`,
`knowledge-contribute`, `knowledge-maintain` and `knowledge-task-plan`.
`axiom` uses only `apps/reasoning_core_main.cpp`, never the legacy `apps/main.cpp`.
All targets use C++20 and `-Wall -Wextra -Wpedantic -Werror`; imported
pkg-config dependency headers are SYSTEM includes. `compile_commands.json`
is generated in the CMake binary directory for clangd.

## Library boundaries

`gammathink_core` is an independent STATIC library containing only the seven
reasoning-core sources. Its sole external link dependencies are GMP/gmpxx
and crypto; its public project include directory is its own component.
Build it independently with:

```sh
cmake --build build --target gammathink_core --parallel 4
```

Other `gammathink_<component_name>` targets are OBJECT libraries, with hyphens
replaced by underscores. Promoted codec and native-goal contracts have their
own OBJECT targets. Each source has exactly one compilation owner.
`gammathink_components` collects these objects into a STATIC composition
library and links the clean core, production dependencies and Threads.
Cross-component references are resolved at this composition boundary, not
through synthetic dependency implementations. The non-core include surface
is shared; this build does not claim those components are independently
linkable archives.

Model-manager, model-backed execution/session operations and maintenance
scheduling are excluded when the runtime is OFF. Formal interpreter-interface
parsers remain available without a neural runtime.

## Tests

CTest registers explicitly selected production C++ tests that are present
under `tests/`, preserving their normal hyphenated `build/*-test` names.
Core tests link only `gammathink_core`; integration tests link the composition
library. The registry test receives the standalone 32-unit fixture under `tests/fixtures/`; no evaluation messages are copied. The grammar test requires llama.cpp internals and is registered only with the optional runtime. The four supplied fixture programs are built but not run as tests.
Working directories are the source root. Executables are always placed in
source-root `build/`, including with a different CMake binary directory, for
compatibility with the existing CLI fixtures.

Bun CLI and viewer tests are registered when Bun and their staged dependencies
are available. The two quantified-source CLI suites use the production
`source-quantified-test` fixture and `gammathink` command. Both are registered
and retain their snapshot/restart, missing-goal and resource-limit assertions.
Session CLI tests still require a non-staged
`source-profile-cli-fixture-v2` and are not registered. The supplied
`context_documents_cli.test.mjs` requires the optional session application.
No training-policy, inference, probe or benchmark tests are selected.
`BUILD_TESTING=OFF` disables tests and fixtures.

## Optional external model runtime

Use an existing, compatible llama.cpp source tree and already-built libraries:

```sh
cmake -S . -B build-model -G Ninja \
  -DGAMMATHINK_WITH_MODEL_RUNTIME=ON \
  -DGAMMATHINK_LLAMA_ROOT=/absolute/path/to/llama.cpp \
  -DGAMMATHINK_LLAMA_RUNTIME_DIR=/absolute/path/to/runtime/bin
cmake --build build-model --parallel 4
```

Both paths are mandatory and absolute. Headers are read from `include`,
`ggml/include`, `common` and `vendor`; the tree must expose `llama.h`,
`gguf.h`, `jinja/parser.h` and `json.h`. Only the explicitly supplied runtime
directory is searched for `llama-common`, `llama` and `ggml-base`.
The directory is included in the build RPATH. This configuration adds
`gammathink-session`, `execution-helper` and the optional production tests.
It neither builds llama.cpp nor accesses weights or runs inference during
configuration or compilation. Runtime-enabled tests are not part of the
default model-free acceptance gate.

## Core-only configuration

```sh
cmake -S . -B build-core -G Ninja -DGAMMATHINK_CORE_ONLY=ON
cmake --build build-core --parallel 4
ctest --test-dir build-core --output-on-failure
```

This configuration discovers only GMP and crypto, not ZIM, HTTP, SQLite or a neural runtime. The seven-source core and formal CLI remain unchanged when composed into the full application. Existing fixture compatibility keeps executables under root `build/`; do not run multiple configurations concurrently.
