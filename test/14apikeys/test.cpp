// API-key request and response contracts (VC-43/#70 and VC-44/#72).
//
// Fixtures are synthetic from Venice OpenAPI 20260826.105305. Complete key
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
      {"consumptionLimits",
       {{"usd", 0}, {"diem", nullptr}, {"vcu", 3.5}, {"future", 1}}},
      {"limitPeriod", "FUTURE_PERIOD"},
      {"modelPrivacy", "FUTURE_PRIVACY"},
      {"createdAt", "2026-08-16T00:00:00Z"},
      {"description", "synthetic fixture"},
      {"expiresAt", nullptr},
      {"id", "synthetic-id"},
      {"last6Chars", "ABC123"},
      {"lastUsedAt", nullptr},
      {"usage",
       {{"trailingSevenDays",
         {{"usd", "0.0000"}, {"diem", "2.5000"}, {"vcu", "1"}}}}},
      {"currentPeriodUsage", {{"usd", "0"}, {"diem", "1.25"}}},
      {"future", true},
  };
}

const venice::Client kPublic{venice::Authentication::public_access(),
                             "http://127.0.0.1:1/api/v1"};
const venice::Client kBearer{venice::Authentication::bearer("synthetic-bearer"),
                             "http://127.0.0.1:1/api/v1"};

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
  REQUIRE_THROWS(venice::web3_api_key_challenge_from_json_body(nlohmann::json::array()));
  REQUIRE_THROWS(venice::web3_api_key_challenge_from_json_body(
      nlohmann::json{{"success", true}, {"data", nlohmann::json::array()}}));
  REQUIRE_THROWS(venice::web3_api_key_challenge_from_json_body(
      nlohmann::json{{"success", "yes"}, {"data", {{"token", "secret"}}}}));
  REQUIRE_THROWS(venice::web3_api_key_challenge_from_json_body(
      nlohmann::json{{"success", true}, {"data", {{"token", 7}}}}));
}

TEST_CASE("Web3 challenge parsing exposes one secret field and redacts every raw copy",
          "[api-keys][web3][parse][security]") {
  const nlohmann::json body{
      {"data", {{"token", "SYNTHETIC_CHALLENGE_SECRET"}, {"future", true}}},
      {"success", true},
      {"futureEnvelope",
       {{"signature", "SYNTHETIC_SIGNATURE_SECRET"},
        {"apiKey", "SYNTHETIC_API_KEY_SECRET"}}},
  };
  const auto challenge = venice::web3_api_key_challenge_from_json_body(body);
  REQUIRE(challenge.success);
  REQUIRE(challenge.token == "SYNTHETIC_CHALLENGE_SECRET");
  REQUIRE(challenge.raw["data"]["token"] == "[REDACTED]");
  REQUIRE(challenge.raw["data"]["future"] == true);
  REQUIRE(challenge.raw["futureEnvelope"]["signature"] == "[REDACTED]");
  REQUIRE(challenge.raw["futureEnvelope"]["apiKey"] == "[REDACTED]");
  REQUIRE(challenge.raw.dump().find("SYNTHETIC_CHALLENGE_SECRET") ==
          std::string::npos);
  REQUIRE(venice::detail::redacted_web3_api_key_body("not-json") ==
          "[REDACTED: non-JSON Web3 API-key response]");
}

TEST_CASE(
    "API-key response-body redaction fails closed for non-JSON diagnostics",
    "[api-keys][parse][security]") {
  const auto redacted = venice::detail::redacted_api_key_body(
      R"({"apiKey":"SYNTHETIC_TOP_LEVEL_SECRET","error":"useful diagnostic","nested":[{"apiKey":"SYNTHETIC_NESTED_SECRET"}]})");
  const auto parsed = nlohmann::json::parse(redacted);
  REQUIRE(parsed["error"] == "useful diagnostic");
  REQUIRE(parsed["apiKey"] == "[REDACTED]");
  REQUIRE(parsed["nested"][0]["apiKey"] == "[REDACTED]");
  REQUIRE(redacted.find("SYNTHETIC_") == std::string::npos);
  REQUIRE(venice::detail::redacted_api_key_body(
              "plain text containing SYNTHETIC_API_KEY_SECRET") ==
          "[REDACTED: non-JSON API-key response]");
  REQUIRE(venice::detail::redacted_api_key_body("").empty());
  REQUIRE(venice::detail::redacted_web3_api_key_body(
              "plain text containing SYNTHETIC_API_KEY_SECRET") ==
          "[REDACTED: non-JSON Web3 API-key response]");
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
  REQUIRE(key.model_privacy == "FUTURE_PRIVACY");
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
  REQUIRE(one.model_privacy == "FUTURE_PRIVACY");
  REQUIRE(one.raw == detail["data"]);
  REQUIRE(one.envelope_raw == detail);
}

TEST_CASE("API-key requests emit engaged fields and modeled fields win", "[api-keys][request]") {
  venice::ApiKeyCreateRequest create{
      .api_key_type = "FUTURE_TYPE",
      .description = "fixture",
      .consumption_limit =
          venice::ApiKeyConsumptionLimitRequest{
              .usd = 0.0,
              .extra = {{"usd", 999}, {"futureLimit", true}},
          },
      .limit_period = "FUTURE_PERIOD",
      .extra = {{"apiKeyType", "shadow"},
                {"description", "shadow"},
                {"modelPrivacy", "shadow"},
                {"future", 1}},
      .model_privacy = "FUTURE_PRIVACY",
  };
  const auto created = create.to_json_body();
  REQUIRE(created["apiKeyType"] == "FUTURE_TYPE");
  REQUIRE(created["description"] == "fixture");
  REQUIRE(created["limitPeriod"] == "FUTURE_PERIOD");
  REQUIRE(created["modelPrivacy"] == "FUTURE_PRIVACY");
  REQUIRE(created["consumptionLimit"]["usd"] == 0.0);
  REQUIRE(created["consumptionLimit"]["futureLimit"] == true);
  REQUIRE(created["future"] == 1);
  REQUIRE_FALSE(created.contains("expiresAt"));

  create.model_privacy.reset();
  create.extra.erase("modelPrivacy");
  REQUIRE_FALSE(create.to_json_body().contains("modelPrivacy"));

  venice::ApiKeyUpdateRequest update{
      .id = "synthetic-id",
      .description = "",
      .expires_at = "",
      .extra = {{"id", "shadow"},
                {"expiresAt", nullptr},
                {"modelPrivacy", "shadow"},
                {"future", true}},
      .model_privacy = "FUTURE_PRIVACY",
  };
  const auto cleared = update.to_json_body();
  REQUIRE(cleared["id"] == "synthetic-id");
  REQUIRE(cleared["description"] == "");
  REQUIRE(cleared["expiresAt"] == "");
  REQUIRE(cleared["modelPrivacy"] == "FUTURE_PRIVACY");
  REQUIRE(cleared["future"] == true);

  update.expires_at.reset();
  update.model_privacy.reset();
  update.extra.erase("modelPrivacy");
  const auto raw_null = update.to_json_body();
  REQUIRE(raw_null["expiresAt"].is_null());
  REQUIRE_FALSE(raw_null.contains("modelPrivacy"));

  create.extra = nlohmann::json::array({1});
  create.consumption_limit->extra = "not-an-object";
  const auto guarded = create.to_json_body();
  REQUIRE(guarded.is_object());
  REQUIRE_FALSE(guarded.contains("future"));
  REQUIRE(guarded["consumptionLimit"] == nlohmann::json{{"usd", 0.0}});
}

TEST_CASE("Web3 API-key requests keep wallet proof raw-shaped and modeled-wins",
          "[api-keys][web3][request]") {
  venice::Web3ApiKeyCreateRequest request{
      .api_key_type = "FUTURE_TYPE",
      .address = "synthetic-address",
      .signature = "SYNTHETIC_SIGNATURE_SECRET",
      .token = "SYNTHETIC_CHALLENGE_SECRET",
      .consumption_limit =
          venice::ApiKeyConsumptionLimitRequest{
              .usd = 0.0,
              .extra = {{"usd", 999}, {"futureLimit", true}},
          },
      .limit_period = "FUTURE_PERIOD",
      .description = "",
      .expires_at = "",
      .extra = {{"apiKeyType", "shadow"},
                {"address", "shadow"},
                {"signature", "shadow"},
                {"token", "shadow"},
                {"modelPrivacy", "shadow"},
                {"future", 1}},
      .model_privacy = "FUTURE_PRIVACY",
  };
  const auto body = request.to_json_body();
  REQUIRE(body["apiKeyType"] == "FUTURE_TYPE");
  REQUIRE(body["address"] == "synthetic-address");
  REQUIRE(body["signature"] == "SYNTHETIC_SIGNATURE_SECRET");
  REQUIRE(body["token"] == "SYNTHETIC_CHALLENGE_SECRET");
  REQUIRE(body["consumptionLimit"]["usd"] == 0.0);
  REQUIRE(body["consumptionLimit"]["futureLimit"] == true);
  REQUIRE(body["limitPeriod"] == "FUTURE_PERIOD");
  REQUIRE(body["description"] == "");
  REQUIRE(body["expiresAt"] == "");
  REQUIRE(body["modelPrivacy"] == "FUTURE_PRIVACY");
  REQUIRE(body["future"] == 1);

  request.consumption_limit.reset();
  request.limit_period.reset();
  request.description.reset();
  request.expires_at.reset();
  request.model_privacy.reset();
  request.extra = nlohmann::json::array({1});
  const auto minimal = request.to_json_body();
  REQUIRE(minimal.size() == 4);
  REQUIRE_FALSE(minimal.contains("consumptionLimit"));
  REQUIRE_FALSE(minimal.contains("limitPeriod"));
  REQUIRE_FALSE(minimal.contains("description"));
  REQUIRE_FALSE(minimal.contains("expiresAt"));
  REQUIRE_FALSE(minimal.contains("modelPrivacy"));
}

TEST_CASE("create update and delete results keep success and secret boundaries", "[api-keys][parse]") {
  const nlohmann::json created_body{
      {"data",
       {{"apiKey", "SYNTHETIC_SECRET_RETURNED_ONCE"},
        {"apiKeyType", "INFERENCE"},
        {"consumptionLimit", {{"usd", 0}}},
        {"limitPeriod", "EPOCH"},
        {"modelPrivacy", "FUTURE_PRIVACY"},
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
  REQUIRE(created.model_privacy == "FUTURE_PRIVACY");
  REQUIRE(created.raw["data"]["apiKey"] == "[REDACTED]");
  REQUIRE(created.raw.dump().find("SYNTHETIC_SECRET_RETURNED_ONCE") ==
          std::string::npos);
  REQUIRE(created.raw["futureEnvelope"] == true);

  const nlohmann::json updated_body{{"data", key_object()}, {"success", false}};
  const auto updated = venice::api_key_update_from_json_body(updated_body);
  REQUIRE_FALSE(updated.success);
  REQUIRE(updated.key.id == "synthetic-id");
  REQUIRE(updated.key.model_privacy == "FUTURE_PRIVACY");
  REQUIRE(updated.raw == updated_body);

  const auto deleted = venice::api_key_delete_from_json_body(
      nlohmann::json{{"success", false}, {"future", 1}});
  REQUIRE_FALSE(deleted.success);
  REQUIRE(deleted.raw["future"] == 1);
}

TEST_CASE("API-key model privacy preserves optional unknown states",
          "[api-keys][parse][model-privacy]") {
  auto missing = key_object();
  missing.erase("modelPrivacy");
  REQUIRE_FALSE(
      venice::api_key_from_json_body({{"data", missing}}).model_privacy);

  for (const auto& value : {nlohmann::json(nullptr), nlohmann::json(false),
                            nlohmann::json(7), nlohmann::json::array()}) {
    auto malformed = key_object();
    malformed["modelPrivacy"] = value;
    const auto parsed = venice::api_key_update_from_json_body(
        {{"data", malformed}, {"success", true}});
    REQUIRE(parsed.success);
    REQUIRE(parsed.key.id == "synthetic-id");
    REQUIRE_FALSE(parsed.key.model_privacy);
    REQUIRE(parsed.key.raw["modelPrivacy"] == value);
  }

  const nlohmann::json created_body{
      {"data",
       {{"apiKey", "SYNTHETIC_SECRET_RETURNED_ONCE"},
        {"id", "synthetic-created-id"},
        {"modelPrivacy", false}}},
      {"success", true},
  };
  const auto created = venice::api_key_created_from_json_body(created_body);
  REQUIRE_FALSE(created.model_privacy);
  REQUIRE(created.raw["data"]["modelPrivacy"] == false);
  REQUIRE(created.raw["data"]["apiKey"] == "[REDACTED]");
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

TEST_CASE("Web3 API-key guards reject missing proof and non-finite limits only",
          "[api-keys][web3][guards][failure]") {
  const venice::Web3ApiKeyCreateRequest valid{
      .api_key_type = "FUTURE_TYPE",
      .address = "synthetic-address",
      .signature = "SYNTHETIC_SIGNATURE_SECRET",
      .token = "SYNTHETIC_CHALLENGE_SECRET",
  };

  auto missing = valid;
  missing.api_key_type.clear();
  REQUIRE(kPublic.create_web3_api_key(missing).error().message ==
          "Web3 API key type must not be empty");
  missing = valid;
  missing.address.clear();
  REQUIRE(kPublic.create_web3_api_key(missing).error().message ==
          "Web3 wallet address must not be empty");
  missing = valid;
  missing.signature.clear();
  REQUIRE(kPublic.create_web3_api_key(missing).error().message ==
          "Web3 wallet signature must not be empty");
  missing = valid;
  missing.token.clear();
  REQUIRE(kPublic.create_web3_api_key(missing).error().message ==
          "Web3 challenge token must not be empty");

  auto non_finite = valid;
  non_finite.consumption_limit = venice::ApiKeyConsumptionLimitRequest{
      .diem = std::numeric_limits<double>::infinity()};
  REQUIRE(kPublic.create_web3_api_key(non_finite).error().message ==
          "consumption_limit.diem is not finite");

  // No local enum, range, address, token or signature-format opinion: all
  // finite caller values reach auth selection. A Bearer client proves that
  // without putting the synthetic proof on a socket.
  auto future = valid;
  future.api_key_type = "FUTURE_TYPE";
  future.address = "not-an-ethereum-address";
  future.signature = "caller-owned-format";
  future.token = "caller-owned-token";
  future.consumption_limit = venice::ApiKeyConsumptionLimitRequest{.usd = -1.0};
  const auto pass = kBearer.create_web3_api_key(future);
  REQUIRE_FALSE(pass);
  REQUIRE(pass.error().message == "endpoint requires public authentication");
}
