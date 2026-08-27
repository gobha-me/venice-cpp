#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "venice/client.hpp"

using namespace venice;

namespace {

auto minimal_chat() -> ChatRequest {
  ChatRequest request;
  request.model = "m";
  request.messages = {Message::user("hello")};
  return request;
}

auto minimal_response() -> ResponsesRequest {
  ResponsesRequest request;
  request.model = "m";
  request.input = responses_input::text("hello");
  return request;
}

}  // namespace

TEST_CASE("VC-24 chat controls serialize without changing method-owned stream state",
          "[vc24][chat][request][failure]") {
  auto request = minimal_chat();
  request.logprobs = false;
  request.top_logprobs = 4;
  request.max_completion_tokens = 128;
  request.max_temp = 1.5;
  request.min_p = 0.1;
  request.min_temp = 0.2;
  request.n = 2;
  request.prompt_cache_key = "conversation-1";
  request.prompt_cache_retention = "24h";
  request.repetition_penalty = 1.1;
  request.reasoning = reasoning_config::make("high", "concise");
  request.reasoning_effort = "medium";
  request.stop_token_ids = std::vector<std::int64_t>{1, 2};
  request.top_k = 40;
  request.user = "caller";
  request.fallbacks = std::vector<nlohmann::json>{fallbacks::model("backup")};
  request.store = false;
  request.verbosity = "low";
  request.text = text_config::verbosity("high");
  request.include = std::vector<std::string>{"message.output_text.logprobs"};
  request.metadata = nlohmann::json::parse(R"({"trace":"abc"})");
  request.extra = nlohmann::json::parse(
      R"({"stream":true,"top_k":99,"future":"kept","stream_options":{"include_usage":false}})");

  ChatStreamOptions stream_options;
  stream_options.include_usage = true;
  request.stream_options = stream_options;

  const auto buffered = request.to_json_body(false);
  REQUIRE(buffered.at("stream") == false);
  REQUIRE_FALSE(buffered.contains("stream_options"));
  REQUIRE(buffered.at("top_k") == 40);
  REQUIRE(buffered.at("future") == "kept");

  const auto streamed = request.to_json_body(true);
  REQUIRE(streamed.at("stream") == true);
  REQUIRE(streamed.at("stream_options").at("include_usage") == true);
  REQUIRE(streamed.at("logprobs") == false);
  REQUIRE(streamed.at("max_completion_tokens") == 128);
  REQUIRE(streamed.at("stop_token_ids") == nlohmann::json::array({1, 2}));
  REQUIRE(streamed.at("fallbacks").at(0).at("model") == "backup");
  REQUIRE(streamed.at("metadata").at("trace") == "abc");
}

TEST_CASE("an unmodeled stream_options object survives only on the streaming path",
          "[vc24][chat][request][failure]") {
  auto request = minimal_chat();
  request.extra["stream_options"] = nlohmann::json::parse(R"({"future":7})");

  REQUIRE_FALSE(request.to_json_body(false).contains("stream_options"));
  REQUIRE(request.to_json_body(true).at("stream_options").at("future") == 7);
}

TEST_CASE("VC-24 builders emit the documented polymorphic shapes", "[vc24][builders]") {
  const auto cache = cache_control::ephemeral("1h");
  REQUIRE(cache == nlohmann::json::parse(R"({"ttl":"1h","type":"ephemeral"})"));

  REQUIRE(message_content::text("hello", cache).at("cache_control") == cache);
  REQUIRE(message_content::image_url("https://example.invalid/a.png").at("type") ==
          "image_url");
  REQUIRE(message_content::input_audio("AAAA", "wav").at("input_audio").at("format") ==
          "wav");
  REQUIRE(message_content::video_url("https://example.invalid/a.mp4").at("type") ==
          "video_url");
  REQUIRE(message_content::file("data:text/plain;base64,QQ==", "a.txt").at("file").at(
              "filename") == "a.txt");

  REQUIRE(responses_content::input_text("hello").at("type") == "input_text");
  REQUIRE(responses_content::input_image("https://example.invalid/a.png", "high")
              .at("image_url")
              .at("detail") == "high");
  REQUIRE(responses_items::message(
              "user", nlohmann::json::array({responses_content::input_text("hello")}))
              .at("type") == "message");
  REQUIRE(responses_items::function_call_output("call-1", 42).at("output") == 42);
  REQUIRE(responses_items::item_reference("item-1").at("id") == "item-1");
}

TEST_CASE("the README Responses example compiles and serializes", "[vc24][readme]") {
  ResponsesRequest request;
  request.model = "zai-org-glm-5-1";
  request.input = responses_input::items({
      responses_items::message(
          "user", nlohmann::json::array({
                      responses_content::input_text("Describe this image"),
                      responses_content::input_image("https://example.com/image.png", "high")}))});
  request.max_output_tokens = 256;
  request.tools = std::vector<nlohmann::json>{
      tools::function("lookup", "Look up a value")};

  const auto body = request.to_json_body();
  REQUIRE(body.at("input").at(0).at("content").size() == 2);
  REQUIRE(body.at("tools").at(0).at("function").at("name") == "lookup");
}

TEST_CASE("message reasoning details and message-level signature are mutation honest",
          "[vc24][message][failure]") {
  auto message = nlohmann::json::parse(R"({
    "role":"assistant","content":null,
    "reasoning_details":[{"type":"provider","future":1}],
    "thought_signature":"opaque","future_message_key":true
  })").get<Message>();
  REQUIRE(message.reasoning_details->at(0).at("future") == 1);
  REQUIRE(message.thought_signature == "opaque");

  message.extra = message.raw;
  message.reasoning_details.reset();
  message.thought_signature.reset();
  const nlohmann::json redacted = message;
  REQUIRE_FALSE(redacted.contains("reasoning_details"));
  REQUIRE_FALSE(redacted.contains("thought_signature"));
  REQUIRE(redacted.at("future_message_key") == true);
}

TEST_CASE("ResponsesRequest forces non-streaming and preserves its raw escape hatch",
          "[vc24][responses][request][failure]") {
  auto request = minimal_response();
  request.include = std::vector<std::string>{"reasoning.encrypted_content"};
  request.max_output_tokens = 64;
  request.temperature = 0.5;
  request.top_p = 0.9;
  request.fallbacks = std::vector<nlohmann::json>{fallbacks::model("backup")};
  request.reasoning = reasoning_config::make("medium", "auto");
  request.tools = std::vector<nlohmann::json>{tools::function("lookup")};
  request.tool_choice = tool_choice::automatic();
  request.web_search = false;
  request.extra = nlohmann::json::parse(R"({"stream":true,"future":"kept","top_p":0.1})");

  const auto body = request.to_json_body();
  REQUIRE(body.at("stream") == false);
  REQUIRE(body.at("top_p") == 0.9);
  REQUIRE(body.at("future") == "kept");
  REQUIRE(body.at("input") == "hello");
  REQUIRE(body.at("tools").at(0).at("function").at("name") == "lookup");
}

TEST_CASE("Responses parsing keeps unknown items while exposing calls citations and text",
          "[vc24][responses][response][failure]") {
  const auto body = nlohmann::json::parse(R"({
    "id":"resp_1","object":"response","created_at":10,"model":"m","status":"completed",
    "output":[
      {"type":"reasoning","id":"r1","future":{"x":1}},
      {"type":"message","id":"msg1","status":"completed","role":"assistant","content":[
        {"type":"output_text","text":"hello ","annotations":[
          {"type":"url_citation","url":"https://example.invalid","title":"Example",
           "start_index":0,"end_index":5}
        ]},
        {"type":"output_text","text":"world","annotations":[]}
      ]},
      {"type":"function_call","id":"fc1","call_id":"call1","name":"lookup",
       "arguments":"{\"q\":1}","status":"completed"},
      {"type":"future_output","payload":7}
    ],
    "usage":{"input_tokens":3,"input_tokens_details":{"cached_tokens":1},
             "output_tokens":2,"output_tokens_details":{"reasoning_tokens":1},
             "total_tokens":5},
    "error":null,
    "future_envelope":true
  })");

  const auto response = responses_from_json_body(body);
  REQUIRE(response.raw == body);
  REQUIRE(response.output.size() == 4);
  REQUIRE(response.output.at(3).at("type") == "future_output");
  REQUIRE(response.output_text() == "hello world");
  REQUIRE(response.function_calls().size() == 1);
  REQUIRE(response.function_calls().front().call_id == "call1");
  REQUIRE(response.function_calls().front().arguments == R"({"q":1})");
  REQUIRE(response.citations().size() == 1);
  REQUIRE(response.citations().front().url == "https://example.invalid");
  REQUIRE(response.usage->cached_tokens == 1);
  REQUIRE(response.usage->reasoning_tokens == 1);
  REQUIRE_FALSE(response.error.has_value());
}

TEST_CASE("Responses required accounting stays loud", "[vc24][responses][response][failure]") {
  auto body = nlohmann::json::parse(R"({
    "id":"resp_1","object":"response","created_at":10,"model":"m","status":"completed",
    "output":[],"usage":{"input_tokens":"many","output_tokens":2,"total_tokens":2}
  })");
  REQUIRE_THROWS(responses_from_json_body(body));
}

TEST_CASE("Responses error payload is typed without discarding its raw object",
          "[vc24][responses][response]") {
  const auto response = responses_from_json_body(nlohmann::json::parse(R"({
    "id":"resp_1","object":"response","created_at":10,"model":"m","status":"failed",
    "output":[],"error":{"code":"MODEL_FAILED","message":"nope","retryable":true}
  })"));
  REQUIRE(response.error->code == "MODEL_FAILED");
  REQUIRE(response.error->message == "nope");
  REQUIRE(response.error->raw.at("retryable") == true);
}

TEST_CASE("ChatResponse exposes every choice while preserving choice-zero conveniences",
          "[vc24][chat][response]") {
  const auto response = ChatResponse::from_json_body(nlohmann::json::parse(R"({
    "id":"chat_1","model":"m","prompt_logprobs":{"token":"p"},
    "choices":[
      {"index":1,"finish_reason":"length","stop_reason":"length","logprobs":{"token":"b"},
       "message":{"role":"assistant","content":"second"}},
      {"index":0,"finish_reason":"stop","stop_reason":null,"logprobs":null,
       "message":{"role":"assistant","content":"first"}}
    ]
  })"));

  REQUIRE(response.choices.size() == 2);
  REQUIRE(response.choices.at(0).index == 1);
  REQUIRE(response.choices.at(1).index == 0);
  REQUIRE(response.content == "second");
  REQUIRE(response.finish_reason == "length");
  REQUIRE(response.prompt_logprobs->at("token") == "p");
  REQUIRE(response.choices.at(1).logprobs->is_null());
  REQUIRE_FALSE(response.choices.at(1).stop_reason.has_value());

  auto corrupt = response.raw;
  corrupt.at("choices").at(0).at("index") = "one";
  REQUIRE_THROWS(ChatResponse::from_json_body(corrupt));
}

TEST_CASE("cache-write usage is distinct from cache-read usage", "[vc24][usage][failure]") {
  const auto usage = nlohmann::json::parse(R"({
    "prompt_tokens":10,"completion_tokens":2,"total_tokens":12,
    "prompt_tokens_details":{"cached_tokens":4,"cache_creation_input_tokens":3}
  })").get<Usage>();
  REQUIRE(usage.cached_tokens == 4);
  REQUIRE(usage.cache_creation_input_tokens == 3);

  const auto absent = nlohmann::json::parse(R"({"prompt_tokens":1})").get<Usage>();
  REQUIRE_FALSE(absent.cache_creation_input_tokens.has_value());
  REQUIRE_THROWS(nlohmann::json::parse(
                     R"({"prompt_tokens_details":{"cache_creation_input_tokens":"many"}})")
                     .get<Usage>());
}

TEST_CASE("streaming choices are joined independently and emitted by index",
          "[vc24][chat][stream][failure]") {
  StreamAccumulator accumulator;
  accumulator.ingest(nlohmann::json::parse(R"({
    "id":"chat_1","model":"m","choices":[
      {"index":1,"delta":{"role":"assistant","content":"sec",
       "reasoning_details":[{"type":"provider","data":"x"}],
       "thought_signature":"opaque"}},
      {"index":0,"delta":{"role":"assistant","content":"fir"}}
    ]
  })"));
  accumulator.ingest(nlohmann::json::parse(R"({
    "choices":[
      {"index":0,"finish_reason":"stop","delta":{"content":"st"}},
      {"index":1,"finish_reason":"length","stop_reason":"length","delta":{"content":"ond"}}
    ],
    "usage":{"prompt_tokens":1,"completion_tokens":2,"total_tokens":3}
  })"));

  const auto response = accumulator.response();
  REQUIRE(response.choices.size() == 2);
  REQUIRE(response.choices.at(0).index == 0);
  REQUIRE(response.choices.at(0).message->text() == "first");
  REQUIRE(response.choices.at(1).index == 1);
  REQUIRE(response.choices.at(1).message->text() == "second");
  REQUIRE(response.choices.at(1).message->reasoning_details->at(0).at("data") == "x");
  REQUIRE(response.choices.at(1).message->thought_signature == "opaque");
  REQUIRE(response.content == "first");
  REQUIRE(response.finish_reason == "stop");
  REQUIRE(response.usage->total_tokens == 3);
  REQUIRE(accumulator.chunks().size() == 2);
}

TEST_CASE("streaming choice logprobs fan out with their own choice",
          "[vc24][chat][stream][logprobs][failure]") {
  const auto chunk = nlohmann::json::parse(R"({
    "choices":[
      {"index":1,"logprobs":{"token":"second","future":2},"delta":{"content":"b"}},
      {"index":0,"logprobs":null,"delta":{"content":"a"}}
    ],
    "prompt_logprobs":{"token":"prompt"}
  })");

  std::vector<int> indices;
  std::vector<nlohmann::json> logprobs;
  int prompt_views = 0;
  for_each_delta_from_chunk(chunk, [&](const StreamDelta& delta) {
    indices.push_back(delta.choice_index.value_or(-1));
    REQUIRE(delta.logprobs != nullptr);
    logprobs.push_back(*delta.logprobs);
    if (delta.prompt_logprobs != nullptr) ++prompt_views;
  });

  REQUIRE(indices == std::vector<int>{1, 0});
  REQUIRE(logprobs.at(0).at("future") == 2);
  REQUIRE(logprobs.at(1).is_null());
  REQUIRE(prompt_views == 1);
}

TEST_CASE("Responses validation fails before transport", "[vc24][responses][guards][failure]") {
  const Client client{"not-a-real-key", "http://127.0.0.1:1"};

  SECTION("missing model") {
    auto request = minimal_response();
    request.model.clear();
    const auto result = client.create_response(request);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().kind == ErrorKind::InvalidArg);
  }
  SECTION("missing input") {
    auto request = minimal_response();
    request.input = nullptr;
    const auto result = client.create_response(request);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().kind == ErrorKind::InvalidArg);
  }
  SECTION("non-finite modeled double") {
    auto request = minimal_response();
    request.temperature = std::nan("");
    const auto result = client.create_response(request);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().kind == ErrorKind::InvalidArg);
  }
  SECTION("explicit E2EE") {
    auto request = minimal_response();
    request.venice_parameters = VeniceParameters{};
    request.venice_parameters->enable_e2ee = true;
    const auto result = client.create_response(request);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().kind == ErrorKind::InvalidArg);
  }
}
