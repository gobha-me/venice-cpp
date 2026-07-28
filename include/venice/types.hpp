#pragma once

// venice-cpp — request/response types for the Venice API.
//
// These model the OpenAI-compatible /chat/completions contract plus Venice's
// `venice_parameters` extension. Plain structs, nlohmann/json (de)serialization
// via to_json/from_json free functions.

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace venice {

// ── tolerant reads ────────────────────────────────────────────────────────
//
// Response-side helpers for fields the API may omit, send as null, or — the
// case that actually bites — send with the wrong type. Each returns nullopt
// instead of throwing, which is what "never an error" means for a listing
// endpoint: one odd field in one entry must not cost the caller the other
// hundred models.
//
// Type *predicates*, not try/catch — and for the numeric ones that is not a
// stylistic preference, because try/catch would not fire. Measured against the
// pinned nlohmann 3.11.3:
//
//   get<int>() on 1.9              -> 1              (no throw)
//   get<int>() on 99999999999999   -> 276447231      (no throw, no UBSan trip:
//                                                     the narrowing is
//                                                     well-defined since C++20)
//   get<int>() on "x"              -> throws type_error.302
//
// So a guard that only catches exceptions turns a wrong-typed number into a
// confident wrong answer, and a context window or a price is exactly the kind
// of number nobody re-reads. Predicates first; range check after.
//
// These are deliberately not retrofitted onto Usage or ChatResponse. Those
// parse inside Client::chat's try/catch, where a malformed body *should* fail
// loudly as ErrorKind::Parse — a chat reply has no other entries to protect,
// and silently zeroing a token count would hide a billing bug. Tolerance is a
// property of listings, not of parsing in general.
//
// One rule these must never break, the same one test/02request/ states: never
// hand a std::optional to nlohmann. cmake/deps/nlohmann_json.cmake falls back
// to a pinned v3.11.3 and optional support landed in 3.12.0, so
// `j.get<std::optional<int>>()` fails to compile on the pin while passing on a
// newer system copy. Every helper below returns an optional built in C++, and
// none is ever handed back.

namespace detail {

[[nodiscard]] inline auto opt_bool(const nlohmann::json& j, const char* key)
    -> std::optional<bool> {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_boolean()) return std::nullopt;
  return it->get<bool>();
}

[[nodiscard]] inline auto opt_i64(const nlohmann::json& j, const char* key)
    -> std::optional<std::int64_t> {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_number_integer()) return std::nullopt;
  return it->get<std::int64_t>();
}

// Via opt_i64, then range-checked. is_number_integer() is also true for
// number_unsigned, so a value past INT_MAX passes the predicate and then
// narrows silently — see the header note. A number this field cannot represent
// is unparseable, i.e. nullopt; it is never a truncated one.
[[nodiscard]] inline auto opt_int(const nlohmann::json& j, const char* key)
    -> std::optional<int> {
  const auto v = opt_i64(j, key);
  if (!v || *v < std::numeric_limits<int>::min() || *v > std::numeric_limits<int>::max())
    return std::nullopt;
  return static_cast<int>(*v);
}

// is_number(), not is_number_float(). Venice quotes prices as whole numbers
// when they are whole: a single /models payload carries "output":{"usd":2}
// beside "input":{"usd":1.875}. Narrowing this to floats silently drops every
// integral price — twenty of the hundred-odd output prices in the live payload
// captured while this was written — and nothing else in the parse would notice.
[[nodiscard]] inline auto opt_double(const nlohmann::json& j, const char* key)
    -> std::optional<double> {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_number()) return std::nullopt;
  return it->get<double>();
}

[[nodiscard]] inline auto opt_string(const nlohmann::json& j, const char* key)
    -> std::optional<std::string> {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_string()) return std::nullopt;
  return it->get<std::string>();
}

// The stored string itself, or nullptr — for the two fields that land in a
// plain std::string and for tests that need to look before copying. Borrowed
// from `j`, so it dies with it.
[[nodiscard]] inline auto opt_string_at(const nlohmann::json& j, const char* key)
    -> const nlohmann::json::string_t* {
  const auto it = j.find(key);
  if (it == j.end()) return nullptr;
  return it->get_ptr<const nlohmann::json::string_t*>();
}

// The nested object a modeled sub-struct parses from, or nullptr. Callers get
// to write one `if` instead of repeating the find/is_object dance, and a
// `"pricing": []` — an array where an object belongs — reads as absent rather
// than throwing out of operator[].
[[nodiscard]] inline auto opt_object(const nlohmann::json& j, const char* key)
    -> const nlohmann::json* {
  const auto it = j.find(key);
  return it != j.end() && it->is_object() ? &*it : nullptr;
}

// As opt_object, for arrays. Distinct from "absent" only to the caller that
// cares — models_from_json_body uses it to tell a `data` that is a list from a
// `data` that is an object, which nlohmann would otherwise iterate happily.
[[nodiscard]] inline auto opt_array(const nlohmann::json& j, const char* key)
    -> const nlohmann::json* {
  const auto it = j.find(key);
  return it != j.end() && it->is_array() ? &*it : nullptr;
}

// Every string in a string array, skipping anything that is not one. A
// non-array yields an empty vector — `traits` is "what this model is tagged
// with", and no tags is a truthful answer to a malformed tag list.
[[nodiscard]] inline auto string_array(const nlohmann::json& j, const char* key)
    -> std::vector<std::string> {
  std::vector<std::string> out;
  const auto it = j.find(key);
  if (it == j.end() || !it->is_array()) return out;
  for (const auto& e : *it)
    if (e.is_string()) out.push_back(e.get<std::string>());
  return out;
}

}  // namespace detail

// ── messages ──────────────────────────────────────────────────────────────

struct Message {
  std::string role;     // "system" | "user" | "assistant" | "tool"
  std::string content;

  friend void to_json(nlohmann::json& j, const Message& m) {
    j = nlohmann::json{{"role", m.role}, {"content", m.content}};
  }
  friend void from_json(const nlohmann::json& j, Message& m) {
    j.at("role").get_to(m.role);
    j.at("content").get_to(m.content);
  }

  static auto system(std::string c) -> Message { return {"system", std::move(c)}; }
  static auto user(std::string c) -> Message { return {"user", std::move(c)}; }
  static auto assistant(std::string c) -> Message { return {"assistant", std::move(c)}; }
};

// ── venice_parameters (the Venice extension block) ───────────────────────
//
// Only set fields are serialized. Mirrors the flags venice-cli surfaces;
// unknown future keys pass through verbatim via `extra`.

struct VeniceParameters {
  std::optional<std::string> enable_web_search;    // "auto" | "on" | "off"
  std::optional<bool> enable_web_citations;
  std::optional<bool> enable_web_scraping;
  std::optional<std::string> character_slug;
  std::optional<bool> include_venice_system_prompt;
  std::optional<bool> strip_thinking_response;
  std::optional<bool> disable_thinking;
  std::optional<bool> enable_x_search;
  nlohmann::json extra;  // forward-compatible passthrough for unmodeled keys

  friend void to_json(nlohmann::json& j, const VeniceParameters& p) {
    j = p.extra.is_object() ? p.extra : nlohmann::json::object();
    if (p.enable_web_search) j["enable_web_search"] = *p.enable_web_search;
    if (p.enable_web_citations) j["enable_web_citations"] = *p.enable_web_citations;
    if (p.enable_web_scraping) j["enable_web_scraping"] = *p.enable_web_scraping;
    if (p.character_slug) j["character_slug"] = *p.character_slug;
    if (p.include_venice_system_prompt)
      j["include_venice_system_prompt"] = *p.include_venice_system_prompt;
    if (p.strip_thinking_response) j["strip_thinking_response"] = *p.strip_thinking_response;
    if (p.disable_thinking) j["disable_thinking"] = *p.disable_thinking;
    if (p.enable_x_search) j["enable_x_search"] = *p.enable_x_search;
  }
};

// ── response_format builders ──────────────────────────────────────────────
//
// `ChatRequest::response_format` is raw JSON rather than an enum: the API
// accepts both {"type":"json_object"} and a full
// {"type":"json_schema","json_schema":{...}} block, and an enum cannot carry a
// schema. These builders supply the ergonomics an enum would have; anything
// they don't cover, assign the object directly.
//
// Free functions, not statics on a struct — a zero-member type whose statics
// return nlohmann::json would invite `ResponseFormat rf = ...;`, which cannot
// compile. Same shape as venice::to_string / venice::kind_for_status.

namespace response_format {

inline auto text() -> nlohmann::json {
  auto j = nlohmann::json::object();
  j["type"] = "text";
  return j;
}

inline auto json_object() -> nlohmann::json {
  auto j = nlohmann::json::object();
  j["type"] = "json_object";
  return j;
}

// Field-by-field assignment, not a brace-init list: an array-valued `schema`
// makes the outer initializer-list ambiguous and nlohmann reads the whole
// thing as an array.
inline auto json_schema(std::string name, nlohmann::json schema, bool strict = true)
    -> nlohmann::json {
  auto inner = nlohmann::json::object();
  inner["name"] = std::move(name);
  inner["schema"] = std::move(schema);
  inner["strict"] = strict;

  auto j = nlohmann::json::object();
  j["type"] = "json_schema";
  j["json_schema"] = std::move(inner);
  return j;
}

}  // namespace response_format

// ── chat request ──────────────────────────────────────────────────────────
//
// Ranges are not checked client-side; representability is. The line this
// library draws: *structural* preconditions that make a request unsendable by
// construction — an empty model, no messages, a non-finite double, since JSON
// has no NaN or infinity — are ErrorKind::InvalidArg, raised by Client::validate
// before any transport. *Value-range policy the server owns* (temperature 0-2,
// top_p 0-1, penalties -2..2) is transmitted verbatim, because a bound
// hardcoded here goes stale the moment Venice widens it.
//
// The same principle governs engaged-but-degenerate optionals: an engaged
// `stop` holding an empty vector serializes as "stop": [], and an engaged
// `response_format` holding a default-constructed (null) json serializes as
// null. The caller said something; silently discarding it is a harder bug to
// diagnose than the 400 that follows.

struct ChatRequest {
  std::string model;
  std::vector<Message> messages;
  std::optional<double> temperature;
  std::optional<double> top_p;
  std::optional<int> max_tokens;
  std::optional<std::vector<std::string>> stop;  // always sent as an array
  std::optional<double> frequency_penalty;
  std::optional<double> presence_penalty;
  // Wider than max_tokens on purpose, not by oversight: callers seed from
  // std::random_device / mt19937, which yield uint32_t values up to
  // 4294967295 — past INT_MAX. max_tokens is bounded by the context window and
  // stays int.
  std::optional<std::int64_t> seed;
  std::optional<nlohmann::json> response_format;  // see the builders above
  std::optional<VeniceParameters> venice_parameters;
  // Forward-compatible top-level passthrough, mirroring VeniceParameters::extra.
  // Venice accepts sampling keys this struct doesn't model (top_k, min_p,
  // repetition_penalty); without this they'd be unreachable without forking the
  // header. Modeled fields always win over a same-named key here.
  nlohmann::json extra;

  // Serialize to the wire body.
  //
  // `stream` is a parameter, not a member: it describes how the request is
  // *sent*, not what it is, and the two Client entry points each already know
  // their own answer. Taking it here is what lets both serialize straight from
  // a `const ChatRequest&` — before this, each copied the whole request,
  // `extra`'s arbitrarily deep json tree included, to flip one bool. With no
  // `stream` member left there is no second source of truth for that bit, and
  // no mutable state a copy could exist to modify.
  //
  // No default argument, on purpose. The defect this signature retires is a bit
  // that looks set and silently never reaches the wire; a defaulted `stream`
  // rebuilds that shape one level up, where a hand-built SSE body would quietly
  // say "stream": false and the server would simply not stream.
  [[nodiscard]] auto to_json_body(bool stream) const -> nlohmann::json {
    // is_object() guard as in VeniceParameters::to_json: a default-constructed
    // json is null, and an array- or number-valued `extra` would make the
    // operator[] below throw type_error.305 straight out of a function the
    // client calls with no try/catch — an exception escaping the public API.
    nlohmann::json j = extra.is_object() ? extra : nlohmann::json::object();
    j["model"] = model;
    j["messages"] = messages;
    // Assigned unconditionally after the `extra` seed, like every other modeled
    // key: modeled fields win. An extra["stream"] never gets a vote, because it
    // cannot change how the transport is actually driven — the Client method the
    // caller chose already decided that, and a body claiming "stream": true
    // under a non-SSE read is a hang, not a preference.
    j["stream"] = stream;
    if (temperature) j["temperature"] = *temperature;
    if (top_p) j["top_p"] = *top_p;
    if (max_tokens) j["max_tokens"] = *max_tokens;
    if (stop) j["stop"] = *stop;
    if (frequency_penalty) j["frequency_penalty"] = *frequency_penalty;
    if (presence_penalty) j["presence_penalty"] = *presence_penalty;
    if (seed) j["seed"] = *seed;
    if (response_format) j["response_format"] = *response_format;
    if (venice_parameters) j["venice_parameters"] = *venice_parameters;
    return j;
  }
};

// ── usage / cost metadata ─────────────────────────────────────────────────
//
// Venice returns prompt/completion token counts; some responses also carry
// cache breakdown. Keep buckets distinct (cache-read prices differently) —
// see venice-cli #75.

struct Usage {
  int prompt_tokens{0};
  int completion_tokens{0};
  int total_tokens{0};
  std::optional<int> cached_tokens;  // cache-read tokens, when reported

  friend void from_json(const nlohmann::json& j, Usage& u) {
    if (j.contains("prompt_tokens")) j.at("prompt_tokens").get_to(u.prompt_tokens);
    if (j.contains("completion_tokens")) j.at("completion_tokens").get_to(u.completion_tokens);
    if (j.contains("total_tokens")) j.at("total_tokens").get_to(u.total_tokens);
    if (j.contains("cached_tokens")) u.cached_tokens = j.at("cached_tokens").get<int>();
  }
};

// ── chat response ─────────────────────────────────────────────────────────

struct ChatResponse {
  std::string id;
  std::string model;
  std::string content;          // assistant message text (choices[0].message.content)
  std::string finish_reason;    // "stop" | "length" | ...
  std::optional<Usage> usage;

  // Parse the non-streaming /chat/completions response body. Throws
  // nlohmann::json exceptions on malformed input — callers wrap in expected.
  static auto from_json_body(const nlohmann::json& j) -> ChatResponse {
    ChatResponse r;
    if (j.contains("id")) r.id = j.at("id").get<std::string>();
    if (j.contains("model")) r.model = j.at("model").get<std::string>();
    const auto& choices = j.at("choices");
    if (!choices.empty()) {
      const auto& c0 = choices.at(0);
      if (c0.contains("message") && c0.at("message").contains("content"))
        r.content = c0.at("message").at("content").get<std::string>();
      if (c0.contains("finish_reason") && !c0.at("finish_reason").is_null())
        r.finish_reason = c0.at("finish_reason").get<std::string>();
    }
    if (j.contains("usage") && !j.at("usage").is_null()) r.usage = j.at("usage").get<Usage>();
    return r;
  }
};


// ── model pricing ─────────────────────────────────────────────────────────
//
// Venice quotes every bucket in two currencies at once: USD and `diem`, its
// credit unit. Both are optional because a bucket may carry either, and
// neither is derivable from the other — a ledger denominated in credits must
// not have to reconstruct them from dollars at a rate this library would be
// guessing at.

struct Price {
  std::optional<double> usd;
  std::optional<double> diem;

  friend void from_json(const nlohmann::json& j, Price& p) {
    p.usd = detail::opt_double(j, "usd");
    p.diem = detail::opt_double(j, "diem");
  }
};

// One complete set of rates. Buckets stay distinct for the same reason
// Usage::cached_tokens does: they price differently, and a client that folds
// them together cannot tell a cheap cache hit from a full-rate prompt
// (venice-cli #75). Text rates are per million tokens.
//
// Worth knowing before building a cost estimate on this: pricing has *two*
// cache buckets — cache_input for a read, cache_write for populating it —
// while Usage reports only one (`cached_tokens`, types.hpp above). So the
// response cannot currently be paired 1:1 with the rate card, and anything
// that claims to is assuming which of the two it got.
struct PriceTier {
  std::optional<Price> input;
  std::optional<Price> output;
  std::optional<Price> cache_input;  // cache read
  std::optional<Price> cache_write;

  friend void from_json(const nlohmann::json& j, PriceTier& t) {
    struct Bucket {
      std::optional<Price> PriceTier::*field;
      const char* key;
    };
    static constexpr std::array<Bucket, 4> kBuckets{{
        {&PriceTier::input, "input"},
        {&PriceTier::output, "output"},
        {&PriceTier::cache_input, "cache_input"},
        {&PriceTier::cache_write, "cache_write"},
    }};

    for (const auto& [field, key] : kBuckets)
      if (const auto* o = detail::opt_object(j, key)) t.*field = o->get<Price>();
  }
};

// A model's rate card: the base tier, and for some models a second tier that
// takes over past a context threshold.
//
// `extended` is the same type as `base` on purpose. Ten of the hundred-odd
// text models reprice past their threshold — by 3x on one of them — and a
// caller that has to reach into raw json for the second table reimplements
// both the nested walk and the int-vs-float trap, and gets one of them wrong.
// Sharing the type makes tier selection a single expression:
//
//   const PriceTier& t = (p.extended && p.extended_threshold_tokens &&
//                         tokens > *p.extended_threshold_tokens)
//                            ? *p.extended : p.base;
//
// That expression is deliberately *not* shipped as a member. Venice documents
// the threshold as `context_token_threshold` without saying whether it counts
// prompt tokens or prompt plus completion, and the two give different answers
// near the boundary. Shipping the data commits to nothing; shipping the
// accessor would encode a guess as an API.
struct Pricing {
  PriceTier base;
  std::optional<std::int64_t> extended_threshold_tokens;
  std::optional<PriceTier> extended;

  friend void from_json(const nlohmann::json& j, Pricing& p) {
    p.base = j.get<PriceTier>();
    if (const auto* ext = detail::opt_object(j, "extended")) {
      p.extended = ext->get<PriceTier>();
      p.extended_threshold_tokens = detail::opt_i64(*ext, "context_token_threshold");
    }
  }
};

// ── model capabilities ────────────────────────────────────────────────────
//
// The flag block Venice attaches to text models. Every flag is modeled rather
// than a chosen subset: all fourteen appear on all 106 text models, so the set
// is the observed schema rather than a guess about which ones a caller will
// want, and it costs one table row each instead of a defence of where the line
// was drawn.
//
// All optional, none defaulted to false. "This model does not support vision"
// and "this response did not say" are different answers, and only one of them
// is safe to build a request on.

struct ModelCapabilities {
  std::optional<bool> supports_function_calling;
  std::optional<bool> supports_vision;
  std::optional<bool> supports_multiple_images;
  std::optional<bool> supports_video_input;
  std::optional<bool> supports_audio_input;
  std::optional<bool> supports_reasoning;
  std::optional<bool> supports_reasoning_effort;
  std::optional<bool> supports_response_schema;
  std::optional<bool> supports_log_probs;
  std::optional<bool> supports_web_search;
  std::optional<bool> supports_x_search;
  std::optional<bool> supports_tee_attestation;
  std::optional<bool> supports_e2ee;
  std::optional<bool> optimized_for_code;

  std::optional<std::string> quantization;
  std::optional<int> max_images;
  // Modeled alongside supports_reasoning_effort rather than left to the raw
  // entry: a flag saying effort is configurable, with no way to learn which
  // efforts exist, is the half-answer that makes callers fork the header.
  std::optional<std::string> default_reasoning_effort;
  std::vector<std::string> reasoning_effort_options;
};

namespace detail {

// Wire key ↔ field, so the two travel together and a flag cannot be added to
// the struct while being forgotten in the parse. Same shape as
// Client::validate's kDoubleFields.
//
// At namespace scope, and not private, because that is what makes it
// checkable: test/04models/ iterates this table against a verbatim captured
// payload and asserts every key is one the API actually sends. A typo'd key
// yields nullopt forever and is invisible to any test that hand-copies the
// same typo — the live payload is the only honest oracle.
struct CapabilityFlag {
  std::optional<bool> ModelCapabilities::*field;
  const char* key;
};

inline constexpr std::array<CapabilityFlag, 14> kCapabilityBoolFields{{
    {&ModelCapabilities::supports_function_calling, "supportsFunctionCalling"},
    {&ModelCapabilities::supports_vision, "supportsVision"},
    {&ModelCapabilities::supports_multiple_images, "supportsMultipleImages"},
    {&ModelCapabilities::supports_video_input, "supportsVideoInput"},
    {&ModelCapabilities::supports_audio_input, "supportsAudioInput"},
    {&ModelCapabilities::supports_reasoning, "supportsReasoning"},
    {&ModelCapabilities::supports_reasoning_effort, "supportsReasoningEffort"},
    {&ModelCapabilities::supports_response_schema, "supportsResponseSchema"},
    {&ModelCapabilities::supports_log_probs, "supportsLogProbs"},
    {&ModelCapabilities::supports_web_search, "supportsWebSearch"},
    {&ModelCapabilities::supports_x_search, "supportsXSearch"},
    {&ModelCapabilities::supports_tee_attestation, "supportsTeeAttestation"},
    {&ModelCapabilities::supports_e2ee, "supportsE2EE"},
    {&ModelCapabilities::optimized_for_code, "optimizedForCode"},
}};

}  // namespace detail

// Free rather than a friend, because the table above must be declared first
// and it needs ModelCapabilities complete. Found by ADL all the same.
inline void from_json(const nlohmann::json& j, ModelCapabilities& c) {
  for (const auto& [field, key] : detail::kCapabilityBoolFields)
    c.*field = detail::opt_bool(j, key);

  c.quantization = detail::opt_string(j, "quantization");
  c.max_images = detail::opt_int(j, "maxImages");
  c.default_reasoning_effort = detail::opt_string(j, "defaultReasoningEffort");
  c.reasoning_effort_options = detail::string_array(j, "reasoningEffortOptions");
}

// ── models ────────────────────────────────────────────────────────────────
//
// One entry from /models. `id` and `type` are the two fields the endpoint
// always sets and every caller needs, so they stay plain strings; everything
// else is optional, because `model_spec` is polymorphic by model type. Text
// models carry capabilities and a context window; image models carry
// generation pricing and style-reference flags; tts carries a voice list. The
// typed surface here is the text shape — the one this client's chat endpoints
// can actually use — and `raw` keeps the rest.
//
// `raw` is the escape hatch, and three things about it are deliberate:
//
//  * It holds the **whole entry**, not just `model_spec`, so top-level keys
//    have somewhere to go and the value round-trips: a downstream cache can
//    persist a model list and rehydrate it without this struct growing a
//    to_json.
//  * It is a **superset**, not a complement — modeled fields are still in
//    there. A subtractive hatch breaks its readers on every graduation: code
//    reading raw["betaModel"] would silently start getting null the release
//    `beta_model` became typed.
//  * It is **not called `extra`**. ChatRequest::extra and
//    VeniceParameters::extra are request-side and additive — their contract is
//    "modeled fields win over these keys on the wire." This one is never sent
//    and contains everything. Same name, inverted rule, so a different name.
//
// It is also the reason a Model is a heavyweight value (~1-2 KB); move or take
// by const reference rather than copying a listing around.

struct Model {
  std::string id;
  std::string type;  // "text" | "image" | "video" | "tts" | "embedding" | ...

  std::optional<std::string> name;  // human label, e.g. "Qwen 3.7 Plus"
  std::optional<std::string> description;
  std::optional<std::string> owned_by;
  std::optional<std::string> privacy;       // "private" | "anonymized" | ...
  std::optional<std::string> model_source;  // upstream weights/docs URL
  // Epoch seconds. Wide on purpose, like ChatRequest::seed: the largest value
  // in the live payload is ~1.78e9 and fits int32, and stops fitting in 2038.
  // A field whose type is correct only until a specific date is a defect with
  // a due date.
  std::optional<std::int64_t> created;

  // Two token budgets from two places, kept as two fields. `context_length` is
  // top-level; `available_context_tokens` is model_spec's spelling. They hold
  // the same number on all 106 text models today — checked, not assumed — but
  // coalescing them would make one field silently mean different things on
  // different entries the day that stops being true.
  std::optional<int> context_length;
  std::optional<int> available_context_tokens;
  std::optional<int> max_completion_tokens;  // cap on a single reply

  std::optional<bool> offline;  // temporarily unavailable
  std::optional<bool> beta_model;
  // Plain vector, not optional: empty is the only degenerate value and no
  // caller behaves differently for absent-vs-empty. "No tags" is a truthful
  // answer to both.
  std::vector<std::string> traits;

  std::optional<ModelCapabilities> capabilities;
  std::optional<Pricing> pricing;

  nlohmann::json raw;  // the verbatim entry — see the note above

  friend void from_json(const nlohmann::json& j, Model& m) {
    if (const auto* s = detail::opt_string_at(j, "id")) m.id = *s;
    if (const auto* s = detail::opt_string_at(j, "type")) m.type = *s;

    m.raw = j;
    m.owned_by = detail::opt_string(j, "owned_by");
    m.created = detail::opt_i64(j, "created");
    m.context_length = detail::opt_int(j, "context_length");

    const auto* spec = detail::opt_object(j, "model_spec");
    if (spec == nullptr) return;

    m.name = detail::opt_string(*spec, "name");
    m.description = detail::opt_string(*spec, "description");
    m.privacy = detail::opt_string(*spec, "privacy");
    m.model_source = detail::opt_string(*spec, "modelSource");
    m.offline = detail::opt_bool(*spec, "offline");
    m.beta_model = detail::opt_bool(*spec, "betaModel");
    m.traits = detail::string_array(*spec, "traits");
    m.available_context_tokens = detail::opt_int(*spec, "availableContextTokens");
    m.max_completion_tokens = detail::opt_int(*spec, "maxCompletionTokens");

    if (const auto* caps = detail::opt_object(*spec, "capabilities"))
      m.capabilities = caps->get<ModelCapabilities>();
    if (const auto* price = detail::opt_object(*spec, "pricing"))
      m.pricing = price->get<Pricing>();
  }
};

// Parse a /models response body into a listing.
//
// A free function rather than a static on Model, because what it produces is a
// vector and `Model::from_json_body` returning a list of somethings-else reads
// wrong — the same reasoning that made the response_format builders free
// functions above. It is separate from Client so the whole failure matrix is
// reachable offline, without a socket: this is the unit test/04models/ tests.
//
// Exactly one thing here is fatal. If the payload is not a list at all, there
// is nothing to degrade *to*, so it throws and Client reports ErrorKind::Parse.
// The check is load-bearing rather than defensive: iterating a json *object*
// yields its values, and iterating a scalar yields the scalar, so without it a
// `{"data": {...}}` body quietly becomes a vector of models built out of
// whatever the values happened to be — a garbage-model factory that reports
// success.
//
// Everything below that degrades instead. An element that is not an object, or
// that carries no usable `id`, is skipped rather than dropped-on: the id is the
// one field with no optional around it, and the whole point of a Model is to
// name something you can hand to chat(). An entry that cannot fill it is not a
// degraded model, it is not a model — and keeping it would ship a value whose
// only future is to come back as "model is empty" from Client::validate, far
// from here. A *partial* entry is a different case and is kept: absent fields
// are absent, which is what the caller asked to be told.
[[nodiscard]] inline auto models_from_json_body(const nlohmann::json& j) -> std::vector<Model> {
  const auto* data = detail::opt_array(j, "data");
  const nlohmann::json& arr = data != nullptr ? *data : j;
  if (!arr.is_array())
    throw std::runtime_error{"models: response is not a list"};

  std::vector<Model> out;
  for (const auto& e : arr) {
    if (!e.is_object()) continue;
    const auto* id = detail::opt_string_at(e, "id");
    if (id == nullptr || id->empty()) continue;
    out.push_back(e.get<Model>());
  }
  return out;
}

}  // namespace venice
