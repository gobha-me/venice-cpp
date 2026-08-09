// The lossless assistant turn — VC-05 (#6) / VC-14 (#22). Offline, no key.
//
// Charter: everything about an assistant turn surviving the trip out of the
// server and back onto the wire — Message's two escape hatches and its
// assign-or-erase merge, ToolCall, the ChatResponse/Usage fields that keep the
// turn whole, and (later sections) the SSE framer and StreamAccumulator that
// rebuild the same Message from chunks. Request-body serialization for
// ChatRequest's own fields stays in test/02request/; precondition guards in
// test/03guards/; the model listing parse in test/04models/. Nothing here binds
// a socket — test/06transport/ owns the one case that must.
//
// ── The assertion this file exists for ────────────────────────────────────
//
// "A response can be sent back as the next turn" is NOT the discriminating
// test, and believing it was is how the first draft of this design shipped a
// silent bug. A merge that seeds serialization from the verbatim server object
// passes replay fidelity *trivially* — it echoes. It fails only on the inverse,
// which is the thing an agent loop actually does:
//
//     m.content.reset();  // redact before storing history
//     -> seed-from-raw resends the whole answer, silently
//
// So §1 is the negative and it comes before the happy path, which is also just
// the house rule. Measured before either was written, with both candidate rules
// implemented over the same redacted turn: seed-from-raw emitted the full
// answer and the already-executed tool call; assign-or-erase emitted
// {"role":"assistant"}. That probe is what the erase branch in Message::to_json
// is paying for.
//
// ── What was proven red, and against what ─────────────────────────────────
//
// Two claims here were verified against the pre-VC-05 headers rather than
// assumed, because a check never seen failing is not trusted:
//
//   * Message::from_json threw on {"role":"assistant","content":null,...} —
//     json.exception.type_error.302 — and on a message with content omitted,
//     out_of_range.403. The canonical tool-call reply was simply not parseable
//     into a Message, so §2 could not have been written at all before this
//     ticket.
//   * The old ChatResponse parse reached through choices[0].message for content
//     and never built a Message, so every §3 assertion below had no field to
//     read.
//
// VC-18's thought_signature cases (§2, §6, §9) could NOT be proven that way, and
// saying so is better than implying they were: the field does not exist on the
// pre-VC-18 header, so those cases do not compile against it — which is absence
// of evidence, not a red run. They were proven the other way instead, by
// installing eight deliberate breaks in the *new* header one at a time and
// recording which cases went red for each; the run is in the PR and the STATUS
// entry. Two of those breaks exist only to prove a case is load bearing:
// emitting the key unconditionally (caught by the two no-signature cases and
// nothing else), and the weaker of the two candidate merge guards (caught by
// the empty-first case and nothing else).
//
// Failure matrix first, happy path last.

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <venice/venice.hpp>

using venice::ChatRequest;
using venice::ChatResponse;
using venice::Message;
using venice::ToolCall;
using venice::Usage;

namespace {

// A reasoning + tool-call reply: the shape this whole ticket is about.
//
// Assembled rather than captured — no single reply carries reasoning_content,
// a tool call and both usage detail objects at once, so this is a composite. It
// is no longer a *guess*, though, which is what it was when written: every
// element of it has since been seen on the wire, the usage block by the VC-17
// sweep (see §4's provenance note) and the rest by VC-05's and VC-18's live
// runs. Message::raw and ChatResponse::raw remain what makes a wrong field name
// here recoverable rather than lossy.
constexpr auto kReplyBody = R"({
  "id":"chatcmpl-1","model":"deepseek-r1","object":"chat.completion","created":1700000000,
  "system_fingerprint":"fp_abc",
  "venice_parameters":{"web_search_citations":[]},
  "choices":[{"index":0,"finish_reason":"tool_calls","message":{
    "role":"assistant","content":null,"reasoning_content":"step one, step two",
    "refusal":null,
    "tool_calls":[{"id":"call_a","type":"function","index":0,
                   "function":{"name":"get_weather","arguments":"{\"location\":\"SF\"}"}}]}}],
  "usage":{"prompt_tokens":10,"completion_tokens":20,"total_tokens":30,
           "prompt_tokens_details":{"cached_tokens":4},
           "completion_tokens_details":{"reasoning_tokens":15}}})";

auto reply() -> ChatResponse {
  return ChatResponse::from_json_body(nlohmann::json::parse(kReplyBody));
}

// The assistant turn out of that body, which most cases start from.
auto turn() -> Message { return *reply().message; }

}  // namespace

// ── §0 the merge is total: every modeled field can be withheld ────────────
//
// The case that discriminates the two candidate designs. Under a merge that
// seeds from the verbatim `raw`, all four REQUIRE_FALSEs below go red while
// every other case in this file stays green — which is precisely why this one
// is first and why "the round trip works" was never sufficient evidence.
//
// The scenario is not contrived. It is the normal agent loop: the caller has
// executed call_a, appended a role:"tool" result, and is now clearing the
// fields that must not be re-issued. Replaying tool_calls here is a 400 for an
// unanswered tool_call, or an infinite loop.

TEST_CASE("a cleared field is absent from the wire, not resurrected", "[message][merge][failure]") {
  auto m = turn();
  REQUIRE(m.content);            // engaged (holding null) before we touch it
  REQUIRE(m.tool_calls);
  REQUIRE(m.reasoning_content);

  m.content.reset();
  m.tool_calls.reset();
  m.reasoning_content.reset();

  const nlohmann::json sent = m;
  INFO("serialized: " << sent.dump());
  REQUIRE_FALSE(sent.contains("content"));
  REQUIRE_FALSE(sent.contains("tool_calls"));
  REQUIRE_FALSE(sent.contains("reasoning_content"));
  REQUIRE(sent.at("role") == "assistant");
}

TEST_CASE("raw is never serialized", "[message][merge][failure]") {
  const auto m = turn();
  REQUIRE_FALSE(m.raw.is_null());  // it is populated...

  auto stripped = m;
  stripped.content.reset();
  stripped.reasoning_content.reset();
  stripped.tool_calls.reset();
  stripped.refusal.reset();

  const nlohmann::json sent = stripped;
  // ...and nothing of it reaches the wire but what the typed fields put there.
  REQUIRE(sent.size() == 1);
  REQUIRE(sent.at("role") == "assistant");
}

TEST_CASE("engaged-but-empty is distinct from disengaged", "[message][merge][failure]") {
  auto m = Message::assistant("hi");

  SECTION("disengaged tool_calls omits the key") {
    REQUIRE_FALSE(nlohmann::json(m).contains("tool_calls"));
  }
  SECTION("engaged empty tool_calls emits []") {
    m.tool_calls = std::vector<ToolCall>{};
    const nlohmann::json sent = m;
    REQUIRE(sent.at("tool_calls").is_array());
    REQUIRE(sent.at("tool_calls").empty());
  }
  SECTION("engaged null content emits null, not an omission") {
    m.content = nullptr;
    const nlohmann::json sent = m;
    REQUIRE(sent.contains("content"));
    REQUIRE(sent.at("content").is_null());
  }
  SECTION("engaged empty-string content emits \"\"") {
    m.content = "";
    const nlohmann::json sent = m;
    REQUIRE(sent.at("content") == "");
  }
}

// `extra` is the additive seed; `raw` is not. They are different contracts on
// purpose, and the reason Message carries two fields where every other type
// here carries one.
TEST_CASE("extra seeds the body and modeled fields win over it", "[message][merge][failure]") {
  auto m = Message::user("hi");
  m.extra = nlohmann::json::parse(R"({"role":"system","cache_control":{"type":"ephemeral"}})");

  const nlohmann::json sent = m;
  REQUIRE(sent.at("role") == "user");                              // modeled wins
  REQUIRE(sent.at("cache_control").at("type") == "ephemeral");     // unmodeled rides
}

// This pins a KNOWN LIMIT, not a desired behaviour (VC-19). `tool_calls` is a
// modeled key, so Message::to_json assigns over whatever the seed put there —
// which means the documented `m.extra = m.raw` escape hatch reaches
// message-level keys only and cannot rescue an unmodeled key *inside* a tool
// call. That is exactly how VC-18's thought_signature was lost, and why it had
// to be modeled rather than left to a hatch. The day ToolCall grows its own
// passthrough this case goes red, and inverting it deliberately is the
// acceptance criterion for that ticket.
TEST_CASE("extra cannot reach a key inside tool_calls, and that is a stated limit",
          "[message][merge][failure]") {
  auto m = turn();
  m.raw["tool_calls"][0]["unmodeled_key"] = "carried by the server";
  m.extra = m.raw;  // the full verbatim-replay hatch, deliberately engaged

  const nlohmann::json sent = m;
  REQUIRE(sent.at("tool_calls").at(0).at("id") == "call_a");
  REQUIRE_FALSE(sent.at("tool_calls").at(0).contains("unmodeled_key"));
}

TEST_CASE("a non-object extra degrades to no seed rather than throwing", "[message][merge][failure]") {
  auto m = Message::user("hi");
  SECTION("array") { m.extra = nlohmann::json::array({1, 2}); }
  SECTION("number") { m.extra = 7; }
  SECTION("null") { m.extra = nullptr; }

  nlohmann::json sent;
  REQUIRE_NOTHROW(sent = nlohmann::json(m));
  REQUIRE(sent.at("role") == "user");
  REQUIRE(sent.at("content") == "hi");
  REQUIRE(sent.size() == 2);
}

// ── §1 from_json is total ─────────────────────────────────────────────────
//
// Every shape below threw before this ticket, which is why the tool-call reply
// could not be parsed into a Message at all. Totality here is deliberately not
// extended to ChatResponse::from_json_body — see §3.

TEST_CASE("Message::from_json never throws", "[message][parse][failure]") {
  const auto shapes = std::vector<std::string>{
      R"({"role":"assistant","content":null})",
      R"({"role":"assistant"})",
      R"({"role":"assistant","content":123})",
      R"({"role":"assistant","content":{"unexpected":"object"}})",
      R"({"content":"no role at all"})",
      R"({})",
      R"({"role":"assistant","tool_calls":"not an array"})",
      R"({"role":"assistant","tool_calls":[1,2,3]})",
      R"({"role":"assistant","reasoning_content":42})",
  };
  for (const auto& s : shapes) {
    INFO("shape: " << s);
    REQUIRE_NOTHROW(nlohmann::json::parse(s).get<Message>());
  }
}

TEST_CASE("absent content and null content stay distinguishable", "[message][parse][failure]") {
  const auto absent = nlohmann::json::parse(R"({"role":"assistant"})").get<Message>();
  const auto null_c = nlohmann::json::parse(R"({"role":"assistant","content":null})").get<Message>();

  REQUIRE_FALSE(absent.content.has_value());
  REQUIRE(null_c.content.has_value());
  REQUIRE(null_c.content->is_null());

  // and they serialize back differently, which is the point of the distinction
  REQUIRE_FALSE(nlohmann::json(absent).contains("content"));
  REQUIRE(nlohmann::json(null_c).at("content").is_null());
}

TEST_CASE("text() flattens whatever content turned out to be", "[message][parse]") {
  auto of = [](const char* body) { return nlohmann::json::parse(body).get<Message>().text(); };

  REQUIRE(of(R"({"content":"plain"})") == "plain");
  REQUIRE(of(R"({"content":null})").empty());
  REQUIRE(of(R"({})").empty());
  REQUIRE(of(R"({"content":123})").empty());
  REQUIRE(of(R"({"content":[{"type":"text","text":"a"},{"type":"text","text":"b"}]})") == "ab");
  // a part with no text field contributes nothing rather than throwing
  REQUIRE(of(R"({"content":[{"type":"image_url","image_url":{"url":"x"}},{"text":"z"}]})") == "z");
}

// A multimodal message is expressible in both directions with no extra field,
// because content is json rather than string. Worth pinning: it is the reason
// the type is optional<json> and not optional<string>.
TEST_CASE("a multimodal parts array round-trips", "[message][parse]") {
  constexpr auto kParts =
      R"({"role":"user","content":[{"type":"text","text":"describe"},)"
      R"({"type":"image_url","image_url":{"url":"data:image/png;base64,AAA"}}]})";

  const auto m = nlohmann::json::parse(kParts).get<Message>();
  REQUIRE(m.content->is_array());
  REQUIRE(nlohmann::json(m) == nlohmann::json::parse(kParts));
}

// ── §2 ToolCall ───────────────────────────────────────────────────────────

TEST_CASE("ToolCall::from_json tolerates every fragment shape", "[toolcall][parse][failure]") {
  const auto shapes = std::vector<std::string>{
      R"({})",
      R"({"index":0})",
      R"({"index":0,"function":{"arguments":"{\"lo"}})",   // a continuation
      R"({"function":"not an object"})",
      R"({"id":42})",
      R"({"thought_signature":42})",
      R"([])",
  };
  for (const auto& s : shapes) {
    INFO("shape: " << s);
    REQUIRE_NOTHROW(nlohmann::json::parse(s).get<ToolCall>());
  }
}

TEST_CASE("arguments are verbatim and never parsed for us", "[toolcall][parse][failure]") {
  const auto t = turn().tool_calls->at(0);
  REQUIRE(t.arguments == R"({"location":"SF"})");

  SECTION("a well-formed argument string parses on request") {
    const auto args = t.parsed_arguments();
    REQUIRE(args.has_value());
    REQUIRE(args->at("location") == "SF");
  }
  SECTION("a malformed one comes back as an error, not a throw") {
    ToolCall bad;
    bad.arguments = R"({"loc)";  // a fragment, or a model that emitted junk
    const auto args = bad.parsed_arguments();
    REQUIRE_FALSE(args.has_value());
    REQUIRE(args.error().kind == venice::ErrorKind::Parse);
    REQUIRE(args.error().body == R"({"loc)");
  }
}

// `index` is a streaming join key, not a property of the call. Replaying it on
// a request would assert an ordering the caller never chose.
TEST_CASE("index is parsed but never serialized", "[toolcall][merge]") {
  const auto t = turn().tool_calls->at(0);
  REQUIRE(t.index == 0);
  REQUIRE_FALSE(nlohmann::json(t).contains("index"));
}

TEST_CASE("a ToolCall with no type defaults to function on the wire", "[toolcall][merge]") {
  ToolCall t;
  t.id = "call_x";
  t.name = "f";
  t.arguments = "{}";
  const nlohmann::json j = t;
  REQUIRE(j.at("type") == "function");
  REQUIRE(j.at("function").at("name") == "f");
  REQUIRE(j.at("function").at("arguments") == "{}");
}

// VC-18 (#29). A Gemini-family model attaches an opaque signature to the
// function-call part and 400s the next turn if it does not come back. The four
// cases below are ordered non-regression first: the field is new, and the
// expensive way to get it wrong is not "the signature is missing" — that fails
// loudly against one family — but "every other family's body moved", which
// fails against all of them and was green the whole time.
//
// A realistic value, because base64 contains '+', '/' and '=' and a naive
// encoder somewhere downstream would only show up on one that does.
namespace {
constexpr auto kSig =
    "AY89a19vUCxARrnsLLo2whPeNwfnUiESldLD2jBxsz/vhLb3XZoh8cVySH2tKTuxVHaLC5at+w==";
}  // namespace

TEST_CASE("a call with no signature emits no thought_signature key at all",
          "[toolcall][merge][failure]") {
  // Byte-exact rather than a contains() sweep, on test/02request's reasoning:
  // a key-by-key absence check passes happily when some *other* key is
  // accidentally emitted. This string is the pre-VC-18 body, unchanged.
  constexpr auto kUnmoved =
      R"({"function":{"arguments":"{}","name":"f"},"id":"call_x","type":"function"})";

  SECTION("hand-built") {
    ToolCall t;
    t.id = "call_x";
    t.name = "f";
    t.arguments = "{}";
    REQUIRE_FALSE(t.thought_signature.has_value());
    REQUIRE(nlohmann::json(t).dump() == kUnmoved);
  }
  SECTION("parsed from a signature-less wire object") {
    const auto t = nlohmann::json::parse(kUnmoved).get<ToolCall>();
    REQUIRE_FALSE(t.thought_signature.has_value());
    REQUIRE(nlohmann::json(t).dump() == kUnmoved);
  }
}

TEST_CASE("an opaque signature is a sibling of function and survives verbatim",
          "[toolcall][parse][failure]") {
  const auto wire = nlohmann::json::parse(
      std::string{R"({"id":"call_0","type":"function","thought_signature":")"} + kSig +
      R"(","function":{"name":"get_weather","arguments":"{\"city\":\"SF\"}"}})");

  const auto t = wire.get<ToolCall>();
  REQUIRE(t.thought_signature == kSig);

  const nlohmann::json sent = t;
  REQUIRE(sent.at("thought_signature") == kSig);
  // The placement assertion, and the reason this case exists at all: nesting it
  // inside `function` parses and serializes and reads fine, and still 400s.
  REQUIRE_FALSE(sent.at("function").contains("thought_signature"));
  // Nothing else moved, and no extra key rode along.
  REQUIRE(sent == wire);
}

TEST_CASE("a wrong-typed signature reads as absent rather than throwing",
          "[toolcall][parse][failure]") {
  // Absent is the honest outcome: it produces the server's 400 rather than
  // putting a confident wrong value on the wire.
  const auto shapes = std::vector<std::string>{
      R"({"id":"c","thought_signature":42})",   R"({"id":"c","thought_signature":null})",
      R"({"id":"c","thought_signature":{"a":1}})", R"({"id":"c","thought_signature":[]})",
      R"({"id":"c","thought_signature":true})",
  };
  for (const auto& s : shapes) {
    INFO("shape: " << s);
    ToolCall t;
    REQUIRE_NOTHROW(t = nlohmann::json::parse(s).get<ToolCall>());
    REQUIRE_FALSE(t.thought_signature.has_value());
    REQUIRE_FALSE(nlohmann::json(t).contains("thought_signature"));
  }
}

TEST_CASE("an engaged empty signature is distinct from a disengaged one", "[toolcall][merge]") {
  // Same rule as content and tool_calls in §0: engaged-but-empty is a value the
  // caller chose. The policy about what an empty signature *means* lives in one
  // place only, the accumulator's merge — not here.
  ToolCall t;
  t.id = "call_x";
  t.name = "f";
  t.arguments = "{}";

  SECTION("disengaged omits the key") {
    REQUIRE_FALSE(nlohmann::json(t).contains("thought_signature"));
  }
  SECTION("engaged empty emits \"\"") {
    t.thought_signature = "";
    const nlohmann::json sent = t;
    REQUIRE(sent.contains("thought_signature"));
    REQUIRE(sent.at("thought_signature") == "");
  }
}

// ── §3 ChatResponse keeps the turn, and stays loud about structure ────────

TEST_CASE("ChatResponse::from_json_body stays loud on a broken shape", "[response][parse][failure]") {
  // Unchanged from before this ticket, and deliberately so: Message became
  // total, ChatResponse did not. A body with no choices is not a shape
  // variation, it is a broken completion, and Client::chat turns this throw
  // into ErrorKind::Parse.
  REQUIRE_THROWS(ChatResponse::from_json_body(nlohmann::json::parse(R"({"id":"x"})")));
}

TEST_CASE("empty choices yields no message rather than a roleless one", "[response][parse][failure]") {
  const auto r = ChatResponse::from_json_body(nlohmann::json::parse(R"({"id":"x","choices":[]})"));
  REQUIRE_FALSE(r.message.has_value());
  REQUIRE(r.content.empty());
  // The reason it is optional: a default-constructed Message has role == "",
  // and appending that to a history sends a message no server will accept.
}

TEST_CASE("the content snapshot agrees with the message it came from", "[response][parse]") {
  const auto r = reply();
  REQUIRE(r.content == r.message->text());

  const auto plain = ChatResponse::from_json_body(nlohmann::json::parse(
      R"({"choices":[{"message":{"role":"assistant","content":"hi"}}]})"));
  REQUIRE(plain.content == "hi");
  REQUIRE(plain.content == plain.message->text());
}

TEST_CASE("raw holds the whole body, including what nothing models", "[response][parse]") {
  const auto body = nlohmann::json::parse(kReplyBody);
  const auto r = ChatResponse::from_json_body(body);
  REQUIRE(r.raw == body);
  // choices[1..n] and logprobs are deliberately untyped; this is what makes
  // that deferral honest rather than lossy.
  REQUIRE(r.raw.at("choices").at(0).at("index") == 0);
}

TEST_CASE("the fields the old parse discarded are all present", "[response][parse]") {
  const auto r = reply();
  REQUIRE(r.created == 1700000000);
  REQUIRE(r.system_fingerprint == "fp_abc");
  REQUIRE(r.venice_parameters.has_value());
  REQUIRE(r.message->role == "assistant");
  REQUIRE(r.message->reasoning_content == "step one, step two");
  REQUIRE(r.message->content->is_null());
  REQUIRE(r.message->tool_calls->size() == 1);
}

// ── §4 Usage ──────────────────────────────────────────────────────────────
//
// PROVENANCE. VC-17 (#28) was filed because this section asserted nestings no
// capture had shown. It has one now: 21 captures on 2026-08-09, seven models ×
// {non-streaming, streaming, streaming+stream_options.include_usage}, via
// `venice-cpp --usage <id>`. What it settled:
//
//   prompt_tokens_details.cached_tokens         5 of 7 — glm-4.7, glm-5,
//                                                 llama-3.3-70b, deepseek-v4-pro,
//                                                 grok-4-5
//   completion_tokens_details.reasoning_tokens  2 of 7 — deepseek-v4-pro,
//                                                 grok-4-5
//   neither                                     gemini-3-6-flash,
//                                                 qwen3-235b-a22b-thinking-2507
//
// So #28's premise was wrong — both nestings are exactly where this parse looks
// — and the reason it was wrong is worth more than the correction. The run
// behind it used the one leg that auto-picked a model without naming the
// alternates, and gemini-3-6-flash is both the first supportsReasoning entry in
// the catalogue and one of the two families that report neither field. A shape
// that varies by family cannot be settled by a leg that only ever runs one.
//
// The cases below are the observed shapes, pinned verbatim. The cases after
// them pin the parse *rule*, and each now says whether its shape has ever been
// on a wire — because "the fixture agrees with the docs" is exactly the state
// this ticket exists to end.

// Captured from `--usage gemini-3-6-flash`, non-streaming, 2026-08-09. The
// streamed frame carried the same three keys.
constexpr auto kLiveUsageBare = R"({"prompt_tokens":1794,"completion_tokens":607,
                                    "total_tokens":2401})";

// Captured from `--usage deepseek-v4-pro`, non-streaming, 2026-08-09.
constexpr auto kLiveUsageFull = R"({"prompt_tokens":1752,"completion_tokens":365,
                                    "total_tokens":2117,
                                    "cache_read_input_tokens":1024,
                                    "prompt_tokens_details":{"cached_tokens":1024},
                                    "completion_tokens_details":{"reasoning_tokens":92}})";

TEST_CASE("the observed live shapes parse as measured", "[usage][parse][live]") {
  auto usage_of = [](const char* body) { return nlohmann::json::parse(body).get<Usage>(); };

  SECTION("three flat counts and nothing else leaves both optionals disengaged") {
    const auto u = usage_of(kLiveUsageBare);
    REQUIRE(u.prompt_tokens == 1794);
    REQUIRE(u.completion_tokens == 607);
    REQUIRE(u.total_tokens == 2401);
    // Not zero. This is the shape #28 saw and read as a parse failure; absent
    // is the correct answer to a question this family does not answer.
    REQUIRE_FALSE(u.cached_tokens.has_value());
    REQUIRE_FALSE(u.reasoning_tokens.has_value());
  }

  SECTION("both detail objects are read when the family sends them") {
    const auto u = usage_of(kLiveUsageFull);
    REQUIRE(u.cached_tokens == 1024);
    REQUIRE(u.reasoning_tokens == 92);
    REQUIRE(u.completion_tokens == 365);
    // cache_read_input_tokens sits right there beside them and is deliberately
    // not modeled: it mirrored the nested value in all 21 captures, so typing
    // it would add a second spelling of one fact. It stays reachable through
    // ChatResponse::raw, which is what makes that omission honest.
  }

  SECTION("an explicit zero is engaged, not absent") {
    // deepseek-v4-pro and llama-3.3-70b both reported "cached_tokens":0 on a
    // cold cache. Collapsing that to nullopt would erase the difference between
    // "nothing was cached" and "this family does not say", which is the whole
    // reason these fields are optional rather than defaulted to 0.
    const auto u = usage_of(R"({"prompt_tokens":1701,"completion_tokens":78,
                                "total_tokens":1779,
                                "prompt_tokens_details":{"cached_tokens":0}})");
    REQUIRE(u.cached_tokens.has_value());
    REQUIRE(*u.cached_tokens == 0);
  }
}

TEST_CASE("cached_tokens is read from both locations", "[usage][parse][failure]") {
  auto usage_of = [](const char* body) { return nlohmann::json::parse(body).get<Usage>(); };

  SECTION("the flat key — UNOBSERVED, and kept anyway") {
    // Venice has never been seen sending a flat `cached_tokens`. What it does
    // send flat, alongside the nested one and never instead of it, is
    // `cache_read_input_tokens` — a different key. So this read is the
    // compatibility shim it was always described as, and this is the only case
    // in §4 pinning a shape with no wire behind it. Kept rather than deleted:
    // the read is harmless, removing it changes a public parse's behaviour to
    // buy nothing, and a gateway in front of Venice is exactly the thing that
    // would flatten a details object.
    REQUIRE(usage_of(R"({"cached_tokens":80})").cached_tokens == 80);
  }
  SECTION("the nested OpenAI-canonical location — OBSERVED on 5 of 7 families") {
    REQUIRE(usage_of(R"({"prompt_tokens_details":{"cached_tokens":4}})").cached_tokens == 4);
  }
  SECTION("nested wins when both are present and disagree — CONSTRUCTED") {
    // No capture disagrees, because no capture carries both. This pins the
    // ordering so a disagreement, if one ever arrives, resolves the documented
    // way rather than by statement order in from_json.
    REQUIRE(usage_of(R"({"cached_tokens":80,"prompt_tokens_details":{"cached_tokens":4}})")
                .cached_tokens == 4);
  }
  SECTION("neither leaves it unset — OBSERVED on gemini and qwen") {
    REQUIRE_FALSE(usage_of(R"({"prompt_tokens":1})").cached_tokens.has_value());
  }
}

TEST_CASE("a missing or null details object is structural, not corruption",
          "[usage][parse][failure]") {
  // Shape variation between gateways must not cost the caller the completion.
  const auto shapes = std::vector<std::string>{
      R"({"prompt_tokens":1,"prompt_tokens_details":null})",
      R"({"prompt_tokens":1,"completion_tokens_details":null})",
      R"({"prompt_tokens":1,"prompt_tokens_details":[]})",
      R"({"prompt_tokens":1,"completion_tokens_details":{}})",
  };
  for (const auto& s : shapes) {
    INFO("shape: " << s);
    REQUIRE_NOTHROW(nlohmann::json::parse(s).get<Usage>());
  }
}

TEST_CASE("a wrong-typed token count stays loud", "[usage][parse][failure]") {
  // The other half of the rule above: a value that is corruption must fail as
  // ErrorKind::Parse rather than silently becoming 0 and hiding a billing bug.
  REQUIRE_THROWS(nlohmann::json::parse(R"({"prompt_tokens":"many"})").get<Usage>());
  REQUIRE_THROWS(
      nlohmann::json::parse(R"({"completion_tokens_details":{"reasoning_tokens":"lots"}})")
          .get<Usage>());
}

TEST_CASE("reasoning_tokens is reported", "[usage][parse]") {
  // Reads the composite reply rather than a bare usage object, so it covers the
  // path through ChatResponse::from_json_body too. The nesting is the one
  // deepseek-v4-pro and grok-4-5 were measured sending.
  const auto r = reply();
  REQUIRE(r.usage->reasoning_tokens == 15);
  REQUIRE(r.usage->cached_tokens == 4);
  REQUIRE(r.usage->completion_tokens == 20);
}

// ── §5 idempotency ────────────────────────────────────────────────────────
//
// Nothing else catches a merge that is stable on the first hop and drifts on
// the second.

TEST_CASE("to_json is stable across a re-parse", "[message][merge]") {
  const auto once = nlohmann::json(turn());
  const auto again = nlohmann::json(once.get<Message>());
  REQUIRE(again == once);

  const auto third = nlohmann::json(again.get<Message>());
  REQUIRE(third == once);
}

TEST_CASE("raw is the object that was parsed, at every hop", "[message][merge]") {
  const auto once = nlohmann::json(turn());
  const auto m2 = once.get<Message>();
  REQUIRE(m2.raw == once);
}

// ── §6 replay fidelity, the happy path, last ──────────────────────────────
//
// The whole point, end to end: a reply becomes the next turn's message and
// nothing about it changes on the way.

TEST_CASE("an assistant turn replays into the next request intact", "[message][replay]") {
  const auto r = reply();

  ChatRequest next;
  next.model = "deepseek-r1";
  next.messages = {Message::user("what is the weather in SF?"), *r.message,
                   Message::tool("call_a", R"({"temp_f":68})")};

  const auto sent = next.to_json_body(/*stream=*/false).at("messages");
  REQUIRE(sent.size() == 3);

  const auto& assistant = sent.at(1);
  REQUIRE(assistant.at("role") == "assistant");
  REQUIRE(assistant.at("content").is_null());
  REQUIRE(assistant.at("reasoning_content") == "step one, step two");
  REQUIRE(assistant.at("tool_calls").size() == 1);
  REQUIRE(assistant.at("tool_calls").at(0).at("id") == "call_a");
  REQUIRE(assistant.at("tool_calls").at(0).at("function").at("name") == "get_weather");
  REQUIRE(assistant.at("tool_calls").at(0).at("function").at("arguments") ==
          R"({"location":"SF"})");

  const auto& tool = sent.at(2);
  REQUIRE(tool.at("role") == "tool");
  REQUIRE(tool.at("tool_call_id") == "call_a");
  REQUIRE(tool.at("content") == R"({"temp_f":68})");
}

// VC-18's acceptance criterion, in the form the live 400 took. Message::to_json
// needed no change for this to work — it assigns j["tool_calls"] and each
// element serializes itself — and that is precisely the claim worth pinning:
// this case goes red if anyone ever "optimises" that assignment into something
// that reconstructs the array by hand.
TEST_CASE("a replayed turn carries the tool-call signature back", "[message][replay][failure]") {
  const auto body = nlohmann::json::parse(
      std::string{R"({"choices":[{"message":{"role":"assistant","content":null,"tool_calls":[)"} +
      R"({"id":"call_0","type":"function","thought_signature":")" + kSig +
      R"(","function":{"name":"get_weather","arguments":"{\"city\":\"SF\"}"}}]}}]})");

  ChatRequest next;
  next.model = "gemini-3-6-flash";
  next.messages = {Message::user("what is the weather in SF?"),
                   *ChatResponse::from_json_body(body).message,
                   Message::tool("call_0", R"({"temp_f":68})")};

  // By value, not by reference: to_json_body returns a temporary, and chaining
  // at() off it dangles. The neighbouring case above does the same, and GCC's
  // -Wdangling-reference catches it — this one was written wrong first.
  const auto replayed = next.to_json_body(/*stream=*/false).at("messages").at(1);
  REQUIRE(replayed.at("tool_calls").at(0).at("thought_signature") == kSig);
  REQUIRE_FALSE(replayed.at("tool_calls").at(0).at("function").contains("thought_signature"));
}

TEST_CASE("thinking can be refed or withheld, per turn", "[message][replay]") {
  // The use case that filed this ticket in this shape: refeeding is a decision
  // the caller makes each turn, so both branches have to be expressible.
  auto m = turn();

  m.reasoning_content = std::nullopt;
  REQUIRE_FALSE(nlohmann::json(m).contains("reasoning_content"));

  m.reasoning_content = "step one, step two";
  REQUIRE(nlohmann::json(m).at("reasoning_content") == "step one, step two");
}

// ── §7 SSE framing ────────────────────────────────────────────────────────
//
// Unreachable offline until VC-05 — the framing lived inside a content_receiver
// lambda, so this ticket's own acceptance criteria (partial frames across chunk
// boundaries, [DONE]) could only ever have been checked through a socket. Three
// of the cases below were measured red against a verbatim copy of the old loop
// before the framer was written; two of those were total silence rather than a
// wrong answer, which is the kind of bug a green suite hides best.

namespace {

// Feed a chunk sequence and collect the payloads that come out, in order.
auto framed(const std::vector<std::string>& chunks, bool finish = true)
    -> std::vector<std::string> {
  venice::detail::SseFramer f;
  std::vector<std::string> seen;
  const auto sink = [&](std::string_view p) { seen.emplace_back(p); };
  for (const auto& c : chunks) f.feed(c, sink);
  if (finish) f.finish(sink);
  return seen;
}

constexpr auto kFrame = R"(data: {"choices":[{"delta":{"content":"hi"}}]})";

}  // namespace

TEST_CASE("CRLF frames are delivered", "[stream][framer][failure]") {
  // Measured against the pre-VC-05 loop: LF gave 1 payload, CRLF gave 0. The
  // old split was on "\n\n", and "\r\n\r\n" contains no such substring, so a
  // spec-legal peer produced an entirely empty stream with no error anywhere.
  // finish=false is load bearing, and the first spelling of this case got it
  // wrong. With the flush enabled, a CRLF frame that never dispatched from the
  // framing loop still came out of finish() at end of stream, so the assertion
  // stayed green with the CRLF branch deleted — it was measuring the flush, not
  // the framing. Verified by deleting that branch and watching this case pass.
  //
  // Streaming is the whole point: a frame must dispatch when it arrives, not
  // when the body ends. Feeding two frames with no flush is what pins that.
  const auto lf = framed({std::string{kFrame} + "\n\n" + kFrame + "\n\n"}, /*finish=*/false);
  const auto crlf = framed({std::string{kFrame} + "\r\n\r\n" + kFrame + "\r\n\r\n"},
                           /*finish=*/false);

  REQUIRE(lf.size() == 2);
  REQUIRE(crlf.size() == 2);
  REQUIRE(crlf.at(0) == lf.at(0));
  // and the payload is parseable — a trailing \r would break json::parse
  REQUIRE_NOTHROW(nlohmann::json::parse(crlf.at(0)).dump());
}

TEST_CASE("a mixed-ending stream still frames", "[stream][framer][failure]") {
  const auto seen = framed({std::string{kFrame} + "\r\n\r\n" + kFrame + "\n\n"});
  REQUIRE(seen.size() == 2);
  REQUIRE(seen.at(0) == seen.at(1));
}

TEST_CASE("the final unterminated frame is not dropped", "[stream][framer][failure]") {
  // Measured against the old loop: 1 of 2 payloads delivered. `leftover` was a
  // local of the receiver lambda and was discarded when send() returned. The
  // lost frame is frequently the usage frame, so this was a billing bug.
  const auto seen = framed({std::string{kFrame} + "\n\n" + kFrame});
  REQUIRE(seen.size() == 2);

  SECTION("without finish() it is still pending, which is what the old code did") {
    const auto without = framed({std::string{kFrame} + "\n\n" + kFrame}, /*finish=*/false);
    REQUIRE(without.size() == 1);
  }
}

TEST_CASE("a frame split across chunk boundaries is reassembled", "[stream][framer][failure]") {
  const std::string whole = std::string{kFrame} + "\n\n";

  SECTION("split at every single byte offset") {
    for (size_t i = 1; i < whole.size(); ++i) {
      INFO("split at " << i);
      const auto seen = framed({whole.substr(0, i), whole.substr(i)});
      REQUIRE(seen.size() == 1);
      REQUIRE_NOTHROW(nlohmann::json::parse(seen.at(0)).dump());
    }
  }
  SECTION("one byte at a time") {
    std::vector<std::string> bytes;
    for (const char c : whole) bytes.emplace_back(1, c);
    const auto seen = framed(bytes);
    REQUIRE(seen.size() == 1);
  }
}

TEST_CASE("lines that are not data: are ignored", "[stream][framer][failure]") {
  SECTION("an event with no data line yields nothing") {
    REQUIRE(framed({": keep-alive comment\n\n"}).empty());
    REQUIRE(framed({"event: message\nid: 7\n\n"}).empty());
  }
  SECTION("a data line with no space keeps its whole payload") {
    REQUIRE(framed({"data:{\"a\":1}\n\n"}).at(0) == R"({"a":1})");
  }
  SECTION("only one leading space is stripped, the rest is payload") {
    REQUIRE(framed({"data:   x\n\n"}).at(0) == "  x");
  }
  SECTION("multiple data lines in one event each dispatch") {
    const auto seen = framed({"data: one\ndata: two\n\n"});
    REQUIRE(seen.size() == 2);
    REQUIRE(seen.at(0) == "one");
    REQUIRE(seen.at(1) == "two");
  }
  SECTION("an empty stream yields nothing and does not fault") {
    REQUIRE(framed({}).empty());
    REQUIRE(framed({""}).empty());
  }
}

TEST_CASE("the buffer is bounded", "[stream][framer][failure]") {
  // A peer that never sends a blank line used to buffer its entire response.
  venice::detail::SseFramer f;
  const std::string big(venice::detail::SseFramer::kMaxEvent + 1, 'x');
  const auto ok = f.feed(big, [](std::string_view) {});
  REQUIRE_FALSE(ok);
  REQUIRE(f.overflowed());
}

// ── §8 delta_from_chunk ───────────────────────────────────────────────────

namespace {

auto delta_of(const char* body, std::vector<ToolCall>& frags) -> venice::StreamDelta {
  static nlohmann::json held;
  held = nlohmann::json::parse(body);
  return venice::delta_from_chunk(held, frags);
}

}  // namespace

TEST_CASE("delta_from_chunk never throws", "[stream][delta][failure]") {
  const auto shapes = std::vector<std::string>{
      R"({})",
      R"([])",
      R"(null)",
      R"({"choices":[]})",
      R"({"choices":"not an array"})",
      R"({"choices":[{}]})",
      R"({"choices":[{"delta":"not an object"}]})",
      R"({"choices":[{"delta":{"content":null}}]})",
      R"({"choices":[{"delta":{"content":123}}]})",
      R"({"choices":[{"finish_reason":null}]})",
      R"({"usage":null})",
  };
  for (const auto& s : shapes) {
    INFO("shape: " << s);
    std::vector<ToolCall> frags;
    REQUIRE_NOTHROW(delta_of(s.c_str(), frags));
  }
}

TEST_CASE("an absent content key differs from an empty one", "[stream][delta][failure]") {
  // This is the guarantee the string_view adapter rests on: on_token fires when
  // a chunk carried a content string, including "". A plain string_view could
  // not tell those apart and an empty-content frame would stop reaching
  // existing callers.
  std::vector<ToolCall> frags;

  REQUIRE_FALSE(delta_of(R"({"choices":[{"delta":{"role":"assistant"}}]})", frags).content);
  REQUIRE_FALSE(delta_of(R"({"choices":[{"delta":{"content":null}}]})", frags).content);

  const auto empty = delta_of(R"({"choices":[{"delta":{"content":""}}]})", frags);
  REQUIRE(empty.content.has_value());
  REQUIRE(empty.content->empty());
}

TEST_CASE("a usage-only frame with empty choices is still read", "[stream][delta][failure]") {
  // Venice sends the usage frame after finish_reason, with "choices": [].
  std::vector<ToolCall> frags;
  const auto d = delta_of(R"({"choices":[],"usage":{"prompt_tokens":3}})", frags);
  REQUIRE(d.usage != nullptr);
  REQUIRE_FALSE(d.empty());
}

// ── §9 the accumulator ────────────────────────────────────────────────────

namespace {

// Drive an accumulator over a list of chunk bodies.
auto accumulate(const std::vector<std::string>& chunks) -> venice::StreamAccumulator {
  venice::StreamAccumulator acc;
  for (const auto& c : chunks) {
    const auto j = nlohmann::json::parse(c);
    acc.note_envelope(j);
    acc.ingest(j);
  }
  return acc;
}

}  // namespace

// The static_assert the overload set depends on. A call operator on
// StreamAccumulator would make it convertible to std::function and silently
// reintroduce the ambiguity chat_stream is shaped to avoid.
static_assert(!std::is_convertible_v<venice::StreamAccumulator&,
                                     std::function<bool(std::string_view)>>,
              "StreamAccumulator must not be callable — see Client::chat_stream");
static_assert(!std::is_convertible_v<venice::StreamAccumulator&,
                                     std::function<bool(const venice::StreamDelta&)>>,
              "StreamAccumulator must not be callable — see Client::chat_stream");

TEST_CASE("tool-call fragments merge by index, not by position",
          "[stream][accumulator][failure]") {
  // The canonical bug: a chunk carrying only the second call sends a
  // one-element array with "index": 1. Merging by array position concatenates
  // two calls' arguments into one and looks entirely plausible.
  const auto acc = accumulate({
      R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_a","type":"function",
           "function":{"name":"f","arguments":"{\"x\":"}}]}}]})",
      R"({"choices":[{"delta":{"tool_calls":[{"index":1,"id":"call_b","type":"function",
           "function":{"name":"g","arguments":"{\"y\":"}}]}}]})",
      R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"1}"}}]}}]})",
      R"({"choices":[{"delta":{"tool_calls":[{"index":1,"function":{"arguments":"2}"}}]}}]})",
  });

  const auto calls = *acc.message().tool_calls;
  REQUIRE(calls.size() == 2);
  REQUIRE(calls.at(0).id == "call_a");
  REQUIRE(calls.at(0).name == "f");
  REQUIRE(calls.at(0).arguments == R"({"x":1})");
  REQUIRE(calls.at(1).id == "call_b");
  REQUIRE(calls.at(1).name == "g");
  REQUIRE(calls.at(1).arguments == R"({"y":2})");
}

TEST_CASE("out-of-order indices emit in index order", "[stream][accumulator][failure]") {
  const auto acc = accumulate({
      R"({"choices":[{"delta":{"tool_calls":[{"index":2,"id":"c","function":{"name":"third"}}]}}]})",
      R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"a","function":{"name":"first"}}]}}]})",
      R"({"choices":[{"delta":{"tool_calls":[{"index":1,"id":"b","function":{"name":"second"}}]}}]})",
  });
  const auto calls = *acc.message().tool_calls;
  REQUIRE(calls.size() == 3);
  REQUIRE(calls.at(0).name == "first");
  REQUIRE(calls.at(1).name == "second");
  REQUIRE(calls.at(2).name == "third");
}

TEST_CASE("a gap in the indices is not filled in", "[stream][accumulator][failure]") {
  // vector[index] would either fault or invent an empty call at 1.
  const auto acc = accumulate({
      R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"a"}]}}]})",
      R"({"choices":[{"delta":{"tool_calls":[{"index":7,"id":"h"}]}}]})",
  });
  const auto calls = *acc.message().tool_calls;
  REQUIRE(calls.size() == 2);
  REQUIRE(calls.at(0).id == "a");
  REQUIRE(calls.at(1).id == "h");
}

TEST_CASE("a continuation does not clobber the scalars", "[stream][accumulator][failure]") {
  // Some gateways send "name": "" on continuation fragments. An unguarded
  // assignment loses the id and the name on fragment two.
  const auto acc = accumulate({
      R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_a","type":"function",
           "function":{"name":"get_weather","arguments":"{"}}]}}]})",
      R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"","type":"",
           "function":{"name":"","arguments":"}"}}]}}]})",
  });
  const auto m = acc.message();  // by value: a reference into the temporary would dangle
  const auto& call = m.tool_calls->at(0);
  REQUIRE(call.id == "call_a");
  REQUIRE(call.type == "function");
  REQUIRE(call.name == "get_weather");
  REQUIRE(call.arguments == "{}");
}

// VC-18 across the streaming path. Every case asserts the merged struct *and*
// the emitted JSON: without the pair, deleting the to_json line is caught in §2
// alone and all of these stay green.
TEST_CASE("a signature arriving on the second fragment survives the merge",
          "[stream][accumulator][failure]") {
  // Measured 2026-08-09: gemini-3-6-flash sends the whole call in one fragment,
  // gemini-3-5-flash splits the same call across two. So the signature is not
  // tied to the opening fragment, and any design that reads it off slot.raw —
  // which holds fragment one only — fails exactly here.
  const auto acc = accumulate({
      R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_0","type":"function",
           "function":{"name":"get_weather","arguments":""}}]}}]})",
      std::string{R"({"choices":[{"delta":{"tool_calls":[{"index":0,"thought_signature":")"} +
          kSig + R"(","function":{"arguments":"{\"city\":\"SF\"}"}}]}}]})",
  });

  const auto m = acc.message();  // by value: a reference into the temporary would dangle
  REQUIRE(m.tool_calls->at(0).thought_signature == kSig);
  REQUIRE(m.tool_calls->at(0).arguments == R"({"city":"SF"})");
  REQUIRE(nlohmann::json(m).at("tool_calls").at(0).at("thought_signature") == kSig);
}

TEST_CASE("a continuation does not blank a signature already seen",
          "[stream][accumulator][failure]") {
  const auto acc = accumulate({
      std::string{R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_0",
           "type":"function","thought_signature":")"} +
          kSig + R"(","function":{"name":"f","arguments":"{"}}]}}]})",
      R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"","thought_signature":"",
           "function":{"name":"","arguments":"}"}}]}}]})",
  });

  const auto m = acc.message();
  REQUIRE(m.tool_calls->at(0).thought_signature == kSig);
  REQUIRE(nlohmann::json(m).at("tool_calls").at(0).at("thought_signature") == kSig);
}

TEST_CASE("an empty signature does not win over the real one that follows",
          "[stream][accumulator][failure]") {
  // The case that discriminates the two candidate merge rules, the way §0's
  // first case discriminates the two candidate serialization rules. A plain
  // `if (!slot.thought_signature)` guard latches the empty string from
  // fragment one and emits "" — which is the same 400 as emitting nothing,
  // with a lie attached. The non-empty guard is what makes this green.
  const auto acc = accumulate({
      R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_0","type":"function",
           "thought_signature":"","function":{"name":"f","arguments":"{"}}]}}]})",
      std::string{R"({"choices":[{"delta":{"tool_calls":[{"index":0,"thought_signature":")"} +
          kSig + R"(","function":{"arguments":"}"}}]}}]})",
  });

  const auto m = acc.message();
  REQUIRE(m.tool_calls->at(0).thought_signature == kSig);
  REQUIRE(nlohmann::json(m).at("tool_calls").at(0).at("thought_signature") == kSig);
}

TEST_CASE("a stream with no signature produces a turn with no signature key",
          "[stream][accumulator][failure]") {
  // zai-org-glm-4.7's shape. This is where the "other families' bodies do not
  // move" claim lives, and it is byte-exact for the reason §2's twin is.
  const auto acc = accumulate({
      R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_a","type":"function",
           "function":{"name":"f","arguments":"{"}}]}}]})",
      R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"}"}}]}}]})",
  });

  const auto m = acc.message();
  REQUIRE_FALSE(m.tool_calls->at(0).thought_signature.has_value());
  REQUIRE(nlohmann::json(m).at("tool_calls").at(0).dump() ==
          R"({"function":{"arguments":"{}","name":"f"},"id":"call_a","type":"function"})");
}

TEST_CASE("two calls each keep their own signature", "[stream][accumulator][failure]") {
  // Fragments interleaved out of order, which catches an implementation that
  // parks the signature on the accumulator instead of on the slot.
  const auto acc = accumulate({
      R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"a","type":"function",
           "function":{"name":"f","arguments":"{}"}}]}}]})",
      R"({"choices":[{"delta":{"tool_calls":[{"index":1,"id":"b","type":"function",
           "thought_signature":"SIG-B","function":{"name":"g","arguments":"[]"}}]}}]})",
      R"({"choices":[{"delta":{"tool_calls":[{"index":0,"thought_signature":"SIG-A"}]}}]})",
  });

  const auto m = acc.message();
  REQUIRE(m.tool_calls->at(0).thought_signature == "SIG-A");
  REQUIRE(m.tool_calls->at(1).thought_signature == "SIG-B");
  const auto sent = nlohmann::json(m).at("tool_calls");
  REQUIRE(sent.at(0).at("thought_signature") == "SIG-A");
  REQUIRE(sent.at(1).at("thought_signature") == "SIG-B");
}

TEST_CASE("raw on a merged call is the first fragment, not the whole call",
          "[stream][accumulator]") {
  // The honesty case for the merge block's corrected comment. There is no
  // single verbatim server object for a call that arrived in pieces, so raw
  // holds the opening fragment and chunks() is the complete record. Overlaying
  // the fragments would be a synthesised value wearing a "verbatim" label.
  const auto acc = accumulate({
      R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_a","type":"function",
           "function":{"name":"f","arguments":"{"}}]}}]})",
      R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"}"}}]}}]})",
  });

  const auto m = acc.message();
  const auto& raw = m.tool_calls->at(0).raw;
  REQUIRE(raw.at("id") == "call_a");
  REQUIRE(raw.at("function").at("arguments") == "{");  // fragment one, not "{}"
  REQUIRE(m.tool_calls->at(0).arguments == "{}");      // the merged value is right
  REQUIRE(acc.chunks().size() == 2);                   // and the full record is here
}

TEST_CASE("two calls in a single chunk are both taken", "[stream][accumulator][failure]") {
  const auto acc = accumulate({
      R"({"choices":[{"delta":{"tool_calls":[
           {"index":0,"id":"a","function":{"name":"f","arguments":"{}"}},
           {"index":1,"id":"b","function":{"name":"g","arguments":"[]"}}]}}]})",
  });
  REQUIRE(acc.message().tool_calls->size() == 2);
}

TEST_CASE("argument fragments are concatenated verbatim", "[stream][accumulator][failure]") {
  // Individually none of these is valid JSON, and the whitespace inside the
  // string literal is significant — a trim would corrupt the value.
  const auto acc = accumulate({
      R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\"q\":\" a "}}]}}]})",
      R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":" b \"}"}}]}}]})",
  });
  const auto m = acc.message();  // by value: a reference into the temporary would dangle
  const auto& call = m.tool_calls->at(0);
  REQUIRE(call.arguments == "{\"q\":\" a  b \"}");
  REQUIRE(call.parsed_arguments()->at("q") == " a  b ");
}

TEST_CASE("content and reasoning are independent streams", "[stream][accumulator][failure]") {
  // Interleaved in both directions, including a return to reasoning after
  // content has started. A state machine would drop one of them.
  const auto acc = accumulate({
      R"({"choices":[{"delta":{"role":"assistant"}}]})",
      R"({"choices":[{"delta":{"reasoning_content":"think1 "}}]})",
      R"({"choices":[{"delta":{"content":"ans1 "}}]})",
      R"({"choices":[{"delta":{"reasoning_content":"think2 "}}]})",
      R"({"choices":[{"delta":{"content":"ans2","reasoning_content":"think3"}}]})",
  });
  const auto m = acc.message();
  REQUIRE(m.role == "assistant");
  REQUIRE(m.text() == "ans1 ans2");
  REQUIRE(m.reasoning_content == "think1 think2 think3");
}

TEST_CASE("accumulation continues past finish_reason", "[stream][accumulator][failure]") {
  // finish_reason can arrive before the last argument fragment, and the usage
  // frame always arrives after it.
  const auto acc = accumulate({
      R"({"choices":[{"finish_reason":"tool_calls","delta":{"tool_calls":[
           {"index":0,"id":"a","function":{"arguments":"{"}}]}}]})",
      R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"}"}}]}}]})",
      R"({"choices":[],"usage":{"prompt_tokens":1,"completion_tokens":2,"total_tokens":3}})",
  });
  const auto r = acc.response();
  REQUIRE(r.finish_reason == "tool_calls");
  REQUIRE(r.message->tool_calls->at(0).arguments == "{}");
  REQUIRE(r.usage->total_tokens == 3);
}

TEST_CASE("every chunk is retained verbatim by default", "[stream][accumulator]") {
  const auto acc = accumulate({
      R"({"choices":[{"delta":{"content":"a"}}],"unmodeled_future_key":{"x":1}})",
      R"({"choices":[{"delta":{"content":"b"}}]})",
  });
  REQUIRE(acc.chunks().size() == 2);
  // Nothing received is discarded — including a key no version of this library
  // has ever heard of.
  REQUIRE(acc.chunks().at(0).at("unmodeled_future_key").at("x") == 1);
}

TEST_CASE("retention can be turned off without losing the assembly", "[stream][accumulator]") {
  venice::StreamAccumulator acc{/*keep_chunks=*/false};
  acc.ingest(nlohmann::json::parse(R"({"choices":[{"delta":{"content":"a"}}]})"));
  REQUIRE(acc.chunks().empty());
  REQUIRE(acc.message().text() == "a");
}

TEST_CASE("empty() distinguishes nothing-arrived from no-content-arrived",
          "[stream][accumulator][failure]") {
  // The old fatal-parse test was "content is empty", which reported
  // ErrorKind::Parse on a reasoning-only stream that had arrived perfectly.
  REQUIRE(venice::StreamAccumulator{}.empty());
  REQUIRE(accumulate({R"({"choices":[{"delta":{}}]})"}).empty());
  REQUIRE_FALSE(accumulate({R"({"choices":[{"delta":{"reasoning_content":"t"}}]})"}).empty());
}

TEST_CASE("ingest(chunk) is complete on its own", "[stream][accumulator][failure]") {
  // The chunk overload is the entry point for a caller driving their own
  // transport. StreamDelta deliberately does not model id/model — they are
  // envelope fields, not deltas — so this overload has to take them itself or
  // that caller silently loses them. The accumulate() helper above calls
  // note_envelope separately and would not have caught this.
  venice::StreamAccumulator acc;
  acc.ingest(nlohmann::json::parse(
      R"({"id":"chatcmpl-9","model":"m","choices":[{"delta":{"content":"x"}}]})"));

  const auto r = acc.response();
  REQUIRE(r.id == "chatcmpl-9");
  REQUIRE(r.model == "m");
  REQUIRE(r.content == "x");
}

TEST_CASE("reset clears everything but the retention setting", "[stream][accumulator]") {
  venice::StreamAccumulator acc{/*keep_chunks=*/false};
  acc.ingest(nlohmann::json::parse(R"({"choices":[{"delta":{"content":"a"}}]})"));
  acc.reset();
  REQUIRE(acc.empty());
  REQUIRE(acc.message().text().empty());

  acc.ingest(nlohmann::json::parse(R"({"choices":[{"delta":{"content":"b"}}]})"));
  REQUIRE(acc.chunks().empty());  // still off
  REQUIRE(acc.message().text() == "b");
}

// ── §10 streamed and non-streamed converge — happy path, last ─────────────
//
// The payoff assertion. If the two paths produce the same Message on the wire,
// then everything the non-streamed parse keeps, the stream keeps too — and the
// stream's own extra plumbing (framing, fragment merge, two text buffers) has
// not quietly dropped or reordered anything.
//
// What it CANNOT see, measured while running VC-18's break matrix rather than
// reasoned: a loss that is symmetric across both paths. Deleting the
// thought_signature emit from ToolCall::to_json, or moving it inside
// `function`, leaves this case green — both sides lose the key identically and
// still compare equal — while seven cases elsewhere in this file go red. It
// caught the two *asymmetric* breaks (dropping the merge line, and taking the
// signature from the first fragment only), where the non-streamed path keeps
// the field and the stream does not. So convergence is evidence about the
// stream's plumbing, never about serialization; that has to be pinned upstream,
// in §2, or it is not pinned at all.

TEST_CASE("a streamed reply assembles to the same message as a non-streamed one",
          "[stream][accumulator][converge]") {
  const auto non_streamed = ChatResponse::from_json_body(nlohmann::json::parse(R"({
    "id":"chatcmpl-1","model":"deepseek-r1",
    "choices":[{"index":0,"finish_reason":"tool_calls","message":{
      "role":"assistant","reasoning_content":"step one, step two",
      "content":"partial answer",
      "tool_calls":[{"id":"call_a","type":"function","thought_signature":"SIG-A",
                     "function":{"name":"get_weather","arguments":"{\"location\":\"SF\"}"}},
                    {"id":"call_b","type":"function",
                     "function":{"name":"clock","arguments":"{}"}}]}}],
    "usage":{"prompt_tokens":10,"completion_tokens":20,"total_tokens":30,
             "prompt_tokens_details":{"cached_tokens":4},
             "completion_tokens_details":{"reasoning_tokens":15}}})"));

  // The same reply as chunks: mid-word content splits, a two-fragment
  // arguments, an id present only in the opening fragment, the second call
  // arriving before the first is finished, and a trailing usage frame.
  const auto acc = accumulate({
      R"({"id":"chatcmpl-1","model":"deepseek-r1","choices":[{"delta":{"role":"assistant"}}]})",
      R"({"choices":[{"delta":{"reasoning_content":"step one, "}}]})",
      R"({"choices":[{"delta":{"reasoning_content":"step two"}}]})",
      R"({"choices":[{"delta":{"content":"parti"}}]})",
      R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_a","type":"function",
           "function":{"name":"get_weather","arguments":"{\"location\":"}}]}}]})",
      R"({"choices":[{"delta":{"content":"al answer"}}]})",
      R"({"choices":[{"delta":{"tool_calls":[{"index":1,"id":"call_b","type":"function",
           "function":{"name":"clock","arguments":"{}"}}]}}]})",
      R"({"choices":[{"delta":{"tool_calls":[{"index":0,"thought_signature":"SIG-A",
           "function":{"arguments":"\"SF\"}"}}]}}]})",
      R"({"choices":[{"finish_reason":"tool_calls","delta":{}}]})",
      R"({"choices":[],"usage":{"prompt_tokens":10,"completion_tokens":20,"total_tokens":30,
           "prompt_tokens_details":{"cached_tokens":4},
           "completion_tokens_details":{"reasoning_tokens":15}}})",
  });
  const auto streamed = acc.response();

  // Wire-level equality rather than a Message::operator==: it compares the
  // thing that actually matters — what the next request would carry — and
  // sidesteps committing to a semantics for `raw` in equality.
  REQUIRE(nlohmann::json(*streamed.message) == nlohmann::json(*non_streamed.message));

  REQUIRE(streamed.id == non_streamed.id);
  REQUIRE(streamed.model == non_streamed.model);
  REQUIRE(streamed.finish_reason == non_streamed.finish_reason);
  REQUIRE(streamed.usage == non_streamed.usage);
  REQUIRE(streamed.content == non_streamed.content);

  // And they must NOT be equal in raw — a streamed body is a different wire
  // object from a completion body, and asserting otherwise would invite
  // "fixing" a failure by making raw equal.
  REQUIRE(streamed.raw != non_streamed.raw);
}

TEST_CASE("the converged turn replays into the next request", "[stream][accumulator][converge]") {
  const auto acc = accumulate({
      R"({"choices":[{"delta":{"role":"assistant","reasoning_content":"thought"}}]})",
      R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_a","type":"function",
           "function":{"name":"f","arguments":"{}"}}]}}]})",
  });

  ChatRequest next;
  next.model = "m";
  next.messages = {Message::user("q"), acc.message(), Message::tool("call_a", "42")};

  const auto sent = next.to_json_body(false).at("messages");
  REQUIRE(sent.at(1).at("role") == "assistant");
  REQUIRE(sent.at(1).at("reasoning_content") == "thought");
  REQUIRE(sent.at(1).at("tool_calls").at(0).at("id") == "call_a");
  // No content key at all: a tool-call-only turn has none, and emitting "" here
  // would differ from what the non-streamed parse produces for the same reply.
  REQUIRE_FALSE(sent.at(1).contains("content"));
  REQUIRE(sent.at(2).at("tool_call_id") == "call_a");
}
