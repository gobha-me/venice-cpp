#pragma once

// venice-cpp — request/response types for the Venice API.
//
// These model the OpenAI-compatible /chat/completions contract plus Venice's
// `venice_parameters` extension. Plain structs, nlohmann/json (de)serialization
// via to_json/from_json free functions.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace venice {

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

// ── models ────────────────────────────────────────────────────────────────

struct Model {
  std::string id;
  std::string type;  // "text" | "image" | "video" | "tts" | "embedding" | ...

  friend void from_json(const nlohmann::json& j, Model& m) {
    if (j.contains("id")) j.at("id").get_to(m.id);
    if (j.contains("type")) j.at("type").get_to(m.type);
  }
};

}  // namespace venice
