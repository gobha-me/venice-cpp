# venice-cpp — status (for the next session)

A session-local snapshot of where the project is and what's next. Supplements
AGENTS.md (which holds standing conventions, not state).

## Where we are

**Phase 0: DONE and verified against the live API.**
- `chat()` non-streaming completion (verified: "venice-cpp works").
- `chat_stream()` SSE streaming, three forms: a content-text callback and two
  that assemble into a caller-owned `StreamAccumulator`. Cancellable.
- A reply is a `Message` and a `Message` is what you send — reasoning_content,
  tool_calls, tool_call_id, refusal, multimodal content parts, all
  round-trippable, all individually withholdable.
- Per-request timeouts and a `CancelToken` on every entry point (VC-06).
- `models()` list with typed per-model metadata, filterable by modality
  (105 text / 299 all, fetched live), `balance()` rate-limit endpoint.
- `characters()` list with typed per-character metadata and a `CharacterQuery`
  carrying the endpoint's filters and its pagination (VC-04). Parsed offline
  only — see the caveat in that entry below.
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
1. **A live capture of a streaming reply.** VC-05 shipped with its wire shapes
   taken from Venice's published docs rather than a capture — `/models` answers
   for any bearer token but chat does not, and the implementing environment had
   no key. `venice-cpp --stream` exists to settle it: it reports whether
   `reasoning_content`, `completion_tokens_details.reasoning_tokens` and
   `prompt_tokens_details.cached_tokens` land where the fixtures in
   `test/07stream/` say. If one disagrees, the fixture is what needs correcting;
   `Message::raw` / `ChatResponse::raw` / `acc.chunks()` are why a wrong guess
   is recoverable rather than lossy.
2. **AIForge chat-TUI MVP** — see issue #1. Composes venice-cpp + termforge.
   Both foundations are proven.
3. **A live capture of a tool call**, the VC-08 half of the same gap.
   `venice-cpp --tools` runs it in two legs; see the VC-08 entry below.
4. **A live capture of a character entry**, the VC-04 half. `venice-cpp
   --characters` is the check, and unlike `--models` it needs a real key:
   /characters answers 402 unauthenticated and 401 to a junk bearer. Two things
   it settles that are currently guesses — that `slug` is always present (the
   parse skips entries without one, so a short listing is that guess failing),
   and that the default page really is 50.
5. Thicken endpoints as AIForge/KDE need them (image/audio/video, TTS,
   embeddings, retries/backoff, async). Driven by real use, not
   speculatively.
6. KDE integration (later leg) — a D-Bus/Qt service layer on top of this
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

**VC-05 (#6) and VC-14 (#22) are done** — see v0.8.0, shipped together because
the streaming accumulator assembles *into* the types VC-14 adds. The ticket was
filed as "structured delta callback" and that framing was wrong: the callback is
a symptom, the defect was that the library could not represent an assistant
turn. `Message` was `{role, content}` — the only wire-facing struct in
`types.hpp` with no passthrough at all — so refeeding a reasoning model's
thinking was unexpressible, and `ChatResponse` reached *through*
`choices[0].message` for content alone, discarding role, tool_calls,
reasoning_content, refusal, the `venice_parameters` echo and `choices[1..n]`
with no `raw` to recover them from.

Five things are worth knowing, four of them measured rather than reasoned:

- **The tempting design is silently wrong, and "the round trip works" does not
  catch it.** One dual-purpose `raw` seeding `to_json` makes a parsed reply
  round-trip losslessly for free. It also honours `m.content = "edited"` while
  ignoring `m.content.reset()` — so a caller redacting history resends the
  answer, and a caller clearing an already-executed `tool_calls` re-issues the
  call. Both rules implemented side by side over the same redacted turn:
  seed-from-raw emitted the full answer plus the tool call, assign-or-erase
  emitted `{"role":"assistant"}`. The rule is therefore **two hatches** (`raw`
  never serialized, `extra` additive) and **assign-or-ERASE** for every modeled
  key. Installing the wrong rule turned four cases red and left 22 green — and
  the one that stayed green was §6, replay fidelity, which a design that merely
  echoes satisfies trivially. That is why the failure matrix opens with the
  negative.

- **A test can be green on both sides of the bug it was written for.** The CRLF
  framing case fed one frame and let `finish()` flush at end of stream, so a
  frame that never dispatched from the framing loop still came out of the flush.
  Deleting the CRLF branch entirely left all 49 cases passing. It now feeds two
  frames with `finish=false`, because streaming means a frame dispatches when it
  arrives, not when the body ends. This is the second time the "confirm the test
  *could* see it" prerequisite has bitten here, after VC-06's SIGPIPE fixture.

- **Three defects were hiding behind the socket.** Extracting `detail::SseFramer`
  to namespace scope — the third time this repo has made that move, after
  VC-03's `models_from_json_body` and VC-13's `percent_encode` — exposed them:
  CRLF frames never dispatched at all (measured 1 payload for LF, 0 for CRLF);
  the trailing unterminated frame was dropped, and it is frequently the *usage*
  frame, so the loss was a billing bug; and the leftover buffer was unbounded.
  Two more went with them: the fatal-parse test was `content.empty()`, which
  reported `ErrorKind::Parse` on a reasoning-only stream that had arrived
  perfectly, and an early stop kept parsing frames already in the buffer.

- **`Usage::cached_tokens` was very likely always `nullopt` against real
  Venice.** It read a *flat* `usage.cached_tokens`, while OpenAI-compatible
  bodies nest it at `prompt_tokens_details.cached_tokens`. Both are read now,
  nested winning on disagreement. `reasoning_tokens` was unreachable entirely,
  which made a reasoning model's actual cost invisible.

- **The overload set is shaped by an ambiguity, and the shape turned out to be
  the better design.** A bare `bool(const StreamDelta&)` overload beside the
  `string_view` one makes `chat_stream(req, [](auto d){...})` and
  `chat_stream(req, nullptr, opts)` hard errors — a source break for code that
  compiles today. Requiring a caller-owned `StreamAccumulator` for the
  structured forms resolves it *and* makes the promise structural: you cannot
  ask for the rich stream without supplying storage that survives a cancel.
  VC-06's contract is untouched (`Cancelled`, no response), but abandoning a
  call no longer destroys what arrived — `test/06transport/` §7b pins that
  through a real socket, and breaking it by resetting the accumulator on cancel
  turns it red.

Public API break: `Message::content` is now `std::optional<nlohmann::json>` (the
only type expressing absent / null / text / multimodal parts) and `ChatResponse`
gained `optional<Message>`. Taken on VC-11's grounds — a `content` you cannot
clear is a defect. `test/02request/`'s `kBaseline` is byte-identical and
untouched.

The wire shapes come from Venice's published docs, **not a capture** — there was
no `VENICE_API_KEY` in the implementing environment. `venice-cpp --stream` is
the live check; see "Next up".

**VC-08 (#9, request-side tool calling) is done** — see v0.9.0, and it closes the
epic. `ChatRequest` gained `tools`, `tool_choice` and `parallel_tool_calls` plus
`venice::tools::` and `venice::tool_choice::` builders. Purely additive: the
`kBaseline` body in `test/02request/` is byte-identical and untouched, which is
the acceptance criterion the whole ticket reduces to. Stages 2 and 3 of the epic
had already landed in VC-05/VC-14, because the accumulator could not be specified
without them.

Four things are worth knowing, three of them measured rather than reasoned:

- **`tools` holds raw json *elements*, not a typed `Tool`.** The tempting design
  mirrors `ToolCall` 250 lines up — flat members, a `to_json` re-nesting under
  `"function"` — and it fails the way an enum fails: it hardcodes one *shape*,
  and emits `{"type":"web_search","function":{"name":""}}` the day Venice accepts
  an entry that is not a function. `ToolCall` may nest unconditionally because it
  re-serializes what the server *sent*; `tools` is caller-authored. It also
  breaks the escape hatch rather than omitting it — a typed element leaves only
  `extra["tools"]`, which *loses* whenever `tools` is engaged, so the hatch's
  behaviour would flip on an unrelated field. The container stays typed so
  engaged-but-empty still emits `[]`.

- **A comment this repo had trusted for four releases was wrong, and finding it
  cost one break that came back green.** `response_format::json_schema` carried
  "an array-valued `schema` makes the outer initializer-list ambiguous and
  nlohmann reads the whole thing as an array", and `test/02request/` had a case
  claiming to pin it. Rebuilding `json_schema` as a single brace-init left all
  115 cases green; six further spellings were then measured on the pinned 3.11.3
  — nested one-shot, array-valued, runtime-variable, inline two-string arrays —
  and every one produced the correct object. The real hazard is one level down
  and is about *scalars*: `nlohmann::json{"auto"}` is `["auto"]` while
  `nlohmann::json("auto")` is `"auto"`, both compile, and only a type assertion
  catches it. Field-by-field assignment stays because it does not depend on
  nlohmann's init-list heuristic; the citation for it was corrected in both
  places, and the old test's claim was rewritten to say what it actually checks.
  This is the third time here that "confirm the test *could* see it" has paid,
  after VC-06's SIGPIPE fixture and VC-05's CRLF case.

- **`if (*parallel_tool_calls)` is worse than the wrong answer it looks like.**
  The bug an `optional<bool>` invites is treating `= false` as unset — and false
  is the interesting value, set precisely when an agent loop cannot run two calls
  at once. But dereferencing the *unset* case is UB, so breaking that one line
  reddened seven cases including the baseline, and the red set varied between
  runs. The guard is the same `if (opt)` the penalties case has pinned since
  VC-02.

- **Nothing tool-shaped is validated, and `Message::role` is why.** Refusing an
  empty tool name looks like it belongs beside "model is empty" — both are
  representable in JSON and refused anyway. But `Client::validate` checks
  `messages` for emptiness and never *enters* it, so a request carrying
  `{Message{}}` already sails through on a guaranteed 400. Guarding
  `tools[i].name` while ignoring `messages[i].role` is a coin flip, not a line;
  and the server's 400 names the offending entry, which is the property the
  non-finite sweep has and this would not. Pinned in `test/03guards/` as a
  decision, next to the identical `extra` boundary.

Eight breaks were run, each reverted; seven went red where intended and the
eighth is the finding above. The wire shapes come from Venice's published docs,
**not a capture** — still no `VENICE_API_KEY` in the implementing environment.
`venice-cpp --tools` is the live check, in two legs, and only the second (answer
the call, send the history back) proves the turn is one Venice will continue.

**VC-04 (#5, characters endpoint) is done** — see v0.10.0. `characters()`
returns a typed `CharacterPage`, `Character` carries
slug/name/description/tags/stats plus the verbatim entry as `raw`, and
`CharacterQuery` carries the endpoint's filters. As with VC-03 the parse left
`Client` — `venice::characters_from_json_body` and
`venice::character_query_params` are free functions in `types.hpp`, so the whole
failure matrix is offline in `test/08characters/`. Purely additive;
`test/02request/`'s `kBaseline` and every `/models` case are untouched.

Six things are worth knowing, four of them measured rather than reasoned:

- **The ticket asked for `extra` and it shipped as `raw`.** The same correction
  VC-03 made against its own wording, for the same reason: a response-side hatch
  is a superset and is never sent, while `extra` is additive and merged into the
  wire body. `CharacterQuery` does get an `extra`, correctly — it is request-side
  and it is sent.

- **`CharacterQuery` exists because the endpoint pages, which the ticket does
  not mention.** `limit` defaults to 50, caps at 100. A `characters()` with
  nothing to say would have answered the first 50 of N and called it the list —
  this ticket's own defect ("you cannot discover what exists") one layer down.
  There is deliberately no all-pages helper: that loop needs a policy for a page
  that fails halfway, and abandon / retry / return-what-we-have are each wrong
  for someone. The loop is written out in `client.hpp` and README instead.

- **The return type is a `CharacterPage` and not the vector the ticket asked
  for, because a vector could not express the thing the paging loop needs.**
  The first draft returned `vector<Character>` and documented `page->size() <
  100` as the termination test. That is wrong, and wrong in the silent
  direction: the parse skips entries with no usable slug, so a full page of 100
  containing one such entry comes back as 99, the loop stops, and a truncated
  catalogue is reported as a complete one. No caller could have fixed it from
  outside — the server's page size was not reachable through the API at all.
  `CharacterPage::returned` is that count, `entries` is what survived, and
  `raw` keeps the envelope so a `total` or cursor turning up later is an
  additive discovery rather than a breaking signature change. This is
  [[information-model-before-api-surface]] again: the bug was that the type
  could not hold the fact, and no amount of fixing the loop would have helped.

- **A comment justified a design with a constraint that did not exist, for the
  second release running.** `tags` / `categories` / `model_id` were
  comma-joined, on the stated grounds that comma "is the one a query string can
  express without `with_query` growing a multimap". The owning `with_query`
  overload added in the same change takes a `vector<pair>` and loops it with no
  dedup — it *is* a multimap, and `{{"tags","a"},{"tags","b"}}` emits
  `?tags=a&tags=b` today, which was measured. So the rationale was false and
  the cost was real: comma-joining made `{"a,b"}` and `{"a","b"}` byte-identical
  on the wire, silently turning one filter into two. They now repeat the key,
  which is the form the endpoint documents as primary anyway. Exactly VC-08's
  `response_format::json_schema` finding, and the lesson generalises: a comment
  asserting "X is impossible" is a claim to test, not a reason to accept.

- **A break came back green, and the reason is worth recording.** Removing the
  empty-value skip from `detail::append_param` left all 107 assertions in
  `test/08characters/` passing and turned six red in `test/05query/`. Not a gap:
  `character_query_params` drops empty values before the encoder ever sees one,
  so the character tests assert the right end-to-end behaviour and are
  structurally blind to which of the two layers produces it. `test/05query/` is
  the only file that can see that skip, which is precisely why the refactor put
  both `with_query` overloads on one shared `append_param` rather than copying
  it. The other three breaks went red where intended: dropping the `is_array()`
  guard reddened §0's four assertions, dropping the slug skip reddened two
  cases, and letting `extra` beat a set modeled field reddened one.

- **The fixtures are not a capture, and this time the endpoint said so out
  loud.** `/models` answers 200 for any bearer token, which is how
  `test/04models/` pinned itself to a real payload. `/characters` answers **402**
  unauthenticated and **401** to a junk bearer — both measured on 2026-08-09 —
  and there was no `VENICE_API_KEY` in the implementing environment. The fixture
  comes from Venice's published OpenAPI document instead
  (`api.venice.ai/doc/api/swagger.yaml`), which is a machine-readable schema with
  a per-field example rather than prose, so the keys and their types are the
  API's own; the *combination* has still never been on a wire. `venice-cpp
  --characters` is the live check and is item 4 in "Next up".

- **The documented-snippet gap is worse than "it might not compile", and the
  compile check does not close it.** Both the new character example and the
  *pre-existing* `models()` one at README.md:129 read `for (const auto& m :
  *client.models())`. That compiles clean on both compilers — and it is a
  use-after-free: the `expected` temporary dies at the end of the range-for's
  initializer, and P2718R0, which extends its lifetime, is GCC 15 / Clang 19
  while AGENTS.md declares GCC 13+ and CI runs stock GCC 13. Reduced to the same
  shape and run under ASan on GCC 14 it aborts with `stack-use-after-scope`;
  the named-variable form is clean. Both README blocks now bind the result
  first. So the VC-08 blind spot has a second half: compiling a documented
  snippet proves it compiles, and the interesting failures in a C++ example are
  lifetime ones that only running it under a sanitizer can see.

- **`Character::from_json` clears `slug` and `stats` before parsing.** Every
  other field is assigned unconditionally, so leaving those two conditional
  meant a re-parse into a live object kept the *previous* entry's slug beside
  the new entry's name — and the slug is the field that picks a persona.
  `characters_from_json_body` never hits it (it always starts fresh), but
  `j.get_to(c)` on a reused object is a reachable public path, and
  `test/08characters/` pins it. `Model::from_json` has the same shape for `id`
  and was left alone: fixing it belongs to a change that can test it.

Three things were left out on purpose. `GET /characters/{slug}` is the same
struct behind a different path and is filed as VC-16 (#26). The
README's FetchContent `GIT_TAG` was bumped by hand again, which is the drive-by
VC-12 (#17) exists to abolish. And **402 still maps to `ErrorKind::Http`, not
`Auth`** — this endpoint answers 402 to a credential-less call, and
`kind_for_status` only maps 401/403, so a consumer branching on `Auth` to
re-prompt will not fire. Widening that mapping changes the error model for every
endpoint, so it is VC-15 (#25) rather than something smuggled into an endpoint
PR; `client.hpp` says so where a caller will read it.

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
