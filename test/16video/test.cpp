// Video request/response contracts (VC-29, #44).
//
// Pure serialization, guards and parsing live here. Exact HTTP targets,
// authentication, cancellation and response media routing use the loopback
// peer in test/06transport/. Failure matrix first, happy paths last.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <string>
#include <variant>

#include <nlohmann/json.hpp>

#include <venice/venice.hpp>

namespace {

const venice::Client kBearer{"not-a-real-key"};

}  // namespace

TEST_CASE("video parsers reject malformed required response fields",
          "[video][parse][failure]") {
  SECTION("quote requires a numeric value") {
    REQUIRE_THROWS(venice::video_quote_from_json_body(nlohmann::json::array()));
    REQUIRE_THROWS(venice::video_quote_from_json_body(
        nlohmann::json{{"quote", "cheap"}}));
  }
  SECTION("queue requires model and queue id strings") {
    REQUIRE_THROWS(venice::video_queued_from_json_body(
        nlohmann::json{{"model", "video"}}));
    REQUIRE_THROWS(venice::video_queued_from_json_body(
        nlohmann::json{{"model", 7}, {"queue_id", "q1"}}));
  }
  SECTION("processing requires status and both timing numbers") {
    REQUIRE_THROWS(venice::video_processing_from_json_body(nlohmann::json{
        {"status", "PROCESSING"}, {"average_execution_time", 10}}));
    REQUIRE_THROWS(venice::video_processing_from_json_body(nlohmann::json{
        {"status", "PROCESSING"},
        {"average_execution_time", "soon"},
        {"execution_duration", 5}}));
  }
  SECTION("cleanup success is a required boolean") {
    REQUIRE_THROWS(venice::video_cleanup_from_json_body(nlohmann::json::object()));
    REQUIRE_THROWS(venice::video_cleanup_from_json_body(
        nlohmann::json{{"success", "yes"}}));
  }
  SECTION("JSON transcription requires a string transcript") {
    REQUIRE_THROWS(
        venice::video_transcription_from_json_body(nlohmann::json::array()));
    REQUIRE_THROWS(venice::video_transcription_from_json_body(
        nlohmann::json{{"transcript", 7}}));
  }
}

TEST_CASE("video guards reject only structurally unsendable values",
          "[video][guards][failure]") {
  SECTION("quote checks non-finite values before required strings") {
    venice::VideoQuoteRequest request;
    request.reference_video_total_duration =
        std::numeric_limits<double>::infinity();
    REQUIRE(kBearer.quote_video(request).error().message ==
            "reference_video_total_duration is not finite");
    request.reference_video_total_duration = -12.0;  // policy stays server-owned
    REQUIRE(kBearer.quote_video(request).error().message == "video model is empty");
    request.model = "future-video";
    REQUIRE(kBearer.quote_video(request).error().message == "video duration is empty");
  }
  SECTION("every modeled queue double rejects non-finite input") {
    constexpr std::array<std::optional<double> venice::VideoQueueRequest::*, 9>
        kFields{&venice::VideoQueueRequest::softness,
                &venice::VideoQueueRequest::creativity,
                &venice::VideoQueueRequest::realism,
                &venice::VideoQueueRequest::sharp,
                &venice::VideoQueueRequest::compression,
                &venice::VideoQueueRequest::noise,
                &venice::VideoQueueRequest::halo,
                &venice::VideoQueueRequest::grain,
                &venice::VideoQueueRequest::recover_detail};
    for (const auto field : kFields) {
      venice::VideoQueueRequest request{
          .model = "future-video", .prompt = "hello", .duration = "5s"};
      request.*field = std::numeric_limits<double>::quiet_NaN();
      const auto result = kBearer.queue_video(request);
      REQUIRE_FALSE(result);
      REQUIRE(result.error().is(venice::ErrorKind::InvalidArg));
      REQUIRE(result.error().status == 0);
    }
  }
  SECTION("queue retrieve cleanup and transcription require routing values") {
    venice::VideoQueueRequest queue;
    REQUIRE(kBearer.queue_video(queue).error().message == "video model is empty");
    queue.model = "future-video";
    REQUIRE(kBearer.queue_video(queue).error().message == "video prompt is empty");
    queue.prompt = "hello";
    REQUIRE(kBearer.queue_video(queue).error().message == "video duration is empty");

    venice::VideoRetrieveRequest retrieve;
    REQUIRE(kBearer.retrieve_video(retrieve).error().message == "video model is empty");
    retrieve.model = "future-video";
    REQUIRE(kBearer.retrieve_video(retrieve).error().message == "video queue id is empty");

    venice::VideoCleanupRequest cleanup;
    REQUIRE(kBearer.cleanup_video(cleanup).error().message == "video model is empty");
    cleanup.model = "future-video";
    REQUIRE(kBearer.cleanup_video(cleanup).error().message == "video queue id is empty");

    REQUIRE(kBearer.transcribe_video({}).error().message ==
            "video transcription URL is empty");
  }
}

TEST_CASE("video builders and queue serialization preserve open ordered shapes",
          "[video][request]") {
  const auto element = venice::video_input::element(
      "front.png", std::vector<std::string>{"left.png", "right.png"},
      "motion.mp4");
  const auto keyframe = venice::video_input::keyframe("start.png", 0);
  REQUIRE(element["frontal_image_url"] == "front.png");
  REQUIRE(element["reference_image_urls"] ==
          nlohmann::json::array({"left.png", "right.png"}));
  REQUIRE(element["video_url"] == "motion.mp4");
  REQUIRE(keyframe ==
          nlohmann::json{{"image_url", "start.png"}, {"frame_index", 0}});

  const venice::VideoQueueRequest request{
      .model = "future-video",
      .prompt = "make a scene",
      .duration = "future-duration",
      .consents = nlohmann::json{{"future-provider", {{"accepted", true}}}},
      .negative_prompt = "none",
      .aspect_ratio = "future-ratio",
      .omni_reference_task_type = "future-task",
      .resolution = "future-resolution",
      .upscale_factor = 0,
      .enhancement_model = "future-enhancer",
      .target_fps = 0,
      .softness = -1.0,
      .creativity = 8.0,
      .realism = 0.0,
      .sharp = 0.0,
      .compression = 0.0,
      .noise = 0.0,
      .halo = 0.0,
      .grain = 0.0,
      .recover_detail = 0.0,
      .h264_output = false,
      .output_format = "future-format",
      .slowdown_factor = 0,
      .audio = false,
      .image_url = "image",
      .end_image_url = "end",
      .audio_url = "audio",
      .video_url = "video",
      .reference_image_urls = std::vector<std::string>{"i1", "i2"},
      .reference_video_urls = std::vector<std::string>{"v1", "v2"},
      .reference_audio_urls = std::vector<std::string>{},
      .reference_document_urls = std::vector<std::string>{"d1"},
      .elements = std::vector<nlohmann::json>{
          element, nlohmann::json{{"future", true}}},
      .scene_image_urls = std::vector<std::string>{"s1", "s2"},
      .keyframes = std::vector<nlohmann::json>{keyframe},
      .extra = {{"model", "shadow"}, {"future", 1}},
  };
  const auto body = request.to_json_body();
  REQUIRE(body["model"] == "future-video");
  REQUIRE(body["duration"] == "future-duration");
  REQUIRE(body["upscale_factor"] == 0);
  REQUIRE(body["audio"] == false);
  REQUIRE(body["h264_output"] == false);
  REQUIRE(body["reference_video_urls"] ==
          nlohmann::json::array({"v1", "v2"}));
  REQUIRE(body["reference_audio_urls"].empty());
  REQUIRE(body["elements"][0] == element);
  REQUIRE(body["elements"][1]["future"] == true);
  REQUIRE(body["keyframes"][0] == keyframe);
  REQUIRE(body["future"] == 1);

  auto minimal = request;
  minimal = venice::VideoQueueRequest{
      .model = "m", .prompt = "p", .duration = "d", .extra = nlohmann::json::array()};
  REQUIRE(minimal.to_json_body() ==
          nlohmann::json{{"model", "m"}, {"prompt", "p"}, {"duration", "d"}});
}

TEST_CASE("video quote retrieve cleanup and transcription serialization keeps false and zero",
          "[video][request]") {
  const venice::VideoQuoteRequest quote{
      .model = "video",
      .duration = "5s",
      .upscale_factor = 0,
      .target_fps = 0,
      .slowdown_factor = 0,
      .audio = false,
      .reference_video_total_duration = 0.0,
      .extra = {{"model", "shadow"}, {"future", true}},
  };
  const auto quote_body = quote.to_json_body();
  REQUIRE(quote_body["model"] == "video");
  REQUIRE(quote_body["audio"] == false);
  REQUIRE(quote_body["reference_video_total_duration"] == 0.0);
  REQUIRE(quote_body["future"] == true);

  const venice::VideoRetrieveRequest retrieve{
      .model = "video",
      .queue_id = "q1",
      .delete_media_on_completion = false,
      .extra = {{"queue_id", "shadow"}, {"future", true}},
  };
  REQUIRE(retrieve.to_json_body()["queue_id"] == "q1");
  REQUIRE(retrieve.to_json_body()["delete_media_on_completion"] == false);

  const venice::VideoCleanupRequest cleanup{
      .model = "video", .queue_id = "q1", .extra = {{"model", "shadow"}}};
  REQUIRE(cleanup.to_json_body()["model"] == "video");

  const venice::VideoTranscriptionRequest transcription{
      .url = "https://example.test/video",
      .response_format = "future-format",
      .extra = {{"url", "shadow"}, {"future", true}},
  };
  REQUIRE(transcription.to_json_body()["url"] == "https://example.test/video");
  REQUIRE(transcription.to_json_body()["future"] == true);
}

TEST_CASE("video response parsers retain typed values and verbatim JSON",
          "[video][parse]") {
  const auto quote = venice::video_quote_from_json_body(
      nlohmann::json{{"quote", 0.0}, {"future", true}});
  REQUIRE(quote.quote == 0.0);
  REQUIRE(quote.raw["future"] == true);

  const auto queued = venice::video_queued_from_json_body(nlohmann::json{
      {"model", "video"},
      {"queue_id", "q1"},
      {"download_url", "https://download.test/video"},
      {"future", 1}});
  REQUIRE(queued.model == "video");
  REQUIRE(queued.queue_id == "q1");
  REQUIRE(queued.download_url == "https://download.test/video");
  REQUIRE(queued.raw["future"] == 1);

  const auto no_download = venice::video_queued_from_json_body(
      nlohmann::json{{"model", "video"}, {"queue_id", "q2"}, {"download_url", 7}});
  REQUIRE_FALSE(no_download.download_url.has_value());

  const auto processing = venice::video_processing_from_json_body(nlohmann::json{
      {"status", "FUTURE_STATUS"},
      {"average_execution_time", 145000},
      {"execution_duration", 53200.5},
      {"future", true}});
  REQUIRE(processing.status == "FUTURE_STATUS");
  REQUIRE(processing.average_execution_time == 145000.0);
  REQUIRE(processing.execution_duration == 53200.5);
  REQUIRE(processing.raw["future"] == true);

  const auto retryable = venice::video_cleanup_from_json_body(
      nlohmann::json{{"success", false}, {"future", "kept"}});
  REQUIRE_FALSE(retryable.success);
  REQUIRE(retryable.raw["future"] == "kept");

  const auto transcript = venice::video_transcription_from_json_body(
      nlohmann::json{{"transcript", "hello"}, {"lang", "en"}, {"future", 1}});
  REQUIRE(transcript.transcript == "hello");
  REQUIRE(transcript.language == "en");
  REQUIRE(transcript.raw["future"] == 1);

  const auto odd_lang = venice::video_transcription_from_json_body(
      nlohmann::json{{"transcript", "hello"}, {"lang", 7}});
  REQUIRE_FALSE(odd_lang.language.has_value());
}
