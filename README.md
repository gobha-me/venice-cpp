# venice-cpp

[![CI](https://github.com/gobha-me/venice-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/gobha-me/venice-cpp/actions/workflows/ci.yml)

A header-only **C++23 client for the [Venice.ai](https://venice.ai) API** — BSD
3-clause licensed. Talks to Venice's OpenAI-compatible endpoints plus the
`venice_parameters` extension block, over HTTPS, with `std::expected` error
handling (no exceptions across the public API).

Built as the foundation for terminal/desktop AI tooling (TermForge TUI, KDE
integration) where a native C++ client — not a Python subprocess — is the
right bridge.

## Features (Phase 0)

- **Chat completions** (`/chat/completions`) — non-streaming and streaming
  (SSE via callback).
- **`venice_parameters`** extension — web search, citations, character,
  thinking toggles, with forward-compatible passthrough for unmodeled keys.
- **Models list** (`/models`).
- **Balance / rate limits** (`/api_keys/rate_limits`).
- **Error model** — `std::expected<T, venice::Error>`; network / HTTP / parse /
  auth / rate-limit / invalid-arg, each carrying status + raw body.

Later phases (fed by real use): image/audio/video, TTS, embeddings,
characters, retries/backoff, async.

## Dependencies

Header-only library; consumers link the CMake target and get everything
transitively:

- [cpp-httplib](https://github.com/yhirose/cpp-httplib) — HTTP transport (header-only)
- [nlohmann/json](https://github.com/nlohmann/json) — JSON (header-only)
- **OpenSSL** — TLS (the one link-time dependency; reimplementing TLS is
  malpractice, so we link it)

## Usage

```cpp
#include <venice/venice.hpp>

venice::Client client{std::getenv("VENICE_API_KEY")};

venice::ChatRequest req;
req.model = "llama-3.3-70b";
req.messages = {venice::Message::user("Hello")};

// non-streaming
if (auto res = client.chat(req)) {
  // res->content, res->usage, res->finish_reason
} else {
  // res.error().kind, .status, .message, .body
}

// streaming (SSE): on_token returns false to cancel early
auto s = client.chat_stream(req, [](std::string_view delta) {
  // print/handle delta
  return true;
});
```

### CMake

```cmake
# after add_subdirectory / FetchContent of venice-cpp:
target_link_libraries(your_target PRIVATE venice-cpp::lib)
```

The `Client` constructor takes the API key explicitly — the library reads no
environment variables of its own, so the caller decides where the key comes from.
The smoke binary in `src/bin` is what reads `$VENICE_API_KEY`. A custom endpoint
is a `base_url` argument to the same constructor.

### Add a dependency

Dependencies are opt-in by name, not by what sits in the directory:

```cmake
# 1. add cmake/deps/<name>.cmake  (find_package first, FetchContent fallback)
# 2. add <name> to ${PROJECT_NAME}_DEPS in the root CMakeLists.txt
```

A recipe that is not listed is never included; a listed name with no recipe is a
hard configure error.

## Build & test

```bash
cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
# cross-compiler (clang opt-in toolchain):
cmake -B build-clang -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/clang.cmake \
  && cmake --build build-clang && ctest --test-dir build-clang --output-on-failure
```

Unit tests are offline (no key/network needed): serialization round-trips,
response parsing incl. malformed input, error-model status mapping. A smoke
binary (`src/bin`, target `venice-cpp`) makes a live call when `$VENICE_API_KEY`
is set — it is never run by `ctest`.

Sanitizer builds use opt-in toolchain files (`address`, `thread`, `undefined`);
`test/30sanitizer-smoke` proves the selected one is actually engaged.

## Continuous integration

Every push and pull request builds and tests on GCC **and** Clang across the
default and all three sanitizer toolchains — nine jobs, including a standalone
run of the version-parser self-test. The Clang jobs pin Clang 20: Ubuntu's stock
Clang 18 cannot compile C++23 `std::expected` against libstdc++, and this
library returns `std::expected` from every fallible entry point.

## Releases

The version is derived from the nearest git tag at **configure** time, so
releasing is `git tag` — no version-bump commit:

```bash
git tag -a v0.1.0 -m "v0.1.0"
cmake -B build          # re-configure: banner reads venice-cpp:0.1.0 (tweak=0 dirty=0)
```

Tags must be `MAJOR.MINOR.PATCH` with an optional `v`/`r` prefix. Anything else —
two components, four, a `-rc1` suffix — is rejected by design and falls back to
`0.0.0` with a status line explaining why. Commits past the tag land in `tweak`
and a dirty worktree sets `dirty`, both exposed in the generated
`include/version.hpp`.

## Status

Phase 0 verified against the live API: chat (non-streaming + streaming),
models list (104 models), token usage. See `AGENTS.md` for contributor/agent
conventions and the testing philosophy (test how it fails, not just the happy
path).
