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
- **The project name is spelled out**, not derived from the directory name as
  upstream does. Since VC-07 it is public API — the spelling in
  `find_package(venice-cpp CONFIG)`, in `venice-cpp::lib`, and in
  `lib/cmake/venice-cpp`. Do not make it derived again.
- **Everything but the library is gated on `PROJECT_IS_TOP_LEVEL`**
  (`venice-cpp_BUILD_BIN`, `_TESTS`, `_INSTALL`). A consumed build gets the
  library and nothing else. The options are declared *above* the dependency list
  because the list and two dep recipes are gated on them.
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
  invalid_arg / cancelled, each carrying `status` + raw `body`. `cancelled` is
  deliberately not folded into `network` (VC-06): a dead network is a fault to
  report or retry, a cancellation is the caller's own decision arriving back at
  them, and collapsing the two leaves the difference reachable only by parsing
  message text.
- **Per-call concerns go in `RequestOptions`, not on the request.** Timeout
  overrides and the cancel token are a property of *this call* — never
  serialized, and applying identically to `models()` and `balance()`, which have
  no request object at all. Same reasoning that keeps `stream` off
  `ChatRequest`. Every member of `RequestOptions` carries an explicit default
  initializer even where the type already default-constructs empty: without
  them `-Wmissing-field-initializers` fires once per omitted member on the
  designated-initializer spelling the type exists for, i.e. on every correct
  caller.
- **Cancellation is a watcher thread, and that is measured rather than
  stylistic.** httplib's `ClientImpl::send_` holds `socket_mutex_` only around
  setup and teardown, so `Client::stop()` from another thread interrupts a
  stalled read — but `create_and_connect_socket` runs under that same mutex, so
  firing `stop()` from `CancelToken::cancel()` would block the *cancelling*
  thread for up to the connect timeout. `detail::CancelGuard` spawns a thread
  only when a token is supplied, and is declared after the httplib client so
  reverse destruction joins it while the client is still alive. It also blocks
  SIGPIPE for the request: over TLS, shutting the socket down makes OpenSSL
  write a close_notify to a peer that is gone, and the resulting EPIPE kills the
  process at the default disposition. Thread-scoped via `pthread_sigmask`, never
  process-wide `signal()` — the disposition belongs to the application.
- **Request serialization:** only serialize set fields. Both `ChatRequest` and
  `VeniceParameters` carry an `extra` json passthrough so future Venice keys are
  reachable without editing the header; modeled fields always win over a
  same-named key in `extra`, and the `is_object()` guard is load-bearing — a
  non-object `extra` would otherwise throw out of `to_json_body()`.
  `ChatRequest` has **no `stream` member** — `to_json_body(bool stream)` takes the
  mode, because it belongs to the call and not to the request. That is what lets
  both `Client` entry points serialize from a `const ChatRequest&` instead of
  copying the whole request (an arbitrarily deep `extra` tree included) to flip
  one bool, and it leaves no second source of truth for that bit. The parameter
  has no default, on purpose: a defaulted `stream` would rebuild the very defect
  it retires, a bit that looks set and silently never reaches the wire.
- **Range checking: none, deliberately — representability checking: yes.**
  Structural preconditions that make a request unsendable by construction are
  `ErrorKind::InvalidArg`, checked in `Client::validate` before any socket: an
  empty model, no messages, and a non-finite `temperature` / `top_p` /
  `frequency_penalty` / `presence_penalty`. NaN and ±inf are not a range opinion
  — JSON cannot encode them, nlohmann collapses them to `null`, and the 400 that
  follows never mentions NaN (VC-10). Value *ranges* (temperature 0-2, top_p
  0-1, penalties -2..2) are the server's policy and are transmitted verbatim,
  because a bound hardcoded here goes stale when Venice widens it. The guard
  covers modeled fields only; `extra` is passthrough and is not walked. The
  non-finite sweep runs *first*, which is what makes the guard's passing path
  testable offline — see `test/03guards/`. `tools` is not walked either (VC-08),
  and that is the same decision rather than a new one: `messages` is checked for
  emptiness and never *entered*, so `Message::role` is already unvalidated, and
  guarding `tools[i].name` while ignoring `messages[i].role` would be a coin flip
  rather than a line. The server's 400 names the offending tool entry, which is
  the property the non-finite sweep has and this would not.
- **Caller-authored polymorphic fields are raw json with builders, never a typed
  struct.** `response_format`, `tool_choice`, and the *elements* of `tools` are
  all `nlohmann::json`. The recurring temptation is a `Tool` struct mirroring
  `ToolCall` — flat members, a `to_json` that re-nests under `"function"`. It
  hardcodes a *shape* set the way an enum hardcodes a value set, and would emit
  `{"type":"web_search","function":{"name":""}}` the day Venice accepts a tool
  entry that is not a function. `ToolCall` may nest unconditionally because it
  re-serializes what the server *sent*. It would also break the escape hatch
  rather than merely omit it: a typed element leaves only `extra["tools"]`, which
  *loses* whenever `tools` is engaged, so the hatch's behaviour would flip on an
  unrelated field. The container stays typed
  (`optional<vector<nlohmann::json>>`) so engaged-but-empty can emit `[]`.
- **Braces build arrays, parentheses build scalars.** `nlohmann::json{"auto"}` is
  `["auto"]`; `nlohmann::json("auto")` is `"auto"`. Both compile, so only an
  `is_string()`-style assertion catches the wrong one. The *object* builders
  assign field by field for readability and not because brace-init mis-parses
  them — VC-08 measured six spellings on the pinned 3.11.3, including the
  array-valued-schema case a comment had long blamed, and every one produced the
  correct object. The style survived; its stated rationale did not.
- **Response parsing: tolerant for listings, loud for everything else.**
  `venice::detail::opt_bool` / `opt_int` / `opt_i64` / `opt_double` /
  `opt_string` / `opt_object` / `opt_array` / `string_array` in `types.hpp` read
  a field or return nullopt, using nlohmann **type predicates rather than
  try/catch**. That is not style: measured against the pinned 3.11.3,
  `get<int>()` returns `1` for `1.9` and `276447231` for `99999999999999` and
  throws in neither case, so an exception-only guard turns a wrong-typed number
  into a confident wrong one — and the second case does not even trip UBSan,
  because the narrowing is well-defined. `opt_int` therefore goes through
  `opt_i64` and range-checks. `opt_double` accepts `is_number()`, not
  `is_number_float()`: Venice quotes whole prices as JSON integers.
  These are for *listings*, where one odd entry must not cost the caller the
  other hundred. Do **not** retrofit them onto `Usage` / `ChatResponse` — those
  parse inside `Client::chat`'s try/catch, where a malformed body should fail as
  `ErrorKind::Parse`; a chat reply has no sibling entries to protect and
  silently zeroing a token count would hide a billing bug.
- **Endpoint filters are caller-supplied strings, and an unset one sends no
  query key.** `models(type)` takes Venice's modality as a string rather than an
  enum, on the same reasoning as `response_format`: the value set is the
  server's, and a list hardcoded here would refuse a valid value the day a
  modality is added. `venice::detail::with_query` in `client.hpp` **skips any
  pair whose value is empty**, so `models()` with no argument produces the bare
  `/models` it always did — that skip is the non-breaking guarantee, pinned by
  `test/05query/`, not a formatting nicety. Both it and `percent_encode` are
  free functions at namespace scope so the encoding is testable without a
  socket, the same move VC-03 made with `models_from_json_body`. Since VC-04
  there are two `with_query` overloads — an `initializer_list` one for filters
  spelled out at the call site and an owning one taking `std::string` pairs for
  a query built at runtime, because a `string_view` into `std::to_string`'s
  temporary dangles. They share `detail::append_param` deliberately: two copies
  of the empty-value skip is exactly how one endpoint keeps that guarantee while
  another quietly loses it. A filter set wide enough to need a struct
  (`CharacterQuery`) flattens to pairs in a free function of its own, so the
  ordering and the encoding stay checkable offline.
- **Response-side escape hatches are named `raw`, not `extra`.** `Model::raw`
  holds the verbatim entry — modeled fields included — because a *subtractive*
  hatch breaks its readers every time a key graduates to a typed field. The
  request-side `extra` on `ChatRequest` / `VeniceParameters` is the opposite
  contract (additive, merged into the wire body, modeled fields win), so it
  keeps the opposite name.
- **`Message` carries BOTH `raw` and `extra`, and one field cannot serve both.**
  It is the only struct here that legitimately travels in both directions, so it
  gets both contracts: `raw` is the verbatim server object and is **never
  serialized**; `extra` is the additive request-side seed. Collapsing them into
  one `raw` that also seeds `to_json` makes a parsed reply round-trip for free
  and is why it keeps getting proposed — it is also silently wrong. It honours
  `m.content = "edited"` but ignores `m.content.reset()`, so a caller redacting
  history resends the answer, and a caller clearing an executed `tool_calls`
  re-issues the call (a 400 for an unanswered tool_call, or an infinite agent
  loop). Measured on the same redacted turn: seed-from-raw emitted the full
  answer and the tool call, assign-or-erase emitted `{"role":"assistant"}`. It
  would also restore the deep-copy-per-request regression VC-11 removed, since
  `j["messages"] = messages` would then copy every message's verbatim server
  body on every call. Verbatim replay stays available as one deliberate line:
  `m.extra = m.raw;`
- **The merge rule is assign-or-ERASE, and `Message::extra` does not shadow.**
  `Message::to_json` seeds from `extra`, then for every modeled key either
  assigns it (engaged) or **erases** it (disengaged). The erase branch is what
  makes the merge total and mutation-honest; without it two fields have the same
  bug as one. Note this deliberately does *not* inherit `ChatRequest::extra`'s
  tolerance for a same-named key: that one is caller-authored, so shadowing is
  arguably the caller's own request, while `Message::extra` will routinely be
  seeded from a response, which makes shadowing the common case rather than a
  pathology.
- **`Message::from_json` is total; `ChatResponse::from_json_body` is loud. Do
  not unify them.** A message sub-object's shape legitimately varies — content
  string/null/absent/parts-array, tool_calls present or not — and the old
  two-key parse threw `type_error.302` on
  `{"role":"assistant","content":null,"tool_calls":[...]}`, the canonical
  tool-call reply, so `Message` was not usable as a parse target at all. A
  completion body's *top-level* shape is a contract: `j.at("choices")` must keep
  throwing, because that is what `Client::chat`'s try/catch turns into
  `ErrorKind::Parse` and `test/01client/` pins it.
- **Usage/cost metadata:** keep cache buckets distinct (cached vs uncached
  tokens price differently — see venice-cli #75). Model *pricing* has two cache
  buckets (`cache_input` read, `cache_write` write) where `Usage` reports one,
  so the two cannot be paired 1:1 — don't write code that assumes they can.
- **KDE/Qt-readiness:** keep the library UI-free and Qt-linkable. No Qt types
  in the API client; a separate service layer owns D-Bus/KF concerns.

## Testing philosophy

**Test how code fails, not just that it produces the right output.** Write the
failure matrix first (bad input, boundaries, malformed JSON, error statuses);
the happy-path check is last. Unit tests are **offline** — no API key or
network. A live smoke check needs `$VENICE_API_KEY`.

`test/06transport/` is the one test that binds a socket, and the rule above is
what constrains how: no API key and no *internet*, but a timeout has no offline
form — it is by definition a property of a peer that does not answer — so it
brings its own peer, an in-process `httplib::Server` on 127.0.0.1 at an
ephemeral port. Two things about that fixture are worth knowing before trusting
a green run:

- **it speaks plain HTTP**, so nothing in it exercises the TLS teardown path,
  which is where VC-06's SIGPIPE bug lived;
- **constructing an `httplib::Server` calls `signal(SIGPIPE, SIG_IGN)`
  process-wide** (httplib.h:6087), so merely having the fixture defuses that
  signal for the whole binary.

Both together mean the suite was green on either side of a bug that killed any
real HTTPS caller. Anything touching socket teardown needs a live check against
api.venice.ai as well — `/models` answers for any bearer token, so that costs
nothing and needs no key.

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

# touching CMake wiring, install rules or the public headers? prove a consumer
# still builds all three ways, on both compilers
./example/consumer/verify.sh
CXX=clang++ ./example/consumer/verify.sh

# touching a sanitizer toolchain? prove it still engages
cmake -B build-asan -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/address.cmake \
  && cmake --build build-asan \
  && ctest --test-dir build-asan -R 30sanitizer-smoke-test -V | grep AddressSanitizer
```

CI (`.github/workflows/ci.yml`) enforces the dual-compiler rule on every push and
pull request: GCC × Clang across {default, address, thread, undefined}, plus the
consumer acceptance check on both compilers and the version self-test standalone
— eleven jobs. A one-compiler change turns that compiler's five jobs red. Run the
commands above locally first.

`example/consumer/verify.sh` is slow by design (four configures, three dependency
fetches), which is why it is a CI job and not a ctest test. For a quick local
loop, point it at an existing `_deps` directory:
`VENICE_DEPS_CACHE=build/_deps ./example/consumer/verify.sh`. CI deliberately
leaves that unset — fetching for real is part of what it tests.

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
- **`cmake/install.cmake` is a port of upstream's (ticket CT-04), not a copy.**
  Four divergences are marked ⚠ in the file; if you re-sync from upstream, keep
  them, because each is a property of venice-cpp that upstream does not share:
  (1) no ARCHIVE/LIBRARY/RUNTIME destinations — header-only by design, not by
  configuration; (2) **no build-tree `export(EXPORT ...)`** — it fails outright
  here, because cpp-httplib is a public dependency that registers no build export
  set (`export called with target "venice-cpp_lib" which requires target
  "httplib" that is not in any export set`); (3) `include/venice/` only, so the
  generated `version.hpp` and its unprefixed globals stay out of consumers'
  include paths; (4) `SameMinorVersion`, since at 0.x `SameMajorVersion` would
  claim 0.1.0 satisfies a request for 0.9.0 — revisit at 1.0.0.
  The public dependencies are the reason any of this needs thought: upstream's
  library has none, ours has four, so `cmake/project-config.cmake.in` must
  `find_dependency` each one, and `HTTPLIB_INSTALL` / `JSON_Install` are tied to
  `venice-cpp_INSTALL` in the dep recipes so those targets land in an export set
  when we install and stay out of a consumer's prefix when we don't.
- **`cmake/check_artifacts.cmake`** runs in enforce mode here (ctest:
  `artifact-check`) — every rule must report zero hits. Class A catches leftover
  template artifacts; Class B catches wiring drift that stays relevant for the
  life of the project: every listed dep has a recipe (B1), no dep is fetched but
  unused (B2), the UBSan define matches on both sides (B3), target-guarded test
  dirs exist (B4), fixture scripts keep mode 100755 (B5). Run it directly with
  `cmake -P`. If a rule fires on something legitimate, fix the citation rather
  than loosening the rule — a rule that matches nothing passes everything.
- Build dirs (`build*/`) are gitignored — don't commit them.
