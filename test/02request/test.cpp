// ChatRequest wire-body serialization — offline, no API key or network.
//
// Every assertion about what `to_json_body()` puts on the wire lives here, so
// the baseline body has exactly one home. Failure matrix first (degenerate
// values, hostile `extra`, non-finite doubles), happy path last.
//
// nlohmann's default `json` is std::map-backed, so `dump()` emits keys in
// alphabetical order regardless of insertion order. That is what makes the
// exact-string comparisons below stable.
//
// One rule this file must not break: never hand a std::optional to nlohmann.
// cmake/deps/nlohmann_json.cmake prefers a system nlohmann and only falls back
// to a pinned v3.11.3; optional support landed in 3.12.0. `j["stop"] = r.stop;`
// therefore fails to compile on the pin and silently emits null on a 3.12 box.
// Always dereference first — in the library and here.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <venice/venice.hpp>

using venice::ChatRequest;
using venice::Message;
using venice::VeniceParameters;

namespace {

// The minimal request every case below starts from.
auto minimal() -> ChatRequest {
  ChatRequest r;
  r.model = "m";
  r.messages = {Message::user("hi")};
  return r;
}

// The byte-exact body a minimal request must produce. Every new optional field
// added to ChatRequest must leave this string untouched when unset.
constexpr auto kBaseline = R"({"messages":[{"content":"hi","role":"user"}],"model":"m","stream":false})";

}  // namespace

// ── the baseline: absent optionals must not appear ────────────────────────
//
// This is the regression the ticket's acceptance criterion is really about, and
// it only works as an exact comparison: a key-by-key `contains` check passes
// happily when an *extra* key is accidentally emitted. Byte equality does not.

TEST_CASE("absent optionals produce the byte-identical baseline body", "[request][baseline]") {
  const auto j = minimal().to_json_body(false);

  REQUIRE(j.dump() == kBaseline);
  REQUIRE(j.size() == 3);

  for (const auto* key : {"top_p", "stop", "frequency_penalty", "presence_penalty", "seed",
                          "response_format", "temperature", "max_tokens", "venice_parameters",
                          "tools", "tool_choice", "parallel_tool_calls"}) {
    INFO("unset field leaked into the body: " << key);
    REQUIRE_FALSE(j.contains(key));
  }
}

// ── degenerate-but-engaged values are transmitted, not silently dropped ───

TEST_CASE("stop transmits caller intent verbatim", "[request][stop][failure]") {
  SECTION("engaged but empty emits an empty array, not nothing") {
    auto r = minimal();
    r.stop = std::vector<std::string>{};
    const auto j = r.to_json_body(false);
    REQUIRE(j.contains("stop"));
    REQUIRE(j["stop"].is_array());
    REQUIRE(j["stop"].empty());
  }
  SECTION("more entries than the API accepts are still sent — we do not validate") {
    auto r = minimal();
    r.stop = std::vector<std::string>{"a", "b", "c", "d", "e"};
    const auto j = r.to_json_body(false);
    REQUIRE(j["stop"].size() == 5);
    REQUIRE(j["stop"][4] == "e");
  }
  SECTION("an empty stop string is not pruned") {
    auto r = minimal();
    r.stop = std::vector<std::string>{"", "END"};
    const auto j = r.to_json_body(false);
    REQUIRE(j["stop"].size() == 2);
    REQUIRE(j["stop"][0] == "");
  }
}

TEST_CASE("response_format engaged with a null json emits null", "[request][failure]") {
  auto r = minimal();
  r.response_format.emplace();  // default-constructed nlohmann::json is null
  const auto j = r.to_json_body(false);
  REQUIRE(j.contains("response_format"));
  REQUIRE(j["response_format"].is_null());
}

TEST_CASE("response_format passes an unmodeled object through verbatim", "[request]") {
  auto r = minimal();
  r.response_format = nlohmann::json::parse(R"({"type":"future_mode","x":1})");
  const auto j = r.to_json_body(false);
  REQUIRE(j["response_format"]["type"] == "future_mode");
  REQUIRE(j["response_format"]["x"] == 1);
}

// ── seed is int64, and provably so ────────────────────────────────────────
//
// Callers seed from std::random_device / mt19937, which produce uint32_t values
// past INT_MAX. Reading back through get<std::int64_t>() is what proves the
// value neither narrowed to int nor round-tripped through a double.

TEST_CASE("seed survives its boundaries", "[request][seed][failure]") {
  SECTION("negative") {
    auto r = minimal();
    r.seed = -1;
    REQUIRE(r.to_json_body(false)["seed"].get<std::int64_t>() == -1);
  }
  SECTION("zero is emitted, not treated as unset") {
    auto r = minimal();
    r.seed = 0;
    const auto j = r.to_json_body(false);
    REQUIRE(j.contains("seed"));
    REQUIRE(j["seed"].get<std::int64_t>() == 0);
  }
  SECTION("past INT_MAX") {
    auto r = minimal();
    r.seed = 4294967296LL;
    REQUIRE(r.to_json_body(false)["seed"].get<std::int64_t>() == 4294967296LL);
  }
}

// ── non-finite doubles: what the serializer alone still does ──────────────
//
// Client::chat and Client::chat_stream reject these with InvalidArg before they
// serialize anything (VC-10, #14 — see test/03guards/). to_json_body does not:
// it is public, it takes no view on whether a request is sendable, and a caller
// who builds a body by hand bypasses the guard entirely. So the old behavior is
// still reachable and still pinned here — JSON cannot represent NaN or infinity,
// so nlohmann emits null and the server 400s with a message that will not
// mention NaN. Two layers, two contracts.

TEST_CASE("non-finite doubles serialize as null", "[request][failure]") {
  auto r = minimal();
  r.top_p = std::numeric_limits<double>::quiet_NaN();
  r.frequency_penalty = std::numeric_limits<double>::infinity();
  const auto j = r.to_json_body(false);

  // The value is still a float in memory — the collapse to null happens at
  // dump() time, which is the only place it matters. Asserting is_null() on the
  // node would pass for the wrong reason (or, as it did here, fail).
  REQUIRE(j["top_p"].is_number_float());
  const auto wire = j.dump();
  REQUIRE(wire.find(R"("top_p":null)") != std::string::npos);
  REQUIRE(wire.find(R"("frequency_penalty":null)") != std::string::npos);
}

// ── zero and negative penalties are meaningful values ─────────────────────
//
// The bug this catches is `if (*opt)` where `if (opt)` was meant: 0.0 is a
// legitimate penalty and would vanish.

TEST_CASE("zero and negative penalties are emitted", "[request][failure]") {
  auto r = minimal();
  r.frequency_penalty = 0.0;
  r.presence_penalty = -2.0;
  const auto j = r.to_json_body(false);
  REQUIRE(j.contains("frequency_penalty"));
  REQUIRE(j["frequency_penalty"] == 0.0);
  REQUIRE(j["presence_penalty"] == -2.0);
}

// ── tools: the field this ticket exists for, failure cases first ──────────
//
// parallel_tool_calls comes first because it is the one new case where the
// plausible, correct-looking implementation — `if (*parallel_tool_calls)` —
// compiles clean under -Wall -Wextra -pedantic, leaves the baseline byte-exact,
// and passes every other case below including the fully populated happy path,
// which sets it true. It is the penalties bug above in a type where the
// degenerate value is half the domain.

TEST_CASE("parallel_tool_calls = false is emitted, not read as unset",
          "[request][tools][failure]") {
  auto r = minimal();
  r.parallel_tool_calls = false;
  const auto j = r.to_json_body(false);
  REQUIRE(j.contains("parallel_tool_calls"));
  REQUIRE(j["parallel_tool_calls"].is_boolean());
  REQUIRE(j["parallel_tool_calls"] == false);
}

TEST_CASE("tools transmits caller intent verbatim", "[request][tools][failure]") {
  SECTION("engaged but empty emits an empty array, not nothing") {
    // The whole reason the member is optional<vector<>> rather than a bare
    // vector: a bare one cannot tell "offer no tools" from "say nothing about
    // tools", and those are different requests.
    auto r = minimal();
    r.tools = std::vector<nlohmann::json>{};
    const auto j = r.to_json_body(false);
    REQUIRE(j.contains("tools"));
    REQUIRE(j["tools"].is_array());
    REQUIRE(j["tools"].empty());
  }
  SECTION("order is preserved") {
    auto r = minimal();
    r.tools = std::vector<nlohmann::json>{venice::tools::function("a"),
                                          venice::tools::function("b")};
    const auto j = r.to_json_body(false);
    REQUIRE(j["tools"].size() == 2);
    REQUIRE(j["tools"][0]["function"]["name"] == "a");
    REQUIRE(j["tools"][1]["function"]["name"] == "b");
  }
  SECTION("two tools with the same name are both sent — we do not dedupe") {
    auto r = minimal();
    r.tools = std::vector<nlohmann::json>{venice::tools::function("dup"),
                                          venice::tools::function("dup")};
    REQUIRE(r.to_json_body(false)["tools"].size() == 2);
  }
}

TEST_CASE("tool_choice engaged with a null json emits null", "[request][tools][failure]") {
  // Same rule as response_format above: the caller said something, and silently
  // discarding it is harder to diagnose than the 400 that follows.
  auto r = minimal();
  r.tool_choice = nlohmann::json();
  const auto j = r.to_json_body(false);
  REQUIRE(j.contains("tool_choice"));
  REQUIRE(j["tool_choice"].is_null());
}

TEST_CASE("tools elements are opaque to the serializer", "[request][tools][failure]") {
  // This pins the *design*, not just a behaviour: elements are raw json, so a
  // tool entry that is not a function reaches the wire intact. Replacing the
  // element type with a typed Tool that re-nests under "function" does not turn
  // this red — it stops the file compiling, which is the intended signal.
  auto r = minimal();
  r.tools = std::vector<nlohmann::json>{
      nlohmann::json::parse(R"({"type":"web_search","x":1})")};
  const auto j = r.to_json_body(false);
  REQUIRE(j["tools"][0]["type"] == "web_search");
  REQUIRE(j["tools"][0]["x"] == 1);
  REQUIRE_FALSE(j["tools"][0].contains("function"));
}

TEST_CASE("the three tool fields are independent", "[request][tools][failure]") {
  // Nothing here can break today. It exists so that the next hand to touch
  // to_json_body cannot add a helpful-looking coupling and stay green — every
  // one of these combinations is a legitimate request the server judges, and a
  // client-side cross-check would refuse a valid one the day Venice widens it.
  SECTION("parallel_tool_calls without tools is still emitted") {
    auto r = minimal();
    r.parallel_tool_calls = true;
    const auto j = r.to_json_body(false);
    REQUIRE(j["parallel_tool_calls"] == true);
    REQUIRE_FALSE(j.contains("tools"));
  }
  SECTION("tool_choice naming a function absent from tools is still emitted") {
    auto r = minimal();
    r.tools = std::vector<nlohmann::json>{venice::tools::function("a")};
    r.tool_choice = venice::tool_choice::function("nope");
    const auto j = r.to_json_body(false);
    REQUIRE(j["tool_choice"]["function"]["name"] == "nope");
    REQUIRE(j["tools"][0]["function"]["name"] == "a");
  }
  SECTION("tool_choice without tools is still emitted") {
    auto r = minimal();
    r.tool_choice = venice::tool_choice::required();
    const auto j = r.to_json_body(false);
    REQUIRE(j["tool_choice"] == "required");
    REQUIRE_FALSE(j.contains("tools"));
  }
}

// ── out-of-range values are the server's call, not ours ───────────────────

TEST_CASE("out-of-range sampling values are transmitted, not rejected", "[request][failure]") {
  auto r = minimal();
  r.temperature = 5.0;  // API range is 0-2
  r.top_p = 1.5;        // API range is 0-1
  const auto j = r.to_json_body(false);
  REQUIRE(j["temperature"] == 5.0);
  REQUIRE(j["top_p"] == 1.5);
}

// ── extra: the hostile cases ──────────────────────────────────────────────

TEST_CASE("extra cannot corrupt the body", "[request][extra][failure]") {
  SECTION("default (null) extra leaves the baseline untouched") {
    REQUIRE(minimal().to_json_body(false).dump() == kBaseline);
  }
  SECTION("an array-valued extra does not throw out of the public API") {
    auto r = minimal();
    r.extra = nlohmann::json::array({1, 2, 3});
    REQUIRE_NOTHROW(r.to_json_body(false));
    REQUIRE(r.to_json_body(false).dump() == kBaseline);
  }
  SECTION("a number-valued extra does not throw either") {
    auto r = minimal();
    r.extra = 42;
    REQUIRE_NOTHROW(r.to_json_body(false));
    REQUIRE(r.to_json_body(false).dump() == kBaseline);
  }
  SECTION("modeled fields win over same-named extra keys") {
    auto r = minimal();
    r.temperature = 0.5;
    r.extra = nlohmann::json::parse(R"({"model":"wrong","stream":true,"temperature":9})");

    // Both directions, because `stream` is now an argument rather than a member:
    // an extra["stream"] of true must lose to a false argument *and* to a true
    // one. Asserting only the first would pass for a to_json_body that ignored
    // its parameter and let extra through.
    const auto off = r.to_json_body(false);
    REQUIRE(off["model"] == "m");
    REQUIRE(off["stream"] == false);
    REQUIRE(off["temperature"] == 0.5);

    REQUIRE(r.to_json_body(true)["stream"] == true);
  }
  SECTION("unmodeled keys pass through") {
    auto r = minimal();
    r.extra = nlohmann::json::parse(R"({"top_k":40,"min_p":0.05})");
    const auto j = r.to_json_body(false);
    REQUIRE(j["top_k"] == 40);
    REQUIRE(j["min_p"] == 0.05);
  }
  SECTION("modeled tool fields win over same-named extra keys") {
    auto r = minimal();
    r.tools = std::vector<nlohmann::json>{venice::tools::function("real")};
    r.tool_choice = venice::tool_choice::none();
    r.parallel_tool_calls = false;
    r.extra = nlohmann::json::parse(
        R"({"tools":[{"type":"bogus"}],"tool_choice":"required","parallel_tool_calls":true})");

    const auto j = r.to_json_body(false);
    REQUIRE(j["tools"].size() == 1);
    REQUIRE(j["tools"][0]["function"]["name"] == "real");
    REQUIRE(j["tool_choice"] == "none");
    REQUIRE(j["parallel_tool_calls"] == false);
  }
  SECTION("with the modeled fields unset, extra's versions survive") {
    // The other half of "modeled fields win": to_json_body is assign-only, with
    // no erase branch, so a disengaged optional leaves an extra-supplied key
    // standing. That is the deliberate asymmetry with Message::to_json, whose
    // extra is routinely seeded from a response — this one is caller-authored.
    auto r = minimal();
    r.extra = nlohmann::json::parse(R"({"tool_choice":"required","parallel_tool_calls":true})");
    const auto j = r.to_json_body(false);
    REQUIRE(j["tool_choice"] == "required");
    REQUIRE(j["parallel_tool_calls"] == true);
  }
}

// "extra cannot smuggle past the InvalidArg guard" moved to test/03guards/
// (VC-10): it asserted on Client::chat's return value rather than on a body, so
// it belonged to that file's charter, not this one's.

// ── happy path ────────────────────────────────────────────────────────────

TEST_CASE("each new field serializes with the right value and json type", "[request]") {
  SECTION("top_p") {
    auto r = minimal();
    r.top_p = 0.9;
    const auto j = r.to_json_body(false);
    REQUIRE(j["top_p"].is_number_float());
    REQUIRE(j["top_p"] == 0.9);
  }
  SECTION("stop") {
    auto r = minimal();
    r.stop = std::vector<std::string>{"\n\n"};
    const auto j = r.to_json_body(false);
    REQUIRE(j["stop"].is_array());
    REQUIRE(j["stop"][0] == "\n\n");
  }
  SECTION("frequency_penalty / presence_penalty") {
    auto r = minimal();
    r.frequency_penalty = 1.5;
    r.presence_penalty = 0.25;
    const auto j = r.to_json_body(false);
    REQUIRE(j["frequency_penalty"].is_number_float());
    REQUIRE(j["frequency_penalty"] == 1.5);
    REQUIRE(j["presence_penalty"] == 0.25);
  }
  SECTION("seed") {
    auto r = minimal();
    r.seed = 1234;
    const auto j = r.to_json_body(false);
    REQUIRE(j["seed"].is_number_integer());
    REQUIRE(j["seed"] == 1234);
  }
  SECTION("response_format") {
    auto r = minimal();
    r.response_format = venice::response_format::json_object();
    const auto j = r.to_json_body(false);
    REQUIRE(j["response_format"].is_object());
    REQUIRE(j["response_format"]["type"] == "json_object");
  }
  SECTION("tools") {
    auto r = minimal();
    r.tools = std::vector<nlohmann::json>{venice::tools::function("get_weather")};
    const auto j = r.to_json_body(false);
    REQUIRE(j["tools"].is_array());
    REQUIRE(j["tools"][0]["function"]["name"] == "get_weather");
  }
  SECTION("tool_choice") {
    auto r = minimal();
    r.tool_choice = venice::tool_choice::automatic();
    const auto j = r.to_json_body(false);
    REQUIRE(j["tool_choice"].is_string());
    REQUIRE(j["tool_choice"] == "auto");
  }
  SECTION("parallel_tool_calls") {
    auto r = minimal();
    r.parallel_tool_calls = true;
    const auto j = r.to_json_body(false);
    REQUIRE(j["parallel_tool_calls"].is_boolean());
    REQUIRE(j["parallel_tool_calls"] == true);
  }
}

TEST_CASE("response_format builders emit the documented shapes", "[request][response_format]") {
  REQUIRE(venice::response_format::text().dump() == R"({"type":"text"})");
  REQUIRE(venice::response_format::json_object().dump() == R"({"type":"json_object"})");

  SECTION("json_schema defaults to strict") {
    const auto j = venice::response_format::json_schema("n", nlohmann::json::parse(R"({"type":"object"})"));
    REQUIRE(j.dump() ==
            R"({"json_schema":{"name":"n","schema":{"type":"object"},"strict":true},"type":"json_schema"})");
  }
  SECTION("an array-valued schema is carried through, not flattened") {
    // This case used to claim it pinned the field-by-field builder style against
    // a brace-init that "would read the outer list as an array". It does not,
    // and never did: rebuilding json_schema as a single brace-init leaves this
    // green on the pinned 3.11.3 (measured in VC-08, along with five other
    // spellings). What it does assert is real and worth keeping — a schema is
    // passed through whatever its json type — so the assertion stands and the
    // claim above it is now accurate. See the note on json_schema in types.hpp.
    const auto j = venice::response_format::json_schema("n", nlohmann::json::array({1, 2}), false);
    REQUIRE(j["type"] == "json_schema");
    REQUIRE(j["json_schema"]["schema"].is_array());
    REQUIRE(j["json_schema"]["strict"] == false);
  }
}

TEST_CASE("tool builders emit the documented shapes", "[request][tools]") {
  SECTION("tool_choice's three keywords are json strings, not arrays") {
    // is_string() is the assertion that matters, and it is not pedantry:
    // `nlohmann::json{"auto"}` is list-initialization and yields the ARRAY
    // ["auto"] — measured against the pinned 3.11.3. Both spellings compile, so
    // only this catches the wrong one. The dump comparisons alone would too, but
    // stating the type says what the mistake looks like.
    REQUIRE(venice::tool_choice::automatic().is_string());
    REQUIRE(venice::tool_choice::automatic().dump() == R"("auto")");
    REQUIRE(venice::tool_choice::none().dump() == R"("none")");
    REQUIRE(venice::tool_choice::required().dump() == R"("required")");
  }
  SECTION("tool_choice::function nests under the name the API expects") {
    REQUIRE(venice::tool_choice::function("f").dump() ==
            R"({"function":{"name":"f"},"type":"function"})");
  }
  SECTION("tools::function with everything set") {
    REQUIRE(venice::tools::function("get_weather", "Current weather",
                                    nlohmann::json::parse(R"({"type":"object"})"))
                .dump() ==
            R"({"function":{"description":"Current weather","name":"get_weather",)"
            R"("parameters":{"type":"object"}},"type":"function"})");
  }
  SECTION("an empty description and a null parameters are omitted") {
    // Omitting `parameters` is how a zero-argument function is declared;
    // "parameters": null is a 400. A plain string/json parameter has no unset
    // state, so the degenerate value is the caller's only way to say "no".
    const auto j = venice::tools::function("noop");
    REQUIRE(j.dump() == R"({"function":{"name":"noop"},"type":"function"})");
    REQUIRE_FALSE(j["function"].contains("description"));
    REQUIRE_FALSE(j["function"].contains("parameters"));
  }
  SECTION("an empty name is still emitted") {
    // Deliberate: the server's 400 names the offending entry, and dropping the
    // key would produce a different, less legible one. Client::validate does not
    // check it either — test/03guards/ pins that.
    const auto j = venice::tools::function("");
    REQUIRE(j["function"].contains("name"));
    REQUIRE(j["function"]["name"] == "");
  }
  SECTION("parameters is carried through whatever its json type") {
    // Not a brace-init tripwire — measured, and no brace spelling of this
    // builder breaks it on the pinned 3.11.3. It pins the passthrough instead:
    // `parameters` is the caller's schema and this builder never inspects it, so
    // an entry Venice grows a use for arrives intact.
    const auto j = venice::tools::function("f", "d", nlohmann::json::array({1, 2}));
    REQUIRE(j["type"] == "function");
    REQUIRE(j["function"]["parameters"].is_array());
    REQUIRE(j["function"]["parameters"].size() == 2);
  }
}

TEST_CASE("a fully populated request builds the whole wire shape", "[request]") {
  auto r = minimal();
  r.temperature = 0.5;
  r.top_p = 0.9;
  r.max_tokens = 64;
  r.stop = std::vector<std::string>{"\n"};
  r.frequency_penalty = 0.5;
  r.presence_penalty = -0.25;
  r.seed = 7;
  r.response_format = venice::response_format::json_object();
  r.tools = std::vector<nlohmann::json>{venice::tools::function(
      "get_weather", "Look up the weather", nlohmann::json::parse(R"({"type":"object"})"))};
  r.tool_choice = venice::tool_choice::automatic();
  r.parallel_tool_calls = true;
  r.venice_parameters = VeniceParameters{};
  r.venice_parameters->disable_thinking = true;
  r.extra = nlohmann::json::parse(R"({"top_k":40})");

  // Exact equality, not a field-by-field walk — an accidentally emitted key
  // fails this. Compared as json rather than as a dumped string only so the
  // assertion does not also encode nlohmann's float formatting.
  const auto expected = nlohmann::json::parse(R"({
    "frequency_penalty": 0.5,
    "max_tokens": 64,
    "messages": [{"content":"hi","role":"user"}],
    "model": "m",
    "parallel_tool_calls": true,
    "presence_penalty": -0.25,
    "response_format": {"type":"json_object"},
    "seed": 7,
    "stop": ["\n"],
    "stream": true,
    "temperature": 0.5,
    "tool_choice": "auto",
    "tools": [{"function":{"description":"Look up the weather","name":"get_weather",
                           "parameters":{"type":"object"}},"type":"function"}],
    "top_k": 40,
    "top_p": 0.9,
    "venice_parameters": {"disable_thinking":true}
  })");

  REQUIRE(r.to_json_body(true) == expected);
}
