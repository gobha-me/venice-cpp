// JSON request representability (VC-48, #83).
//
// Pure encoding behavior lives here. Public entry-point routing and the proof
// that rejected bodies never reach a socket live in test/06transport/.
// Failure matrix first, unchanged valid output last.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <venice/venice.hpp>

namespace {

auto discarded() -> nlohmann::json {
  auto value = nlohmann::json::parse("{", nullptr, /*allow_exceptions=*/false);
  REQUIRE(value.is_discarded());
  return value;
}

auto invalid_utf8() -> std::string {
  return std::string(1, static_cast<char>(0xff));
}

void require_invalid(const nlohmann::json &value,
                     std::string_view field_class) {
  const auto result = venice::detail::encode_json(value, field_class);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().is(venice::ErrorKind::InvalidArg));
  REQUIRE(result.error().status == 0);
  REQUIRE(result.error().body.empty());
  REQUIRE(result.error().message.find(field_class) != std::string::npos);
  REQUIRE(result.error().message.find(invalid_utf8()) == std::string::npos);
}

} // namespace

TEST_CASE("JSON encoding rejects discarded values at every structural position",
          "[json-encoding][failure]") {
  SECTION("root") { require_invalid(discarded(), "JSON request body"); }
  SECTION("object value") {
    auto value = nlohmann::json::object();
    value["future"] = discarded();
    require_invalid(value, "JSON request body");
  }
  SECTION("array element") {
    auto value = nlohmann::json::array();
    value.push_back(nlohmann::json{{"nested", discarded()}});
    require_invalid(value, "JSON request body");
  }
}

TEST_CASE("JSON encoding rejects invalid UTF-8 without echoing caller content",
          "[json-encoding][failure]") {
  SECTION("modeled-style string value") {
    require_invalid(nlohmann::json(invalid_utf8()), "JSON request body");
  }
  SECTION("raw object key") {
    auto value = nlohmann::json::object();
    value[invalid_utf8()] = true;
    require_invalid(value, "JSON request body");
  }
  SECTION("nested raw value") {
    const nlohmann::json value{
        {"items",
         nlohmann::json::array({nlohmann::json{{"value", invalid_utf8()}}})}};
    require_invalid(value, "JSON request body");
  }
}

TEST_CASE("multipart JSON fields share the fail-closed encoder",
          "[json-encoding][multipart][failure]") {
  venice::detail::MultipartBody body;

  const auto rejected =
      venice::detail::append_json_form_field(body, "future", discarded());
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().is(venice::ErrorKind::InvalidArg));
  REQUIRE(rejected.error().message.find("multipart JSON field") !=
          std::string::npos);
  REQUIRE(body.parts.empty());

  REQUIRE(venice::detail::append_json_form_field(body, "enabled", true));
  REQUIRE(venice::detail::append_json_form_field(body, "strength", 0.75));
  REQUIRE(body.parts.size() == 2);
  REQUIRE(body.parts[0].bytes == "true");
  REQUIRE(body.parts[1].bytes == "0.75");
}

TEST_CASE("valid JSON keeps the existing byte representation",
          "[json-encoding]") {
  const nlohmann::json value{
      {"unicode", "Venice \xE2\x98\x83"},
      {"raw", nlohmann::json::array({nullptr, false, 7, 1.25, "text"})},
      {"future", nlohmann::json{{"kept", true}}}};

  const auto encoded = venice::detail::encode_json(value, "JSON request body");
  REQUIRE(encoded);
  REQUIRE(*encoded == value.dump());
  REQUIRE(nlohmann::json::parse(*encoded) == value);
}
