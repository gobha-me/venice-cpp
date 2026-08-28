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
  now fails the build if that recurs. Rule B6 likewise keeps README's
  `FetchContent` release pin from falling behind the newest reachable tag.
- cpp-httplib API notes (v0.18.x): `Request` has `body` + `set_header()` for
  content type (no `set_content`, no `content_type_`); `send(req,res,err)`
  returns `bool`. These bit once — check the vendored header before assuming.
- **Buffered HTTP goes through `detail::send_buffered`; endpoint methods do not
  grow their own verb/content helpers.** Its response owns status, headers,
  normalized media type and byte-exact body. Non-2xx status is classified before
  success-media validation; a wrong/missing media type on a 2xx is `Parse`.
  Multipart uses cpp-httplib's public encoder and is POST-only because every
  multipart operation in the audited contract is POST. SSE remains specialized
  but shares transport construction and error helpers.
- **Redirects are untrusted responses, not transport instructions.** Venice's
  audited contract publishes no 3xx response, so the shared transport never
  follows `Location`, even on the same origin. A 3xx is `ErrorKind::Http` with
  its exact status, body, headers and `ResponseMetadata`. This stronger rule is
  shared by buffered JSON/binary/multipart, Chat SSE and streamed speech; it
  prevents valid credentials, payment proofs, idempotency keys and request
  bodies from being replayed across origins or onto a downgraded scheme (VC-47).

## Conventions that matter

- **Errors:** `std::expected<T, venice::Error>` everywhere fallible. Never throw
  across the public API; a transport/parse/HTTP failure is a value the caller
  inspects. Error kinds: network / http / parse / auth / rate_limited /
  payment_required / invalid_arg / cancelled, each carrying `status` + raw
  `body`. A response-derived error also carries `ResponseMetadata`; 402 is
  `PaymentRequired`, while 401/403 remain `Auth`. `cancelled` is
  deliberately not folded into `network` (VC-06): a dead network is a fault to
  report or retry, a cancellation is the caller's own decision arriving back at
  them, and collapsing the two leaves the difference reachable only by parsing
  message text.
- **Per-call concerns go in `RequestOptions`, not on the request.** Timeout
  overrides, the cancel token and an authentication override are a property of
  *this call* — never serialized, and applying identically to `models()` and
  `balance()`, which have no request object at all. Same reasoning that keeps `stream` off
  `ChatRequest`. Every member of `RequestOptions` carries an explicit default
  initializer even where the type already default-constructs empty: without
  them `-Wmissing-field-initializers` fires once per omitted member on the
  designated-initializer spelling the type exists for, i.e. on every correct
  caller.
- **Authentication is explicit transport state.** `Authentication` distinguishes
  Public, Bearer, pre-signed SIWX and pre-built x402 payment payloads. The
  compatible `Client(std::string)` constructor means Bearer even when the
  string is empty; empty credentials and endpoint/mode mismatches are
  `InvalidArg` before a socket. `RequestOptions::authentication` overrides the
  client default for one call. Emit the audited canonical headers
  (`Authorization: Bearer`, `SIGN-IN-WITH-X`, `PAYMENT-SIGNATURE`), not Venice's
  legacy migration aliases. This library does not own wallet keys, sign SIWX,
  decode payment requirements, or construct USDC transactions.
- **Response protocol metadata stays owned and exact.** `ResponseMetadata`
  preserves all response headers and extracts `X-Balance-Remaining`,
  `PAYMENT-REQUIRED` and `PAYMENT-RESPONSE` as strings. It is attached to every
  response-derived `Error` and to `ChatResponse` on buffered, completed-stream
  and deliberate early-stop success paths. Do not parse a decimal balance into
  binary floating point or decode/sign an opaque payment envelope here.
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
- **Chat stream options do not own the stream bit.** `ChatRequest::stream_options`
  is emitted only by `chat_stream()`; buffered `chat()` erases even an
  `extra["stream_options"]`, while the streaming path preserves an extra-supplied
  object only when the typed member is unset. `ChatResponse::choices` exposes
  every choice; legacy message/content/finish fields remain first-choice
  conveniences. `StreamDelta::logprobs` borrows the current choice fragment;
  the accumulator does not invent a merge for provider-shaped logprobs whose
  published schema defines no cross-frame composition.
- **Range checking: none, deliberately — representability checking: yes.**
  Structural preconditions that make a request unsendable by construction are
  `ErrorKind::InvalidArg`, checked in `Client::validate` before any socket: an
  empty model, no messages, and a non-finite `temperature` / `top_p` /
  `frequency_penalty` / `presence_penalty` / `max_temp` / `min_p` / `min_temp` /
  `repetition_penalty`. NaN and ±inf are not a range opinion
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
  re-serializes what the server *sent* — with the corollary VC-18 paid for: a
  freshly built object serializes exactly what it models and drops everything
  else, so any server-sent key a model *requires back* has to be modeled. That
  was `thought_signature`, dropped silently until a live 400 found it. It would also break the escape hatch
  rather than merely omit it: a typed element leaves only `extra["tools"]`, which
  *loses* whenever `tools` is engaged, so the hatch's behaviour would flip on an
  unrelated field. The container stays typed
  (`optional<vector<nlohmann::json>>`) so engaged-but-empty can emit `[]`.
  Message content and Responses input/output follow the same rule: item
  universes remain raw JSON, while builders cover the documented text, image,
  audio, video, file, cache-control, function-call and reference shapes.
- **Embedding input follows that same raw-with-builders rule; embedding output
  does not collapse two wire shapes.** `EmbeddingRequest::input` is raw json,
  with `embedding_input::{text,texts,tokens,token_batches}` for the documented
  forms. The client rejects only missing/empty required structure and leaves
  element kinds, counts, dimensions, model ids and encoding names to the server,
  so the raw escape hatch remains an escape hatch. On the response side,
  `EmbeddingValue` distinguishes `vector<double>` from opaque base64 even
  though the 20260811 OpenAPI 200 schema describes only the numeric array while
  the request explicitly offers `encoding_format: base64`. Indices, vector
  elements and usage counts parse loudly: a tolerant zero there would silently
  corrupt ordering, search data or accounting. Base64 is never decoded because
  Venice specifies neither element width nor byte order.
- **Image generation has two request contracts and a response union.**
  `/image/generate` and `/images/generations` remain distinct public request
  types: compatibility fields that one endpoint accepts must not look usable on
  the other. Native generation selects typed JSON versus owned JPEG/PNG/WebP
  bytes from the successful response's actual normalized `Content-Type`, never
  from `return_binary` or the requested format. Non-2xx classification still
  happens before media validation. The client preserves bytes and metadata but
  never decodes, saves, displays or silently base64-expands an image.
- **Image transformations choose their media form from an explicit input.**
  `ImageInput` distinguishes inline encoded text, a URL and owned file bytes;
  no prefix heuristic decides whether a string becomes multipart. Upscale,
  edit, multi-edit and background removal keep distinct request types and
  literal wire spellings (`model` is not derived into multi-edit's `modelId`).
  A file selects multipart and JSON `extra` is then rejected rather than
  silently lost. Multi-edit accepts an ordered all-file list or an ordered JSON
  list of inline/URL values, never a mixed request. Its maximum is model-owned:
  the 2026-08-16 catalogue reports six on several inpaint models, so the old
  three-image limit is not a client guard. All four results are actual media
  bytes plus normalized content type and metadata; no decode or filesystem I/O.
- **Image request policy comes from the catalogue, not duplicated guards.**
  Formats, qualities, resolutions, aspect ratios, steps, dimensions and style
  limits remain caller-supplied values. The client rejects only empty required
  structure and non-finite modeled doubles; `Model::image` carries the live
  constraints a caller can consult, while `Model::inpaint` carries edit and
  multi-edit constraints. `style_references` is optional so omitted
  and explicitly empty remain distinguishable, and its elements use the same
  modeled-wins `extra` merge as the containing request.
- **Audio has three workflows, and none is a hint for another.** Speech is a
  JSON request whose selected method owns buffered versus streamed delivery;
  transcription and voice cloning are multipart uploads selected by an
  explicit owned `AudioFile`; music generation is an explicit
  quote/queue/retrieve/cleanup sequence. Successful response `Content-Type`
  selects JSON, text or owned media bytes. Non-2xx status is classified before
  media validation, and the client never decodes, plays or writes those bytes.
  On streamed speech, callback `false` is deliberate early success while a
  cancel token remains `ErrorKind::Cancelled` and wins a race between them.
- **Audio policy belongs to the server and catalogue.** Formats, voices,
  durations, speeds and language codes are caller-supplied values; local guards
  reject only missing required structure and non-finite modeled doubles.
  `MusicModelSpec` exposes the live music constraints, while ASR currently has
  no distinct live metadata shape and therefore does not grow an empty typed
  view. Retrieve returns a processing/media union from the actual response, a
  cleanup body with `success: false` remains a successful retryable result, and
  no method hides polling, cleanup or remote deletion from the caller.
- **Video is an explicit paid-work lifecycle, never a convenience loop.** Quote,
  queue, retrieve and cleanup remain separate calls; URL transcription is a
  fifth independent operation. Queueing can spend funds and cleanup deletes
  remote state, so no helper polls, queues or cleans up implicitly. Successful
  retrieval is processing JSON or owned MP4 bytes selected from actual
  normalized `Content-Type`; transcription is typed JSON or exact plain text.
  Provider-shaped `elements`, keyframes and legal `consents` stay raw JSON, and
  a smoke leg may consult the catalogue and quote but must never enqueue work.
- **Augment returns source material directly; it is not a chat flag.** Document
  parsing owns multipart bytes and selects typed JSON versus exact text from the
  actual successful Content-Type. Scrape and search keep separate request and
  response types, ordered tolerant result fields and verbatim provider objects.
  Provider names, limits, response formats and URL policy stay server-owned.
  Never print uploaded or returned content in the live leg, and describe
  Venice's retention statement as server behavior rather than a local
  secure-erasure guarantee.
- **Responses is Alpha, stateless and non-streaming in this public API.**
  `create_response()` forces `stream=false` because the audited document
  publishes no SSE event schema. Input/output item collections remain raw and
  unknown items survive; status, usage, error, function-call and citation views
  are typed. Explicit E2EE enablement is `InvalidArg`, and the client never
  silently reroutes to Chat Completions.
- **Crypto RPC keeps the method universe raw and the transport layers distinct.**
  Network discovery is a tolerant public listing; proxy request params and
  result/error payloads remain raw JSON with small JSON-RPC 2.0 builders.
  HTTP-200 `error` members are successful proxy transport results, while
  non-2xx status remains `venice::Error`. Network slugs are open strings encoded
  as one path segment. `Idempotency-Key` is per-call header state in
  `RequestOptions`, never request JSON or diagnostics; response IDs, batch order,
  charge headers and x402 metadata stay exact. The client hardcodes neither
  supported networks/methods nor Venice's batch and idempotency policy.
- **x402 wallet operations transport proofs; they never own them.** Balance and
  transaction history require caller-produced SIWX for the wallet encoded in
  the path; top-up accepts Public discovery or a caller-produced payment
  signature. An empty public POST intentionally returns 402 requirements as a
  typed success, while every unrelated 402 remains `PaymentRequired` and every
  other non-2xx is classified before media validation. Canonical headers are
  `SIGN-IN-WITH-X`, `PAYMENT-SIGNATURE`, `PAYMENT-REQUIRED` and
  `PAYMENT-RESPONSE`; legacy migration aliases are never emitted. Base-unit
  amounts remain strings, response money follows the wire's JSON numbers, and
  the library never signs, decodes opaque payment envelopes, constructs USDC
  transactions, polls or retries a payment. A committed live leg may discover
  requirements but must never submit a payment or expose a proof.
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

  **The boundary that rule is actually drawing.** The sentence above overstates
  it, and VC-20 is where that showed: `ChatResponse` has read `created`,
  `system_fingerprint` and `venice_parameters` through `opt_i64` / `opt_string`
  / `opt_object` since before VC-20, so "do not retrofit onto `ChatResponse`"
  was never true of the struct as a whole. What makes tolerance wrong for
  `Usage` is not that it is a response: it is that `Usage::prompt_tokens` is
  `int{0}` and so has **no representation for "unknown"** — tolerance there maps
  corrupt onto a number the caller cannot tell from a real one. Where a field's
  disengaged state already means unknown, and callers already branch on it,
  tolerance maps corrupt onto a state they have written code for. That is the
  line. `ChatResponse::cost` (VC-20) is `optional<Price>` of `optional<double>`
  and sits on the tolerant side of it; `Usage`'s counts sit on the loud side.
  Two further consequences of a loud read, both checkable: it would turn a
  metadata field into `ErrorKind::Parse` for a completion already paid for; and
  on the streamed path `chat_stream`'s SSE lambda catches the throw into
  `parse_err`, which is surfaced only when the accumulator is empty — so a loud
  parse there is a half-ingested frame with `on_delta` silently skipped, not a
  loud failure. `test/07stream/` §3's wrong-typed-cost case is where the choice
  is pinned.

  (Noted while measuring that: `StreamAccumulator::ingest`'s
  `d.usage->get<Usage>()` has the same exposure today — a wrong-typed
  `prompt_tokens` on a streamed usage frame half-ingests and reports nothing,
  where the non-streamed path fails loudly on the same body. A streamed/
  non-streamed asymmetry in `Usage`, and its own ticket.)
- **The `data` envelope fallback is only safe when the inner container's JSON
  type differs from the envelope's.** Three parsers in `types.hpp` open with
  `const auto* data = opt_array(j, "data"); const json& arr = data ? *data : j;`
  — tolerating a body that is a bare list rather than a wrapped one. That works
  there for a reason easy to mistake for a style: they demand an **array** while
  the envelope is an **object**, so the fallback is type-disjoint and can only
  ever fire on a body that really is a bare list. VC-38's two operations wrap a
  string→string **object** in an object, where the same idiom becomes
  type-*indistinguishable*: `{"object":"list","type":"text"}` would parse into a
  two-entry map with keys `object` and `type` and **report success**. So
  `string_map_envelope_from_json_body` requires `data` and requires it to be an
  object, and `test/09catalogue/` pins that exact body as a throw with a comment
  saying it is the only case in the suite that a reintroduced fallback would
  turn red. Before copying the idiom to a fourth parser, check the two types.
- **Endpoint filters are caller-supplied strings, and an unset one sends no
  query key.** `models(type)` takes Venice's modality as a string rather than an
  enum, on the same reasoning as `response_format`: the value set is the
  server's, and a list hardcoded here would refuse a valid value the day a
  modality is added. VC-38 turned that from a principle into a measurement: on
  2026-08-11 `/models/traits?type=all` answered 200 and
  `/models/compatibility_mapping?type=all` answered 400, **despite byte-identical
  `parameters` blocks in Venice's own OpenAPI document** — whose request enum
  omits `all` and `code` for both, wrongly for the first and rightly for the
  second. A validated set in this header would have had to encode a divergence
  the specification itself gets wrong. The server's 400 also names the values it
  accepts, verbatim, in `Error::body`; no local `InvalidArg` could. `venice::detail::with_query` in `client.hpp` **skips any
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
- **Wire spelling is per-modality, and is never derived.** The same concept in
  the same wire position arrives as `aspectRatios` on image and inpaint models
  and `aspect_ratios` on video ones, `promptCharacterLimit` beside
  `prompt_character_limit` — measured 2026-08-11, on 111 and 37 live entries
  respectively. A mechanical camelCase→snake_case rule would read one family as
  absent on every entry, forever, and nothing but a verbatim capture would say
  so. Key tables carry literal strings, and `test/10modalities/` §5 pins each
  family as **deaf to the other's spelling** rather than merely fluent in its
  own — the one-directional version of that check passes with either table
  wrong.
- **An empty list equals an absent one only where measurement says so, and the
  reasoning goes next to the field.** `Model::traits` is a plain vector because
  no caller branches on absent-vs-empty; `VideoConstraints::aspect_ratios` is
  an `optional<vector>` because 40 of 111 live video models send `[]` and the
  specification defines that as "no defined aspect ratio", which a request
  builder must tell from silence. Two calls, opposite ways, on adjacent fields
  — so each comment names the other. `detail::string_array` now delegates to
  `opt_string_array` so the two cannot drift.
- **A key the document does not have is not thereby absent from the wire, and a
  key it does have is not thereby present.** VC-39 measured seven keys on 100%
  of video models that appear nowhere in the swagger fetched the same day, and
  four documented keys that have never been sent. Model what the wire carries,
  record per field which of the two it is, and leave the never-observed in
  `raw`. What turns that from a bet into a check is the live leg's
  set-difference **per nesting level** — a new server key surfaces as unmodeled
  rather than as silence — plus a raw↔typed reconciliation in both directions,
  which is where a read at the wrong nesting level shows up (VC-37's bug, in
  the shape it would take on a struct rather than an envelope).
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
- **Both hatches reach message-level keys only.** `tool_calls` is modeled, so it
  is assigned over whatever the seed put there — `m.extra = m.raw` cannot rescue
  an unmodeled key *inside* a tool call, and `ToolCall` has `raw` but no `extra`
  of its own. VC-18 is the proof and VC-19 (#31) is the ticket; the limit is
  pinned by a §0 case in `test/07stream/` rather than left implicit, so removing
  it means inverting that case deliberately.
- **`Message::from_json` is total; `ChatResponse::from_json_body` is loud. Do
  not unify them.** A message sub-object's shape legitimately varies — content
  string/null/absent/parts-array, tool_calls present or not — and the old
  two-key parse threw `type_error.302` on
  `{"role":"assistant","content":null,"tool_calls":[...]}`, the canonical
  tool-call reply, so `Message` was not usable as a parse target at all. A
  completion body's *top-level* shape is a contract: `j.at("choices")` must keep
  throwing, because that is what `Client::chat`'s try/catch turns into
  `ErrorKind::Parse` and `test/01client/` pins it.
- **Usage/cost metadata:** keep cache buckets distinct (cached-read,
  cache-write and uncached
  tokens price differently — see venice-cli #75). Model *pricing* has two cache
  buckets (`cache_input` read, `cache_write` write), and `Usage` now preserves
  both independently rather than pairing or collapsing them.
  **All three of `Usage`'s optional fields are per-family, and absent is not zero.**
  VC-17 measured seven families: five send
  `prompt_tokens_details.cached_tokens`, two of those also send
  `completion_tokens_details.reasoning_tokens`, and two send neither while both
  claiming `supportsReasoning`. An explicit `"cached_tokens":0` also arrives, on
  a cold cache, so a disengaged optional means "this family does not say" and
  collapsing the two loses a real distinction. Cost estimates treat absent as
  unknown. `venice-cpp --usage <model>` is how you find out for a given family,
  and it is the check that a nesting has not moved.

  **What the server charged is `ChatResponse::cost`, a top-level sibling of
  `usage` typed `optional<Price>` (VC-20).** It is authoritative where the
  rate-card product above is a reconstruction. Unlike `Usage`'s optionals it is
  **not** per-family — measured 2026-08-10, all seven families send it on both
  paths, one frame per stream. Two things not to get wrong: **an engaged `usd`
  of `0` is a value the server sent, not a claim the call was free** — it has
  been `0` on every capture including one whose rate-card value was $0.0645, so
  it means "not reported for this account" and `diem` is the number that
  answers; and `cost` lives on `ChatResponse`, not on `Usage`, because
  `Usage::from_json` only ever receives the `usage` sub-object and structurally
  cannot reach a sibling.
- **Billing is account state, not the historical `balance()` rate-limit call.**
  The three account methods keep `billing_` in their public names and Venice
  requires their Bearer token to be an admin API key; a valid ordinary inference
  key returns 401 with `Admin API key required`. JSON money remains
  `optional<double>` because the wire publishes numbers: it preserves absent
  versus zero but promises approximate arithmetic, not decimal-ledger equality.
  Usage-history CSV is the exact export and stays byte-for-byte with its
  continuation and disposition headers. A cursor is an exclusive continuation
  input — never combine it with first-page filters — and actual successful
  `Content-Type`, not the requested format, selects the JSON/CSV result union
  Analytics' four daily chart families stay raw objects because their keys are
  account-defined display names; `byModelDailyUsd` and `byKeyDailyUsd` were
  present live on 2026-08-16 but absent from that day's OpenAPI document
  (VC-42).
- **API-key administration never turns a returned key into diagnostics.**
  `ApiKeyCreated::api_key` is the one public field that can contain complete
  one-time credential material. Its `raw` tree and create-error bodies replace
  every nested `apiKey` value with `[REDACTED]`; it has no display/stream helper and the
  committed `--api-keys` leg is read-only: list/detail expose only Venice's
  last-six suffix, while create/update/delete stay in offline loopback tests.
  API-key usage totals remain decimal strings because that is the wire's exact
  accounting representation; configured limits are optional JSON numbers, so
  absent and zero remain distinct. Type, limit-period and rate-limit values are
  server-owned strings. `balance()` keeps its historical `expected<json,
  Error>` source contract by returning `api_key_rate_limits()`'s retained raw
  envelope rather than preserving a second parser (VC-43).
- **Web3 API-key proof is public body authentication, not SIWX.**
  `web3_api_key_challenge()` requires Public transport state and returns its
  token only through the typed field; `create_web3_api_key()` accepts a
  caller-produced address, signature and token in JSON and reuses the one-time
  `ApiKeyCreated` result. Bearer, SIWX and x402 modes fail before a socket unless
  the call explicitly overrides to Public, so unrelated client credentials do
  not ride along to an endpoint whose audited security declaration is empty.
  The client never owns a wallet key, signs or verifies the proof, or adds a
  crypto dependency. Retained Web3 trees and errors redact `token`, `signature`
  and `apiKey`; unsafe non-JSON bodies become a redacted marker. There is no CLI
  leg because GET returns proof material and POST creates a credential (VC-44).
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

**A live smoke leg that auto-picks "the first model claiming a capability" must
name the alternates it did not pick.** VC-18 read as a library bug for as long as
it did because `--tools` silently chose one model and the screen gave no reason
to suspect the next one would answer differently — it did, and the same assembled
turn that a Gemini model rejected was accepted by glm. Server behaviour that
varies by model family is invisible to a leg that only ever runs one, so printing
the runners-up is part of the check, not decoration.

That rule was written here after VC-18 and put into `--tools` only, and VC-17 is
what it cost: `--stream` still picked silently, so #28 was filed claiming Venice
never sends `Usage`'s detail objects, on a run against one of the two families
in seven that do not. **The rule now lives in one place** —
`pick_by_capability` / `report_pick` in `src/bin/main.cpp` — because a
convention re-implemented per leg is a convention three legs are exempt from. A
new leg picks through those or explains why not.

**A live leg reports the verbatim server object, not only what parsed out of
it.** The corollary of the above, and the other half of VC-17. A typed field
reading absent means either "the server did not send it" or "we are looking in
the wrong place"; `--stream` printed `(absent -- check the nesting)`, which
asserts the second, and no fixture could contradict it. `--usage` prints the raw
`usage` beside the parse and fails when a modeled key is in one and not the
other, which turns the question into a check rather than a judgement call.

**And it reports the verbatim ENVELOPE, not only the sub-object it was written
for.** VC-20's finding rather than its subject. `--usage` named the unmodeled
keys inside `usage` and nothing ever looked one level up, so `cost` — a sibling
of `usage`, carrying what the call actually charged — rode untyped for three
releases with no leg positioned to see it. The rule is mechanical now:
`kModeledBodyKeys` beside `kModeledUsageKeys` in `src/bin/main.cpp`, one
set-difference per level. Its first live run named `service_tier` on four
families and four more keys on `llama-3.3-70b`. A leg that reports a sub-object
reports the object it came out of too.

**Where the per-level rule stops, it says so and puts something in its place.**
VC-38's `--traits` and `--compat` are the one exception, and the exception has
to be visible or the next reader takes it for an oversight. Every key inside
those responses' `data` is caller-unknown *by construction* — they are trait
names and foreign vendor model ids, the server's data rather than this client's
schema — so a set-difference one level down would print the whole payload on
every run and mean nothing. What replaces it detects the same class of defect:
`returned`, the count the server sent, against the number of entries that
parsed. A value this client could not read shows up as a gap between the two,
and the leg then names the offending keys. A level whose keys you do not model
is not a level you stop checking; it is a level that needs a different check.

The first version of that check compared three numbers, and the third was
`raw["data"]`'s own member count — which is where `returned` is computed from,
so it was the parser checked against itself and could not fail on any server
response. Worth stating because the mistake is an easy one to repeat under the
name of thoroughness: **a reconciliation is only a check if the two numbers have
independent origins.** Re-deriving a value from the same field the parser read
and comparing them tests nothing the offline suite does not already pin
absolutely.

**A fixture written from the spec cannot check the reading of the spec.** VC-37
is the whole argument. `Client::character(slug)` shipped in v0.14.0 returning a
`Character` with every field absent, because `/characters/{slug}` answers with
an envelope — `{"data": {...}, "object": "character"}` — and the parse read the
envelope as the character. The offline suite was green throughout, and could
not have been anything else: the fixtures were built from the same OpenAPI
document by the same reader, who took `properties.data.properties` for the body
rather than for the body's `data` member. Parser and fixture shared one wrong
premise and agreed perfectly with each other.

No amount of offline coverage fixes that, because every case in the file is
downstream of the premise. Only the wire disagrees. So a spec-derived fixture
is a *hypothesis about the wire*, and the file that holds it says so out loud
(`test/08characters/` opens with exactly that caveat) until a capture replaces
it. When the capture arrives, pin it beside the derived cases rather than
instead of them — the derived ones still cover shapes the capture does not
happen to contain.

The corollary for the live legs: **write the check absolutely, not relative to
what the response happens to contain.** `--character`'s guard asked whether
raw's `slug` *disagreed* with the typed one. The envelope has no top-level
`slug`, so the comparison was skipped and the leg passed while every field on
screen was blank. It could only fire when the parse was nearly right. "A 200
that parsed no slug at all has failed" is the version that catches both.

**A convergence assertion cannot see a symmetric loss.** `test/07stream/`'s §10
compares the streamed and non-streamed turns and is the file's payoff — and
measured while running VC-18's break matrix, deleting the `thought_signature`
emit leaves it *green*, because both sides lose the key identically and still
compare equal. It caught only the breaks where one path kept the field and the
other did not. Convergence is evidence about the stream's plumbing, never about
serialization; that has to be pinned upstream or it is not pinned at all.

VC-20 measured the same blindness a second time and found one narrow mitigation.
Deleting the `cost` read from *both* `from_json_body` and `delta_from_chunk`
leaves §10's `streamed.cost == non_streamed.cost` **green** — `nullopt ==
nullopt` — and only the sibling assertion pinning `cost->diem` against a literal
goes red. So a convergence case can also pin a leaf **value**, which does catch
a symmetric loss. That works only for a field whose value the fixture can name,
and not at all for a serialization behaviour with no value to assert. It is a
per-field mitigation, not a repair: anything whose loss reads as "both sides
emit nothing" is still invisible in §10.

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

- **A documented snippet that compiles is not a documented snippet that works.**
  README blocks and header comment snippets are never built by anything, which
  cost VC-08 a `tools` example that did not compile. Paste them into a throwaway
  TU against the real headers before claiming they work — and then look at
  lifetimes by hand, because the compiler will not. VC-04 found
  `for (const auto& x : *client.models())` in README, clean on both compilers
  and a `stack-use-after-scope` under ASan: the `expected` temporary dies at the
  end of the range-for's initializer, and P2718R0 fixes that in GCC 15 / Clang
  19 while this project supports GCC 13+. Bind the result to a named variable.
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
  dirs exist (B4), fixture scripts keep mode 100755 (B5), and README's
  `FetchContent` tag is not older than the newest reachable release (B6). Run it
  directly with `cmake -P`. If a rule fires on something legitimate, fix the
  citation rather than loosening the rule — a rule that matches nothing passes
  everything.
- Build dirs (`build*/`) are gitignored — don't commit them.
