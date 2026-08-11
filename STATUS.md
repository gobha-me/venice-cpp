# venice-cpp — status (for the next session)

A session-local snapshot of where the project is and what's next. Supplements
AGENTS.md (which holds standing conventions, not state).

## Where we are

**Phase 0: DONE and verified against the live API.**
- `chat()` non-streaming completion (verified: "venice-cpp works").
- `chat_stream()` SSE streaming, three forms: a content-text callback and two
  that assemble into a caller-owned `StreamAccumulator`. Cancellable.
- A reply is a `Message` and a `Message` is what you send — reasoning_content,
  tool_calls, tool_call_id, refusal, multimodal content parts and a tool call's
  `thought_signature`, all round-trippable, all individually withholdable.
- Per-request timeouts, a `CancelToken`, and an authentication override on every
  entry point (VC-06/VC-23).
- `models()` list with typed per-model metadata, filterable by modality
  (106 text / 314 all / 48 code, fetched live 2026-08-11), `balance()`
  rate-limit endpoint.
- `model_traits(type)` and `model_compatibility_mapping(type)` (VC-38) — the
  catalogue's own answer to "which model", and the first two operations in this
  library that need no credential at all. Verified live keyless on 2026-08-11.
- `characters()` list with typed per-character metadata and a `CharacterQuery`
  carrying the endpoint's filters and its pagination (VC-04). Verified live on
  2026-08-09: every modeled key present, no unmodeled key, default page 50.
- `character(slug)` direct fetch with segment-safe path encoding (VC-16), and
  since VC-37 it actually parses: the response is an envelope and v0.14.0
  returned an all-absent `Character`.
- `character_reviews(slug, query)` — the reviews behind a rating, paged by the
  server's own `pagination` (VC-36). Verified live on 2026-08-11: every modeled
  key present, no unmodeled key at any of the four levels.
- `venice_parameters` extension with forward-compatible `extra` passthrough.
- Error model: `std::expected<T, Error>`, kinds network/http/parse/auth/
  payment_required/rate_limited/invalid_arg/cancelled, each carrying status +
  raw body and response metadata when a response exists.
- Header-only INTERFACE lib; cpp-httplib + nlohmann/json (header-only) +
  OpenSSL (link-time). KDE/Qt-ready shape (UI-free, Qt-linkable).
- OpenAPI coverage: 8/49 operations implemented. The other 41 are assigned to
  family issues and checked in `cmake/openapi_manifest.json` (VC-35). Characters
  and Models are both 3/3 on operations; Models keeps its epic open for the
  `Model` metadata half (VC-39, #60).

**Build system synced with cpp-template, tagged v0.1.0 — the first release.**
The repo was scaffolded before upstream's fix rounds landed, so it was running a
stale build system. Ported: declarative opt-in deps (closes #2 — fmt and argparse
were being FetchContent-pulled with nothing linking them), hardened git-describe
version parsing with a self-test, working sanitizer toolchains (ASan/TSan were
silent no-ops — `find_library` sets `ASAN`, never `ASAN_FOUND`) plus UBSan and a
smoke test proving engagement, the leftover-artifact/wiring-drift check, test
discovery fixes, `.clangd` off its C++20 pin, narrowed `.gitignore`, and CI
enforcing the dual-compiler rule across eleven jobs.

Two defects were found *in* the ported artifact checker and fixed here: rule B3's
unanchored token regex gave a false pass on suffix-appended renames, and rule B2
was blinded by any comment mentioning a package name. Both were upstream bugs and
have since landed there as CT-14.

## Next up

**The live captures are done.** A key was available on 2026-08-09 and all of
`--models`, `--stream`, `--tools`, `--characters` and (since VC-17) `--usage`
were run against the live API. What they settled is recorded in each ticket's
entry below; what they *opened* is filed. One caution the VC-17 entry pays for
in full: a leg that auto-picks one model settles nothing about a shape that
varies by model family, and two tickets have now been filed on the strength of
a single-family run.

1. **AIForge chat-TUI MVP** — see issue #1. Composes venice-cpp + termforge.
   Both foundations are proven, and now measured rather than assumed.
2. **VC-19 (#31): no escape hatch reaches inside a tool call.** `m.extra =
   m.raw` is overwritten for `tool_calls` because that key is modeled, and
   `ToolCall` has `raw` but no `extra`, so an unmodeled tool-call key is
   unrecoverable. VC-18 was exactly that failure. The limit is pinned by a §0
   case in `test/07stream/` whose deliberate inversion is the ticket's
   acceptance criterion. The hard half is streaming: a merge rule for arbitrary
   unmodeled keys across fragments has no wire evidence to choose it yet.
3. Thicken endpoints as AIForge/KDE need them (image/audio/video, TTS,
   embeddings, retries/backoff, async). Driven by real use, not
   speculatively.
4. KDE integration (later leg) — a D-Bus/Qt service layer on top of this
   client (KRunner plugin first). Qt types stay OUT of this library.

**VC-38 (#59) is done** — see v0.16.0. It is the first ticket here whose whole
contract was measured before a line of it was written, because for once that was
possible: both operations answer without a key.

`Client::model_traits(type)` and `Client::model_compatibility_mapping(type)`
take the Models family to 3/3 on operations. `models()` hands back a hundred-odd
entries and leaves "which one" to the caller; these two answer it. Traits maps a
Venice capability name to the model currently holding it, and the mapping goes
the other way, from a foreign vendor's model id to what serves it here — which
is what lets an OpenAI-shaped codebase port without a translation table of its
own going stale.

**These are the first legs that run without a key** — which is not the same as
the first public operations, and the review of this branch is what caught the
difference. `models()` is `PublicOrBearer` too, and on the same 2026-08-11 run
`/models` answered 200 with no `Authorization` header, exactly as the two new
ones did; `/characters` answered 402. So the whole Models family has been
reachable without a credential since VC-13, and nothing here had ever
demonstrated it: `Authentication::public_access()` has existed since VC-23 but
every live leg sat behind `main()`'s `VENICE_API_KEY` early-return, so the
public path was proven only against the loopback fixture. `--traits` and
`--compat` dispatch above that guard and use `public_access()` even when a key
is set.

The first draft of this entry, and of the README and both headers, claimed these
were "the only operations in this library" that answer without a credential.
That was written from the shape of the tickets rather than from a measurement,
and this branch's own catalogue cross-check disproves it in passing — it calls
`models()` from a keyless public client on every run. `server-claims-need-measuring`
applies to the claims a PR makes about its own work, not only to the ones it
inherits.

What the live runs settled, all on 2026-08-11 and all with no key in the
environment:

- Both answer 200 with no `Authorization` header. `/models/traits` answers 200
  even for an *invalid* bearer — it serves publicly regardless of the header.
- **The two operations do not accept the same `type` values, despite
  byte-identical `parameters` blocks in Venice's OpenAPI document.**
  `traits?type=all` is a 200 with ten entries and `traits?type=code` a 200 with
  one; `compatibility_mapping?type=all` is a **400** naming the nine modalities
  it will take. The document's request enum omits `all` and `code` for both,
  which is wrong for the first and right for the second, while its *response*
  enum adds them back for both. This is the sharpest case yet for the standing
  rule that filters are caller-supplied strings: a validated set in the header
  would have had to encode a mistake the specification itself makes.
- An empty result is a success, not a 404 — `traits?type=tts`, `?type=video`,
  `?type=embedding` and `compatibility_mapping?type=image` all return `"data":{}`
  with 200. The jessica-2 lesson from VC-36 arriving a second time.
- The response `type` echoes the filter the server actually applied, so a caller
  that sends nothing is told it got `text` rather than left to assume. The leg
  turns that into a check: a query string that never arrived shows up as an echo
  mismatch, and nothing else in the leg can see one.
- `"object":"list"` over a `data` that is an object, not a list. Recorded as
  measured, not corrected.
- Trait key spelling is not uniform — `most_intelligent` beside `eliza-default`
  — and every mapping key is a foreign vendor id. No enum was ever possible.
- Every one of the 30 targets across both operations resolves to a real id in
  `models()`, and no mapping key collides with a real catalogue id, so an alias
  can never shadow a model a caller could have named directly. The leg re-checks
  this on every run but does **not** fail on it: a retired target is Venice's
  data drifting, not a defect here, and no change to this code could turn it
  green.

The design decision worth carrying forward is the one about the `data` envelope.
Three parsers in `types.hpp` fall back to the whole body when `data` is absent,
and copying that here would have been the obvious move and wrong: those demand
an *array* while the envelope is an *object*, so the fallback is type-disjoint,
whereas both levels here are objects and `{"object":"list","type":"text"}` would
have parsed into a two-entry map and reported success. `test/09catalogue/` pins
that exact body as the one case a reintroduced fallback would turn red — nothing
else in the suite can see it, since the map would be populated and `raw` intact.
The generalisation is now in AGENTS.md.

Also filed: **VC-39 (#60)**, the other half of #40 — extending `Model` with
modality constraints, output options, voice-cloning capability and deprecation
metadata for the media epics. Split out because it touches a type every existing
caller already uses, needs live captures across non-text modalities that
`test/04models/` does not cover, and flips no manifest state. #40 stays open for
it.

**VC-36 (#56) and VC-37 (#57) are done** — see v0.15.0, and the second of them
is the first ticket in this repo that a live leg filed on itself.

`Client::character_reviews(slug, query)` completes the Characters family: it is
the third of its three operations, and the first family to reach full coverage.
The reviews behind a rating were unreachable — `CharacterStats` has quoted an
average since VC-04 with no way to read a word of what it averaged.

The page is honest in a way the listing's cannot be. `/characters` carries no
total, so a walk has to compare `returned` against the limit; this response
carries `pagination` with the server's own page, pageSize, total and totalPages,
so a caller advances without inferring anything. Those four are `int` and read
strictly: a fractional or out-of-range value reads absent, and a loop that stops
early is recoverable where one driven by a truncated page number is not. The
ratings are doubles, which is the call `CharacterStats` already made for the same
numbers. Reviews and the listing do not share a pagination type — `page = 2`
skips a page and `offset = 2` skips two entries.

Nothing is dropped from a review for a missing field, and that is a deliberate
divergence from the listing: an entry there without a slug is skipped because a
slug is what a caller hands back to the API, and no field on a review is such a
handle. Only a non-object element is skipped, still counted in `returned`.

The slug is one encoded path segment, and it matters more here than on the
direct fetch because the path continues after it: an unencoded slash would
address a route this operation does not own, not merely another character. The
loopback fixture pins the exact target, including that the reviews route is
registered ahead of the detail catch-all — httplib matches in registration order
and `characters/(.*)` swallows `alan-watts/reviews` otherwise.

**The live leg ran, and it found VC-37.** `venice-cpp --character alan-watts`
against api.venice.ai on 2026-08-11 returned the reviews envelope exactly as
modeled — `{data, object, pagination, summary}`, the same nine entry keys, no
unmodeled key at any of the four levels, `pageSize` echoing the 5 requested,
`userAvatarUrl` null, one `message` an empty string, `averageRating` an integer.
The same run printed `typed slug: (absent)` for the character itself.

A second character was run rather than trusting one — the VC-17 lesson.
`--character jessica-2`, which has no reviews, returned an empty `data` with
`total` and `totalPages` both 0 rather than a 404, so "no reviews" is not a
failure and the leg does not treat it as one. The same listing quotes
`2.33333` for nora-clark beside alan-watts's `5`, which is why the ratings are
doubles: one live sample would have justified either reader, and both are on
the wire.

`Client::character(slug)` had been returning an all-absent `Character` since
v0.14.0. The response is an envelope — `{"data": {...}, "object": "character"}`
— and `character_from_json_body` parsed the envelope as the character. Two
checks failed to see it, and both failures are the same shape:

- the offline fixtures were the *inner* object, because VC-16 read the OpenAPI
  document's `properties.data.properties` as the body rather than as the body's
  `data` member. Parser and fixture shared one wrong premise and agreed
  perfectly. The document was not ambiguous; the reading was.
- `--character`'s raw-versus-typed check asked whether raw's slug *disagreed*
  with the typed one. The envelope has no top-level slug, so the comparison was
  skipped rather than failed — a check that can only fire when the parse is
  nearly right.

The fix unwraps an object-valued `data` exactly as the listing already unwraps
an array-valued one; a bare object still parses, which is what lets a listing
entry be re-parsed through the same function. `Character::raw` is the character
and not the envelope. The captured envelope is now pinned in
`test/08characters/`, and the leg's slug check is absolute — an empty typed slug
on a 200 fails outright — rather than relative to what raw happens to contain.

**VC-12 (#17) is done** — see v0.14.1. Artifact rule B6 parses README's
`FetchContent_Declare(venice-cpp ...)` block and rejects a missing, ambiguous,
malformed, or stale `GIT_TAG`. A release PR may name a tag newer than the latest
existing tag because the tag is created only after merge; once released the two
are equal. Shallow clones and source tarballs still validate the README shape
but skip the age comparison when no release tag is reachable. The pure CMake
failure matrix covers stale patch/minor pins, malformed and describe-style
values, missing/duplicate declarations, the no-tag path, a future patch, and an
equal release.

**VC-16 (#26) is done** — see v0.14.0. `Client::character(slug)` fetches one
known character without paging the catalogue. The response reuses `Character`:
its top level must be an object, its preview fields remain tolerant, and `raw`
retains the complete server object. The requested slug is never synthesized
into a response that omitted it.

An empty slug is structural invalidity and returns `InvalidArg` before auth or
transport. Every non-empty slug is encoded as one RFC 3986 path segment, so a
slash, query delimiter, fragment delimiter, percent sign, high byte or embedded
NUL cannot select another route. The loopback fixture pins that exact target and
also proves Bearer/per-call auth, 404 body and metadata retention, and malformed
success-body classification. `--character <slug>` is the live check and prints
the verbatim object beside the typed fields; it was not run for this release
because the implementing environment had no `VENICE_API_KEY`.

**It was run one release later, and the fetch was broken the whole time** — see
VC-37 above. Everything in the two paragraphs above is still true except the
claim that the response reuses `Character` directly: its top level is an
envelope. The unrun live check is exactly what let that ship.

**VC-23 (#38) and VC-15 (#25) are done** — see v0.13.0. Authentication is now
transport state with four explicit modes: Public, Bearer, pre-signed SIWX and a
pre-built x402 payment payload. A client supplies the default and
`RequestOptions::authentication` can override one call. The compatible string
constructor remains exactly Bearer; an empty string is an invalid Bearer rather
than a secret spelling for Public. Existing endpoints enforce the audited
matrix before a socket: models accepts Public/Bearer, chat accepts Bearer/SIWX,
and characters/rate-limits require Bearer.

The emitted wallet headers follow the audited specification rather than the
ticket's legacy names: `SIGN-IN-WITH-X` and `PAYMENT-SIGNATURE` are canonical;
`X-Sign-In-With-X` and `X-402-Payment` are migration aliases Venice accepts but
this client does not emit. No wallet key, signature production, base64 decode,
or USDC transaction construction enters the library.

402 is now `ErrorKind::PaymentRequired`, distinct from 401/403 `Auth`.
`ResponseMetadata` preserves every response header and extracts the three
protocol values callers need: `X-Balance-Remaining`, `PAYMENT-REQUIRED` and
`PAYMENT-RESPONSE`, all exact strings. Response-derived failures carry it, and
`ChatResponse` carries it on buffered success, completed streaming success and
deliberate early stop. The SSE path now buffers a non-2xx body instead of trying
to parse the JSON payment error as events and losing the raw body.

The offline matrix covers all four header modes, empty secrets, per-call
precedence, every current endpoint policy, zero-hit preflight rejection,
buffered/completed/early-stop balance metadata, 402 body/header parity across
both chat paths, case-insensitive lookup, generic parse/HTTP error metadata and
secret non-disclosure. Endpoint coverage remains 4/49, hence a public-API minor
release rather than an endpoint-count change.

**VC-22 (#37) is done** — see v0.12.2. Every buffered endpoint now travels
through one internal request/response substrate instead of the old GET-JSON and
POST-JSON pair. It expresses GET, POST, PATCH and DELETE; caller-supplied
headers; empty or binary-safe bodies; and multipart parts with exact field
names, filenames, media types and bytes. The owned response keeps status,
case-insensitive headers, normalized content type and the byte-exact body, which
is the information VC-23 and the media/account endpoint families need.

The substrate returns transport and cancellation failures immediately but does
not erase an HTTP response merely because its status is non-2xx. Typed decoders
apply status first, then media type: an error body with an unexpected content
type remains the correct HTTP error, while a 2xx JSON response missing a JSON
media type is `ErrorKind::Parse`. `application/json` and structured
`application/*+json` suffixes are accepted case-insensitively with parameters
ignored. The verbatim Content-Type header remains in the retained headers.

Multipart encoding uses cpp-httplib's public multipart API rather than a local
boundary implementation. All multipart operations in the audited contract are
POST, so the internal layer rejects any other method before opening a socket.
`test/06transport/` proves repeated field names, NUL-containing file bytes and
cancellation while a multipart response is stalled; JSON, plain text, CSV and
binary responses prove metadata and exact-byte retention. Existing chat,
models, characters and rate-limit entry points keep their public signatures and
now decode over this substrate. SSE keeps its specialized receiver but shares
transport construction and error helpers.

No supported public C++ API or endpoint coverage changes, hence the patch bump.
Public exposure of payment headers and response metadata landed in VC-23.

**VC-35 (#50) is done** — see v0.12.1. Full API coverage is now an inventory
rather than a hand count: 49 method/path pairs, four implemented and 45 planned,
each assigned to its family issue. The manifest also snapshots effective
security alternatives and request/response media types, so a new operation is
not the only kind of drift the audit can name.

The ticket's own baseline was already stale when implementation began. It
recorded OpenAPI version `20260520.112759`, SHA-256 `e29a…`, and 48 operations.
The official document on 2026-08-10 was version `20260806.142021`, SHA-256
`afb975c4…`, and 49 operations: `GET /billing/usage-history` had joined the
Billing family. That is the audit's first real failure case, not a constructed
fixture. The source and its exact digest now live beside the rows they justify.

Normal builds remain dependency-free beyond the library's existing CMake/C++
requirements. Structural checks are pure CMake and run in ctest; the supplied-
file audit is explicitly maintainer-only, uses a pinned YAML 1.2 parser so
`16:9` stays a string, and never downloads a specification itself.

**VC-20 (#34) is done** — see v0.12.0. Venice reports what it charged, on both
paths, and the library now types it: `ChatResponse::cost`, a `std::optional<Price>`
read from the body's top level and assembled off the streamed chunk the same way
`usage` is.

Measured 2026-08-10, api.venice.ai — the same seven families as VC-17, so the two
tables read as one sweep, and these are the plan's ground truth rather than its
reasoning:

| | cost, non-streaming | cost, streamed | frames |
|---|---|---|---|
| `deepseek-v4-pro` | ✓ | ✓ | 1 |
| `grok-4-5` | ✓ | ✓ | 1 |
| `zai-org-glm-4.7` | ✓ | ✓ | 1 |
| `zai-org-glm-5` | ✓ | ✓ | 1 |
| `llama-3.3-70b` | ✓ | ✓ | 1 |
| `gemini-3-6-flash` | ✓ | ✓ | 1 |
| `qwen3-235b-a22b-thinking-2507` | ✓ | ✓ | 1 |

Six things are worth knowing, five measured rather than reasoned:

- **`usd` is 0 on every capture, and it does not mean free — the ticket's own
  worry, confirmed rather than dismissed.** #34 asked for this to be measured
  before typing it as a number a caller can display. The probe was
  `openai-gpt-55-pro` at $37.50/$225 per million: 1685 prompt + 6 completion
  tokens, a rate-card value of **$0.0645**, and the reply was
  `{"usd":0,"diem":0.0645375}`. `diem` carried exactly that magnitude while
  `usd` reported zero. So `usd` is not populated for this account, an engaged 0
  means "not reported", and both the header and README say to read `diem`. The
  library reports what arrived and interprets nothing.

- **Unlike `Usage`'s optionals, cost is not per-family.** All seven send it on
  both paths, which is why the absence case in `test/07stream/` §3 is labelled
  CONSTRUCTED rather than OBSERVED — and why the break matrix has no control
  family. VC-17's discipline says to confirm a break stays *green* where the
  server sends nothing; there is no such family here, and that is recorded
  rather than skipped.

- **The cost frame carries `"choices": []`, which decides where the read goes.**
  `delta_from_chunk` returns early on an empty choices array, so the read sits
  above it — a read placed below would parse perfectly in every fixture and
  never fire on a live stream. Measured, not reasoned: break B4 moves it below
  and reddens six cases. Exactly one cost frame per stream on every probe, and
  `stream_options.include_usage` changes nothing — cost and usage both arrive
  without it.

- **The tolerance choice is deliberate, and the first comment written to defend
  it was wrong in the way this repo keeps being wrong.** It claimed `cost` was
  "the one field on `ChatResponse` that deviates from the loud-parse rule". It
  is not: `created`, `system_fingerprint` and `venice_parameters` already read
  through `opt_i64` / `opt_string` / `opt_object`, and did before this ticket.
  Caught in review, corrected in all three places it had been copied to, and it
  is the **fifth** time a comment here has justified a design with a constraint
  that does not exist — after VC-08's `json_schema`, VC-04's two, and VC-17's
  `opt_object`. The rule the tolerant read actually bends protects fields with
  *no representation for "unknown"* —
  `prompt_tokens` is `int{0}` — while both members here are `optional<double>`
  whose disengaged state already means unknown. The sharper half of the reason
  is structural and was found by reading rather than assumed: a throw out of
  `StreamAccumulator::ingest` is caught into `parse_err`, which `chat_stream`
  surfaces only when the accumulator is empty, so a loud parse on the streamed
  path is a **half-ingested frame with `on_delta` silently skipped** — not a
  loud failure. Break B7 makes `Price::from_json` loud and reddens five cases
  across two binaries, so the tolerance is load bearing rather than decorative.
  A distinct `Cost` struct was considered and rejected: it would say the
  rate-vs-amount difference in the type system, but two structs differing only
  in a two-line `from_json` is the second convention #34 warned against, and the
  field name plus a header sentence carries it.

- **§10's symmetric blindness reproduced, and there is one narrow mitigation.**
  Break B3 deletes the cost read from both paths: `streamed.cost ==
  non_streamed.cost` comes back **green** (`nullopt == nullopt`, 7 of 8
  assertions passing) and only the sibling assertion pinning the value goes red.
  So a convergence case can pin a leaf *value* as well as an equality — which
  does catch a symmetric loss, but only for a field whose value a fixture can
  name, and not at all for a serialization behaviour. Recorded in AGENTS.md as a
  per-field mitigation, explicitly not a repair.

  Review caught that the mitigation was originally written as a bare
  `streamed.cost->diem == …`, which under the very break it exists to catch is
  `operator->` on a disengaged optional — undefined behaviour, not a red
  assertion, and the first B3 run's "red" rested on it. Guarded now, and the
  same defect was in two §9 cases. A case whose failure mode is undefined proves
  nothing, which makes this a break-matrix lesson rather than a style fix.

- **Nothing in `ctest` covers the smoke binary, and this is the second ticket to
  rely on it.** Break B9 removes the envelope unmodeled-key report and the suite
  stays 13/13 green — confirmed on a genuinely modified tree after a first
  attempt at the break silently failed to apply and produced a meaningless
  green. The counter-measure is VC-17's: `Price::from_json` was pointed at a
  misspelled key, and `--usage llama-3.3-70b` reported `RAW SAYS 0.000000: the
  wire moved and the parse did not` on **both** paths and exited 1. Reverted.

Twelve breaks were run, each reverted. Ten reddened where intended, one is a
build break (removing `Price::operator==` fails to compile §10, which is what
that operator is for), and one — B9 — is the expected green above. B12 came out
of review rather than the plan: putting the cost read back *after* the loud
usage read reddens exactly the new ordering case, so the fix is pinned by the
thing it fixed. B11 is likewise unplanned, added because §4b makes a claim that
needed pinning:
narrowing `opt_double` to `is_number_float()` reddens eight cases across three
binaries, so `is_number()` is load bearing on this path too. Venice sends `usd`
as a JSON **integer**, and a float-only predicate would read every one of them as
absent.

`--usage` grew the envelope report that found this, and it is the transferable
part: `cost` was not deferred, it was **unseen**, because every leg reported the
sub-object it was written for and nothing looked one level up. Its first live run
also named `service_tier` on four families and `kv_transfer_params`,
`prompt_logprobs`, `prompt_text`, `prompt_token_ids` on `llama-3.3-70b`. All
reachable via `ChatResponse::raw`; none typed, none needed yet.

Additive to two public headers and `chat()`/`chat_stream()` return more than they
did, hence the minor bump.

**VC-17 (#28) is done** — see v0.11.1, and the ticket's premise was wrong. It
said `Usage`'s nested detail objects "are modeled and fixture-pinned, but Venice
does not send them". Venice sends both, at exactly the nesting
`Usage::from_json` reads. Measured on 2026-08-09, 21 captures — seven model
families × {non-streaming, streaming, streaming with
`stream_options.include_usage`} — and these are the plan's ground truth rather
than its reasoning:

| | `prompt_tokens_details.cached_tokens` | `completion_tokens_details.reasoning_tokens` |
|---|---|---|
| `deepseek-v4-pro` | ✓ | ✓ |
| `grok-4-5` | ✓ | ✓ |
| `zai-org-glm-4.7` | ✓ | — |
| `zai-org-glm-5` | ✓ | — |
| `llama-3.3-70b` | ✓ | — |
| `gemini-3-6-flash` | — | — |
| `qwen3-235b-a22b-thinking-2507` | — | — |

Five things are worth knowing, four of them measured rather than reasoned:

- **The bug was in how the evidence was gathered, and the repo had already
  written down the rule that would have prevented it.** AGENTS.md has said since
  VC-18 that a leg auto-picking "the first model claiming a capability" must
  name the runners-up. That was written into `--tools` and into no other leg, so
  `--stream` still picked silently — and it picks the first `supportsReasoning`
  entry in the catalogue, which is `gemini-3-6-flash`, one of exactly two
  families in the table above that report neither field. A shape that varies by
  family cannot be settled by a leg that only ever runs one. `pick_by_capability`
  and `report_pick` are that logic factored out so there is one copy to forget.

- **The new `--usage [model]` leg prints the verbatim `usage` object beside the
  typed `Usage`, and the distinction is the entire ticket.** A typed field
  reading absent means either "the server did not send it" or "we are looking in
  the wrong place", and only the raw object separates them. `--stream` said
  `(absent -- check completion_tokens_details nesting)`, which asserts the
  second, and that sentence is what sent this ticket down the wrong path. It now
  says the absence is per-family. The leg reads the streamed frame off
  `acc.chunks()`, so no library change was needed to reach it.

- **The leg is a check, not a printer, and the check was proven live.** A modeled
  key present in the raw object but absent from the typed struct is case (c) in
  #28's own scope — the only one of the three that is a parse bug — and fails the
  run. With `completion_tokens_details` misspelled in `Usage::from_json`,
  `--usage deepseek-v4-pro` reports `RAW SAYS 117: the wire moved and the parse
  did not` on both paths and exits 1. The same break stays **green** on
  `gemini-3-6-flash`, which is the runners-up argument earning itself twice in
  one ticket.

- **A break came back green and the header was wrong about why.**
  `Usage::from_json` described `detail::opt_object` as what stands between a
  `"prompt_tokens_details": null` and a thrown parse. Swapping the predicate for
  a plain `contains()` leaves all 13 test binaries green: nlohmann's `contains`
  answers false for a null or an array exactly as the predicate does. What is
  load bearing is that *some* guard exists — reaching straight through turns the
  null and empty-object shapes red. Corrected in the header, because the old
  wording would have had a reader defend the wrong property. This is the fourth
  time a comment in this repo has justified a design with a constraint that did
  not exist, after VC-08's `json_schema` and VC-04's two.

- **The flat `cached_tokens` key is now the one shape here with no wire behind
  it.** Venice does send a flat sibling — `cache_read_input_tokens` — but it is a
  *different key*, it appears only beside the nested one and never instead of it,
  and it carried the same number in all 21 captures. So it stays untyped (a
  second spelling of one fact is not a fact), and the flat `cached_tokens` read
  stays too, labelled: removing it changes a public parse to buy nothing, and a
  gateway that flattens a details object is the case it was written for. Both
  `test/07stream/` §4 and `test/01client/` now say which of their shapes have
  been observed and which have not.

Three observed shapes are pinned verbatim, including an explicit
`"cached_tokens":0` — `deepseek-v4-pro` and `llama-3.3-70b` both send one on a
cold cache, so absent and zero are different answers on the wire and the
`optional<int>` is load bearing rather than merely tidy. Six breaks were run,
each reverted; five went red where intended and the sixth is the finding above.
Break 5 (remove the nested read, i.e. the pre-VC-05 behaviour) reddens four
assertions including both new live-shape cases, and it is a break that could not
have been demonstrated before this ticket — until §4 carried an observed nested
value, deleting that read reddened only constructed fixtures.

Comments, fixtures and the smoke binary only; no library behaviour changed,
hence the patch bump.

**VC-18 (#29) is done** — see v0.11.0. A key settled what the ticket left open,
and the answer was the third option it did not list: Venice passes Gemini's
`thought_signature` through untouched, and the library dropped it.

Measured against api.venice.ai on 2026-08-09, and these are the plan's ground
truth rather than its reasoning:

- `gemini-3-6-flash` returns the call as `{"id","type","thought_signature",
  "function":{...}}` — the signature is a **sibling of `function`**, not a member
  of it.
- Replayed with it stripped: `HTTP 400 "Function call is missing a
  thought_signature in functionCall parts"`, the ticket's error reproduced.
  Replayed with it echoed: `200`. Necessary *and* sufficient — nothing else about
  the turn had to change.
- `gemini-3-5-flash` splits one tool call across **two** streaming fragments.
  Multi-fragment calls are real, which is what rules out reading the signature
  off `slot.raw`.

Four things are worth knowing:

- **There were two independent losses, so the ticket's "either/or" was a false
  dichotomy.** `ToolCall::to_json` builds a *fresh* object from four modeled keys
  and never consults `t.raw`; and `Message::to_json` assigns `j["tool_calls"]`
  unconditionally, so the documented `m.extra = m.raw` hatch could not have
  rescued it either. `Message::to_json` needed no change in the end — the gap was
  one level down from where #29 guessed. The residual hole is VC-19 (#31).
- **The merge rule is first-non-empty, and one half of it is analogical rather
  than measured.** The signature is not tied to the opening fragment the way
  id/type/name are, so it obeys the same rule those do; the extra guard against a
  *blank* signature on a continuation mirrors the `"name": ""` behaviour already
  documented in that comment block, and no capture has shown it. It is labelled
  as such in the code, and it is the only rule difference a test can discriminate.
- **A generic `ToolCall::extra` was considered and rejected**, not on taste: it
  would not have fixed this bug (the only rule touching unmodeled data across
  fragments is the first-fragment `slot.raw` rule that finding 3 falsifies), and
  no unmodeled key exists today whose loss can be demonstrated — a hatch with no
  failure case is the speculative design this repo refuses.
- **§10's convergence assertion is blind to half the break matrix, and that was
  measured, not assumed.** Eight deliberate breaks were installed one at a time
  against the fixed header and the red sets recorded. Deleting the emit turns
  seven cases red across §2/§6/§9 and leaves §10 **green** — both paths lose the
  key identically and still compare equal. §10 caught only the asymmetric breaks.
  Two breaks exist solely to prove a case is load bearing: emitting the key
  unconditionally is caught by the two no-signature cases and nothing else, and
  the weaker merge guard is caught by the empty-first case and nothing else.
  These cases could not be run red against the pre-VC-18 header — the field does
  not exist there, so they do not compile — and the test file says so rather than
  implying a red run.

`--tools` is now a live regression check rather than a one-family spot check: it
names the runners-up it did not auto-pick (the second-order finding in #29 — the
bug read as a library defect precisely because one model was chosen silently),
reports whether a signature came back, reads the echo off the **serialized body**
rather than the struct (the defect lived downstream of the field), and returns
failure when a signature is seen but not carried back, so a future tolerant model
cannot let this regress behind a green leg two.

Live on three families, all both legs: `gemini-3-6-flash` (signature present, 384
chars, echoed), `gemini-3-5-flash` (no signature), `zai-org-glm-4.7` (no
signature — the proof that other families' bodies did not move). Suite 13/13 on
gcc and clang, 13/13 under ASan/UBSan/TSan, check_artifacts 16/16, consumer
harness 3/3 on both compilers. Additive to a public header, and the wire body
changes for a whole model family, hence the minor bump.

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

- **`Usage::cached_tokens` was always `nullopt` against real Venice** — "very
  likely" when this was written, measured by VC-17 since. It read a *flat*
  `usage.cached_tokens`, a key Venice has never been seen sending, while
  OpenAI-compatible bodies nest it at `prompt_tokens_details.cached_tokens` and
  five of seven model families do exactly that. Both are read now, nested
  winning on disagreement. `reasoning_tokens` was unreachable entirely, which
  made a reasoning model's actual cost invisible on the two families that report
  it.

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

- **A comment justified a design with a constraint that did not exist — and its
  replacement then over-claimed, which took a live run to catch.** `tags` /
  `categories` / `model_id` were comma-joined, on the stated grounds that comma
  "is the one a query string can express without `with_query` growing a
  multimap". False: the owning overload added in the same change takes a
  `vector<pair>` and loops it with no dedup, so `{{"tags","a"},{"tags","b"}}`
  emits `?tags=a&tags=b`. They now repeat the key, and live measurement confirms
  repetition is honoured and means OR — `tags=Buddhism` → 2, `tags=mythology` →
  2, both together → 4, the union.

  **But the reason given for the switch was itself wrong.** The new comment said
  repetition is what lets a value containing a comma survive. It is not:
  `tags=Buddhism%2Cmythology` — one repetition, one encoded comma — also
  returned 4, so the server splits on commas *inside* a value and no client-side
  spelling can express a comma-containing tag. Repetition still stands, on the
  two grounds that survived contact: it is the documented primary form, and it
  does not depend on that splitting behaviour continuing. Corrected in
  `types.hpp`, README and the test's own claim in v0.10.1.

  Twice in one ticket, then, and the second time the replacement rationale went
  out in a release. The lesson is not "check comments" but something narrower:
  a claim about what *the server* does cannot be settled by reading this
  codebase, and writing one down without a measurement is how folklore starts.
  VC-08's `json_schema` finding was the same shape.

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

- **The fixtures were not a capture when this shipped — they have since been
  checked against one, and they hold.** With a key available on 2026-08-09 the
  live payload was compared key-for-key against what `Character` models: the
  union of entry keys across a 50-entry page is exactly the fifteen modeled
  fields, no more and no less, every one present in every entry, and every type
  as the parse expects. Two details vindicate specific choices. `averageRating`
  arrives as **both float and int** across entries, which is why it reads
  through `opt_double` (`is_number()`) — `is_number_float()` would have silently
  dropped every whole rating. `userRating` is `null` on every entry, matching
  the nullable-means-absent reading. The envelope is `{data, object}` with no
  total, as the pagination design assumed, and `50 usable of 50 returned`
  settles both open guesses at once: the default page really is 50, and `slug`
  really is always present. The query half was exercised too — `limit=3`/`100`
  honoured, `limit=5000` a clean 400, `offset` paging with zero overlap,
  `search` and `isWebEnabled` filtering correctly, and a bogus `sortBy` a 400,
  which is the "the value set belongs to Venice" decision earning itself.
  The original wording follows, because it is what was true at release:

  **The fixtures are not a capture, and this time the endpoint said so out
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

Three things were left out on purpose. `GET /characters/{slug}` was the same
struct behind a different path and is now implemented by VC-16 (#26). The
README's FetchContent `GIT_TAG` was bumped by hand again, which is the drive-by
VC-12 (#17) exists to abolish. The 402 ambiguity described by this historical
entry is resolved in v0.13.0: it is `PaymentRequired`, while 401/403 remain
`Auth`, and response headers are preserved for the caller.

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
