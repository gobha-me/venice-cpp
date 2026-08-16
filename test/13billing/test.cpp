// Billing response and query contracts (VC-42, #68).
//
// Fixtures are synthetic from Venice OpenAPI 20260814.194349 and the structural
// live capture of 2026-08-16. No account values are persisted here. Failure
// matrix first; loopback HTTP/media behavior lives in test/06transport/.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <venice/venice.hpp>

namespace {

auto analytics_body() -> nlohmann::json {
  return nlohmann::json::parse(R"({
    "lookback":"7d",
    "byDate":[{"date":"2026-08-15","USD":0,"DIEM":2.5},"future-row"],
    "byModel":[{
      "modelName":"Example Model","unitType":"tokens","modelType":"LLM",
      "totalUsd":1,"totalDiem":2.25,"totalUnits":300,
      "breakdown":[{"type":"Input","usd":0.25,"diem":1,"units":100},7]
    }],
    "byModelDaily":[{"date":1786752000000,"Example Model":2.25}],
    "byModelDailyUsd":[{"date":1786752000000,"Example Model":1.25}],
    "topModels":["Example Model"],
    "byKey":[{
      "apiKeyId":null,"description":"Web App","totalUsd":0,
      "totalDiem":2.25,"totalUnits":300
    }],
    "byKeyDaily":[{"date":1786752000000,"Web App":2.25}],
    "byKeyDailyUsd":[{"date":1786752000000,"Web App":1.25}],
    "topKeyNames":["Web App"],
    "future":{"kept":true}
  })");
}

auto history_body() -> nlohmann::json {
  return nlohmann::json::parse(R"({
    "data":[
      {
        "amount":-0.06356,"currency":"DIEM",
        "inferenceDetails":{
          "completionTokens":227,"inferenceExecutionTime":2964,
          "promptTokens":339,"requestId":"synthetic-request"
        },
        "notes":"API Inference","pricePerUnitUsd":2.8,
        "sku":"example-output-mtoken","timestamp":"2026-08-15T19:05:10.504Z",
        "units":0.000227,"future":"kept"
      },
      {
        "amount":0,"currency":"USD","inferenceDetails":null,
        "notes":"zero is a value","pricePerUnitUsd":0,"sku":"example",
        "timestamp":"2026-08-15T20:00:00Z","units":1
      },
      "future-entry"
    ],
    "nextCursor":"synthetic_cursor",
    "future_envelope":true
  })");
}

const venice::Client kClient{"not-a-real-key"};
const venice::Client kPublic{venice::Authentication::public_access()};

}  // namespace

TEST_CASE("billing parsers reject malformed top-level contracts",
          "[billing][parse][failure]") {
  REQUIRE_THROWS(venice::billing_balance_from_json_body(nlohmann::json::array()));
  REQUIRE_THROWS(
      venice::billing_usage_analytics_from_json_body(nlohmann::json("analytics")));
  REQUIRE_THROWS(venice::billing_usage_history_from_json_body(nlohmann::json::array()));

  auto history = history_body();
  history.erase("data");
  REQUIRE_THROWS(venice::billing_usage_history_from_json_body(history));
  history = history_body();
  history["data"] = nlohmann::json::object();
  REQUIRE_THROWS(venice::billing_usage_history_from_json_body(history));
  history = history_body();
  history.erase("nextCursor");
  REQUIRE_THROWS(venice::billing_usage_history_from_json_body(history));
  history = history_body();
  history["nextCursor"] = 7;
  REQUIRE_THROWS(venice::billing_usage_history_from_json_body(history));
}

TEST_CASE("billing balance distinguishes absent, null and zero", "[billing][balance]") {
  const nlohmann::json body{{"canConsume", false},
                            {"consumptionCurrency", nullptr},
                            {"balances", {{"diem", 0}, {"usd", nullptr}, {"future", 4}}},
                            {"diemEpochAllocation", 100},
                            {"future", true}};
  const auto balance = venice::billing_balance_from_json_body(body);
  REQUIRE(balance.can_consume == false);
  REQUIRE_FALSE(balance.consumption_currency);
  REQUIRE(balance.balances);
  REQUIRE(balance.balances->diem == 0.0);
  REQUIRE_FALSE(balance.balances->usd);
  REQUIRE(balance.diem_epoch_allocation == 100.0);
  REQUIRE(balance.balances->raw["future"] == 4);
  REQUIRE(balance.raw == body);

  const auto degraded = venice::billing_balance_from_json_body(
      nlohmann::json{{"canConsume", "yes"}, {"balances", nlohmann::json::array()}});
  REQUIRE_FALSE(degraded.can_consume);
  REQUIRE_FALSE(degraded.balances);

  const auto large = venice::billing_balance_from_json_body(
      nlohmann::json{{"balances", {{"diem", 1e100}}}, {"diemEpochAllocation", 1e100}});
  REQUIRE(large.balances->diem == 1e100);
  REQUIRE(large.diem_epoch_allocation == 1e100);
}

TEST_CASE("billing analytics keeps unknown states and dynamic maps",
          "[billing][analytics]") {
  const auto body = analytics_body();
  const auto analytics = venice::billing_usage_analytics_from_json_body(body);

  REQUIRE(analytics.lookback == "7d");
  REQUIRE(analytics.by_date);
  REQUIRE(analytics.by_date->size() == 2);
  REQUIRE(analytics.by_date->front().usd == 0.0);
  REQUIRE(analytics.by_date->back().raw == "future-row");
  REQUIRE_FALSE(analytics.by_date->back().date);

  REQUIRE(analytics.by_model);
  REQUIRE(analytics.by_model->front().total_usd == 1.0);
  REQUIRE(analytics.by_model->front().breakdown);
  REQUIRE(analytics.by_model->front().breakdown->size() == 2);
  REQUIRE(analytics.by_model->front().breakdown->back().raw == 7);

  REQUIRE(analytics.by_model_daily);
  REQUIRE(analytics.by_model_daily->front()["Example Model"] == 2.25);
  REQUIRE(analytics.by_model_daily_usd);
  REQUIRE(analytics.by_model_daily_usd->front()["Example Model"] == 1.25);
  REQUIRE(analytics.by_key);
  REQUIRE_FALSE(analytics.by_key->front().api_key_id);
  REQUIRE(analytics.by_key_daily->front()["Web App"] == 2.25);
  REQUIRE(analytics.by_key_daily_usd);
  REQUIRE(analytics.by_key_daily_usd->front()["Web App"] == 1.25);
  REQUIRE(analytics.raw == body);

  const auto partial = venice::billing_usage_analytics_from_json_body(
      nlohmann::json{{"byDate", nlohmann::json::array()}, {"byModel", "wrong"}});
  REQUIRE(partial.by_date);
  REQUIRE(partial.by_date->empty());
  REQUIRE_FALSE(partial.by_model);
  REQUIRE_FALSE(partial.top_models);
}

TEST_CASE("billing history preserves order, raw entries and optional accounting",
          "[billing][history]") {
  const auto body = history_body();
  const auto page = venice::billing_usage_history_from_json_body(body);

  REQUIRE(page.returned == 3);
  REQUIRE(page.entries.size() == 3);
  REQUIRE(page.next_cursor == "synthetic_cursor");
  REQUIRE(page.entries[0].amount == -0.06356);
  REQUIRE(page.entries[0].inference_details);
  REQUIRE(page.entries[0].inference_details->completion_tokens == 227);
  REQUIRE(page.entries[0].inference_details->request_id == "synthetic-request");
  REQUIRE(page.entries[0].raw["future"] == "kept");
  REQUIRE(page.entries[1].amount == 0.0);
  REQUIRE_FALSE(page.entries[1].inference_details);
  REQUIRE(page.entries[2].raw == "future-entry");
  REQUIRE_FALSE(page.entries[2].amount);
  REQUIRE(page.raw == body);

  auto last = history_body();
  last["nextCursor"] = nullptr;
  REQUIRE_FALSE(venice::billing_usage_history_from_json_body(last).next_cursor);

  auto wrong = history_body();
  wrong["data"][0]["amount"] = "charged";
  wrong["data"][0]["inferenceDetails"]["promptTokens"] = 1.5;
  const auto degraded = venice::billing_usage_history_from_json_body(wrong);
  REQUIRE_FALSE(degraded.entries[0].amount);
  REQUIRE_FALSE(degraded.entries[0].inference_details->prompt_tokens);
}

TEST_CASE("billing query builders omit unset values and keep server-owned policy",
          "[billing][query]") {
  REQUIRE(venice::billing_usage_analytics_query_params({}).empty());
  const auto analytics = venice::billing_usage_analytics_query_params({
      .lookback = "future-window",
      .start_date = "not-client-validated",
      .end_date = "still-sent",
      .extra = {{"future", "a b"}, {"lookback", "shadow"}},
  });
  REQUIRE(analytics ==
          std::vector<std::pair<std::string, std::string>>{{"lookback", "future-window"},
                                                           {"startDate", "not-client-validated"},
                                                           {"endDate", "still-sent"},
                                                           {"future", "a b"}});

  const auto history = venice::billing_usage_history_query_params({
      .currency = "FUTURE",
      .end_timestamp = "later",
      .page_size = -7,
      .start_timestamp = "earlier",
      .extra = {{"future", "x"}, {"pageSize", "shadow"}},
  });
  REQUIRE(history ==
          std::vector<std::pair<std::string, std::string>>{{"currency", "FUTURE"},
                                                           {"endTimestamp", "later"},
                                                           {"pageSize", "-7"},
                                                           {"startTimestamp", "earlier"},
                                                           {"future", "x"}});
}

TEST_CASE("billing guards reject only structurally conflicting queries",
          "[billing][guards][failure]") {
  const auto half_dates = kClient.billing_usage_analytics({.start_date = "2026-08-01"});
  REQUIRE_FALSE(half_dates);
  REQUIRE(half_dates.error().is(venice::ErrorKind::InvalidArg));
  REQUIRE(half_dates.error().message.find("must be paired") != std::string::npos);

  const auto empty_cursor = kClient.billing_usage_history({.query = {.cursor = ""}});
  REQUIRE_FALSE(empty_cursor);
  REQUIRE(empty_cursor.error().message == "billing usage cursor must not be empty");

  const auto mixed_cursor = kClient.billing_usage_history(
      {.query = {.currency = "USD", .cursor = "next"}});
  REQUIRE_FALSE(mixed_cursor);
  REQUIRE(mixed_cursor.error().message.find("first-page filters") != std::string::npos);

  const auto extra_cursor = kClient.billing_usage_history(
      {.query = {.cursor = "next", .extra = {{"future", "value"}}}});
  REQUIRE_FALSE(extra_cursor);
  REQUIRE(extra_cursor.error().message.find("first-page filters") != std::string::npos);

  // Both date values and out-of-range/server-future values pass the structural
  // guards. Public auth then fails before a socket, proving which guard won.
  const auto paired = kPublic.billing_usage_analytics(
      {.lookback = "future", .start_date = "x", .end_date = "y"});
  REQUIRE_FALSE(paired);
  REQUIRE(paired.error().message == "endpoint requires Bearer authentication");
  const auto range = kPublic.billing_usage_history({.query = {.page_size = -7}});
  REQUIRE_FALSE(range);
  REQUIRE(range.error().message == "endpoint requires Bearer authentication");
}
