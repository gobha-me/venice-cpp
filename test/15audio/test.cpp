// Audio request/response contracts (VC-28, #43).
//
// Pure serialization, guards and parsing live here. Exact HTTP targets,
// multipart preservation, streaming and response classification use the
// loopback peer in test/06transport/. Failure matrix first, happy paths last.

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <string>
#include <variant>

#include <nlohmann/json.hpp>

#include <venice/venice.hpp>

namespace {

const venice::Client kBearer{"not-a-real-key"};

auto audio_file() -> venice::AudioFile {
  return {.bytes = std::string{"RIFF\0WAVE", 9},
          .filename = "sample.wav",
          .media_type = "audio/wav"};
}

}  // namespace

TEST_CASE("audio parsers reject malformed required response fields",
          "[audio][parse][failure]") {
  SECTION("transcription requires an object and string text") {
    REQUIRE_THROWS(venice::audio_transcription_from_json_body(nlohmann::json::array()));
    REQUIRE_THROWS(venice::audio_transcription_from_json_body(
        nlohmann::json{{"text", 7}}));
  }
  SECTION("present timestamp entries are loud") {
    auto body = nlohmann::json::parse(R"({
      "text":"hello",
      "timestamps":{"word":[{"word":"hello","start":0,"end":1}]}
    })");
    body["timestamps"]["word"][0]["end"] = "later";
    REQUIRE_THROWS(venice::audio_transcription_from_json_body(body));
    body = nlohmann::json::parse(R"({"text":"hello","timestamps":{"char":[7]}})");
    REQUIRE_THROWS(venice::audio_transcription_from_json_body(body));
  }
  SECTION("cloned voice requires both identifiers") {
    REQUIRE_THROWS(venice::cloned_voice_from_json_body(
        nlohmann::json{{"id", "vv_test"}}));
    REQUIRE_THROWS(venice::cloned_voice_from_json_body(
        nlohmann::json{{"id", 7}, {"model", "tts-test"}}));
  }
  SECTION("quote is a number") {
    REQUIRE_THROWS(venice::audio_quote_from_json_body(
        nlohmann::json{{"quote", "cheap"}}));
  }
  SECTION("queue identifiers and status are required strings") {
    REQUIRE_THROWS(venice::audio_queued_from_json_body(
        nlohmann::json{{"model", "music"}, {"queue_id", 7}, {"status", "QUEUED"}}));
  }
  SECTION("processing status requires both timing values") {
    REQUIRE_THROWS(venice::audio_processing_from_json_body(nlohmann::json{
        {"status", "PROCESSING"}, {"average_execution_time", 20}}));
    REQUIRE_THROWS(venice::audio_processing_from_json_body(nlohmann::json{
        {"status", "PROCESSING"},
        {"average_execution_time", "soon"},
        {"execution_duration", 5}}));
  }
  SECTION("cleanup success is a required boolean") {
    REQUIRE_THROWS(venice::audio_cleanup_from_json_body(nlohmann::json::object()));
    REQUIRE_THROWS(venice::audio_cleanup_from_json_body(
        nlohmann::json{{"success", "yes"}}));
  }
}

TEST_CASE("audio guards reject only structurally unsendable values",
          "[audio][guards][failure]") {
  SECTION("speech checks non-finite values before required input") {
    venice::SpeechRequest request;
    request.speed = std::numeric_limits<double>::infinity();
    const auto non_finite = kBearer.generate_speech(request);
    REQUIRE_FALSE(non_finite);
    REQUIRE(non_finite.error().is(venice::ErrorKind::InvalidArg));
    REQUIRE(non_finite.error().message == "speed is not finite");

    request.speed = 8.0;  // server-owned range; finite values pass this guard
    const auto empty = kBearer.generate_speech(request);
    REQUIRE_FALSE(empty);
    REQUIRE(empty.error().message == "speech input is empty");
  }
  SECTION("all modeled speech doubles reject NaN and infinity") {
    venice::SpeechRequest request{.input = "hello"};
    request.temperature = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(kBearer.generate_speech(request).error().message ==
            "temperature is not finite");
    request.temperature = 1.0;
    request.top_p = -std::numeric_limits<double>::infinity();
    REQUIRE(kBearer.generate_speech(request).error().message == "top_p is not finite");
  }
  SECTION("multipart file bytes name and type are required") {
    venice::AudioTranscriptionRequest request;
    request.file = audio_file();
    request.file.bytes.clear();
    REQUIRE(kBearer.transcribe_audio(request).error().message ==
            "audio file bytes are empty");
    request.file = audio_file();
    request.file.filename.clear();
    REQUIRE(kBearer.transcribe_audio(request).error().message ==
            "audio file name is empty");
    request.file = audio_file();
    request.file.media_type.clear();
    REQUIRE(kBearer.transcribe_audio(request).error().message ==
            "audio file media type is empty");

    venice::AudioVoiceCloneRequest clone;
    REQUIRE(kBearer.clone_voice(clone).error().message == "audio file bytes are empty");
  }
  SECTION("quote queue retrieve and cleanup require routing identifiers") {
    REQUIRE(kBearer.quote_audio({}).error().message == "audio model is empty");

    venice::AudioQueueRequest queue;
    queue.speed = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(kBearer.queue_audio(queue).error().message == "speed is not finite");
    queue.speed = 1.0;
    REQUIRE(kBearer.queue_audio(queue).error().message == "audio model is empty");
    queue.model = "future-music";
    REQUIRE(kBearer.queue_audio(queue).error().message == "audio prompt is empty");

    venice::AudioRetrieveRequest retrieve;
    REQUIRE(kBearer.retrieve_audio(retrieve).error().message == "audio model is empty");
    retrieve.model = "future-music";
    REQUIRE(kBearer.retrieve_audio(retrieve).error().message == "audio queue id is empty");

    venice::AudioCleanupRequest cleanup;
    REQUIRE(kBearer.cleanup_audio(cleanup).error().message == "audio model is empty");
    cleanup.model = "future-music";
    REQUIRE(kBearer.cleanup_audio(cleanup).error().message == "audio queue id is empty");
  }
}

TEST_CASE("speech serialization has one streaming source of truth",
          "[audio][request][streaming]") {
  const venice::SpeechRequest request{
      .input = "hello",
      .language = "Future Language",
      .model = "future-tts",
      .prompt = "excited",
      .response_format = "future-format",
      .speed = 3.5,
      .temperature = 1.7,
      .top_p = 0.2,
      .voice = "future-voice",
      .extra = {{"input", "shadow"}, {"streaming", "shadow"}, {"future", true}},
  };

  const auto buffered = request.to_json_body(false);
  const auto streamed = request.to_json_body(true);
  REQUIRE(buffered["input"] == "hello");
  REQUIRE(buffered["streaming"] == false);
  REQUIRE(streamed["streaming"] == true);
  REQUIRE(streamed["model"] == "future-tts");
  REQUIRE(streamed["future"] == true);
  REQUIRE(streamed["speed"] == 3.5);

  auto minimal = request;
  minimal.language.reset();
  minimal.model.reset();
  minimal.prompt.reset();
  minimal.response_format.reset();
  minimal.speed.reset();
  minimal.temperature.reset();
  minimal.top_p.reset();
  minimal.voice.reset();
  minimal.extra = nlohmann::json::array({1});
  REQUIRE(minimal.to_json_body(false) ==
          nlohmann::json{{"input", "hello"}, {"streaming", false}});
}

TEST_CASE("audio duration builders produce scalars and async fields preserve false and zero",
          "[audio][request]") {
  const auto seconds = venice::audio_duration::seconds(60);
  const auto numeric = venice::audio_duration::numeric_string("90");
  REQUIRE(seconds.is_number_integer());
  REQUIRE(seconds == 60);
  REQUIRE(numeric.is_string());
  REQUIRE(numeric == "90");

  const venice::AudioQueueRequest queue{
      .model = "future-music",
      .prompt = "make a sound",
      .lyrics_prompt = "",
      .duration_seconds = numeric,
      .force_instrumental = false,
      .lyrics_optimizer = false,
      .loop = false,
      .voice = "",
      .language_code = "",
      .speed = 0.0,
      .extra = {{"model", "shadow"}, {"future", 1}},
  };
  const auto body = queue.to_json_body();
  REQUIRE(body["model"] == "future-music");
  REQUIRE(body["duration_seconds"] == "90");
  REQUIRE(body["force_instrumental"] == false);
  REQUIRE(body["loop"] == false);
  REQUIRE(body["speed"] == 0.0);
  REQUIRE(body["future"] == 1);

  const venice::AudioRetrieveRequest retrieve{
      .model = "future-music",
      .queue_id = "queue-1",
      .delete_media_on_completion = false,
      .extra = {{"queue_id", "shadow"}, {"future", true}},
  };
  REQUIRE(retrieve.to_json_body()["queue_id"] == "queue-1");
  REQUIRE(retrieve.to_json_body()["delete_media_on_completion"] == false);
  REQUIRE(retrieve.to_json_body()["future"] == true);
}

TEST_CASE("audio response parsers retain typed values and verbatim JSON",
          "[audio][parse]") {
  const auto transcription_body = nlohmann::json::parse(R"({
    "text":"hello",
    "duration":1.25,
    "timestamps":{
      "word":[{"word":"hello","start":0,"end":1.25,"future":true}],
      "segment":[{"text":"hello","start":0,"end":1.25}],
      "char":[{"char":"h","start":0,"end":0.1}]
    },
    "future_envelope":true
  })");
  const auto transcript = venice::audio_transcription_from_json_body(transcription_body);
  REQUIRE(transcript.text == "hello");
  REQUIRE(transcript.duration == 1.25);
  REQUIRE(transcript.timestamps.has_value());
  REQUIRE(transcript.timestamps->words->front().word == "hello");
  REQUIRE(transcript.timestamps->words->front().raw["future"] == true);
  REQUIRE(transcript.timestamps->segments->front().text == "hello");
  REQUIRE(transcript.timestamps->characters->front().character == "h");
  REQUIRE(transcript.raw == transcription_body);

  const auto cloned = venice::cloned_voice_from_json_body(
      nlohmann::json{{"id", "vv_test"}, {"model", "tts-test"}, {"future", 1}});
  REQUIRE(cloned.id == "vv_test");
  REQUIRE(cloned.model == "tts-test");
  REQUIRE(cloned.raw["future"] == 1);

  const auto quote = venice::audio_quote_from_json_body(
      nlohmann::json{{"quote", 0.75}, {"future", true}});
  REQUIRE(quote.quote == 0.75);
  REQUIRE(quote.raw["future"] == true);

  const auto queued = venice::audio_queued_from_json_body(nlohmann::json{
      {"model", "music"}, {"queue_id", "q1"}, {"status", "FUTURE_STATUS"}});
  REQUIRE(queued.status == "FUTURE_STATUS");

  const auto processing = venice::audio_processing_from_json_body(nlohmann::json{
      {"status", "PROCESSING"},
      {"average_execution_time", 20000},
      {"execution_duration", 5200.5}});
  REQUIRE(processing.average_execution_time == 20000.0);
  REQUIRE(processing.execution_duration == 5200.5);

  const auto retryable = venice::audio_cleanup_from_json_body(
      nlohmann::json{{"success", false}, {"future", "kept"}});
  REQUIRE_FALSE(retryable.success);
  REQUIRE(retryable.raw["future"] == "kept");
}
