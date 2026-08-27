// Crypto RPC request/response contract — VC-33 (#48).
//
// Pure builders and parsers only. Socket behavior lives in test/06transport,
// the repository's single loopback fixture. Failure matrix first.

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include <venice/venice.hpp>

TEST_CASE("crypto RPC discovery rejects malformed envelopes",
          "[crypto-rpc][networks][failure]") {
  for (const auto& body : std::vector<nlohmann::json>{
           nullptr, 7, "networks", nlohmann::json::object(),
           nlohmann::json{{"networks", "ethereum-mainnet"}}}) {
    REQUIRE_THROWS(venice::crypto_rpc_networks_from_json_body(body));
  }
}

TEST_CASE("crypto RPC discovery tolerates junk while preserving order and raw",
          "[crypto-rpc][networks]") {
  const auto envelope = nlohmann::json{
      {"networks", nlohmann::json::array(
                       {"ethereum-mainnet", 42, nullptr, "solana-mainnet"})},
      {"future", true}};
  const auto parsed = venice::crypto_rpc_networks_from_json_body(envelope);
  REQUIRE(parsed.networks ==
          std::vector<std::string>{"ethereum-mainnet", "solana-mainnet"});
  REQUIRE(parsed.raw == envelope);

  const auto bare = nlohmann::json::array({"base-mainnet", "polygon-mainnet"});
  REQUIRE(venice::crypto_rpc_networks_from_json_body(bare).networks ==
          std::vector<std::string>{"base-mainnet", "polygon-mainnet"});
}

TEST_CASE("crypto RPC parser rejects corrupt protocol items",
          "[crypto-rpc][response][failure]") {
  const std::vector<nlohmann::json> bad{
      nullptr,
      7,
      nlohmann::json::object(),
      nlohmann::json{{"jsonrpc", "1.0"}, {"id", 1}, {"result", true}},
      nlohmann::json{{"jsonrpc", "2.0"}, {"result", true}},
      nlohmann::json{{"jsonrpc", "2.0"}, {"id", 1.5}, {"result", true}},
      nlohmann::json{{"jsonrpc", "2.0"}, {"id", 1}},
      nlohmann::json{{"jsonrpc", "2.0"},
                     {"id", 1},
                     {"result", true},
                     {"error", nlohmann::json::object()}},
      nlohmann::json{{"jsonrpc", "2.0"}, {"id", 1}, {"error", "bad"}},
      nlohmann::json::array({nlohmann::json{{"jsonrpc", "2.0"},
                                            {"id", 1},
                                            {"result", true}},
                             42})};
  for (const auto& body : bad) {
    CAPTURE(body);
    REQUIRE_THROWS(venice::crypto_rpc_from_json_body(body));
  }
}

TEST_CASE("crypto RPC response preserves IDs results errors order and raw",
          "[crypto-rpc][response]") {
  const auto single_body = nlohmann::json{{"jsonrpc", "2.0"},
                                          {"id", nullptr},
                                          {"result", nullptr},
                                          {"future", 1}};
  const auto single = venice::crypto_rpc_from_json_body(single_body);
  const auto* item = std::get_if<venice::CryptoRpcResponseItem>(&single.payload);
  REQUIRE(item != nullptr);
  REQUIRE(item->id.is_null());
  REQUIRE(item->result == std::optional<nlohmann::json>{nullptr});
  REQUIRE_FALSE(item->error.has_value());
  REQUIRE(item->raw == single_body);
  REQUIRE(single.raw == single_body);

  const auto batch_body = nlohmann::json::array(
      {{{"jsonrpc", "2.0"}, {"id", "first"}, {"result", "0x1"}},
       {{"jsonrpc", "2.0"},
        {"id", 2},
        {"error", {{"code", -32602}, {"message", "invalid params"}}}}});
  const auto batch = venice::crypto_rpc_from_json_body(batch_body);
  const auto* items =
      std::get_if<std::vector<venice::CryptoRpcResponseItem>>(&batch.payload);
  REQUIRE(items != nullptr);
  REQUIRE(items->size() == 2);
  REQUIRE(items->at(0).id == "first");
  REQUIRE(items->at(0).result == std::optional<nlohmann::json>{"0x1"});
  REQUIRE(items->at(1).id == 2);
  REQUIRE(items->at(1).error->at("code") == -32602);
  REQUIRE(batch.raw == batch_body);
}

TEST_CASE("crypto RPC builders emit exact open JSON-RPC shapes",
          "[crypto-rpc][request]") {
  const auto numeric = venice::crypto_rpc_input::request(
      "eth_getBalance", nlohmann::json::array({"0xabc", "latest"}),
      nlohmann::json(7));
  REQUIRE(numeric == nlohmann::json{{"jsonrpc", "2.0"},
                                    {"method", "eth_getBalance"},
                                    {"params", {"0xabc", "latest"}},
                                    {"id", 7}});

  const auto string_id = venice::crypto_rpc_input::request(
      "future_method", nlohmann::json{{"future", true}},
      nlohmann::json("request-a"));
  REQUIRE(string_id.at("id") == "request-a");
  REQUIRE(string_id.at("params").at("future") == true);

  const auto null_id = venice::crypto_rpc_input::request(
      "eth_chainId", nlohmann::json::array(), nlohmann::json(nullptr));
  REQUIRE(null_id.contains("id"));
  REQUIRE(null_id.at("id").is_null());

  const auto notification = venice::crypto_rpc_input::request("net_version");
  REQUIRE_FALSE(notification.contains("id"));
  REQUIRE(notification.at("params").is_array());

  const auto batch = venice::crypto_rpc_input::batch({numeric, string_id});
  REQUIRE(batch.is_array());
  REQUIRE(batch.at(0) == numeric);
  REQUIRE(batch.at(1) == string_id);
}
