// Unit tests for venice-cpp — offline, no API key or network required.
//
// Following the repo's testing philosophy: probe the failure modes, not just
// the happy path. These cover serialization round-trips, response parsing
// (including malformed input), and the error model's status mapping.

#include <catch2/catch_test_macros.hpp>

#include <venice/venice.hpp>

using venice::ChatRequest;
using venice::ChatResponse;
using venice::Error;
using venice::ErrorKind;
using venice::Message;
using venice::Usage;
using venice::VeniceParameters;

// ── Message / serialization ───────────────────────────────────────────────

TEST_CASE("Message: role/content round-trip and named constructors", "[types]") {
  auto sys = Message::system("be terse");
  REQUIRE(sys.role == "system");
  REQUIRE(sys.content == "be terse");

  nlohmann::json j = Message::user("hello");
  REQUIRE(j["role"] == "user");
  REQUIRE(j["content"] == "hello");
}

// ── VeniceParameters: only set fields are serialized ─────────────────────

TEST_CASE("VeniceParameters serializes only set fields", "[types][params]") {
  VeniceParameters p;
  SECTION("empty params produce an empty object") {
    REQUIRE(nlohmann::json(p).empty());
  }
  SECTION("set fields appear; unset do not") {
    p.enable_web_search = "auto";
    p.disable_thinking = true;
    auto j = nlohmann::json(p);
    REQUIRE(j["enable_web_search"] == "auto");
    REQUIRE(j["disable_thinking"] == true);
    REQUIRE_FALSE(j.contains("character_slug"));
    REQUIRE_FALSE(j.contains("enable_x_search"));
  }
  SECTION("extra passthrough preserves unknown future keys") {
    p.extra = {{"some_future_flag", true}};
    p.enable_x_search = true;
    auto j = nlohmann::json(p);
    REQUIRE(j["some_future_flag"] == true);
    REQUIRE(j["enable_x_search"] == true);
  }
}

// ── ChatRequest body ──────────────────────────────────────────────────────

TEST_CASE("ChatRequest::to_json_body builds the wire shape", "[types]") {
  ChatRequest r;
  r.model = "llama-3.3-70b";
  r.messages = {Message::user("hi")};
  r.temperature = 0.7;
  r.max_tokens = 64;
  r.venice_parameters = VeniceParameters{};
  r.venice_parameters->enable_web_search = "off";

  auto j = r.to_json_body();
  REQUIRE(j["model"] == "llama-3.3-70b");
  REQUIRE(j["messages"].size() == 1);
  REQUIRE(j["temperature"] == 0.7);
  REQUIRE(j["max_tokens"] == 64);
  REQUIRE(j["stream"] == false);
  REQUIRE(j["venice_parameters"]["enable_web_search"] == "off");
}

// ── ChatResponse parsing (happy + failure) ───────────────────────────────

TEST_CASE("ChatResponse::from_json_body parses a normal response", "[parse]") {
  const auto body = nlohmann::json::parse(R"({
    "id": "chatcmpl-123",
    "model": "llama-3.3-70b",
    "choices": [{
      "message": {"role": "assistant", "content": "Hello there."},
      "finish_reason": "stop"
    }],
    "usage": {"prompt_tokens": 5, "completion_tokens": 3, "total_tokens": 8}
  })");

  auto r = ChatResponse::from_json_body(body);
  REQUIRE(r.id == "chatcmpl-123");
  REQUIRE(r.model == "llama-3.3-70b");
  REQUIRE(r.content == "Hello there.");
  REQUIRE(r.finish_reason == "stop");
  REQUIRE(r.usage.has_value());
  REQUIRE(r.usage->prompt_tokens == 5);
  REQUIRE(r.usage->completion_tokens == 3);
}

TEST_CASE("ChatResponse::from_json_body fails loudly on malformed input", "[parse][failure]") {
  SECTION("missing choices array throws") {
    const auto bad = nlohmann::json::parse(R"({"id":"x"})");
    REQUIRE_THROWS(ChatResponse::from_json_body(bad));
  }
  SECTION("non-json input throws") {
    REQUIRE_THROWS(nlohmann::json::parse("this is not json"));
  }
  SECTION("empty choices yields empty content, not a crash") {
    const auto body = nlohmann::json::parse(R"({"id":"x","choices":[]})");
    auto r = ChatResponse::from_json_body(body);
    REQUIRE(r.content.empty());
  }
  SECTION("absent usage leaves usage empty") {
    const auto body = nlohmann::json::parse(
        R"({"choices":[{"message":{"content":"hi"},"finish_reason":"stop"}]})");
    auto r = ChatResponse::from_json_body(body);
    REQUIRE(r.content == "hi");
    REQUIRE_FALSE(r.usage.has_value());
  }
}

// ── Usage cache buckets stay distinct (venice-cli #75) ───────────────────

TEST_CASE("Usage keeps cached_tokens distinct when reported", "[parse][usage]") {
  const auto j = nlohmann::json::parse(
      R"({"prompt_tokens":100,"completion_tokens":10,"total_tokens":110,"cached_tokens":80})");
  auto u = j.get<Usage>();
  REQUIRE(u.prompt_tokens == 100);
  REQUIRE(u.cached_tokens.has_value());
  REQUIRE(*u.cached_tokens == 80);
}

// ── Error model: status → kind mapping ───────────────────────────────────

TEST_CASE("kind_for_status maps auth and rate-limit distinctly", "[error]") {
  REQUIRE(venice::kind_for_status(401) == ErrorKind::Auth);
  REQUIRE(venice::kind_for_status(403) == ErrorKind::Auth);
  REQUIRE(venice::kind_for_status(429) == ErrorKind::RateLimited);
  REQUIRE(venice::kind_for_status(500) == ErrorKind::Http);
  REQUIRE(venice::kind_for_status(200) == ErrorKind::Http);  // non-error path maps generic
}

TEST_CASE("Error carries status and body for inspection", "[error]") {
  Error e{ErrorKind::RateLimited, 429, "HTTP 429", "{\"detail\":\"slow down\"}"};
  REQUIRE(e.is(ErrorKind::RateLimited));
  REQUIRE(e.status == 429);
  REQUIRE(e.body.find("slow down") != std::string::npos);
}
