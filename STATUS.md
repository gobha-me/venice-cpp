# venice-cpp — status (for the next session)

A session-local snapshot of where the project is and what's next. Supplements
AGENTS.md (which holds standing conventions, not state).

## Where we are

**Phase 0: DONE and verified against the live API.**
- `chat()` non-streaming completion (verified: "venice-cpp works").
- `chat_stream()` SSE streaming via callback, cancellable (verified live).
- `models()` list (104 models fetched), `balance()` rate-limit endpoint.
- `venice_parameters` extension with forward-compatible `extra` passthrough.
- Error model: `std::expected<T, Error>`, kinds network/http/parse/auth/
  rate_limited/invalid_arg, each carrying status + raw body.
- Header-only INTERFACE lib; cpp-httplib + nlohmann/json (header-only) +
  OpenSSL (link-time). KDE/Qt-ready shape (UI-free, Qt-linkable).

**Build system synced with cpp-template, tagged v0.1.0 — the first release.**
The repo was scaffolded before upstream's fix rounds landed, so it was running a
stale build system. Ported: declarative opt-in deps (closes #2 — fmt and argparse
were being FetchContent-pulled with nothing linking them), hardened git-describe
version parsing with a self-test, working sanitizer toolchains (ASan/TSan were
silent no-ops — `find_library` sets `ASAN`, never `ASAN_FOUND`) plus UBSan and a
smoke test proving engagement, the leftover-artifact/wiring-drift check, test
discovery fixes, `.clangd` off its C++20 pin, narrowed `.gitignore`, and CI
enforcing the dual-compiler rule across nine jobs.

Two defects were found *in* the ported artifact checker and fixed here: rule B3's
unanchored token regex gave a false pass on suffix-appended renames, and rule B2
was blinded by any comment mentioning a package name. Both are upstream bugs and
should go back to cpp-template.

## Next up
1. **AIForge chat-TUI MVP** — see issue #1. Composes venice-cpp + termforge.
   This is the agreed next move; both foundations are proven.
2. Thicken endpoints as AIForge/KDE need them (image/audio/video, TTS,
   embeddings, characters, retries/backoff, async). Driven by real use, not
   speculatively.
3. KDE integration (later leg) — a D-Bus/Qt service layer on top of this
   client (KRunner plugin first). Qt types stay OUT of this library.

**VC-07 (#8, install/export) is done** — see v0.2.0. Upstream CT-04 landed the
pattern for the whole cpp-template family and this repo ported it rather than
inventing a second one, with four divergences recorded in `cmake/install.cmake`.
`find_package(venice-cpp CONFIG)` is real; `PROJECT_IS_TOP_LEVEL` gating came
with it. Porting it surfaced a third upstream-relevant finding: the build-tree
`export(EXPORT ...)` in upstream's file cannot work for any project with a public
FetchContent dependency that ships no `export()` call of its own.

## Cross-project context
- Stack: cpp-template (base) -> venice-cpp (API) + termforge (TUI) -> AIForge.
- venice-cpp issues double as the AIForge kickoff tracker for now (#1).
- The template↔fork sync runs both directions: this repo took upstream's fixes,
  and owes it three — two artifact-checker fixes (B2, B3) and the build-tree
  `export(EXPORT ...)` finding from the VC-07 port.

## How to verify
CI does the gate now (gcc + clang × {default,asan,tsan,ubsan}, nine jobs), but
run it locally first — see AGENTS.md "How to verify before a PR". Offline unit
tests (Catch2); live smoke needs $VENICE_API_KEY and is never run by ctest.
Two dependency-free checks worth knowing: `cmake -P cmake/version_selftest.cmake`
and `cmake -P cmake/check_artifacts.cmake`.

Watch the cpp-httplib API version quirks (Request has body+set_header, no
set_content/content_type_; send(req,res,err) returns bool).
