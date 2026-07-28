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
using venice::Client;
using venice::ErrorKind;
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
  const auto j = minimal().to_json_body();

  REQUIRE(j.dump() == kBaseline);
  REQUIRE(j.size() == 3);

  for (const auto* key : {"top_p", "stop", "frequency_penalty", "presence_penalty", "seed",
                          "response_format", "temperature", "max_tokens", "venice_parameters"}) {
    INFO("unset field leaked into the body: " << key);
    REQUIRE_FALSE(j.contains(key));
  }
}

// ── degenerate-but-engaged values are transmitted, not silently dropped ───

TEST_CASE("stop transmits caller intent verbatim", "[request][stop][failure]") {
  SECTION("engaged but empty emits an empty array, not nothing") {
    auto r = minimal();
    r.stop = std::vector<std::string>{};
    const auto j = r.to_json_body();
    REQUIRE(j.contains("stop"));
    REQUIRE(j["stop"].is_array());
    REQUIRE(j["stop"].empty());
  }
  SECTION("more entries than the API accepts are still sent — we do not validate") {
    auto r = minimal();
    r.stop = std::vector<std::string>{"a", "b", "c", "d", "e"};
    const auto j = r.to_json_body();
    REQUIRE(j["stop"].size() == 5);
    REQUIRE(j["stop"][4] == "e");
  }
  SECTION("an empty stop string is not pruned") {
    auto r = minimal();
    r.stop = std::vector<std::string>{"", "END"};
    const auto j = r.to_json_body();
    REQUIRE(j["stop"].size() == 2);
    REQUIRE(j["stop"][0] == "");
  }
}

TEST_CASE("response_format engaged with a null json emits null", "[request][failure]") {
  auto r = minimal();
  r.response_format.emplace();  // default-constructed nlohmann::json is null
  const auto j = r.to_json_body();
  REQUIRE(j.contains("response_format"));
  REQUIRE(j["response_format"].is_null());
}

TEST_CASE("response_format passes an unmodeled object through verbatim", "[request]") {
  auto r = minimal();
  r.response_format = nlohmann::json::parse(R"({"type":"future_mode","x":1})");
  const auto j = r.to_json_body();
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
    REQUIRE(r.to_json_body()["seed"].get<std::int64_t>() == -1);
  }
  SECTION("zero is emitted, not treated as unset") {
    auto r = minimal();
    r.seed = 0;
    const auto j = r.to_json_body();
    REQUIRE(j.contains("seed"));
    REQUIRE(j["seed"].get<std::int64_t>() == 0);
  }
  SECTION("past INT_MAX") {
    auto r = minimal();
    r.seed = 4294967296LL;
    REQUIRE(r.to_json_body()["seed"].get<std::int64_t>() == 4294967296LL);
  }
}

// ── non-finite doubles: a known quirk, pinned rather than left a mystery ──
//
// JSON cannot represent NaN or infinity, so nlohmann emits null and the server
// 400s with a message that will not mention NaN. This is structural rather than
// a range question, so it arguably belongs behind an InvalidArg guard in
// Client — filed as a follow-up. Until then, the behavior is documented here.

TEST_CASE("non-finite doubles serialize as null", "[request][failure]") {
  auto r = minimal();
  r.top_p = std::numeric_limits<double>::quiet_NaN();
  r.frequency_penalty = std::numeric_limits<double>::infinity();
  const auto j = r.to_json_body();

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
  const auto j = r.to_json_body();
  REQUIRE(j.contains("frequency_penalty"));
  REQUIRE(j["frequency_penalty"] == 0.0);
  REQUIRE(j["presence_penalty"] == -2.0);
}

// ── out-of-range values are the server's call, not ours ───────────────────

TEST_CASE("out-of-range sampling values are transmitted, not rejected", "[request][failure]") {
  auto r = minimal();
  r.temperature = 5.0;  // API range is 0-2
  r.top_p = 1.5;        // API range is 0-1
  const auto j = r.to_json_body();
  REQUIRE(j["temperature"] == 5.0);
  REQUIRE(j["top_p"] == 1.5);
}

// ── extra: the hostile cases ──────────────────────────────────────────────

TEST_CASE("extra cannot corrupt the body", "[request][extra][failure]") {
  SECTION("default (null) extra leaves the baseline untouched") {
    REQUIRE(minimal().to_json_body().dump() == kBaseline);
  }
  SECTION("an array-valued extra does not throw out of the public API") {
    auto r = minimal();
    r.extra = nlohmann::json::array({1, 2, 3});
    REQUIRE_NOTHROW(r.to_json_body());
    REQUIRE(r.to_json_body().dump() == kBaseline);
  }
  SECTION("a number-valued extra does not throw either") {
    auto r = minimal();
    r.extra = 42;
    REQUIRE_NOTHROW(r.to_json_body());
    REQUIRE(r.to_json_body().dump() == kBaseline);
  }
  SECTION("modeled fields win over same-named extra keys") {
    auto r = minimal();
    r.temperature = 0.5;
    r.stream = false;
    r.extra = nlohmann::json::parse(R"({"model":"wrong","stream":true,"temperature":9})");
    const auto j = r.to_json_body();
    REQUIRE(j["model"] == "m");
    REQUIRE(j["stream"] == false);
    REQUIRE(j["temperature"] == 0.5);
  }
  SECTION("unmodeled keys pass through") {
    auto r = minimal();
    r.extra = nlohmann::json::parse(R"({"top_k":40,"min_p":0.05})");
    const auto j = r.to_json_body();
    REQUIRE(j["top_k"] == 40);
    REQUIRE(j["min_p"] == 0.05);
  }
}

TEST_CASE("extra cannot smuggle past the InvalidArg guard", "[request][extra][failure]") {
  // Offline-safe: Client::chat returns on the empty-model check before it
  // touches the transport.
  const Client c{"not-a-real-key"};
  ChatRequest r;
  r.messages = {Message::user("hi")};
  r.extra = nlohmann::json::parse(R"({"model":"m"})");

  const auto res = c.chat(r);
  REQUIRE_FALSE(res.has_value());
  REQUIRE(res.error().is(ErrorKind::InvalidArg));
}

// ── happy path ────────────────────────────────────────────────────────────

TEST_CASE("each new field serializes with the right value and json type", "[request]") {
  SECTION("top_p") {
    auto r = minimal();
    r.top_p = 0.9;
    const auto j = r.to_json_body();
    REQUIRE(j["top_p"].is_number_float());
    REQUIRE(j["top_p"] == 0.9);
  }
  SECTION("stop") {
    auto r = minimal();
    r.stop = std::vector<std::string>{"\n\n"};
    const auto j = r.to_json_body();
    REQUIRE(j["stop"].is_array());
    REQUIRE(j["stop"][0] == "\n\n");
  }
  SECTION("frequency_penalty / presence_penalty") {
    auto r = minimal();
    r.frequency_penalty = 1.5;
    r.presence_penalty = 0.25;
    const auto j = r.to_json_body();
    REQUIRE(j["frequency_penalty"].is_number_float());
    REQUIRE(j["frequency_penalty"] == 1.5);
    REQUIRE(j["presence_penalty"] == 0.25);
  }
  SECTION("seed") {
    auto r = minimal();
    r.seed = 1234;
    const auto j = r.to_json_body();
    REQUIRE(j["seed"].is_number_integer());
    REQUIRE(j["seed"] == 1234);
  }
  SECTION("response_format") {
    auto r = minimal();
    r.response_format = venice::response_format::json_object();
    const auto j = r.to_json_body();
    REQUIRE(j["response_format"].is_object());
    REQUIRE(j["response_format"]["type"] == "json_object");
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
  SECTION("an array-valued schema is not flattened into the outer object") {
    // The reason the builders assign field by field instead of using a brace
    // initializer list: nlohmann would read the outer list as an array here.
    const auto j = venice::response_format::json_schema("n", nlohmann::json::array({1, 2}), false);
    REQUIRE(j["type"] == "json_schema");
    REQUIRE(j["json_schema"]["schema"].is_array());
    REQUIRE(j["json_schema"]["strict"] == false);
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
  r.venice_parameters = VeniceParameters{};
  r.venice_parameters->disable_thinking = true;
  r.extra = nlohmann::json::parse(R"({"top_k":40})");
  r.stream = true;

  // Exact equality, not a field-by-field walk — an accidentally emitted key
  // fails this. Compared as json rather than as a dumped string only so the
  // assertion does not also encode nlohmann's float formatting.
  const auto expected = nlohmann::json::parse(R"({
    "frequency_penalty": 0.5,
    "max_tokens": 64,
    "messages": [{"content":"hi","role":"user"}],
    "model": "m",
    "presence_penalty": -0.25,
    "response_format": {"type":"json_object"},
    "seed": 7,
    "stop": ["\n"],
    "stream": true,
    "temperature": 0.5,
    "top_k": 40,
    "top_p": 0.9,
    "venice_parameters": {"disable_thinking":true}
  })");

  REQUIRE(r.to_json_body() == expected);
}
