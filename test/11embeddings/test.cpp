// /embeddings request and response contract (VC-26, #41).
//
// Failure matrix first. Pure serialization and parsing live here; the exact
// HTTP target, authentication, metadata and status classification use the
// loopback peer in test/06transport/.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include <venice/venice.hpp>

namespace {

auto good_body() -> nlohmann::json {
  return nlohmann::json::parse(R"({
    "data":[
      {"embedding":[0.25,-1,2.5],"index":1,"object":"embedding","future":"kept"},
      {"embedding":"AQIDBA==","index":0,"object":"embedding"}
    ],
    "model":"text-embedding-test",
    "object":"list",
    "usage":{"prompt_tokens":4,"total_tokens":4,"future_usage":7},
    "future_envelope":{"kept":true}
  })");
}

auto minimal_request() -> venice::EmbeddingRequest {
  venice::EmbeddingRequest request;
  request.model = "text-embedding-test";
  request.input = venice::embedding_input::text("hello");
  return request;
}

const venice::Client kClient{"not-a-real-key"};

}  // namespace

TEST_CASE("embedding response rejects a malformed envelope", "[embeddings][parse][failure]") {
  SECTION("top level must be an object") {
    REQUIRE_THROWS(venice::embeddings_from_json_body(nlohmann::json::array()));
  }
  SECTION("data is required and must be an array") {
    auto body = good_body();
    body.erase("data");
    REQUIRE_THROWS(venice::embeddings_from_json_body(body));
    body["data"] = nlohmann::json::object();
    REQUIRE_THROWS(venice::embeddings_from_json_body(body));
  }
  SECTION("usage is required and must be an object") {
    auto body = good_body();
    body["usage"] = nullptr;
    REQUIRE_THROWS(venice::embeddings_from_json_body(body));
  }
  SECTION("model and object are required strings") {
    auto body = good_body();
    body["model"] = 7;
    REQUIRE_THROWS(venice::embeddings_from_json_body(body));
    body = good_body();
    body.erase("object");
    REQUIRE_THROWS(venice::embeddings_from_json_body(body));
  }
}

TEST_CASE("embedding response rejects corrupt ordering and accounting fields",
          "[embeddings][parse][failure]") {
  SECTION("an entry must be an object") {
    auto body = good_body();
    body["data"][0] = "not an entry";
    REQUIRE_THROWS(venice::embeddings_from_json_body(body));
  }
  SECTION("index must be a representable integer") {
    auto body = good_body();
    body["data"][0]["index"] = 1.5;
    REQUIRE_THROWS(venice::embeddings_from_json_body(body));
    body = good_body();
    body["data"][0]["index"] = 99999999999999LL;
    REQUIRE_THROWS(venice::embeddings_from_json_body(body));
  }
  SECTION("usage counts must be representable integers") {
    auto body = good_body();
    body["usage"]["prompt_tokens"] = "four";
    REQUIRE_THROWS(venice::embeddings_from_json_body(body));
    body = good_body();
    body["usage"]["total_tokens"] = 1.9;
    REQUIRE_THROWS(venice::embeddings_from_json_body(body));
  }
}

TEST_CASE("embedding values are either numeric arrays or opaque base64 strings",
          "[embeddings][parse][failure]") {
  SECTION("embedding is required") {
    auto body = good_body();
    body["data"][0].erase("embedding");
    REQUIRE_THROWS(venice::embeddings_from_json_body(body));
  }
  SECTION("wrong top-level value type is loud") {
    auto body = good_body();
    body["data"][0]["embedding"] = nlohmann::json::object();
    REQUIRE_THROWS(venice::embeddings_from_json_body(body));
  }
  SECTION("every numeric vector element must be numeric") {
    auto body = good_body();
    body["data"][0]["embedding"][1] = "minus one";
    REQUIRE_THROWS(venice::embeddings_from_json_body(body));
  }
}

TEST_CASE("embedding request guards reject only structural emptiness",
          "[embeddings][guards][failure]") {
  SECTION("model is required") {
    auto request = minimal_request();
    request.model.clear();
    const auto result = kClient.embeddings(request);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().is(venice::ErrorKind::InvalidArg));
    REQUIRE(result.error().message == "model is empty");
  }
  SECTION("input is required") {
    auto request = minimal_request();
    request.input = nullptr;
    const auto result = kClient.embeddings(request);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().is(venice::ErrorKind::InvalidArg));
    REQUIRE(result.error().message == "embedding input is missing");
  }
  SECTION("empty string is rejected") {
    auto request = minimal_request();
    request.input = venice::embedding_input::text("");
    const auto result = kClient.embeddings(request);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().message == "embedding input is empty");
  }
  SECTION("empty top-level array is rejected") {
    auto request = minimal_request();
    request.input = venice::embedding_input::texts({});
    const auto result = kClient.embeddings(request);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().message == "embedding input is empty");
  }
}

TEST_CASE("embedding input builders preserve all documented shapes", "[embeddings][request]") {
  REQUIRE(venice::embedding_input::text("one") == nlohmann::json("one"));
  REQUIRE(venice::embedding_input::texts({"one", "two"}) ==
          nlohmann::json::array({"one", "two"}));
  REQUIRE(venice::embedding_input::tokens({1212, 318, 257}) ==
          nlohmann::json::array({1212, 318, 257}));
  REQUIRE(venice::embedding_input::token_batches({{1, 2}, {3, 4}}) ==
          nlohmann::json::array({nlohmann::json::array({1, 2}),
                                 nlohmann::json::array({3, 4})}));
}

TEST_CASE("embedding request keeps raw reachability and modeled precedence",
          "[embeddings][request][failure]") {
  auto request = minimal_request();
  request.dimensions = -7;  // server-owned range policy is transmitted
  request.encoding_format = "future-format";
  request.user = "caller";
  request.input = nlohmann::json::object({{"future_input", true}});
  request.extra = nlohmann::json::parse(
      R"({"model":"shadow","input":"shadow","dimensions":999,"future":42})");

  const auto body = request.to_json_body();
  REQUIRE(body["model"] == "text-embedding-test");
  REQUIRE(body["input"] == nlohmann::json::object({{"future_input", true}}));
  REQUIRE(body["dimensions"] == -7);
  REQUIRE(body["encoding_format"] == "future-format");
  REQUIRE(body["user"] == "caller");
  REQUIRE(body["future"] == 42);

  request.extra = nlohmann::json::array({1, 2});
  REQUIRE(request.to_json_body().is_object());
}

TEST_CASE("embedding response preserves wire order, variants and raw", "[embeddings]") {
  const auto body = good_body();
  const auto response = venice::embeddings_from_json_body(body);

  REQUIRE(response.data.size() == 2);
  REQUIRE(response.data[0].index == 1);
  REQUIRE(response.data[1].index == 0);
  const auto* numbers = std::get_if<std::vector<double>>(&response.data[0].value);
  REQUIRE(numbers != nullptr);
  REQUIRE(*numbers == std::vector<double>{0.25, -1.0, 2.5});
  const auto* encoded = std::get_if<std::string>(&response.data[1].value);
  REQUIRE(encoded != nullptr);
  REQUIRE(*encoded == "AQIDBA==");
  REQUIRE(response.usage == venice::EmbeddingUsage{.prompt_tokens = 4, .total_tokens = 4});
  REQUIRE(response.raw == body);
  REQUIRE(response.data[0].raw["future"] == "kept");
}
