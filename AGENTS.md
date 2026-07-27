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
- **Toolchains are opt-in files**, not flags: `default`, `clang`, `address`,
  `thread`, `undefined` under `cmake/toolchain/`. You cannot pass two, so
  compose a sanitizer with clang via `CXX=clang++` rather than stacking files.

## Dependencies

- Transport: **cpp-httplib** (header-only). JSON: **nlohmann/json**
  (header-only). TLS: **OpenSSL** (link-time; do NOT reimplement TLS).
- Managed via `find_package` first, `FetchContent` fallback, 100% CMake. No
  conan/vcpkg unless the maintainer asks.
- **Deps are opt-in via a list, not the filesystem.** A recipe in `cmake/deps/`
  is fetched only if its stem is named in `${PROJECT_NAME}_DEPS` in the root
  `CMakeLists.txt`. Dropping a file into `cmake/deps/` does **not** activate it,
  and naming a dep with no recipe is a hard configure error. Adding a dep means
  editing both. This repo previously glob-included everything in that directory
  and pulled two libraries nothing linked (VC-01, #2); `artifact-check` rule B2
  now fails the build if that recurs.
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

`test/30sanitizer-smoke/` is not a unit test — it deliberately trips the active
sanitizer to prove the toolchain is engaged rather than a silent no-op, which is
what this repo shipped until the toolchains were fixed. Two couplings the
`artifact-check` rules enforce, so a half-rename fails the build instead of
quietly disabling the proof:

- the `VENICE_UBSAN` define must match in **both** `cmake/toolchain/undefined.cmake`
  and `test/30sanitizer-smoke/test.cpp` (rule B3);
- renaming the directory means updating the `if (TARGET 30sanitizer-smoke-test)`
  guard in `test/CMakeLists.txt` (rule B4).

ASan and TSan abort on their injected bug, so those cases are scored by
`PASS_REGULAR_EXPRESSION` on the sanitizer's own diagnostic, not by exit code.

## How to verify before a PR

```bash
# both compilers must build clean and pass — this is the gate
cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
cmake -B build-clang -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/clang.cmake \
  && cmake --build build-clang && ctest --test-dir build-clang --output-on-failure

# dependency-free self-tests (no compiler, no network); both also run in ctest
cmake -P cmake/version_selftest.cmake     # git-describe parser
cmake -P cmake/check_artifacts.cmake      # leftover artifacts + wiring drift

# touching a sanitizer toolchain? prove it still engages
cmake -B build-asan -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/address.cmake \
  && cmake --build build-asan \
  && ctest --test-dir build-asan -R 30sanitizer-smoke-test -V | grep AddressSanitizer
```

CI (`.github/workflows/ci.yml`) enforces the dual-compiler rule on every push and
pull request: GCC × Clang across {default, address, thread, undefined}, plus the
version self-test standalone — nine jobs. A one-compiler change turns that
compiler's four jobs red. Run the commands above locally first.

CI pins its Clang jobs to Clang 20. Ubuntu 24.04's stock Clang 18 cannot compile
C++23 `std::expected` against libstdc++, and `std::expected` is in this library's
*public* header — so stock Clang 18 fails everything here, not one test. If you
develop with Clang, verify against a version CI would accept rather than whatever
`clang++` happens to resolve to.

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
  the `.in.cmake` source, not the generated file. Keep the `#include <cstdint>`
  (`std::uint32_t` needs it).
- **Version parsing** is pure string logic in `cmake/version_parse.cmake`
  (`parse_git_describe`); `cmake/version.cmake` only runs `git describe` and
  calls it. Change the parsing and you add a row to the self-test and re-run it
  (`cmake -P cmake/version_selftest.cmake`, also in ctest as
  `version-parse-selftest`). Failure-matrix-first, like the other tests.
  Release tags must be `[rv]?MAJOR.MINOR.PATCH` — anything else (two components,
  four, `-rc1` suffixes) is rejected by design and falls back to `0.0.0` with a
  STATUS line saying why. The version is read at **configure** time, so a tag
  cut after configuring does not show up until you re-run `cmake -B`.
- **`cmake/check_artifacts.cmake`** runs in enforce mode here (ctest:
  `artifact-check`) — every rule must report zero hits. Class A catches leftover
  template artifacts; Class B catches wiring drift that stays relevant for the
  life of the project: every listed dep has a recipe (B1), no dep is fetched but
  unused (B2), the UBSan define matches on both sides (B3), target-guarded test
  dirs exist (B4), fixture scripts keep mode 100755 (B5). Run it directly with
  `cmake -P`. If a rule fires on something legitimate, fix the citation rather
  than loosening the rule — a rule that matches nothing passes everything.
- Build dirs (`build*/`) are gitignored — don't commit them.
