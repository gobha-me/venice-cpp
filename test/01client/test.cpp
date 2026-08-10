// Unit tests for venice-cpp — offline, no API key or network required.
//
// Following the repo's testing philosophy: probe the failure modes, not just
// the happy path. These cover message/venice_parameters serialization, response
// parsing (including malformed input), and the error model's status mapping.
// ChatRequest body serialization lives in test/02request/ — all of it, so the
// wire-body baseline has exactly one home.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_template_test_macros.hpp>

#include <cstdint>
#include <type_traits>

#include <venice/venice.hpp>
#include <version.hpp>

using venice::ChatRequest;
using venice::ChatResponse;
using venice::Authentication;
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
  SECTION("absent cost leaves cost empty") {
    // Its sibling, kept aligned here on purpose — this is the smaller of the
    // two from_json_body sets and its job is to stay level with test/07stream/.
    // Venice sent a cost object on every family swept for VC-20, so the shape
    // below is constructed rather than observed; see §4b's provenance note.
    const auto body = nlohmann::json::parse(
        R"({"choices":[{"message":{"content":"hi"},"finish_reason":"stop"}]})");
    auto r = ChatResponse::from_json_body(body);
    REQUIRE_FALSE(r.cost.has_value());
  }
}

// ── Usage cache buckets stay distinct (venice-cli #75) ───────────────────

TEST_CASE("Usage keeps cached_tokens distinct when reported", "[parse][usage]") {
  // The FLAT spelling, which VC-17's 21-capture sweep did not see Venice use
  // once — the wire location is prompt_tokens_details.cached_tokens, pinned in
  // test/07stream/ §4 against real captures. This case is what makes the
  // compatibility read a contract rather than an accident; it is not evidence
  // about the API. Read §4's provenance note before treating it as such.
  const auto j = nlohmann::json::parse(
      R"({"prompt_tokens":100,"completion_tokens":10,"total_tokens":110,"cached_tokens":80})");
  auto u = j.get<Usage>();
  REQUIRE(u.prompt_tokens == 100);
  REQUIRE(u.cached_tokens.has_value());
  REQUIRE(*u.cached_tokens == 80);
}

// ── Error model: status → kind mapping ───────────────────────────────────

TEST_CASE("kind_for_status maps auth payment and rate-limit distinctly", "[error]") {
  REQUIRE(venice::kind_for_status(401) == ErrorKind::Auth);
  REQUIRE(venice::kind_for_status(402) == ErrorKind::PaymentRequired);
  REQUIRE(venice::kind_for_status(403) == ErrorKind::Auth);
  REQUIRE(venice::kind_for_status(429) == ErrorKind::RateLimited);
  REQUIRE(venice::kind_for_status(500) == ErrorKind::Http);
  REQUIRE(venice::kind_for_status(200) == ErrorKind::Http);  // non-error path maps generic
  REQUIRE(venice::to_string(ErrorKind::PaymentRequired) == "payment_required");
}

TEST_CASE("authentication modes emit one canonical transport header", "[auth]") {
  const auto public_headers =
      venice::detail::authentication_headers(Authentication::public_access());
  REQUIRE(public_headers.has_value());
  REQUIRE(public_headers->empty());

  const auto bearer = venice::detail::authentication_headers(Authentication::bearer("api-token"));
  REQUIRE(bearer.has_value());
  REQUIRE(bearer->size() == 1);
  const auto bearer_header = bearer->find("Authorization");
  REQUIRE(bearer_header != bearer->end());
  REQUIRE(bearer_header->second == "Bearer api-token");

  const auto siwx =
      venice::detail::authentication_headers(Authentication::sign_in_with_x("signed-wallet"));
  REQUIRE(siwx.has_value());
  REQUIRE(siwx->size() == 1);
  const auto siwx_header = siwx->find("SIGN-IN-WITH-X");
  REQUIRE(siwx_header != siwx->end());
  REQUIRE(siwx_header->second == "signed-wallet");
  REQUIRE(siwx->find("X-Sign-In-With-X") == siwx->end());

  const auto payment =
      venice::detail::authentication_headers(Authentication::x402_payment("payment-payload"));
  REQUIRE(payment.has_value());
  REQUIRE(payment->size() == 1);
  const auto payment_header = payment->find("PAYMENT-SIGNATURE");
  REQUIRE(payment_header != payment->end());
  REQUIRE(payment_header->second == "payment-payload");
  REQUIRE(payment->find("X-402-Payment") == payment->end());
}

TEST_CASE("empty secret-bearing authentication modes fail without disclosing a value",
          "[auth][failure]") {
  for (const auto& authentication : {Authentication::bearer({}),
                                     Authentication::sign_in_with_x({}),
                                     Authentication::x402_payment({})}) {
    const auto headers = venice::detail::authentication_headers(authentication);
    REQUIRE_FALSE(headers.has_value());
    REQUIRE(headers.error().kind == ErrorKind::InvalidArg);
    REQUIRE(headers.error().status == 0);
    REQUIRE(headers.error().body.empty());
    REQUIRE(headers.error().metadata.headers.empty());
  }
}

TEST_CASE("response metadata preserves raw headers and extracts x402 values case-insensitively",
          "[metadata]") {
  const httplib::Headers headers{{"x-balance-remaining", "4.230000"},
                                 {"Payment-Required", "requirements"},
                                 {"PAYMENT-RESPONSE", "settlement"},
                                 {"X-Trace", "raw"}};
  const auto metadata = venice::detail::metadata_from_headers(headers);

  REQUIRE(metadata.headers.size() == 4);
  REQUIRE(metadata.x_balance_remaining == "4.230000");
  REQUIRE(metadata.payment_required == "requirements");
  REQUIRE(metadata.payment_response == "settlement");
  REQUIRE(metadata.header("x-trace") == "raw");
  REQUIRE(metadata.header("X-BaLaNcE-ReMaInInG") == "4.230000");
  REQUIRE_FALSE(metadata.header("missing").has_value());
}

TEST_CASE("Error carries status and body for inspection", "[error]") {
  Error e{ErrorKind::RateLimited, 429, "HTTP 429", "{\"detail\":\"slow down\"}"};
  REQUIRE(e.is(ErrorKind::RateLimited));
  REQUIRE(e.status == 429);
  REQUIRE(e.body.find("slow down") != std::string::npos);
}

// ── Generated version header ──────────────────────────────────────────────
// include/version.hpp is produced by configure_file from version.hpp.in.cmake,
// with the values cmake/version_parse.cmake derives from `git describe`. Nothing
// else in the tree includes it, so without this case the whole version pipeline
// could emit a header that does not compile — or one whose fields are silently
// empty-brace-initialized — and every build would still be green.
TEST_CASE("generated version header is well-formed", "[version]") {
  // The name is the one field with a knowable value, and it proves
  // configure_file actually substituted rather than emitting the raw @TOKEN@.
  REQUIRE(PROGRAM_NAME == "venice-cpp");

  // The numeric fields have no assertable value — 0.0.0 is correct on an
  // untagged clone and 0.1.0 after the release tag. What is worth pinning is
  // the header's contract, which downstream code compiles against: these are
  // constexpr integers of a fixed width, not whatever a future edit makes them.
  STATIC_REQUIRE(std::is_same_v<decltype(VERSION_MAJOR), const std::uint32_t>);
  STATIC_REQUIRE(std::is_same_v<decltype(VERSION_TWEAK), const std::uint32_t>);
  STATIC_REQUIRE(std::is_same_v<decltype(VERSION_DIRTY), const bool>);
  STATIC_REQUIRE(VERSION_MAJOR == VERSION_MAJOR);  // usable in a constant expression
}
