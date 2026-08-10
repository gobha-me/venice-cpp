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

- **Chat completions** (`/chat/completions`) — non-streaming, and streaming in
  three forms: a content-text callback, or a `StreamAccumulator` you own that
  assembles the reply and survives a cancel.
- **A reply is a `Message`, and a `Message` is what you send.** The whole
  assistant turn round-trips — `reasoning_content` (so a reasoning model's
  thinking can be fed back), `tool_calls`, `tool_call_id`, `refusal`, multimodal
  content parts — and every field can be individually **withheld**, because an
  unset one is erased from the body rather than falling back to what the server
  sent.
- **Sampling and control parameters** — `temperature`, `top_p`, `max_tokens`,
  `stop`, `frequency_penalty`, `presence_penalty`, `seed`, `response_format`,
  plus a top-level `extra` passthrough for keys this struct doesn't model.
- **Tool / function calling, both directions** — declare functions with `tools`,
  steer with `tool_choice` and `parallel_tool_calls`; read the model's request
  back as typed `ToolCall`s and answer it with `Message::tool()`. Tool-call
  argument fragments are merged across a stream by index, never by position, and
  the opaque `thought_signature` some families require echoed on the next turn
  is carried back for you.
- **`venice_parameters`** extension — web search, citations, character,
  thinking toggles, with forward-compatible passthrough for unmodeled keys.
- **Models list** (`/models`) — typed metadata per model: context window,
  fourteen capability flags (function calling, vision, reasoning, web search,
  …), and a full rate card with cache buckets and extended-context tiers, plus
  the verbatim entry as `raw`. Filterable by modality — `models("image")`,
  `models("all")` — since the endpoint's own default is text-only.
- **Characters list** (`/characters`) — the discovery half of
  `venice_parameters.character_slug`: slug, name, description, tags, the model
  a character runs on, and its rating stats, plus the verbatim entry as `raw`.
  Filterable and, because the endpoint pages at 50, pageable.
- **Explicit authentication** — Public, Bearer, pre-signed SIWX and pre-built
  x402 payment payloads are distinct modes, selectable per client or per call.
  The client never owns wallet private keys or constructs signatures.
- **Balance / rate limits** (`/api_keys/rate_limits`).
- **Per-request timeouts and cancellation** — every call takes an optional
  `RequestOptions` with connect/read/write timeout overrides and a
  `CancelToken` that aborts an in-flight request from another thread, including
  one that has received nothing at all, plus an optional authentication
  override for that call.
- **Error model** — `std::expected<T, venice::Error>`; network / HTTP / parse /
  auth / payment-required / rate-limit / invalid-arg / cancelled. Response
  failures carry status, raw body and response metadata, including x402 headers.

Later phases (fed by real use): image/audio/video, TTS, embeddings,
retries/backoff, async.

## OpenAPI coverage

OpenAPI coverage: 4/49 operations implemented.

The checked inventory is [`cmake/openapi_manifest.json`](cmake/openapi_manifest.json),
keyed by HTTP method + path rather than `operationId` (the published
`GET /image/styles` operation has none). It records which family issue owns
every planned operation and snapshots the effective authentication and request/
response media contracts. The dependency-free CMake self-test checks the
manifest's structure and this coverage line during every normal test run.

To compare it with a newly downloaded Venice specification, use the
maintainer-only audit tool. Its YAML parser lives in an isolated environment and
is not a library, build, test, or consumer dependency:

```bash
python3 -m venv build-openapi-audit
build-openapi-audit/bin/pip install -r tools/openapi_audit_requirements.txt
curl -fsSL https://api.venice.ai/doc/api/swagger.yaml -o /tmp/venice-openapi.yaml
build-openapi-audit/bin/python tools/openapi_audit.py /tmp/venice-openapi.yaml
```

The tool itself performs no network access. It returns 0 only when the supplied
document's version, SHA-256, operations, effective security alternatives, and
request/response media types match the audited manifest; drift returns 1 and an
invalid or unsupported input returns 2.

## Dependencies

Header-only library; consumers link the CMake target and get everything
transitively:

- [cpp-httplib](https://github.com/yhirose/cpp-httplib) — HTTP transport (header-only)
- [nlohmann/json](https://github.com/nlohmann/json) — JSON (header-only)
- **OpenSSL** — TLS (the one link-time dependency; reimplementing TLS is
  malpractice, so we link it)

## Usage

```cpp
#include <cstdlib>
#include <string>
#include <vector>

#include <venice/venice.hpp>

const char* api_key = std::getenv("VENICE_API_KEY");
venice::Client client{
    venice::Authentication::bearer(api_key == nullptr ? "" : api_key)};

venice::ChatRequest req;
req.model = "llama-3.3-70b";
req.messages = {venice::Message::user("Hello")};

// sampling controls — every one is optional; unset fields are never serialized
req.temperature = 0.7;
req.top_p = 0.9;
req.stop = std::vector<std::string>{"\n\n"};
req.seed = 42;
req.response_format = venice::response_format::json_object();

// offering functions — see "Declaring tools" below
req.tools = std::vector<nlohmann::json>{venice::tools::function("get_weather")};
req.tool_choice = venice::tool_choice::automatic();

// non-streaming
if (auto res = client.chat(req)) {
  // res->content, res->usage, res->finish_reason
} else {
  // res.error().kind, .status, .message, .body
}

// streaming (SSE): on_token returns false to stop early
auto s = client.chat_stream(req, [](std::string_view delta) {
  // print/handle delta
  return true;
});

// streaming, structured: you supply the storage, so nothing is dropped
venice::StreamAccumulator acc;
auto s2 = client.chat_stream(req, acc, [](const venice::StreamDelta& d) {
  if (d.reasoning_content) render_thinking(*d.reasoning_content);
  if (d.content)           render_answer(*d.content);
  return true;
});
// acc.message() is the whole assistant turn; acc.chunks() is every frame verbatim
```

### Authentication and x402 metadata

Authentication is transport state, not request JSON. The string constructor
remains the compatible Bearer spelling; the explicit form reaches public and
wallet-authenticated calls without ever sending `Authorization: Bearer `:

```cpp
venice::Client public_client{venice::Authentication::public_access()};
const auto public_models = public_client.models();

const std::string signed_siwx = "base64 payload signed outside venice-cpp";
venice::Client wallet_client{
    venice::Authentication::sign_in_with_x(signed_siwx)};

// One call may override its client's default without changing request JSON.
const auto wallet_reply = client.chat(
    req, {.authentication = venice::Authentication::sign_in_with_x(signed_siwx)});
```

Venice's current canonical wallet header is `SIGN-IN-WITH-X`; the library does
not emit its migration alias `X-Sign-In-With-X`. Likewise,
`Authentication::x402_payment(payload)` emits the canonical
`PAYMENT-SIGNATURE`, not the legacy `X-402-Payment` spelling. That mode is the
transport foundation for the x402 top-up endpoint; none of the four endpoints
currently exposed accepts a payment signature.

Successful SIWX inference exposes the balance string exactly as Venice sent it.
A 402 is distinct from bad credentials and retains both the JSON body and the
base64 payment-requirements header:

```cpp
if (wallet_reply) {
  if (wallet_reply->metadata.x_balance_remaining) {
    const std::string& balance = *wallet_reply->metadata.x_balance_remaining;
    // Display or parse with the decimal policy appropriate to your application.
  }
} else if (wallet_reply.error().kind == venice::ErrorKind::PaymentRequired) {
  const std::string& body = wallet_reply.error().body;
  const auto& requirement = wallet_reply.error().metadata.payment_required;
  // Decode/sign outside this library, without putting wallet keys in the client.
}
```

`ResponseMetadata::headers` preserves all received headers, and `header(name)`
looks one up case-insensitively. `x_balance_remaining`, `payment_required`, and
`payment_response` are convenience fields but deliberately remain strings:
balances are decimal protocol values and payment envelopes are opaque base64.
Empty credentials and endpoint/mode mismatches return `InvalidArg` before a
socket is opened. Credentials are never copied into an `Error`.

Whether a request streams is decided by the method you call, not by a field on
`ChatRequest`. If you build the wire body yourself, say so explicitly:
`req.to_json_body(/*stream=*/false)`.

**`response_format` is raw JSON, not an enum.** The API accepts both
`{"type":"json_object"}` and a full `{"type":"json_schema", …}` block, and no
enum can carry a schema — so the field is `std::optional<nlohmann::json>` and
the ergonomics live in builders:

```cpp
req.response_format = venice::response_format::text();
req.response_format = venice::response_format::json_object();
req.response_format = venice::response_format::json_schema("reply", my_schema);
// anything the builders don't cover, assign the object yourself
```

### Picking a model

`models()` returns typed metadata, so choosing one is a filter rather than a
second round of JSON parsing:

```cpp
const auto list = client.models();      // named, not iterated as a temporary
if (!list) return;                      // inspect list.error()

for (const auto& m : *list) {
  if (!m.capabilities || m.capabilities->supports_function_calling != true) continue;
  if (!m.context_length || *m.context_length < 200000) continue;

  // per-million-token input rate, if quoted
  if (m.pricing && m.pricing->base.input && m.pricing->base.input->usd)
    std::cout << m.id << " $" << *m.pricing->base.input->usd << '\n';
}
```

The result is bound to a named variable on purpose. `for (const auto& m :
*client.models())` compiles and reads freed memory — the `expected` temporary
dies at the end of the range-for's initializer, and the lifetime extension that
would save it (P2718R0) is GCC 15 / Clang 19 while this library supports GCC
13+. It aborts under ASan on GCC 14.

Every field but `id` and `type` is optional, and absent means *the response did
not say* — not `false` and not zero. `m.capabilities->supports_vision != true`
above is deliberate: an unset flag is not a "no".

**The bare call lists text models only** — that is Venice's default for the
endpoint, not a choice this client makes, and it is 105 of the 299 models the
API actually serves. Pass a type to reach the rest:

```cpp
client.models();          // no query string — text, as before
client.models("image");   // 37
client.models("all");     // 299: text, video, image, inpaint, music, tts,
                          //      embedding, asr, upscale
```

The type is a plain string, not an enum, for the same reason `response_format`
is raw JSON: the value set is Venice's, and a list hardcoded in this header
would start refusing valid values the day a modality is added. An unrecognised
type comes back as the server's `ErrorKind::Http` 400.

### What a call cost

Venice says, so you do not have to work it out. `ChatResponse::cost` carries the
server's own figure on **both** paths — the non-streaming reply and the
assembled streamed one:

```cpp
if (res->cost && res->cost->diem)
  std::cout << "this call cost " << *res->cost->diem << " diem\n";
```

Two things to know before displaying it, both measured on 2026-08-10 across the
seven model families VC-17 swept:

- **`usd` has been `0` on every capture, and `0` does not mean free.** A call to
  `openai-gpt-55-pro` with a rate-card value of $0.0645 came back
  `{"usd":0,"diem":0.0645375}` — `diem` carried the magnitude, `usd` reported
  zero. So read `diem` unless you have measured otherwise for your own key. The
  library reports what arrived and interprets nothing.
- Both currencies are `std::optional<double>`, and **absent is not zero**. A
  disengaged value means the server did not say.

The rate card below is still useful, but for estimating *before* a call. After
one, Venice has told you.

### Estimating before the call

Some models reprice past a context threshold, so an estimate picks a tier:

```cpp
const auto& p = *m.pricing;
const venice::PriceTier& tier =
    (p.extended && p.extended_threshold_tokens && tokens > *p.extended_threshold_tokens)
        ? *p.extended : p.base;
```

Pairing that against a reply's `Usage` afterwards is a *reconstruction*, and it
cannot be made exact — which is why `cost` above exists. Pricing carries **two**
cache buckets (`cache_input` for a read, `cache_write` for populating it) while
`Usage` reports only one (`cached_tokens`), so a response cannot be paired 1:1
with the rate card. And `cached_tokens` itself is **per-family**: five of the
seven models VC-17 swept report it, two report nothing but the three flat
counts, so an estimate must treat an absent bucket as unknown rather than as
zero. Run `venice-cpp --usage <model>` to see what a given family reports.

`venice::Price` is the type on both sides, and the two are not the same
quantity: `Model::pricing` is a **rate** (per million tokens), `res->cost` is an
**amount** (what one call charged). Do not multiply the latter by a token count.

**`Model::raw` holds the whole entry verbatim**, modeled fields included. It is
not called `extra` because it is the opposite of the request-side hatches: it is
never sent, and it is a superset rather than a complement — so code reading
`m.raw["model_spec"]["betaModel"]` keeps working the release that key becomes a
typed field. It is also how you reach anything this struct does not model:

```cpp
m.raw["model_spec"]["pricing"]["upscale"]["2x"]["usd"];  // image models
m.raw["model_spec"]["constraints"];                      // per-model defaults
```

`model_spec` is polymorphic by model type — image models carry generation
pricing and no context window, tts carries a voice list — so the typed surface
is the text shape and `raw` carries the rest. That is worth knowing before
listing a non-text type: those entries parse, and their `name`, `description`,
`traits` and `offline` are typed like any other, but `context_length`,
`capabilities` and `pricing->base` come back empty because the keys behind them
are text-only. Their rates are in `raw`, as above. Malformed entries degrade rather
than failing the listing: an entry with no usable `id` is skipped, a
wrong-typed field reads as absent, and only a response that is not a list at
all comes back as `ErrorKind::Parse`.

### Picking a character

`venice_parameters.character_slug` selects a persona, and `characters()` is how
you find one to select:

```cpp
venice::CharacterQuery q;
q.search = "philosophy";
q.tags = {"helpful", "productivity"};   // sent as tags=helpful&tags=productivity
q.is_web_enabled = true;

const auto page = client.characters(q);
if (!page) return;                      // inspect page.error()

for (const auto& c : page->entries) {
  std::cout << c.slug;
  if (c.name) std::cout << "  " << *c.name;
  if (c.stats && c.stats->average_rating) std::cout << "  " << *c.stats->average_rating;
  std::cout << '\n';
}
```

Note the named `page`. Writing `for (const auto& c : *client.characters(q))`
compiles and is a use-after-free: the `expected` is a temporary that dies at the
end of the range-for's initializer, and the fix that extends its lifetime
(P2718R0) is GCC 15 / Clang 19, while this library supports GCC 13+. Under ASan
on GCC 14 that spelling aborts with `stack-use-after-scope`.

A default-constructed query sends the bare `/characters`. Every filter is
optional and an unset one contributes no query key at all, so building a query
up conditionally never changes the shape of what the other filters send.

**The endpoint pages, and the default page is 50.** It caps at 100, so a
complete listing means asking until a short page comes back:

```cpp
venice::CharacterQuery q;
q.limit = 100;
std::vector<venice::Character> all;
for (q.offset = 0; ; *q.offset += 100) {
  auto page = client.characters(q);
  if (!page) break;                       // inspect page.error()
  const bool last = page->returned < 100;
  all.insert(all.end(), std::make_move_iterator(page->entries.begin()),
             std::make_move_iterator(page->entries.end()));
  if (last) break;
}
```

**`page->returned`, not `page->entries.size()`** — and that distinction is why
`characters()` returns a `CharacterPage` rather than the vector `models()`
returns. The parse skips entries it cannot use, so a full page containing one
slug-less entry comes back with fewer usable characters than the server sent;
paging on the usable count would end the walk right there and report a truncated
catalogue as a complete one. `returned` is the server's own page size and is the
only honest thing to compare a limit against. `page->raw` is the whole envelope,
so a `total` or a cursor appearing later is reachable without this signature
changing.

There is no all-pages helper here on purpose: that loop needs a policy for a
page that fails halfway through — abandon, retry, or return what it has — and
each answer is wrong for somebody. The one above abandons; yours may not want to.

`sort_by` and `sort_order` are plain strings for the reason `models(type)` is
one — the value set is Venice's (`featured`, `highestRating`,
`highlyRated`, `highlyRatedAndRecent`, `imports`, `mostRecent`, `ratingCount`),
and a list hardcoded here would refuse a valid value the day one is added.
`tags`, `categories` and `model_id` each repeat their key once per element —
`tags=helpful&tags=productivity`, which the endpoint documents as the primary
form and which is measured to mean OR rather than last-wins. Note what it does
*not* buy you: the server also splits on commas inside a single value, so a tag
containing a comma cannot be expressed by any spelling and is the endpoint's
constraint rather than this client's.
`CharacterQuery::extra` reaches any filter this struct does not model, and a
modeled field that is set wins the key rather than sending it twice.

Only `slug` is a plain string; everything else is optional and absent means *the
response did not say*. Degradation matches `models()`: an entry with no usable
slug is skipped, a wrong-typed field reads as absent, and only a body that is
not a list at all is an `ErrorKind::Parse`. `Character::raw` holds the whole
entry verbatim — worth more here than elsewhere, because Venice documents
`/characters` as a preview API that may change.

Unlike `/models`, this endpoint is Bearer-only. Selecting Public, SIWX or an
x402 payment for it returns `InvalidArg` before transport; server responses use
`Auth` for 401/403 and `PaymentRequired` for 402.

**`ChatRequest::extra` is a top-level passthrough**, same idea as
`VeniceParameters::extra`: Venice accepts sampling keys this struct doesn't
model (`top_k`, `min_p`, `repetition_penalty`), and `extra` reaches them without
forking the header. Modeled fields always win over a same-named key in `extra`.

Ranges are not checked client-side, but representability is. Structural problems
that make a request unsendable — an empty model, no messages, or a non-finite
`temperature` / `top_p` / `frequency_penalty` / `presence_penalty`, since JSON
has no NaN or infinity — come back as `ErrorKind::InvalidArg` naming the
offending field, before any HTTP call is made. Value-range policy belongs to the
server, so `temperature = 5.0` is transmitted and the API decides. Values inside
`extra` are passthrough and are not inspected, finiteness included.

### Timeouts and cancellation

Every entry point takes a trailing, defaulted `RequestOptions`. The defaults are
the ones this library has always used — 300 s read, 30 s connect — because a
chat completion can legitimately take minutes; what changed is that they are now
a floor you can move per call rather than a property of the library:

```cpp
using namespace std::chrono_literals;

auto models = client.models("all", {.connect_timeout = 5s, .read_timeout = 10s});
```

Cancellation needs a second thread, necessarily: the calling thread is blocked
inside the transport, so nothing on it can run.

```cpp
venice::CancelToken token;

std::thread ui{[&] {
  if (user_pressed_escape()) token.cancel();   // safe from any thread
}};

auto res = client.chat(req, {.cancel = &token});
ui.join();

if (!res && res.error().kind == venice::ErrorKind::Cancelled) {
  // our own decision coming back to us — not a failure to log
}
```

The token is sticky and one-shot; construct one per call that needs one. It must
outlive the call, which is why `RequestOptions::cancel` is a borrowed pointer.
Cancelling works on a request that has received nothing yet, which is the case
that matters and the one a callback cannot reach.

**Streaming has two different ways to stop, and they mean different things:**

| how | result | meaning |
| --- | --- | --- |
| `on_token` returns `false` | success, partial `ChatResponse` | you have what you wanted |
| `token.cancel()` | `ErrorKind::Cancelled`, no response | you have abandoned the call |

The first is unchanged from earlier releases. The second exists because the
first cannot cover the cases worth covering: `on_token` only runs when a content
delta arrives, so a stop wanted before the first delta, during a gap between
frames, or while the server stalls after headers is invisible to it and waits
out the read timeout.

A cancelled call returns `ErrorKind::Cancelled` rather than `Network`. Both
arrive as a socket that stopped working, but a dead network is a fault to report
or retry and a cancellation is your own decision handed back — collapsing them
would leave the difference reachable only by parsing message text.

**Cancelling loses the response, not the data.** A cancelled `chat_stream` still
returns no `ChatResponse` — that part is deliberate — but if you passed a
`StreamAccumulator`, it is *your* object and it still holds every token, every
thought and every verbatim chunk that arrived before you gave up:

```cpp
venice::StreamAccumulator acc;
auto res = client.chat_stream(req, acc, {.cancel = &token});

if (!res && res.error().kind == venice::ErrorKind::Cancelled) {
  auto partial = acc.message();   // everything that did arrive
}
```

### Streaming structure, and feeding it back

`chat_stream` has three forms. The `std::string_view` one is unchanged and is
now a thin adapter; the other two take a `StreamAccumulator` you own:

```cpp
chat_stream(req, on_token,      opts = {})   // content text only
chat_stream(req, acc,           opts = {})   // accumulate, no callback
chat_stream(req, acc, on_delta, opts = {})   // accumulate + observe
```

There is no callback-only structured overload on purpose. Beside the
`string_view` one it would make `chat_stream(req, [](auto d){ ... })` ambiguous —
a source break for code that compiles today — and requiring the accumulator is
what guarantees you cannot ask for the rich stream and then lose it.

A `StreamDelta` is one SSE frame: `content`, `reasoning_content`, `role`,
`finish_reason`, `refusal`, tool-call fragments, `usage`, `cost`, and `chunk`
pointing at the whole verbatim frame. Every field is optional because a frame carries some
of them, and it is a struct rather than a variant so that a future field is
additive instead of an ABI break. **It is a view** — valid only for the duration
of the callback. Anything worth keeping is already in the accumulator.

### Round-tripping a turn

A reply is a `Message`, and a `Message` is what you send. That is the whole
contract: nothing received is discarded, anything received can be sent back —
and anything received can be **withheld**.

```cpp
auto res = client.chat(req);

req.messages.push_back(*res->message);          // replay the turn verbatim,
                                                // thinking and tool calls included
req.messages.push_back(venice::Message::tool("call_a", R"({"temp_f":68})"));
```

Some reasoning models need the prior turn's thinking replayed; others must not
see it. Both are one line, because every modeled field is optional and an unset
one is **erased** from the body rather than falling back to what the server
originally sent:

```cpp
auto turn = *res->message;
turn.reasoning_content.reset();   // this turn goes without it
turn.tool_calls.reset();          // and does not re-issue an executed call
```

That erase-on-clear rule is the reason `Message` carries two escape hatches
where every other type here carries one:

| field | direction | contract |
| --- | --- | --- |
| `raw` | response-side | the verbatim server object, a superset. **Never serialized.** |
| `extra` | request-side | additive seed for the body; modeled fields win. |

Collapsing them into a single `raw` that also seeds serialization looks
attractive — a parsed reply would round-trip for free — but it honours
`turn.content = "edited"` while silently ignoring `turn.content.reset()`, which
resends the answer you just redacted. Verbatim replay of unmodeled keys is still
one deliberate line: `turn.extra = turn.raw;`

**Those hatches reach message-level keys only.** `tool_calls` is a modeled field,
so it wins over the seed — an unmodeled key *inside* a tool call is not
recoverable by `turn.extra = turn.raw`. That is not a hypothetical: it is how
`thought_signature` was lost (#29), and why the field is modeled rather than left
to a hatch. A tool-call-level passthrough is filed as #31.

### Signatures on a tool call

Gemini-family models attach an opaque `thought_signature` to the function-call
part and **require it echoed verbatim** on the next turn. Measured against
`api.venice.ai` on `gemini-3-6-flash`, 2026-08-09: the same turn replayed with it
stripped is `HTTP 400` — *"Function call is missing a thought_signature in
functionCall parts"* — and replayed with it echoed is `200`. Venice passes the
field through rather than stripping it; it sits beside `function`, not inside it.

`ToolCall::thought_signature` holds it, and the caller does nothing: replaying
`*res->message` carries it back. It is emitted **only when the server sent one**,
so a family that uses no signature — `zai-org-glm-4.7`, for instance — produces
the byte-identical body it always did. The value is opaque and is never decoded,
validated or length-checked.

`content` is `std::optional<nlohmann::json>` rather than a string, because the
wire has four states and only that type holds all of them: absent (`nullopt`),
`null` (`= nullptr`), text, and a multimodal parts array. `text()` flattens it
when you just want the words.

### Declaring tools

The section above answers a tool call. This is where the function was offered in
the first place, which closes the loop:

```cpp
req.tools = std::vector<nlohmann::json>{
    venice::tools::function("get_weather", "Look up the current weather",
                            nlohmann::json::parse(R"({
                              "type": "object",
                              "properties": {"city": {"type": "string"}},
                              "required": ["city"]
                            })"))};

req.tool_choice = venice::tool_choice::automatic();   // or none() / required()
                                                      // or function("get_weather")
req.parallel_tool_calls = false;                      // one call at a time
```

**`tools` holds raw JSON elements, not a typed `Tool`.** A struct that re-nested
`name`/`description`/`parameters` under `"function"` would hardcode exactly one
tool shape — and would emit `{"type":"web_search","function":{"name":""}}` the
day Venice accepts an entry that is not a function. `ToolCall` on the response
side may nest unconditionally because it re-serializes what the server *sent*;
`tools` is yours to author. The corollary, learned the expensive way in #29: a
freshly built object serializes exactly what it models and drops the rest, so a
server-sent tool-call key that a model *requires back* has to be modeled — no
hatch reaches it. So the builders supply the ergonomics and anything
they don't cover you assign yourself, exactly as with `response_format`:

```cpp
req.tools->push_back(nlohmann::json::parse(R"({"type":"web_search"})"));
(*req.tools)[0]["function"]["strict"] = true;   // any unmodeled sub-key
```

Two details worth knowing:

- **`venice::tool_choice::automatic()` emits the string `"auto"`.** The builder
  is not spelled `auto` because that is a keyword, and not `any` because `any`
  is Anthropic's name for what OpenAI calls `required` — a caller who knew one
  API would read it as the other. `none()` and `required()` keep their wire
  spelling.
- **Nothing here is validated client-side, tool names included.** An empty name
  or a `tool_choice` naming a function you never declared is transmitted, and
  Venice's 400 names the offending entry — which is more than this library could
  say. That is the same boundary `extra` sits behind, and the same reason
  `messages` is checked for emptiness but never entered.

### CMake

However you acquire venice-cpp, the line you write is the same:

```cmake
target_link_libraries(your_target PRIVATE venice-cpp::lib)
```

No include directories, no `CMAKE_CXX_STANDARD`, no OpenSSL — the C++23
requirement, the headers and the transitive dependencies all arrive as usage
requirements of that one target. Three ways to get it:

```cmake
# 1. A copy in your tree
add_subdirectory(third_party/venice-cpp)

# 2. FetchContent
include(FetchContent)
FetchContent_Declare(venice-cpp
  GIT_REPOSITORY https://github.com/gobha-me/venice-cpp.git
  GIT_TAG        v0.13.0)
FetchContent_MakeAvailable(venice-cpp)

# 3. An installed package
find_package(venice-cpp CONFIG REQUIRED)
```

Modes 1 and 2 build the library and nothing else: the smoke binary, the test
suite and the install rules all default to `PROJECT_IS_TOP_LEVEL`, so a consumed
build does not drag them along. Override with `-Dvenice-cpp_BUILD_BIN=ON`,
`-Dvenice-cpp_TESTS=ON`, `-Dvenice-cpp_INSTALL=ON` if you want them anyway.

To install:

```bash
cmake -B build -DCMAKE_INSTALL_PREFIX=/your/prefix \
  -Dvenice-cpp_BUILD_BIN=OFF -Dvenice-cpp_TESTS=OFF
cmake --build build && cmake --install build
```

Two things to know about that prefix:

- **It may contain more than venice-cpp.** Dependencies are `find_package`-first
  with a FetchContent fallback, and a dependency built from source has to be
  installed alongside us — CMake cannot export a target's interface while
  omitting an edge of it. Configure says which ones, by name. Install
  cpp-httplib and nlohmann/json as packages first and reconfigure for a prefix
  holding only venice-cpp. Everything third-party is filed under its own name
  (`share/doc/httplib/`, `share/licenses/httplib/`), never under ours.
- **`venice-cpp_ROOT` is not usable as a shell environment variable** — POSIX
  variable names cannot contain a hyphen. Point consumers at the prefix with
  `-DCMAKE_PREFIX_PATH=` or `-Dvenice-cpp_DIR=<prefix>/lib/cmake/venice-cpp`.

`example/consumer/` is a miniature downstream project that builds all three ways;
`example/consumer/verify.sh` runs them and is part of CI.

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
default and all three sanitizer toolchains — eleven jobs, including a standalone
run of the version-parser self-test and the three-mode consumer acceptance check
on both compilers. The Clang jobs pin Clang 20: Ubuntu's stock Clang 18 cannot
compile C++23 `std::expected` against libstdc++, and this library returns
`std::expected` from every fallible entry point.

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
models list (105 text models, 299 across all modalities), token usage — the
counts move as Venice's catalogue does.

**All four smoke legs have now been run against the live API** (2026-08-09), so
the "documented, not measured" caveat that covered v0.8.0 through v0.10.0 is
retired. `--characters` confirmed every modeled key, its types and the 50-entry
default page; `--stream` confirmed where `reasoning_content` sits and that the
turn replays; `--tools` confirmed a tool call round-trips and is accepted back —
**on the family it happened to pick.** Run against another it was rejected, which
is #29 below. Two things the runs opened rather than closed were filed as #28
(`Usage`'s nested detail objects are modeled but never sent) and #29 (a replayed
tool turn rejected by Gemini-family models); **#29 is fixed in v0.11.0**, and
`--tools` now passes both legs on `gemini-3-6-flash`, `gemini-3-5-flash` and
`zai-org-glm-4.7`.

**#28 is settled in v0.11.1, and its premise was wrong.** Venice does send both
detail objects, at exactly the nesting the library reads — but only on some
model families. `--stream` had auto-picked `gemini-3-6-flash`, one of two
families in a seven-model sweep that report neither, and printed "(absent —
check the nesting)", which reads as a parse bug. Both mistakes are fixed: every
auto-picking leg now names the runners-up it did not try, and the new
`venice-cpp --usage [model]` prints the verbatim `usage` object beside the typed
one, because that is the only thing that tells "the server did not send it"
apart from "we are looking in the wrong place". Since VC-20 it reports the whole
response **envelope** that way and not only the `usage` sub-object — which is
how `cost` was found: a sibling key that no leg had ever been positioned to see.

The original caveat, for the record: the
**v0.8.0, v0.9.0 and v0.10.0 wire shapes were documented, not measured**. Where
`reasoning_content` sits, where `cached_tokens` is nested, how tool-call
fragments are keyed, how `tools` / `tool_choice` / `parallel_tool_calls` are
spelled on the way out, and now what a `/characters` entry contains — none of it
came from a capture. `/models` answers for any bearer token; chat and
`/characters` do not, and the implementing environment had no key.
(`/characters` was measured far enough to know that: 402 with no credentials,
401 with a junk bearer.) Three commands settle the rest against the live API:

```bash
VENICE_API_KEY=... venice-cpp --stream "..."   # v0.8.0: the reply shapes
VENICE_API_KEY=... venice-cpp --tools          # v0.9.0: the request shape
VENICE_API_KEY=... venice-cpp --characters     # v0.10.0: the character entry
VENICE_API_KEY=... venice-cpp --usage [model]  # v0.12.0: usage + cost + envelope
```

`--tools` runs two legs, and the second is the one that matters: leg one proves
`tools` parsed, leg two answers the call and proves the assembled turn is a
conversation Venice will continue. With no model argument it auto-picks the first
text model claiming `supports_function_calling` and **names the runners-up**,
because #29 read as a library bug until the same run was tried on another family
— `venice-cpp --tools <id>` runs the same two legs on any of them, and a pass
there with a failure here is a model-family gap. It also reports whether a
`thought_signature` came back, dumps the turn as it would be replayed, and fails
the run if a signature was seen but not carried back, so VC-18 cannot regress
quietly behind a green leg two. `--characters` prints the columns a picker
would branch on, so a key that parses in the fixture and not in reality shows up
as a blank column; its count is also the only evidence for the claim that the
page defaults to 50. If something disagrees, the fixture in `test/07stream/`,
`test/02request/` or `test/08characters/` is what needs correcting.
`Message::raw`, `ChatResponse::raw`, `Character::raw` and `acc.chunks()` are why
a wrong guess there is recoverable rather than lossy.

See `AGENTS.md` for contributor/agent conventions and the testing philosophy
(test how it fails, not just the happy path).
