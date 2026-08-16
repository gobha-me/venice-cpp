// API-key request and response contracts (VC-43, #70).
//
// Fixtures are synthetic from Venice OpenAPI 20260814.194349. Complete key
// material is represented by an unmistakable non-credential marker and is
// never printed. Transport method/path/header behavior lives in 06transport.

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <string>

#include <nlohmann/json.hpp>

#include <venice/venice.hpp>

namespace {

auto key_object() -> nlohmann::json {
  return nlohmann::json{
      {"apiKeyType", "FUTURE_TYPE"},
      {"consumptionLimits", {{"usd", 0}, {"diem", nullptr}, {"vcu", 3.5}, {"future", 1}}},
      {"limitPeriod", "FUTURE_PERIOD"},
      {"createdAt", "2026-08-16T00:00:00Z"},
      {"description", "synthetic fixture"},
      {"expiresAt", nullptr},
      {"id", "synthetic-id"},
      {"last6Chars", "ABC123"},
      {"lastUsedAt", nullptr},
      {"usage", {{"trailingSevenDays", {{"usd", "0.0000"}, {"diem", "2.5000"}, {"vcu", "1"}}}}},
      {"currentPeriodUsage", {{"usd", "0"}, {"diem", "1.25"}}},
      {"future", true},
  };
}

const venice::Client kPublic{venice::Authentication::public_access()};

}  // namespace

TEST_CASE("API-key parsers reject malformed envelopes", "[api-keys][parse][failure]") {
  REQUIRE_THROWS(venice::api_keys_from_json_body(nlohmann::json::array()));
  REQUIRE_THROWS(venice::api_keys_from_json_body(nlohmann::json{{"data", nlohmann::json::object()}}));
  REQUIRE_THROWS(venice::api_key_from_json_body(nlohmann::json{{"data", nlohmann::json::array()}}));
  REQUIRE_THROWS(venice::api_key_created_from_json_body(nlohmann::json{{"success", true}, {"data", nlohmann::json::object()}}));
  REQUIRE_THROWS(venice::api_key_update_from_json_body(nlohmann::json{{"success", "yes"}, {"data", nlohmann::json::object()}}));
  REQUIRE_THROWS(venice::api_key_delete_from_json_body(nlohmann::json{{"success", 1}}));
  REQUIRE_THROWS(venice::api_key_rate_limits_from_json_body(nlohmann::json{{"data", nlohmann::json::array()}}));
  REQUIRE_THROWS(venice::api_key_rate_limit_logs_from_json_body(nlohmann::json{{"data", nlohmann::json::object()}}));
}

TEST_CASE("API-key listings retain absent zero malformed and raw states", "[api-keys][parse]") {
  const nlohmann::json body{
      {"data", nlohmann::json::array({key_object(), "future-entry"})},
      {"object", "list"},
      {"futureEnvelope", true},
  };
  const auto page = venice::api_keys_from_json_body(body);
  REQUIRE(page.returned == 2);
  REQUIRE(page.entries.size() == 2);
  REQUIRE(page.raw == body);

  const auto& key = page.entries.front();
  REQUIRE(key.api_key_type == "FUTURE_TYPE");
  REQUIRE(key.limit_period == "FUTURE_PERIOD");
  REQUIRE(key.consumption_limits);
  REQUIRE(key.consumption_limits->usd == 0.0);
  REQUIRE_FALSE(key.consumption_limits->diem);
  REQUIRE(key.consumption_limits->vcu == 3.5);
  REQUIRE(key.consumption_limits->raw["future"] == 1);
  REQUIRE_FALSE(key.expires_at);
  REQUIRE(key.usage->trailing_seven_days->usd == "0.0000");
  REQUIRE(key.current_period_usage->diem == "1.25");
  REQUIRE(key.raw["future"] == true);
  REQUIRE(page.entries.back().raw == "future-entry");
  REQUIRE_FALSE(page.entries.back().id);

  const nlohmann::json detail{{"data", key_object()}, {"futureEnvelope", 7}};
  const auto one = venice::api_key_from_json_body(detail);
  REQUIRE(one.id == "synthetic-id");
  REQUIRE(one.raw == detail["data"]);
  REQUIRE(one.envelope_raw == detail);
}

TEST_CASE("API-key requests emit engaged fields and modeled fields win", "[api-keys][request]") {
  venice::ApiKeyCreateRequest create{
      .api_key_type = "FUTURE_TYPE",
      .description = "fixture",
      .consumption_limit = venice::ApiKeyConsumptionLimitRequest{
          .usd = 0.0,
          .extra = {{"usd", 999}, {"futureLimit", true}},
      },
      .limit_period = "FUTURE_PERIOD",
      .extra = {{"apiKeyType", "shadow"}, {"description", "shadow"}, {"future", 1}},
  };
  const auto created = create.to_json_body();
  REQUIRE(created["apiKeyType"] == "FUTURE_TYPE");
  REQUIRE(created["description"] == "fixture");
  REQUIRE(created["limitPeriod"] == "FUTURE_PERIOD");
  REQUIRE(created["consumptionLimit"]["usd"] == 0.0);
  REQUIRE(created["consumptionLimit"]["futureLimit"] == true);
  REQUIRE(created["future"] == 1);
  REQUIRE_FALSE(created.contains("expiresAt"));

  venice::ApiKeyUpdateRequest update{
      .id = "synthetic-id",
      .description = "",
      .expires_at = "",
      .extra = {{"id", "shadow"}, {"expiresAt", nullptr}, {"future", true}},
  };
  const auto cleared = update.to_json_body();
  REQUIRE(cleared["id"] == "synthetic-id");
  REQUIRE(cleared["description"] == "");
  REQUIRE(cleared["expiresAt"] == "");
  REQUIRE(cleared["future"] == true);

  update.expires_at.reset();
  const auto raw_null = update.to_json_body();
  REQUIRE(raw_null["expiresAt"].is_null());

  create.extra = nlohmann::json::array({1});
  create.consumption_limit->extra = "not-an-object";
  const auto guarded = create.to_json_body();
  REQUIRE(guarded.is_object());
  REQUIRE_FALSE(guarded.contains("future"));
  REQUIRE(guarded["consumptionLimit"] == nlohmann::json{{"usd", 0.0}});
}

TEST_CASE("create update and delete results keep success and secret boundaries", "[api-keys][parse]") {
  const nlohmann::json created_body{
      {"data", {{"apiKey", "SYNTHETIC_SECRET_RETURNED_ONCE"},
                {"apiKeyType", "INFERENCE"},
                {"consumptionLimit", {{"usd", 0}}},
                {"limitPeriod", "EPOCH"},
                {"expiresAt", nullptr},
                {"id", "synthetic-created-id"},
                {"future", 9}}},
      {"success", true},
      {"futureEnvelope", true},
  };
  const auto created = venice::api_key_created_from_json_body(created_body);
  REQUIRE(created.api_key == "SYNTHETIC_SECRET_RETURNED_ONCE");
  REQUIRE(created.id == "synthetic-created-id");
  REQUIRE(created.success);
  REQUIRE(created.consumption_limit->usd == 0.0);
  REQUIRE(created.raw["data"]["apiKey"] == "[REDACTED]");
  REQUIRE(created.raw.dump().find("SYNTHETIC_SECRET_RETURNED_ONCE") ==
          std::string::npos);
  REQUIRE(created.raw["futureEnvelope"] == true);

  const nlohmann::json updated_body{{"data", key_object()}, {"success", false}};
  const auto updated = venice::api_key_update_from_json_body(updated_body);
  REQUIRE_FALSE(updated.success);
  REQUIRE(updated.key.id == "synthetic-id");
  REQUIRE(updated.raw == updated_body);

  const auto deleted = venice::api_key_delete_from_json_body(
      nlohmann::json{{"success", false}, {"future", 1}});
  REQUIRE_FALSE(deleted.success);
  REQUIRE(deleted.raw["future"] == 1);
}

TEST_CASE("rate-limit responses retain nested unknown and last-50 log order", "[api-keys][rate-limits]") {
  const nlohmann::json limits_body{
      {"data", {{"accessPermitted", false},
                {"apiTier", {{"id", "future-tier"}, {"isCharged", true}, {"future", 1}}},
                {"balances", {{"USD", 0}, {"DIEM", 2.5}, {"future", 7}}},
                {"keyExpiration", nullptr},
                {"nextEpochBegins", "later"},
                {"rateLimits", nlohmann::json::array({
                    {{"apiModelId", "model-a"},
                     {"rateLimits", nlohmann::json::array({{{"amount", 0}, {"type", "FUTURE"}, {"future", true}}, "future-limit"})}},
                    "future-group"})},
                {"future", true}}},
      {"futureEnvelope", true},
  };
  const auto limits = venice::api_key_rate_limits_from_json_body(limits_body);
  REQUIRE(limits.access_permitted == false);
  REQUIRE(limits.api_tier->id == "future-tier");
  REQUIRE(limits.balances->usd == 0.0);
  REQUIRE_FALSE(limits.key_expiration);
  REQUIRE(limits.rate_limits->size() == 2);
  REQUIRE(limits.rate_limits->front().rate_limits->front().amount == 0.0);
  REQUIRE(limits.rate_limits->front().rate_limits->front().type == "FUTURE");
  REQUIRE(limits.rate_limits->back().raw == "future-group");
  REQUIRE(limits.raw == limits_body);

  const nlohmann::json logs_body{
      {"data", nlohmann::json::array({
                   {{"apiKeyId", "key-a"}, {"modelId", "m"}, {"rateLimitTier", "paid"},
                    {"rateLimitType", "FUTURE"}, {"timestamp", "first"}, {"future", 1}},
                   "future-entry",
                   {{"apiKeyId", "key-b"}, {"timestamp", "last"}}})},
      {"object", "list"},
  };
  const auto logs = venice::api_key_rate_limit_logs_from_json_body(logs_body);
  REQUIRE(logs.returned == 3);
  REQUIRE(logs.entries.size() == 3);
  REQUIRE(logs.entries.front().timestamp == "first");
  REQUIRE(logs.entries[1].raw == "future-entry");
  REQUIRE(logs.entries.back().timestamp == "last");
}

TEST_CASE("API-key guards reject structural and non-representable input only", "[api-keys][guards][failure]") {
  REQUIRE_FALSE(kPublic.api_key(""));
  REQUIRE_FALSE(kPublic.delete_api_key(""));
  REQUIRE_FALSE(kPublic.update_api_key({.id = ""}));

  const double nan = std::numeric_limits<double>::quiet_NaN();
  const auto bad_create = kPublic.create_api_key({
      .api_key_type = "FUTURE",
      .description = "fixture",
      .consumption_limit = venice::ApiKeyConsumptionLimitRequest{.usd = nan},
  });
  REQUIRE_FALSE(bad_create);
  REQUIRE(bad_create.error().message == "consumption_limit.usd is not finite");

  const auto bad_update = kPublic.update_api_key({
      .id = "synthetic-id",
      .consumption_limit = venice::ApiKeyConsumptionLimitRequest{
          .diem = std::numeric_limits<double>::infinity()},
  });
  REQUIRE_FALSE(bad_update);
  REQUIRE(bad_update.error().message == "consumption_limit.diem is not finite");

  // Finite out-of-policy values and future strings pass local validation;
  // Public auth then wins before a socket and proves no range enum was added.
  const auto pass = kPublic.create_api_key({
      .api_key_type = "FUTURE",
      .description = "",
      .consumption_limit = venice::ApiKeyConsumptionLimitRequest{.usd = -1.0},
      .limit_period = "FUTURE",
  });
  REQUIRE_FALSE(pass);
  REQUIRE(pass.error().message == "endpoint requires Bearer authentication");
}
