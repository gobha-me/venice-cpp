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
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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

}  // namespace venice
