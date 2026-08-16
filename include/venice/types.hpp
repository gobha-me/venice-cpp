#pragma once

// venice-cpp — request/response types for the Venice API.
//
// These model the OpenAI-compatible /chat/completions contract plus Venice's
// `venice_parameters` extension. Plain structs, nlohmann/json (de)serialization
// via to_json/from_json free functions.

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

// ToolCall::parsed_arguments hands a malformed-arguments failure back rather
// than throwing. error.hpp includes nothing from here, so this is one-way.
#include "venice/error.hpp"

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

// Every string in a string array, skipping anything that is not one, with
// "absent" and "present but not an array" kept distinct from "present and
// empty".
//
// That distinction is not always worth having, which is why there are two
// helpers rather than one. `Model::traits` takes the plain vector below,
// because no caller behaves differently for absent-vs-empty. Video's
// `aspect_ratios` takes this one, because the specification assigns the empty
// array a meaning of its own — "the model does not support a defined aspect
// ratio" — and 40 of the 111 video models sent exactly that on 2026-08-11.
// Flattening the two there would answer "what ratios may I ask for?" with
// silence in both the case where the answer is none and the case where the
// server never said.
//
// Neither helper can tell `[]` from an array whose every element was
// unusable — both yield an engaged empty vector. That is recoverable through
// `raw` and deliberately not modeled: a third state here would have to be
// carried by every caller to be worth anything.
[[nodiscard]] inline auto opt_string_array(const nlohmann::json& j, const char* key)
    -> std::optional<std::vector<std::string>> {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_array()) return std::nullopt;
  std::vector<std::string> out;
  for (const auto& e : *it)
    if (e.is_string()) out.push_back(e.get<std::string>());
  return out;
}

// As opt_string_array, with absent and malformed both flattened onto empty.
[[nodiscard]] inline auto string_array(const nlohmann::json& j, const char* key)
    -> std::vector<std::string> {
  return opt_string_array(j, key).value_or(std::vector<std::string>{});
}

// ── wire key ↔ field tables ───────────────────────────────────────────────
//
// A row pairs a pointer-to-member with the key it is read from, so the two
// travel together and a field cannot be added to a struct while being
// forgotten in the parse. kCapabilityBoolFields established the shape; VC-39
// generalised it because the per-modality structs need the same table five
// times over in four different field types.
//
// Every table is at namespace scope and none is private, because that is what
// makes them checkable: test/10modalities/ iterates them against verbatim
// captured payloads in both directions — every table key is one the API sent,
// and every key the API sent is in some table. A typo'd key yields nullopt
// forever and is invisible to any test that hand-copies the same typo, and a
// deleted row is invisible to the forward check alone.
//
// The reader is the member type, not a guess from it: `Field<int, T>` is read
// with opt_int and `Field<double, T>` with opt_double, and picking the wrong
// one is what break #7 of the VC-39 matrix demonstrates you cannot do by
// accident.
template <typename T, typename Struct>
struct Field {
  std::optional<T> Struct::*field;
  const char* key;
};

template <typename T, typename Struct>
struct VectorField {
  std::optional<std::vector<T>> Struct::*field;
  const char* key;
};

// A nested modeled object — `steps`, `voice_cloning`, `constraints`, each text
// sampling parameter. Same shape as Field, under its own name because the
// reader differs: opt_object then the sub-struct's own from_json, so a
// `"constraints": []` reads as absent rather than throwing.
//
// Its own name rather than a Field overload, so that adding a modeled struct
// type can never silently select a scalar reader.
template <typename T, typename Struct>
struct ObjectField {
  std::optional<T> Struct::*field;
  const char* key;
};

template <typename Struct, std::size_t N>
inline void read_table(const nlohmann::json& j, Struct& s,
                       const std::array<Field<bool, Struct>, N>& table) {
  for (const auto& [field, key] : table) s.*field = opt_bool(j, key);
}

template <typename Struct, std::size_t N>
inline void read_table(const nlohmann::json& j, Struct& s,
                       const std::array<Field<int, Struct>, N>& table) {
  for (const auto& [field, key] : table) s.*field = opt_int(j, key);
}

template <typename Struct, std::size_t N>
inline void read_table(const nlohmann::json& j, Struct& s,
                       const std::array<Field<double, Struct>, N>& table) {
  for (const auto& [field, key] : table) s.*field = opt_double(j, key);
}

template <typename Struct, std::size_t N>
inline void read_table(const nlohmann::json& j, Struct& s,
                       const std::array<Field<std::string, Struct>, N>& table) {
  for (const auto& [field, key] : table) s.*field = opt_string(j, key);
}

template <typename Struct, std::size_t N>
inline void read_table(const nlohmann::json& j, Struct& s,
                       const std::array<VectorField<std::string, Struct>, N>& table) {
  for (const auto& [field, key] : table) s.*field = opt_string_array(j, key);
}

template <typename Struct, typename T, std::size_t N>
inline void read_table(const nlohmann::json& j, Struct& s,
                       const std::array<ObjectField<T, Struct>, N>& table) {
  for (const auto& [field, key] : table)
    if (const auto* nested = opt_object(j, key)) s.*field = nested->template get<T>();
}

}  // namespace detail

// ── tool calls ────────────────────────────────────────────────────────────
//
// The assistant's request to call a function. Modeled on the response side
// (VC-05); the request-side `tools` array that offers functions in the first
// place is VC-08 (#9). The split is not arbitrary — the streaming accumulator
// cannot be specified without the merge rule, the merge rule cannot be specified
// without this shape, so deferring it would mean redesigning the delta model
// later. `tools` has no such coupling and defers for free.
//
// `arguments` is a verbatim JSON *string*, never a parsed object, and that is
// load bearing twice over. Streamed, it arrives in fragments that are not valid
// JSON individually (`{"loc` then `ation":"SF"}`), so there is nothing to parse
// until the stream ends. And a model can emit malformed arguments — that is a
// fact about the reply the caller must be able to see and handle, not a parse
// error that costs them the rest of the turn. Same reasoning that keeps
// `response_format` raw json. `parsed_arguments()` is the opt-in.
struct ToolCall {
  std::string id{};
  std::string type{};   // "function"
  std::string name{};   // function.name
  std::string arguments{};
  // Present on streaming fragments, absent on a non-streamed reply. It is the
  // *join key* while accumulating, not an array position — see stream.hpp.
  std::optional<int> index{};
  // Gemini-family models attach an opaque signature to the function-call part
  // and require it echoed back on the next turn (VC-18, #29). Measured against
  // api.venice.ai on gemini-3-6-flash, 2026-08-09: the same turn replayed with
  // it stripped is HTTP 400 ("Function call is missing a thought_signature in
  // functionCall parts"), replayed with it echoed is 200. Necessary and
  // sufficient — nothing else about the turn had to change.
  //
  // It is a SIBLING of `function`, not a member of it, and Venice passes it
  // through unmodified rather than stripping it. Nesting it inside `function`
  // is the one way to reimplement this and still get the 400.
  //
  // optional<string> rather than string for the same reason ChatRequest's
  // fields are optional: a family that sends none must keep producing the
  // byte-identical body it always did. zai-org-glm-4.7 sends none and accepts
  // the replay today; that body does not move.
  //
  // Opaque on purpose — never decoded, validated, or length-checked. It is a
  // token carried between two turns, and any opinion here about its contents
  // would be this library inventing one.
  std::optional<std::string> thought_signature{};
  nlohmann::json raw{};  // verbatim; never serialized (see Message::raw)

  // The one place `arguments` is interpreted, and it hands the failure back
  // rather than throwing: a model emitting bad JSON is a normal outcome.
  [[nodiscard]] auto parsed_arguments() const -> std::expected<nlohmann::json, Error> {
    try {
      return nlohmann::json::parse(arguments);
    } catch (const std::exception& e) {
      return std::unexpected{
          Error{ErrorKind::Parse, 0, std::string{"tool call arguments: "} + e.what(), arguments}};
    }
  }

  friend void to_json(nlohmann::json& j, const ToolCall& t) {
    j = nlohmann::json::object();
    j["id"] = t.id;
    j["type"] = t.type.empty() ? "function" : t.type;
    auto fn = nlohmann::json::object();
    fn["name"] = t.name;
    fn["arguments"] = t.arguments;
    j["function"] = std::move(fn);
    // Only when the server sent one. An unconditional key — even "" or null —
    // moves the wire body for every family that sends no signature, which is
    // the non-regression half of VC-18. Emitted beside `function`, never
    // inside it; see the member's comment for what nesting it costs.
    if (t.thought_signature) j["thought_signature"] = *t.thought_signature;
    // `index` is deliberately NOT emitted. It is a streaming-transport artifact
    // — the position this fragment merges into — and replaying it on a request
    // would assert an ordering the caller never chose.
  }

  // Total: never throws. A fragment carries `function.arguments` alone; a reply
  // carries no `index`; a gateway may send `"name": ""` on a continuation.
  friend void from_json(const nlohmann::json& j, ToolCall& t) {
    if (!j.is_object()) return;
    t.raw = j;
    if (const auto* s = detail::opt_string_at(j, "id")) t.id = *s;
    if (const auto* s = detail::opt_string_at(j, "type")) t.type = *s;
    t.index = detail::opt_int(j, "index");
    // Predicate-based like every other read here, and that is load bearing: a
    // wrong-typed signature reads as *absent*, which produces the server's
    // honest 400 rather than putting a confident wrong value on the wire.
    t.thought_signature = detail::opt_string(j, "thought_signature");
    if (const auto* fn = detail::opt_object(j, "function")) {
      if (const auto* s = detail::opt_string_at(*fn, "name")) t.name = *s;
      if (const auto* s = detail::opt_string_at(*fn, "arguments")) t.arguments = *s;
    }
  }
};

// ── messages ──────────────────────────────────────────────────────────────
//
// A message is the only struct in this header that legitimately travels *both*
// directions: it is what the caller sends and what the assistant replies with,
// and the whole point of VC-05 is that a reply can become the next turn's
// request without losing anything. That bidirectionality is why it carries two
// escape hatches where every other type has one, and why they keep the names
// AGENTS.md already assigned to those contracts:
//
//   raw   — response-side. The verbatim server object, a superset of the modeled
//           fields. Written only by from_json. NEVER serialized.
//   extra — request-side. Additive seed for to_json; modeled fields win.
//
// It is tempting to collapse these into one `raw` that also seeds to_json, so a
// parsed reply round-trips losslessly for free. That design is wrong, and it
// fails silently:
//
//     auto m = *res->message;   // assistant turn, a long answer
//     m.content.reset();        // redact it before storing history
//     // -> content disengaged, falls through to raw, the whole answer is resent
//
// Same shape, worse consequence for tool calls: execute call_a, append the
// role:"tool" result, clear tool_calls so it is not re-issued — and the merge
// replays it, which is a 400 for an unanswered tool_call or an infinite agent
// loop. A rule that honours `m.content = "edited"` but ignores
// `m.content.reset()` is more dangerous than one that honours neither, because
// it fails precisely when the caller was being careful, and trimming history is
// the most common thing an agent loop does. Measured, not reasoned: a probe
// against both rules on the same redacted turn emitted the full answer under
// seed-from-raw and `{"role":"assistant"}` under the rule below.
//
// It would also drag back the regression VC-11 removed. `j["messages"] =
// messages` would then deep-copy every message's full verbatim server body on
// every request — twenty turns of history, twenty json trees per call, each
// growing with the reply.
//
// So: **to_json seeds from `extra`, then for every modeled key either assigns it
// (engaged) or ERASES it (disengaged).** The erase branch is what makes the
// merge total and mutation-honest. Verbatim replay of unmodeled keys stays
// available, as one deliberate and greppable line: `m.extra = m.raw;`
//
// Note `Message::extra` does NOT inherit ChatRequest::extra's shadowing
// tolerance. That one is caller-authored, so a same-named key is arguably the
// caller's own request. This one will routinely be seeded from a response, which
// makes shadowing the common case rather than a pathology.
//
// from_json is **total** — it never throws. A message sub-object's shape
// legitimately varies (content string/null/absent/array, tool_calls present or
// not), and the shape it varies *to* is the one that matters most:
// {"role":"assistant","content":null,"tool_calls":[...]} is the canonical
// tool-call reply and the old two-key parse threw type_error.302 on it, so
// Message was not usable as a parse target for a real assistant message at all.
// This totality is deliberately NOT extended to ChatResponse::from_json_body,
// whose top-level shape is a contract — see there.
struct Message {
  std::string role{};  // "system" | "user" | "assistant" | "tool"

  // Four wire states, one type. std::optional<std::string> can express only
  // three of them and none of the interesting ones:
  //
  //   nullopt          key omitted entirely
  //   nullptr          "content": null   — what a tool-call-only reply sends
  //   "text"           "content": "text"
  //   json::array({…}) multimodal parts, verbatim
  //
  // Being json is what makes to_json rule-free (`if (content) assign else
  // erase`) and what makes the parts form expressible at all, in both
  // directions. `text()` flattens it for the common case.
  //
  // The nlohmann pin still applies with full force: dereference before handing
  // it over. `j["content"] = content;` (no star) fails to compile on the pinned
  // 3.11.3 and silently emits null on a 3.12 system copy.
  std::optional<nlohmann::json> content{};

  // The thinking stream, kept so it can be fed back. Some models require the
  // prior turn's reasoning to be replayed; without a field for it that is
  // simply unexpressible, which is what filed VC-05 in this shape.
  std::optional<std::string> reasoning_content{};

  // optional<vector>, not a bare vector, for the reason ChatRequest::stop is:
  // engaged-but-empty must emit `[]` and disengaged must omit the key, and a
  // bare vector collapses those two into one.
  std::optional<std::vector<ToolCall>> tool_calls{};

  std::optional<std::string> tool_call_id{};  // required when role == "tool"
  std::optional<std::string> name{};
  std::optional<std::string> refusal{};

  nlohmann::json raw{};    // response-side, verbatim, never serialized
  nlohmann::json extra{};  // request-side, additive seed

  // The content as text, whatever shape it arrived in: the string itself, the
  // concatenated `text` of a parts array, or "" for null/absent/anything else.
  // Not an error channel — a flattening. It is also what populates the
  // ChatResponse::content convenience snapshot.
  [[nodiscard]] auto text() const -> std::string {
    if (!content) return {};
    if (content->is_string()) return content->get<std::string>();
    if (content->is_array()) {
      std::string out;
      for (const auto& part : *content)
        if (part.is_object())
          if (const auto* s = detail::opt_string_at(part, "text")) out += *s;
      return out;
    }
    return {};
  }

  friend void to_json(nlohmann::json& j, const Message& m) {
    // is_object() guard as everywhere else in this header: a default-constructed
    // json is null, and an array-valued one would make the operator[] below
    // throw type_error.305 out of a function called with no try/catch. It also
    // means `m.extra = some_array` degrades to "no seed" rather than to a throw.
    j = m.extra.is_object() ? m.extra : nlohmann::json::object();
    j["role"] = m.role;

    // Assign-or-erase, every modeled key. erase() on an absent key is a no-op
    // and is safe here because j is guaranteed an object by the line above.
    if (m.content) j["content"] = *m.content; else j.erase("content");
    if (m.reasoning_content) j["reasoning_content"] = *m.reasoning_content;
    else j.erase("reasoning_content");
    if (m.tool_calls) j["tool_calls"] = *m.tool_calls; else j.erase("tool_calls");
    if (m.tool_call_id) j["tool_call_id"] = *m.tool_call_id; else j.erase("tool_call_id");
    if (m.name) j["name"] = *m.name; else j.erase("name");
    if (m.refusal) j["refusal"] = *m.refusal; else j.erase("refusal");
  }

  friend void from_json(const nlohmann::json& j, Message& m) {
    if (!j.is_object()) return;
    m.raw = j;
    if (const auto* s = detail::opt_string_at(j, "role")) m.role = *s;
    // Distinguishing absent from null is the entire point, so this is a find()
    // rather than any of the opt_ helpers — all of which collapse both to
    // nullopt.
    if (const auto it = j.find("content"); it != j.end()) m.content = *it;
    m.reasoning_content = detail::opt_string(j, "reasoning_content");
    m.tool_call_id = detail::opt_string(j, "tool_call_id");
    m.name = detail::opt_string(j, "name");
    m.refusal = detail::opt_string(j, "refusal");
    if (const auto* arr = detail::opt_array(j, "tool_calls")) {
      std::vector<ToolCall> calls;
      calls.reserve(arr->size());
      for (const auto& e : *arr) calls.push_back(e.get<ToolCall>());
      m.tool_calls = std::move(calls);
    }
  }

  static auto system(std::string c) -> Message { return with_content("system", std::move(c)); }
  static auto user(std::string c) -> Message { return with_content("user", std::move(c)); }
  static auto assistant(std::string c) -> Message { return with_content("assistant", std::move(c)); }

  // The reply half of tool calling. Three lines, and without them the round trip
  // is only half told — a caller could send tool_calls back but never answer
  // them, which leaves the whole feature untestable end to end.
  static auto tool(std::string call_id, std::string c) -> Message {
    auto m = with_content("tool", std::move(c));
    m.tool_call_id = std::move(call_id);
    return m;
  }

 private:
  // Named rather than brace-init: `return {"user", std::move(c)};` stops
  // compiling cleanly under -Wextra the moment this struct grows a member
  // without a default member initializer (-Wmissing-field-initializers), the
  // same trap AGENTS.md records for RequestOptions. Every member above has one,
  // and this keeps the builders indifferent to the next field added.
  static auto with_content(std::string role, std::string c) -> Message {
    Message m;
    m.role = std::move(role);
    m.content = std::move(c);
    return m;
  }
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

// Field-by-field assignment rather than a brace-init list. The rationale
// originally recorded here — that an array-valued `schema` makes the outer
// initializer-list ambiguous and nlohmann reads the whole thing as an array —
// did not survive being measured (VC-08): rewriting this body as a single
// brace-init leaves every case in test/02request/ green on the pinned 3.11.3,
// array-valued schema included. The style stays, because it is unambiguous by
// construction and does not depend on nlohmann's init-list heuristic holding.
// The real brace hazard is one level down and is documented under the tool
// builders below: a json *scalar* built with braces becomes a one-element array.
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

// ── tool / tool_choice builders ───────────────────────────────────────────
//
// `ChatRequest::tools` holds raw json *elements* rather than a typed Tool, and
// `tool_choice` holds raw json, for the reason response_format above does and
// models(type) does: the value set — here the *shape* set — belongs to the
// server.
//
// The tempting alternative is a Tool struct mirroring ToolCall: flat members,
// a to_json that re-nests name/description/parameters under "function". It is
// tempting because ToolCall sits 250 lines up and the symmetry is obvious. But
// ToolCall may nest unconditionally because it re-serializes something the
// server *sent*; `tools` is authored by the caller. A Tool that always nests
// hardcodes exactly one shape, and would emit
// {"type":"web_search","function":{"name":""}} the day Venice accepts a tool
// entry that is not a function.
//
// It would also break the escape hatch rather than merely omit it.
// response_format's documented escape is "assign the object directly"; a typed
// Tool inside optional<vector<Tool>> leaves only extra["tools"], which LOSES
// whenever `tools` is engaged and works only when it is disengaged. A hatch
// whose behaviour flips on an unrelated field is worse than no hatch. With json
// elements there is nothing to escape from:
//
//     r.tools = std::vector<nlohmann::json>{venice::tools::function("f", "d", schema)};
//     r.tools->push_back(nlohmann::json::parse(R"({"type":"web_search"})"));
//     (*r.tools)[0]["function"]["strict"] = true;   // or any unmodeled sub-key
//
// The element type is spelled out in that first line because it has to be:
// `r.tools = {venice::tools::function("f")}` is an *ambiguous overload* and does
// not compile — a braced list can match optional's converting assignment, its
// nullopt_t overload, and both copy and move assignment at once. Every call site
// in this repo already used the explicit form, which is exactly why the suite
// stayed green while this comment did not compile (VC-08).
//
// which is also why `strict` is not a parameter below: Venice documents it on
// response_format.json_schema, not on tools, and a parameter for an undocumented
// key is this header speculating.
//
// Parentheses, never braces, when building a json SCALAR — and that is the only
// brace rule here that is load bearing. Measured against the pinned 3.11.3:
//
//     nlohmann::json{"auto"}  ->  ["auto"]     (an array)
//     nlohmann::json("auto")  ->  "auto"
//
// Both spellings compile, so nothing but an assertion on the type catches the
// wrong one; test/02request/ asserts is_string() for exactly that reason.
//
// The *object* builders below assign field by field for consistency with
// response_format and because it stays readable, NOT because brace-init would
// mis-parse them. That was checked rather than assumed, and the check corrected
// the belief: six spellings were measured on 3.11.3 — nested one-shot,
// array-valued values, runtime-variable values, inline two-string arrays — and
// every one produced the correct object. Rebuilding json_schema above as a
// single brace-init left all 115 cases in test/02request/ green, which means the
// "an array-valued schema makes the outer list ambiguous" rationale that used to
// sit there described a hazard this pin does not have. The rule survives; the
// citation for it did not.

namespace tools {

// One entry for ChatRequest::tools.
//
// An empty `description` and a null `parameters` are OMITTED. A plain (non-
// optional) parameter has no "unset" state, so the degenerate value is the only
// way a caller can say "no" — the same convention detail::with_query uses for an
// empty query value. Omitting `parameters` is also the documented way to declare
// a function that takes no arguments, where "parameters": null is a 400. This
// does not contradict the engaged-optional rule below ChatRequest: there is no
// optional here to have engaged.
//
// `name` is emitted even when empty, deliberately. A nameless tool is the
// caller's error to see in the server's answer — which names the offending entry
// — and dropping the key would produce a different, less legible 400.
// Client::validate does not check it either; see test/03guards/.
inline auto function(std::string name, std::string description = {},
                     nlohmann::json parameters = nlohmann::json()) -> nlohmann::json {
  auto fn = nlohmann::json::object();
  fn["name"] = std::move(name);
  if (!description.empty()) fn["description"] = std::move(description);
  if (!parameters.is_null()) fn["parameters"] = std::move(parameters);

  auto j = nlohmann::json::object();
  j["type"] = "function";
  j["function"] = std::move(fn);
  return j;
}

}  // namespace tools

namespace tool_choice {

// The wire value is the string "auto"; `auto` is a keyword, so the builder is
// not. `automatic` rather than `auto_` because a trailing underscore is a purely
// lexical dodge that carries no information, and its one supposed advantage —
// greppability — is false in a C++23 codebase where `auto` is on nearly every
// line. And rather than `any` because `any` is Anthropic's name for what OpenAI
// calls `required`: a caller who knows one API would read it as the other.
// none() and required() keep their wire spelling, so exactly one builder
// diverges from the string it emits.
inline auto automatic() -> nlohmann::json { return nlohmann::json("auto"); }
inline auto none() -> nlohmann::json { return nlohmann::json("none"); }
inline auto required() -> nlohmann::json { return nlohmann::json("required"); }

// Naming a specific function. Same nesting as a tools entry, minus everything
// the server already knows from the declaration.
inline auto function(std::string name) -> nlohmann::json {
  auto fn = nlohmann::json::object();
  fn["name"] = std::move(name);

  auto j = nlohmann::json::object();
  j["type"] = "function";
  j["function"] = std::move(fn);
  return j;
}

}  // namespace tool_choice

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
  // optional<vector>, not a bare vector, for the reason `stop` and
  // Message::tool_calls are: engaged-but-empty must emit "tools": [] and
  // disengaged must omit the key, and a bare vector collapses those two.
  //
  // The element type is raw json — see the builders above. Note that
  // std::vector<nlohmann::json> IS nlohmann::json::array_t for the default json
  // alias (measured, not assumed), so this is optional-of-the-array-type and
  // nothing converts on the way to the wire. That also rules out the other near
  // miss, optional<json>: `r.tools = venice::tools::function("f")` would compile
  // and send a bare object where the API wants an array.
  std::optional<std::vector<nlohmann::json>> tools;
  // "auto" | "none" | "required" (json *strings*) or
  // {"type":"function","function":{"name":"..."}}. Genuinely polymorphic across
  // scalar and object, which is a stronger case for raw json than
  // response_format has — that one is at least always an object.
  std::optional<nlohmann::json> tool_choice;
  std::optional<bool> parallel_tool_calls;
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
    if (tools) j["tools"] = *tools;
    if (tool_choice) j["tool_choice"] = *tool_choice;
    // `if (parallel_tool_calls)`, NOT `if (*parallel_tool_calls)`. The second
    // compiles clean under -Wall -Wextra -pedantic and makes `= false`
    // indistinguishable from unset — and false is the interesting value here: a
    // caller disables parallel calls precisely when their agent loop cannot
    // execute two at once. Worse than wrong, in fact: dereferencing the unset
    // case is UB, which is why breaking this line reddens the baseline too.
    if (parallel_tool_calls) j["parallel_tool_calls"] = *parallel_tool_calls;
    if (venice_parameters) j["venice_parameters"] = *venice_parameters;
    return j;
  }
};

// ── embeddings ────────────────────────────────────────────────────────────
//
// `input` is caller-authored and polymorphic, so it follows the same contract
// as response_format and tools: raw json is the forward-compatible floor and
// builders make the documented shapes easy to spell. A closed variant here
// would turn the next input form Venice adds into a source release of this
// header instead of something callers can use immediately.

namespace embedding_input {

inline auto text(std::string value) -> nlohmann::json {
  return nlohmann::json(std::move(value));
}

inline auto texts(std::vector<std::string> values) -> nlohmann::json {
  return nlohmann::json(std::move(values));
}

inline auto tokens(std::vector<int> values) -> nlohmann::json {
  return nlohmann::json(std::move(values));
}

inline auto token_batches(std::vector<std::vector<int>> values) -> nlohmann::json {
  return nlohmann::json(std::move(values));
}

}  // namespace embedding_input

struct EmbeddingRequest {
  std::string model{};
  nlohmann::json input{};
  std::optional<int> dimensions{};
  std::optional<std::string> encoding_format{};
  std::optional<std::string> user{};
  nlohmann::json extra{};

  [[nodiscard]] auto to_json_body() const -> nlohmann::json {
    nlohmann::json j = extra.is_object() ? extra : nlohmann::json::object();
    j["model"] = model;
    j["input"] = input;
    if (dimensions) j["dimensions"] = *dimensions;
    if (encoding_format) j["encoding_format"] = *encoding_format;
    if (user) j["user"] = *user;
    return j;
  }
};

// Venice documents `encoding_format` as float or base64, although the current
// 200-response schema describes only the numeric-array half. Keep the two wire
// shapes discriminated and keep base64 opaque: the API does not specify the
// decoded element width or byte order.
using EmbeddingValue = std::variant<std::vector<double>, std::string>;

struct Embedding {
  EmbeddingValue value{};
  int index{0};
  std::string object{};
  nlohmann::json raw{};
};

struct EmbeddingUsage {
  int prompt_tokens{0};
  int total_tokens{0};

  friend auto operator==(const EmbeddingUsage&, const EmbeddingUsage&) -> bool = default;
};

struct EmbeddingResponse {
  std::vector<Embedding> data{};
  std::string model{};
  std::string object{};
  EmbeddingUsage usage{};
  ResponseMetadata metadata{};
  nlohmann::json raw{};
};

// Parse a successful /embeddings envelope. Every modeled field is required by
// the operation and affects either vector ordering or accounting, so this is a
// loud parser: malformed data throws here and Client::embeddings turns it into
// ErrorKind::Parse. Unknown fields survive in the two raw objects.
[[nodiscard]] inline auto embeddings_from_json_body(const nlohmann::json& j)
    -> EmbeddingResponse {
  const auto required_string = [](const nlohmann::json& object, const char* key,
                                  const char* where) -> std::string {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string())
      throw std::runtime_error{std::string{where} + ": " + key + " must be a string"};
    return it->get<std::string>();
  };
  const auto required_int = [](const nlohmann::json& object, const char* key,
                               const char* where) -> int {
    const auto value = detail::opt_int(object, key);
    if (!value)
      throw std::runtime_error{std::string{where} + ": " + key + " must be an int"};
    return *value;
  };

  if (!j.is_object()) throw std::runtime_error{"embeddings: response must be an object"};
  const auto* entries = detail::opt_array(j, "data");
  if (entries == nullptr) throw std::runtime_error{"embeddings: response has no data array"};
  const auto* usage = detail::opt_object(j, "usage");
  if (usage == nullptr) throw std::runtime_error{"embeddings: response has no usage object"};

  EmbeddingResponse response;
  response.raw = j;
  response.model = required_string(j, "model", "embeddings");
  response.object = required_string(j, "object", "embeddings");
  response.usage.prompt_tokens = required_int(*usage, "prompt_tokens", "embeddings usage");
  response.usage.total_tokens = required_int(*usage, "total_tokens", "embeddings usage");
  response.data.reserve(entries->size());

  for (const auto& item : *entries) {
    if (!item.is_object()) throw std::runtime_error{"embeddings: data entry must be an object"};
    const auto value = item.find("embedding");
    if (value == item.end())
      throw std::runtime_error{"embeddings: data entry has no embedding"};

    Embedding entry;
    entry.raw = item;
    entry.index = required_int(item, "index", "embedding entry");
    entry.object = required_string(item, "object", "embedding entry");
    if (value->is_string()) {
      entry.value = value->get<std::string>();
    } else if (value->is_array()) {
      std::vector<double> numbers;
      numbers.reserve(value->size());
      for (const auto& element : *value) {
        if (!element.is_number())
          throw std::runtime_error{"embeddings: vector element must be a number"};
        numbers.push_back(element.get<double>());
      }
      entry.value = std::move(numbers);
    } else {
      throw std::runtime_error{"embeddings: embedding must be an array or string"};
    }
    response.data.push_back(std::move(entry));
  }
  return response;
}

// ── image generation ─────────────────────────────────────────────────────
//
// Venice exposes two generation operations with deliberately distinct
// contracts. /image/generate is Venice-native and may return either JSON or
// encoded image bytes; /images/generations is OpenAI-compatible and always
// returns JSON. Keeping the request and response types separate prevents a
// field accepted by one spelling from looking meaningful on the other.

struct ImageStyleReference {
  std::string image{};
  std::optional<double> strength{};
  nlohmann::json extra{};

  [[nodiscard]] auto to_json_body() const -> nlohmann::json {
    nlohmann::json j = extra.is_object() ? extra : nlohmann::json::object();
    j["image"] = image;
    if (strength) j["strength"] = *strength;
    return j;
  }
};

struct ImageGenerationRequest {
  std::string model{};
  std::string prompt{};
  std::optional<double> cfg_scale{};
  std::optional<bool> embed_exif_metadata{};
  std::optional<std::string> format{};
  std::optional<int> height{};
  std::optional<bool> hide_watermark{};
  std::optional<int> lora_strength{};
  std::optional<std::string> negative_prompt{};
  std::optional<bool> return_binary{};
  std::optional<int> variants{};
  std::optional<bool> safe_mode{};
  std::optional<int> seed{};
  std::optional<int> steps{};
  std::optional<std::string> style_preset{};
  std::optional<std::string> aspect_ratio{};
  std::optional<std::string> resolution{};
  std::optional<std::string> quality{};
  std::optional<bool> enable_web_search{};
  std::optional<bool> disable_prompt_optimization_thinking{};
  std::optional<bool> enhance_prompt{};
  std::optional<int> width{};
  // Optional container so omission and an explicit [] remain different wire
  // requests. The model catalogue says whether references are supported and
  // how many are accepted; this client does not duplicate that policy.
  std::optional<std::vector<ImageStyleReference>> style_references{};
  nlohmann::json extra{};

  [[nodiscard]] auto to_json_body() const -> nlohmann::json {
    nlohmann::json j = extra.is_object() ? extra : nlohmann::json::object();
    j["model"] = model;
    j["prompt"] = prompt;
    if (cfg_scale) j["cfg_scale"] = *cfg_scale;
    if (embed_exif_metadata) j["embed_exif_metadata"] = *embed_exif_metadata;
    if (format) j["format"] = *format;
    if (height) j["height"] = *height;
    if (hide_watermark) j["hide_watermark"] = *hide_watermark;
    if (lora_strength) j["lora_strength"] = *lora_strength;
    if (negative_prompt) j["negative_prompt"] = *negative_prompt;
    if (return_binary) j["return_binary"] = *return_binary;
    if (variants) j["variants"] = *variants;
    if (safe_mode) j["safe_mode"] = *safe_mode;
    if (seed) j["seed"] = *seed;
    if (steps) j["steps"] = *steps;
    if (style_preset) j["style_preset"] = *style_preset;
    if (aspect_ratio) j["aspect_ratio"] = *aspect_ratio;
    if (resolution) j["resolution"] = *resolution;
    if (quality) j["quality"] = *quality;
    if (enable_web_search) j["enable_web_search"] = *enable_web_search;
    if (disable_prompt_optimization_thinking)
      j["disable_prompt_optimization_thinking"] = *disable_prompt_optimization_thinking;
    if (enhance_prompt) j["enhance_prompt"] = *enhance_prompt;
    if (width) j["width"] = *width;
    if (style_references) {
      j["style_references"] = nlohmann::json::array();
      for (const auto& reference : *style_references)
        j["style_references"].push_back(reference.to_json_body());
    }
    return j;
  }
};

struct ImageGenerationTiming {
  double inference_duration{0.0};
  double inference_preprocessing_time{0.0};
  double inference_queue_time{0.0};
  double total{0.0};
};

struct NativeImageGenerationResponse {
  std::string id{};
  std::vector<std::string> images{};
  std::optional<nlohmann::json> request{};
  ImageGenerationTiming timing{};
  ResponseMetadata metadata{};
  nlohmann::json raw{};
};

// The bytes stay in std::string because that is cpp-httplib's byte container.
// size(), not a terminating NUL, is authoritative. The media type is the
// normalized actual response Content-Type, never inferred from the request.
struct GeneratedImageMedia {
  std::string bytes{};
  std::string media_type{};
  ResponseMetadata metadata{};
};

using ImageGenerationResult =
    std::variant<NativeImageGenerationResponse, GeneratedImageMedia>;

[[nodiscard]] inline auto native_image_generation_from_json_body(const nlohmann::json& j)
    -> NativeImageGenerationResponse {
  const auto required_string = [](const nlohmann::json& object, const char* key,
                                  const char* where) -> std::string {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string())
      throw std::runtime_error{std::string{where} + ": " + key + " must be a string"};
    return it->get<std::string>();
  };
  const auto required_number = [](const nlohmann::json& object, const char* key,
                                  const char* where) -> double {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number())
      throw std::runtime_error{std::string{where} + ": " + key + " must be a number"};
    return it->get<double>();
  };

  if (!j.is_object()) throw std::runtime_error{"image generation: response must be an object"};
  const auto* images = detail::opt_array(j, "images");
  if (images == nullptr)
    throw std::runtime_error{"image generation: response has no images array"};
  const auto* timing = detail::opt_object(j, "timing");
  if (timing == nullptr)
    throw std::runtime_error{"image generation: response has no timing object"};

  NativeImageGenerationResponse response;
  response.raw = j;
  response.id = required_string(j, "id", "image generation");
  response.images.reserve(images->size());
  for (const auto& image : *images) {
    if (!image.is_string())
      throw std::runtime_error{"image generation: image must be a string"};
    response.images.push_back(image.get<std::string>());
  }
  if (const auto request = j.find("request"); request != j.end() && !request->is_null())
    response.request = *request;
  response.timing = {
      .inference_duration =
          required_number(*timing, "inferenceDuration", "image generation timing"),
      .inference_preprocessing_time = required_number(
          *timing, "inferencePreprocessingTime", "image generation timing"),
      .inference_queue_time =
          required_number(*timing, "inferenceQueueTime", "image generation timing"),
      .total = required_number(*timing, "total", "image generation timing"),
  };
  return response;
}

struct OpenAIImageGenerationRequest {
  std::string prompt{};
  std::optional<std::string> background{};
  std::optional<std::string> model{};
  std::optional<std::string> moderation{};
  std::optional<int> n{};
  std::optional<int> output_compression{};
  std::optional<std::string> output_format{};
  std::optional<std::string> quality{};
  std::optional<std::string> response_format{};
  std::optional<std::string> size{};
  std::optional<std::string> style{};
  std::optional<std::string> user{};
  nlohmann::json extra{};

  [[nodiscard]] auto to_json_body() const -> nlohmann::json {
    nlohmann::json j = extra.is_object() ? extra : nlohmann::json::object();
    j["prompt"] = prompt;
    if (background) j["background"] = *background;
    if (model) j["model"] = *model;
    if (moderation) j["moderation"] = *moderation;
    if (n) j["n"] = *n;
    if (output_compression) j["output_compression"] = *output_compression;
    if (output_format) j["output_format"] = *output_format;
    if (quality) j["quality"] = *quality;
    if (response_format) j["response_format"] = *response_format;
    if (size) j["size"] = *size;
    if (style) j["style"] = *style;
    if (user) j["user"] = *user;
    return j;
  }
};

struct OpenAIImageGenerationEntry {
  std::optional<std::string> b64_json{};
  std::optional<std::string> url{};
  nlohmann::json raw{};
};

struct OpenAIImageGenerationResponse {
  std::int64_t created{0};
  std::vector<OpenAIImageGenerationEntry> data{};
  ResponseMetadata metadata{};
  nlohmann::json raw{};
};

[[nodiscard]] inline auto openai_image_generation_from_json_body(const nlohmann::json& j)
    -> OpenAIImageGenerationResponse {
  if (!j.is_object())
    throw std::runtime_error{"OpenAI image generation: response must be an object"};
  const auto created = detail::opt_i64(j, "created");
  if (!created)
    throw std::runtime_error{"OpenAI image generation: created must be an int64"};
  const auto* entries = detail::opt_array(j, "data");
  if (entries == nullptr)
    throw std::runtime_error{"OpenAI image generation: response has no data array"};

  OpenAIImageGenerationResponse response;
  response.raw = j;
  response.created = *created;
  response.data.reserve(entries->size());
  for (const auto& item : *entries) {
    if (!item.is_object())
      throw std::runtime_error{"OpenAI image generation: data entry must be an object"};
    OpenAIImageGenerationEntry entry;
    entry.raw = item;
    if (const auto value = item.find("b64_json"); value != item.end()) {
      if (!value->is_string())
        throw std::runtime_error{"OpenAI image generation: b64_json must be a string"};
      entry.b64_json = value->get<std::string>();
    }
    if (const auto value = item.find("url"); value != item.end()) {
      if (!value->is_string())
        throw std::runtime_error{"OpenAI image generation: url must be a string"};
      entry.url = value->get<std::string>();
    }
    if (!entry.b64_json && !entry.url)
      throw std::runtime_error{"OpenAI image generation: entry has no image value"};
    response.data.push_back(std::move(entry));
  }
  return response;
}

struct ImageStyles {
  std::vector<std::string> entries{};
  std::size_t returned{0};
  std::optional<std::string> object{};
  ResponseMetadata metadata{};
  nlohmann::json raw{};
};

[[nodiscard]] inline auto image_styles_from_json_body(const nlohmann::json& j) -> ImageStyles {
  if (!j.is_object()) throw std::runtime_error{"image styles: response must be an object"};
  const auto* data = detail::opt_array(j, "data");
  if (data == nullptr) throw std::runtime_error{"image styles: response has no data array"};

  ImageStyles response;
  response.raw = j;
  response.object = detail::opt_string(j, "object");
  response.returned = data->size();
  response.entries.reserve(data->size());
  for (const auto& item : *data)
    if (item.is_string()) response.entries.push_back(item.get<std::string>());
  return response;
}

// ── image transformations ────────────────────────────────────────────────
//
// The same logical image reaches Venice in three materially different forms:
// an inline encoded value, a remote URL, or an uploaded file. A bare string
// cannot distinguish the first two from the third without guessing from its
// contents, and guessing would make media selection depend on a prefix rather
// than on the caller's choice. The small wrappers keep that choice explicit.

struct InlineImage {
  std::string value{};
};

struct ImageUrl {
  std::string value{};
};

struct ImageFile {
  // std::string is the byte container used by cpp-httplib. Embedded NUL bytes
  // are data; bytes.size(), never a terminator, is authoritative.
  std::string bytes{};
  std::string filename{};
  std::string media_type{};
};

using ImageInput = std::variant<InlineImage, ImageUrl, ImageFile>;

namespace image_input {

[[nodiscard]] inline auto base64(std::string value) -> ImageInput {
  return InlineImage{std::move(value)};
}

// Kept distinct at the builder surface because it documents caller intent,
// even though both spellings travel as one verbatim JSON string.
[[nodiscard]] inline auto data_url(std::string value) -> ImageInput {
  return InlineImage{std::move(value)};
}

[[nodiscard]] inline auto url(std::string value) -> ImageInput {
  return ImageUrl{std::move(value)};
}

[[nodiscard]] inline auto file(std::string bytes, std::string filename,
                               std::string media_type) -> ImageInput {
  return ImageFile{std::move(bytes), std::move(filename), std::move(media_type)};
}

}  // namespace image_input

namespace detail {

[[nodiscard]] inline auto image_input_json_value(const ImageInput& input)
    -> const std::string* {
  if (const auto* encoded = std::get_if<InlineImage>(&input)) return &encoded->value;
  if (const auto* url = std::get_if<ImageUrl>(&input)) return &url->value;
  return nullptr;
}

[[nodiscard]] inline auto image_json_seed(const nlohmann::json& extra) -> nlohmann::json {
  return extra.is_object() ? extra : nlohmann::json::object();
}

}  // namespace detail

struct ImageUpscaleRequest {
  ImageInput image{};
  std::optional<double> creativity{};
  std::optional<double> scale{};
  // JSON-form passthrough only. A file selects multipart, where guessing how
  // an arbitrary JSON value should become a form field would not be lossless;
  // Client rejects that combination instead of silently dropping the object.
  nlohmann::json extra{};

  [[nodiscard]] auto to_json_body() const -> nlohmann::json {
    auto j = detail::image_json_seed(extra);
    if (const auto* value = detail::image_input_json_value(image)) j["image"] = *value;
    if (creativity) j["creativity"] = *creativity;
    if (scale) j["scale"] = *scale;
    return j;
  }
};

struct ImageEditRequest {
  ImageInput image{};
  std::string prompt{};
  std::optional<std::string> model{};
  std::optional<std::string> aspect_ratio{};
  std::optional<bool> disable_prompt_optimization_thinking{};
  std::optional<bool> enhance_prompt{};
  std::optional<std::string> resolution{};
  std::optional<std::string> output_format{};
  std::optional<bool> safe_mode{};
  nlohmann::json extra{};

  [[nodiscard]] auto to_json_body() const -> nlohmann::json {
    auto j = detail::image_json_seed(extra);
    if (const auto* value = detail::image_input_json_value(image)) j["image"] = *value;
    j["prompt"] = prompt;
    if (model) j["model"] = *model;
    if (aspect_ratio) j["aspect_ratio"] = *aspect_ratio;
    if (disable_prompt_optimization_thinking)
      j["disable_prompt_optimization_thinking"] = *disable_prompt_optimization_thinking;
    if (enhance_prompt) j["enhance_prompt"] = *enhance_prompt;
    if (resolution) j["resolution"] = *resolution;
    if (output_format) j["output_format"] = *output_format;
    if (safe_mode) j["safe_mode"] = *safe_mode;
    return j;
  }
};

struct MultiImageEditRequest {
  std::vector<ImageInput> images{};
  std::string prompt{};
  // The C++ concept is a model id on both edit operations. The wire is not:
  // this endpoint still spells it `modelId`, while /image/edit uses `model`.
  std::optional<std::string> model{};
  std::optional<std::string> aspect_ratio{};
  std::optional<std::string> output_format{};
  std::optional<std::string> quality{};
  std::optional<std::string> resolution{};
  std::optional<bool> safe_mode{};
  std::optional<bool> disable_prompt_optimization_thinking{};
  std::optional<bool> enhance_prompt{};
  nlohmann::json extra{};

  [[nodiscard]] auto to_json_body() const -> nlohmann::json {
    auto j = detail::image_json_seed(extra);
    j["images"] = nlohmann::json::array();
    for (const auto& image : images)
      if (const auto* value = detail::image_input_json_value(image))
        j["images"].push_back(*value);
    j["prompt"] = prompt;
    if (model) j["modelId"] = *model;
    if (aspect_ratio) j["aspect_ratio"] = *aspect_ratio;
    if (output_format) j["output_format"] = *output_format;
    if (quality) j["quality"] = *quality;
    if (resolution) j["resolution"] = *resolution;
    if (safe_mode) j["safe_mode"] = *safe_mode;
    if (disable_prompt_optimization_thinking)
      j["disable_prompt_optimization_thinking"] = *disable_prompt_optimization_thinking;
    if (enhance_prompt) j["enhance_prompt"] = *enhance_prompt;
    return j;
  }
};

struct ImageBackgroundRemovalRequest {
  ImageInput image{};
  nlohmann::json extra{};

  [[nodiscard]] auto to_json_body() const -> nlohmann::json {
    auto j = detail::image_json_seed(extra);
    // These are alternatives, not independent optional fields. Erasing both
    // first prevents an extra seed from silently sending two source images.
    j.erase("image");
    j.erase("image_url");
    if (const auto* encoded = std::get_if<InlineImage>(&image))
      j["image"] = encoded->value;
    else if (const auto* url = std::get_if<ImageUrl>(&image))
      j["image_url"] = url->value;
    return j;
  }
};

// ── money ─────────────────────────────────────────────────────────────────
//
// Venice quotes every amount in two currencies at once: USD and `diem`, its
// credit unit. Both are optional because a bucket may carry either, and
// neither is derivable from the other — a ledger denominated in credits must
// not have to reconstruct them from dollars at a rate this library would be
// guessing at.
//
// Used on both sides of a call and the two are NOT the same quantity:
// Model::pricing is a *rate* (per million tokens, per image), ChatResponse::cost
// is an *amount* (what this one call charged). Do not multiply the latter by a
// token count.

struct Price {
  std::optional<double> usd;
  std::optional<double> diem;

  friend auto operator==(const Price&, const Price&) -> bool = default;

  friend void from_json(const nlohmann::json& j, Price& p) {
    p.usd = detail::opt_double(j, "usd");
    p.diem = detail::opt_double(j, "diem");
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
  std::optional<int> cached_tokens{};  // cache-read tokens, when reported
  // Of completion_tokens, how many went to thinking. Without this a reasoning
  // model's actual cost is invisible: the tokens are billed inside
  // completion_tokens with nothing saying what fraction was reasoning.
  std::optional<int> reasoning_tokens{};

  friend auto operator==(const Usage&, const Usage&) -> bool = default;

  // Loud, deliberately — these parse inside Client::chat's try/catch, where a
  // malformed body *should* fail as ErrorKind::Parse. The tolerant detail::opt_*
  // helpers are for listings, where one odd entry must not cost the caller the
  // other hundred; a chat reply has no siblings to protect and silently zeroing
  // a token count would hide a billing bug.
  //
  // One distinction that rule does not currently draw, and this parse needs:
  //
  //   * a wrong-typed *value* stays loud. "prompt_tokens": "many" is
  //     corruption; throw, and let the caller see ErrorKind::Parse.
  //   * a missing or null nested *object* is structural, not corruption.
  //     "prompt_tokens_details": null is a shape variation between gateways,
  //     and letting it throw would turn a metadata nicety into a failed chat
  //     completion. detail::opt_object is a pure structural predicate, so it is
  //     the right tool for the container while .get<int>() stays strict inside.
  friend void from_json(const nlohmann::json& j, Usage& u) {
    if (j.contains("prompt_tokens")) j.at("prompt_tokens").get_to(u.prompt_tokens);
    if (j.contains("completion_tokens")) j.at("completion_tokens").get_to(u.completion_tokens);
    if (j.contains("total_tokens")) j.at("total_tokens").get_to(u.total_tokens);

    // Read flat first, then let the nested location override; nested wins on
    // disagreement because prompt_tokens_details.cached_tokens is the
    // OpenAI-canonical location and the flat one is a compatibility shim.
    //
    // Both halves of that are now measured (VC-17, 21 captures on 2026-08-09 —
    // `venice-cpp --usage`). The nested read is the one that earns its keep: 5
    // of 7 model families send it, so reading only the flat key, which is what
    // every release through v0.7.0 did, really did report nullopt against real
    // Venice for the whole life of the field. The flat key itself has **never
    // been seen**. What Venice sends flat, beside the nested one and never
    // instead of it, is `cache_read_input_tokens` — a different key, carrying
    // the same number in every capture, which is why it stays untyped rather
    // than becoming a third read of one fact.
    if (j.contains("cached_tokens")) u.cached_tokens = j.at("cached_tokens").get<int>();
    if (const auto* pd = detail::opt_object(j, "prompt_tokens_details"))
      if (pd->contains("cached_tokens")) u.cached_tokens = pd->at("cached_tokens").get<int>();

    // opt_object here is intent, not protection, and that was measured rather
    // than assumed: swapping it for a plain `contains` leaves the whole suite
    // green, because nlohmann's contains() answers false for a null or an array
    // just as the predicate does. What is load bearing is that *some* guard
    // exists — dropping both and reaching straight through turns the null and
    // empty-object shapes below red. Keep a guard; which one is style.
    if (const auto* cd = detail::opt_object(j, "completion_tokens_details"))
      if (cd->contains("reasoning_tokens")) u.reasoning_tokens = cd->at("reasoning_tokens").get<int>();
    // Both details objects are per-family, not universal: gemini-3-6-flash and
    // qwen3-235b-a22b-thinking-2507 send neither, and both *claim*
    // supportsReasoning. So an absent reasoning_tokens is not evidence of a
    // parse bug, and #28 was filed believing it was — off a run against the
    // first of those two. `venice-cpp --usage <id>` prints the verbatim object
    // beside the parse, which is the only thing that separates the two cases.
    //
    // completion_tokens_details also carries audio_tokens,
    // accepted_prediction_tokens and rejected_prediction_tokens. They ride in
    // ChatResponse::raw rather than growing this struct speculatively — this
    // ticket is about reasoning, and raw is what makes leaving them untyped
    // honest rather than lossy.
  }
};

// ── chat response ─────────────────────────────────────────────────────────

struct ChatResponse {
  std::string id{};
  std::string model{};
  std::string content{};        // assistant message text — a snapshot; see below
  std::string finish_reason{};  // "stop" | "length" | ...
  std::optional<Usage> usage{};

  // What Venice charged for THIS call, as the server reports it — authoritative
  // where pairing Usage against Model::pricing is a reconstruction, and VC-17
  // established that reconstruction cannot be exact anyway (cached_tokens is
  // per-family; pricing has two cache buckets where Usage reports one).
  //
  // It lives here and not on Usage because it is a top-level SIBLING of `usage`
  // on the wire, and Usage::from_json only ever receives the `usage` sub-object
  // — it has no handle on the parent. That is a placement impossibility, not a
  // placement preference, and test/07stream/ §3 pins it.
  //
  // MEASURED 2026-08-10, api.venice.ai: present on all seven VC-17 families on
  // both paths, so unlike Usage's optional fields this is not per-family. Still
  // optional — a 402 body, a gateway, or a future endpoint need not carry it.
  //
  // `usd` HAS BEEN ZERO ON EVERY CAPTURE, including a call whose rate-card value
  // was $0.0645 (openai-gpt-55-pro, 1685 prompt + 6 completion at $37.50/$225
  // per million, with diem reporting exactly that magnitude). So an engaged
  // usd == 0 means "not reported for this account", NOT "this call was free".
  // Read `diem` unless you have measured otherwise for your own key. The library
  // reports what arrived and interprets nothing.
  //
  // Parsed through the tolerant detail::opt_double, and that is deliberate
  // rather than unprecedented: `created`, `system_fingerprint` and
  // `venice_parameters` below already read through opt_i64 / opt_string /
  // opt_object, and did before this field existed. What the loud-parse rule at
  // the top of this header actually protects is a field with **no
  // representation for "unknown"** — Usage::prompt_tokens is int{0}, so
  // tolerance there maps corrupt onto a number the caller cannot tell from a
  // real one. Every member here is optional<double> whose disengaged state
  // already means unknown, which puts cost on the same side of that line as the
  // three fields above rather than on Usage's side.
  //
  // Two consequences of a loud read, both checkable rather than rhetorical: on
  // the non-streamed path it turns a metadata field into ErrorKind::Parse for a
  // completion already paid for; and on the streamed path client.hpp's SSE
  // lambda catches the throw into `parse_err`, which is surfaced only when the
  // accumulator is empty, so a loud parse there yields a half-ingested frame
  // with on_delta silently skipped. Reasoning, not measurement — no corrupt
  // cost has ever been observed.
  std::optional<Price> cost{};

  // HTTP response metadata, populated by Client rather than from the JSON body.
  // X-Balance-Remaining is meaningful for SIWX-authenticated inference; raw
  // headers remain available for forward-compatible protocol additions.
  ResponseMetadata metadata{};

  // The whole assistant turn, complete enough to send back as the next message.
  // Optional because "choices": [] is a real body and a pinned non-throwing
  // case: a default-constructed Message would carry role == "" straight into a
  // caller's history and onto the wire.
  std::optional<Message> message{};

  // The verbatim body. Everything this struct does not model is here —
  // choices[1..n], logprobs, the completion_tokens_details buckets Usage
  // leaves untyped, and whatever Venice adds next. It is what makes deferring
  // those typings honest rather than lossy, and it is never sent anywhere.
  nlohmann::json raw{};

  std::optional<std::int64_t> created{};
  std::optional<std::string> system_fingerprint{};
  // What Venice reports it actually applied — web search state, character, and
  // so on. Echoed back on the response and previously discarded, so a caller
  // could not tell whether the venice_parameters they asked for took effect.
  std::optional<nlohmann::json> venice_parameters{};

  // Parse the non-streaming /chat/completions response body. Throws
  // nlohmann::json exceptions on malformed input — callers wrap in expected.
  //
  // This stays LOUD while Message::from_json became total, and the split is
  // deliberate rather than an inconsistency. A message sub-object's shape
  // legitimately varies, so refusing to parse one is wrong. The top-level shape
  // of a completion body is a contract: `j.at("choices")` throwing on a body
  // with no choices is exactly what Client::chat's try/catch turns into
  // ErrorKind::Parse, and test/01client/ pins it. Do not make both total.
  static auto from_json_body(const nlohmann::json& j) -> ChatResponse {
    ChatResponse r;
    r.raw = j;
    if (j.contains("id")) r.id = j.at("id").get<std::string>();
    if (j.contains("model")) r.model = j.at("model").get<std::string>();
    r.created = detail::opt_i64(j, "created");
    r.system_fingerprint = detail::opt_string(j, "system_fingerprint");
    if (const auto* vp = detail::opt_object(j, "venice_parameters")) r.venice_parameters = *vp;

    const auto& choices = j.at("choices");
    if (!choices.empty()) {
      const auto& c0 = choices.at(0);
      if (c0.contains("message")) {
        r.message = c0.at("message").get<Message>();
        // A derived snapshot, populated once here and never read by the
        // library. It exists so `res->content` keeps working; mutating it
        // changes nothing about what a later request sends. The invariant
        // r.content == r.message->text() is asserted in test/07stream/ so the
        // duplication is policed rather than merely documented.
        r.content = r.message->text();
      }
      if (c0.contains("finish_reason") && !c0.at("finish_reason").is_null())
        r.finish_reason = c0.at("finish_reason").get<std::string>();
    }
    if (j.contains("usage") && !j.at("usage").is_null()) r.usage = j.at("usage").get<Usage>();
    // Top level, beside usage rather than inside it — see ChatResponse::cost.
    if (const auto* c = detail::opt_object(j, "cost")) r.cost = c->get<Price>();
    return r;
  }
};


// ── model pricing ─────────────────────────────────────────────────────────
//
// Rates, in the two currencies `Price` above carries. These are what a model
// charges; ChatResponse::cost is what one call actually cost.

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

// Image-family pricing uses keys that are not token buckets. Keep the literal
// 2x/4x wire split rather than flattening it into one number: callers choose an
// upscale factor, and those factors are priced differently.
struct ImageUpscalePricing {
  std::optional<Price> x2;
  std::optional<Price> x4;

  friend void from_json(const nlohmann::json& j, ImageUpscalePricing& p) {
    p.x2.reset();
    p.x4.reset();
    if (const auto* value = detail::opt_object(j, "2x")) p.x2 = value->get<Price>();
    if (const auto* value = detail::opt_object(j, "4x")) p.x4 = value->get<Price>();
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
  std::optional<Price> generation;
  std::optional<ImageUpscalePricing> upscale;

  friend void from_json(const nlohmann::json& j, Pricing& p) {
    p.base = j.get<PriceTier>();
    p.generation.reset();
    p.upscale.reset();
    if (const auto* ext = detail::opt_object(j, "extended")) {
      p.extended = ext->get<PriceTier>();
      p.extended_threshold_tokens = detail::opt_i64(*ext, "context_token_threshold");
    }
    if (const auto* generation = detail::opt_object(j, "generation"))
      p.generation = generation->get<Price>();
    if (const auto* upscale = detail::opt_object(j, "upscale"))
      p.upscale = upscale->get<ImageUpscalePricing>();
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
//
// An alias since VC-39 rather than its own struct: the per-modality tables
// need the identical shape, and the name stays because callers and two tests
// spell it. Field<bool, ModelCapabilities> has the same two members in the
// same order, so the structured bindings over this table are unchanged.
using CapabilityFlag = Field<bool, ModelCapabilities>;

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

// ── per-modality model metadata ───────────────────────────────────────────
//
// `model_spec` is polymorphic by model type, and until VC-39 only its text
// shape was modeled. Everything below is the rest of it: the values a caller
// building an image, inpaint, video, TTS or embedding request needs in order
// to know whether its request is even well-formed.
//
// Four things about this block are measured rather than assumed, each from a
// keyless capture of every modality on 2026-08-11, and each is the reason a
// design taken from the specification alone would have been wrong:
//
//  * **Wire spelling is per-modality in the same wire position.** Image and
//    inpaint send `aspectRatios` and `promptCharacterLimit`; video sends
//    `aspect_ratios` and `prompt_character_limit`. The tables below carry
//    literal keys for exactly this reason — a mechanical camelCase rule would
//    read one of the two families as absent, on every entry, forever.
//  * **Seven keys on 100% of video models are documented nowhere**, including
//    in swagger 20260811.123440 fetched the same day. They are marked below.
//    They are also precisely what a video request builder needs, so they are
//    modeled and their provenance is recorded rather than inferred later.
//  * **The specification documents keys the wire has never sent**:
//    `frequency_penalty` and `presence_penalty` on text constraints,
//    `regionRestrictions` and `beta` on model_spec. Those are not modeled;
//    `startsAt` and `replacementModelId` are, because they sit inside a
//    Deprecation that is modeled anyway and cost one row each. The
//    `--modality` leg reports what it has never observed without failing on
//    it.
//  * **Music and ASR are deliberately out of scope** (VC-39). Music carries
//    nineteen model_spec keys of its own and no epic consumes them yet; ASR
//    carries none beyond pricing. Both stay reachable through `Model::raw`,
//    and `--modality music` prints their full key inventory so the follow-up
//    ticket starts from a measurement rather than a guess.
//
// Every string list here is optional<vector>, unlike Model::traits. In a
// constraints block "the server said none" and "the server did not say" are
// different answers a request builder must branch on differently — video sent
// `aspect_ratios: []` on 40 of 111 models, where the specification assigns the
// empty array the meaning "no defined aspect ratio". traits keeps its plain
// vector because no caller there behaves differently for absent-vs-empty.

// Image generation step bounds. A nested object rather than two scalars
// because that is how it arrives, and because it already proves such an object
// can carry more than one key.
struct StepsConstraint {
  std::optional<int> default_value;  // wire: "default"
  std::optional<int> max;
};

// One sampling parameter's server-side default, from text constraints. A
// single-key object today; `steps` above is the standing evidence that a
// single-key object is not a scalar with extra punctuation.
struct TextParamConstraint {
  std::optional<double> default_value;  // wire: "default"
};

// What a TTS model will accept as a voice-cloning reference sample.
//
// Absence is not a "no". The specification states that models whose cloning is
// gated behind a private alpha omit this field for non-staff callers while
// still appearing in the listing, so a disengaged optional means "this
// response did not say" and never "this model cannot clone". One of the
// eleven live TTS models carried it on 2026-08-11.
struct VoiceCloning {
  std::optional<std::string> mode;  // "zero_shot" | "persistent" — no enum
  std::optional<std::vector<std::string>> accepted_formats;
  std::optional<double> min_sample_seconds;
  std::optional<int> retention_days;
};

// When a model id stops working, and what to move to.
//
// Every instant stays a string. They arrive as ISO 8601 and converting them
// here would pick a calendar library for every consumer and lose the offset;
// `date` is additionally marked legacy by the specification, which prefers
// `startsAt`/`removesAt` — so a caller that coalesced the three would be
// building on the one the server is moving away from.
struct Deprecation {
  std::optional<bool> auto_remap;
  std::optional<std::string> date;        // legacy; prefer removes_at
  std::optional<std::string> removes_at;  // wire: "removesAt"
  std::optional<std::string> starts_at;   // wire: "startsAt" — never observed
  std::optional<std::string> replacement_model_id;  // never observed
};

struct ImageConstraints {
  std::optional<int> prompt_character_limit;
  std::optional<int> width_height_divisor;  // width and height must divide by it
  std::optional<int> max_style_references;
  std::optional<bool> supports_style_reference_strength;
  std::optional<std::string> default_aspect_ratio;
  std::optional<std::string> default_resolution;
  std::optional<std::string> default_quality;
  // Strings such as "16:9", never numbers. Worth stating because video's
  // reference_image_min/max_aspect_ratio are doubles: "aspect ratio" implies
  // no single type in this API.
  std::optional<std::vector<std::string>> aspect_ratios;
  std::optional<std::vector<std::string>> resolutions;
  std::optional<std::vector<std::string>> qualities;
  std::optional<StepsConstraint> steps;
};

// Overlaps ImageConstraints on six keys and is a different shape all the same
// — combineImages and maxInputImages have no image counterpart, steps and
// widthHeightDivisor no inpaint one. The overlap is why Model dispatches on
// `type` rather than on the shape of the object: no set of present keys
// distinguishes these two branches, and the specification's own anyOf carries
// no discriminator to help.
struct InpaintConstraints {
  std::optional<int> prompt_character_limit;
  std::optional<int> max_input_images;
  std::optional<bool> combine_images;
  std::optional<bool> single_image_aspect_ratio;
  std::optional<std::string> default_resolution;
  std::optional<std::string> default_quality;
  std::optional<std::vector<std::string>> aspect_ratios;
  std::optional<std::vector<std::string>> resolutions;
  std::optional<std::vector<std::string>> qualities;
};

struct VideoConstraints {
  std::optional<std::string> model_type;  // "image-to-video" | "text-to-video" | "video"
  std::optional<int> prompt_character_limit;
  std::optional<bool> audio;
  std::optional<bool> audio_configurable;
  // The three below sit on all 111 video models and appear in no published
  // specification — measured 2026-08-11 against swagger 20260811.123440. They
  // are what answers "may I hand this model an image, or a video, or a
  // per-reference audio track", which is the first question an image-to-video
  // caller has.
  std::optional<bool> audio_input;
  std::optional<bool> per_reference_audio;
  std::optional<bool> video_input;
  // Also undocumented, same capture. Doubles, not strings and not ints: the
  // live values include 0.5, and opt_double accepts a whole number so a model
  // quoting 2 still parses.
  std::optional<int> reference_image_min_short_side_pixels;
  std::optional<double> reference_image_min_aspect_ratio;
  std::optional<double> reference_image_max_aspect_ratio;
  // Engaged-but-empty is a real answer here — see the block comment above.
  std::optional<std::vector<std::string>> aspect_ratios;
  std::optional<std::vector<std::string>> resolutions;
  std::optional<std::vector<std::string>> durations;  // "5s", "10s" — not numbers
};

// Text models carry constraints too — 5 of 106 on 2026-08-11 — which is why
// this ticket is not only about media. The specification also lists
// frequency_penalty and presence_penalty; neither has ever been observed, so
// neither is modeled.
struct TextConstraints {
  std::optional<TextParamConstraint> temperature;
  std::optional<TextParamConstraint> top_p;
  std::optional<TextParamConstraint> repetition_penalty;
};

// Image models carry no `capabilities` block at all. These three flags sit
// directly on model_spec, one level above `constraints`, and folding them into
// ModelCapabilities would both invent a block the server never sent and put
// image flags in a struct whose every existing reader is a text caller.
struct ImageModelSpec {
  std::optional<ImageConstraints> constraints;
  std::optional<bool> supports_web_search;
  std::optional<bool> supports_style_references;
  std::optional<bool> supports_optimize_prompt_thinking;  // undocumented, 37/37
};

struct InpaintModelSpec {
  std::optional<InpaintConstraints> constraints;
  std::optional<bool> supports_optimize_prompt_thinking;  // undocumented, 20/20
};

struct VideoModelSpec {
  std::optional<VideoConstraints> constraints;
};

struct TtsModelSpec {
  std::optional<std::vector<std::string>> voices;
  std::optional<std::vector<std::string>> supported_formats;
  std::optional<std::string> default_format;
  std::optional<bool> supports_custom_voice_id;
  std::optional<VoiceCloning> voice_cloning;  // absent is not a "no" — see above
};

struct EmbeddingModelSpec {
  std::optional<int> embedding_dimensions;
  std::optional<int> max_input_tokens;
  std::optional<bool> supports_custom_dimensions;
};

namespace detail {

inline constexpr std::array<Field<int, StepsConstraint>, 2> kStepsIntFields{{
    {&StepsConstraint::default_value, "default"},
    {&StepsConstraint::max, "max"},
}};

inline constexpr std::array<Field<double, TextParamConstraint>, 1> kTextParamDoubleFields{{
    {&TextParamConstraint::default_value, "default"},
}};

inline constexpr std::array<Field<std::string, VoiceCloning>, 1> kVoiceCloningStringFields{{
    {&VoiceCloning::mode, "mode"},
}};
inline constexpr std::array<Field<double, VoiceCloning>, 1> kVoiceCloningDoubleFields{{
    {&VoiceCloning::min_sample_seconds, "min_sample_seconds"},
}};
inline constexpr std::array<Field<int, VoiceCloning>, 1> kVoiceCloningIntFields{{
    {&VoiceCloning::retention_days, "retention_days"},
}};
inline constexpr std::array<VectorField<std::string, VoiceCloning>, 1> kVoiceCloningListFields{{
    {&VoiceCloning::accepted_formats, "accepted_formats"},
}};

inline constexpr std::array<Field<bool, Deprecation>, 1> kDeprecationBoolFields{{
    {&Deprecation::auto_remap, "autoRemap"},
}};
inline constexpr std::array<Field<std::string, Deprecation>, 4> kDeprecationStringFields{{
    {&Deprecation::date, "date"},
    {&Deprecation::removes_at, "removesAt"},
    {&Deprecation::starts_at, "startsAt"},
    {&Deprecation::replacement_model_id, "replacementModelId"},
}};

// camelCase throughout — the image family's spelling. Compare the video table.
inline constexpr std::array<Field<int, ImageConstraints>, 3> kImageConstraintIntFields{{
    {&ImageConstraints::prompt_character_limit, "promptCharacterLimit"},
    {&ImageConstraints::width_height_divisor, "widthHeightDivisor"},
    {&ImageConstraints::max_style_references, "maxStyleReferences"},
}};
inline constexpr std::array<Field<bool, ImageConstraints>, 1> kImageConstraintBoolFields{{
    {&ImageConstraints::supports_style_reference_strength, "supportsStyleReferenceStrength"},
}};
inline constexpr std::array<Field<std::string, ImageConstraints>, 3> kImageConstraintStringFields{{
    {&ImageConstraints::default_aspect_ratio, "defaultAspectRatio"},
    {&ImageConstraints::default_resolution, "defaultResolution"},
    {&ImageConstraints::default_quality, "defaultQuality"},
}};
inline constexpr std::array<VectorField<std::string, ImageConstraints>, 3>
    kImageConstraintListFields{{
        {&ImageConstraints::aspect_ratios, "aspectRatios"},
        {&ImageConstraints::resolutions, "resolutions"},
        {&ImageConstraints::qualities, "qualities"},
    }};
inline constexpr std::array<ObjectField<StepsConstraint, ImageConstraints>, 1>
    kImageConstraintObjectFields{{
        {&ImageConstraints::steps, "steps"},
    }};

inline constexpr std::array<Field<int, InpaintConstraints>, 2> kInpaintConstraintIntFields{{
    {&InpaintConstraints::prompt_character_limit, "promptCharacterLimit"},
    {&InpaintConstraints::max_input_images, "maxInputImages"},
}};
inline constexpr std::array<Field<bool, InpaintConstraints>, 2> kInpaintConstraintBoolFields{{
    {&InpaintConstraints::combine_images, "combineImages"},
    {&InpaintConstraints::single_image_aspect_ratio, "singleImageAspectRatio"},
}};
inline constexpr std::array<Field<std::string, InpaintConstraints>, 2>
    kInpaintConstraintStringFields{{
        {&InpaintConstraints::default_resolution, "defaultResolution"},
        {&InpaintConstraints::default_quality, "defaultQuality"},
    }};
inline constexpr std::array<VectorField<std::string, InpaintConstraints>, 3>
    kInpaintConstraintListFields{{
        {&InpaintConstraints::aspect_ratios, "aspectRatios"},
        {&InpaintConstraints::resolutions, "resolutions"},
        {&InpaintConstraints::qualities, "qualities"},
    }};

// snake_case throughout — video's spelling, in the same wire position as the
// camelCase image table above. Neither is derivable from the other.
inline constexpr std::array<Field<int, VideoConstraints>, 2> kVideoConstraintIntFields{{
    {&VideoConstraints::prompt_character_limit, "prompt_character_limit"},
    {&VideoConstraints::reference_image_min_short_side_pixels,
     "reference_image_min_short_side_pixels"},
}};
inline constexpr std::array<Field<double, VideoConstraints>, 2> kVideoConstraintDoubleFields{{
    {&VideoConstraints::reference_image_min_aspect_ratio, "reference_image_min_aspect_ratio"},
    {&VideoConstraints::reference_image_max_aspect_ratio, "reference_image_max_aspect_ratio"},
}};
inline constexpr std::array<Field<bool, VideoConstraints>, 5> kVideoConstraintBoolFields{{
    {&VideoConstraints::audio, "audio"},
    {&VideoConstraints::audio_configurable, "audio_configurable"},
    {&VideoConstraints::audio_input, "audio_input"},
    {&VideoConstraints::per_reference_audio, "per_reference_audio"},
    {&VideoConstraints::video_input, "video_input"},
}};
inline constexpr std::array<Field<std::string, VideoConstraints>, 1> kVideoConstraintStringFields{{
    {&VideoConstraints::model_type, "model_type"},
}};
inline constexpr std::array<VectorField<std::string, VideoConstraints>, 3>
    kVideoConstraintListFields{{
        {&VideoConstraints::aspect_ratios, "aspect_ratios"},
        {&VideoConstraints::resolutions, "resolutions"},
        {&VideoConstraints::durations, "durations"},
    }};

inline constexpr std::array<ObjectField<TextParamConstraint, TextConstraints>, 3>
    kTextConstraintObjectFields{{
        {&TextConstraints::temperature, "temperature"},
        {&TextConstraints::top_p, "top_p"},
        {&TextConstraints::repetition_penalty, "repetition_penalty"},
    }};

inline constexpr std::array<Field<bool, ImageModelSpec>, 3> kImageSpecBoolFields{{
    {&ImageModelSpec::supports_web_search, "supportsWebSearch"},
    {&ImageModelSpec::supports_style_references, "supportsStyleReferences"},
    {&ImageModelSpec::supports_optimize_prompt_thinking, "supportsOptimizePromptThinking"},
}};
inline constexpr std::array<ObjectField<ImageConstraints, ImageModelSpec>, 1>
    kImageSpecObjectFields{{
        {&ImageModelSpec::constraints, "constraints"},
    }};

inline constexpr std::array<Field<bool, InpaintModelSpec>, 1> kInpaintSpecBoolFields{{
    {&InpaintModelSpec::supports_optimize_prompt_thinking, "supportsOptimizePromptThinking"},
}};
inline constexpr std::array<ObjectField<InpaintConstraints, InpaintModelSpec>, 1>
    kInpaintSpecObjectFields{{
        {&InpaintModelSpec::constraints, "constraints"},
    }};

inline constexpr std::array<ObjectField<VideoConstraints, VideoModelSpec>, 1>
    kVideoSpecObjectFields{{
        {&VideoModelSpec::constraints, "constraints"},
    }};

inline constexpr std::array<Field<bool, TtsModelSpec>, 1> kTtsSpecBoolFields{{
    {&TtsModelSpec::supports_custom_voice_id, "supports_custom_voice_id"},
}};
inline constexpr std::array<Field<std::string, TtsModelSpec>, 1> kTtsSpecStringFields{{
    {&TtsModelSpec::default_format, "default_format"},
}};
inline constexpr std::array<VectorField<std::string, TtsModelSpec>, 2> kTtsSpecListFields{{
    {&TtsModelSpec::voices, "voices"},
    {&TtsModelSpec::supported_formats, "supported_formats"},
}};
inline constexpr std::array<ObjectField<VoiceCloning, TtsModelSpec>, 1> kTtsSpecObjectFields{{
    {&TtsModelSpec::voice_cloning, "voice_cloning"},
}};

inline constexpr std::array<Field<int, EmbeddingModelSpec>, 2> kEmbeddingSpecIntFields{{
    {&EmbeddingModelSpec::embedding_dimensions, "embeddingDimensions"},
    {&EmbeddingModelSpec::max_input_tokens, "maxInputTokens"},
}};
inline constexpr std::array<Field<bool, EmbeddingModelSpec>, 1> kEmbeddingSpecBoolFields{{
    {&EmbeddingModelSpec::supports_custom_dimensions, "supportsCustomDimensions"},
}};

}  // namespace detail

// Free rather than friends, for the same reason ModelCapabilities' is: the
// tables above must be declared first and need the structs complete. Found by
// ADL all the same.

inline void from_json(const nlohmann::json& j, StepsConstraint& s) {
  detail::read_table(j, s, detail::kStepsIntFields);
}

inline void from_json(const nlohmann::json& j, TextParamConstraint& t) {
  detail::read_table(j, t, detail::kTextParamDoubleFields);
}

inline void from_json(const nlohmann::json& j, VoiceCloning& v) {
  detail::read_table(j, v, detail::kVoiceCloningStringFields);
  detail::read_table(j, v, detail::kVoiceCloningDoubleFields);
  detail::read_table(j, v, detail::kVoiceCloningIntFields);
  detail::read_table(j, v, detail::kVoiceCloningListFields);
}

inline void from_json(const nlohmann::json& j, Deprecation& d) {
  detail::read_table(j, d, detail::kDeprecationBoolFields);
  detail::read_table(j, d, detail::kDeprecationStringFields);
}

inline void from_json(const nlohmann::json& j, ImageConstraints& c) {
  detail::read_table(j, c, detail::kImageConstraintIntFields);
  detail::read_table(j, c, detail::kImageConstraintBoolFields);
  detail::read_table(j, c, detail::kImageConstraintStringFields);
  detail::read_table(j, c, detail::kImageConstraintListFields);
  detail::read_table(j, c, detail::kImageConstraintObjectFields);
}

inline void from_json(const nlohmann::json& j, InpaintConstraints& c) {
  detail::read_table(j, c, detail::kInpaintConstraintIntFields);
  detail::read_table(j, c, detail::kInpaintConstraintBoolFields);
  detail::read_table(j, c, detail::kInpaintConstraintStringFields);
  detail::read_table(j, c, detail::kInpaintConstraintListFields);
}

inline void from_json(const nlohmann::json& j, VideoConstraints& c) {
  detail::read_table(j, c, detail::kVideoConstraintIntFields);
  detail::read_table(j, c, detail::kVideoConstraintDoubleFields);
  detail::read_table(j, c, detail::kVideoConstraintBoolFields);
  detail::read_table(j, c, detail::kVideoConstraintStringFields);
  detail::read_table(j, c, detail::kVideoConstraintListFields);
}

inline void from_json(const nlohmann::json& j, TextConstraints& c) {
  detail::read_table(j, c, detail::kTextConstraintObjectFields);
}

inline void from_json(const nlohmann::json& j, ImageModelSpec& s) {
  detail::read_table(j, s, detail::kImageSpecBoolFields);
  detail::read_table(j, s, detail::kImageSpecObjectFields);
}

inline void from_json(const nlohmann::json& j, InpaintModelSpec& s) {
  detail::read_table(j, s, detail::kInpaintSpecBoolFields);
  detail::read_table(j, s, detail::kInpaintSpecObjectFields);
}

inline void from_json(const nlohmann::json& j, VideoModelSpec& s) {
  detail::read_table(j, s, detail::kVideoSpecObjectFields);
}

inline void from_json(const nlohmann::json& j, TtsModelSpec& s) {
  detail::read_table(j, s, detail::kTtsSpecBoolFields);
  detail::read_table(j, s, detail::kTtsSpecStringFields);
  detail::read_table(j, s, detail::kTtsSpecListFields);
  detail::read_table(j, s, detail::kTtsSpecObjectFields);
}

inline void from_json(const nlohmann::json& j, EmbeddingModelSpec& s) {
  detail::read_table(j, s, detail::kEmbeddingSpecIntFields);
  detail::read_table(j, s, detail::kEmbeddingSpecBoolFields);
}

// ── models ────────────────────────────────────────────────────────────────
//
// One entry from /models. `id` and `type` are the two fields the endpoint
// always sets and every caller needs, so they stay plain strings; everything
// else is optional, because `model_spec` is polymorphic by model type. Text
// models carry capabilities and a context window; image models carry
// generation pricing and style-reference flags; tts carries a voice list.
//
// Until VC-39 the typed surface here was the text shape alone and `raw` kept
// the rest, which made this struct the one place that knew an image model's
// constraints and could not state them. It now carries each modality's shape
// as an optional view, and three rules govern them:
//
//  * **At most one view is ever engaged, chosen by `type`.** `m.video`
//    engaged means the server called this a video model — not merely that the
//    entry happened to hold keys a video parse recognises. Image and inpaint
//    constraints overlap on six keys and no set of present keys tells them
//    apart, so shape-dispatch was never available; the specification's own
//    anyOf has no discriminator either.
//
//    What makes that true is dispatching on ONE string against distinct
//    literals, not the else-if chain in from_json: VC-39's break matrix
//    rewrote the chain as independent ifs and the suite stayed green, because
//    at most one comparison can match either way. Worth writing down rather
//    than leaving as an inference — the chain is there so the exclusivity
//    reads locally, and it is not the part a test can hold. The break that
//    does go red is replacing the `type` test with a structural one, which is
//    test/10modalities/ §1.
//  * **An unmodeled modality degrades to every view disengaged**, with `raw`
//    whole. A modality Venice adds tomorrow costs this header nothing and
//    costs its callers a `raw` walk, which is where they are today for all of
//    them.
//  * **Scope is image, inpaint, video, text, tts and embedding.** Music and
//    ASR are deliberately not modeled — see the block above `StepsConstraint`
//    — and upscale carries nothing beyond pricing.
//
// `deprecation` and `model_sets` sit on Model rather than inside a view
// because they are cross-modality: deprecation was observed on video and is
// documented for any type, and model_sets arrived on video, image and text
// alike.
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

  // At most one of these six is engaged, selected by `type`. See the note
  // above; `text_constraints` is spelled out rather than wrapped in a
  // TextModelSpec because text's other metadata has been flat on Model since
  // VC-03 and moving it would break every existing caller to buy symmetry.
  std::optional<ImageModelSpec> image;
  std::optional<InpaintModelSpec> inpaint;
  std::optional<VideoModelSpec> video;
  std::optional<TtsModelSpec> tts;
  std::optional<EmbeddingModelSpec> embedding;
  std::optional<TextConstraints> text_constraints;  // 5 of 106 text models

  // Cross-modality, so read for every type rather than inside a view.
  std::optional<Deprecation> deprecation;
  // Undocumented in swagger 20260811.123440; observed on all 111 video, 17 of
  // 37 image and 13 of 106 text models on 2026-08-11. Plain vector, like
  // traits and unlike the constraint lists: it is a tag set, and no caller
  // behaves differently for absent-vs-empty.
  std::vector<std::string> model_sets;

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

    // Cross-modality, so before the dispatch rather than repeated inside it.
    if (const auto* dep = detail::opt_object(*spec, "deprecation"))
      m.deprecation = dep->get<Deprecation>();
    m.model_sets = detail::string_array(*spec, "model_sets");

    // One branch, chosen by `type`. The chain is else-if so the exclusivity
    // reads locally, but the exclusivity comes from comparing one string
    // against distinct literals — measured, not assumed: rewriting this as
    // independent ifs leaves the whole suite green. What a stray *structural*
    // test here would break is test/10modalities/ §1, since image and inpaint
    // share six constraint keys.
    //
    // A type this client has never heard of falls off the end with every view
    // disengaged and `raw` intact, which is the correct answer and not a
    // failure. "music", "asr" and "upscale" take that path deliberately.
    if (m.type == "image")
      m.image = spec->get<ImageModelSpec>();
    else if (m.type == "inpaint")
      m.inpaint = spec->get<InpaintModelSpec>();
    else if (m.type == "video")
      m.video = spec->get<VideoModelSpec>();
    else if (m.type == "tts")
      m.tts = spec->get<TtsModelSpec>();
    else if (m.type == "embedding")
      m.embedding = spec->get<EmbeddingModelSpec>();
    else if (m.type == "text") {
      if (const auto* c = detail::opt_object(*spec, "constraints"))
        m.text_constraints = c->get<TextConstraints>();
    }
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


// ── the catalogue's own answers: traits and compatibility mapping ─────────
//
// Two operations (VC-38, #59) that answer the question `models()` leaves to the
// caller: of the hundred-odd entries, which one. /models/traits maps a Venice
// capability name to the model that currently holds it — "default", "fastest",
// "most_uncensored" — and /models/compatibility_mapping maps a *foreign* vendor's
// model id to the Venice model that serves it, so a caller porting from an
// OpenAI-shaped client can look up what "gpt-4o" resolves to here.
//
// Both are public. Measured 2026-08-11: both answer 200 with no Authorization
// header at all, and traits answers 200 even for an *invalid* bearer. So does
// /models, on the same measurement — the whole Models family is reachable
// without a key, where /characters answers 402. What is new here is not the
// capability but that a live leg finally exercises it: --traits and --compat
// run with no VENICE_API_KEY in the environment.
//
// Both speak the same three-key envelope over a flat string->string object:
//
//   {"data": {"default": "zai-org-glm-4.7", ...}, "object": "list", "type": "text"}
//
// Note "object":"list" over something that is not a list. Recorded as measured,
// not corrected.

// The map itself. std::less<> so a lookup from a string_view costs no allocation.
//
// A map rather than a vector of pairs, and the reason is a claim this type would
// otherwise make falsely: nlohmann's json object IS a std::map on the pinned
// 3.11.3, so the server's insertion order is destroyed before any parser here
// sees the body. A vector would preserve nlohmann's lexicographic sort while
// looking like it preserved the wire's — an ordering guarantee invented by the
// container choice. A map claims nothing, and iterates in the same order as
// raw["data"].items(), which is what lets a caller print the typed and verbatim
// views side by side without sorting either.
using StringMap = std::map<std::string, std::string, std::less<>>;

namespace detail {

// The shared shape behind both operations. Not a public type: the two public
// ones are distinct on purpose (see below) and this is only how they avoid
// spelling the same parse twice.
struct StringMapEnvelope {
  StringMap entries;
  // How many members the server put in `data`, before any skipping — the same
  // contract CharacterPage::returned carries, so a reader who knows one knows
  // the other. `returned != entries.size()` is how a caller detects that
  // something arrived unusable, which a bare map cannot express.
  std::size_t returned{0};
  std::optional<std::string> object;
  std::optional<std::string> type;
  nlohmann::json raw;  // the whole envelope, verbatim superset
};

// `what` names the operation in the throw text, so the ErrorKind::Parse message
// Client builds says which of the two calls failed.
[[nodiscard]] inline auto string_map_envelope_from_json_body(const nlohmann::json& j,
                                                             const char* what)
    -> StringMapEnvelope {
  // This is where this parser diverges from models_from_json_body above, and the
  // divergence is load-bearing rather than stylistic.
  //
  // Every list parser in this file opens with `opt_array(j,"data")` and falls
  // back to the whole body when it is absent. That fallback is safe there only
  // because it demands an ARRAY while the envelope is an OBJECT: the two are
  // type-disjoint, so the fallback can only ever fire on a body that really is a
  // bare list. Here both levels are objects. The same idiom would be
  // type-INDISTINGUISHABLE — `{"object":"list","type":"text"}` would parse into a
  // two-entry map with keys `object` and `type` and report success, which is the
  // "garbage factory that reports success" the is_array() check upstream exists
  // to prevent, reintroduced one level over. So `data` is required, and required
  // to be an object.
  //
  // Generalised: the envelope-fallback idiom is only safe when the inner
  // container's JSON type differs from the envelope's. Three parsers in this file
  // use it and all three pass that test. This one does not.
  const auto* data = detail::opt_object(j, "data");
  if (data == nullptr)
    throw std::runtime_error{std::string{what} + ": response has no data object"};

  StringMapEnvelope out;
  out.raw = j;
  out.object = detail::opt_string(j, "object");
  out.type = detail::opt_string(j, "type");
  out.returned = data->size();

  for (const auto& [key, value] : data->items()) {
    // A value that is not a string is skipped and left in `raw`, on the listing
    // rule: a `default_image` that arrives as null must not cost the caller the
    // other nine traits. The skip is detectable two ways — find() gives nullptr,
    // a state callers already branch on, and `returned` still counts it.
    //
    // An empty KEY, by contrast, is kept. Unlike Model::id nothing downstream is
    // ever handed this key, so an empty one is inert rather than a value whose
    // only future is to come back as a 400 far from here.
    if (!value.is_string()) continue;
    out.entries.emplace(key, value.get<std::string>());
  }
  return out;
}

}  // namespace detail

// The two results are structurally identical and deliberately distinct types.
//
// The keys are different vocabularies. A traits key is a Venice-owned capability
// name; a compatibility key is a foreign vendor's model id. `find("gpt-4o")`
// against a traits result is a bug, and one shared type would make it a silent
// nullptr instead of a compile error.
//
// The operations also already disagree about their own input, which is the
// empirical half of the argument. Measured 2026-08-11: `traits?type=all` returns
// 200, `compatibility_mapping?type=all` returns 400 — despite byte-identical
// `parameters` blocks in the OpenAPI document. One shared doc comment about the
// accepted `type` set would be wrong for one of the two.

// /models/traits — capability name -> the model that currently holds it.
struct ModelTraits {
  StringMap entries;
  std::size_t returned{0};
  // Both optional rather than plain strings, because `type` echoes the effective
  // filter and is therefore something callers compare against what they asked
  // for. A plain string would map "the server did not say" and "the server sent a
  // number" both onto "", firing that comparison on a field that was merely
  // malformed. Neither field is a handle handed back to the API, so neither has
  // Model::id's reason to be plain.
  std::optional<std::string> object;
  std::optional<std::string> type;
  nlohmann::json raw;

  // Pointer-returning, mirroring detail::opt_string_at: no it != end() dance at
  // every call site, and no .at() that throws across the public API.
  [[nodiscard]] auto find(std::string_view key) const -> const std::string* {
    const auto it = entries.find(key);
    return it != entries.end() ? &it->second : nullptr;
  }
};

// /models/compatibility_mapping — foreign vendor model id -> the Venice model.
struct ModelCompatibilityMapping {
  StringMap entries;
  std::size_t returned{0};
  std::optional<std::string> object;
  std::optional<std::string> type;
  nlohmann::json raw;

  [[nodiscard]] auto find(std::string_view key) const -> const std::string* {
    const auto it = entries.find(key);
    return it != entries.end() ? &it->second : nullptr;
  }
};

[[nodiscard]] inline auto model_traits_from_json_body(const nlohmann::json& j) -> ModelTraits {
  auto env = detail::string_map_envelope_from_json_body(j, "model traits");
  // Designated, not positional: `object` and `type` are adjacent
  // optional<string> members, so a positional list would survive reordering them
  // and silently swap the two — and no fixture could see it, because every
  // capture sets both.
  return ModelTraits{.entries = std::move(env.entries),
                     .returned = env.returned,
                     .object = std::move(env.object),
                     .type = std::move(env.type),
                     .raw = std::move(env.raw)};
}

[[nodiscard]] inline auto model_compatibility_mapping_from_json_body(const nlohmann::json& j)
    -> ModelCompatibilityMapping {
  auto env = detail::string_map_envelope_from_json_body(j, "model compatibility mapping");
  return ModelCompatibilityMapping{.entries = std::move(env.entries),
                                   .returned = env.returned,
                                   .object = std::move(env.object),
                                   .type = std::move(env.type),
                                   .raw = std::move(env.raw)};
}


// ── characters ────────────────────────────────────────────────────────────
//
// `venice_parameters.character_slug` has been sendable since Phase 0, and until
// VC-04 (#5) nothing here could tell you what slugs exist — the feature was
// reachable only by someone who already knew the answer.
//
// Venice marks /characters a *preview* API that may change, which is the
// strongest argument this file has for the tolerant-read discipline above: the
// shape below is the one documented today, `raw` carries whatever it becomes.

// The engagement numbers on a character, all quoted as JSON numbers rather than
// integers — hence opt_double throughout and not opt_int. `imports` and
// `rating_count` are counts and will read as whole doubles; taking them as int
// would be a narrowing this file has no reason to risk for a display value.
//
// `user_rating` is documented nullable: it is the *authenticated caller's* own
// rating, absent when they have not rated. Absent and unrated are the same
// answer here, so no third state is modeled.
struct CharacterStats {
  std::optional<double> average_rating;
  std::optional<double> imports;
  std::optional<double> rating_count;
  std::optional<double> rating_sum;
  std::optional<double> user_rating;

  friend void from_json(const nlohmann::json& j, CharacterStats& s) {
    s.average_rating = detail::opt_double(j, "averageRating");
    s.imports = detail::opt_double(j, "imports");
    s.rating_count = detail::opt_double(j, "ratingCount");
    s.rating_sum = detail::opt_double(j, "ratingSum");
    s.user_rating = detail::opt_double(j, "userRating");
  }
};

// One entry from /characters.
//
// `slug` is the only plain field, and it is `slug` rather than `id` for the
// reason Model keeps `id`: it is the one this library can hand back to the API.
// `character_slug` is what VeniceParameters sends; the `id` here is a UUID no
// call in this client accepts. So an entry without a usable slug is not a
// degraded character, it is not a character — see characters_from_json_body.
//
// `created_at` / `updated_at` stay strings. Model::created is epoch seconds and
// these are ISO-8601 timestamps: two endpoints, two shapes, and neither is
// worth a date parser in a header-only client that already hands you `raw`.
//
// Wire keys are camelCase here (modelId, photoUrl, webEnabled, shareUrl) where
// /models is snake_case at the top level — the same split Model already lives
// with inside model_spec. Field names stay snake_case regardless.
//
// `raw` is the whole entry verbatim, superset and never sent, on Model::raw's
// three rules — and it matters more here, because a preview API is exactly the
// one whose unmodeled keys are worth keeping.
struct Character {
  std::string slug;

  std::optional<std::string> id;
  std::optional<std::string> name;
  std::optional<std::string> description;
  std::optional<std::string> author;    // short anonymized author identifier
  std::optional<std::string> model_id;  // the model this character runs on
  std::optional<std::string> share_url;
  std::optional<std::string> photo_url;
  std::optional<std::string> created_at;
  std::optional<std::string> updated_at;

  std::optional<bool> adult;
  std::optional<bool> featured;
  std::optional<bool> web_enabled;

  // Plain, like Model::traits: empty is a truthful answer to both "no tags" and
  // "the response did not say", and no caller branches on the difference.
  std::vector<std::string> tags;

  std::optional<CharacterStats> stats;

  nlohmann::json raw;

  friend void from_json(const nlohmann::json& j, Character& c) {
    // Cleared rather than left alone, unlike Model::from_json's `id`. Every
    // other field here is assigned unconditionally — opt_string on an absent
    // key assigns nullopt — so leaving these two conditional would make a
    // re-parse into a used object keep the *previous* entry's slug and stats
    // beside the new entry's everything-else. The slug is the field that
    // selects a persona, so the failure is chatting with the wrong one while
    // every other field on screen says otherwise.
    c.slug.clear();
    c.stats.reset();

    if (const auto* s = detail::opt_string_at(j, "slug")) c.slug = *s;

    c.raw = j;
    c.id = detail::opt_string(j, "id");
    c.name = detail::opt_string(j, "name");
    c.description = detail::opt_string(j, "description");
    c.author = detail::opt_string(j, "author");
    c.model_id = detail::opt_string(j, "modelId");
    c.share_url = detail::opt_string(j, "shareUrl");
    c.photo_url = detail::opt_string(j, "photoUrl");
    c.created_at = detail::opt_string(j, "createdAt");
    c.updated_at = detail::opt_string(j, "updatedAt");

    c.adult = detail::opt_bool(j, "adult");
    c.featured = detail::opt_bool(j, "featured");
    c.web_enabled = detail::opt_bool(j, "webEnabled");

    c.tags = detail::string_array(j, "tags");

    if (const auto* s = detail::opt_object(j, "stats")) c.stats = s->get<CharacterStats>();
  }
};

// Parse a /characters/{slug} response. Unlike the listing, there is no sibling
// entry to protect, so the top-level object shape is loud; fields inside it keep
// Character's tolerant preview-API reads. In particular, this does not invent a
// missing response slug from the requested path — an absent modeled field
// remains absent.
//
// **The response is an envelope**, `{"data": {...}, "object": "character"}`, and
// the `data` member is unwrapped here exactly as the listing unwraps its
// array-valued one. VC-16 shipped without that (VC-37, #57): it read the
// OpenAPI document's `properties.data.properties` as the body rather than as
// the body's `data` member, so every typed field came back absent and only
// `raw` was usable. The document was not ambiguous; the reading was. What made
// it survive a test suite was that the fixtures were written from the same
// misreading, so parser and fixture agreed — measured against the live API on
// 2026-08-11, which is the only thing that could have disagreed.
//
// A bare object still parses, and that is deliberate rather than
// bug-compatibility: the same tolerant read is what lets an entry taken from a
// listing be re-parsed here.
//
// `raw` is the entry, not the envelope — `Character::raw` means "this
// character, verbatim" everywhere else in this library, and the envelope's only
// other key is the `object` discriminator.
[[nodiscard]] inline auto character_from_json_body(const nlohmann::json& j) -> Character {
  if (!j.is_object())
    throw std::runtime_error{"character: response is not an object"};
  const auto* data = detail::opt_object(j, "data");
  return data != nullptr ? data->get<Character>() : j.get<Character>();
}

// One page of /characters.
//
// A page and not a bare vector, which is where this type differs from
// models() — and the difference is forced by the endpoint, not chosen. Venice
// pages /characters and /models is unpaginated, so a caller here has to answer
// "was that the last page?" and `entries.size()` cannot tell them: the parse
// skips entries it cannot use, so a full page containing one slug-less entry
// comes back short and a size-versus-limit test ends the walk early, reporting
// a truncated catalogue as a complete one. `returned` is the server's own count
// and is the only honest thing to compare a limit against.
//
// `raw` is the whole envelope, on Character::raw's reasoning applied one level
// up. The documented body is `{data, object}` with no total and no cursor —
// but that shape has never been seen on a wire here (see the STATUS entry), and
// a total appearing later must be an additive field a caller can already reach
// rather than a breaking change to this signature.
struct CharacterPage {
  // Usable characters. An entry the parse could not use is not here and *is*
  // counted in `returned`.
  std::vector<Character> entries;

  // How many elements the server put in `data`, before any skipping. Compare
  // this against CharacterQuery::limit to decide whether to ask for more.
  std::size_t returned{0};

  nlohmann::json raw;
};

// Parse a /characters response body into a page. Same contract as
// models_from_json_body, for the same reasons and with the same one fatal case:
// a body that is not a list has nothing to degrade to, and without the
// is_array() check a `{"data":{...}}` quietly becomes a vector of characters
// built out of whatever the values happened to be.
[[nodiscard]] inline auto characters_from_json_body(const nlohmann::json& j) -> CharacterPage {
  const auto* data = detail::opt_array(j, "data");
  const nlohmann::json& arr = data != nullptr ? *data : j;
  if (!arr.is_array())
    throw std::runtime_error{"characters: response is not a list"};

  CharacterPage page;
  page.raw = j;
  page.returned = arr.size();
  for (const auto& e : arr) {
    if (!e.is_object()) continue;
    const auto* slug = detail::opt_string_at(e, "slug");
    if (slug == nullptr || slug->empty()) continue;
    page.entries.push_back(e.get<Character>());
  }
  return page;
}

// The filters /characters accepts.
//
// A struct rather than positional parameters the way models(type) took one,
// because there are eleven of them — but the deciding field is `limit`. The
// endpoint pages: it defaults to 50, caps at 100, and the response carries no
// total, so a listing call with no way to say "the next 50" silently answers
// the first 50 of N. That is the same defect this ticket exists to fix, one
// layer down, and it is why this type exists at all.
//
// Nothing here is an enum. `sort_by` and `sort_order` have documented value
// sets — featured, highestRating, mostRecent, … — and they are still strings,
// on the reasoning that keeps models(type) one: the value set belongs to
// Venice, and a list hardcoded here starts refusing valid values the day one is
// added. A bad sort is the server's 400 to give.
//
// `extra` is request-side and therefore keeps that name, unlike Character::raw
// — it is additive and it is sent. Its contract is the same as
// ChatRequest::extra's: modeled fields win. Duplicate query keys are not
// something servers agree on, so character_query_params drops an extra pair
// whose key a set field already emitted rather than sending the key twice.
struct CharacterQuery {
  std::optional<std::string> search;
  std::optional<std::string> sort_by;
  std::optional<std::string> sort_order;

  // Ints, not size_t: they are sent as decimal text and the server's own bounds
  // (offset >= 0, 0 < limit <= 100) fit in an int many times over. Out-of-range
  // values are passed through to the 400 they earn, per AGENTS.md's
  // "range checking: none".
  std::optional<int> limit;
  std::optional<int> offset;

  std::optional<bool> is_adult;
  std::optional<bool> is_pro;
  std::optional<bool> is_web_enabled;

  // Each element is sent as its own repetition of the key —
  // `?tags=helpful&tags=productivity`, which the endpoint documents as the
  // primary form and which was measured against the live API on 2026-08-09:
  // repetition is honoured and means OR, not last-wins.
  //
  //   tags=Buddhism                 -> 2   {alan-watts, alan-watts-2}
  //   tags=mythology                -> 2   {loki, talos}
  //   tags=Buddhism&tags=mythology  -> 4   the union of both
  //
  // What that same run refuted, and it was this comment's original claim: that
  // repetition is what lets a value containing a comma survive. It does not.
  // `tags=Buddhism%2Cmythology` — one repetition, one percent-encoded comma —
  // also returned 4, so the server splits on commas *inside* a value and a tag
  // containing one cannot be expressed by any spelling this client could
  // choose. That is the endpoint's behaviour, not the encoder's, and no
  // comment here should promise otherwise.
  //
  // Repetition stays, on the two grounds that survived: it is the documented
  // primary form, and it does not depend on the server's comma-splitting
  // continuing to behave this way.
  std::vector<std::string> tags;
  std::vector<std::string> categories;
  std::vector<std::string> model_id;

  std::vector<std::pair<std::string, std::string>> extra;
};

// Flatten a CharacterQuery into ordered key/value pairs, ready for
// detail::with_query to encode.
//
// Free, at namespace scope, and returning pairs rather than a finished query
// string: this is the half worth testing and none of it needs a socket — the
// same move models_from_json_body and percent_encode both made.
//
// Emission order is the declaration order of CharacterQuery, so the resulting
// URL is deterministic and a test can assert the whole string rather than
// picking through it. An unset field, an empty string and an empty vector all
// emit nothing at all, which is what keeps a default-constructed query sending
// the bare /characters.
[[nodiscard]] inline auto character_query_params(const CharacterQuery& q)
    -> std::vector<std::pair<std::string, std::string>> {
  std::vector<std::pair<std::string, std::string>> out;

  const auto add = [&out](std::string key, std::string value) {
    if (value.empty()) return;
    out.emplace_back(std::move(key), std::move(value));
  };
  const auto add_bool = [&add](const char* key, const std::optional<bool>& v) {
    if (v) add(key, *v ? "true" : "false");
  };
  // One repetition of the key per element, empty elements skipped. See the
  // note on CharacterQuery::tags for why this is not comma-joined.
  const auto add_each = [&add](const char* key, const std::vector<std::string>& values) {
    for (const auto& v : values) add(key, v);
  };

  if (q.search) add("search", *q.search);
  if (q.sort_by) add("sortBy", *q.sort_by);
  if (q.sort_order) add("sortOrder", *q.sort_order);
  if (q.limit) add("limit", std::to_string(*q.limit));
  if (q.offset) add("offset", std::to_string(*q.offset));
  add_bool("isAdult", q.is_adult);
  add_bool("isPro", q.is_pro);
  add_bool("isWebEnabled", q.is_web_enabled);
  add_each("tags", q.tags);
  add_each("categories", q.categories);
  add_each("modelId", q.model_id);

  for (const auto& [key, value] : q.extra) {
    if (key.empty() || value.empty()) continue;
    const bool taken = std::any_of(out.begin(), out.end(),
                                   [&key](const auto& p) { return p.first == key; });
    if (taken) continue;
    out.emplace_back(key, value);
  }
  return out;
}

// ── character reviews (VC-36, #56) ────────────────────────────────────────
//
// The half of a character's rating this library could not reach. CharacterStats
// has carried averageRating, ratingCount and ratingSum since VC-04, so a caller
// could see that a persona is rated 4.7 and not one word of why.
//
// Preview API, like the rest of the family, so `raw` at both levels matters for
// the same reason it does on Character.
//
// **One rule decides int versus double here, and it is worth stating once.**
// A number a caller *computes* with is an int, read through detail::opt_int,
// which range-checks rather than truncating (see the header note above it). A
// number a caller *displays* is a double. So pagination is int — it drives the
// paging loop, and a value this platform cannot represent has to read unknown
// rather than wrong — while the ratings are doubles, which is the call
// CharacterStats already made for exactly these numbers.

// One review of one character.
//
// Every field is optional, and that is the difference from Character, not an
// oversight. Character::slug is plain because it is the one value this client
// can hand back to the API; nothing on a review is such a handle — `id` and
// `characterId` are UUIDs no call here accepts — so there is no field whose
// absence makes this not a review. See character_reviews_from_json_body for
// what that costs the skip rule.
//
// `created_at` stays a string, on Character's reasoning: an ISO-8601 timestamp
// is not worth a date parser in a header-only client that hands you `raw`.
//
// `locale`, `message` and `user_avatar_url` are documented nullable. A JSON null
// reads as nullopt, which is the same answer as absent — no caller branches on
// the difference between "wrote no message" and "sent no message key".
struct CharacterReview {
  std::optional<std::string> id;
  std::optional<std::string> character_id;
  std::optional<std::string> created_at;
  std::optional<std::string> locale;
  std::optional<std::string> message;
  std::optional<std::string> user_avatar_url;
  std::optional<std::string> username;

  std::optional<bool> is_owner;  // whether the authenticated caller wrote it

  // Documented as an integer 1-5 and modeled as a double anyway: it is a
  // display value, it sits beside CharacterStats::average_rating, and a preview
  // API that starts sending 4.5 should not silently read as absent.
  std::optional<double> rating;

  nlohmann::json raw;

  friend void from_json(const nlohmann::json& j, CharacterReview& r) {
    r.raw = j;
    r.id = detail::opt_string(j, "id");
    r.character_id = detail::opt_string(j, "characterId");
    r.created_at = detail::opt_string(j, "createdAt");
    r.locale = detail::opt_string(j, "locale");
    r.message = detail::opt_string(j, "message");
    r.user_avatar_url = detail::opt_string(j, "userAvatarUrl");
    r.username = detail::opt_string(j, "username");
    r.is_owner = detail::opt_bool(j, "isOwner");
    r.rating = detail::opt_double(j, "rating");
  }
};

// Where the caller is in the reviews of one character.
//
// Ints, per the rule above: these are the four numbers a paging loop does
// arithmetic on. detail::opt_int reads only an integral JSON number in int
// range, so a float or an out-of-range value reads absent — a loop that stops
// early is recoverable, a loop driven by a truncated page number is not.
struct CharacterReviewPagination {
  std::optional<int> page;
  std::optional<int> page_size;
  std::optional<int> total;
  std::optional<int> total_pages;

  friend void from_json(const nlohmann::json& j, CharacterReviewPagination& p) {
    p.page = detail::opt_int(j, "page");
    p.page_size = detail::opt_int(j, "pageSize");
    p.total = detail::opt_int(j, "total");
    p.total_pages = detail::opt_int(j, "totalPages");
  }
};

// The character's rating, as the reviews endpoint reports it. Doubles on both,
// matching CharacterStats: `total_reviews` is a count, and taking it as an int
// here while ratingCount is a double there would be two answers to one
// question.
struct CharacterReviewSummary {
  std::optional<double> average_rating;
  std::optional<double> total_reviews;

  friend void from_json(const nlohmann::json& j, CharacterReviewSummary& s) {
    s.average_rating = detail::opt_double(j, "averageRating");
    s.total_reviews = detail::opt_double(j, "totalReviews");
  }
};

// One page of /characters/{slug}/reviews.
//
// `pagination` is what makes this different from CharacterPage in practice: the
// listing has no total and no cursor, so `returned` versus the requested limit
// is the only way to know whether to ask for more. Here the server says
// outright how many pages there are. `returned` is kept anyway, on the same
// contract CharacterPage gives it — the count of elements the server put in
// `data`, before any skipping — because a preview API may send a page whose
// pagination object is absent or malformed, and then it is all a caller has.
struct CharacterReviewPage {
  std::vector<CharacterReview> entries;
  std::size_t returned{0};

  std::optional<CharacterReviewPagination> pagination;
  std::optional<CharacterReviewSummary> summary;

  nlohmann::json raw;
};

// Parse a /characters/{slug}/reviews response body into a page.
//
// Same container contract as characters_from_json_body, and the same single
// fatal case: a body that is not a list has nothing to degrade to.
//
// The skip rule deliberately differs from the listing's. There, an entry
// without a usable slug is dropped, because a slug is what a caller feeds back
// to the API and an entry lacking one is not a character. No field here is such
// a handle, so nothing is dropped for a missing field — an entry with only a
// message is still that reviewer's message. Only a non-object element is
// skipped, and it stays counted in `returned`, so the two numbers still differ
// exactly when something was unusable.
[[nodiscard]] inline auto character_reviews_from_json_body(const nlohmann::json& j)
    -> CharacterReviewPage {
  const auto* data = detail::opt_array(j, "data");
  const nlohmann::json& arr = data != nullptr ? *data : j;
  if (!arr.is_array())
    throw std::runtime_error{"character reviews: response is not a list"};

  CharacterReviewPage page;
  page.raw = j;
  page.returned = arr.size();
  for (const auto& e : arr) {
    if (!e.is_object()) continue;
    page.entries.push_back(e.get<CharacterReview>());
  }

  if (j.is_object()) {
    if (const auto* p = detail::opt_object(j, "pagination"))
      page.pagination = p->get<CharacterReviewPagination>();
    if (const auto* s = detail::opt_object(j, "summary"))
      page.summary = s->get<CharacterReviewSummary>();
  }
  return page;
}

// The two filters /characters/{slug}/reviews accepts.
//
// A separate type from CharacterQuery, and the epic (#42) says why: reviews page
// by page/pageSize and the listing pages by offset/limit. One pagination type
// covering both would have fields that silently mean different things —
// `page = 2` skipping one page and `offset = 2` skipping two entries is the
// kind of difference nobody reads twice.
//
// `extra` carries the same contract CharacterQuery::extra does: additive, sent,
// and a pair whose key a modeled field already emitted is dropped rather than
// sent twice.
// Every member carries an explicit default, which CharacterQuery's do not, and
// that is for the caller rather than for this file: two fields is few enough
// that `client.character_reviews(slug, {.page = 2})` is the natural spelling,
// and designated initialization of an aggregate whose remaining members have no
// initializer is a -Wmissing-field-initializers warning in the *caller's*
// build. RequestOptions spells its defaults out for the same reason.
struct CharacterReviewQuery {
  // Ints, and unchecked, per AGENTS.md's "range checking: none" — the server
  // owns `page > 0` and `0 < pageSize <= 100` and gives the 400 that says so.
  std::optional<int> page = std::nullopt;
  std::optional<int> page_size = std::nullopt;

  std::vector<std::pair<std::string, std::string>> extra = {};
};

// Flatten a CharacterReviewQuery into ordered key/value pairs. Free, and for
// the reasons character_query_params is: this is the half worth testing and
// none of it needs a socket. A default-constructed query emits nothing, which
// is what keeps the bare /characters/{slug}/reviews free of a trailing '?'.
[[nodiscard]] inline auto character_review_query_params(const CharacterReviewQuery& q)
    -> std::vector<std::pair<std::string, std::string>> {
  std::vector<std::pair<std::string, std::string>> out;

  if (q.page) out.emplace_back("page", std::to_string(*q.page));
  if (q.page_size) out.emplace_back("pageSize", std::to_string(*q.page_size));

  for (const auto& [key, value] : q.extra) {
    if (key.empty() || value.empty()) continue;
    const bool taken = std::any_of(out.begin(), out.end(),
                                   [&key](const auto& p) { return p.first == key; });
    if (taken) continue;
    out.emplace_back(key, value);
  }
  return out;
}

// ── billing ───────────────────────────────────────────────────────────────
//
// Venice publishes billing values as JSON numbers, not decimal strings. The
// typed values below are therefore display/arithmetic approximations, like
// Price's usd/diem members; they do not promise exact decimal-ledger equality.
// The usage-history CSV alternative is retained byte-for-byte for callers that
// need the server's exact export representation.

struct BillingBalanceBuckets {
  std::optional<double> diem{};
  std::optional<double> usd{};
  nlohmann::json raw{};
};

struct BillingBalance {
  std::optional<bool> can_consume{};
  std::optional<std::string> consumption_currency{};
  std::optional<BillingBalanceBuckets> balances{};
  std::optional<double> diem_epoch_allocation{};
  ResponseMetadata metadata{};
  nlohmann::json raw{};
};

[[nodiscard]] inline auto billing_balance_from_json_body(const nlohmann::json& j)
    -> BillingBalance {
  if (!j.is_object()) throw std::runtime_error{"billing balance: response must be an object"};

  BillingBalance response;
  response.raw = j;
  response.can_consume = detail::opt_bool(j, "canConsume");
  response.consumption_currency = detail::opt_string(j, "consumptionCurrency");
  response.diem_epoch_allocation = detail::opt_double(j, "diemEpochAllocation");
  if (const auto* value = detail::opt_object(j, "balances")) {
    BillingBalanceBuckets buckets;
    buckets.raw = *value;
    buckets.diem = detail::opt_double(*value, "diem");
    buckets.usd = detail::opt_double(*value, "usd");
    response.balances = std::move(buckets);
  }
  return response;
}

struct BillingUsageAnalyticsQuery {
  std::optional<std::string> lookback = std::nullopt;
  std::optional<std::string> start_date = std::nullopt;
  std::optional<std::string> end_date = std::nullopt;
  std::vector<std::pair<std::string, std::string>> extra = {};
};

[[nodiscard]] inline auto billing_usage_analytics_query_params(
    const BillingUsageAnalyticsQuery& q)
    -> std::vector<std::pair<std::string, std::string>> {
  std::vector<std::pair<std::string, std::string>> out;
  const auto add = [&out](std::string key, const std::optional<std::string>& value) {
    if (value && !value->empty()) out.emplace_back(std::move(key), *value);
  };
  add("lookback", q.lookback);
  add("startDate", q.start_date);
  add("endDate", q.end_date);
  for (const auto& [key, value] : q.extra) {
    if (key.empty() || value.empty()) continue;
    const bool taken = std::any_of(out.begin(), out.end(),
                                   [&key](const auto& p) { return p.first == key; });
    if (!taken) out.emplace_back(key, value);
  }
  return out;
}

struct BillingUsageByDate {
  std::optional<std::string> date{};
  std::optional<double> usd{};
  std::optional<double> diem{};
  nlohmann::json raw{};
};

struct BillingUsageBreakdown {
  std::optional<std::string> type{};
  std::optional<double> usd{};
  std::optional<double> diem{};
  std::optional<double> units{};
  nlohmann::json raw{};
};

struct BillingUsageByModel {
  std::optional<std::string> model_name{};
  std::optional<std::string> unit_type{};
  std::optional<std::string> model_type{};
  std::optional<double> total_usd{};
  std::optional<double> total_diem{};
  std::optional<double> total_units{};
  std::optional<std::vector<BillingUsageBreakdown>> breakdown{};
  nlohmann::json raw{};
};

struct BillingUsageByKey {
  std::optional<std::string> api_key_id{};
  std::optional<std::string> description{};
  std::optional<double> total_usd{};
  std::optional<double> total_diem{};
  std::optional<double> total_units{};
  nlohmann::json raw{};
};

struct BillingUsageAnalytics {
  std::optional<std::string> lookback{};
  std::optional<std::vector<BillingUsageByDate>> by_date{};
  std::optional<std::vector<BillingUsageByModel>> by_model{};
  // Keys other than `date` are response-generated model/key display names.
  // Keeping each chart object whole avoids freezing those names into an API.
  std::optional<std::vector<nlohmann::json>> by_model_daily{};
  std::optional<std::vector<std::string>> top_models{};
  std::optional<std::vector<BillingUsageByKey>> by_key{};
  std::optional<std::vector<nlohmann::json>> by_key_daily{};
  std::optional<std::vector<std::string>> top_key_names{};
  ResponseMetadata metadata{};
  nlohmann::json raw{};
};

[[nodiscard]] inline auto billing_usage_analytics_from_json_body(const nlohmann::json& j)
    -> BillingUsageAnalytics {
  if (!j.is_object())
    throw std::runtime_error{"billing usage analytics: response must be an object"};

  BillingUsageAnalytics response;
  response.raw = j;
  response.lookback = detail::opt_string(j, "lookback");

  if (const auto* values = detail::opt_array(j, "byDate")) {
    response.by_date.emplace();
    response.by_date->reserve(values->size());
    for (const auto& value : *values) {
      BillingUsageByDate item;
      item.raw = value;
      if (value.is_object()) {
        item.date = detail::opt_string(value, "date");
        item.usd = detail::opt_double(value, "USD");
        item.diem = detail::opt_double(value, "DIEM");
      }
      response.by_date->push_back(std::move(item));
    }
  }

  if (const auto* values = detail::opt_array(j, "byModel")) {
    response.by_model.emplace();
    response.by_model->reserve(values->size());
    for (const auto& value : *values) {
      BillingUsageByModel item;
      item.raw = value;
      if (value.is_object()) {
        item.model_name = detail::opt_string(value, "modelName");
        item.unit_type = detail::opt_string(value, "unitType");
        item.model_type = detail::opt_string(value, "modelType");
        item.total_usd = detail::opt_double(value, "totalUsd");
        item.total_diem = detail::opt_double(value, "totalDiem");
        item.total_units = detail::opt_double(value, "totalUnits");
        if (const auto* parts = detail::opt_array(value, "breakdown")) {
          item.breakdown.emplace();
          item.breakdown->reserve(parts->size());
          for (const auto& part : *parts) {
            BillingUsageBreakdown parsed;
            parsed.raw = part;
            if (part.is_object()) {
              parsed.type = detail::opt_string(part, "type");
              parsed.usd = detail::opt_double(part, "usd");
              parsed.diem = detail::opt_double(part, "diem");
              parsed.units = detail::opt_double(part, "units");
            }
            item.breakdown->push_back(std::move(parsed));
          }
        }
      }
      response.by_model->push_back(std::move(item));
    }
  }

  const auto copy_raw_array = [&j](const char* key)
      -> std::optional<std::vector<nlohmann::json>> {
    const auto* values = detail::opt_array(j, key);
    if (values == nullptr) return std::nullopt;
    return values->get<std::vector<nlohmann::json>>();
  };
  response.by_model_daily = copy_raw_array("byModelDaily");
  response.top_models = detail::opt_string_array(j, "topModels");

  if (const auto* values = detail::opt_array(j, "byKey")) {
    response.by_key.emplace();
    response.by_key->reserve(values->size());
    for (const auto& value : *values) {
      BillingUsageByKey item;
      item.raw = value;
      if (value.is_object()) {
        item.api_key_id = detail::opt_string(value, "apiKeyId");
        item.description = detail::opt_string(value, "description");
        item.total_usd = detail::opt_double(value, "totalUsd");
        item.total_diem = detail::opt_double(value, "totalDiem");
        item.total_units = detail::opt_double(value, "totalUnits");
      }
      response.by_key->push_back(std::move(item));
    }
  }
  response.by_key_daily = copy_raw_array("byKeyDaily");
  response.top_key_names = detail::opt_string_array(j, "topKeyNames");
  return response;
}

struct BillingUsageHistoryQuery {
  std::optional<std::string> currency = std::nullopt;
  std::optional<std::string> cursor = std::nullopt;
  std::optional<std::string> end_timestamp = std::nullopt;
  std::optional<int> page_size = std::nullopt;
  std::optional<std::string> start_timestamp = std::nullopt;
  std::vector<std::pair<std::string, std::string>> extra = {};
};

[[nodiscard]] inline auto billing_usage_history_query_params(
    const BillingUsageHistoryQuery& q)
    -> std::vector<std::pair<std::string, std::string>> {
  std::vector<std::pair<std::string, std::string>> out;
  const auto add = [&out](std::string key, const std::optional<std::string>& value) {
    if (value && !value->empty()) out.emplace_back(std::move(key), *value);
  };
  add("currency", q.currency);
  add("cursor", q.cursor);
  add("endTimestamp", q.end_timestamp);
  if (q.page_size) out.emplace_back("pageSize", std::to_string(*q.page_size));
  add("startTimestamp", q.start_timestamp);
  for (const auto& [key, value] : q.extra) {
    if (key.empty() || value.empty()) continue;
    const bool taken = std::any_of(out.begin(), out.end(),
                                   [&key](const auto& p) { return p.first == key; });
    if (!taken) out.emplace_back(key, value);
  }
  return out;
}

enum class BillingUsageHistoryFormat { Json, Csv };

struct BillingUsageHistoryRequest {
  BillingUsageHistoryQuery query = {};
  BillingUsageHistoryFormat format = BillingUsageHistoryFormat::Json;
};

struct BillingInferenceDetails {
  std::optional<std::int64_t> completion_tokens{};
  std::optional<std::int64_t> inference_execution_time{};
  std::optional<std::int64_t> prompt_tokens{};
  std::optional<std::string> request_id{};
  nlohmann::json raw{};
};

struct BillingUsageHistoryEntry {
  std::optional<double> amount{};
  std::optional<std::string> currency{};
  std::optional<BillingInferenceDetails> inference_details{};
  std::optional<std::string> notes{};
  std::optional<double> price_per_unit_usd{};
  std::optional<std::string> sku{};
  std::optional<std::string> timestamp{};
  std::optional<double> units{};
  nlohmann::json raw{};
};

struct BillingUsageHistoryPage {
  std::vector<BillingUsageHistoryEntry> entries{};
  std::size_t returned{0};
  std::optional<std::string> next_cursor{};
  ResponseMetadata metadata{};
  nlohmann::json raw{};
};

struct BillingUsageHistoryCsv {
  std::string text{};
  std::string media_type{};
  std::optional<std::string> next_cursor{};
  std::optional<std::string> content_disposition{};
  ResponseMetadata metadata{};
};

using BillingUsageHistoryResult =
    std::variant<BillingUsageHistoryPage, BillingUsageHistoryCsv>;

[[nodiscard]] inline auto billing_usage_history_from_json_body(const nlohmann::json& j)
    -> BillingUsageHistoryPage {
  if (!j.is_object())
    throw std::runtime_error{"billing usage history: response must be an object"};
  const auto* values = detail::opt_array(j, "data");
  if (values == nullptr)
    throw std::runtime_error{"billing usage history: response has no data array"};

  const auto cursor = j.find("nextCursor");
  if (cursor == j.end() || (!cursor->is_null() && !cursor->is_string()))
    throw std::runtime_error{
        "billing usage history: nextCursor must be a string or null"};

  BillingUsageHistoryPage response;
  response.raw = j;
  response.returned = values->size();
  if (cursor->is_string()) response.next_cursor = cursor->get<std::string>();
  response.entries.reserve(values->size());
  for (const auto& value : *values) {
    BillingUsageHistoryEntry item;
    item.raw = value;
    if (value.is_object()) {
      item.amount = detail::opt_double(value, "amount");
      item.currency = detail::opt_string(value, "currency");
      item.notes = detail::opt_string(value, "notes");
      item.price_per_unit_usd = detail::opt_double(value, "pricePerUnitUsd");
      item.sku = detail::opt_string(value, "sku");
      item.timestamp = detail::opt_string(value, "timestamp");
      item.units = detail::opt_double(value, "units");
      if (const auto* details = detail::opt_object(value, "inferenceDetails")) {
        BillingInferenceDetails parsed;
        parsed.raw = *details;
        parsed.completion_tokens = detail::opt_i64(*details, "completionTokens");
        parsed.inference_execution_time =
            detail::opt_i64(*details, "inferenceExecutionTime");
        parsed.prompt_tokens = detail::opt_i64(*details, "promptTokens");
        parsed.request_id = detail::opt_string(*details, "requestId");
        item.inference_details = std::move(parsed);
      }
    }
    response.entries.push_back(std::move(item));
  }
  return response;
}

}  // namespace venice
