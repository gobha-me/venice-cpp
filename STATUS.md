# venice-cpp — status (for the next session)

A session-local snapshot of where the project is and what's next. Supplements
AGENTS.md (which holds standing conventions, not state).

## Where we are

**Phase 0: DONE and verified against the live API.**
- `chat()` non-streaming completion (verified: "venice-cpp works").
- `chat_stream()` SSE streaming via callback, cancellable (verified live).
- Per-request timeouts and a `CancelToken` on every entry point (VC-06).
- `models()` list with typed per-model metadata, filterable by modality
  (105 text / 299 all, fetched live), `balance()` rate-limit endpoint.
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
was blinded by any comment mentioning a package name. Both were upstream bugs and
have since landed there as CT-14.

## Next up
1. **AIForge chat-TUI MVP** — see issue #1. Composes venice-cpp + termforge.
   This is the agreed next move; both foundations are proven.
2. **VC-05 (#6), the structured stream-delta design**, is now unblocked — it had
   to state its cancellation semantics and could not until VC-06 existed. The
   answer it inherits: `on_token` false stays a partial-success early stop,
   token cancel is `ErrorKind::Cancelled`, and a delta callback should not try
   to carry either.
3. Thicken endpoints as AIForge/KDE need them (image/audio/video, TTS,
   embeddings, characters, retries/backoff, async). Driven by real use, not
   speculatively.
4. KDE integration (later leg) — a D-Bus/Qt service layer on top of this
   client (KRunner plugin first). Qt types stay OUT of this library.

**VC-07 (#8, install/export) is done** — see v0.2.0. Upstream CT-04 landed the
pattern for the whole cpp-template family and this repo ported it rather than
inventing a second one, with four divergences recorded in `cmake/install.cmake`.
`find_package(venice-cpp CONFIG)` is real; `PROJECT_IS_TOP_LEVEL` gating came
with it. Porting it surfaced a third upstream-relevant finding: the build-tree
`export(EXPORT ...)` in upstream's file cannot work for any project with a public
FetchContent dependency that ships no `export()` call of its own.

**VC-02 (#3, request sampling parameters) is done** — see v0.3.0. `ChatRequest`
now carries `top_p`, `stop`, `frequency_penalty`, `presence_penalty`, `seed`
(int64 — callers seed from `mt19937`, which overflows `int`) and
`response_format`, plus a top-level `extra` json passthrough beyond the ticket's
text so unmodeled Venice keys (`top_k`, `min_p`, `repetition_penalty`) don't
require editing the header. `response_format` is raw JSON rather than an enum
because no enum can carry a `json_schema`; `venice::response_format::` supplies
builders. All request-body serialization tests now live in `test/02request/`,
anchored on a byte-exact baseline body — which was proven to fail by adding a
stray key before it was trusted.

**VC-11 (#15) and VC-10 (#14) are done** — see v0.4.0, shipped together because
both rewrite the head of `Client::chat` / `Client::chat_stream`. `ChatRequest`
lost its `stream` member; `to_json_body(bool stream)` takes the mode instead, so
neither entry point copies the request (VC-02's `extra` had made that a deep json
tree copy per call) and no state remains for a copy to mutate — the regression is
now uncompilable rather than merely untested. The duplicated precondition checks
became one private `Client::validate` returning `std::expected<void, Error>`,
which also gained an `std::isfinite` sweep over the four double fields: JSON
cannot encode NaN or ±inf, nlohmann emits `null`, and the server's 400 never says
why. The sweep runs before the emptiness checks on purpose — that ordering is
what lets `test/03guards/` prove the *passing* path without opening a socket.
`extra` is still not walked; that boundary is pinned by a test rather than left
implicit. Both are public API breaks, hence the minor bump.

**VC-03 (#4, typed model metadata) is done** — see v0.5.0. `Model` was `id` +
`type`; it now carries the context window, fourteen capability flags, a rate
card (`Price` / `PriceTier` / `Pricing`) and the human name/description, all
optional, plus `raw` holding the verbatim entry. Three decisions went past the
ticket's text and are worth knowing:

- **The escape hatch is `raw`, not `extra`, and it is a superset.** The ticket
  said "a raw `nlohmann::json extra` mirroring `VeniceParameters::extra`", but
  that mirror is inverted: the request-side `extra` is additive and merged into
  the wire body, this one is never sent. Making it *subtractive* (only unmodeled
  keys) would break every reader the release a key graduates to a typed field,
  so it holds the whole entry — which also makes a listing round-trippable for a
  downstream cache without adding a `to_json`.
- **`extended` pricing is typed, sharing `PriceTier` with the base rates.** Ten
  text models reprice past a context threshold, by 3x on one of them. Sharing
  the type makes tier selection an expression rather than a second code path.
  The selector itself is deliberately *not* shipped: Venice does not document
  whether `context_token_threshold` counts prompt tokens or prompt+completion,
  and a member function would encode that guess as API.
- **The parse left `Client`.** `models_from_json_body` is a free function in
  `types.hpp`, because the ticket's own acceptance criterion — malformed entries
  degrade rather than failing the list — is unreachable offline while the parse
  only exists behind a socket.

Two real defects surfaced on the way. `Client::models()` did
`res->contains("data") ? res->at("data") : *res` and then iterated: a `data`
that was an *object* iterated its values and a scalar body iterated as itself,
so both silently produced models built from whatever was there, reporting
success. That is now the one fatal case (`ErrorKind::Parse`); everything below
it degrades. And `get<int>()` neither throws nor trips UBSan on a float or an
out-of-range integer — it returns `1` for `1.9` and `276447231` for
`99999999999999` — which is why the `detail::opt_*` helpers are predicate-based
and `opt_int` range-checks. All four guards were proven by breaking them and
watching the intended case go red; the fourth break restored the old container
handling and turned all three §0 cases red at once.

Additive to `Model`, but `models()` changed behaviour on malformed input, hence
the minor bump. `?type=` was filed separately as VC-13 and is now done.

**VC-13 (#19, models type filter) is done** — see v0.6.0. `models()` grew a
defaulted `std::string_view type`, and with it the client's first query-string
encoder: `venice::detail::percent_encode` and `venice::detail::with_query`, free
functions at namespace scope in `client.hpp` for the reason VC-03 moved the
model parse out of `Client` — a private helper is reachable only through a
socket, and an encoder is a pure string transform. `test/05query/` is the unit.

Three things are worth knowing:

- **`with_query` skips any pair whose value is empty**, so a no-argument
  `models()` still sends the bare `/models` rather than `/models?type=`. That
  skip *is* the non-breaking guarantee and is asserted, not asserted-in-a-
  comment — breaking it deliberately turned four cases red, including the one
  that mirrors the pre-VC-13 call site.
- **The type is a caller-supplied string, not an enum**, on the same reasoning
  `response_format` is raw JSON. An unrecognised modality is the server's 400 to
  give; a list hardcoded in the header would refuse a valid value the day Venice
  adds one.
- **The signedness bug in `percent_encode` was measured, and the sanitizers
  disagree about it.** Dropping the `unsigned char` cast makes 0xFF encode as
  `"% F"` and `"é"` as `"%s3%t9"` — a negative index into the hex table. The
  default build is silent, **UBSan is also silent**, and **ASan does catch it**
  ("global-buffer-overflow ... in percent_encode"). The first draft of that
  comment claimed only an assertion could catch it; running all three is what
  corrected it. Worth knowing which check fails here: the property-shaped case
  (every high byte is three characters long) stays *green* through the bug,
  because the length is right and only the bytes are wrong.

Live, all nine modalities are now reachable: 105 text, 97 video, 37 image, 20
inpaint, 14 music, 11 tts, 9 embedding, 5 asr, 1 upscale — 299 total against the
105 the library could see before. (The July capture read 106/287; Venice's
catalogue moves, so the fixtures in `test/04models/` are pinned and these counts
are not.) Purely additive, hence the minor bump.

The ticket's optional companion — typed image pricing (`generation`,
`upscale.{2x,4x}`) and `model_spec.constraints` — was deliberately left out and
noted on the image epic (#10) instead. `Pricing` is the text shape by VC-03's
design; making it a per-modality union is a schema decision, and #10 needs the
constraints anyway.

**VC-06 (#7, transport timeouts + cancellation) is done** — see v0.7.0. Every
entry point grew a trailing defaulted `venice::RequestOptions`: connect / read /
write timeout overrides, and a borrowed `CancelToken*` that aborts a request in
flight. New `ErrorKind::Cancelled`. The 300s read / 30s connect defaults are
unchanged — a chat completion can legitimately take minutes — but they are a
floor a caller can move rather than a property of the library.

Four things are worth knowing, three of them measured rather than reasoned:

- **Cancellation is a watcher thread, not a callback, and httplib's source is
  why.** `ClientImpl::send_` holds `socket_mutex_` only around setup and the
  scope_exit teardown, so `Client::stop()` from another thread does interrupt a
  stalled read. But `create_and_connect_socket` runs under that same mutex — so
  firing `stop()` directly from `CancelToken::cancel()` would block the
  *cancelling* thread for up to the connect timeout, which is exactly the thread
  (a UI's) that must not block. `detail::CancelGuard` spawns a thread only when
  a token is supplied and joins in its destructor.

- **The retry loop and the pre-send check are not redundant, and neither
  subsumes the other.** No test covers the retry loop directly — §0's pre-send
  check short-circuits the only case that reaches it, so reducing the loop to a
  single `stop()` leaves the whole suite green. Removing *both* is what exposes
  it. §0 run repeatedly against a server whose stall handler gives up after ten
  seconds:

      single stop(), no pre-send check — 6 of 10 runs took 10018ms and failed;
        the cancel was simply lost and the call waited the server out
      retry loop, no pre-send check    — 0 of 10 hung, all finished in 10-17ms,
        but 2 of 10 still failed on "no request was sent"

  The loop is what makes a cancel that beats the socket into existence
  *effective*; the pre-send check is what makes a pre-cancelled token send
  nothing at all.

- **Cancelling a TLS request killed the process, and the test suite could not
  have caught it.** Shutting a socket down under a live request makes OpenSSL's
  teardown write a close_notify to a peer that is gone; EPIPE on a socket raises
  SIGPIPE, which terminates by default. Live against api.venice.ai the cancel
  case exited 141 (128+SIGPIPE) where the control and read-timeout cases
  returned normally; with SIGPIPE ignored the same binary reported
  `cancel over TLS: 52ms cancelled`. `test/06transport/` is structurally blind
  to this twice over — its peer speaks plain HTTP, which has no close_notify to
  write, and constructing an `httplib::Server` at all runs
  `signal(SIGPIPE, SIG_IGN)` process-wide (httplib.h:6087), defusing the signal
  for the whole binary. The fix is `detail::SigPipeBlock`: `pthread_sigmask` for
  the request's duration plus a drain of anything left pending, thread-scoped
  because the disposition belongs to the application. §7 pins the half that *is*
  checkable on loopback — the mask and disposition come back as found — and its
  first spelling was itself wrong, comparing before against after and staying
  green with the restore deleted, because an earlier case had already leaked the
  block. It asserts the mask is clean at both ends now.

- **`test/06transport/` is the first test in the repo to bind a socket.** The
  offline rule stands — no key, no internet — but a timeout has no offline form,
  so the test brings its own peer. Nine cases, failure matrix first. The whole
  file runs in 0.79s against a 300s default, which is itself the evidence that
  cancellation is prompt.

Streaming keeps both stop mechanisms, deliberately distinct: `on_token`
returning false is still a partial-success early stop, `token.cancel()` is
`Cancelled` with no response. §5 pins them apart. Additive to every signature,
but a new `ErrorKind` enumerator is visible to an exhaustive switch, hence the
minor bump. Threads is now a public CMake requirement.

Validated on gcc + clang × {default, asan, tsan, ubsan}, 11/11 each; 20
consecutive transport runs clean under TSan and under ASan; consumer harness
3/3 on both compilers.

## Cross-project context
- Stack: cpp-template (base) -> venice-cpp (API) + termforge (TUI) -> AIForge.
- venice-cpp issues double as the AIForge kickoff tracker for now (#1).
- The template↔fork sync runs both directions and is current: the artifact-checker
  fixes this repo owed upstream landed there as CT-14, and the VC-07 port's own
  finding — upstream's build-tree `export(EXPORT ...)` cannot work for a fork with
  a public FetchContent dependency — is filed as CT-15.

## How to verify
CI does the gate now (gcc + clang × {default,asan,tsan,ubsan}, nine jobs), but
run it locally first — see AGENTS.md "How to verify before a PR". Offline unit
tests (Catch2); live smoke needs $VENICE_API_KEY and is never run by ctest.
Two dependency-free checks worth knowing: `cmake -P cmake/version_selftest.cmake`
and `cmake -P cmake/check_artifacts.cmake`.

Watch the cpp-httplib API version quirks (Request has body+set_header, no
set_content/content_type_; send(req,res,err) returns bool).
