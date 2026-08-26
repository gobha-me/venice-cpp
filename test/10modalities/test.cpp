// Per-modality Model metadata (VC-39): the constraint blocks, view structs and
// wire-key tables that let an image, inpaint, video, TTS or embedding caller
// read what its request must satisfy without walking Model::raw.
//
// Not covered here, and where it lives instead: the /models request and its
// `type` filter -> test/05query/; transport, auth and the exact HTTP target ->
// test/06transport/; the listing container policy, pricing, capabilities and
// the raw hatch in general -> test/04models/. This file is only the
// per-modality half.
//
// THE FIXTURES AT THE BOTTOM OF THIS FILE ARE CAPTURES, not spec-derived. Each
// is one verbatim entry from
//
//   curl -s 'https://api.venice.ai/api/v1/models?type=<modality>'
//
// run on 2026-08-11 with NO Authorization header — every modality answers 200
// keyless. Key order, spelling and numeric spelling are as received. That
// provenance is the point of the file: VC-37 shipped a parser and a fixture
// built from the same misreading of the same document, and they agreed
// perfectly with each other. It matters twice over here, because the wire
// carries keys the specification does not: `audio_input`, `video_input`,
// `per_reference_audio` and the three `reference_image_*` values sit on all
// 111 video models and appear nowhere in swagger 20260811.123440, fetched the
// same day.
//
// The counts asserted below (37 image, 20 inpaint, 111 video, 11 tts, 9
// embedding, 5-of-106 text) come from that capture and are recorded in the
// VC-39 entry in STATUS.md. They are not asserted against the live API here —
// unit tests are offline and take no key.
//
// One rule this file must never break, the same one test/02request/ and
// test/09catalogue/ state: never hand a std::optional to nlohmann. The pinned
// nlohmann is 3.11.3 and optional support landed in 3.12.0.
//
// Failure matrix first, happy path last.

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <venice/types.hpp>

#include <set>
#include <string>
#include <vector>

using venice::models_from_json_body;

namespace {

// Fixtures — see the provenance note above. Declared at namespace scope, as
// test/04models/ does, because several sections read the same entry.

// image, with style references and no resolution/quality block.
constexpr auto kImageStyleRefs = R"J({"created":1779408000,"id":"krea-v2-large","model_spec":{"betaModel":true,"pricing":{"generation":{"usd":0.07,"diem":0.07},"upscale":{"2x":{"usd":0.02,"diem":0.02},"4x":{"usd":0.08,"diem":0.08}}},"constraints":{"promptCharacterLimit":5000,"aspectRatios":["1:1","3:2","16:9","2:3","9:16"],"defaultAspectRatio":"1:1","steps":{"default":20,"max":50},"widthHeightDivisor":1,"maxStyleReferences":3,"supportsStyleReferenceStrength":true},"supportsWebSearch":false,"supportsOptimizePromptThinking":false,"supportsStyleReferences":true,"name":"Krea v2 Large","offline":false,"privacy":"anonymized","traits":[]},"object":"model","owned_by":"venice.ai","type":"image"})J";

// image, the one entry in the capture carrying qualities/resolutions.
constexpr auto kImageQualities = R"J({"created":1776729600,"id":"gpt-image-2","model_spec":{"pricing":{"upscale":{"2x":{"usd":0.02,"diem":0.02},"4x":{"usd":0.08,"diem":0.08}}},"model_sets":["venice_recommendations","featured"],"constraints":{"promptCharacterLimit":10000,"defaultResolution":"1K","resolutions":["1K","2K","4K"],"defaultQuality":"high","qualities":["low","medium","high"],"aspectRatios":["1:1","3:2","16:9"],"defaultAspectRatio":"1:1","steps":{"default":20,"max":50},"widthHeightDivisor":1},"supportsWebSearch":false,"supportsOptimizePromptThinking":false,"supportsStyleReferences":false,"name":"GPT Image 2","offline":false,"privacy":"anonymized","traits":[]},"object":"model","owned_by":"venice.ai","type":"image"})J";

// inpaint. Shares aspectRatios, promptCharacterLimit, defaultResolution and
// resolutions with the image entries above and carries two keys no image model
// has — the overlap that makes shape-dispatch impossible.
constexpr auto kInpaint = R"J({"created":1776902400,"id":"wan-2-7-pro-edit","model_spec":{"pricing":{"inpaint":{"usd":0.094,"diem":0.094}},"constraints":{"aspectRatios":["auto"],"promptCharacterLimit":5000,"combineImages":true,"maxInputImages":6,"singleImageAspectRatio":true,"defaultResolution":"1K","resolutions":["1K","2K"]},"supportsOptimizePromptThinking":false,"name":"Wan 2.7 Pro Edit","offline":false,"privacy":"anonymized","traits":[]},"object":"model","owned_by":"venice.ai","type":"inpaint"})J";

// inpaint, the 1 of 20 carrying a quality/resolution block. Its absence from
// the entry above is not a defect in either: 19 of the 20 live inpaint models
// state no qualities at all.
constexpr auto kInpaintQualities = R"J({"created":1776729600,"id":"gpt-image-2-edit","model_spec":{"constraints":{"aspectRatios":["1:1","3:2","16:9"],"promptCharacterLimit":10000,"combineImages":true,"maxInputImages":6,"singleImageAspectRatio":true,"defaultResolution":"1K","resolutions":["1K","2K","4K"],"defaultQuality":"high","qualities":["low","medium","high"]},"supportsOptimizePromptThinking":false,"name":"GPT Image 2","offline":false,"privacy":"anonymized","traits":[]},"object":"model","owned_by":"venice.ai","type":"inpaint"})J";

// video, with an EMPTY aspect_ratios and all three undocumented
// reference_image_* values. 40 of the 111 video models sent an empty list.
constexpr auto kVideoEmptyRatios = R"J({"created":1773964800,"id":"seedance-1-5-pro-image-to-video-basic","model_spec":{"privacy":"anonymized","constraints":{"model_type":"image-to-video","aspect_ratios":[],"resolutions":["1080p","720p","480p"],"durations":["4s","5s","6s"],"audio":true,"audio_configurable":true,"audio_input":false,"per_reference_audio":false,"video_input":false,"prompt_character_limit":3500,"reference_image_min_short_side_pixels":300,"reference_image_min_aspect_ratio":0.4,"reference_image_max_aspect_ratio":2.5},"model_sets":["uncensored","high_resolution","audio"],"name":"Seedance 1.5 Pro","offline":false,"traits":[]},"object":"model","owned_by":"venice.ai","type":"video"})J";

// video, carrying the only deprecation block in the whole capture.
constexpr auto kVideoDeprecated = R"J({"created":1758825748,"id":"sora-2-image-to-video","model_spec":{"privacy":"anonymized","constraints":{"model_type":"image-to-video","aspect_ratios":["16:9","9:16"],"resolutions":["720p"],"durations":["4s","8s","12s"],"audio":true,"audio_configurable":false,"audio_input":false,"per_reference_audio":false,"video_input":false},"model_sets":["audio","cinematic","photorealistic"],"deprecation":{"autoRemap":false,"date":"2026-09-24T00:00:00.000Z","removesAt":"2026-09-24T00:00:00.000Z"},"name":"Sora 2","offline":false,"traits":[]},"object":"model","owned_by":"venice.ai","type":"video"})J";

// tts, the 1 of 11 carrying voice_cloning.
constexpr auto kTtsCloning = R"J({"created":1776384000,"id":"tts-chatterbox-hd","model_spec":{"pricing":{"input":{"usd":50,"diem":50}},"default_format":"wav","supported_formats":["wav"],"supports_custom_voice_id":false,"voices":["Aurora","Blade","Britney"],"voice_cloning":{"mode":"zero_shot","accepted_formats":["mp3","wav","flac","mp4"],"min_sample_seconds":5,"retention_days":7},"name":"Chatterbox HD (Resemble AI)","offline":false,"privacy":"private","traits":[]},"object":"model","owned_by":"venice.ai","type":"tts"})J";

// tts, one of the 10 without voice_cloning — absence here is gating, not a no.
constexpr auto kTtsNoCloning = R"J({"created":1776384000,"id":"tts-kokoro","model_spec":{"pricing":{"input":{"usd":6,"diem":6}},"default_format":"mp3","supported_formats":["mp3","wav"],"supports_custom_voice_id":false,"voices":["af_alloy","af_aoede"],"name":"Kokoro","offline":false,"privacy":"private","traits":[]},"object":"model","owned_by":"venice.ai","type":"tts"})J";

constexpr auto kEmbedding = R"J({"created":1776384000,"id":"text-embedding-qwen3-8b","model_spec":{"pricing":{"input":{"usd":0.0125,"diem":0.0125},"output":{"usd":0.0125,"diem":0.0125}},"embeddingDimensions":4096,"maxInputTokens":32768,"supportsCustomDimensions":true,"name":"Qwen3 Embedding 8B","modelSource":"https://huggingface.co/Qwen/Qwen3-Embedding-8B","offline":false,"privacy":"private","traits":[]},"object":"model","owned_by":"venice.ai","type":"embedding"})J";

// text, one of the 5 of 106 carrying constraints. Capabilities and description
// trimmed — test/04models/ owns those; what matters here is the constraints
// block and that no media view engages.
constexpr auto kTextConstraints = R"J({"context_length":256000,"created":1771977600,"id":"qwen3-5-35b-a3b","model_spec":{"betaModel":true,"pricing":{"input":{"usd":0.3125,"diem":0.3125},"output":{"usd":1.25,"diem":1.25}},"availableContextTokens":256000,"maxCompletionTokens":16384,"model_sets":["featured"],"constraints":{"temperature":{"default":1},"top_p":{"default":0.95},"repetition_penalty":{"default":1}},"name":"Qwen 3.5 35B A3B","offline":false,"privacy":"private","traits":[]},"object":"model","owned_by":"venice.ai","type":"text"})J";

// music — deliberately not modeled by VC-39. Pricing trimmed.
constexpr auto kMusic = R"J({"created":1771804800,"id":"ace-step-15","model_spec":{"supports_lyrics":true,"lyrics_required":false,"supports_force_instrumental":false,"supports_lyrics_optimizer":false,"supports_loop":false,"duration_options":[60,90,120],"min_duration":60,"max_duration":210,"default_duration":60,"supported_formats":["flac"],"default_format":"flac","prompt_character_limit":512,"lyrics_character_limit":4096,"min_prompt_length":10,"supports_custom_voice_id":false,"supports_language_code":false,"supports_speed":false,"name":"ACE-Step 1.5","offline":false,"privacy":"anonymized","traits":[]},"object":"model","owned_by":"venice.ai","type":"music"})J";

// upscale — the whole catalogue's only entry, and it carries no metadata
// beyond pricing.
constexpr auto kUpscale = R"J({"created":1744453050,"id":"upscaler","model_spec":{"pricing":{"generation":{"usd":0.01,"diem":0.01},"upscale":{"2x":{"usd":0.02,"diem":0.02},"4x":{"usd":0.08,"diem":0.08}}},"name":"Upscaler","offline":false,"privacy":"private","traits":[]},"object":"model","owned_by":"venice.ai","type":"upscale"})J";

// Parse one entry, as test/04models/'s `one` does.
auto one(std::string_view entry) -> venice::Model {
  const auto body = nlohmann::json::parse(std::string{"{\"data\":["} + std::string{entry} + "]}");
  auto models = models_from_json_body(body);
  REQUIRE(models.size() == 1);
  return models.front();
}

// An entry whose model_spec is the given body, at the given type. Lets a
// section vary one key without restating a whole capture.
auto spec_entry(std::string_view type, std::string_view spec_body) -> venice::Model {
  return one(std::string{R"({"id":"m","type":")"} + std::string{type} + R"(","model_spec":)" +
             std::string{spec_body} + "}");
}

// How many of the seven per-modality views are engaged.
auto views_engaged(const venice::Model& m) -> int {
  return static_cast<int>(m.image.has_value()) + static_cast<int>(m.inpaint.has_value()) +
         static_cast<int>(m.video.has_value()) + static_cast<int>(m.tts.has_value()) +
         static_cast<int>(m.music.has_value()) + static_cast<int>(m.embedding.has_value()) +
         static_cast<int>(m.text_constraints.has_value());
}

// Every key a table names, for the §6 set differences. Collected from the
// tables themselves rather than hand-listed — a hand-listed set would drift
// from the tables in exactly the direction this section exists to catch.
template <typename Table>
void collect(const Table& table, std::set<std::string>& out) {
  for (const auto& row : table) out.insert(row.key);
}

auto image_constraint_keys() -> std::set<std::string> {
  std::set<std::string> k;
  collect(venice::detail::kImageConstraintIntFields, k);
  collect(venice::detail::kImageConstraintBoolFields, k);
  collect(venice::detail::kImageConstraintStringFields, k);
  collect(venice::detail::kImageConstraintListFields, k);
  collect(venice::detail::kImageConstraintObjectFields, k);
  return k;
}

auto inpaint_constraint_keys() -> std::set<std::string> {
  std::set<std::string> k;
  collect(venice::detail::kInpaintConstraintIntFields, k);
  collect(venice::detail::kInpaintConstraintBoolFields, k);
  collect(venice::detail::kInpaintConstraintStringFields, k);
  collect(venice::detail::kInpaintConstraintListFields, k);
  return k;
}

auto video_constraint_keys() -> std::set<std::string> {
  std::set<std::string> k;
  collect(venice::detail::kVideoConstraintIntFields, k);
  collect(venice::detail::kVideoConstraintDoubleFields, k);
  collect(venice::detail::kVideoConstraintBoolFields, k);
  collect(venice::detail::kVideoConstraintStringFields, k);
  collect(venice::detail::kVideoConstraintListFields, k);
  return k;
}

// Keys of a JSON object.
auto keys_of(const nlohmann::json& obj) -> std::set<std::string> {
  std::set<std::string> k;
  for (const auto& [key, value] : obj.items()) k.insert(key);
  return k;
}

// In `have` but not in `want`.
auto missing_from(const std::set<std::string>& have, const std::set<std::string>& want)
    -> std::vector<std::string> {
  std::vector<std::string> out;
  for (const auto& k : have)
    if (!want.contains(k)) out.push_back(k);
  return out;
}

}  // namespace

// ── §1 the branches overlap, so dispatch is by type and never by shape ─────

TEST_CASE("an inpaint entry does not engage the image view", "[modalities][failure]") {
  const auto m = one(kInpaint);

  // Four keys this entry shares with the image captures. A parser that decided
  // "it has aspectRatios and promptCharacterLimit, so it is an image" would be
  // reading a real inpaint model as an image one, and every field it then
  // reported would be wrong about a request it cannot serve.
  const auto& constraints = m.raw["model_spec"]["constraints"];
  REQUIRE(constraints.contains("aspectRatios"));
  REQUIRE(constraints.contains("promptCharacterLimit"));
  REQUIRE(constraints.contains("defaultResolution"));
  REQUIRE(constraints.contains("resolutions"));

  REQUIRE(!m.image.has_value());
  REQUIRE(m.inpaint.has_value());
  REQUIRE(m.inpaint->constraints.has_value());
  REQUIRE(m.inpaint->constraints->max_input_images == 6);
}

TEST_CASE("an image entry does not engage the inpaint view", "[modalities][failure]") {
  const auto m = one(kImageQualities);
  REQUIRE(m.image.has_value());
  REQUIRE(!m.inpaint.has_value());
}

TEST_CASE("image-family pricing retains generation and both upscale factors",
          "[modalities][pricing]") {
  // Verbatim catalogue captures, not a pricing object assembled from the
  // schema. This is the response-side proof that the new buckets are read at
  // the level Venice actually sends them.
  const auto image = one(kImageStyleRefs);
  REQUIRE(image.pricing.has_value());
  REQUIRE(image.pricing->generation.has_value());
  REQUIRE(image.pricing->generation->usd == 0.07);
  REQUIRE(image.pricing->upscale.has_value());
  REQUIRE(image.pricing->upscale->x2.has_value());
  REQUIRE(image.pricing->upscale->x2->usd == 0.02);
  REQUIRE(image.pricing->upscale->x4.has_value());
  REQUIRE(image.pricing->upscale->x4->usd == 0.08);

  const auto upscale = one(kUpscale);
  REQUIRE(upscale.pricing.has_value());
  REQUIRE(upscale.pricing->generation.has_value());
  REQUIRE(upscale.pricing->generation->usd == 0.01);
  REQUIRE(upscale.pricing->upscale.has_value());
  REQUIRE(upscale.pricing->upscale->x2.has_value());
  REQUIRE(upscale.pricing->upscale->x4.has_value());
  REQUIRE(upscale.pricing->upscale->x2->usd == 0.02);
  REQUIRE(upscale.pricing->upscale->x4->usd == 0.08);
}

TEST_CASE("a type this client does not model engages nothing and keeps raw",
          "[modalities][failure]") {
  const auto upscale = one(kUpscale);
  REQUIRE(views_engaged(upscale) == 0);
  REQUIRE(upscale.pricing.has_value());

  const auto unknown = spec_entry("holograph", R"({"name":"Later","constraints":{"depth":3}})");
  REQUIRE(views_engaged(unknown) == 0);
  REQUIRE(unknown.name == "Later");
  REQUIRE(unknown.raw["model_spec"]["constraints"]["depth"] == 3);
}

// ── §2 at most one view, always ────────────────────────────────────────────

// What this section can and cannot see, stated because the difference is not
// obvious: it holds "the dispatch selects one branch", and the break matrix
// showed it does NOT go red when from_json's else-if chain is rewritten as
// independent ifs — at most one string comparison matches either way, so the
// chain is readability rather than the guarantee. The break it does see is the
// dispatch keying on something other than `type`, which is §1.
TEST_CASE("exactly one view engages, across every modeled modality",
          "[modalities][failure]") {
  for (const auto* entry : {kImageStyleRefs, kImageQualities, kInpaint, kInpaintQualities,
                            kVideoEmptyRatios, kVideoDeprecated, kTtsCloning, kTtsNoCloning,
                            kEmbedding, kTextConstraints})
    REQUIRE(views_engaged(one(entry)) == 1);

  // A text model without constraints engages none of the six — the one
  // modeled type for which zero is the right answer, because text's other
  // metadata is flat on Model rather than in a view.
  const auto plain_text = spec_entry("text", R"({"name":"Plain","availableContextTokens":8192})");
  REQUIRE(views_engaged(plain_text) == 0);
  REQUIRE(plain_text.available_context_tokens == 8192);
}

// ── §3 wrong-typed numbers degrade to absent, never to a confident wrong
//      answer ──────────────────────────────────────────────────────────────

TEST_CASE("a fractional int reads absent rather than truncating", "[modalities][failure]") {
  // get<int>() on 1.9 returns 1 on the pinned nlohmann and throws nothing. A
  // steps default of 1 where the server said 1.9 is the kind of number nobody
  // re-reads.
  const auto m = spec_entry("image", R"({"constraints":{"steps":{"default":1.9,"max":50}}})");
  REQUIRE(m.image->constraints->steps.has_value());
  REQUIRE(!m.image->constraints->steps->default_value.has_value());
  REQUIRE(m.image->constraints->steps->max == 50);
}

TEST_CASE("a past-INT_MAX prompt limit reads absent rather than narrowing",
          "[modalities][failure]") {
  const auto m = spec_entry("image", R"({"constraints":{"promptCharacterLimit":99999999999999}})");
  REQUIRE(!m.image->constraints->prompt_character_limit.has_value());
}

TEST_CASE("a fractional aspect ratio bound survives as a double", "[modalities]") {
  // The counterweight to the two above: reading these with opt_int would make
  // every one of the twelve live models carrying them report absent, and
  // 0.4 is the actual value on the wire.
  const auto m = spec_entry(
      "video", R"({"constraints":{"reference_image_min_aspect_ratio":0.4,)"
               R"("reference_image_max_aspect_ratio":2.5,)"
               R"("reference_image_min_short_side_pixels":300}})");
  REQUIRE(m.video->constraints->reference_image_min_aspect_ratio == 0.4);
  REQUIRE(m.video->constraints->reference_image_max_aspect_ratio == 2.5);
  REQUIRE(m.video->constraints->reference_image_min_short_side_pixels == 300);
}

TEST_CASE("a whole-number aspect ratio bound still parses", "[modalities]") {
  // opt_double accepts is_number(), not is_number_float(). A model quoting a
  // square bound as 1 rather than 1.0 must not read as absent — the same call
  // Pricing made, for the same reason.
  const auto m = spec_entry("video", R"({"constraints":{"reference_image_min_aspect_ratio":2}})");
  REQUIRE(m.video->constraints->reference_image_min_aspect_ratio == 2.0);
}

TEST_CASE("a wrong-typed constraint leaves its neighbours parsed", "[modalities][failure]") {
  const auto m = spec_entry("video",
                            R"({"constraints":{"model_type":42,"audio":"yes",)"
                            R"("prompt_character_limit":3500,"video_input":true}})");
  REQUIRE(!m.video->constraints->model_type.has_value());
  REQUIRE(!m.video->constraints->audio.has_value());
  REQUIRE(m.video->constraints->prompt_character_limit == 3500);
  REQUIRE(m.video->constraints->video_input == true);
}

TEST_CASE("a non-object constraints block reads as absent, not as a throw",
          "[modalities][failure]") {
  const auto m = spec_entry("image", R"({"constraints":[],"supportsWebSearch":true})");
  REQUIRE(m.image.has_value());
  REQUIRE(!m.image->constraints.has_value());
  REQUIRE(m.image->supports_web_search == true);
}

// ── §4 an empty list is an answer, an absent one is not ────────────────────

TEST_CASE("video aspect_ratios engages when empty and disengages when absent",
          "[modalities][failure]") {
  // The specification assigns `[]` the meaning "the model does not support a
  // defined aspect ratio", and 40 of the 111 live video models sent exactly
  // that. Flattening the two onto an empty vector would answer "which ratios
  // may I ask for?" with silence in both cases.
  const auto empty = one(kVideoEmptyRatios);
  REQUIRE(empty.video->constraints->aspect_ratios.has_value());
  REQUIRE(empty.video->constraints->aspect_ratios->empty());
  REQUIRE(empty.video->constraints->resolutions.has_value());
  REQUIRE(empty.video->constraints->resolutions->size() == 3);

  const auto absent = spec_entry("video", R"({"constraints":{"model_type":"video"}})");
  REQUIRE(!absent.video->constraints->aspect_ratios.has_value());
  REQUIRE(!absent.video->constraints->resolutions.has_value());
  REQUIRE(absent.video->constraints->model_type == "video");

  // A non-array reads as absent, not as empty.
  const auto malformed = spec_entry("video", R"({"constraints":{"aspect_ratios":"16:9"}})");
  REQUIRE(!malformed.video->constraints->aspect_ratios.has_value());
}

TEST_CASE("model_sets flattens absent onto empty, unlike the constraint lists",
          "[modalities]") {
  // The deliberate opposite call, on the same struct, for the reason stated on
  // Model::traits: a tag set has no caller that branches on absent-vs-empty.
  const auto with = one(kVideoDeprecated);
  REQUIRE(with.model_sets.size() == 3);
  REQUIRE(with.model_sets.front() == "audio");

  const auto without = one(kInpaint);
  REQUIRE(without.model_sets.empty());
}

// ── §5 wire spelling is per-modality, in the same wire position ────────────

TEST_CASE("image reads camelCase and video reads snake_case", "[modalities][failure]") {
  // Same concept, same position in the payload, two spellings. A mechanical
  // case rule would read one family as absent on every entry.
  const auto image = spec_entry("image", R"({"constraints":{"promptCharacterLimit":5000}})");
  REQUIRE(image.image->constraints->prompt_character_limit == 5000);

  const auto video = spec_entry("video", R"({"constraints":{"prompt_character_limit":3500}})");
  REQUIRE(video.video->constraints->prompt_character_limit == 3500);

  // And each is deaf to the other's spelling, which is what makes the two
  // tables load-bearing rather than decorative.
  const auto image_wrong = spec_entry("image", R"({"constraints":{"prompt_character_limit":5000}})");
  REQUIRE(!image_wrong.image->constraints->prompt_character_limit.has_value());

  const auto video_wrong = spec_entry("video", R"({"constraints":{"promptCharacterLimit":3500}})");
  REQUIRE(!video_wrong.video->constraints->prompt_character_limit.has_value());

  const auto image_ratios = spec_entry("image", R"({"constraints":{"aspectRatios":["1:1"]}})");
  REQUIRE(image_ratios.image->constraints->aspect_ratios.has_value());
  const auto video_ratios = spec_entry("video", R"({"constraints":{"aspect_ratios":["1:1"]}})");
  REQUIRE(video_ratios.video->constraints->aspect_ratios.has_value());
}

// ── §6 the tables against the captures, in both directions ────────────────

TEST_CASE("every table key is one the live payload actually sends", "[modalities]") {
  // Forward: a typo'd key yields nullopt forever and is invisible to a test
  // that hand-copies the same typo. Only a verbatim capture can see it.
  //
  // Checked against the union of the captures for that modality, because no
  // single entry carries every optional key — image's qualities are on 1 of 37
  // and its style-reference keys on 4.
  const auto image_seen = [] {
    std::set<std::string> k;
    for (const auto* e : {kImageStyleRefs, kImageQualities})
      for (const auto& key : keys_of(one(e).raw["model_spec"]["constraints"])) k.insert(key);
    return k;
  }();
  for (const auto& key : image_constraint_keys()) {
    INFO("image constraint key: " << key);
    REQUIRE(image_seen.contains(key));
  }

  const auto inpaint_seen = [] {
    std::set<std::string> k;
    for (const auto* e : {kInpaint, kInpaintQualities})
      for (const auto& key : keys_of(one(e).raw["model_spec"]["constraints"])) k.insert(key);
    return k;
  }();
  for (const auto& key : inpaint_constraint_keys()) {
    INFO("inpaint constraint key: " << key);
    REQUIRE(inpaint_seen.contains(key));
  }

  const auto video_seen = [] {
    std::set<std::string> k;
    for (const auto* e : {kVideoEmptyRatios, kVideoDeprecated})
      for (const auto& key : keys_of(one(e).raw["model_spec"]["constraints"])) k.insert(key);
    return k;
  }();
  for (const auto& key : video_constraint_keys()) {
    INFO("video constraint key: " << key);
    REQUIRE(video_seen.contains(key));
  }
}

TEST_CASE("every key the live payload sends is one some table names", "[modalities]") {
  // Reverse, and the only direction that can see a DELETED row: the forward
  // check above passes happily with a field missing from both struct and
  // table, because it only ever asks about keys the tables already name.
  REQUIRE(missing_from(keys_of(one(kImageStyleRefs).raw["model_spec"]["constraints"]),
                       image_constraint_keys())
              .empty());
  REQUIRE(missing_from(keys_of(one(kImageQualities).raw["model_spec"]["constraints"]),
                       image_constraint_keys())
              .empty());
  REQUIRE(missing_from(keys_of(one(kInpaint).raw["model_spec"]["constraints"]),
                       inpaint_constraint_keys())
              .empty());
  REQUIRE(missing_from(keys_of(one(kInpaintQualities).raw["model_spec"]["constraints"]),
                       inpaint_constraint_keys())
              .empty());
  REQUIRE(missing_from(keys_of(one(kVideoEmptyRatios).raw["model_spec"]["constraints"]),
                       video_constraint_keys())
              .empty());
  REQUIRE(missing_from(keys_of(one(kVideoDeprecated).raw["model_spec"]["constraints"]),
                       video_constraint_keys())
              .empty());

  // The nested objects, one level down.
  std::set<std::string> steps_keys;
  collect(venice::detail::kStepsIntFields, steps_keys);
  REQUIRE(missing_from(keys_of(one(kImageStyleRefs).raw["model_spec"]["constraints"]["steps"]),
                       steps_keys)
              .empty());

  std::set<std::string> cloning_keys;
  collect(venice::detail::kVoiceCloningStringFields, cloning_keys);
  collect(venice::detail::kVoiceCloningDoubleFields, cloning_keys);
  collect(venice::detail::kVoiceCloningIntFields, cloning_keys);
  collect(venice::detail::kVoiceCloningListFields, cloning_keys);
  REQUIRE(
      missing_from(keys_of(one(kTtsCloning).raw["model_spec"]["voice_cloning"]), cloning_keys)
          .empty());

  std::set<std::string> deprecation_keys;
  collect(venice::detail::kDeprecationBoolFields, deprecation_keys);
  collect(venice::detail::kDeprecationStringFields, deprecation_keys);
  REQUIRE(missing_from(keys_of(one(kVideoDeprecated).raw["model_spec"]["deprecation"]),
                       deprecation_keys)
              .empty());

  std::set<std::string> text_keys;
  collect(venice::detail::kTextConstraintObjectFields, text_keys);
  REQUIRE(missing_from(keys_of(one(kTextConstraints).raw["model_spec"]["constraints"]), text_keys)
              .empty());
}

TEST_CASE("the modeled spec-level keys cover what each capture carries", "[modalities]") {
  // Same reverse check one level up, where the per-modality views live beside
  // the fields Model has read since VC-03.
  const std::set<std::string> shared{"name",        "description",           "privacy",
                                     "modelSource", "offline",               "betaModel",
                                     "traits",      "availableContextTokens", "maxCompletionTokens",
                                     "capabilities", "pricing",              "constraints",
                                     "model_sets",  "deprecation"};

  auto covered = [&](std::set<std::string> extra) {
    auto all = shared;
    all.insert(extra.begin(), extra.end());
    return all;
  };

  std::set<std::string> image_spec;
  collect(venice::detail::kImageSpecBoolFields, image_spec);
  collect(venice::detail::kImageSpecObjectFields, image_spec);
  REQUIRE(missing_from(keys_of(one(kImageStyleRefs).raw["model_spec"]), covered(image_spec))
              .empty());

  std::set<std::string> tts_spec;
  collect(venice::detail::kTtsSpecBoolFields, tts_spec);
  collect(venice::detail::kTtsSpecStringFields, tts_spec);
  collect(venice::detail::kTtsSpecListFields, tts_spec);
  collect(venice::detail::kTtsSpecObjectFields, tts_spec);
  REQUIRE(missing_from(keys_of(one(kTtsCloning).raw["model_spec"]), covered(tts_spec)).empty());

  std::set<std::string> music_spec;
  collect(venice::detail::kMusicSpecBoolFields, music_spec);
  collect(venice::detail::kMusicSpecIntFields, music_spec);
  collect(venice::detail::kMusicSpecDoubleFields, music_spec);
  collect(venice::detail::kMusicSpecStringFields, music_spec);
  collect(venice::detail::kMusicSpecIntListFields, music_spec);
  collect(venice::detail::kMusicSpecStringListFields, music_spec);
  REQUIRE(missing_from(keys_of(one(kMusic).raw["model_spec"]), covered(music_spec)).empty());

  std::set<std::string> embedding_spec;
  collect(venice::detail::kEmbeddingSpecIntFields, embedding_spec);
  collect(venice::detail::kEmbeddingSpecBoolFields, embedding_spec);
  REQUIRE(missing_from(keys_of(one(kEmbedding).raw["model_spec"]), covered(embedding_spec)).empty());
}

// ── §7 an absent voice_cloning is not a "no" ───────────────────────────────

TEST_CASE("voice_cloning absent leaves the optional disengaged", "[modalities][failure]") {
  // The specification states that models whose cloning is gated behind a
  // private alpha omit this field for non-staff callers while still appearing
  // in the listing. An engaged empty struct here would report "clones, with no
  // accepted formats and no minimum sample" for a model that may clone fine.
  const auto without = one(kTtsNoCloning);
  REQUIRE(without.tts.has_value());
  REQUIRE(!without.tts->voice_cloning.has_value());
  REQUIRE(without.tts->voices.has_value());

  const auto with = one(kTtsCloning);
  REQUIRE(with.tts->voice_cloning.has_value());
  REQUIRE(with.tts->voice_cloning->mode == "zero_shot");
  REQUIRE(with.tts->voice_cloning->min_sample_seconds == 5.0);
  REQUIRE(with.tts->voice_cloning->retention_days == 7);
  REQUIRE(with.tts->voice_cloning->accepted_formats.has_value());
  REQUIRE(with.tts->voice_cloning->accepted_formats->size() == 4);
  REQUIRE(with.tts->voice_cloning->accepted_formats->front() == "mp3");
}

// ── §8 deprecation instants stay strings ───────────────────────────────────

TEST_CASE("deprecation carries ISO 8601 strings, not epochs", "[modalities]") {
  const auto m = one(kVideoDeprecated);
  REQUIRE(m.deprecation.has_value());
  REQUIRE(m.deprecation->auto_remap == false);
  REQUIRE(m.deprecation->date == "2026-09-24T00:00:00.000Z");
  REQUIRE(m.deprecation->removes_at == "2026-09-24T00:00:00.000Z");
  // Documented, never observed in the 2026-08-11 capture. Modeled because they
  // cost one table row each inside a struct that exists anyway; absent here is
  // the honest answer and not a parse failure.
  REQUIRE(!m.deprecation->starts_at.has_value());
  REQUIRE(!m.deprecation->replacement_model_id.has_value());

  // Cross-modality: read for every type, not inside a view.
  REQUIRE(!one(kInpaint).deprecation.has_value());

  const auto text_dep = spec_entry(
      "text", R"({"deprecation":{"autoRemap":true,"removesAt":"2027-01-01T00:00:00.000Z",)"
              R"("replacementModelId":"llama-3-3-70b"}})");
  REQUIRE(text_dep.deprecation->auto_remap == true);
  REQUIRE(text_dep.deprecation->replacement_model_id == "llama-3-3-70b");
}

// ── §9 model_sets, across modalities ───────────────────────────────────────

TEST_CASE("model_sets is read for every type", "[modalities]") {
  // Undocumented in swagger 20260811.123440 and on all 111 video, 17 of 37
  // image and 13 of 106 text models on 2026-08-11 — which is why it sits on
  // Model rather than in any one view.
  REQUIRE(one(kVideoEmptyRatios).model_sets ==
          std::vector<std::string>{"uncensored", "high_resolution", "audio"});
  REQUIRE(one(kImageQualities).model_sets ==
          std::vector<std::string>{"venice_recommendations", "featured"});
  REQUIRE(one(kTextConstraints).model_sets == std::vector<std::string>{"featured"});
}

// ── §10 raw is still the whole entry ───────────────────────────────────────

TEST_CASE("every modeled field is still in raw", "[modalities]") {
  // The superset doctrine: a subtractive hatch breaks its readers on every
  // graduation, and this change graduates twenty-odd keys at once. Code that
  // read raw["model_spec"]["constraints"]["durations"] before VC-39 must still
  // read it after.
  const auto video = one(kVideoEmptyRatios);
  const auto& spec = video.raw["model_spec"];
  REQUIRE(spec["constraints"]["durations"].size() == 3);
  REQUIRE(spec["constraints"]["audio_input"] == false);
  REQUIRE(spec["constraints"]["reference_image_min_aspect_ratio"] == 0.4);
  REQUIRE(spec["model_sets"].size() == 3);
  REQUIRE(video.raw["id"] == "seedance-1-5-pro-image-to-video-basic");

  const auto tts = one(kTtsCloning);
  REQUIRE(tts.raw["model_spec"]["voice_cloning"]["mode"] == "zero_shot");
  REQUIRE(tts.raw["model_spec"]["voices"].size() == 3);

  const auto image = one(kImageStyleRefs);
  REQUIRE(image.raw["model_spec"]["constraints"]["steps"]["max"] == 50);
  REQUIRE(image.raw["model_spec"]["supportsStyleReferences"] == true);
}

// ── §11 happy path, one per modality ───────────────────────────────────────

TEST_CASE("an image model states what an image request must satisfy", "[modalities]") {
  const auto m = one(kImageStyleRefs);
  const auto& c = *m.image->constraints;
  REQUIRE(c.prompt_character_limit == 5000);
  REQUIRE(c.width_height_divisor == 1);
  REQUIRE(c.default_aspect_ratio == "1:1");
  REQUIRE(c.aspect_ratios->size() == 5);
  REQUIRE(c.aspect_ratios->at(2) == "16:9");  // a string, never the integer 969
  REQUIRE(c.steps->default_value == 20);
  REQUIRE(c.steps->max == 50);
  REQUIRE(c.max_style_references == 3);
  REQUIRE(c.supports_style_reference_strength == true);
  // Absent on this entry, present on gpt-image-2 — both are real answers.
  REQUIRE(!c.resolutions.has_value());
  REQUIRE(!c.qualities.has_value());

  // The three flags that sit above constraints, where a text model would have
  // a capabilities block. This entry has none, and none is synthesised.
  REQUIRE(m.image->supports_style_references == true);
  REQUIRE(m.image->supports_web_search == false);
  REQUIRE(m.image->supports_optimize_prompt_thinking == false);
  REQUIRE(!m.capabilities.has_value());

  const auto full = one(kImageQualities);
  REQUIRE(full.image->constraints->resolutions->size() == 3);
  REQUIRE(full.image->constraints->qualities->at(2) == "high");
  REQUIRE(full.image->constraints->default_quality == "high");
  REQUIRE(full.image->constraints->default_resolution == "1K");
}

TEST_CASE("an inpaint model states its input-image rules", "[modalities]") {
  const auto m = one(kInpaint);
  const auto& c = *m.inpaint->constraints;
  REQUIRE(c.prompt_character_limit == 5000);
  REQUIRE(c.combine_images == true);
  REQUIRE(c.max_input_images == 6);
  REQUIRE(c.single_image_aspect_ratio == true);
  REQUIRE(c.aspect_ratios == std::vector<std::string>{"auto"});  // "auto" is a ratio here
  REQUIRE(c.resolutions->size() == 2);
  REQUIRE(m.inpaint->supports_optimize_prompt_thinking == false);
}

TEST_CASE("a video model states its durations and its input kinds", "[modalities]") {
  const auto m = one(kVideoEmptyRatios);
  const auto& c = *m.video->constraints;
  REQUIRE(c.model_type == "image-to-video");
  REQUIRE(c.durations == std::vector<std::string>{"4s", "5s", "6s"});  // strings with a unit
  REQUIRE(c.prompt_character_limit == 3500);
  REQUIRE(c.audio == true);
  REQUIRE(c.audio_configurable == true);
  // The undocumented half, and the half an image-to-video caller needs first.
  REQUIRE(c.audio_input == false);
  REQUIRE(c.per_reference_audio == false);
  REQUIRE(c.video_input == false);
  REQUIRE(c.reference_image_min_short_side_pixels == 300);
  REQUIRE(c.reference_image_min_aspect_ratio == 0.4);
  REQUIRE(c.reference_image_max_aspect_ratio == 2.5);
  // Video sent no pricing on any of the 111 entries.
  REQUIRE(!m.pricing.has_value());
}

TEST_CASE("a tts model states its voices and formats", "[modalities]") {
  const auto m = one(kTtsCloning);
  REQUIRE(m.tts->default_format == "wav");
  REQUIRE(m.tts->supported_formats == std::vector<std::string>{"wav"});
  REQUIRE(m.tts->supports_custom_voice_id == false);
  REQUIRE(m.tts->voices->size() == 3);
  REQUIRE(m.tts->voices->front() == "Aurora");
}

TEST_CASE("a music model states the request policy Audio consumes", "[modalities][audio]") {
  const auto m = one(kMusic);
  REQUIRE(m.type == "music");
  REQUIRE(views_engaged(m) == 1);
  REQUIRE(m.music.has_value());
  REQUIRE(m.music->supports_lyrics == true);
  REQUIRE(m.music->lyrics_required == false);
  REQUIRE(m.music->supports_force_instrumental == false);
  REQUIRE(m.music->default_duration == 60);
  REQUIRE(m.music->duration_options == std::vector<int>{60, 90, 120});
  REQUIRE(m.music->min_prompt_length == 10);
  REQUIRE(m.music->prompt_character_limit == 512);
  REQUIRE(m.music->lyrics_character_limit == 4096);
  REQUIRE(m.music->default_format == "flac");
  REQUIRE(m.music->supported_formats == std::vector<std::string>{"flac"});
  REQUIRE(m.raw["model_spec"]["supports_lyrics"] == true);
  REQUIRE(m.name == "ACE-Step 1.5");
}

TEST_CASE("an embedding model states its dimensions", "[modalities]") {
  const auto m = one(kEmbedding);
  REQUIRE(m.embedding->embedding_dimensions == 4096);
  REQUIRE(m.embedding->max_input_tokens == 32768);
  REQUIRE(m.embedding->supports_custom_dimensions == true);
  REQUIRE(m.model_source == "https://huggingface.co/Qwen/Qwen3-Embedding-8B");
}

TEST_CASE("a text model states its sampling defaults", "[modalities]") {
  const auto m = one(kTextConstraints);
  REQUIRE(m.text_constraints.has_value());
  // {"default": 1} — an object with one key, not a bare number. `steps` is the
  // standing evidence that such an object can grow a second.
  REQUIRE(m.text_constraints->temperature->default_value == 1.0);
  REQUIRE(m.text_constraints->top_p->default_value == 0.95);
  REQUIRE(m.text_constraints->repetition_penalty->default_value == 1.0);
  // Documented by the specification, never observed on the wire, not modeled —
  // and still reachable if Venice starts sending them.
  REQUIRE(!m.raw["model_spec"]["constraints"].contains("frequency_penalty"));

  // The text metadata VC-03 modeled is untouched by any of this.
  REQUIRE(m.context_length == 256000);
  REQUIRE(m.available_context_tokens == 256000);
  REQUIRE(m.max_completion_tokens == 16384);
  REQUIRE(m.beta_model == true);
}
