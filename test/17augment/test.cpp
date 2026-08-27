// Augment request/response contracts (VC-32, #47).
//
// Pure serialization, guards and parsing live here. Exact HTTP targets,
// multipart preservation, auth, errors and media classification use the
// loopback peer in test/06transport/. Failure matrix first, happy paths last.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <variant>

#include <nlohmann/json.hpp>

#include <venice/venice.hpp>

namespace {

const venice::Client kBearer{"not-a-real-key"};

auto document_file() -> venice::DocumentFile {
  return {.bytes = std::string{"text\0bytes", 10},
          .filename = "fixture.txt",
          .media_type = "text/plain"};
}

} // namespace

TEST_CASE("augment parsers reject malformed response structure",
          "[augment][parse][failure]") {
  SECTION("document JSON requires an object, text and numeric tokens") {
    REQUIRE_THROWS(
        venice::document_parse_from_json_body(nlohmann::json::array()));
    REQUIRE_THROWS(venice::document_parse_from_json_body(
        nlohmann::json{{"text", 7}, {"tokens", 1}}));
    REQUIRE_THROWS(venice::document_parse_from_json_body(
        nlohmann::json{{"text", "hello"}}));
    REQUIRE_THROWS(venice::document_parse_from_json_body(
        nlohmann::json{{"text", "hello"}, {"tokens", "many"}}));
  }
  SECTION("scrape requires every stable display field") {
    REQUIRE_THROWS(venice::web_scrape_from_json_body(nlohmann::json::array()));
    REQUIRE_THROWS(venice::web_scrape_from_json_body(
        nlohmann::json{{"url", "https://example.test"}, {"content", "body"}}));
    REQUIRE_THROWS(venice::web_scrape_from_json_body(nlohmann::json{
        {"url", 7}, {"content", "body"}, {"format", "markdown"}}));
  }
  SECTION("search requires an object and a results array") {
    REQUIRE_THROWS(venice::web_search_from_json_body(nlohmann::json::array()));
    REQUIRE_THROWS(
        venice::web_search_from_json_body(nlohmann::json{{"query", "q"}}));
    REQUIRE_THROWS(venice::web_search_from_json_body(
        nlohmann::json{{"query", "q"}, {"results", nlohmann::json::object()}}));
  }
}

TEST_CASE("augment guards reject only structurally unsendable values",
          "[augment][guards][failure]") {
  venice::DocumentParseRequest parse{.file = document_file()};
  parse.file.bytes.clear();
  REQUIRE(kBearer.parse_document(parse).error().message ==
          "document file bytes are empty");
  parse.file = document_file();
  parse.file.filename.clear();
  REQUIRE(kBearer.parse_document(parse).error().message ==
          "document file name is empty");
  parse.file = document_file();
  parse.file.media_type.clear();
  REQUIRE(kBearer.parse_document(parse).error().message ==
          "document file media type is empty");

  REQUIRE(kBearer.scrape_web({}).error().message == "web scrape URL is empty");
  REQUIRE(kBearer.search_web({}).error().message ==
          "web search query is empty");
}

TEST_CASE(
    "augment JSON requests are modeled-wins and preserve server-owned values",
    "[augment][request]") {
  const venice::WebScrapeRequest scrape{
      .url = "future://caller-owned",
      .extra = {{"url", "shadow"}, {"future", true}},
  };
  REQUIRE(scrape.to_json_body()["url"] == "future://caller-owned");
  REQUIRE(scrape.to_json_body()["future"] == true);

  const venice::WebSearchRequest search{
      .query = "query",
      .limit = 0,
      .search_provider = "future-provider",
      .extra = {{"query", "shadow"},
                {"limit", 99},
                {"search_provider", "shadow"},
                {"future", 1}},
  };
  const auto body = search.to_json_body();
  REQUIRE(body["query"] == "query");
  REQUIRE(body["limit"] == 0);
  REQUIRE(body["search_provider"] == "future-provider");
  REQUIRE(body["future"] == 1);

  const venice::WebSearchRequest minimal{.query = "q",
                                         .extra = nlohmann::json::array({1})};
  REQUIRE(minimal.to_json_body() == nlohmann::json{{"query", "q"}});
}

TEST_CASE(
    "augment response parsers preserve exact values and raw provider data",
    "[augment][parse]") {
  const auto parsed = venice::document_parse_from_json_body(
      nlohmann::json{{"text", "hello"}, {"tokens", 1.5}, {"future", true}});
  REQUIRE(parsed.text == "hello");
  REQUIRE(parsed.tokens == 1.5);
  REQUIRE(parsed.raw["future"] == true);

  const auto scraped = venice::web_scrape_from_json_body(
      nlohmann::json{{"url", "https://example.test"},
                     {"content", "# Example"},
                     {"format", "future-format"},
                     {"future", 1}});
  REQUIRE(scraped.url == "https://example.test");
  REQUIRE(scraped.content == "# Example");
  REQUIRE(scraped.format == "future-format");
  REQUIRE(scraped.raw["future"] == 1);
}

TEST_CASE("search listings degrade individual fields without losing siblings",
          "[augment][search][parse]") {
  const auto body = nlohmann::json::parse(R"({
    "query":"q",
    "results":[
      {"title":"first","url":"https://one.test","content":"one","date":"today","future":1},
      {"title":7,"url":"https://two.test","content":null},
      "junk",
      {"title":"last","url":"","content":"three","date":""}
    ],
    "future_envelope":true
  })");
  const auto response = venice::web_search_from_json_body(body);
  REQUIRE(response.query == "q");
  REQUIRE(response.returned == 4);
  REQUIRE(response.results.size() == 3);
  REQUIRE(response.results[0].title == "first");
  REQUIRE(response.results[0].raw["future"] == 1);
  REQUIRE_FALSE(response.results[1].title.has_value());
  REQUIRE(response.results[1].url == "https://two.test");
  REQUIRE_FALSE(response.results[1].content.has_value());
  REQUIRE(response.results[2].url == "");
  REQUIRE(response.results[2].date == "");
  REQUIRE(response.raw == body);

  const auto odd_query = venice::web_search_from_json_body(
      nlohmann::json{{"query", 7}, {"results", nlohmann::json::array()}});
  REQUIRE_FALSE(odd_query.query.has_value());
}
