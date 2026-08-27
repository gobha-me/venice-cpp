// x402 wallet balance, top-up and transaction contracts — VC-34 (#49).
//
// Pure query builders and parsers only. Socket behavior lives in
// test/06transport, the repository's single loopback fixture. Failure matrix
// first.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include <venice/venice.hpp>

TEST_CASE("x402 balance rejects malformed required structure",
          "[x402][balance][failure]") {
  const auto valid_data = nlohmann::json{{"walletAddress", "0xabc"},
                                         {"balanceUsd", 12.5},
                                         {"canConsume", true},
                                         {"minimumTopUpUsd", 5},
                                         {"suggestedTopUpUsd", 10}};
  for (const auto& body : std::vector<nlohmann::json>{
           nullptr,
           nlohmann::json::object(),
           nlohmann::json{{"success", true}, {"data", nullptr}},
           nlohmann::json{{"success", "true"}, {"data", valid_data}},
           nlohmann::json{{"success", true},
                          {"data", nlohmann::json{{"walletAddress", 7},
                                                  {"balanceUsd", 12.5},
                                                  {"canConsume", true},
                                                  {"minimumTopUpUsd", 5},
                                                  {"suggestedTopUpUsd", 10}}}},
           nlohmann::json{{"success", true},
                          {"data", nlohmann::json{{"walletAddress", "0xabc"},
                                                  {"balanceUsd", "12.5"},
                                                  {"canConsume", true},
                                                  {"minimumTopUpUsd", 5},
                                                  {"suggestedTopUpUsd", 10}}}}}) {
    CAPTURE(body);
    REQUIRE_THROWS(venice::x402_balance_from_json_body(body));
  }
}

TEST_CASE("x402 balance preserves required values optional DIEM and raw",
          "[x402][balance]") {
  const auto body = nlohmann::json{{"success", true},
                                   {"data", {{"walletAddress", "0xabc"},
                                             {"balanceUsd", 12.5},
                                             {"canConsume", true},
                                             {"minimumTopUpUsd", 5},
                                             {"suggestedTopUpUsd", 10},
                                             {"diemBalanceUsd", 3.25},
                                             {"future", "retained"}}},
                                   {"object", "x402.balance"}};
  const auto parsed = venice::x402_balance_from_json_body(body);
  REQUIRE(parsed.success);
  REQUIRE(parsed.wallet_address == "0xabc");
  REQUIRE(parsed.balance_usd == 12.5);
  REQUIRE(parsed.can_consume);
  REQUIRE(parsed.minimum_top_up_usd == 5.0);
  REQUIRE(parsed.suggested_top_up_usd == 10.0);
  REQUIRE(parsed.diem_balance_usd == std::optional<double>{3.25});
  REQUIRE(parsed.raw == body);
}

TEST_CASE("x402 payment requirements reject unsafe partial options",
          "[x402][top-up][failure]") {
  const auto good = nlohmann::json{{"scheme", "exact"},
                                   {"network", "eip155:8453"},
                                   {"amount", "5000000"},
                                   {"asset", "0xasset"},
                                   {"payTo", "0xreceiver"},
                                   {"maxTimeoutSeconds", 300}};
  for (const auto& body : std::vector<nlohmann::json>{
           nullptr,
           nlohmann::json::object(),
           nlohmann::json{{"x402Version", "2"}, {"accepts", {good}}},
           nlohmann::json{{"x402Version", std::numeric_limits<std::uint64_t>::max()},
                          {"accepts", {good}}},
           nlohmann::json{{"x402Version", 2}, {"accepts", "exact"}},
           nlohmann::json{{"x402Version", 2},
                          {"accepts", {nlohmann::json{{"scheme", "exact"}}}}},
           nlohmann::json{{"x402Version", 2},
                          {"accepts", {nlohmann::json{{"scheme", "exact"},
                                                        {"network", "eip155:8453"},
                                                        {"amount", 5000000},
                                                        {"asset", "0xasset"},
                                                        {"payTo", "0xreceiver"},
                                                        {"maxTimeoutSeconds", 300}}}}}}) {
    CAPTURE(body);
    REQUIRE_THROWS(venice::x402_payment_requirements_from_json_body(body));
  }
}

TEST_CASE("x402 payment requirements keep base units and provider data exact",
          "[x402][top-up]") {
  const auto body = nlohmann::json{
      {"x402Version", 2},
      {"accepts",
       {{{"scheme", "exact"},
         {"network", "solana:mainnet"},
         {"amount", "5000000"},
         {"asset", "mint"},
         {"payTo", "receiver"},
         {"maxTimeoutSeconds", 300},
         {"extra", {{"feePayer", "payer"}, {"version", "2"}}},
         {"future", true}}}}};
  const auto parsed = venice::x402_payment_requirements_from_json_body(body);
  REQUIRE(parsed.x402_version == 2);
  REQUIRE(parsed.accepts.size() == 1);
  REQUIRE(parsed.accepts.front().amount == "5000000");
  REQUIRE(parsed.accepts.front().network_extra ==
          std::optional<nlohmann::json>{{{"feePayer", "payer"}, {"version", "2"}}});
  REQUIRE(parsed.accepts.front().raw.at("future") == true);
  REQUIRE(parsed.raw == body);
}

TEST_CASE("x402 top-up receipt requires complete accounting fields",
          "[x402][top-up][failure]") {
  const auto valid_data = nlohmann::json{{"walletAddress", "0xabc"},
                                         {"amountCredited", 10},
                                         {"newBalance", 22.5},
                                         {"paymentId", "payment-1"}};
  for (const auto& body : std::vector<nlohmann::json>{
           nullptr,
           nlohmann::json{{"success", true}},
           nlohmann::json{{"success", 1}, {"data", valid_data}},
           nlohmann::json{{"success", true},
                          {"data", nlohmann::json{{"walletAddress", "0xabc"},
                                                  {"amountCredited", "10"},
                                                  {"newBalance", 22.5},
                                                  {"paymentId", "payment-1"}}}}}) {
    CAPTURE(body);
    REQUIRE_THROWS(venice::x402_top_up_receipt_from_json_body(body));
  }

  const auto parsed = venice::x402_top_up_receipt_from_json_body(
      nlohmann::json{{"success", true}, {"data", valid_data}, {"future", 1}});
  REQUIRE(parsed.success);
  REQUIRE(parsed.wallet_address == "0xabc");
  REQUIRE(parsed.amount_credited == 10.0);
  REQUIRE(parsed.new_balance == 22.5);
  REQUIRE(parsed.payment_id == "payment-1");
  REQUIRE(parsed.raw.at("future") == 1);
}

TEST_CASE("x402 transaction page requires its envelope and list",
          "[x402][transactions][failure]") {
  for (const auto& body : std::vector<nlohmann::json>{
           nullptr,
           nlohmann::json::object(),
           nlohmann::json{{"success", true}, {"data", nullptr}},
           nlohmann::json{{"success", "true"},
                          {"data", {{"transactions", nlohmann::json::array()}}}},
           nlohmann::json{{"success", true},
                          {"data", {{"transactions", nlohmann::json::object()}}}}}) {
    CAPTURE(body);
    REQUIRE_THROWS(venice::x402_transactions_from_json_body(body));
  }
}

TEST_CASE("x402 transaction listing degrades leaves and preserves order and raw",
          "[x402][transactions]") {
  const auto body = nlohmann::json{
      {"success", true},
      {"data",
       {{"walletAddress", "0xabc"},
        {"currentBalance", 12.35},
        {"transactions",
         {{{"id", "ledger-1"},
           {"amount", -0.15},
           {"balanceAfter", 12.35},
           {"type", "CHARGE"},
           {"createdAt", "2026-04-03T12:34:56.000Z"},
           {"requestId", nullptr},
           {"modelId", "model-a"},
           {"future", 7}},
          {{"id", 9}, {"amount", "bad"}},
          "not-an-object"}},
        {"pagination", {{"limit", 50}, {"offset", 0}, {"hasMore", false}}},
        {"futurePage", true}}},
      {"futureEnvelope", "retained"}};

  const auto parsed = venice::x402_transactions_from_json_body(body);
  REQUIRE(parsed.success);
  REQUIRE(parsed.wallet_address == std::optional<std::string>{"0xabc"});
  REQUIRE(parsed.current_balance == std::optional<double>{12.35});
  REQUIRE(parsed.returned == 3);
  REQUIRE(parsed.entries.size() == 2);
  REQUIRE(parsed.entries.at(0).id == std::optional<std::string>{"ledger-1"});
  REQUIRE(parsed.entries.at(0).request_id == std::nullopt);
  REQUIRE(parsed.entries.at(0).model_id == std::optional<std::string>{"model-a"});
  REQUIRE(parsed.entries.at(0).raw.at("future") == 7);
  REQUIRE_FALSE(parsed.entries.at(1).id.has_value());
  REQUIRE_FALSE(parsed.entries.at(1).amount.has_value());
  REQUIRE(parsed.pagination.has_value());
  REQUIRE(parsed.pagination->limit == std::optional<int>{50});
  REQUIRE(parsed.pagination->offset == std::optional<int>{0});
  REQUIRE(parsed.pagination->has_more == std::optional<bool>{false});
  REQUIRE(parsed.raw == body);
}

TEST_CASE("x402 transaction query keeps modeled order and open-string passthrough",
          "[x402][query]") {
  venice::X402TransactionsQuery query;
  query.limit = -1;  // Server-owned range; the client transmits it verbatim.
  query.offset = 7;
  query.extra = {{"future filter", "a/b"}, {"limit", "shadowed"}, {"", "skip"}};

  REQUIRE(venice::x402_transactions_query_params(query) ==
          std::vector<std::pair<std::string, std::string>>{
              {"limit", "-1"}, {"offset", "7"}, {"future filter", "a/b"}});
  REQUIRE(venice::detail::with_query(
              venice::detail::with_path_segment("/x402/transactions", "wallet/with space"),
              venice::x402_transactions_query_params(query)) ==
          "/x402/transactions/wallet%2Fwith%20space?limit=-1&offset=7&future%20filter=a%2Fb");
}
