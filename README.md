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
- **Each modality's own shape**, not just text's — an image model's step and
  aspect-ratio constraints, a video model's durations and whether it takes an
  image or a video as input, a TTS model's voices and voice-cloning terms, a
  music model's duration/lyrics/voice policy, an embedding model's dimensions,
  and deprecation dates with a replacement id.
  One optional view per modality, engaged by the model's `type`.
- **The catalogue's own answer to "which model"** (`/models/traits`,
  `/models/compatibility_mapping`) — the model currently holding a capability
  (`default`, `fastest`, `default_vision`), and what a foreign vendor's model id
  such as `gpt-4o` resolves to here. Both are **public** — they answer with no
  credential at all, as `models()` does; the whole Models family is reachable
  without a key.
- **Characters** (`/characters`, `/characters/{slug}`, `/characters/{slug}/reviews`)
  — the discovery half of `venice_parameters.character_slug`: slug, name,
  description, tags, the model a character runs on, and its rating stats, plus
  the verbatim entry as `raw`. The listing is filterable and pageable, a known
  slug can be fetched directly, and the reviews behind a rating are pageable
  with the server's own page count.
- **Embeddings** (`/embeddings`) — single text, text batches, token arrays and
  token-array batches; float vectors and opaque base64 results remain distinct,
  with strict index and usage accounting plus the verbatim response as `raw`.
- **Image generation** (`/image/generate`, `/images/generations`) — separate
  Venice-native and OpenAI-compatible requests. Native success is a typed JSON
  envelope or byte-exact JPEG/PNG/WebP selected from the actual response media
  type; the client never decodes or writes an image. Public `/image/styles`
  discovers the current style strings without a credential.
- **Image transformations** — upscale, single/multi-image edit and background
  removal accept explicit inline, URL or owned-file inputs as each endpoint
  permits. JSON versus multipart is selected from that input, and successful
  image bytes retain their actual media type and response metadata.
- **Audio** — buffered and chunk-streamed text-to-speech, multipart
  transcription in typed JSON or exact text, TTL-bound cloned-voice handles,
  and explicit quote/queue/retrieve/cleanup calls for asynchronous audio.
  Successful AAC/FLAC/MP3/Opus/PCM/WAV bytes are selected by actual media type
  and are never decoded, played, or written by the client.
- **Video** — explicit quote/queue/retrieve/cleanup generation calls plus URL
  transcription. Retrieval preserves processing JSON or byte-exact MP4 based
  on the actual response media type; transcription likewise preserves typed
  JSON or exact text. The client never polls, deletes, decodes, displays or
  writes media implicitly.
- **Augment** — owned multipart document parsing with typed JSON or exact text,
  plus direct web scrape and ordered web search calls. Stable render fields are
  typed, provider metadata remains in `raw`, and the client never writes or
  retains uploaded documents.
- **Crypto RPC** — public supported-network discovery plus a Bearer/SIWX
  JSON-RPC 2.0 proxy for single calls or ordered batches. Method-specific
  params/results stay raw and forward-compatible; HTTP-200 JSON-RPC errors,
  billing headers and x402 balance metadata remain directly inspectable.
- **x402 wallets** — SIWX-authenticated balance and paginated transaction
  reads plus explicit public payment discovery and signed top-up. Requirements
  and receipts are distinct typed outcomes; base-unit amounts remain exact
  strings, and the client never owns wallet keys or constructs a payment.
- **Account billing** (`/billing/balance`, `/billing/usage-analytics`,
  `/billing/usage-history`) — typed balance and aggregate views plus ordered,
  cursor-paged ledger history. History can return typed JSON or byte-exact CSV,
  selected from the response's actual media type.
- **API-key administration and wallet proof** — typed list/detail/create/update/
  delete, current rate limits and the account's last 50 exceeded-limit records,
  plus a public challenge/sign/create flow for caller-owned wallets. Returned
  tokens, signatures and key material are never duplicated into diagnostics or
  printed by the library.
- **Explicit authentication** — Public, Bearer, pre-signed SIWX and pre-built
  x402 payment payloads are distinct modes, selectable per client or per call.
  The client never owns wallet private keys or constructs signatures.
- **API-key rate limits** (`/api_keys/rate_limits`) through typed
  `api_key_rate_limits()` and the historical raw `balance()` compatibility
  spelling, kept distinct from account billing.
- **Per-request timeouts and cancellation** — every call takes an optional
  `RequestOptions` with connect/read/write timeout overrides and a
  `CancelToken` that aborts an in-flight request from another thread, including
  one that has received nothing at all, plus an optional authentication
  override and header-only idempotency key for that call.
- **Error model** — `std::expected<T, venice::Error>`; network / HTTP / parse /
  auth / payment-required / rate-limit / invalid-arg / cancelled. Response
  failures carry status, raw body and response metadata, including x402 headers.

Later phases (fed by real use): retries/backoff and high-level async
workflow helpers.

## OpenAPI coverage

OpenAPI coverage: 48/49 operations implemented.

One additional published operation is explicitly unsupported:
`GET /billing/usage` already returns 410 for every request and points callers
to `/billing/usage-history`. Keeping the retired method in the inventory makes
that decision visible without adding a public method that can never succeed.

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
#include <iostream>
#include <string>
#include <variant>
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
req.max_completion_tokens = 256;  // preferred; max_tokens remains compatible
req.reasoning_effort = "medium";
req.prompt_cache_key = "conversation-42";
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

`ChatResponse::choices` exposes every returned choice, including its index,
message, finish/stop reasons and raw logprobs. The historical `message`,
`content`, and `finish_reason` fields remain choice-zero conveniences. Streaming
assembles choices independently by their wire index; `StreamDelta::choice_index`
identifies the choice represented by a callback, and `StreamDelta::logprobs`
borrows that choice's provider-shaped logprobs for the current chunk.

`ChatStreamOptions` does not own the `stream` bit. The selected client method is
the only source of truth: `chat()` emits `stream=false` and no stream options,
while `chat_stream()` emits `stream=true` and may include them:

```cpp
req.stream_options = venice::ChatStreamOptions{.include_usage = true};
```

### Responses API

`create_response` is Venice's Alpha, stateless Responses endpoint. Its input
and output item lists remain raw JSON so a new item type is immediately
reachable; builders and typed accessors cover the documented forms:

```cpp
venice::ResponsesRequest request;
request.model = "zai-org-glm-5-1";
request.input = venice::responses_input::items({
    venice::responses_items::message(
        "user", nlohmann::json::array({
                    venice::responses_content::input_text("Describe this image"),
                    venice::responses_content::input_image(
                        "https://example.com/image.png", "high")}))});
request.max_output_tokens = 256;
request.tools = std::vector<nlohmann::json>{
    venice::tools::function("lookup", "Look up a value")};

if (auto response = client.create_response(request)) {
  const std::string text = response->output_text();
  const auto calls = response->function_calls();
  const auto citations = response->citations();
  // response->output and response->raw preserve every unknown item/key.
}
```

The public method deliberately forces `stream=false`: the current document
describes a JSON success body but publishes no SSE event schema. A streaming
API will not be guessed from another provider's contract. The endpoint also
does not support E2EE; explicitly enabling it is `InvalidArg`, and the client
never silently reroutes the request to Chat Completions.

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
`PAYMENT-SIGNATURE`, not the legacy `X-402-Payment` spelling. The x402 methods
use those modes directly:

```cpp
// Discovery is an empty public POST. HTTP 402 is the expected typed result.
const auto discovery = public_client.x402_top_up();
if (discovery) {
  if (const auto* requirements =
          std::get_if<venice::X402PaymentRequirements>(&*discovery)) {
    // Select one of requirements->accepts and sign outside this library.
    // `amount` is exact base-unit text; metadata.payment_required is opaque.
  }
}

const std::string signed_payment = "base64 payload signed outside venice-cpp";
const auto receipt = public_client.x402_top_up(
    {.authentication = venice::Authentication::x402_payment(signed_payment)});

const auto balance = wallet_client.x402_balance("0xYOUR_WALLET_ADDRESS");
venice::X402TransactionsQuery page;
page.limit = 50;
page.offset = 0;
const auto transactions =
    wallet_client.x402_transactions("0xYOUR_WALLET_ADDRESS", page);
```

Balance and transactions require a caller-produced SIWX proof for the same
wallet named in the path. Venice verifies that relationship and the accepted
EVM/Solana address syntax; the client only encodes the address as one path
segment. A paid top-up never polls or retries implicitly.

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
socket is opened. Caller-owned credential/proof values containing NUL, DEL or
forbidden C0 controls likewise fail before transport, so they cannot change the
request's header structure. Their semantic formats remain server-owned and
otherwise pass through byte-exact. Credentials are never copied into an
`Error`.

Owned multipart filenames and media types are checked at the same structural
boundary before any image, audio or document upload. Filenames containing C0
controls, DEL, quote or backslash, and media types containing C0 controls or
DEL, return `InvalidArg` without echoing the value. The client does not rewrite
metadata or impose an endpoint MIME allow-list; safe metadata and all payload
bytes remain byte-exact.

The client does not follow HTTP redirects. Venice publishes no 3xx contract,
and replaying an authenticated, paid or multipart request to a response-provided
`Location` would cross a security boundary. A 3xx therefore reaches the caller
as `ErrorKind::Http` with its original status, body and response metadata,
whether the target is same-origin or cross-origin.

Custom base URLs are validated before cpp-httplib constructs a transport.
Unsupported schemes and malformed or out-of-range ports return
`ErrorKind::InvalidArg` without throwing or copying the URL into diagnostics.
HTTP/HTTPS hosts, IPv6 literals, explicit valid ports and `/api/...` prefixes
remain supported; failures after construction, such as DNS, TLS, connection,
timeout or cancellation failures, keep their existing error kinds.

Whether a request streams is decided by the method you call, not by a field on
`ChatRequest`. If you build the wire body yourself, say so explicitly:
`req.to_json_body(/*stream=*/false)`.

### Embeddings

Embedding input is raw JSON with builders for the four documented forms, so a
new server-owned form remains reachable without waiting for a library release:

```cpp
venice::EmbeddingRequest embedding_request;
embedding_request.model = "text-embedding-qwen3-8b";
embedding_request.input = venice::embedding_input::texts(
    {"first document", "second document"});
embedding_request.encoding_format = "float";

// Name the expected result. Dereferencing a temporary expected in a range-for
// is unsafe on the oldest supported compilers; the same rule models() follows.
const auto embedding_result = client.embeddings(embedding_request);
if (!embedding_result) return;  // inspect embedding_result.error()

for (const auto& entry : embedding_result->data) {
  if (const auto* values = std::get_if<std::vector<double>>(&entry.value))
    std::cout << entry.index << ": " << values->size() << " dimensions\n";
  else if (const auto* encoded = std::get_if<std::string>(&entry.value))
    std::cout << entry.index << ": " << encoded->size() << " base64 bytes\n";
}
```

Assign `embedding_request.input` directly for an input shape the builders do not
cover. `model`, `dimensions` and `encoding_format` are server-owned values: the
client rejects missing/empty required structure, but transmits range and value
policy verbatim. Base64 is not decoded because Venice does not specify the
decoded element width or byte order. Successful SIWX calls expose the exact
`X-Balance-Remaining` string through `EmbeddingResponse::metadata`.

### Image generation

Native generation can return JSON/base64 or encoded media. The actual response
`Content-Type` selects the result alternative; `return_binary` is a request,
not a promise the client uses to guess the response:

```cpp
venice::ImageGenerationRequest image_request;
image_request.model = "flux-dev-uncensored";
image_request.prompt = "A blue ceramic cup on a white background";
image_request.format = "png";
image_request.return_binary = true;

const auto image_result = client.generate_image(image_request);
if (!image_result) return;  // inspect image_result.error()

if (const auto* media = std::get_if<venice::GeneratedImageMedia>(&*image_result)) {
  std::cout << media->media_type << ": " << media->bytes.size() << " bytes\n";
} else if (const auto* json =
               std::get_if<venice::NativeImageGenerationResponse>(&*image_result)) {
  std::cout << json->images.size() << " base64 image(s)\n";
}
```

`OpenAIImageGenerationRequest` and `generate_image_openai()` expose the
OpenAI-compatible operation separately because its fields and JSON-only result
are different. Both requests serialize only engaged options and retain an
`extra` passthrough with modeled fields winning. Model-specific formats,
qualities, dimensions and ranges pass through to Venice; `models("image")`
provides the typed constraints callers can use to choose them. `image_styles()`
is public and returns its complete envelope in `ImageStyles::raw`.

### Image transformations

Transformation inputs are explicit so a caller's file bytes are never guessed
from a string prefix. Inline base64 and data URLs use `image_input::base64()` /
`data_url()`, remote images use `url()`, and multipart uploads use `file()` with
owned bytes, filename and media type:

```cpp
venice::ImageUpscaleRequest upscale;
upscale.image = venice::image_input::base64("AAEC");  // caller's complete base64 value
upscale.scale = 2;

const auto upscaled = client.upscale_image(upscale);
if (!upscaled) return;  // inspect upscaled.error()
std::cout << upscaled->media_type << ": " << upscaled->bytes.size() << " bytes\n";
```

`ImageEditRequest`, `MultiImageEditRequest` and
`ImageBackgroundRemovalRequest` feed `edit_image()`, `multi_edit_image()` and
`remove_image_background()` respectively. Multi-edit preserves caller order
and accepts either an all-file multipart list or a JSON list of inline/URL
values; mixing the two media forms is an `InvalidArg` before a socket. The
per-model input maximum is available through `models("inpaint")` — current
models report as many as six — and is not duplicated as a stale client guard.
All four results are `GeneratedImageMedia`; the library preserves bytes and
metadata but never decodes, saves or displays an image.

### Audio

Speech has buffered and chunk-callback forms. The selected method owns the
`streaming` wire bit, so a reusable `SpeechRequest` has no second source of
truth for it:

```cpp
venice::SpeechRequest speech;
speech.input = "Hello from Venice.";
speech.model = "tts-kokoro";       // server-owned string
speech.response_format = "mp3";    // consult models("tts")

const auto generated = client.generate_speech(speech);
if (!generated) return;             // inspect generated.error()
std::cout << generated->media_type << ": " << generated->bytes.size() << " bytes\n";

std::string streamed_bytes;
const auto streamed = client.generate_speech_stream(
    speech, [&](std::string_view chunk) {
      streamed_bytes.append(chunk);  // the view lives only for this callback
      return true;                   // false is deliberate early success
    });
if (!streamed) return;
```

`CancelToken` abandonment returns `ErrorKind::Cancelled`; callback `false`
returns `SpeechStreamResult{.completed=false}`. Both forms preserve the actual
normalized AAC/FLAC/MP3/Opus/PCM/WAV media type and exact response metadata.

Transcription and voice cloning upload owned bytes plus filename/media type.
The actual successful media type selects typed JSON versus exact text:

```cpp
venice::AudioTranscriptionRequest transcription;
transcription.file = {generated->bytes, "speech.mp3", generated->media_type};
transcription.model = "openai/whisper-large-v3";
transcription.timestamps = true;

const auto transcript = client.transcribe_audio(transcription);
if (!transcript) return;
if (const auto* json = std::get_if<venice::JsonAudioTranscription>(&*transcript))
  std::cout << json->text << '\n';
else
  std::cout << std::get<venice::TextAudioTranscription>(*transcript).text << '\n';
```

`clone_voice()` returns the server's typed voice handle and model while retaining
the raw response. The handle remains bound to that model; consult the selected
TTS model's `voice_cloning.retention_days` for its server-controlled lifetime
(currently seven days on the measured cloning model).

Asynchronous generation remains explicit: `quote_audio()`, `queue_audio()`,
`retrieve_audio()` and `cleanup_audio()` expose each server operation. Retrieval
returns `AudioProcessing` or `AudioMedia` from the actual response type;
`cleanup_audio()` is named for its destructive meaning and a returned
`success=false` remains a retryable value. The library deliberately does not
poll or delete remote media automatically.

### Video

Video generation follows the server's explicit paid-work lifecycle. Consult
`models("video")` for the chosen model's durations, aspect ratios and
resolutions, quote first, then decide whether to enqueue:

```cpp
venice::VideoQuoteRequest quote_request;
quote_request.model = "a-video-model";
quote_request.duration = "5s";  // a server-owned string from Model::video

const auto quote = client.quote_video(quote_request);
if (!quote) return;              // no paid work has been created
std::cout << "quote: " << quote->quote << '\n';

venice::VideoQueueRequest queue;
queue.model = quote_request.model;
queue.prompt = "Sunlight moving across a quiet mountain lake";
queue.duration = quote_request.duration;
const auto queued = client.queue_video(queue);  // explicit paid side effect
if (!queued) return;
```

`retrieve_video()` returns `VideoProcessing` or byte-exact `VideoMedia` from
the successful response's actual JSON/MP4 media type. `cleanup_video()` is the
explicit destructive operation; `success=false` remains a retryable value.
`transcribe_video()` accepts a caller-owned URL and returns typed JSON or exact
plain text. Nothing polls, deletes, decodes, displays or writes media
implicitly. Queue `elements`, keyframes and legal `consents` remain raw JSON so
future provider shapes and caller attestations are not hardcoded by the client.

### Augment

Augment operations return source material directly rather than changing chat
flags. Document parsing owns the upload bytes and uses the successful response's
actual media type to distinguish structured JSON from exact plain text:

```cpp
venice::DocumentParseRequest document;
document.file = {.bytes = bytes,
                 .filename = "report.pdf",
                 .media_type = "application/pdf"};
document.response_format = "json";

const auto parsed = client.parse_document(document);
if (!parsed) return;

const auto scraped = client.scrape_web({.url = "https://example.com"});
const auto searched = client.search_web(
    {.query = "Venice AI API", .limit = 5, .search_provider = "brave"});
```

`scrape_web()` returns the stable URL/content/format fields and the verbatim
response. `search_web()` preserves result order; each provider result keeps
optional title/URL/content/date views plus its exact `raw` object. Provider
names, limits, formats and URL policy remain server-owned. Venice's retention
statement describes server behavior; the C++ client does not turn it into a
local secure-erasure guarantee.

Live on 2026-08-27, `--augment` parsed the synthetic document as JSON, scraped
`example.com` as markdown and returned one search result; all three typed views
agreed with their retained raw responses. The leg printed no source content.

### Crypto RPC

Network discovery is public. The proxy accepts arbitrary JSON-RPC method
parameters and returns a typed single/batch discriminator while preserving each
method-specific result or error as raw JSON:

```cpp
const auto networks = client.crypto_rpc_networks(
    {.authentication = venice::Authentication::public_access()});

const auto call = venice::crypto_rpc_input::request(
    "eth_chainId", nlohmann::json::array(), nlohmann::json(1));
const auto response = client.crypto_rpc(
    "ethereum-mainnet", call, {.idempotency_key = "chain-id-check-1"});
if (!response) return;
```

An HTTP 200 item containing `error` is a successful proxy transport result and
remains inspectable beside `result`; HTTP failures still use `venice::Error`.
IDs and batch ordering are exact. `ResponseMetadata` retains RPC charge, cost,
request/replay and x402 balance headers. Network and method names, the maximum
batch size and idempotency-key syntax remain Venice policy rather than client
enums or duplicated range guards.

Live on 2026-08-27, public discovery returned 27 ordered networks. The smoke leg
selected `ethereum-mainnet`, received `0x1` from `eth_chainId`, and confirmed an
identical replay through `Idempotent-Replayed: true`; the first call reported 20
credits and $0.00001400. It submitted no transaction.

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
endpoint, not a choice this client makes, and it is 106 of the 314 models the
API actually serves. Pass a type to reach the rest:

```cpp
client.models();          // no query string — text, as before
client.models("image");   // 37
client.models("all");     // 314: text 106, video 111, image 37, inpaint 20,
                          //      music 14, tts 11, embedding 9, asr 5,
                          //      upscale 1   (measured 2026-08-11)
```

The type is a plain string, not an enum, for the same reason `response_format`
is raw JSON: the value set is Venice's, and a list hardcoded in this header
would start refusing valid values the day a modality is added. An unrecognised
type comes back as the server's `ErrorKind::Http` 400.

### Picking a non-text model

A media caller's first question is not "which is cheapest" but "will this model
accept the request I am about to build". `Model` answers that per modality —
one optional view, engaged by the model's `type`:

```cpp
const auto list = client.models("video");   // named, not a temporary
if (!list) return;

for (const auto& m : *list) {
  if (!m.video || !m.video->constraints) continue;
  const auto& c = *m.video->constraints;

  if (c.model_type != "image-to-video") continue;  // a server-owned string
  if (c.audio_configurable != true) continue;      // absent is not a "no"

  std::cout << m.id;
  if (c.durations) for (const auto& d : *c.durations) std::cout << ' ' << d;
  std::cout << '\n';
}
```

That prints 30 of the 111 video models, the longest offering fifteen one-second
steps. Note `c.audio_configurable != true` rather than `!c.audio_configurable`:
an unset flag is not a "no", the same rule `supports_vision` follows.

Five things that surprise people, all of them measured rather than read off the
specification:

- **`model_type` and `video_input` are not independent.** All six models that
  accept a video as input are the six whose `model_type` is `"video"`; no
  `image-to-video` model has `video_input` true. A filter that asks for both is
  empty, which is easier to discover from here than from a run that silently
  prints nothing.

- **Image models carry no `capabilities` block at all.** Their feature flags —
  `supports_web_search`, `supports_style_references` — sit on `m.image`, beside
  the constraints rather than inside a `ModelCapabilities` this library refuses
  to invent for them.
- **An empty list is an answer; an absent one is not.** `m.video->constraints
  ->aspect_ratios` is `optional<vector>`, and 40 of the 111 live video models
  send `[]` — which the API defines as "no defined aspect ratio", not "we did
  not say". `Model::traits` stays a plain vector because nothing branches on
  that difference there.
- **`voice_cloning` absent does not mean the model cannot clone.** Models whose
  cloning is behind a private alpha omit the field for non-staff callers while
  still appearing in the listing.
- **Music is typed where Audio needs policy; ASR has no separate live shape.**
  Music exposes duration, prompt, lyrics, voice, language, speed and format
  constraints. ASR entries currently carry only the common model/pricing fields
  and remain fully available through those fields plus `Model::raw`.

The wire spelling differs by modality in the same position — image sends
`aspectRatios` and `promptCharacterLimit`, video sends `aspect_ratios` and
`prompt_character_limit` — which is why the key tables are literal and why
nothing here derives one spelling from the other.

### Letting the catalogue pick

Scanning a hundred entries is the right answer when your criteria are your own.
When they are Venice's — "the default", "the fastest", "the one for vision" —
the catalogue already knows, and `model_traits()` asks it directly:

```cpp
const auto traits = client.model_traits("image");   // named, not a temporary
if (!traits) return;                                // inspect traits.error()

if (const std::string* fastest = traits->find("fastest"))
  std::cout << "fastest image model: " << *fastest << '\n';
```

`find()` returns a pointer, `nullptr` when the trait is not in this catalogue —
no exception, and no need to compare an iterator against `end()`. The trait
names are the server's and they differ by modality: `image` answers `default`,
`fastest`, `highest_quality`, `most_uncensored` and `eliza-default`, while
`text` answers `most_intelligent`, `default_reasoning`, `default_vision` and
`default_code`. Nothing is hardcoded here, so a trait added next month is
readable without a release.

`model_compatibility_mapping()` is the same shape pointed the other way — from a
foreign vendor's model id to the Venice model that serves it, which is what
makes an OpenAI-shaped codebase portable without a translation table of its own:

```cpp
const auto compat = client.model_compatibility_mapping();
if (compat)
  if (const std::string* venice_id = compat->find("gpt-4o"))
    std::cout << "gpt-4o here is " << *venice_id << '\n';   // llama-3.3-70b
```

Both carry `returned` — how many entries the server sent, before any that could
not be read were skipped — so `returned != entries.size()` tells you something
arrived unusable, and `raw` holds the whole envelope either way.

**Both answer without a credential**, and so does `models()` — measured
2026-08-11, all three return 200 with no `Authorization` header, for every
modality, while `/characters` answers 402. A `Client` built with
`venice::Authentication::public_access()` reaches the whole Models family, and
`venice-cpp --traits`, `--compat` and `--modality` run with no
`VENICE_API_KEY` set at all — the three legs here that do.

One asymmetry worth knowing before you hit it: **these two do not accept the
same `type` values**, despite identical parameter definitions in Venice's
OpenAPI document. `model_traits("all")` is fine and `model_compatibility_mapping("all")`
is a 400. That divergence is the server's, measured rather than inferred, and it
is why `type` is passed through untouched rather than validated here — a set
hardcoded in this header would have to encode a mistake the specification itself
makes. The 400's body names the values it would have accepted.

### Account billing

Account balance, aggregate analytics and the ledger export have unambiguous
`billing_` names; `balance()` remains the older API-key rate-limit call. Venice
requires an **admin API key** for all three billing operations. A valid ordinary
inference key still receives HTTP 401 with `Admin API key required`.

```cpp
const auto balance = client.billing_balance();
if (!balance) return;  // inspect balance.error()
if (balance->balances && balance->balances->diem)
  std::cout << "diem balance: " << *balance->balances->diem << '\n';

const auto analytics = client.billing_usage_analytics({.lookback = "7d"});
if (!analytics) return;

venice::BillingUsageHistoryRequest request;
request.query.page_size = 100;
const auto history = client.billing_usage_history(request);
if (!history) return;
if (const auto* page = std::get_if<venice::BillingUsageHistoryPage>(&*history)) {
  for (const auto& entry : page->entries)
    if (entry.amount) std::cout << *entry.amount << '\n';

  // A continuation request carries only the cursor. First-page filters cannot
  // be mixed with it, so a stale filter cannot silently change the page chain.
  if (page->next_cursor) {
    venice::BillingUsageHistoryRequest next;
    next.query.cursor = *page->next_cursor;
    const auto next_page = client.billing_usage_history(next);
    (void)next_page;  // inspect success/error as appropriate
  }
}
```

For an export, set `request.format = venice::BillingUsageHistoryFormat::Csv`.
The result variant then normally holds `BillingUsageHistoryCsv`, including the
exact response bytes, normalized media type, `Content-Disposition`,
`X-Next-Cursor` and all response metadata. The union is selected from the
server's actual `Content-Type`, not from the requested format, so a JSON error
or a server-side representation change is never mislabeled as CSV.

JSON monetary values are `std::optional<double>` because Venice publishes JSON
numbers rather than decimal strings. They are suitable for display and
approximate arithmetic, not exact ledger equality; use the retained CSV bytes
when the export representation must remain exact. All response structs also
carry `raw`, and missing or malformed optional values remain disengaged rather
than becoming zero. Analytics is Beta and currently sends four dynamic daily
chart maps (`byModelDaily`, `byModelDailyUsd`, `byKeyDaily`, `byKeyDailyUsd`);
their objects remain raw JSON because their keys are account-defined display
names rather than a stable schema.

### API-key administration

Administrative API-key inventory and mutation calls are Bearer-only. Value sets
such as `api_key_type`, `limit_period` and rate-limit types stay strings because
Venice owns them; response structs retain both typed optionals and their
verbatim `raw` objects.
Usage totals remain decimal strings, while configured limits are the JSON
numbers the server publishes. Missing and explicit zero never collapse.
The one exception is `ApiKeyCreated::raw`: every nested `apiKey` value is
replaced with `[REDACTED]`, so the complete one-time secret exists only in
`ApiKeyCreated::api_key`. Valid-JSON create errors receive the same recursive
redaction; a non-JSON body becomes a generic redacted marker because its bytes
cannot be inspected safely. Web3 creation applies the same fail-closed policy
while additionally redacting challenge tokens and signatures.

```cpp
const auto keys = client.api_keys();
if (!keys) return;  // inspect keys.error()
for (const auto& key : keys->entries) {
  if (!key.id) continue;
  const auto detail = client.api_key(*key.id);
  if (!detail) return;
}

venice::ApiKeyCreateRequest create;
create.api_key_type = "INFERENCE";
create.description = "short-lived worker";
create.limit_period = "MONTH";
create.consumption_limit = venice::ApiKeyConsumptionLimitRequest{.usd = 10.0};
auto created = client.create_api_key(create);
if (!created) return;

// created->api_key is complete secret material shown by Venice once. Move it
// directly into the application's secret store; never print or log it.
std::string secret = std::move(created->api_key);
(void)secret;  // hand to the application's secret-store API

venice::ApiKeyUpdateRequest update;
update.id = created->id;
update.description = "renamed worker";
update.expires_at = "";  // engaged empty string explicitly clears expiration
const auto updated = client.update_api_key(update);
if (!updated) return;

const auto deleted = client.delete_api_key(created->id);
if (!deleted || !deleted->success) return;
```

Create and update bodies serialize only engaged fields. Both carry additive
`extra` JSON for future request keys, with modeled fields winning on collision.
An omitted update field remains untouched; an engaged empty `expires_at`
clears expiration, while `extra["expiresAt"] = nullptr` exposes the wire's
equivalent null spelling. The client rejects only an empty ID and non-finite
modeled limit values before transport; server-owned ranges and strings are sent
verbatim so the server's error body can name current policy.

`api_key_rate_limits()` returns the typed current view.
`api_key_rate_limit_logs()` returns the ordered last-50 list the server exposes;
it is not presented as pageable. Existing source using `balance()` keeps its
`std::expected<nlohmann::json, Error>` return type and receives the typed rate-
limit parser's retained raw envelope.

The Web3 creation flow is a separate public protocol. Construct a public client
or use a per-call public authentication override: the wallet address, challenge
token and caller-produced signature belong in JSON, never in Bearer, SIWX or
x402 headers. This library does not own a wallet key, sign or verify the proof,
or add a crypto dependency.

```cpp
venice::Client web3{venice::Authentication::public_access()};
const auto challenge = web3.web3_api_key_challenge();
if (!challenge) return;

venice::Web3ApiKeyCreateRequest web3_request;
web3_request.api_key_type = "INFERENCE";
web3_request.address = "caller-wallet-address";
web3_request.signature = "caller-produced-signature";
web3_request.token = challenge->token;
web3_request.description = "wallet-created worker";

auto web3_created = web3.create_web3_api_key(web3_request);
if (!web3_created) return;
std::string web3_secret = std::move(web3_created->api_key);
(void)web3_secret;  // hand directly to the application's secret-store API
```

The challenge token is reachable only as `Web3ApiKeyChallenge::token`; its
retained `raw` tree is redacted. The POST result reuses `ApiKeyCreated`, with the
same one-time-secret boundary as Bearer creation. Web3 errors preserve status
and response metadata while recursively redacting `token`, `signature` and
`apiKey`; a non-JSON body is replaced by a redacted marker because it cannot be
safely inspected. No CLI leg performs this flow: GET returns proof material and
POST creates a credential.

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
cache buckets (`cache_input` for a read, `cache_write` for populating it), and
`Usage` preserves the corresponding `cached_tokens` and
`cache_creation_input_tokens` only when the provider reports them. Those
details are **per-family**: five of the
seven models VC-17 swept report it, two report nothing but the three flat
counts, so an estimate must treat an absent bucket as unknown rather than as
zero. Run `venice-cpp --usage <model>` to see what a given family reports.
The three flat counters are required when a usage object arrives, and every
reported counter is checked before integer conversion. A fractional or
unrepresentable value returns `ErrorKind::Parse` on buffered and streamed calls
rather than becoming a truncated count or a wrapped negative; a streamed caller
still retains every earlier delta in its `StreamAccumulator`.

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

A slug already stored in configuration or obtained from a link does not require
walking the catalogue. Fetch that character directly:

```cpp
const auto character = client.character("alan-watts");
if (!character) return;                 // inspect character.error()

std::cout << character->slug;
if (character->name) std::cout << "  " << *character->name;
std::cout << '\n';
```

The slug is encoded as one path segment. Reserved bytes such as `/`, `?`, and
`#` cannot change the endpoint reached; an empty slug is `InvalidArg` before a
socket. An unknown slug remains an ordinary `Http` error with status 404 and
the server's exact response body.

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
the Characters API as preview functionality that may change. The direct fetch
uses the same tolerant fields and the same verbatim `raw` contract.

Unlike `/models`, this endpoint is Bearer-only. Selecting Public, SIWX or an
x402 payment for it returns `InvalidArg` before transport; server responses use
`Auth` for 401/403 and `PaymentRequired` for 402.

### Reading the reviews behind a rating

`stats->average_rating` says a character is rated 4.7 and nothing about why.
`character_reviews()` is the other half, and it pages by `page`/`pageSize`
rather than the listing's `offset`/`limit`:

```cpp
venice::CharacterReviewQuery q;
q.page = 1;
q.page_size = 100;

for (;;) {
  const auto reviews = client.character_reviews("alan-watts", q);
  if (!reviews) break;                    // inspect reviews.error()

  for (const auto& r : reviews->entries) {
    if (r.rating) std::cout << *r.rating << "  ";
    std::cout << r.username.value_or("(anonymous)");
    if (r.message) std::cout << " — " << *r.message;
    std::cout << '\n';
  }

  const auto& p = reviews->pagination;
  if (!p || !p->total_pages || *q.page >= *p->total_pages) break;
  ++*q.page;
}
```

That loop can be written honestly, which the listing's cannot: this response
carries `pagination` with the server's own `page`, `page_size`, `total` and
`total_pages`, so nothing has to be inferred from how many entries parsed.
Those four are `int` and read strictly — a fractional or out-of-range value
reads as absent rather than as a truncated page number, and the loop above then
stops instead of re-reading page one forever. `summary` carries
`average_rating` and `total_reviews` as doubles, matching `CharacterStats`.

Reviews are separate types (`CharacterReviewQuery`, `CharacterReviewPage`) and
not a mode of the listing's, because `page = 2` skips a page and `offset = 2`
skips two entries; one struct meaning both is the kind of difference nobody
reads twice.

Nothing is dropped from a review for a missing field. A character entry with no
slug is skipped because a slug is what you hand back to the API; no field on a
review is such a handle, so an entry carrying only a message is still that
reviewer's message, and only a non-object element is skipped — while still
counted in `returned`. The slug is encoded as one path segment here too, and it
matters more than on the direct fetch: the path continues after the slug, so an
unencoded `/` would address a route this operation does not own.

**`ChatRequest::extra` is a top-level passthrough**, same idea as
`VeniceParameters::extra`: every currently documented request property now has
a first-class member, while `extra` keeps future Venice additions reachable
without forking the header. Modeled fields always win over a same-named key.

Ranges are not checked client-side, but representability is. Structural problems
that make a request unsendable — an empty model, no messages, or a non-finite
`temperature` / `top_p` / `frequency_penalty` / `presence_penalty` / `max_temp`
/ `min_p` / `min_temp` / `repetition_penalty`, since JSON has no NaN or infinity
— come back as `ErrorKind::InvalidArg` naming the
offending field, before any HTTP call is made. Value-range policy belongs to the
server, so `temperature = 5.0` is transmitted and the API decides. Values inside
`extra` are passthrough and are not inspected, finiteness included.

JSON representation itself does fail closed. Invalid UTF-8 and a discarded
`nlohmann::json` value at any depth return `ErrorKind::InvalidArg` before a
socket; the client neither substitutes replacement characters nor turns the
value into `null`. This applies equally to buffered requests, Chat SSE,
streamed speech, JSON-form image transformations and JSON-valued multipart
fields. Diagnostics name the field class without copying prompt, upload or raw
passthrough content. Valid raw JSON remains unchanged and is not schema-walked.

### Timeouts and cancellation

Every entry point takes a trailing, defaulted `RequestOptions`. The defaults are
the ones this library has always used — 300 s read, 30 s connect — because a
chat completion can legitimately take minutes; what changed is that they are now
a floor you can move per call rather than a property of the library:

```cpp
using namespace std::chrono_literals;

auto models = client.models("all", {.connect_timeout = 5s, .read_timeout = 10s});
```

`RequestOptions::idempotency_key` is emitted only as `Idempotency-Key`. It is
never serialized into request JSON or copied into an error message. The same
field-value representability check used for credentials rejects structural
control bytes before a socket without imposing a Venice-specific key syntax.

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

A `StreamDelta` is one choice within an SSE frame: `content`,
`reasoning_content`, `role`,
`finish_reason`, `refusal`, tool-call fragments, `usage`, `cost`, and `chunk`
pointing at the whole verbatim frame. Every field is optional because a frame carries some
of them; an `n > 1` frame produces one callback per choice with the same
`chunk` pointer and a distinct `choice_index`. It is a struct rather than a variant so that a future field is
additive instead of an ABI break. **It is a view** — valid only for the duration
of the callback. Anything worth keeping is already in the accumulator.

The stream boundary fails closed. A successful response must carry
`text/event-stream`; malformed or oversized events and typed-ingest failures
return `ErrorKind::Parse`, while an exception from either callback shape returns
`ErrorKind::InvalidArg` without escaping. In every case, a caller-supplied
`StreamAccumulator` still owns the valid deltas accepted before the failure.
Multiple SSE `data:` fields are newline-joined into one event, and `[DONE]` is
terminal. Callback `false` remains deliberate partial success, and cancellation
still takes precedence over every competing outcome.

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

The content builders spell the documented multimodal and prompt-cache shapes
without closing the raw JSON escape hatch:

```cpp
const auto cache = venice::cache_control::ephemeral("1h");
venice::Message multimodal = venice::Message::user("placeholder");
multimodal.content = nlohmann::json::array({
    venice::message_content::text("Compare these inputs", cache),
    venice::message_content::image_url("https://example.com/image.png"),
    venice::message_content::input_audio("<base64>", "wav"),
    venice::message_content::video_url("https://example.com/video.mp4"),
    venice::message_content::file(
        "data:application/pdf;base64,<base64>", "document.pdf", cache)});
```

`reasoning_details` elements remain raw provider objects and message-level
`thought_signature` remains opaque; both are assign-or-erase modeled fields so
they can be deliberately replayed or withheld like `reasoning_content`.

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
  GIT_TAG        v0.29.8)
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

**The original four smoke legs have all run against the live API** (2026-08-09),
so the "documented, not measured" caveat that covered v0.8.0 through v0.10.0
is retired. `--characters` confirmed every modeled key, its types and the 50-entry
default page; `--stream` confirmed where `reasoning_content` sits and that the
turn replays; `--tools` confirmed a tool call round-trips and is accepted back —
**on the family it happened to pick.** Run against another it was rejected, which
is #29 below. Two things the runs opened rather than closed were filed as #28
(`Usage`'s nested detail objects are modeled but never sent) and #29 (a replayed
tool turn rejected by Gemini-family models); **#29 is fixed in v0.11.0**, and
`--tools` now passes both legs on `gemini-3-6-flash`, `gemini-3-5-flash` and
`zai-org-glm-4.7`.

**Embeddings were measured live in both formats on 2026-08-13.** The selected
`text-embedding-bge-m3` returned a 1,024-element float vector and a 5,464-byte
opaque base64 string for the same input. Raw and typed shapes agreed, with no
unmodeled envelope, usage or entry keys; four alternate embedding models were
reported beside the pick.

**Audio's safe live leg passed on 2026-08-26.** `--audio` named the alternate
TTS, ASR and music models, generated the same 16,845-byte MP3 through buffered
and streamed speech with `tts-kokoro`, transcribed the in-memory bytes with
`nvidia/parakeet-tdt-0.6b-v3`, and quoted `ace-step-15` at $0.03. Transcript and
quote typed values agreed with their verbatim envelopes. The leg deliberately
does not clone a voice, enqueue work, poll, delete, play or write media.

**Video's safe live leg passed on 2026-08-26.** `--video` selected
`seedance-1-5-pro-text-to-video-basic`, named four eligible alternates, took
`4s`, `21:9` and `1080p` from its typed catalogue policy, and received a $0.69
quote whose typed value agreed with the verbatim envelope. It did not enqueue,
poll, delete, decode, display or write anything.

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
401 with a junk bearer.)

**What that costs is no longer hypothetical.** `--character` went unrun for the
whole of v0.14.0 and v0.14.1, and the first time it did run — during v0.15.0,
against a key that had become available — it reported `typed slug: (absent)`.
`/characters/{slug}` answers with an envelope, `{"data": {...}, "object":
"character"}`, and the parse had been reading the envelope as the character
since the operation shipped. The offline fixtures could not catch it because
they were written from the same misreading of the OpenAPI document. Nothing but
the live leg was ever going to disagree.

These commands settle the rest against the live API:

```bash
VENICE_API_KEY=... venice-cpp --stream "..."   # v0.8.0: the reply shapes
VENICE_API_KEY=... venice-cpp --tools          # v0.9.0: the request shape
VENICE_API_KEY=... venice-cpp --characters     # v0.10.0: the character entry
VENICE_API_KEY=... venice-cpp --character SLUG # v0.14.0: detail + v0.15.0: reviews
VENICE_API_KEY=... venice-cpp --usage [model]  # v0.12.0: usage + cost + envelope
VENICE_API_KEY=... venice-cpp --embeddings [model] # v0.18.0: float + base64
VENICE_API_KEY=... venice-cpp --image [model]      # v0.19.0: JSON + media
VENICE_API_KEY=... venice-cpp --image-transform [model] # v0.20.0: four transforms
VENICE_API_KEY=... venice-cpp --audio [tts] [asr] [music] # v0.24.0: speech + transcription + quote
VENICE_API_KEY=... venice-cpp --video [model]             # v0.25.0: catalogue + quote only
VENICE_API_KEY=... venice-cpp --augment                   # v0.26.0: three minimal billed calls
VENICE_API_KEY=... venice-cpp --crypto-rpc                # v0.27.0: discovery + read-only RPC/replay
VENICE_API_KEY=... venice-cpp --billing [lookback] # v0.21.0: balance + analytics + history
VENICE_API_KEY=... venice-cpp --api-keys          # v0.22.0: read-only keys + rate limits

venice-cpp --traits [type]                     # v0.16.0: no key needed
venice-cpp --compat [type]                     # v0.16.0: no key needed
venice-cpp --modality [type]                   # v0.17.0: no key needed
venice-cpp --styles                             # v0.19.0: no key needed
venice-cpp --x402                               # v0.28.0: no key/no-spend payment discovery
```

`--modality` walks **every** entry of every modality rather than picking one,
so what it prints instead of runners-up is a coverage column — how many of a
modality's models carried each modeled key. `promptCharacterLimit 37/37` beside
`maxStyleReferences 4/37` is the per-family variation a single auto-picked
model cannot show, which is the failure #29 and #28 were both filed for. It
also differences the unmodeled keys at every nesting level, reconciles each
typed field against `raw` in both directions, and reports the modeled keys the
wire has never sent without failing on them.

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
