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

**VC-07 (#8, install/export) stays deferred on purpose.** Upstream CT-04 solves
install/export once for the whole cpp-template family precisely so downstream
repos don't each invent one. When it lands, crib it — the root CMakeLists TODO
says so. `PROJECT_IS_TOP_LEVEL` gating belongs to the same ticket.

## Cross-project context
- Stack: cpp-template (base) -> venice-cpp (API) + termforge (TUI) -> AIForge.
- venice-cpp issues double as the AIForge kickoff tracker for now (#1).
- The template↔fork sync runs both directions: this repo took upstream's fixes,
  and owes it two artifact-checker fixes.

## How to verify
CI does the gate now (gcc + clang × {default,asan,tsan,ubsan}, nine jobs), but
run it locally first — see AGENTS.md "How to verify before a PR". Offline unit
tests (Catch2); live smoke needs $VENICE_API_KEY and is never run by ctest.
Two dependency-free checks worth knowing: `cmake -P cmake/version_selftest.cmake`
and `cmake -P cmake/check_artifacts.cmake`.

Watch the cpp-httplib API version quirks (Request has body+set_header, no
set_content/content_type_; send(req,res,err) returns bool).
