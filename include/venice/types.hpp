#pragma once

// venice-cpp — request/response types for the Venice API.
//
// These model the OpenAI-compatible /chat/completions contract plus Venice's
// `venice_parameters` extension. Plain structs, nlohmann/json (de)serialization
// via to_json/from_json free functions.

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

// ── chat request ──────────────────────────────────────────────────────────

struct ChatRequest {
  std::string model;
  std::vector<Message> messages;
  std::optional<double> temperature;
  std::optional<int> max_tokens;
  std::optional<VeniceParameters> venice_parameters;
  bool stream{false};

  // Serialize to the wire body. `stream` is emitted as "stream": true|false.
  [[nodiscard]] auto to_json_body() const -> nlohmann::json {
    nlohmann::json j{{"model", model}, {"messages", messages}, {"stream", stream}};
    if (temperature) j["temperature"] = *temperature;
    if (max_tokens) j["max_tokens"] = *max_tokens;
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
