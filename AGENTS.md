# AGENTS.md — conventions for AI agents working in this repo

If you're an LLM (or an LLM-driven editor) about to make changes here, read
this first. This is **venice-cpp**, a header-only C++23 client for the Venice
API (BSD 3-clause). It is the foundation for terminal/desktop AI tooling
(TermForge TUI, KDE integration).

## Baseline (keep in sync if changed)

- **CMake ≥ 3.28**, **C++23** (GCC 13+ / Clang 17+).
- **Compiler respects the environment** by default; clang is an opt-in
  toolchain (`cmake/toolchain/clang.cmake`), like the sanitizer toolchains.
- **Catch2 v3** for tests (`FetchContent`).
- **Header-only library** (`src/lib/CMakeLists.txt` is an INTERFACE target).
  No compiled sources for the library itself.

## Dependencies

- Transport: **cpp-httplib** (header-only). JSON: **nlohmann/json**
  (header-only). TLS: **OpenSSL** (link-time; do NOT reimplement TLS).
- Managed via `find_package` first, `FetchContent` fallback, 100% CMake. No
  conan/vcpkg unless the maintainer asks.
- cpp-httplib API notes (v0.18.x): `Request` has `body` + `set_header()` for
  content type (no `set_content`, no `content_type_`); `send(req,res,err)`
  returns `bool`. These bit once — check the vendored header before assuming.

## Conventions that matter

- **Errors:** `std::expected<T, venice::Error>` everywhere fallible. Never throw
  across the public API; a transport/parse/HTTP failure is a value the caller
  inspects. Error kinds: network / http / parse / auth / rate_limited /
  invalid_arg, each carrying `status` + raw `body`.
- **`venice_parameters`:** only serialize set fields; keep `extra` as a
  forward-compatible passthrough so future Venice keys don't break the client.
- **Usage/cost metadata:** keep cache buckets distinct (cached vs uncached
  tokens price differently — see venice-cli #75).
- **KDE/Qt-readiness:** keep the library UI-free and Qt-linkable. No Qt types
  in the API client; a separate service layer owns D-Bus/KF concerns.

## Testing philosophy

**Test how code fails, not just that it produces the right output.** Write the
failure matrix first (bad input, boundaries, malformed JSON, error statuses);
the happy-path check is last. Unit tests are **offline** — no API key or
network. A live smoke check needs `$VENICE_API_KEY`.

## How to verify before a PR

```bash
cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
cmake -B build-clang -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/clang.cmake \
  && cmake --build build-clang && ctest --test-dir build-clang
```

Both compilers must build clean and pass. (This is how the fmt-under-clang-20
breakage was caught in cpp-template.)

## Attribution

Agent-authored commits carry a trailer naming the model, e.g.

```
Co-authored-by: Kimi K3 (vcoder via Venice) <noreply@venice.ai>
Agent: vcoder / Kimi K3
```

and PRs note what was actually run to verify.

## Notes for agents

- **Path caution:** the editing tools in some environments write relative to a
  session's original project root, not the shell's cwd. If you `cp`/`cd` into a
  new repo mid-session, confirm file writes land in the right tree (a
  venice-cpp change once leaked into cpp-template). Prefer shell writes
  (`run`) or verify paths after `write_file`/`edit_file`.
- `include/version.hpp.in.cmake` configures into `include/version.hpp`; edit
  the `.in.cmake` source, not the generated file.
- Build dirs (`build*/`) are gitignored — don't commit them.
