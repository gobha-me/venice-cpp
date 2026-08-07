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

- **Chat completions** (`/chat/completions`) — non-streaming and streaming
  (SSE via callback).
- **Sampling and control parameters** — `temperature`, `top_p`, `max_tokens`,
  `stop`, `frequency_penalty`, `presence_penalty`, `seed`, `response_format`,
  plus a top-level `extra` passthrough for keys this struct doesn't model.
- **`venice_parameters`** extension — web search, citations, character,
  thinking toggles, with forward-compatible passthrough for unmodeled keys.
- **Models list** (`/models`) — typed metadata per model: context window,
  fourteen capability flags (function calling, vision, reasoning, web search,
  …), and a full rate card with cache buckets and extended-context tiers, plus
  the verbatim entry as `raw`. Filterable by modality — `models("image")`,
  `models("all")` — since the endpoint's own default is text-only.
- **Balance / rate limits** (`/api_keys/rate_limits`).
- **Per-request timeouts and cancellation** — every call takes an optional
  `RequestOptions` with connect/read/write timeout overrides and a
  `CancelToken` that aborts an in-flight request from another thread, including
  one that has received nothing at all.
- **Error model** — `std::expected<T, venice::Error>`; network / HTTP / parse /
  auth / rate-limit / invalid-arg / cancelled, each carrying status + raw body.

Later phases (fed by real use): image/audio/video, TTS, embeddings,
characters, retries/backoff, async.

## Dependencies

Header-only library; consumers link the CMake target and get everything
transitively:

- [cpp-httplib](https://github.com/yhirose/cpp-httplib) — HTTP transport (header-only)
- [nlohmann/json](https://github.com/nlohmann/json) — JSON (header-only)
- **OpenSSL** — TLS (the one link-time dependency; reimplementing TLS is
  malpractice, so we link it)

## Usage

```cpp
#include <venice/venice.hpp>

venice::Client client{std::getenv("VENICE_API_KEY")};

venice::ChatRequest req;
req.model = "llama-3.3-70b";
req.messages = {venice::Message::user("Hello")};

// sampling controls — every one is optional; unset fields are never serialized
req.temperature = 0.7;
req.top_p = 0.9;
req.stop = std::vector<std::string>{"\n\n"};
req.seed = 42;
req.response_format = venice::response_format::json_object();

// non-streaming
if (auto res = client.chat(req)) {
  // res->content, res->usage, res->finish_reason
} else {
  // res.error().kind, .status, .message, .body
}

// streaming (SSE): on_token returns false to cancel early
auto s = client.chat_stream(req, [](std::string_view delta) {
  // print/handle delta
  return true;
});
```

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
for (const auto& m : *client.models()) {
  if (!m.capabilities || m.capabilities->supports_function_calling != true) continue;
  if (!m.context_length || *m.context_length < 200000) continue;

  // per-million-token input rate, if quoted
  if (m.pricing && m.pricing->base.input && m.pricing->base.input->usd)
    std::cout << m.id << " $" << *m.pricing->base.input->usd << '\n';
}
```

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

Some models reprice past a context threshold, so a cost estimate picks a tier:

```cpp
const auto& p = *m.pricing;
const venice::PriceTier& tier =
    (p.extended && p.extended_threshold_tokens && tokens > *p.extended_threshold_tokens)
        ? *p.extended : p.base;
```

Note that pricing carries **two** cache buckets (`cache_input` for a read,
`cache_write` for populating it) while `Usage` reports only one
(`cached_tokens`), so a response cannot yet be paired 1:1 with the rate card.

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
  GIT_TAG        v0.7.0)
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
counts move as Venice's catalogue does. See `AGENTS.md` for contributor/agent
conventions and the testing philosophy (test how it fails, not just the happy
path).
