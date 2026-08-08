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

// A reasoning + tool-call reply: the shape this whole ticket is about. Written
// in the nesting the docs describe rather than captured live — there is no
// VENICE_API_KEY in the implementing environment, and saying so is better than
// implying a capture. Message::raw and ChatResponse::raw are what make that
// gap recoverable if a field name here is wrong.
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

TEST_CASE("cached_tokens is read from both locations", "[usage][parse][failure]") {
  auto usage_of = [](const char* body) { return nlohmann::json::parse(body).get<Usage>(); };

  SECTION("the flat key, as every release through v0.7.0 read it") {
    REQUIRE(usage_of(R"({"cached_tokens":80})").cached_tokens == 80);
  }
  SECTION("the nested OpenAI-canonical location, which was silently unread") {
    REQUIRE(usage_of(R"({"prompt_tokens_details":{"cached_tokens":4}})").cached_tokens == 4);
  }
  SECTION("nested wins when both are present and disagree") {
    REQUIRE(usage_of(R"({"cached_tokens":80,"prompt_tokens_details":{"cached_tokens":4}})")
                .cached_tokens == 4);
  }
  SECTION("neither leaves it unset") {
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

TEST_CASE("thinking can be refed or withheld, per turn", "[message][replay]") {
  // The use case that filed this ticket in this shape: refeeding is a decision
  // the caller makes each turn, so both branches have to be expressible.
  auto m = turn();

  m.reasoning_content = std::nullopt;
  REQUIRE_FALSE(nlohmann::json(m).contains("reasoning_content"));

  m.reasoning_content = "step one, step two";
  REQUIRE(nlohmann::json(m).at("reasoning_content") == "step one, step two");
}
