// Image generation request/response contracts (VC-40, #64).
//
// Pure serialization, guards and parsing live here. Exact HTTP targets,
// authentication, status/media classification and cancellation use the
// loopback peer in test/06transport/. Failure matrix first, happy paths last.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include <venice/venice.hpp>

namespace {

auto native_body() -> nlohmann::json {
  return nlohmann::json::parse(R"({
    "id":"img_123",
    "images":["AAEC","AwQF"],
    "request":{"prompt":"paint it","future":true},
    "timing":{
      "inferenceDuration":1250.5,
      "inferencePreprocessingTime":20,
      "inferenceQueueTime":4.25,
      "total":1274.75
    },
    "future_envelope":{"kept":true}
  })");
}

auto openai_body() -> nlohmann::json {
  return nlohmann::json::parse(R"({
    "created":1786644000,
    "data":[
      {"b64_json":"AAEC","future":"kept"},
      {"url":"data:image/png;base64,AwQF"}
    ]
  })");
}

auto minimal_native_request() -> venice::ImageGenerationRequest {
  venice::ImageGenerationRequest request;
  request.model = "image-test";
  request.prompt = "paint it";
  return request;
}

const venice::Client kClient{"not-a-real-key"};

}  // namespace

TEST_CASE("native image response rejects malformed required structure",
          "[images][parse][failure]") {
  SECTION("top level must be an object") {
    REQUIRE_THROWS(venice::native_image_generation_from_json_body(
        nlohmann::json::array()));
  }
  SECTION("id must be a string") {
    auto body = native_body();
    body["id"] = 7;
    REQUIRE_THROWS(venice::native_image_generation_from_json_body(body));
  }
  SECTION("images must be an array of strings") {
    auto body = native_body();
    body["images"] = nlohmann::json::object();
    REQUIRE_THROWS(venice::native_image_generation_from_json_body(body));
    body = native_body();
    body["images"][1] = 7;
    REQUIRE_THROWS(venice::native_image_generation_from_json_body(body));
  }
  SECTION("timing must be a complete numeric object") {
    auto body = native_body();
    body.erase("timing");
    REQUIRE_THROWS(venice::native_image_generation_from_json_body(body));
    body = native_body();
    body["timing"]["inferenceQueueTime"] = "fast";
    REQUIRE_THROWS(venice::native_image_generation_from_json_body(body));
    body = native_body();
    body["timing"].erase("total");
    REQUIRE_THROWS(venice::native_image_generation_from_json_body(body));
  }
}

TEST_CASE("OpenAI image response rejects corrupt timestamps and entries",
          "[images][openai][parse][failure]") {
  SECTION("created is a representable integer") {
    auto body = openai_body();
    body["created"] = 1.5;
    REQUIRE_THROWS(venice::openai_image_generation_from_json_body(body));
    body["created"] = "now";
    REQUIRE_THROWS(venice::openai_image_generation_from_json_body(body));
  }
  SECTION("data is required and every entry is an object") {
    auto body = openai_body();
    body.erase("data");
    REQUIRE_THROWS(venice::openai_image_generation_from_json_body(body));
    body = openai_body();
    body["data"][0] = "image";
    REQUIRE_THROWS(venice::openai_image_generation_from_json_body(body));
  }
  SECTION("an entry has a string base64 value or URL") {
    auto body = openai_body();
    body["data"][0] = nlohmann::json::object();
    REQUIRE_THROWS(venice::openai_image_generation_from_json_body(body));
    body = openai_body();
    body["data"][0]["b64_json"] = 7;
    REQUIRE_THROWS(venice::openai_image_generation_from_json_body(body));
    body = openai_body();
    body["data"][1]["url"] = nullptr;
    REQUIRE_THROWS(venice::openai_image_generation_from_json_body(body));
  }
}

TEST_CASE("image styles require a list envelope and degrade individual entries",
          "[images][styles][failure]") {
  REQUIRE_THROWS(venice::image_styles_from_json_body(nlohmann::json::array()));
  REQUIRE_THROWS(venice::image_styles_from_json_body(
      nlohmann::json{{"object", "list"}}));
  REQUIRE_THROWS(venice::image_styles_from_json_body(
      nlohmann::json{{"data", nlohmann::json::object()}}));

  const nlohmann::json body{{"data", nlohmann::json::array({"Anime", 7, nullptr, "Film Noir"})},
                            {"object", "list"},
                            {"future", true}};
  const auto styles = venice::image_styles_from_json_body(body);
  REQUIRE(styles.returned == 4);
  REQUIRE(styles.entries == std::vector<std::string>{"Anime", "Film Noir"});
  REQUIRE(styles.object == "list");
  REQUIRE(styles.raw == body);
}

TEST_CASE("image request guards reject only unsendable local structure",
          "[images][guards][failure]") {
  SECTION("non-finite modeled doubles are checked before required strings") {
    auto request = minimal_native_request();
    request.model.clear();
    request.cfg_scale = std::numeric_limits<double>::infinity();
    const auto result = kClient.generate_image(request);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().is(venice::ErrorKind::InvalidArg));
    REQUIRE(result.error().message == "cfg_scale is not finite");
  }
  SECTION("model and prompt are required") {
    auto request = minimal_native_request();
    request.model.clear();
    const auto no_model = kClient.generate_image(request);
    REQUIRE_FALSE(no_model);
    REQUIRE(no_model.error().message == "model is empty");
    request.model = "image-test";
    request.prompt.clear();
    const auto no_prompt = kClient.generate_image(request);
    REQUIRE_FALSE(no_prompt);
    REQUIRE(no_prompt.error().message == "prompt is empty");
  }
  SECTION("style reference image and finite strength are structural") {
    auto request = minimal_native_request();
    request.style_references = std::vector<venice::ImageStyleReference>{{
        .image = "",
        .strength = std::numeric_limits<double>::quiet_NaN(),
    }};
    const auto non_finite = kClient.generate_image(request);
    REQUIRE_FALSE(non_finite);
    REQUIRE(non_finite.error().message ==
            "style_references[0].strength is not finite");
    request.style_references->front().strength = 0.5;
    const auto empty_image = kClient.generate_image(request);
    REQUIRE_FALSE(empty_image);
    REQUIRE(empty_image.error().message == "style_references[0].image is empty");
  }
  SECTION("OpenAI-compatible generation requires only its prompt locally") {
    const auto result = kClient.generate_image_openai({});
    REQUIRE_FALSE(result);
    REQUIRE(result.error().message == "prompt is empty");
  }
}

TEST_CASE("image transformation guards reject structurally unsendable inputs first",
          "[images][transform][guards][failure]") {
  SECTION("upscale rejects non-finite doubles before its missing image") {
    venice::ImageUpscaleRequest request;
    request.creativity = std::numeric_limits<double>::infinity();
    const auto result = kClient.upscale_image(request);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().is(venice::ErrorKind::InvalidArg));
    REQUIRE(result.error().message == "creativity is not finite");
  }
  SECTION("upscale accepts inline data or a file, but not a URL") {
    venice::ImageUpscaleRequest request;
    request.image = venice::image_input::url("https://example.test/source.png");
    const auto result = kClient.upscale_image(request);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().message == "upscale image cannot be a URL");
  }
  SECTION("a multipart file owns bytes, filename and media type") {
    venice::ImageEditRequest request;
    request.prompt = "paint it";
    request.image = venice::image_input::file("", "source.png", "image/png");
    const auto no_bytes = kClient.edit_image(request);
    REQUIRE_FALSE(no_bytes);
    REQUIRE(no_bytes.error().message == "image file bytes are empty");

    request.image = venice::image_input::file("PNG", "", "image/png");
    const auto no_name = kClient.edit_image(request);
    REQUIRE_FALSE(no_name);
    REQUIRE(no_name.error().message == "image file name is empty");

    request.image = venice::image_input::file("PNG", "source.png", "");
    const auto no_type = kClient.edit_image(request);
    REQUIRE_FALSE(no_type);
    REQUIRE(no_type.error().message == "image file media type is empty");
  }
  SECTION("edit requires its prompt after validating the image") {
    venice::ImageEditRequest request;
    request.image = venice::image_input::base64("AAEC");
    const auto result = kClient.edit_image(request);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().message == "prompt is empty");
  }
  SECTION("multi-edit rejects empty and mixed input collections") {
    venice::MultiImageEditRequest request;
    request.prompt = "combine them";
    const auto empty = kClient.multi_edit_image(request);
    REQUIRE_FALSE(empty);
    REQUIRE(empty.error().message == "images are empty");

    request.images = {
        venice::image_input::base64("AAEC"),
        venice::image_input::file("PNG", "layer.png", "image/png"),
    };
    const auto mixed = kClient.multi_edit_image(request);
    REQUIRE_FALSE(mixed);
    REQUIRE(mixed.error().message ==
            "multi-edit images must be all files or all inline/URL values");
  }
  SECTION("background removal requires one non-empty source") {
    venice::ImageBackgroundRemovalRequest request;
    const auto missing = kClient.remove_image_background(request);
    REQUIRE_FALSE(missing);
    REQUIRE(missing.error().message == "image is empty");
  }
  SECTION("JSON passthrough is not silently discarded by multipart selection") {
    venice::ImageUpscaleRequest request;
    request.image = venice::image_input::file("PNG", "source.png", "image/png");
    request.extra = nlohmann::json{{"future", true}};
    const auto result = kClient.upscale_image(request);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().message ==
            "JSON extra fields cannot be used with multipart image input");
  }
}

TEST_CASE("image input builders retain the caller's representation verbatim",
          "[images][transform][request]") {
  const auto encoded = venice::image_input::base64("AAEC");
  REQUIRE(std::get<venice::InlineImage>(encoded).value == "AAEC");

  const auto data_url = venice::image_input::data_url("data:image/png;base64,AAEC");
  REQUIRE(std::get<venice::InlineImage>(data_url).value ==
          "data:image/png;base64,AAEC");

  const auto url = venice::image_input::url("https://example.test/source.png");
  REQUIRE(std::get<venice::ImageUrl>(url).value ==
          "https://example.test/source.png");

  const auto file = venice::image_input::file(
      std::string{{'P', 'N', char{0}, 'G'}}, "source.png", "image/png");
  const auto& uploaded = std::get<venice::ImageFile>(file);
  REQUIRE(uploaded.bytes == std::string{{'P', 'N', char{0}, 'G'}});
  REQUIRE(uploaded.filename == "source.png");
  REQUIRE(uploaded.media_type == "image/png");
}

TEST_CASE("image transformation JSON contracts keep endpoint wire spelling distinct",
          "[images][transform][request]") {
  SECTION("upscale") {
    venice::ImageUpscaleRequest request;
    request.image = venice::image_input::base64("AAEC");
    request.creativity = -4.5;  // server-owned range passes through
    request.scale = 9.25;
    request.extra = nlohmann::json{{"scale", 2}, {"future", true}};
    const auto body = request.to_json_body();
    REQUIRE(body == nlohmann::json{{"image", "AAEC"},
                                   {"creativity", -4.5},
                                   {"scale", 9.25},
                                   {"future", true}});
  }
  SECTION("single edit uses model, not the multi-edit modelId spelling") {
    venice::ImageEditRequest request;
    request.image = venice::image_input::url("https://example.test/source.png");
    request.prompt = "paint it";
    request.model = "future-edit-model";
    request.safe_mode = false;
    request.extra = nlohmann::json{{"model", "shadow"}, {"future", 7}};
    const auto body = request.to_json_body();
    REQUIRE(body["image"] == "https://example.test/source.png");
    REQUIRE(body["prompt"] == "paint it");
    REQUIRE(body["model"] == "future-edit-model");
    REQUIRE_FALSE(body.contains("modelId"));
    REQUIRE(body["safe_mode"] == false);
    REQUIRE(body["future"] == 7);
  }
  SECTION("multi-edit preserves JSON ordering and literal modelId") {
    venice::MultiImageEditRequest request;
    request.images = {venice::image_input::url("https://example.test/base.png"),
                      venice::image_input::base64("LAYER")};
    request.prompt = "combine them";
    request.model = "future-multi-model";
    request.quality = "future-quality";
    request.extra = nlohmann::json{{"modelId", "shadow"}, {"future", 8}};
    const auto body = request.to_json_body();
    REQUIRE(body["images"] == nlohmann::json::array(
                                  {"https://example.test/base.png", "LAYER"}));
    REQUIRE(body["modelId"] == "future-multi-model");
    REQUIRE_FALSE(body.contains("model"));
    REQUIRE(body["quality"] == "future-quality");
    REQUIRE(body["future"] == 8);
  }
  SECTION("background removal emits exactly one source key") {
    venice::ImageBackgroundRemovalRequest request;
    request.image = venice::image_input::url("https://example.test/source.png");
    request.extra = nlohmann::json{{"image", "shadow"},
                                   {"image_url", "shadow"},
                                   {"future", 9}};
    auto body = request.to_json_body();
    REQUIRE_FALSE(body.contains("image"));
    REQUIRE(body["image_url"] == "https://example.test/source.png");
    REQUIRE(body["future"] == 9);

    request.image = venice::image_input::base64("AAEC");
    body = request.to_json_body();
    REQUIRE(body["image"] == "AAEC");
    REQUIRE_FALSE(body.contains("image_url"));
  }
}

TEST_CASE("native image request omits unset fields and modeled values win",
          "[images][request]") {
  auto request = minimal_native_request();
  request.cfg_scale = -25.5;  // server-owned range policy passes through
  request.format = "future-format";
  request.variants = -2;
  request.enable_web_search = false;
  request.style_references = std::vector<venice::ImageStyleReference>{
      {.image = "data:image/png;base64,AAEC",
       .strength = 1.5,
       .extra = nlohmann::json{{"image", "shadow"}, {"future_nested", 42}}}};
  request.extra = nlohmann::json{{"model", "shadow"},
                                 {"prompt", "shadow"},
                                 {"format", "shadow"},
                                 {"future", true}};

  const auto body = request.to_json_body();
  REQUIRE(body["model"] == "image-test");
  REQUIRE(body["prompt"] == "paint it");
  REQUIRE(body["cfg_scale"] == -25.5);
  REQUIRE(body["format"] == "future-format");
  REQUIRE(body["variants"] == -2);
  REQUIRE(body["enable_web_search"] == false);
  REQUIRE(body["future"] == true);
  REQUIRE(body["style_references"].size() == 1);
  REQUIRE(body["style_references"][0]["image"] == "data:image/png;base64,AAEC");
  REQUIRE(body["style_references"][0]["strength"] == 1.5);
  REQUIRE(body["style_references"][0]["future_nested"] == 42);
  REQUIRE_FALSE(body.contains("height"));

  request.style_references = std::vector<venice::ImageStyleReference>{};
  REQUIRE(request.to_json_body()["style_references"].empty());
  request.style_references.reset();
  REQUIRE_FALSE(request.to_json_body().contains("style_references"));
  request.extra = nlohmann::json::array({1, 2});
  REQUIRE(request.to_json_body().is_object());
}

TEST_CASE("OpenAI-compatible request has independent fields and passthrough",
          "[images][openai][request]") {
  venice::OpenAIImageGenerationRequest request;
  request.prompt = "paint it";
  request.model = "future-model";
  request.n = -4;  // server-owned range policy passes through
  request.output_format = "future-format";
  request.response_format = "url";
  request.extra = nlohmann::json{{"prompt", "shadow"}, {"future", 9}};
  const auto body = request.to_json_body();
  REQUIRE(body["prompt"] == "paint it");
  REQUIRE(body["model"] == "future-model");
  REQUIRE(body["n"] == -4);
  REQUIRE(body["output_format"] == "future-format");
  REQUIRE(body["response_format"] == "url");
  REQUIRE(body["future"] == 9);
  REQUIRE_FALSE(body.contains("quality"));
}

TEST_CASE("image pricing keeps generation and literal upscale factors distinct",
          "[images][pricing]") {
  const auto pricing = nlohmann::json::parse(R"({
    "generation":{"usd":0.07,"diem":0.08},
    "upscale":{
      "2x":{"usd":0.02,"diem":0.03},
      "4x":{"usd":0.08,"diem":0.09}
    }
  })").get<venice::Pricing>();
  REQUIRE(pricing.generation.has_value());
  REQUIRE(pricing.generation->usd == 0.07);
  REQUIRE(pricing.generation->diem == 0.08);
  REQUIRE(pricing.upscale.has_value());
  REQUIRE(pricing.upscale->x2.has_value());
  REQUIRE(pricing.upscale->x2->usd == 0.02);
  REQUIRE(pricing.upscale->x4.has_value());
  REQUIRE(pricing.upscale->x4->diem == 0.09);
  REQUIRE_FALSE(pricing.base.input.has_value());

  auto reused = pricing;
  nlohmann::json::object().get_to(reused);
  REQUIRE_FALSE(reused.generation.has_value());
  REQUIRE_FALSE(reused.upscale.has_value());

  auto reused_upscale = *pricing.upscale;
  nlohmann::json{{"2x", {{"usd", 0.01}}}}.get_to(reused_upscale);
  REQUIRE(reused_upscale.x2.has_value());
  REQUIRE_FALSE(reused_upscale.x4.has_value());
}

TEST_CASE("image response happy paths preserve order values and raw", "[images]") {
  const auto native_json = native_body();
  const auto native = venice::native_image_generation_from_json_body(native_json);
  REQUIRE(native.id == "img_123");
  REQUIRE(native.images == std::vector<std::string>{"AAEC", "AwQF"});
  REQUIRE(native.request.has_value());
  REQUIRE((*native.request)["future"] == true);
  REQUIRE(native.timing.inference_duration == 1250.5);
  REQUIRE(native.timing.inference_preprocessing_time == 20.0);
  REQUIRE(native.timing.inference_queue_time == 4.25);
  REQUIRE(native.timing.total == 1274.75);
  REQUIRE(native.raw == native_json);

  const auto openai_json = openai_body();
  const auto openai = venice::openai_image_generation_from_json_body(openai_json);
  REQUIRE(openai.created == std::int64_t{1786644000});
  REQUIRE(openai.data.size() == 2);
  REQUIRE(openai.data[0].b64_json == "AAEC");
  REQUIRE_FALSE(openai.data[0].url.has_value());
  REQUIRE(openai.data[1].url == "data:image/png;base64,AwQF");
  REQUIRE(openai.data[0].raw["future"] == "kept");
  REQUIRE(openai.raw == openai_json);
}
