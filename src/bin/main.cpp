// venice-cpp smoke binary — exercises the client against the live API when a
// key is present, else prints the library shape. Real usage comes later; for
// now this proves the header-only client links and runs.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include <venice/venice.hpp>

namespace {

// ── choosing a model from the live catalogue ──────────────────────────────
//
// A model chosen by capability, plus the runners-up it beat. Both halves are
// the point. AGENTS.md makes naming the alternates a rule after VC-18, where
// --tools auto-picked one family and nothing on screen suggested the next one
// would answer differently — it did, and the bug read as a library defect for
// as long as it did because of that.
//
// VC-17 (#28) is the second bug that rule would have caught, and this helper
// exists because writing the rule down in one leg did not put it in the others.
// #28 was filed as "Venice does not send Usage's nested detail objects" on the
// strength of a --stream run against `gemini-3-6-flash` — which is simply the
// first supportsReasoning entry in /models?type=text, and one of the two models
// in a seven-model sweep that reports neither field. Five of the other six send
// prompt_tokens_details.cached_tokens and two send
// completion_tokens_details.reasoning_tokens, at exactly the nesting the ticket
// called unobserved. One model settles nothing about a per-family shape.
struct ModelPick {
  std::string chosen{};
  std::vector<std::string> alternates{};
};

// nullopt after printing why: the list call failed, or nothing claimed the flag.
// Both are this leg's failure to report, not something a caller can fix.
//
// The flag is a pointer-to-member rather than a name, so a typo cannot compile
// — the same reason detail::kCapabilityBoolFields pairs the field with its wire
// key instead of looking one up by string.
auto pick_by_capability(const venice::Client& client,
                        std::optional<bool> venice::ModelCapabilities::*flag,
                        std::string_view flag_name,
                        std::size_t max_alternates = 4) -> std::optional<ModelPick> {
  const auto models = client.models("text");
  if (!models) {
    std::cerr << "models failed [" << venice::to_string(models.error().kind) << "] "
              << models.error().message << '\n';
    return std::nullopt;
  }

  ModelPick pick;
  for (const auto& m : *models) {
    if (!m.capabilities) continue;
    const auto& claimed = (*m.capabilities).*flag;
    if (!claimed || !*claimed) continue;
    if (pick.chosen.empty())
      pick.chosen = m.id;
    else if (pick.alternates.size() < max_alternates)
      pick.alternates.push_back(m.id);
    else
      break;
  }

  if (pick.chosen.empty()) {
    std::cerr << "no text model reported " << flag_name
              << " -- either none does, or that flag is not where Model expects it\n";
    return std::nullopt;
  }
  return pick;
}

// The embedding endpoint chooses by modality rather than by a flag inside a
// text model's capabilities. It still returns ModelPick so the reporting rule
// stays shared: an auto-picked family never appears alone on screen.
auto pick_by_modality(const venice::Client& client, std::string_view type,
                      std::size_t max_alternates = 4) -> std::optional<ModelPick> {
  const auto models = client.models(type);
  if (!models) {
    std::cerr << "models(" << type << ") failed [" << venice::to_string(models.error().kind)
              << "] " << models.error().message << '\n';
    return std::nullopt;
  }

  ModelPick pick;
  for (const auto& model : *models) {
    if (model.id.empty()) continue;
    if (pick.chosen.empty())
      pick.chosen = model.id;
    else if (pick.alternates.size() < max_alternates)
      pick.alternates.push_back(model.id);
    else
      break;
  }
  if (pick.chosen.empty()) {
    std::cerr << "no " << type << " model carried a usable id\n";
    return std::nullopt;
  }
  return pick;
}

// Edit and multi-edit share the inpaint catalogue, but the latter needs a
// model that says it combines images. Selecting on that typed constraint makes
// the live leg exercise the exact metadata the request is meant to consult.
auto pick_for_image_edit(const venice::Client& client,
                         std::size_t max_alternates = 4)
    -> std::optional<ModelPick> {
  const auto models = client.models("inpaint");
  if (!models) {
    std::cerr << "models(inpaint) failed ["
              << venice::to_string(models.error().kind) << "] "
              << models.error().message << '\n';
    return std::nullopt;
  }

  ModelPick pick;
  for (const auto& model : *models) {
    if (!model.inpaint || !model.inpaint->constraints ||
        model.inpaint->constraints->combine_images != true || model.id.empty())
      continue;
    if (pick.chosen.empty())
      pick.chosen = model.id;
    else if (pick.alternates.size() < max_alternates)
      pick.alternates.push_back(model.id);
    else
      break;
  }
  if (pick.chosen.empty()) {
    std::cerr << "no inpaint model reported combineImages=true\n";
    return std::nullopt;
  }
  return pick;
}

// `rerun` names the command that runs this same leg on a named model, because
// a list of alternates nobody can act on is decoration. Prints nothing beyond
// the model when the caller named one — there are no runners-up to a choice
// that was not made here.
void report_pick(const ModelPick& pick, std::string_view flag_name, std::string_view rerun) {
  std::cerr << "model: " << pick.chosen << '\n';
  if (pick.alternates.empty()) return;
  std::cerr << "others claiming " << flag_name << ": ";
  for (std::size_t i = 0; i < pick.alternates.size(); ++i)
    std::cerr << (i != 0U ? ", " : "") << pick.alternates[i];
  std::cerr << "\n  (" << rerun
            << " runs this same leg on any of them; server behaviour that varies by\n"
               "   model family is invisible to a leg that only ever runs one)\n";
}

// `--models [type]`: the live check for Model's typed metadata and, since
// VC-13, for the type filter. Prints the fields a picker would actually branch
// on, so a field that parses in a fixture but not on the wire shows up as a
// blank column rather than passing quietly.
//
// `type` is passed through verbatim — text, image, video, tts, embedding,
// inpaint, music, asr, upscale, all — and an empty one sends no filter, which
// Venice answers with text models only. The runs worth doing:
//
//   --models          the pre-VC-13 behaviour, and it must still equal
//   --models text     this one exactly
//   --models all      every modality; roughly three times the bare count
//
// Deliberately no expected numbers here: Venice's catalogue moved by twelve
// models in the ten days between VC-03 and VC-13, so a count written into a
// comment is wrong within the month. The bare-equals-text relation is the part
// that holds.
//
// /models answers 200 for any bearer token, so VENICE_API_KEY=unused is enough.
auto list_models(const venice::Client& client, std::string_view type) -> int {
  const auto res = client.models(type);
  if (!res) {
    std::cerr << "models failed [" << venice::to_string(res.error().kind) << "] "
              << res.error().message << '\n';
    return EXIT_FAILURE;
  }

  std::cout << res->size() << " models";
  if (!type.empty()) std::cout << " (type=" << type << ')';
  std::cout << '\n';

  for (const auto& m : *res) {
    // The type column earns its place once non-text models are listable: for
    // those, every column after it is a `?`, and without it the output reads
    // like a parse failure rather than a different model_spec shape.
    std::cout << m.id << "  [" << m.type << "]  ctx=";
    if (m.context_length) std::cout << *m.context_length;
    else std::cout << '?';

    std::cout << "  tools=";
    if (m.capabilities && m.capabilities->supports_function_calling)
      std::cout << (*m.capabilities->supports_function_calling ? "yes" : "no");
    else std::cout << '?';

    std::cout << "  in=$";
    if (m.pricing && m.pricing->base.input && m.pricing->base.input->usd)
      std::cout << *m.pricing->base.input->usd;
    else std::cout << '?';

    if (m.name) std::cout << "  (" << *m.name << ')';
    std::cout << '\n';
  }
  return EXIT_SUCCESS;
}

// Name the keys an object carries that nothing here models.
//
// One implementation, and that is the point rather than the tidiness. This was
// written twice — once for `usage`, once for the envelope beside it — and
// VC-36's reviews leg would have been the third copy. AGENTS.md's rule is that
// a leg reporting a sub-object reports the object it came out of; a rule with a
// copy per leg is a rule the next leg is exempt from, which is exactly how
// `cost` rode untyped for three releases.
//
// It lives up here, above its first caller, rather than beside the chat report
// sets it was extracted from — the alternative is a forward declaration whose
// only purpose is to keep the definition somewhere prettier.
//
// The is_object guard is not defensive: nlohmann's items() on a scalar yields
// one pair with an empty key, so a body that is not an object would otherwise
// be reported as carrying an unmodeled key named "".
void report_unmodeled(std::string_view label, const nlohmann::json& obj,
                      std::span<const std::string_view> modeled,
                      std::string_view reachable_via) {
  if (!obj.is_object()) return;
  std::vector<std::string> unmodeled;
  for (const auto& [k, v] : obj.items())
    if (std::find(modeled.begin(), modeled.end(), k) == modeled.end()) unmodeled.push_back(k);
  if (unmodeled.empty()) return;

  std::cerr << label;
  for (std::size_t i = 0; i < unmodeled.size(); ++i)
    std::cerr << (i != 0U ? ", " : "") << unmodeled[i];
  std::cerr << "   (reachable via " << reachable_via << ")\n";
}

// ── `--embeddings [model]` (VC-26, #41) ─────────────────────────────────
//
// Two paid calls because the wire question is itself two-valued. The OpenAPI
// request offers float and base64 while its 200 schema describes only an array;
// a float-only leg would therefore leave the disputed half unmeasured. Each run
// prints the complete envelope and independently reconciles the raw value type
// with the variant selected by the parser.

constexpr std::array<std::string_view, 4> kModeledEmbeddingBodyKeys{
    "data", "model", "object", "usage"};
constexpr std::array<std::string_view, 3> kModeledEmbeddingEntryKeys{
    "embedding", "index", "object"};
constexpr std::array<std::string_view, 2> kModeledEmbeddingUsageKeys{
    "prompt_tokens", "total_tokens"};

auto embeddings_report(const venice::Client& client, std::string_view model) -> int {
  ModelPick pick;
  if (model.empty()) {
    auto selected = pick_by_modality(client, "embedding");
    if (!selected) return EXIT_FAILURE;
    pick = std::move(*selected);
  } else {
    pick.chosen = std::string{model};
  }
  report_pick(pick, "type=embedding", "`venice-cpp --embeddings <id>`");

  const auto run = [&](std::string_view format) -> bool {
    venice::EmbeddingRequest request;
    request.model = pick.chosen;
    request.input = venice::embedding_input::text("The quick brown fox jumped over the lazy dog");
    request.encoding_format = std::string{format};

    const auto response = client.embeddings(request);
    if (!response) {
      std::cerr << format << " embeddings failed [" << venice::to_string(response.error().kind)
                << "] " << response.error().message << '\n';
      if (!response.error().body.empty()) std::cerr << response.error().body << '\n';
      return false;
    }

    std::cerr << "\n-- " << format << ", envelope verbatim --\n"
              << response->raw.dump(2) << '\n';
    report_unmodeled("unmodeled envelope keys: ", response->raw, kModeledEmbeddingBodyKeys,
                     "EmbeddingResponse::raw");
    if (const auto* usage = venice::detail::opt_object(response->raw, "usage"))
      report_unmodeled("unmodeled usage keys: ", *usage, kModeledEmbeddingUsageKeys,
                       "EmbeddingResponse::raw[\"usage\"]");

    if (response->data.empty()) {
      std::cerr << "EMPTY DATA -- one input produced no embedding\n";
      return false;
    }

    bool agrees = true;
    for (std::size_t i = 0; i < response->data.size(); ++i) {
      const auto& entry = response->data[i];
      report_unmodeled("unmodeled entry keys: ", entry.raw, kModeledEmbeddingEntryKeys,
                       "Embedding::raw");
      const auto raw = entry.raw.find("embedding");
      const bool raw_float = raw != entry.raw.end() && raw->is_array();
      const bool raw_base64 = raw != entry.raw.end() && raw->is_string();
      const bool typed_float = std::holds_alternative<std::vector<double>>(entry.value);
      const bool typed_base64 = std::holds_alternative<std::string>(entry.value);
      const bool expected = format == "float" ? typed_float : typed_base64;
      if (raw_float != typed_float || raw_base64 != typed_base64 || !expected) {
        std::cerr << "entry " << i << " RAW/TYPED FORMAT MISMATCH\n";
        agrees = false;
      }
      std::cerr << "entry " << i << ": index=" << entry.index << ", typed="
                << (typed_float ? "float[" +
                                      std::to_string(std::get<std::vector<double>>(entry.value).size()) +
                                      "]"
                                : "base64[" +
                                      std::to_string(std::get<std::string>(entry.value).size()) +
                                      "]")
                << '\n';
    }
    if (response->metadata.x_balance_remaining)
      std::cerr << "X-Balance-Remaining: " << *response->metadata.x_balance_remaining << '\n';
    return agrees;
  };

  const bool floats = run("float");
  const bool base64 = run("base64");
  return floats && base64 ? EXIT_SUCCESS : EXIT_FAILURE;
}

// ── image generation and public styles (VC-40, #64) ─────────────────────

constexpr std::array<std::string_view, 4> kModeledNativeImageKeys{
    "id", "images", "request", "timing"};
constexpr std::array<std::string_view, 4> kModeledImageTimingKeys{
    "inferenceDuration", "inferencePreprocessingTime", "inferenceQueueTime", "total"};
constexpr std::array<std::string_view, 2> kModeledOpenAIImageKeys{"created", "data"};
constexpr std::array<std::string_view, 2> kModeledOpenAIImageEntryKeys{"b64_json", "url"};
constexpr std::array<std::string_view, 2> kModeledImageStyleKeys{"data", "object"};

auto image_styles_report(const venice::Client& client) -> int {
  const auto response = client.image_styles();
  if (!response) {
    std::cerr << "image styles failed [" << venice::to_string(response.error().kind) << "] "
              << response.error().message << '\n';
    if (!response.error().body.empty()) std::cerr << response.error().body << '\n';
    return EXIT_FAILURE;
  }

  std::cerr << "image styles, envelope verbatim:\n" << response->raw.dump(2) << '\n';
  report_unmodeled("unmodeled style envelope keys: ", response->raw,
                   kModeledImageStyleKeys, "ImageStyles::raw");
  std::cout << response->entries.size() << " usable styles (" << response->returned
            << " values arrived)\n";
  for (const auto& style : response->entries) std::cout << "  " << style << '\n';

  if (response->returned != response->entries.size()) {
    std::cerr << "STYLE COUNT MISMATCH -- a non-string entry arrived; see ImageStyles::raw\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

auto image_report(const venice::Client& client, std::string_view model) -> int {
  ModelPick pick;
  if (model.empty()) {
    auto selected = pick_by_modality(client, "image");
    if (!selected) return EXIT_FAILURE;
    pick = std::move(*selected);
  } else {
    pick.chosen = std::string{model};
  }
  report_pick(pick, "type=image", "`venice-cpp --image <id>`");

  const std::string prompt =
      "A small blue ceramic teacup on a plain white background, product photograph";
  bool agrees = true;

  venice::ImageGenerationRequest native;
  native.model = pick.chosen;
  native.prompt = prompt;
  native.format = "png";
  native.variants = 1;
  native.safe_mode = true;
  native.return_binary = false;
  const auto json_result = client.generate_image(native);
  if (!json_result) {
    std::cerr << "native JSON generation failed ["
              << venice::to_string(json_result.error().kind) << "] "
              << json_result.error().message << '\n';
    if (!json_result.error().body.empty()) std::cerr << json_result.error().body << '\n';
    agrees = false;
  } else if (const auto* json =
                 std::get_if<venice::NativeImageGenerationResponse>(&*json_result)) {
    std::cerr << "\n-- native JSON, envelope verbatim --\n" << json->raw.dump(2) << '\n';
    report_unmodeled("unmodeled native image keys: ", json->raw,
                     kModeledNativeImageKeys, "NativeImageGenerationResponse::raw");
    if (const auto* timing = venice::detail::opt_object(json->raw, "timing"))
      report_unmodeled("unmodeled image timing keys: ", *timing,
                       kModeledImageTimingKeys,
                       "NativeImageGenerationResponse::raw[\"timing\"]");
    std::cerr << "typed images: " << json->images.size() << ", id=" << json->id << '\n';
    const auto* raw_images = venice::detail::opt_array(json->raw, "images");
    if (raw_images == nullptr || raw_images->size() != json->images.size()) {
      std::cerr << "RAW/TYPED IMAGE COUNT MISMATCH\n";
      agrees = false;
    }
  } else {
    std::cerr << "native JSON request returned media instead of JSON\n";
    agrees = false;
  }

  native.return_binary = true;
  const auto binary_result = client.generate_image(native);
  if (!binary_result) {
    std::cerr << "native binary generation failed ["
              << venice::to_string(binary_result.error().kind) << "] "
              << binary_result.error().message << '\n';
    if (!binary_result.error().body.empty()) std::cerr << binary_result.error().body << '\n';
    agrees = false;
  } else if (const auto* media =
                 std::get_if<venice::GeneratedImageMedia>(&*binary_result)) {
    std::cerr << "\n-- native binary --\nmedia type: " << media->media_type
              << "\nbytes: " << media->bytes.size() << '\n';
    if (media->bytes.empty()) {
      std::cerr << "EMPTY IMAGE MEDIA\n";
      agrees = false;
    }
  } else {
    std::cerr << "native binary request returned JSON instead of media\n";
    agrees = false;
  }

  venice::OpenAIImageGenerationRequest openai;
  openai.prompt = prompt;
  openai.model = pick.chosen;
  openai.output_format = "png";
  openai.response_format = "b64_json";
  openai.n = 1;
  const auto openai_result = client.generate_image_openai(openai);
  if (!openai_result) {
    std::cerr << "OpenAI-compatible generation failed ["
              << venice::to_string(openai_result.error().kind) << "] "
              << openai_result.error().message << '\n';
    if (!openai_result.error().body.empty()) std::cerr << openai_result.error().body << '\n';
    agrees = false;
  } else {
    std::cerr << "\n-- OpenAI-compatible JSON, envelope verbatim --\n"
              << openai_result->raw.dump(2) << '\n';
    report_unmodeled("unmodeled OpenAI image keys: ", openai_result->raw,
                     kModeledOpenAIImageKeys, "OpenAIImageGenerationResponse::raw");
    for (const auto& entry : openai_result->data)
      report_unmodeled("unmodeled OpenAI image entry keys: ", entry.raw,
                       kModeledOpenAIImageEntryKeys, "OpenAIImageGenerationEntry::raw");
    std::cerr << "typed images: " << openai_result->data.size()
              << ", created=" << openai_result->created << '\n';
    const auto* raw_data = venice::detail::opt_array(openai_result->raw, "data");
    if (raw_data == nullptr || raw_data->size() != openai_result->data.size()) {
      std::cerr << "RAW/TYPED OPENAI IMAGE COUNT MISMATCH\n";
      agrees = false;
    }
  }

  std::cerr << "\nNo image bytes were decoded or written to disk.\n";
  return agrees ? EXIT_SUCCESS : EXIT_FAILURE;
}

// `--audio [tts-model] [asr-model] [music-model]` (VC-28, #43)
//
// This deliberately exercises only operations without persistent remote state:
// buffered and streamed speech, transcription of the generated bytes, and a
// quote. Voice cloning and the queue/retrieve/cleanup workflow stay in the
// loopback suite; a smoke command must not mint a voice or enqueue paid work
// merely because somebody wanted to check a build.
auto audio_report(const venice::Client& client, std::string_view tts_model,
                  std::string_view asr_model, std::string_view music_model) -> int {
  const auto select = [&](std::string_view type, std::string_view named)
      -> std::optional<ModelPick> {
    if (named.empty()) return pick_by_modality(client, type);
    return ModelPick{.chosen = std::string{named}};
  };

  auto tts_pick = select("tts", tts_model);
  auto asr_pick = select("asr", asr_model);
  auto music_pick = select("music", music_model);
  if (!tts_pick || !asr_pick || !music_pick) return EXIT_FAILURE;

  std::cerr << "\n-- speech model --\n";
  report_pick(*tts_pick, "type=tts",
              "`venice-cpp --audio <tts-id> [asr-id] [music-id]`");
  std::cerr << "\n-- transcription model --\n";
  report_pick(*asr_pick, "type=asr",
              "`venice-cpp --audio <tts-id> <asr-id> [music-id]`");
  std::cerr << "\n-- quote model --\n";
  report_pick(*music_pick, "type=music",
              "`venice-cpp --audio <tts-id> <asr-id> <music-id>`");

  const auto tts_models = client.models("tts");
  if (!tts_models) {
    std::cerr << "models(tts) failed [" << venice::to_string(tts_models.error().kind)
              << "] " << tts_models.error().message << '\n';
    return EXIT_FAILURE;
  }
  const auto tts = std::find_if(tts_models->begin(), tts_models->end(),
                                [&](const venice::Model& model) {
                                  return model.id == tts_pick->chosen;
                                });
  if (tts == tts_models->end() || !tts->tts) {
    std::cerr << "selected TTS model is absent from the typed TTS catalogue: "
              << tts_pick->chosen << '\n';
    return EXIT_FAILURE;
  }

  venice::SpeechRequest speech;
  speech.input = "The Venice C plus plus audio smoke check is working.";
  speech.model = tts_pick->chosen;
  speech.response_format = tts->tts->default_format;
  if (tts->tts->voices && !tts->tts->voices->empty())
    speech.voice = tts->tts->voices->front();

  const auto buffered = client.generate_speech(speech);
  if (!buffered) {
    std::cerr << "buffered speech failed ["
              << venice::to_string(buffered.error().kind) << "] "
              << buffered.error().message << '\n';
    if (!buffered.error().body.empty()) std::cerr << buffered.error().body << '\n';
    return EXIT_FAILURE;
  }
  std::cerr << "\nbuffered speech: " << buffered->bytes.size() << " bytes, "
            << buffered->media_type << '\n';
  if (buffered->bytes.empty()) {
    std::cerr << "EMPTY BUFFERED SPEECH MEDIA\n";
    return EXIT_FAILURE;
  }

  std::size_t streamed_bytes = 0;
  const auto streamed = client.generate_speech_stream(
      speech, [&](std::string_view bytes) {
        streamed_bytes += bytes.size();
        return true;
      });
  if (!streamed) {
    std::cerr << "streamed speech failed ["
              << venice::to_string(streamed.error().kind) << "] "
              << streamed.error().message << '\n';
    if (!streamed.error().body.empty()) std::cerr << streamed.error().body << '\n';
    return EXIT_FAILURE;
  }
  std::cerr << "streamed speech: " << streamed_bytes << " bytes, "
            << streamed->media_type
            << ", completed=" << (streamed->completed ? "true" : "false") << '\n';
  if (streamed_bytes == 0U || !streamed->completed) {
    std::cerr << "INCOMPLETE STREAMED SPEECH MEDIA\n";
    return EXIT_FAILURE;
  }

  std::string extension = "audio";
  if (buffered->media_type == "audio/mpeg") extension = "mp3";
  else if (buffered->media_type == "audio/wav") extension = "wav";
  else if (buffered->media_type == "audio/flac") extension = "flac";
  else if (buffered->media_type == "audio/aac") extension = "aac";
  else if (buffered->media_type == "audio/opus") extension = "opus";
  else if (buffered->media_type == "audio/pcm") extension = "pcm";

  venice::AudioTranscriptionRequest transcription;
  transcription.file = venice::AudioFile{.bytes = buffered->bytes,
                                         .filename = "smoke." + extension,
                                         .media_type = buffered->media_type};
  transcription.model = asr_pick->chosen;
  transcription.response_format = "json";
  const auto transcript = client.transcribe_audio(transcription);
  if (!transcript) {
    std::cerr << "transcription failed ["
              << venice::to_string(transcript.error().kind) << "] "
              << transcript.error().message << '\n';
    if (!transcript.error().body.empty()) std::cerr << transcript.error().body << '\n';
    return EXIT_FAILURE;
  }
  const auto* json_transcript =
      std::get_if<venice::JsonAudioTranscription>(&*transcript);
  if (json_transcript == nullptr) {
    std::cerr << "transcription requested JSON but returned text media\n";
    return EXIT_FAILURE;
  }
  std::cerr << "\ntranscription, envelope verbatim:\n"
            << json_transcript->raw.dump(2) << '\n';
  std::cerr << "typed transcript: " << json_transcript->text << '\n';
  if (venice::detail::opt_string(json_transcript->raw, "text") !=
      std::optional<std::string>{json_transcript->text}) {
    std::cerr << "RAW/TYPED TRANSCRIPT MISMATCH\n";
    return EXIT_FAILURE;
  }

  const auto music_models = client.models("music");
  if (!music_models) {
    std::cerr << "models(music) failed ["
              << venice::to_string(music_models.error().kind) << "] "
              << music_models.error().message << '\n';
    return EXIT_FAILURE;
  }
  const auto music = std::find_if(music_models->begin(), music_models->end(),
                                  [&](const venice::Model& model) {
                                    return model.id == music_pick->chosen;
                                  });
  if (music == music_models->end() || !music->music) {
    std::cerr << "selected music model is absent from the typed music catalogue: "
              << music_pick->chosen << '\n';
    return EXIT_FAILURE;
  }

  venice::AudioQuoteRequest quote_request;
  quote_request.model = music_pick->chosen;
  if (music->music->default_duration)
    quote_request.duration_seconds =
        venice::audio_duration::seconds(*music->music->default_duration);
  else
    quote_request.character_count = 40;
  const auto quote = client.quote_audio(quote_request);
  if (!quote) {
    std::cerr << "audio quote failed [" << venice::to_string(quote.error().kind)
              << "] " << quote.error().message << '\n';
    if (!quote.error().body.empty()) std::cerr << quote.error().body << '\n';
    return EXIT_FAILURE;
  }
  std::cerr << "\naudio quote, envelope verbatim:\n" << quote->raw.dump(2)
            << "\ntyped quote: $" << quote->quote << '\n';
  if (venice::detail::opt_double(quote->raw, "quote") !=
      std::optional<double>{quote->quote}) {
    std::cerr << "RAW/TYPED AUDIO QUOTE MISMATCH\n";
    return EXIT_FAILURE;
  }
  std::cerr << "\nNo audio bytes were decoded, played or written to disk; no voice was\n"
               "cloned and no asynchronous work was queued.\n";
  return EXIT_SUCCESS;
}

// `--video [model]` (VC-29, #44) is intentionally quote-only. Queueing video
// creates paid asynchronous work and cleanup deletes remote state, so neither
// belongs in a smoke command. The catalogue chooses only a model whose typed
// policy supplies a duration; a named model must satisfy the same contract.
auto video_report(const venice::Client& client, std::string_view named_model) -> int {
  const auto models = client.models("video");
  if (!models) {
    std::cerr << "models(video) failed ["
              << venice::to_string(models.error().kind) << "] "
              << models.error().message << '\n';
    return EXIT_FAILURE;
  }

  ModelPick pick;
  const venice::VideoConstraints* constraints = nullptr;
  for (const auto& model : *models) {
    if (model.id.empty() || !model.video || !model.video->constraints ||
        !model.video->constraints->durations ||
        model.video->constraints->durations->empty())
      continue;
    if (!named_model.empty()) {
      if (model.id == named_model) {
        pick.chosen = model.id;
        constraints = &*model.video->constraints;
        break;
      }
      continue;
    }
    if (pick.chosen.empty()) {
      pick.chosen = model.id;
      constraints = &*model.video->constraints;
    } else if (pick.alternates.size() < 4U) {
      pick.alternates.push_back(model.id);
    }
  }
  if (pick.chosen.empty() || constraints == nullptr) {
    std::cerr << (named_model.empty()
                      ? "no video model carried a usable id and non-empty duration policy\n"
                      : "named model is absent or has no typed video duration policy: " +
                            std::string{named_model} + "\n");
    return EXIT_FAILURE;
  }
  report_pick(pick, "type=video and a non-empty duration policy",
              "`venice-cpp --video <id>`");

  venice::VideoQuoteRequest request;
  request.model = pick.chosen;
  request.duration = constraints->durations->front();
  if (constraints->aspect_ratios && !constraints->aspect_ratios->empty())
    request.aspect_ratio = constraints->aspect_ratios->front();
  if (constraints->resolutions && !constraints->resolutions->empty())
    request.resolution = constraints->resolutions->front();

  std::cerr << "quote request: " << request.to_json_body().dump() << '\n';
  const auto quote = client.quote_video(request);
  if (!quote) {
    std::cerr << "video quote failed [" << venice::to_string(quote.error().kind)
              << "] " << quote.error().message << '\n';
    if (!quote.error().body.empty()) std::cerr << quote.error().body << '\n';
    return EXIT_FAILURE;
  }
  std::cerr << "video quote, envelope verbatim:\n" << quote->raw.dump(2)
            << "\ntyped quote: $" << quote->quote << '\n';
  if (venice::detail::opt_double(quote->raw, "quote") !=
      std::optional<double>{quote->quote}) {
    std::cerr << "RAW/TYPED VIDEO QUOTE MISMATCH\n";
    return EXIT_FAILURE;
  }
  std::cerr << "\nNo video work was queued or polled, no remote media was deleted, and\n"
               "no media bytes were decoded, displayed or written to disk.\n";
  return EXIT_SUCCESS;
}

// `--augment` (VC-32, #47) performs exactly one minimal call to each direct
// source-material operation. These calls may consume account balance. Inputs
// are synthetic/public, and the leg never prints uploaded or returned content:
// only shapes, counts and raw-to-typed agreement leave the process.
auto augment_report(const venice::Client &client) -> int {
  bool agrees = true;

  const std::string document = "venice-cpp augment smoke check\n";
  const auto parsed =
      client.parse_document({.file = {.bytes = document,
                                      .filename = "venice-cpp-smoke.txt",
                                      .media_type = "text/plain"},
                             .response_format = "json"});
  if (!parsed) {
    std::cerr << "document parse failed ["
              << venice::to_string(parsed.error().kind) << "] "
              << parsed.error().message << '\n';
    return EXIT_FAILURE;
  }
  const auto *json = std::get_if<venice::JsonDocumentParse>(&*parsed);
  if (json == nullptr) {
    std::cerr << "document parser requested JSON but returned text media\n";
    return EXIT_FAILURE;
  }
  const bool document_agrees =
      venice::detail::opt_string(json->raw, "text") ==
          std::optional<std::string>{json->text} &&
      venice::detail::opt_double(json->raw, "tokens") ==
          std::optional<double>{json->tokens};
  std::cerr << "document: " << json->text.size() << " extracted chars, "
            << json->tokens
            << " tokens; raw/typed=" << (document_agrees ? "agree" : "MISMATCH")
            << '\n';
  agrees = document_agrees && agrees;

  const auto scraped = client.scrape_web({.url = "https://example.com"});
  if (!scraped) {
    std::cerr << "web scrape failed ["
              << venice::to_string(scraped.error().kind) << "] "
              << scraped.error().message << '\n';
    return EXIT_FAILURE;
  }
  const bool scrape_agrees =
      venice::detail::opt_string(scraped->raw, "url") ==
          std::optional<std::string>{scraped->url} &&
      venice::detail::opt_string(scraped->raw, "content") ==
          std::optional<std::string>{scraped->content} &&
      venice::detail::opt_string(scraped->raw, "format") ==
          std::optional<std::string>{scraped->format};
  std::cerr << "scrape: " << scraped->content.size()
            << " chars, format=" << scraped->format
            << "; raw/typed=" << (scrape_agrees ? "agree" : "MISMATCH") << '\n';
  agrees = scrape_agrees && agrees;

  const auto searched =
      client.search_web({.query = "Venice AI API", .limit = 1});
  if (!searched) {
    std::cerr << "web search failed ["
              << venice::to_string(searched.error().kind) << "] "
              << searched.error().message << '\n';
    return EXIT_FAILURE;
  }
  const auto *raw_results = venice::detail::opt_array(searched->raw, "results");
  bool search_agrees =
      raw_results != nullptr && raw_results->size() == searched->returned &&
      searched->query == venice::detail::opt_string(searched->raw, "query");
  if (raw_results != nullptr && !raw_results->empty() &&
      !searched->results.empty() && raw_results->front().is_object()) {
    const auto &raw = raw_results->front();
    const auto &typed = searched->results.front();
    search_agrees =
        search_agrees &&
        typed.title == venice::detail::opt_string(raw, "title") &&
        typed.url == venice::detail::opt_string(raw, "url") &&
        typed.content == venice::detail::opt_string(raw, "content") &&
        typed.date == venice::detail::opt_string(raw, "date");
  }
  std::cerr << "search: " << searched->results.size() << " usable of "
            << searched->returned
            << " returned; raw/typed=" << (search_agrees ? "agree" : "MISMATCH")
            << '\n';
  agrees = search_agrees && agrees;

  std::cerr << "No uploaded or returned content was printed, saved or retained "
               "by this leg.\n";
  return agrees ? EXIT_SUCCESS : EXIT_FAILURE;
}

// ── crypto RPC (VC-33, #48) ──────────────────────────────────
//
// This first proves public discovery with a per-call auth override, then makes
// one minimal read-only EVM call and an identical idempotent replay. The first
// call is billed; no transaction is signed or submitted.
auto crypto_rpc_report(const venice::Client& client) -> int {
  const auto networks = client.crypto_rpc_networks(
      {.authentication = venice::Authentication::public_access()});
  if (!networks) {
    std::cerr << "crypto RPC network discovery failed ["
              << venice::to_string(networks.error().kind) << "] "
              << networks.error().message << '\n';
    return EXIT_FAILURE;
  }

  constexpr std::string_view kNetwork = "ethereum-mainnet";
  if (std::find(networks->networks.begin(), networks->networks.end(), kNetwork) ==
      networks->networks.end()) {
    std::cerr << kNetwork << " is absent from " << networks->networks.size()
              << " advertised crypto RPC networks\n";
    return EXIT_FAILURE;
  }
  std::cerr << "crypto RPC networks: " << networks->networks.size()
            << "; selected " << kNetwork << '\n';

  const auto request = venice::crypto_rpc_input::request(
      "eth_chainId", nlohmann::json::array(), nlohmann::json(1));
  const venice::RequestOptions opts{
      .idempotency_key = "vc33-crypto-rpc-chain-id-smoke"};
  const auto first = client.crypto_rpc(kNetwork, request, opts);
  if (!first) {
    std::cerr << "crypto RPC failed [" << venice::to_string(first.error().kind)
              << "] " << first.error().message << '\n';
    if (!first.error().body.empty()) std::cerr << first.error().body << '\n';
    return EXIT_FAILURE;
  }
  const auto replay = client.crypto_rpc(kNetwork, request, opts);
  if (!replay) {
    std::cerr << "crypto RPC replay failed ["
              << venice::to_string(replay.error().kind) << "] "
              << replay.error().message << '\n';
    return EXIT_FAILURE;
  }

  const auto* item = std::get_if<venice::CryptoRpcResponseItem>(&first->payload);
  const auto* replay_item =
      std::get_if<venice::CryptoRpcResponseItem>(&replay->payload);
  if (item == nullptr || replay_item == nullptr || !item->result ||
      !replay_item->result || item->id != nlohmann::json(1) ||
      replay_item->id != item->id || replay_item->result != item->result ||
      item->raw != first->raw) {
    std::cerr << "crypto RPC raw/typed or replay mismatch\n";
    return EXIT_FAILURE;
  }
  if (replay->metadata.header("Idempotent-Replayed") != "true") {
    std::cerr << "crypto RPC replay lacked Idempotent-Replayed: true\n";
    return EXIT_FAILURE;
  }

  std::cerr << "eth_chainId result: " << item->result->dump()
            << "; raw/typed=agree; replay=confirmed";
  if (const auto credits = first->metadata.header("X-Venice-RPC-Credits"))
    std::cerr << "; credits=" << *credits;
  if (const auto cost = first->metadata.header("X-Venice-RPC-Cost-USD"))
    std::cerr << "; cost_usd=" << *cost;
  std::cerr << '\n';
  return EXIT_SUCCESS;
}

// ── x402 payment discovery (VC-34, #49) ─────────────────────────────────
//
// The only safe credential-free live leg for this family. It sends an empty
// POST, receives the payment choices Venice is currently advertising and stops
// there: no wallet key, SIWX proof or payment signature is available to this
// process, and no balance is changed.
constexpr std::array<std::string_view, 2> kModeledX402RequirementKeys{
    "x402Version", "accepts"};
constexpr std::array<std::string_view, 7> kModeledX402OptionKeys{
    "scheme", "network", "amount", "asset", "payTo", "maxTimeoutSeconds",
    "extra"};

auto x402_report(const venice::Client& client) -> int {
  const auto result = client.x402_top_up();
  if (!result) {
    std::cerr << "x402 discovery failed ["
              << venice::to_string(result.error().kind) << "] "
              << result.error().message << '\n';
    if (!result.error().body.empty()) std::cerr << result.error().body << '\n';
    return EXIT_FAILURE;
  }

  const auto* requirements =
      std::get_if<venice::X402PaymentRequirements>(&*result);
  if (requirements == nullptr) {
    std::cerr << "an empty public top-up unexpectedly returned a payment receipt\n";
    return EXIT_FAILURE;
  }

  std::cerr << "x402 requirements, envelope verbatim:\n"
            << requirements->raw.dump(2) << '\n';
  report_unmodeled("unmodeled x402 requirement keys: ", requirements->raw,
                   kModeledX402RequirementKeys,
                   "X402PaymentRequirements::raw");

  const auto* raw_accepts =
      venice::detail::opt_array(requirements->raw, "accepts");
  bool agrees =
      venice::detail::opt_int(requirements->raw, "x402Version") ==
          std::optional<int>{requirements->x402_version} &&
      raw_accepts != nullptr &&
      raw_accepts->size() == requirements->accepts.size();
  if (raw_accepts != nullptr) {
    for (std::size_t i = 0;
         i < raw_accepts->size() && i < requirements->accepts.size(); ++i) {
      const auto& raw = raw_accepts->at(i);
      const auto& typed = requirements->accepts.at(i);
      report_unmodeled("unmodeled x402 option keys: ", raw,
                       kModeledX402OptionKeys, "X402PaymentOption::raw");
      const auto* extra = venice::detail::opt_object(raw, "extra");
      agrees = agrees &&
               venice::detail::opt_string(raw, "scheme") ==
                   std::optional<std::string>{typed.scheme} &&
               venice::detail::opt_string(raw, "network") ==
                   std::optional<std::string>{typed.network} &&
               venice::detail::opt_string(raw, "amount") ==
                   std::optional<std::string>{typed.amount} &&
               venice::detail::opt_string(raw, "asset") ==
                   std::optional<std::string>{typed.asset} &&
               venice::detail::opt_string(raw, "payTo") ==
                   std::optional<std::string>{typed.pay_to} &&
               venice::detail::opt_int(raw, "maxTimeoutSeconds") ==
                   std::optional<int>{typed.max_timeout_seconds} &&
               ((extra == nullptr && !typed.network_extra) ||
                (extra != nullptr && typed.network_extra &&
                 *extra == *typed.network_extra));
    }
  }

  if (!requirements->metadata.payment_required) {
    std::cerr << "x402 discovery lacked the opaque PAYMENT-REQUIRED header\n";
    agrees = false;
  }
  std::cerr << "x402 version " << requirements->x402_version << ": "
            << requirements->accepts.size()
            << " accepted payment options; raw/typed="
            << (agrees ? "agree" : "MISMATCH") << '\n';
  for (const auto& option : requirements->accepts)
    std::cerr << "  " << option.network << " / " << option.asset
              << " / base units " << option.amount << '\n';
  std::cerr << "No payment payload was signed or submitted; no wallet balance was changed.\n";
  return agrees ? EXIT_SUCCESS : EXIT_FAILURE;
}

// ── image transformations (VC-41, #66) ─────────────────────────────────
//
// A valid 256x256 solid-colour PNG, kept as base64 so the smoke leg performs
// no decode and no filesystem I/O. Every operation receives the same owned
// in-memory value; only the request contract changes.
constexpr std::string_view kTransformSourceBase64 =
    "iVBORw0KGgoAAAANSUhEUgAAAQAAAAEACAIAAADTED8xAAACAElEQVR42u3TQQ0AAAjEsJOCNKQijTcaaFIFS5bqgbciAQYA"
    "A4ABwABgADAAGAAMAAYAA4ABwABgADAAGAAMAAYAA4ABwABgADAAGAAMAAYAA4ABwABgADAAGAAMAAYAA4ABwABgADAAGAAM"
    "AAYAA4ABwABgADAAGAAMAAYAA4ABMIAKGAAMAAYAA4ABwABgADAAGAAMAAYAA4ABwABgADAAGAAMAAYAA4ABwABgADAAGAAM"
    "AAYAA4ABwABgADAAGAAMAAYAA4ABwABgADAAGAAMAAYAA4ABwABgADAAGAAMAAbAAGAAMAAYAAwABgADgAHAAGAAMAAYAAwA"
    "BgADgAHAAGAAMAAYAAwABgADgAHAAGAAMAAYAAwABgADgAHAAGAAMAAYAAwABgADgAHAAGAAMAAYAAwABgADgAHAAGAAMAAG"
    "UAEDgAHAAGAAMAAYAAwABgADgAHAAGAAMAAYAAwABgADgAHAAGAAMAAYAAwABgADgAHAAGAAMAAYAAwABgADgAHAAGAAMAAY"
    "AAwABgADgAHAAGAAMAAYAAwABgADgAHAABgADAAGAAOAAcAAYAAwABgADAAGAAOAAcAAYAAwABgADAAGAAOAAcAAYAAwABgA"
    "DAAGAAOAAcAAYAAwABgADAAGAAOAAcAAYAAwABgADAAGAAOAAcAAYAAwAFwL/kMSwY/0go4AAAAASUVORK5CYII=";

auto image_transform_report(const venice::Client& client, std::string_view model)
    -> int {
  ModelPick pick;
  if (model.empty()) {
    auto selected = pick_for_image_edit(client);
    if (!selected) return EXIT_FAILURE;
    pick = std::move(*selected);
  } else {
    pick.chosen = std::string{model};
  }
  report_pick(pick, "type=inpaint and combineImages=true",
              "`venice-cpp --image-transform <id>`");

  bool agrees = true;
  const auto report = [&](std::string_view label,
                          const std::expected<venice::GeneratedImageMedia,
                                              venice::Error>& result,
                          std::span<const std::string_view> allowed) {
    if (!result) {
      std::cerr << label << " failed ["
                << venice::to_string(result.error().kind) << "] "
                << result.error().message << '\n';
      if (!result.error().body.empty()) std::cerr << result.error().body << '\n';
      agrees = false;
      return;
    }
    std::cerr << label << ": " << result->media_type << ", "
              << result->bytes.size() << " bytes";
    if (result->metadata.x_balance_remaining)
      std::cerr << ", balance=" << *result->metadata.x_balance_remaining;
    std::cerr << '\n';
    if (result->bytes.empty() ||
        std::find(allowed.begin(), allowed.end(), result->media_type) == allowed.end()) {
      std::cerr << "  EMPTY OR UNEXPECTED IMAGE MEDIA\n";
      agrees = false;
    }
  };

  venice::ImageUpscaleRequest upscale;
  upscale.image = venice::image_input::base64(std::string{kTransformSourceBase64});
  upscale.creativity = 0.01;
  upscale.scale = 2;
  static constexpr std::array<std::string_view, 1> kPng{"image/png"};
  report("upscale", client.upscale_image(upscale), kPng);

  venice::ImageEditRequest edit;
  edit.image = venice::image_input::base64(std::string{kTransformSourceBase64});
  edit.prompt = "Make the image a warm orange colour";
  edit.model = pick.chosen;
  edit.output_format = "png";
  edit.safe_mode = true;
  static constexpr std::array<std::string_view, 3> kEdited{
      "image/jpeg", "image/png", "image/webp"};
  report("edit", client.edit_image(edit), kEdited);

  venice::MultiImageEditRequest multi;
  multi.images = {
      venice::image_input::base64(std::string{kTransformSourceBase64}),
      venice::image_input::base64(std::string{kTransformSourceBase64}),
  };
  multi.prompt = "Combine the two images into one blue square";
  multi.model = pick.chosen;
  multi.output_format = "png";
  multi.safe_mode = true;
  report("multi-edit", client.multi_edit_image(multi), kEdited);

  venice::ImageBackgroundRemovalRequest background;
  background.image =
      venice::image_input::base64(std::string{kTransformSourceBase64});
  report("background removal", client.remove_image_background(background), kPng);

  std::cerr << "No source or result image was decoded or written to disk.\n";
  return agrees ? EXIT_SUCCESS : EXIT_FAILURE;
}


// ── `--billing [lookback]` (VC-42, #68) ───────────────────────────────────
//
constexpr std::array<std::string_view, 4> kModeledBillingBalanceKeys{
    "canConsume", "consumptionCurrency", "balances", "diemEpochAllocation"};
constexpr std::array<std::string_view, 2> kModeledBillingBucketKeys{"diem", "usd"};
constexpr std::array<std::string_view, 10> kModeledBillingAnalyticsKeys{
    "lookback",       "byDate",       "byModel",    "byModelDaily",
    "byModelDailyUsd", "topModels",    "byKey",      "byKeyDaily",
    "byKeyDailyUsd",   "topKeyNames"};
constexpr std::array<std::string_view, 3> kModeledBillingDateKeys{"date", "USD", "DIEM"};
constexpr std::array<std::string_view, 7> kModeledBillingModelKeys{
    "modelName", "unitType", "modelType", "totalUsd", "totalDiem", "totalUnits",
    "breakdown"};
constexpr std::array<std::string_view, 4> kModeledBillingBreakdownKeys{
    "type", "usd", "diem", "units"};
constexpr std::array<std::string_view, 5> kModeledBillingKeyKeys{
    "apiKeyId", "description", "totalUsd", "totalDiem", "totalUnits"};
constexpr std::array<std::string_view, 2> kModeledBillingHistoryKeys{"data", "nextCursor"};
constexpr std::array<std::string_view, 8> kModeledBillingHistoryEntryKeys{
    "amount", "currency", "inferenceDetails", "notes", "pricePerUnitUsd", "sku",
    "timestamp", "units"};
constexpr std::array<std::string_view, 4> kModeledBillingInferenceKeys{
    "completionTokens", "inferenceExecutionTime", "promptTokens", "requestId"};

auto billing_report(const venice::Client& client, std::string_view lookback) -> int {
  bool agrees = true;

  const auto balance = client.billing_balance();
  if (!balance) {
    std::cerr << "billing balance failed [" << venice::to_string(balance.error().kind)
              << "] " << balance.error().message << '\n';
    if (!balance.error().body.empty()) std::cerr << balance.error().body << '\n';
    return EXIT_FAILURE;
  }
  std::cerr << "\n-- billing balance, envelope verbatim --\n"
            << balance->raw.dump(2) << '\n';
  report_unmodeled("unmodeled balance keys: ", balance->raw,
                   kModeledBillingBalanceKeys, "BillingBalance::raw");
  if (const auto* buckets = venice::detail::opt_object(balance->raw, "balances"))
    report_unmodeled("unmodeled balance bucket keys: ", *buckets,
                     kModeledBillingBucketKeys, "BillingBalance::balances->raw");

  venice::BillingUsageAnalyticsQuery analytics_query;
  if (!lookback.empty()) analytics_query.lookback = std::string{lookback};
  const auto analytics = client.billing_usage_analytics(analytics_query);
  if (!analytics) {
    std::cerr << "billing analytics failed ["
              << venice::to_string(analytics.error().kind) << "] "
              << analytics.error().message << '\n';
    if (!analytics.error().body.empty()) std::cerr << analytics.error().body << '\n';
    return EXIT_FAILURE;
  }
  std::cerr << "\n-- billing analytics, envelope verbatim --\n"
            << analytics->raw.dump(2) << '\n';
  report_unmodeled("unmodeled analytics keys: ", analytics->raw,
                   kModeledBillingAnalyticsKeys, "BillingUsageAnalytics::raw");

  const auto check_count = [&](const char* key, std::optional<std::size_t> typed) {
    const auto raw = analytics->raw.find(key);
    const bool raw_array = raw != analytics->raw.end() && raw->is_array();
    const auto raw_size = raw_array ? std::optional<std::size_t>{raw->size()} : std::nullopt;
    if (raw_size != typed) {
      std::cerr << key << " RAW/TYPED COUNT MISMATCH: raw="
                << (raw_size ? std::to_string(*raw_size) : "absent/malformed")
                << ", typed=" << (typed ? std::to_string(*typed) : "absent") << '\n';
      agrees = false;
    }
  };
  check_count("byDate", analytics->by_date
                            ? std::optional<std::size_t>{analytics->by_date->size()}
                            : std::nullopt);
  check_count("byModel", analytics->by_model
                             ? std::optional<std::size_t>{analytics->by_model->size()}
                             : std::nullopt);
  check_count("byModelDaily", analytics->by_model_daily
                                  ? std::optional<std::size_t>{analytics->by_model_daily->size()}
                                  : std::nullopt);
  check_count("byModelDailyUsd",
              analytics->by_model_daily_usd
                  ? std::optional<std::size_t>{analytics->by_model_daily_usd->size()}
                  : std::nullopt);
  check_count("topModels", analytics->top_models
                              ? std::optional<std::size_t>{analytics->top_models->size()}
                              : std::nullopt);
  check_count("byKey", analytics->by_key
                           ? std::optional<std::size_t>{analytics->by_key->size()}
                           : std::nullopt);
  check_count("byKeyDaily", analytics->by_key_daily
                                ? std::optional<std::size_t>{analytics->by_key_daily->size()}
                                : std::nullopt);
  check_count("byKeyDailyUsd",
              analytics->by_key_daily_usd
                  ? std::optional<std::size_t>{analytics->by_key_daily_usd->size()}
                  : std::nullopt);
  check_count("topKeyNames", analytics->top_key_names
                                ? std::optional<std::size_t>{analytics->top_key_names->size()}
                                : std::nullopt);

  if (analytics->by_date)
    for (const auto& item : *analytics->by_date)
      report_unmodeled("unmodeled byDate keys: ", item.raw, kModeledBillingDateKeys,
                       "BillingUsageByDate::raw");
  if (analytics->by_model)
    for (const auto& item : *analytics->by_model) {
      report_unmodeled("unmodeled byModel keys: ", item.raw, kModeledBillingModelKeys,
                       "BillingUsageByModel::raw");
      if (item.breakdown)
        for (const auto& part : *item.breakdown)
          report_unmodeled("unmodeled breakdown keys: ", part.raw,
                           kModeledBillingBreakdownKeys,
                           "BillingUsageBreakdown::raw");
    }
  if (analytics->by_key)
    for (const auto& item : *analytics->by_key)
      report_unmodeled("unmodeled byKey keys: ", item.raw, kModeledBillingKeyKeys,
                       "BillingUsageByKey::raw");
  // The four daily-map families' keys are response-generated display names.
  // Their whole objects are the modeled value, so a per-key set difference
  // there would print the payload rather than detect a parser gap.

  venice::BillingUsageHistoryRequest json_request;
  json_request.query.page_size = 10;
  const auto json_history = client.billing_usage_history(json_request);
  if (!json_history) {
    std::cerr << "billing JSON history failed ["
              << venice::to_string(json_history.error().kind) << "] "
              << json_history.error().message << '\n';
    if (!json_history.error().body.empty()) std::cerr << json_history.error().body << '\n';
    return EXIT_FAILURE;
  }
  const auto* page = std::get_if<venice::BillingUsageHistoryPage>(&*json_history);
  if (page == nullptr) {
    std::cerr << "billing JSON request returned CSV; actual media routing worked, but the live"
                 " contract disagrees with Accept\n";
    agrees = false;
  } else {
    std::cerr << "\n-- billing history, envelope verbatim --\n"
              << page->raw.dump(2) << '\n';
    report_unmodeled("unmodeled history envelope keys: ", page->raw,
                     kModeledBillingHistoryKeys, "BillingUsageHistoryPage::raw");
    if (page->returned != page->entries.size()) {
      std::cerr << "history RAW/TYPED COUNT MISMATCH\n";
      agrees = false;
    }
    for (const auto& item : page->entries) {
      report_unmodeled("unmodeled history entry keys: ", item.raw,
                       kModeledBillingHistoryEntryKeys,
                       "BillingUsageHistoryEntry::raw");
      if (item.inference_details)
        report_unmodeled("unmodeled inference detail keys: ",
                         item.inference_details->raw, kModeledBillingInferenceKeys,
                         "BillingInferenceDetails::raw");
    }
  }

  venice::BillingUsageHistoryRequest csv_request = json_request;
  csv_request.format = venice::BillingUsageHistoryFormat::Csv;
  const auto csv_history = client.billing_usage_history(csv_request);
  if (!csv_history) {
    std::cerr << "billing CSV history failed ["
              << venice::to_string(csv_history.error().kind) << "] "
              << csv_history.error().message << '\n';
    if (!csv_history.error().body.empty()) std::cerr << csv_history.error().body << '\n';
    return EXIT_FAILURE;
  }
  const auto* csv = std::get_if<venice::BillingUsageHistoryCsv>(&*csv_history);
  if (csv == nullptr || csv->text.empty()) {
    std::cerr << "billing CSV request returned no CSV bytes\n";
    agrees = false;
  } else {
    std::cerr << "CSV history: " << csv->text.size() << " bytes, " << csv->media_type
              << ", next cursor " << (csv->next_cursor ? "present" : "absent")
              << ", disposition " << (csv->content_disposition ? "present" : "absent")
              << '\n';
  }
  std::cerr << "CSV contents were retained in memory and not written to disk.\n";
  return agrees ? EXIT_SUCCESS : EXIT_FAILURE;
}

// ── `--api-keys` (VC-43, #70) ───────────────────────────────────────────
//
// Read-only by construction. Create/update/delete are loopback-tested and stay
// out of this long-lived binary so a smoke command cannot accidentally mint or
// revoke a credential. None of the four calls below can return complete key
// material; list/detail expose only the server's last-six display suffix.
constexpr std::array<std::string_view, 2> kModeledApiKeyListKeys{"data", "object"};
constexpr std::array<std::string_view, 12> kModeledApiKeyKeys{
    "apiKeyType", "consumptionLimits",  "limitPeriod",
    "createdAt",  "description",        "expiresAt",
    "id",         "last6Chars",         "lastUsedAt",
    "usage",      "currentPeriodUsage", "modelPrivacy"};
constexpr std::array<std::string_view, 3> kModeledApiKeyLimitKeys{"usd", "diem", "vcu"};
constexpr std::array<std::string_view, 1> kModeledApiKeyUsageKeys{"trailingSevenDays"};
constexpr std::array<std::string_view, 1> kModeledApiKeyRateEnvelopeKeys{"data"};
constexpr std::array<std::string_view, 6> kModeledApiKeyRateDataKeys{
    "accessPermitted", "apiTier", "balances", "keyExpiration", "nextEpochBegins",
    "rateLimits"};
constexpr std::array<std::string_view, 2> kModeledApiKeyTierKeys{"id", "isCharged"};
constexpr std::array<std::string_view, 2> kModeledApiKeyBalanceKeys{"USD", "DIEM"};
constexpr std::array<std::string_view, 2> kModeledApiKeyModelRateKeys{"apiModelId",
                                                                     "rateLimits"};
constexpr std::array<std::string_view, 2> kModeledApiKeyRateKeys{"amount", "type"};
constexpr std::array<std::string_view, 5> kModeledApiKeyLogKeys{
    "apiKeyId", "modelId", "rateLimitTier", "rateLimitType", "timestamp"};

auto api_keys_report(const venice::Client& client) -> int {
  bool agrees = true;
  const auto list = client.api_keys();
  if (!list) {
    std::cerr << "API-key list failed [" << venice::to_string(list.error().kind) << "] "
              << list.error().message << '\n';
    if (!list.error().body.empty()) std::cerr << list.error().body << '\n';
    return EXIT_FAILURE;
  }

  std::cerr << "\n-- API-key list, envelope verbatim (no complete keys are returned) --\n"
            << list->raw.dump(2) << '\n';
  report_unmodeled("unmodeled API-key list keys: ", list->raw,
                   kModeledApiKeyListKeys, "ApiKeyList::raw");
  const auto* raw_keys = venice::detail::opt_array(list->raw, "data");
  if (raw_keys == nullptr || raw_keys->size() != list->returned ||
      list->returned != list->entries.size()) {
    std::cerr << "API-key RAW/TYPED COUNT MISMATCH\n";
    agrees = false;
  }

  std::optional<std::string> detail_id;
  const auto reconcile_model_privacy = [&agrees](const venice::ApiKey& key) {
    if (venice::detail::opt_string(key.raw, "modelPrivacy") ==
        key.model_privacy)
      return;
    std::cerr << "API-key modelPrivacy RAW/TYPED MISMATCH\n";
    agrees = false;
  };
  for (const auto& key : list->entries) {
    report_unmodeled("unmodeled API-key fields: ", key.raw, kModeledApiKeyKeys,
                     "ApiKey::raw");
    if (key.consumption_limits)
      report_unmodeled("unmodeled consumption-limit fields: ",
                       key.consumption_limits->raw, kModeledApiKeyLimitKeys,
                       "ApiKeyConsumptionLimits::raw");
    if (key.usage)
      report_unmodeled("unmodeled usage fields: ", key.usage->raw,
                       kModeledApiKeyUsageKeys, "ApiKeyUsage::raw");
    reconcile_model_privacy(key);
    if (!detail_id && key.id) detail_id = key.id;
  }

  if (detail_id) {
    const auto detail = client.api_key(*detail_id);
    if (!detail) {
      std::cerr << "API-key detail failed [" << venice::to_string(detail.error().kind)
                << "] " << detail.error().message << '\n';
      return EXIT_FAILURE;
    }
    std::cerr << "\n-- API-key detail, envelope verbatim --\n"
              << detail->envelope_raw.dump(2) << '\n';
    report_unmodeled("unmodeled API-key detail fields: ", detail->raw,
                     kModeledApiKeyKeys, "ApiKey::raw");
    reconcile_model_privacy(*detail);
  } else {
    std::cerr << "No API-key id was available for the read-only detail check.\n";
  }

  const auto limits = client.api_key_rate_limits();
  if (!limits) {
    std::cerr << "API-key rate limits failed ["
              << venice::to_string(limits.error().kind) << "] "
              << limits.error().message << '\n';
    return EXIT_FAILURE;
  }
  std::cerr << "\n-- API-key rate limits, envelope verbatim --\n"
            << limits->raw.dump(2) << '\n';
  report_unmodeled("unmodeled rate-limit envelope keys: ", limits->raw,
                   kModeledApiKeyRateEnvelopeKeys, "ApiKeyRateLimits::raw");
  if (const auto* data = venice::detail::opt_object(limits->raw, "data")) {
    report_unmodeled("unmodeled rate-limit data keys: ", *data,
                     kModeledApiKeyRateDataKeys, "ApiKeyRateLimits::raw[data]");
    if (const auto* tier = venice::detail::opt_object(*data, "apiTier"))
      report_unmodeled("unmodeled tier keys: ", *tier, kModeledApiKeyTierKeys,
                       "ApiKeyTier::raw");
    if (const auto* balances = venice::detail::opt_object(*data, "balances"))
      report_unmodeled("unmodeled rate-limit balance keys: ", *balances,
                       kModeledApiKeyBalanceKeys, "ApiKeyBalances::raw");
  }
  if (limits->rate_limits)
    for (const auto& group : *limits->rate_limits) {
      report_unmodeled("unmodeled model rate-limit keys: ", group.raw,
                       kModeledApiKeyModelRateKeys, "ApiKeyModelRateLimits::raw");
      if (group.rate_limits)
        for (const auto& limit : *group.rate_limits)
          report_unmodeled("unmodeled rate-limit keys: ", limit.raw,
                           kModeledApiKeyRateKeys, "ApiKeyRateLimit::raw");
    }

  const auto logs = client.api_key_rate_limit_logs();
  if (!logs) {
    std::cerr << "API-key rate-limit logs failed ["
              << venice::to_string(logs.error().kind) << "] "
              << logs.error().message << '\n';
    return EXIT_FAILURE;
  }
  std::cerr << "\n-- API-key rate-limit logs, envelope verbatim --\n"
            << logs->raw.dump(2) << '\n';
  report_unmodeled("unmodeled rate-limit log envelope keys: ", logs->raw,
                   kModeledApiKeyListKeys, "ApiKeyRateLimitLogPage::raw");
  const auto* raw_logs = venice::detail::opt_array(logs->raw, "data");
  if (raw_logs == nullptr || raw_logs->size() != logs->returned ||
      logs->returned != logs->entries.size()) {
    std::cerr << "rate-limit-log RAW/TYPED COUNT MISMATCH\n";
    agrees = false;
  }
  for (const auto& entry : logs->entries)
    report_unmodeled("unmodeled rate-limit log fields: ", entry.raw,
                     kModeledApiKeyLogKeys, "ApiKeyRateLimitLogEntry::raw");

  std::cerr << "Read-only API-key checks completed; no key was created, updated, or deleted.\n";
  return agrees ? EXIT_SUCCESS : EXIT_FAILURE;
}

// ── `--traits` and `--compat` (VC-38, #59) ────────────────────────────────
//
// The two legs that run with no key at all. Everything above this point needs a
// credential; these two exist partly to prove that the library does not, and
// they are dispatched in main() above the VENICE_API_KEY guard for that reason.
//
// Both operations answer the same three-key envelope, so both legs share one
// reporter. What each of them checks:
//
//   * the verbatim envelope, printed before anything is said about the parse;
//   * the unmodeled-key difference at the envelope level;
//   * a COUNT RECONCILIATION in place of that difference one level down;
//   * the `type` echo against the filter the leg asked for;
//   * every value resolved against the live catalogue.
//
// The second of those needs explaining, because its absence would otherwise read
// as an oversight. AGENTS.md requires a leg to difference the modeled keys
// against the actual ones *per nesting level*, and this is the one place where
// that rule stops a level short. Every key inside `data` is caller-unknown by
// construction — they are trait names and foreign vendor model ids, the server's
// data rather than this client's schema — so a set-difference there would print
// the entire payload on every run and mean nothing. What replaces it detects the
// same class of defect: raw["data"]'s member count against `returned` against
// the parsed entry count. A key this parser could not use shows up as a gap
// between the second and third, and the leg then names it.
constexpr std::array<std::string_view, 3> kModeledCatalogueKeys{"data", "object", "type"};

// The picker convention (pick_by_capability / report_pick) does not bind here,
// and AGENTS.md asks a leg that skips it to say why rather than stay silent:
// there is no per-model pick to make. These legs call one endpoint that returns
// every entry it has. If anything, the relationship runs the other way — the
// traits map is where a "default" or "fastest" for a modality comes from, so
// this is the endpoint a future picker would consult instead of scanning.
template <typename Result>
auto report_catalogue(const Result& res, const venice::Client& client, std::string_view what,
                      std::string_view requested) -> int {
  std::cout << what << ", verbatim: " << res.raw.dump() << '\n';

  bool broken = false;

  report_unmodeled("unmodeled envelope keys: ", res.raw, kModeledCatalogueKeys,
                   "the result's raw");

  std::cout << "typed object       : " << (res.object ? *res.object : "(absent -- not a string)")
            << '\n';
  std::cout << "typed type         : " << (res.type ? *res.type : "(absent -- not a string)")
            << '\n';

  // The echo check, written absolutely rather than relative to what was asked
  // for. AGENTS.md's VC-37 rule is the reason: a check that only runs when the
  // caller passed a filter cannot fire on `--traits` with no argument, which is
  // the invocation README documents and the one most likely to be run. A 200
  // that echoed no `type` at all has failed whatever the arguments were —
  // measured 2026-08-11, an unfiltered call answers "text", so the field is
  // always there to be missing.
  if (!res.type) {
    std::cerr << "  NO `type` ECHO -- the server did not say which catalogue this is\n";
    broken = true;
  } else if (!requested.empty() && *res.type != requested) {
    std::cerr << "  TYPE ECHO MISMATCH -- asked for '" << requested << "', server applied '"
              << *res.type << "'\n";
    broken = true;
  }

  // The count reconciliation. Two numbers, not three: `raw["data"]` is where
  // `returned` came from, so re-deriving it here and comparing would be the
  // parser checked against itself, and the case where `data` is absent entirely
  // cannot reach this function at all — the parse throws on it and the leg has
  // already returned EXIT_FAILURE above. What is left is the one comparison that
  // carries live information, between what the server sent and what parsed.
  const auto* raw_data = venice::detail::opt_object(res.raw, "data");
  std::cout << "typed returned     : " << res.returned
            << " (entries parsed: " << res.entries.size() << ")\n";

  // A gap means an entry arrived with a value this parser could not use. The
  // numbers alone do not say which, so name them.
  //
  // This one flips `broken` where the catalogue cross-check below deliberately
  // does not, and the difference is whether a change here could fix it. A
  // retired compat target is Venice's data moving and no edit to this repo makes
  // it green; a value that is not a string is the wire carrying a shape these
  // types do not model, which is actionable — VC-39 would be where it is
  // actioned.
  if (res.returned != res.entries.size() && raw_data != nullptr) {
    std::cerr << "  SKIPPED (value was not a string): ";
    bool first = true;
    for (const auto& [k, v] : raw_data->items())
      if (!v.is_string()) {
        std::cerr << (first ? "" : ", ") << k << '=' << v.dump();
        first = false;
      }
    std::cerr << '\n';
    broken = true;
  }

  // `returned == 0` and not merely `entries.empty()`: a page whose every value
  // was skipped is also empty, and calling that "a normal answer" on stdout
  // would contradict the SKIPPED line just written to stderr.
  if (res.entries.empty() && res.returned == 0)
    std::cout << "  (empty -- measured 2026-08-11, this is a 200 and a normal answer)\n";
  for (const auto& [key, value] : res.entries) std::cout << "  " << key << " -> " << value << '\n';

  // Cross-check every target against the catalogue itself. Two endpoints make a
  // claim about the same model ids and nothing had ever compared them; on
  // 2026-08-11 every one of the 30 targets across both operations resolved.
  //
  // This is reported but does NOT fail the leg, and the line matters: a retired
  // target is Venice's data drifting, not a defect in this library, and no code
  // change here could turn it green. The checks above are the ones a library bug
  // can cause, and those do fail.
  if (!res.entries.empty()) {
    const auto catalogue = client.models(res.type ? *res.type : std::string_view{});
    if (!catalogue) {
      std::cerr << "  (catalogue cross-check skipped: models() failed -- "
                << catalogue.error().message << ")\n";
    } else {
      std::vector<std::string> unknown;
      for (const auto& [key, value] : res.entries) {
        const auto hit = std::find_if(catalogue->begin(), catalogue->end(),
                                      [&value](const venice::Model& m) { return m.id == value; });
        if (hit == catalogue->end()) unknown.push_back(key + " -> " + value);
      }
      if (unknown.empty()) {
        std::cout << "cross-check        : all " << res.entries.size() << " targets are in the "
                  << catalogue->size() << "-model catalogue\n";
      } else {
        std::cerr << "  TARGETS NOT IN THE CATALOGUE (server-side drift, not a library bug): ";
        for (std::size_t i = 0; i < unknown.size(); ++i)
          std::cerr << (i != 0U ? ", " : "") << unknown[i];
        std::cerr << '\n';
      }
    }
  }

  return broken ? EXIT_FAILURE : EXIT_SUCCESS;
}

// `--traits [type]`: which model currently holds which capability.
auto show_model_traits(const venice::Client& client, std::string_view type) -> int {
  const auto res = client.model_traits(type);
  if (!res) {
    std::cerr << "model traits failed [" << venice::to_string(res.error().kind) << "] "
              << res.error().message << '\n';
    // "HTTP 400" alone names nothing; the body is where the server says which
    // values it would have accepted.
    if (!res.error().body.empty()) std::cerr << res.error().body << '\n';
    return EXIT_FAILURE;
  }
  return report_catalogue(*res, client, "traits", type);
}

// `--compat [type]`: what a foreign vendor's model id resolves to here.
//
// Worth running with no argument and then with `all` to see the divergence this
// ticket documents: traits accepts `all`, this does not, and the 400 it answers
// with is the evidence for passing `type` through untouched.
auto show_compatibility_mapping(const venice::Client& client, std::string_view type) -> int {
  const auto res = client.model_compatibility_mapping(type);
  if (!res) {
    std::cerr << "compatibility mapping failed [" << venice::to_string(res.error().kind) << "] "
              << res.error().message << '\n';
    if (!res.error().body.empty()) std::cerr << res.error().body << '\n';
    return EXIT_FAILURE;
  }
  return report_catalogue(*res, client, "compatibility", type);
}

// ── `--modality [type]` (VC-39, #60) ──────────────────────────────────────
//
// The third leg that needs no key: /models answers 200 with no Authorization
// header for every modality, measured 2026-08-11. It is dispatched above the
// guard for the same reason --traits and --compat are.
//
// What it checks, and why it does not pick a model:
//
// AGENTS.md makes naming the runners-up a rule after VC-18, and asks a leg
// that skips pick_by_capability to say why rather than stay silent. There is
// no per-model pick to make here — this leg walks EVERY entry rather than
// choosing one. What replaces the runners-up list is the coverage column: for
// each modeled key, how many of the modality's entries carried it. That is a
// strictly stronger answer to the question the rule exists to force. A single
// chosen model could never show that `maxStyleReferences` is on 4 of 37 image
// models while `promptCharacterLimit` is on all 37, and that difference is
// precisely the per-family variation VC-18 was filed for missing.
//
// Three kinds of finding, deliberately not treated alike:
//
//   * A RECONCILIATION MISMATCH fails the leg. Either a typed field engaged
//     while its key is absent from raw at that level — VC-37's bug, which was
//     a read one level too high — or raw carried a usable value the typed
//     field did not take. Both are library defects an edit here can fix.
//   * An UNMODELED KEY fails nothing and is printed per nesting level. It is
//     how a wire key nothing here names becomes visible, which is the only way
//     the seven undocumented video keys were found in the first place, and it
//     is what a renamed table row would surface as.
//   * A NEVER-OBSERVED modeled key is reported and does not fail. `startsAt`
//     and `replacementModelId` are documented, modeled and have never been on
//     a wire; that is Venice's data, and no change here makes it green.

struct Mismatch {
  std::string model;
  std::string level;
  std::string key;
  std::string why;
};

// Every key a set of tables names. The keys are string literals with static
// storage, so views into them outlive any call.
template <typename... Tables>
auto table_keys(const Tables&... tables) -> std::vector<std::string_view> {
  std::vector<std::string_view> out;
  (
      [&] {
        for (const auto& row : tables) out.emplace_back(row.key);
      }(),
      ...);
  return out;
}

// One row's worth of raw-vs-typed. `usable` is the JSON predicate the parse
// itself applies, so "raw has it and the type is right" means the same thing
// in both places; what the comparison adds is that the key is looked for at
// THIS level, which is the half the parse cannot check about itself.
template <typename Engaged>
void reconcile_row(const nlohmann::json& obj, bool typed, const char* key, Engaged usable,
                   std::string_view model, std::string_view level,
                   std::vector<Mismatch>& out) {
  const auto it = obj.find(key);
  const bool present = it != obj.end();
  if (typed && !present)
    out.push_back({std::string{model}, std::string{level}, key,
                   "typed field engaged but raw carries no such key at this level"});
  if (typed && present && !usable(*it))
    out.push_back({std::string{model}, std::string{level}, key,
                   "typed field engaged but the raw value has the wrong type"});
  if (present && usable(*it) && !typed)
    out.push_back({std::string{model}, std::string{level}, key,
                   "raw carries a usable value but the typed field is absent"});
}

// Walk one table: reconcile every row and count the engaged ones.
template <typename Struct, typename Row, std::size_t N, typename Engaged>
void walk(const nlohmann::json& obj, const Struct& s, const std::array<Row, N>& table,
          Engaged usable, std::string_view model, std::string_view level,
          std::map<std::string, int>& coverage, std::vector<Mismatch>& out) {
  for (const auto& [field, key] : table) {
    const bool typed = (s.*field).has_value();
    if (typed) ++coverage[std::string{level} + '.' + key];
    else coverage.try_emplace(std::string{level} + '.' + key, 0);
    reconcile_row(obj, typed, key, usable, model, level, out);
  }
}

// Presence in both directions catches wrong nesting; equality additionally
// catches two same-typed rows wired to each other's keys. VC-56 uses this for
// the capability block and the cross-modality policy field whose live values
// are scalar and therefore have an unambiguous comparison.
template <typename Struct, typename T, std::size_t N, typename Engaged,
          typename Reader>
void walk_values(const nlohmann::json& obj, const Struct& s,
                 const std::array<venice::detail::Field<T, Struct>, N>& table,
                 Engaged usable, Reader read, std::string_view model,
                 std::string_view level, std::map<std::string, int>& coverage,
                 std::vector<Mismatch>& out) {
  walk(obj, s, table, usable, model, level, coverage, out);
  for (const auto& [field, key] : table) {
    const auto expected = read(obj, key);
    const auto& typed = s.*field;
    if (expected && typed && *expected != *typed)
      out.push_back({std::string{model}, std::string{level}, key,
                     "typed value disagrees with the usable raw value"});
  }
}

// The one legacy plain-vector table. Its public empty value collapses absent,
// malformed and an explicitly empty array, so reconciliation compares the
// parser's exact filtered result when the raw array is present rather than
// pretending the type has an engaged state it cannot represent.
template <typename Struct, std::size_t N>
void walk_flat_strings(
    const nlohmann::json& obj, const Struct& s,
    const std::array<venice::detail::FlatVectorField<std::string, Struct>, N>&
        table,
    std::string_view model, std::string_view level,
    std::map<std::string, int>& coverage, std::vector<Mismatch>& out) {
  for (const auto& [field, key] : table) {
    const auto it = obj.find(key);
    const bool usable = it != obj.end() && it->is_array();
    if (usable)
      ++coverage[std::string{level} + '.' + key];
    else
      coverage.try_emplace(std::string{level} + '.' + key, 0);

    if (it == obj.end() && !(s.*field).empty())
      out.push_back({std::string{model}, std::string{level}, key,
                     "typed list is non-empty but raw carries no such key at "
                     "this level"});
    if (usable && s.*field != venice::detail::string_array(obj, key))
      out.push_back({std::string{model}, std::string{level}, key,
                     "typed list disagrees with the usable raw array"});
  }
}

const auto kIsBool = [](const nlohmann::json& v) { return v.is_boolean(); };
const auto kIsString = [](const nlohmann::json& v) { return v.is_string(); };
const auto kIsArray = [](const nlohmann::json& v) { return v.is_array(); };
const auto kIsObject = [](const nlohmann::json& v) { return v.is_object(); };
// Mirrors opt_int: an integer this field can represent. A past-INT_MAX value
// legitimately reads absent, so calling it usable here would report a
// reconciliation failure for correct behaviour.
const auto kIsInt = [](const nlohmann::json& v) {
  return venice::detail::integer_if_representable<int>(v).has_value();
};
// Mirrors opt_double: is_number(), not is_number_float().
const auto kIsNumber = [](const nlohmann::json& v) { return v.is_number(); };

constexpr std::array<std::string_view, 2> kPriceKeys{"usd", "diem"};
constexpr std::array<std::string_view, 5> kTokenPricingKeys{
    "input", "output", "cache_input", "cache_write", "extended"};
constexpr std::array<std::string_view, 5> kExtendedPricingKeys{
    "context_token_threshold", "input", "output", "cache_input", "cache_write"};
constexpr std::array<std::string_view, 4> kImagePricingKeys{
    "generation", "resolutions", "quality", "upscale"};
constexpr std::array<std::string_view, 2> kUpscalePricingKeys{"2x", "4x"};
constexpr std::array<std::string_view, 4> kInpaintPricingKeys{
    "inpaint", "resolutions", "quality", "inputImages"};
constexpr std::array<std::string_view, 2> kInputImagePricingKeys{"included",
                                                                 "additional"};
constexpr std::array<std::string_view, 1> kTtsPricingKeys{"input"};
constexpr std::array<std::string_view, 1> kAsrPricingKeys{"per_audio_second"};
constexpr std::array<std::string_view, 4> kMusicPricingKeys{
    "generation", "durations", "per_second", "per_thousand_characters"};
constexpr std::array<std::string_view, 4> kMusicDurationPriceKeys{
    "usd", "diem", "min_seconds", "max_seconds"};

template <typename T, typename Reader, typename Usable>
void audit_scalar(const nlohmann::json &raw, const char *key,
                  const std::optional<T> &typed, Reader read, Usable usable,
                  std::string_view model, std::string_view level,
                  std::map<std::string, int> &coverage,
                  std::vector<Mismatch> &mismatches) {
  if (typed)
    ++coverage[std::string{level} + '.' + key];
  else
    coverage.try_emplace(std::string{level} + '.' + key, 0);
  reconcile_row(raw, typed.has_value(), key, usable, model, level, mismatches);
  const auto expected = read(raw, key);
  if (expected && typed && *expected != *typed)
    mismatches.push_back({std::string{model}, std::string{level}, key,
                          "typed value disagrees with the usable raw value"});
}

void audit_price(const nlohmann::json &raw, const venice::Price &typed,
                 std::string_view model, std::string level,
                 std::map<std::string, int> &coverage,
                 std::vector<Mismatch> &mismatches) {
  report_unmodeled("unmodeled price keys [" + std::string{model} + "]: ", raw,
                   kPriceKeys, "Model::raw[...][\"pricing\"]");
  audit_scalar(raw, "usd", typed.usd, venice::detail::opt_double, kIsNumber,
               model, level, coverage, mismatches);
  audit_scalar(raw, "diem", typed.diem, venice::detail::opt_double, kIsNumber,
               model, level, coverage, mismatches);
}

void audit_price_member(const nlohmann::json &raw, const char *key,
                        const std::optional<venice::Price> &typed,
                        std::string_view model, std::string_view level,
                        std::map<std::string, int> &coverage,
                        std::vector<Mismatch> &mismatches) {
  if (typed)
    ++coverage[std::string{level} + '.' + key];
  else
    coverage.try_emplace(std::string{level} + '.' + key, 0);
  reconcile_row(raw, typed.has_value(), key, kIsObject, model, level,
                mismatches);
  if (const auto *value = venice::detail::opt_object(raw, key); value && typed)
    audit_price(*value, *typed, model, std::string{level} + '.' + key, coverage,
                mismatches);
}

void audit_price_map(const nlohmann::json &raw,
                     const venice::OptionPriceMap &typed,
                     std::string_view model, std::string level,
                     std::map<std::string, int> &coverage,
                     std::vector<Mismatch> &mismatches) {
  for (const auto &[key, value] : raw.items()) {
    if (!value.is_object())
      continue;
    const auto found = typed.find(key);
    if (found == typed.end()) {
      mismatches.push_back(
          {std::string{model}, level, key,
           "usable raw map entry is absent from the typed map"});
      continue;
    }
    audit_price(value, found->second, model, level + '.' + key, coverage,
                mismatches);
  }
  for (const auto &[key, value] : typed) {
    const auto found = raw.find(key);
    if (found == raw.end() || !found->is_object())
      mismatches.push_back({std::string{model}, level, key,
                            "typed map entry has no usable raw object"});
  }
}

template <typename Map, typename AuditEntry>
void audit_open_map_member(const nlohmann::json &raw, const char *key,
                           const std::optional<Map> &typed, AuditEntry audit,
                           std::string_view model, std::string_view level,
                           std::map<std::string, int> &coverage,
                           std::vector<Mismatch> &mismatches) {
  if (typed)
    ++coverage[std::string{level} + '.' + key];
  else
    coverage.try_emplace(std::string{level} + '.' + key, 0);
  reconcile_row(raw, typed.has_value(), key, kIsObject, model, level,
                mismatches);
  if (const auto *value = venice::detail::opt_object(raw, key); value && typed)
    audit(*value, *typed, model, std::string{level} + '.' + key, coverage,
          mismatches);
}

void audit_quality_map(const nlohmann::json &raw,
                       const venice::ResolutionQualityPriceMap &typed,
                       std::string_view model, std::string level,
                       std::map<std::string, int> &coverage,
                       std::vector<Mismatch> &mismatches) {
  for (const auto &[resolution, value] : raw.items()) {
    if (!value.is_object())
      continue;
    const auto found = typed.find(resolution);
    if (found == typed.end()) {
      mismatches.push_back(
          {std::string{model}, level, resolution,
           "usable raw resolution is absent from the typed map"});
      continue;
    }
    audit_price_map(value, found->second, model, level + '.' + resolution,
                    coverage, mismatches);
  }
  for (const auto &[resolution, value] : typed) {
    const auto found = raw.find(resolution);
    if (found == raw.end() || !found->is_object())
      mismatches.push_back({std::string{model}, level, resolution,
                            "typed resolution has no usable raw object"});
  }
}

void audit_upscale(const nlohmann::json &raw,
                   const venice::ImageUpscalePricing &typed,
                   std::string_view model, std::string level,
                   std::map<std::string, int> &coverage,
                   std::vector<Mismatch> &mismatches) {
  report_unmodeled(
      "unmodeled upscale pricing keys [" + std::string{model} + "]: ", raw,
      kUpscalePricingKeys, "Model::raw[...][\"pricing\"][\"upscale\"]");
  audit_price_member(raw, "2x", typed.x2, model, level, coverage, mismatches);
  audit_price_member(raw, "4x", typed.x4, model, level, coverage, mismatches);
}

void audit_token_pricing(const nlohmann::json &raw,
                         const venice::TokenPricing &typed,
                         std::string_view model,
                         std::map<std::string, int> &coverage,
                         std::vector<Mismatch> &mismatches) {
  audit_price_member(raw, "input", typed.base.input, model, "pricing", coverage,
                     mismatches);
  audit_price_member(raw, "output", typed.base.output, model, "pricing",
                     coverage, mismatches);
  audit_price_member(raw, "cache_input", typed.base.cache_input, model,
                     "pricing", coverage, mismatches);
  audit_price_member(raw, "cache_write", typed.base.cache_write, model,
                     "pricing", coverage, mismatches);

  if (typed.extended)
    ++coverage["pricing.extended"];
  else
    coverage.try_emplace("pricing.extended", 0);
  reconcile_row(raw, typed.extended.has_value(), "extended", kIsObject, model,
                "pricing", mismatches);
  const auto *extended = venice::detail::opt_object(raw, "extended");
  if (extended == nullptr || !typed.extended)
    return;
  report_unmodeled("unmodeled extended pricing keys [" + std::string{model} +
                       "]: ",
                   *extended, kExtendedPricingKeys,
                   "Model::raw[...][\"pricing\"][\"extended\"]");
  audit_scalar(
      *extended, "context_token_threshold", typed.extended_threshold_tokens,
      venice::detail::opt_i64,
      [](const auto &value) {
        return venice::detail::integer_if_representable<std::int64_t>(value)
            .has_value();
      },
      model, "pricing.extended", coverage, mismatches);
  audit_price_member(*extended, "input", typed.extended->input, model,
                     "pricing.extended", coverage, mismatches);
  audit_price_member(*extended, "output", typed.extended->output, model,
                     "pricing.extended", coverage, mismatches);
  audit_price_member(*extended, "cache_input", typed.extended->cache_input,
                     model, "pricing.extended", coverage, mismatches);
  audit_price_member(*extended, "cache_write", typed.extended->cache_write,
                     model, "pricing.extended", coverage, mismatches);
}

void audit_image_pricing(const nlohmann::json &raw,
                         const venice::ImagePricing &typed,
                         std::string_view model,
                         std::map<std::string, int> &coverage,
                         std::vector<Mismatch> &mismatches) {
  audit_price_member(raw, "generation", typed.generation, model, "pricing",
                     coverage, mismatches);
  audit_open_map_member(raw, "resolutions", typed.resolutions, audit_price_map,
                        model, "pricing", coverage, mismatches);
  audit_open_map_member(raw, "quality", typed.quality, audit_quality_map, model,
                        "pricing", coverage, mismatches);

  if (typed.upscale)
    ++coverage["pricing.upscale"];
  else
    coverage.try_emplace("pricing.upscale", 0);
  reconcile_row(raw, typed.upscale.has_value(), "upscale", kIsObject, model,
                "pricing", mismatches);
  if (const auto *value = venice::detail::opt_object(raw, "upscale");
      value && typed.upscale)
    audit_upscale(*value, *typed.upscale, model, "pricing.upscale", coverage,
                  mismatches);
}

void audit_inpaint_pricing(const nlohmann::json &raw,
                           const venice::InpaintPricing &typed,
                           std::string_view model,
                           std::map<std::string, int> &coverage,
                           std::vector<Mismatch> &mismatches) {
  audit_price_member(raw, "inpaint", typed.base, model, "pricing", coverage,
                     mismatches);
  audit_open_map_member(raw, "resolutions", typed.resolutions, audit_price_map,
                        model, "pricing", coverage, mismatches);
  audit_open_map_member(raw, "quality", typed.quality, audit_quality_map, model,
                        "pricing", coverage, mismatches);

  if (typed.input_images)
    ++coverage["pricing.inputImages"];
  else
    coverage.try_emplace("pricing.inputImages", 0);
  reconcile_row(raw, typed.input_images.has_value(), "inputImages", kIsObject,
                model, "pricing", mismatches);
  const auto *input_images = venice::detail::opt_object(raw, "inputImages");
  if (input_images == nullptr || !typed.input_images)
    return;
  report_unmodeled("unmodeled inputImages pricing keys [" + std::string{model} +
                       "]: ",
                   *input_images, kInputImagePricingKeys,
                   "Model::raw[...][\"pricing\"][\"inputImages\"]");
  audit_scalar(*input_images, "included", typed.input_images->included,
               venice::detail::opt_int, kIsInt, model, "pricing.inputImages",
               coverage, mismatches);
  audit_price_member(*input_images, "additional",
                     typed.input_images->additional, model,
                     "pricing.inputImages", coverage, mismatches);
}

void audit_music_duration_map(const nlohmann::json &raw,
                              const venice::MusicDurationPriceMap &typed,
                              std::string_view model, std::string level,
                              std::map<std::string, int> &coverage,
                              std::vector<Mismatch> &mismatches) {
  for (const auto &[key, value] : raw.items()) {
    if (!value.is_object())
      continue;
    const auto found = typed.find(key);
    if (found == typed.end()) {
      mismatches.push_back(
          {std::string{model}, level, key,
           "usable raw duration is absent from the typed map"});
      continue;
    }
    const auto tier_level = level + '.' + key;
    report_unmodeled(
        "unmodeled duration price keys [" + std::string{model} + "]: ", value,
        kMusicDurationPriceKeys, "Model::raw[...][\"pricing\"][\"durations\"]");
    audit_scalar(value, "usd", found->second.price.usd,
                 venice::detail::opt_double, kIsNumber, model, tier_level,
                 coverage, mismatches);
    audit_scalar(value, "diem", found->second.price.diem,
                 venice::detail::opt_double, kIsNumber, model, tier_level,
                 coverage, mismatches);
    audit_scalar(value, "min_seconds", found->second.min_seconds,
                 venice::detail::opt_int, kIsInt, model, tier_level, coverage,
                 mismatches);
    audit_scalar(value, "max_seconds", found->second.max_seconds,
                 venice::detail::opt_int, kIsInt, model, tier_level, coverage,
                 mismatches);
  }
  for (const auto &[key, value] : typed) {
    const auto found = raw.find(key);
    if (found == raw.end() || !found->is_object())
      mismatches.push_back({std::string{model}, level, key,
                            "typed duration has no usable raw object"});
  }
}

void audit_music_pricing(const nlohmann::json &raw,
                         const venice::MusicPricing &typed,
                         std::string_view model,
                         std::map<std::string, int> &coverage,
                         std::vector<Mismatch> &mismatches) {
  audit_price_member(raw, "generation", typed.generation, model, "pricing",
                     coverage, mismatches);
  audit_open_map_member(raw, "durations", typed.durations,
                        audit_music_duration_map, model, "pricing", coverage,
                        mismatches);
  audit_price_member(raw, "per_second", typed.per_second, model, "pricing",
                     coverage, mismatches);
  audit_price_member(raw, "per_thousand_characters",
                     typed.per_thousand_characters, model, "pricing", coverage,
                     mismatches);
}

template <typename View>
void reconcile_pricing_view(bool expected, const std::optional<View> &typed,
                            std::string_view model,
                            std::vector<Mismatch> &mismatches) {
  if (expected && !typed)
    mismatches.push_back({std::string{model}, "pricing", "view",
                          "usable modality pricing has no typed view"});
  if (!expected && typed)
    mismatches.push_back(
        {std::string{model}, "pricing", "view",
         "typed pricing view engaged without a usable bucket"});
}

void audit_model_pricing(const nlohmann::json &raw, const venice::Model &model,
                         std::map<std::string, int> &coverage,
                         std::vector<Mismatch> &mismatches) {
  using venice::detail::has_any_object;
  if (model.type == "text" || model.type == "embedding") {
    report_unmodeled("unmodeled pricing keys [" + model.id + "]: ", raw,
                     kTokenPricingKeys, "Model::raw[...][\"pricing\"]");
    const bool expected = has_any_object(
        raw, {"input", "output", "cache_input", "cache_write", "extended"});
    reconcile_pricing_view(expected, model.token_pricing, model.id, mismatches);
    if (model.token_pricing)
      audit_token_pricing(raw, *model.token_pricing, model.id, coverage,
                          mismatches);
  } else if (model.type == "image" || model.type == "upscale") {
    report_unmodeled("unmodeled pricing keys [" + model.id + "]: ", raw,
                     kImagePricingKeys, "Model::raw[...][\"pricing\"]");
    const bool expected = has_any_object(
        raw, {"generation", "resolutions", "quality", "upscale"});
    reconcile_pricing_view(expected, model.image_pricing, model.id, mismatches);
    if (model.image_pricing)
      audit_image_pricing(raw, *model.image_pricing, model.id, coverage,
                          mismatches);
  } else if (model.type == "inpaint") {
    report_unmodeled("unmodeled pricing keys [" + model.id + "]: ", raw,
                     kInpaintPricingKeys, "Model::raw[...][\"pricing\"]");
    const bool expected = has_any_object(
        raw, {"inpaint", "resolutions", "quality", "inputImages"});
    reconcile_pricing_view(expected, model.inpaint_pricing, model.id,
                           mismatches);
    if (model.inpaint_pricing)
      audit_inpaint_pricing(raw, *model.inpaint_pricing, model.id, coverage,
                            mismatches);
  } else if (model.type == "tts") {
    report_unmodeled("unmodeled pricing keys [" + model.id + "]: ", raw,
                     kTtsPricingKeys, "Model::raw[...][\"pricing\"]");
    const bool expected = has_any_object(raw, {"input"});
    reconcile_pricing_view(expected, model.tts_pricing, model.id, mismatches);
    if (model.tts_pricing)
      audit_price_member(raw, "input",
                         model.tts_pricing->per_million_characters, model.id,
                         "pricing", coverage, mismatches);
  } else if (model.type == "asr") {
    report_unmodeled("unmodeled pricing keys [" + model.id + "]: ", raw,
                     kAsrPricingKeys, "Model::raw[...][\"pricing\"]");
    const bool expected = has_any_object(raw, {"per_audio_second"});
    reconcile_pricing_view(expected, model.asr_pricing, model.id, mismatches);
    if (model.asr_pricing)
      audit_price_member(raw, "per_audio_second",
                         model.asr_pricing->per_audio_second, model.id,
                         "pricing", coverage, mismatches);
  } else if (model.type == "music") {
    report_unmodeled("unmodeled pricing keys [" + model.id + "]: ", raw,
                     kMusicPricingKeys, "Model::raw[...][\"pricing\"]");
    const bool expected =
        has_any_object(raw, {"generation", "durations", "per_second",
                             "per_thousand_characters"});
    reconcile_pricing_view(expected, model.music_pricing, model.id, mismatches);
    if (model.music_pricing)
      audit_music_pricing(raw, *model.music_pricing, model.id, coverage,
                          mismatches);
  } else {
    constexpr std::array<std::string_view, 0> kNoPricingKeys{};
    report_unmodeled("unmodeled pricing keys [" + model.id + "]: ", raw,
                     kNoPricingKeys, "Model::raw[...][\"pricing\"]");
  }
}

// The model_spec keys Model has read since VC-03, plus the VC-39 reads for
// every type. Cross-modality and per-modality table keys are appended below.
constexpr std::array<std::string_view, 14> kSharedSpecKeys{
    "name",        "description",          "privacy",             "modelSource",
    "offline",     "betaModel",            "traits",              "availableContextTokens",
    "maxCompletionTokens", "capabilities", "pricing",             "constraints",
    "model_sets",  "deprecation"};

constexpr std::array<std::string_view, 7> kModeledEntryKeys{
    "id", "type", "created", "object", "owned_by", "model_spec", "context_length"};

void report_coverage(const std::map<std::string, int>& coverage, std::size_t total) {
  std::cout << "coverage (engaged / " << total << "):\n";
  std::vector<std::string> never;
  for (const auto& [key, n] : coverage) {
    if (n == 0) {
      never.push_back(key);
      continue;
    }
    std::cout << "  " << std::left << std::setw(48) << key << ' ' << n << '/' << total << '\n';
  }
  if (!never.empty()) {
    // Reported, not failed: a modeled key the wire has never carried is
    // Venice's data, and no edit here turns it green.
    std::cout << "  NEVER OBSERVED (reported, not a failure): ";
    for (std::size_t i = 0; i < never.size(); ++i)
      std::cout << (i != 0U ? ", " : "") << never[i];
    std::cout << '\n';
  }
}

// One modality: the verbatim first entry, the per-level differences, the
// reconciliation and the coverage column.
auto report_modality(const std::vector<venice::Model>& models, std::string_view type) -> int {
  if (models.empty()) {
    std::cout << "  (no models of this type)\n";
    return EXIT_SUCCESS;
  }

  std::map<std::string, int> coverage;
  std::vector<Mismatch> mismatches;
  std::vector<std::string_view> spec_keys{kSharedSpecKeys.begin(), kSharedSpecKeys.end()};

  {
    using namespace venice::detail;
    const auto shared = table_keys(kModelSpecBoolFields);
    spec_keys.insert(spec_keys.end(), shared.begin(), shared.end());
    std::vector<std::string_view> extra;
    if (type == "image") extra = table_keys(kImageSpecBoolFields);
    else if (type == "inpaint") extra = table_keys(kInpaintSpecBoolFields);
    else if (type == "tts")
      extra = table_keys(kTtsSpecBoolFields, kTtsSpecStringFields, kTtsSpecListFields,
                         kTtsSpecObjectFields);
    else if (type == "music")
      extra = table_keys(kMusicSpecBoolFields, kMusicSpecIntFields,
                         kMusicSpecDoubleFields, kMusicSpecStringFields,
                         kMusicSpecIntListFields, kMusicSpecStringListFields);
    else if (type == "embedding")
      extra = table_keys(kEmbeddingSpecIntFields, kEmbeddingSpecBoolFields);
    spec_keys.insert(spec_keys.end(), extra.begin(), extra.end());
  }

  const auto constraint_keys = [&]() -> std::vector<std::string_view> {
    using namespace venice::detail;
    if (type == "image")
      return table_keys(kImageConstraintIntFields, kImageConstraintBoolFields,
                        kImageConstraintStringFields, kImageConstraintListFields,
                        kImageConstraintObjectFields);
    if (type == "inpaint")
      return table_keys(kInpaintConstraintIntFields, kInpaintConstraintBoolFields,
                        kInpaintConstraintStringFields, kInpaintConstraintListFields);
    if (type == "video")
      return table_keys(kVideoConstraintIntFields, kVideoConstraintDoubleFields,
                        kVideoConstraintBoolFields, kVideoConstraintStringFields,
                        kVideoConstraintListFields);
    if (type == "text") return table_keys(kTextConstraintObjectFields);
    return {};
  }();

  std::cout << "first entry model_spec, verbatim: " << models.front().raw["model_spec"].dump()
            << '\n';

  for (const auto& m : models) {
    using namespace venice::detail;
    const auto& raw = m.raw;
    const auto* spec = opt_object(raw, "model_spec");

    // One set-difference per nesting level. These print and do not fail: a key
    // nothing here names is how the seven undocumented video keys surfaced.
    report_unmodeled("unmodeled entry keys [" + m.id + "]: ", raw, kModeledEntryKeys, "Model::raw");
    if (spec == nullptr) continue;
    report_unmodeled("unmodeled model_spec keys [" + m.id + "]: ", *spec, spec_keys,
                     "Model::raw[\"model_spec\"]");
    walk_values(
        *spec, m, kModelSpecBoolFields, kIsBool,
        [](const auto& obj, const char* key) { return opt_bool(obj, key); },
        m.id, "model_spec", coverage, mismatches);
    if (const auto *pricing = opt_object(*spec, "pricing"))
      audit_model_pricing(*pricing, m, coverage, mismatches);
    if (const auto* c = opt_object(*spec, "constraints"))
      report_unmodeled("unmodeled constraint keys [" + m.id + "]: ", *c, constraint_keys,
                       "Model::raw[\"model_spec\"][\"constraints\"]");

    if (const auto* caps = opt_object(*spec, "capabilities")) {
      report_unmodeled(
          "unmodeled capability keys [" + m.id + "]: ", *caps,
          table_keys(kCapabilityBoolFields, kCapabilityIntFields,
                     kCapabilityStringFields, kCapabilityListFields),
          "Model::raw[...][\"capabilities\"]");
      if (m.capabilities) {
        walk_values(
            *caps, *m.capabilities, kCapabilityBoolFields, kIsBool,
            [](const auto& obj, const char* key) { return opt_bool(obj, key); },
            m.id, "capabilities", coverage, mismatches);
        walk_values(
            *caps, *m.capabilities, kCapabilityIntFields, kIsInt,
            [](const auto& obj, const char* key) { return opt_int(obj, key); },
            m.id, "capabilities", coverage, mismatches);
        walk_values(
            *caps, *m.capabilities, kCapabilityStringFields, kIsString,
            [](const auto& obj, const char* key) {
              return opt_string(obj, key);
            },
            m.id, "capabilities", coverage, mismatches);
        walk_flat_strings(*caps, *m.capabilities, kCapabilityListFields, m.id,
                          "capabilities", coverage, mismatches);
      }
    }

    if (const auto* dep = opt_object(*spec, "deprecation")) {
      report_unmodeled("unmodeled deprecation keys [" + m.id + "]: ", *dep,
                       table_keys(kDeprecationBoolFields, kDeprecationStringFields),
                       "Model::raw[...][\"deprecation\"]");
      walk(*dep, *m.deprecation, kDeprecationBoolFields, kIsBool, m.id, "deprecation", coverage,
           mismatches);
      walk(*dep, *m.deprecation, kDeprecationStringFields, kIsString, m.id, "deprecation",
           coverage, mismatches);
    }

    if (type == "image" && m.image) {
      walk(*spec, *m.image, kImageSpecBoolFields, kIsBool, m.id, "model_spec", coverage,
           mismatches);
      walk(*spec, *m.image, kImageSpecObjectFields, kIsObject, m.id, "model_spec", coverage,
           mismatches);
      if (const auto* c = opt_object(*spec, "constraints"); c != nullptr && m.image->constraints) {
        const auto& cc = *m.image->constraints;
        walk(*c, cc, kImageConstraintIntFields, kIsInt, m.id, "constraints", coverage, mismatches);
        walk(*c, cc, kImageConstraintBoolFields, kIsBool, m.id, "constraints", coverage,
             mismatches);
        walk(*c, cc, kImageConstraintStringFields, kIsString, m.id, "constraints", coverage,
             mismatches);
        walk(*c, cc, kImageConstraintListFields, kIsArray, m.id, "constraints", coverage,
             mismatches);
        walk(*c, cc, kImageConstraintObjectFields, kIsObject, m.id, "constraints", coverage,
             mismatches);
        if (const auto* st = opt_object(*c, "steps"); st != nullptr && cc.steps) {
          report_unmodeled("unmodeled steps keys [" + m.id + "]: ", *st,
                           table_keys(kStepsIntFields), "raw[...][\"constraints\"][\"steps\"]");
          walk(*st, *cc.steps, kStepsIntFields, kIsInt, m.id, "steps", coverage, mismatches);
        }
      }
    } else if (type == "inpaint" && m.inpaint) {
      walk(*spec, *m.inpaint, kInpaintSpecBoolFields, kIsBool, m.id, "model_spec", coverage,
           mismatches);
      walk(*spec, *m.inpaint, kInpaintSpecObjectFields, kIsObject, m.id, "model_spec", coverage,
           mismatches);
      if (const auto* c = opt_object(*spec, "constraints");
          c != nullptr && m.inpaint->constraints) {
        const auto& cc = *m.inpaint->constraints;
        walk(*c, cc, kInpaintConstraintIntFields, kIsInt, m.id, "constraints", coverage,
             mismatches);
        walk(*c, cc, kInpaintConstraintBoolFields, kIsBool, m.id, "constraints", coverage,
             mismatches);
        walk(*c, cc, kInpaintConstraintStringFields, kIsString, m.id, "constraints", coverage,
             mismatches);
        walk(*c, cc, kInpaintConstraintListFields, kIsArray, m.id, "constraints", coverage,
             mismatches);
      }
    } else if (type == "video" && m.video) {
      walk(*spec, *m.video, kVideoSpecObjectFields, kIsObject, m.id, "model_spec", coverage,
           mismatches);
      if (const auto* c = opt_object(*spec, "constraints"); c != nullptr && m.video->constraints) {
        const auto& cc = *m.video->constraints;
        walk(*c, cc, kVideoConstraintIntFields, kIsInt, m.id, "constraints", coverage, mismatches);
        walk(*c, cc, kVideoConstraintDoubleFields, kIsNumber, m.id, "constraints", coverage,
             mismatches);
        walk(*c, cc, kVideoConstraintBoolFields, kIsBool, m.id, "constraints", coverage,
             mismatches);
        walk(*c, cc, kVideoConstraintStringFields, kIsString, m.id, "constraints", coverage,
             mismatches);
        walk(*c, cc, kVideoConstraintListFields, kIsArray, m.id, "constraints", coverage,
             mismatches);
      }
    } else if (type == "tts" && m.tts) {
      walk(*spec, *m.tts, kTtsSpecBoolFields, kIsBool, m.id, "model_spec", coverage, mismatches);
      walk(*spec, *m.tts, kTtsSpecStringFields, kIsString, m.id, "model_spec", coverage,
           mismatches);
      walk(*spec, *m.tts, kTtsSpecListFields, kIsArray, m.id, "model_spec", coverage, mismatches);
      walk(*spec, *m.tts, kTtsSpecObjectFields, kIsObject, m.id, "model_spec", coverage,
           mismatches);
      if (const auto* vc = opt_object(*spec, "voice_cloning"); vc != nullptr && m.tts->voice_cloning) {
        report_unmodeled("unmodeled voice_cloning keys [" + m.id + "]: ", *vc,
                         table_keys(kVoiceCloningStringFields, kVoiceCloningDoubleFields,
                                    kVoiceCloningIntFields, kVoiceCloningListFields),
                         "raw[...][\"voice_cloning\"]");
        const auto& c = *m.tts->voice_cloning;
        walk(*vc, c, kVoiceCloningStringFields, kIsString, m.id, "voice_cloning", coverage,
             mismatches);
        walk(*vc, c, kVoiceCloningDoubleFields, kIsNumber, m.id, "voice_cloning", coverage,
             mismatches);
        walk(*vc, c, kVoiceCloningIntFields, kIsInt, m.id, "voice_cloning", coverage, mismatches);
        walk(*vc, c, kVoiceCloningListFields, kIsArray, m.id, "voice_cloning", coverage,
             mismatches);
      }
    } else if (type == "music" && m.music) {
      walk(*spec, *m.music, kMusicSpecBoolFields, kIsBool, m.id, "model_spec", coverage,
           mismatches);
      walk(*spec, *m.music, kMusicSpecIntFields, kIsInt, m.id, "model_spec", coverage,
           mismatches);
      walk(*spec, *m.music, kMusicSpecDoubleFields, kIsNumber, m.id, "model_spec", coverage,
           mismatches);
      walk(*spec, *m.music, kMusicSpecStringFields, kIsString, m.id, "model_spec", coverage,
           mismatches);
      walk(*spec, *m.music, kMusicSpecIntListFields, kIsArray, m.id, "model_spec", coverage,
           mismatches);
      walk(*spec, *m.music, kMusicSpecStringListFields, kIsArray, m.id, "model_spec", coverage,
           mismatches);
    } else if (type == "embedding" && m.embedding) {
      walk(*spec, *m.embedding, kEmbeddingSpecIntFields, kIsInt, m.id, "model_spec", coverage,
           mismatches);
      walk(*spec, *m.embedding, kEmbeddingSpecBoolFields, kIsBool, m.id, "model_spec", coverage,
           mismatches);
    } else if (type == "text" && m.text_constraints) {
      if (const auto* c = opt_object(*spec, "constraints"))
        walk(*c, *m.text_constraints, kTextConstraintObjectFields, kIsObject, m.id, "constraints",
             coverage, mismatches);
    }
  }

  // Absolute checks, written against what must be true rather than against
  // what this response happens to contain — the VC-37 rule. A guard that only
  // fires when the field is present cannot fire when the field is the thing
  // that went missing.
  int broken = 0;
  const bool modeled_type = type == "image" || type == "inpaint" || type == "video" ||
                            type == "tts" || type == "music" || type == "embedding";
  for (const auto& m : models) {
    const int engaged = static_cast<int>(m.image.has_value()) +
                        static_cast<int>(m.inpaint.has_value()) +
                        static_cast<int>(m.video.has_value()) + static_cast<int>(m.tts.has_value()) +
                        static_cast<int>(m.music.has_value()) +
                        static_cast<int>(m.embedding.has_value()) +
                        static_cast<int>(m.text_constraints.has_value());
    if (engaged > 1) {
      std::cerr << "  TWO VIEWS ENGAGED AT ONCE [" << m.id << "] -- the dispatch is not exclusive\n";
      ++broken;
    }
    if (modeled_type && m.raw["model_spec"].is_object() && engaged == 0) {
      std::cerr << "  NO VIEW ENGAGED [" << m.id << "] for a modeled type, model_spec: "
                << m.raw["model_spec"].dump() << '\n';
      ++broken;
    }
    if (modeled_type && venice::detail::opt_object(m.raw["model_spec"], "constraints") != nullptr) {
      const bool parsed = (m.image && m.image->constraints) ||
                          (m.inpaint && m.inpaint->constraints) ||
                          (m.video && m.video->constraints);
      if (!parsed && type != "tts" && type != "music" && type != "embedding") {
        std::cerr << "  CONSTRAINTS IN RAW BUT NOT PARSED [" << m.id << "]\n";
        ++broken;
      }
    }
  }

  for (const auto& mm : mismatches) {
    std::cerr << "  RECONCILIATION MISMATCH [" << mm.model << "] " << mm.level << '.' << mm.key
              << " -- " << mm.why << '\n';
    ++broken;
  }

  report_coverage(coverage, models.size());
  return broken == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

// `--modality [type]`: the per-modality typed metadata, against every entry.
auto show_modality(const venice::Client& client, std::string_view type) -> int {
  const std::vector<std::string_view> types =
      type.empty() || type == "all"
          ? std::vector<std::string_view>{"text",  "image", "inpaint", "video",  "tts",
                                          "embedding", "music", "asr",  "upscale"}
          : std::vector<std::string_view>{type};

  int worst = EXIT_SUCCESS;
  for (const auto& t : types) {
    const auto res = client.models(t);
    if (!res) {
      std::cerr << "models(" << t << ") failed [" << venice::to_string(res.error().kind) << "] "
                << res.error().message << '\n';
      worst = EXIT_FAILURE;
      continue;
    }
    std::cout << "\n=== " << t << ": " << res->size() << " models ===\n";

    if (report_modality(*res, t) != EXIT_SUCCESS) worst = EXIT_FAILURE;
  }
  return worst;
}

// `--characters [search]`: the live check VC-04 could not run offline, and the
// reason its PR says "documented, not captured".
//
// test/08characters/ pins the shape Venice's OpenAPI document describes, but
// nothing there has been on a wire: unlike /models, this endpoint answers 402
// unauthenticated and 401 to a junk bearer, so no fixture could be captured
// without a real key. The columns below are exactly the fields a TUI picker
// would branch on, and an absent one prints '?' — so a key that parses in the
// fixture and not in reality shows up as a blank column rather than passing
// quietly. If one disagrees, the fixture is what needs correcting;
// Character::raw is why that is recoverable.
//
// Two things this leg is specifically watching for, both of them guesses until
// it runs:
//
//   * that `slug` really is always present, since the parse skips entries
//     without one — a listing that comes back short is that guess being wrong.
//   * that the page really is 50 by default. The count printed below against a
//     `--characters` with no search is the whole evidence for the pagination
//     paragraph in client.hpp.
//
// The optional argument is a search term, which is also the cheapest way to see
// that a query parameter survives encoding: `--characters "alan watts"`.
auto list_characters(const venice::Client& client, std::string_view search) -> int {
  venice::CharacterQuery q;
  if (!search.empty()) q.search = std::string{search};

  const auto res = client.characters(q);
  if (!res) {
    // The body is printed and not just the message because this endpoint's
    // documented failures are 402 and 401, and "HTTP 402" alone names nothing.
    // The 402 body is where "Authentication required" actually appears.
    std::cerr << "characters failed [" << venice::to_string(res.error().kind) << "] "
              << res.error().message << '\n';
    if (!res.error().body.empty()) std::cerr << res.error().body << '\n';
    return EXIT_FAILURE;
  }

  // Both counts, because one number cannot answer both questions this leg
  // exists to ask. `returned` is the server's page — if it is 50 with no limit
  // set, the default-page claim holds. `entries` is what survived the parse —
  // if it is lower, `slug` is not always present after all, which is the other
  // guess. Printing only the second would make the two indistinguishable.
  std::cout << res->entries.size() << " usable of " << res->returned << " returned";
  if (!search.empty()) std::cout << " (search=" << search << ')';
  std::cout << '\n';
  if (res->returned > res->entries.size()) {
    std::cout << "   <-- " << res->returned - res->entries.size()
              << " entry/entries skipped for want of a slug: the schema says "
                 "slug is required, so this is the schema being wrong\n";
  }
  if (res->returned == 0) {
    std::cout << "   <-- ZERO returned: the account sees no characters at all\n";
  }

  for (const auto& c : res->entries) {
    std::cout << c.slug << "  model=";
    if (c.model_id) std::cout << *c.model_id;
    else std::cout << '?';

    std::cout << "  web=";
    if (c.web_enabled) std::cout << (*c.web_enabled ? "yes" : "no");
    else std::cout << '?';

    std::cout << "  adult=";
    if (c.adult) std::cout << (*c.adult ? "yes" : "no");
    else std::cout << '?';

    std::cout << "  rating=";
    if (c.stats && c.stats->average_rating) std::cout << *c.stats->average_rating;
    else std::cout << '?';

    std::cout << "  tags=" << c.tags.size();
    if (c.name) std::cout << "  (" << *c.name << ')';
    std::cout << '\n';
  }
  return EXIT_SUCCESS;
}

// ── the reviews half of `--character` (VC-36, #56) ────────────────────────
//
// Every key the reviews parse reads, one set per level. Four sets rather than
// one because the set-difference is per object, and the two-level version of
// this report is what VC-20 had to add after `cost` rode untyped for three
// releases beside a sub-object that was fully reported.
constexpr std::array<std::string_view, 4> kModeledReviewPageKeys{"data", "object", "pagination",
                                                                 "summary"};
constexpr std::array<std::string_view, 9> kModeledReviewKeys{
    "id",     "characterId",   "createdAt", "isOwner", "locale",
    "message", "userAvatarUrl", "username",  "rating"};
constexpr std::array<std::string_view, 4> kModeledPaginationKeys{"page", "pageSize", "total",
                                                                 "totalPages"};
constexpr std::array<std::string_view, 2> kModeledSummaryKeys{"averageRating", "totalReviews"};

// One page of reviews, printed verbatim beside the parse.
//
// A deliberately small pageSize, and it is a check rather than a courtesy: the
// response echoes the page size it used, so a query that never reached the
// server shows up as 20 where 5 was asked for. Nothing else in this leg can see
// a dropped query string.
//
// `stats` is what /characters/{slug} reported for the same character. Two
// endpoints quote the same average, and until this ran nothing had ever
// compared them.
auto show_character_reviews(const venice::Client& client, std::string_view slug,
                            const std::optional<venice::CharacterStats>& stats) -> int {
  constexpr int kPageSize = 5;

  std::cout << "\n-- reviews (page 1 of " << kPageSize << ") --\n";
  const auto res = client.character_reviews(slug, {.page = 1, .page_size = kPageSize});
  if (!res) {
    std::cerr << "character reviews failed [" << venice::to_string(res.error().kind) << "] "
              << res.error().message << '\n';
    if (!res.error().body.empty()) std::cerr << res.error().body << '\n';
    return EXIT_FAILURE;
  }

  std::cout << "reviews, verbatim  : " << res->raw.dump() << '\n';

  bool broken = false;

  // The envelope, then each level inside it.
  report_unmodeled("unmodeled page keys: ", res->raw, kModeledReviewPageKeys,
                   "CharacterReviewPage::raw");

  const auto* raw_data = venice::detail::opt_array(res->raw, "data");
  if (raw_data == nullptr) {
    std::cerr << "  NO `data` ARRAY IN THE ENVELOPE -- the wire shape moved\n";
    broken = true;
  } else if (raw_data->size() != res->returned) {
    std::cerr << "  RETURNED (" << res->returned << ") != raw data length (" << raw_data->size()
              << ") -- the page count is wrong\n";
    broken = true;
  }

  std::cout << "typed returned     : " << res->returned << " (entries parsed: "
            << res->entries.size() << ")\n";

  if (res->pagination) {
    const auto show = [](const char* label, const std::optional<int>& v) {
      std::cout << label;
      if (v) std::cout << *v;
      else std::cout << "(absent -- not an integer this platform can hold)";
      std::cout << '\n';
    };
    show("typed page         : ", res->pagination->page);
    show("typed pageSize     : ", res->pagination->page_size);
    show("typed total        : ", res->pagination->total);
    show("typed totalPages   : ", res->pagination->total_pages);

    if (res->pagination->page_size && *res->pagination->page_size != kPageSize)
      std::cerr << "   <-- ASKED FOR " << kPageSize
                << ": either the query never arrived or the server capped it\n";
    if (const auto* raw_pagination = venice::detail::opt_object(res->raw, "pagination"))
      report_unmodeled("unmodeled pagination keys: ", *raw_pagination, kModeledPaginationKeys,
                       "CharacterReviewPage::raw");
  } else {
    std::cout << "typed pagination   : (absent)\n";
    if (venice::detail::opt_object(res->raw, "pagination") != nullptr) {
      std::cerr << "  TYPED PAGINATION ABSENT while a pagination object arrived -- parse bug\n";
      broken = true;
    }
  }

  if (res->summary) {
    std::cout << "typed avg rating   : ";
    if (res->summary->average_rating) std::cout << *res->summary->average_rating;
    else std::cout << "(absent)";
    std::cout << "\ntyped totalReviews : ";
    if (res->summary->total_reviews) std::cout << *res->summary->total_reviews;
    else std::cout << "(absent)";
    std::cout << '\n';

    // The cross-endpoint check. /characters/{slug} and this endpoint both quote
    // an average for the same character; a disagreement is reported and not
    // failed, because rounding and caching are the server's business and this
    // leg has no evidence about which one is authoritative.
    if (stats && stats->average_rating && res->summary->average_rating &&
        *stats->average_rating != *res->summary->average_rating)
      std::cout << "   <-- the character object said " << *stats->average_rating
                << ": the two endpoints round or cache differently\n";

    if (const auto* raw_summary = venice::detail::opt_object(res->raw, "summary"))
      report_unmodeled("unmodeled summary keys: ", *raw_summary, kModeledSummaryKeys,
                       "CharacterReviewPage::raw");
  } else {
    std::cout << "typed summary      : (absent)\n";
    if (venice::detail::opt_object(res->raw, "summary") != nullptr) {
      std::cerr << "  TYPED SUMMARY ABSENT while a summary object arrived -- parse bug\n";
      broken = true;
    }
  }

  if (res->entries.empty()) {
    std::cout << "   <-- ZERO usable reviews on this page\n";
    return broken ? EXIT_FAILURE : EXIT_SUCCESS;
  }

  for (const auto& r : res->entries) {
    std::cout << "  ";
    if (r.rating) std::cout << *r.rating;
    else std::cout << "?";
    std::cout << "  " << r.username.value_or("(no username)") << "  "
              << r.created_at.value_or("(no date)");
    if (r.message) std::cout << "\n      " << *r.message;
    std::cout << '\n';

    report_unmodeled("  unmodeled review keys: ", r.raw, kModeledReviewKeys,
                     "CharacterReview::raw");

    // Raw against typed, per entry: an absent typed rating beside a numeric raw
    // one is a parse bug, which is the only one of the three explanations for a
    // blank field that this leg can settle.
    const auto raw_rating = r.raw.find("rating");
    if (raw_rating != r.raw.end() && raw_rating->is_number() && !r.rating) {
      std::cerr << "  RAW HAS A NUMERIC rating AND THE PARSE DOES NOT -- parse bug\n";
      broken = true;
    }
  }

  return broken ? EXIT_FAILURE : EXIT_SUCCESS;
}

// `--character <slug>`: the direct-fetch counterpart to --characters (VC-16,
// #26). Print the whole object because this is a preview API and a typed blank
// cannot distinguish server absence from a parser looking in the wrong place.
// The typed-vs-raw slug comparison makes the leg a check rather than a viewer.
// Since VC-36 it continues into one page of that character's reviews.
auto show_character(const venice::Client& client, std::string_view slug) -> int {
  if (slug.empty()) {
    std::cerr << "--character requires a slug\n";
    return EXIT_FAILURE;
  }

  const auto res = client.character(slug);
  if (!res) {
    std::cerr << "character failed [" << venice::to_string(res.error().kind) << "] "
              << res.error().message << '\n';
    if (!res.error().body.empty()) std::cerr << res.error().body << '\n';
    return EXIT_FAILURE;
  }

  std::cout << "character, verbatim: " << res->raw.dump() << '\n';
  std::cout << "typed slug         : " << (res->slug.empty() ? "(absent)" : res->slug) << '\n';
  std::cout << "typed model        : "
            << (res->model_id ? *res->model_id : "(absent)") << '\n';
  std::cout << "typed name         : " << (res->name ? *res->name : "(absent)") << '\n';

  // Absolute, not relative, and VC-37 (#57) is what it cost to learn that here.
  // The comparison below can only fire when the parse is nearly right: it asks
  // whether raw's slug *disagrees*, and when the whole response turned out to
  // be an envelope there was no top-level slug key to disagree with, so it was
  // skipped and the leg passed while every typed field was blank. A fetch that
  // parses no slug at all has failed, whatever raw does or does not contain.
  if (res->slug.empty()) {
    std::cerr << "TYPED SLUG ABSENT -- the parse found no slug in a 200 response\n";
    return EXIT_FAILURE;
  }

  const auto raw_slug = res->raw.find("slug");
  if (raw_slug != res->raw.end() && !raw_slug->is_string()) {
    std::cerr << "RAW SLUG IS NOT A STRING -- the wire shape moved\n";
    return EXIT_FAILURE;
  }
  if (raw_slug != res->raw.end() && raw_slug->get<std::string>() != res->slug) {
    std::cerr << "RAW AND TYPED SLUG DISAGREE -- Character::from_json moved\n";
    return EXIT_FAILURE;
  }

  return show_character_reviews(client, slug, res->stats);
}

// The live check VC-05 could not run offline, and the reason the PR says
// "documented, not measured".
//
// Everything in stream.hpp about *shape* — where reasoning_content sits, where
// cached_tokens is nested, how tool-call fragments are keyed — came from
// Venice's published docs rather than a capture, because /models answers for
// any bearer token but chat does not, and the implementing environment had no
// key. This is where a key-holder confirms it. Run:
//
//     VENICE_API_KEY=... venice-cpp --stream "think step by step: 2+2?"
//
// and read the report. Every line of it is a claim the offline suite asserts
// against a hand-written fixture; if one disagrees with the live API, the
// fixture in test/07stream/ is what needs correcting.
auto stream_report(const venice::Client& client, const std::string& prompt) -> int {
  // The model is chosen from the live list by capability, not hardcoded. This
  // leg used to name `deepseek-r1-671b` and by 2026-08-09 that model was gone
  // from the catalogue, so the one command that settles VC-05 answered HTTP 404
  // instead — a live check that cannot run is worse than no live check, because
  // it looks like a failure of the thing under test. It is the same hazard the
  // --models leg records about hardcoded *counts*, and a model id ages faster.
  // Picking on supports_reasoning also gives that VC-03 flag a live use, the
  // way tools_report does for supports_function_calling.
  //
  // It also names the runners-up, which it did not until VC-17. Picking the
  // first entry and printing only that is what let #28 be filed against the one
  // model family that answers this question differently from most; see
  // pick_by_capability.
  const auto pick = pick_by_capability(client, &venice::ModelCapabilities::supports_reasoning,
                                       "supportsReasoning");
  if (!pick) return EXIT_FAILURE;
  report_pick(*pick, "supportsReasoning", "`venice-cpp --usage <id>`");

  venice::ChatRequest req;
  req.model = pick->chosen;  // a reasoning model, so thinking has somewhere to come from
  req.messages = {venice::Message::user(prompt)};

  venice::StreamAccumulator acc;
  std::size_t content_frames = 0;
  std::size_t reasoning_frames = 0;

  const auto res = client.chat_stream(req, acc, [&](const venice::StreamDelta& d) {
    if (d.reasoning_content) ++reasoning_frames;
    if (d.content) {
      ++content_frames;
      std::cout << *d.content << std::flush;
    }
    return true;
  });
  std::cout << '\n';

  if (!res) {
    std::cerr << "stream failed [" << venice::to_string(res.error().kind) << "] "
              << res.error().message << '\n';
    return EXIT_FAILURE;
  }

  const auto msg = acc.message();
  std::cerr << "\n-- what the stream carried --\n"
            << "chunks retained    : " << acc.chunks().size() << '\n'
            << "content frames     : " << content_frames << '\n'
            << "reasoning frames   : " << reasoning_frames
            << (reasoning_frames == 0
                    ? "   <-- ZERO: either the model did not think, or"
                      " reasoning_content is not where we look\n"
                    : "\n")
            << "reasoning chars    : " << (msg.reasoning_content ? msg.reasoning_content->size() : 0)
            << '\n'
            << "tool calls         : " << (msg.tool_calls ? msg.tool_calls->size() : 0) << '\n'
            << "finish_reason      : " << res->finish_reason << '\n';

  // Absence here is a fact about the model, not a symptom. Both of these read
  // "(absent -- check the nesting)" until VC-17, which pointed the reader at a
  // parse bug that does not exist: the nestings are right, and whether they
  // arrive is per-family. `--usage` prints the verbatim object, which is the
  // only thing that can tell those two apart.
  if (res->usage) {
    std::cerr << "prompt/completion  : " << res->usage->prompt_tokens << '/'
              << res->usage->completion_tokens << '\n'
              << "reasoning_tokens   : ";
    if (res->usage->reasoning_tokens) std::cerr << *res->usage->reasoning_tokens << '\n';
    else std::cerr << "(absent -- this family does not report one; venice-cpp --usage)\n";
    std::cerr << "cached_tokens      : ";
    if (res->usage->cached_tokens) std::cerr << *res->usage->cached_tokens << '\n';
    else std::cerr << "(absent -- this family does not report one; venice-cpp --usage)\n";
  } else {
    std::cerr << "usage              : (no usage frame arrived at all)\n";
  }

  // The streamed cost, off the assembled response rather than the chunks — the
  // only place a reader looking at the reply's *shape* sees the field at all.
  // `--usage` is the check; this is the shape report, and a field missing from
  // it reads as a gap in the library rather than a gap in the leg.
  std::cerr << "cost               : ";
  if (res->cost && res->cost->diem) std::cerr << *res->cost->diem << " diem\n";
  else if (res->cost) std::cerr << "(object arrived, no diem in it)\n";
  else std::cerr << "(no cost frame arrived at all)\n";

  // The claim that matters most, and the one no fixture can settle: whether the
  // assembled turn is something Venice will accept back.
  std::cerr << "\n-- the turn, as it would be replayed --\n"
            << nlohmann::json(msg).dump(2) << '\n';
  return EXIT_SUCCESS;
}

// `--structured [model]`: VC-59's discriminator for two conflicting first-party
// contracts. The Structured Responses guide publishes OpenAI's
// name/strict/schema wrapper, while the Chat Completions reference publishes
// the schema object directly. Fixtures can prove what this client emits; only
// the server can say which spelling it accepts.
//
// The prompt is read from stdin and never printed. Successful model output is
// parsed and checked but never printed. Error bodies are represented only by
// their byte count, so a gateway cannot reflect either back into the report.
enum class StructuredShape { Wrapper, Direct };

[[nodiscard]] auto structured_format(StructuredShape shape,
                                     const nlohmann::json &schema)
    -> nlohmann::json {
  if (shape == StructuredShape::Wrapper)
    return venice::response_format::json_schema("vc59_response", schema);

  auto format = nlohmann::json::object();
  format["type"] = "json_schema";
  format["json_schema"] = schema;
  return format;
}

[[nodiscard]] auto structured_json_diagnosis(const nlohmann::json &parsed)
    -> std::string_view {
  if (!parsed.is_object())
    return "not-object";
  if (!parsed.contains("answer"))
    return "answer-missing";
  if (!parsed.at("answer").is_string())
    return "answer-not-string";
  if (parsed.size() != 1)
    return "extra-keys";
  return "conformant";
}

[[nodiscard]] auto
structured_output_diagnosis(const venice::ChatResponse &response)
    -> std::string_view {
  if (!response.message || !response.message->content)
    return "empty";
  const auto &content = *response.message->content;
  if (!content.is_string())
    return structured_json_diagnosis(content);
  try {
    return structured_json_diagnosis(
        nlohmann::json::parse(content.get_ref<const std::string &>()));
  } catch (const std::exception &) {
    return "invalid-json";
  }
}

[[nodiscard]] auto
structured_output_conforms(const venice::ChatResponse &response) -> bool {
  return structured_output_diagnosis(response) == "conformant";
}

void report_structured_result(
    std::string_view shape, std::string_view mode,
    const std::expected<venice::ChatResponse, venice::Error> &result) {
  std::cerr << shape << ' ' << mode << ": ";
  if (!result) {
    std::cerr << "rejected [" << venice::to_string(result.error().kind)
              << "] status=" << result.error().status << " body=[redacted "
              << result.error().body.size() << " bytes]\n";
    return;
  }

  std::cerr << "accepted; schema=" << structured_output_diagnosis(*result)
            << "; content_type=";
  if (result->message && result->message->content)
    std::cerr << result->message->content->type_name();
  else
    std::cerr << "absent";
  if (result->message && result->message->content &&
      result->message->content->is_string()) {
    const auto &text = result->message->content->get_ref<const std::string &>();
    const auto first = text.find_first_not_of(" \t\r\n");
    const auto last = text.find_last_not_of(" \t\r\n");
    std::cerr << "; output_bytes=" << text.size() << "; object_delimiters="
              << (first != std::string::npos && text[first] == '{' &&
                          last != std::string::npos && text[last] == '}'
                      ? "yes"
                      : "no")
              << "; code_fence="
              << (text.find("```") != std::string::npos ? "yes" : "no");
  }
  if (result->usage)
    std::cerr << "; tokens=" << result->usage->prompt_tokens << '/'
              << result->usage->completion_tokens;
  if (result->cost) {
    if (result->cost->usd)
      std::cerr << "; cost_usd=" << *result->cost->usd;
    if (result->cost->diem)
      std::cerr << "; cost_diem=" << *result->cost->diem;
  }
  std::cerr << '\n';
}

auto structured_report(const venice::Client &client, std::string_view model)
    -> int {
  ModelPick pick;
  if (model.empty()) {
    auto picked = pick_by_capability(
        client, &venice::ModelCapabilities::supports_response_schema,
        "supportsResponseSchema");
    if (!picked)
      return EXIT_FAILURE;
    pick = *std::move(picked);
  } else {
    const auto models = client.models("text");
    if (!models) {
      std::cerr << "models failed [" << venice::to_string(models.error().kind)
                << "] " << models.error().message << '\n';
      return EXIT_FAILURE;
    }
    const auto it =
        std::ranges::find_if(*models, [&](const venice::Model &candidate) {
          return candidate.id == model;
        });
    if (it == models->end() || !it->capabilities ||
        it->capabilities->supports_response_schema != true) {
      std::cerr << "named model does not report supportsResponseSchema=true\n";
      return EXIT_FAILURE;
    }
    pick.chosen = std::string{model};
  }
  report_pick(pick, "supportsResponseSchema", "`venice-cpp --structured <id>`");

  std::string prompt;
  if (!std::getline(std::cin, prompt) || prompt.empty()) {
    std::cerr << "--structured requires a non-empty prompt on stdin; it is "
                 "never printed\n";
    return EXIT_FAILURE;
  }

  auto schema = nlohmann::json::object();
  schema["type"] = "object";
  schema["properties"] = nlohmann::json::object();
  schema["properties"]["answer"] = nlohmann::json{{"type", "string"}};
  schema["required"] = nlohmann::json::array({"answer"});
  schema["additionalProperties"] = false;

  bool wrapper_buffered = false;
  bool wrapper_streamed = false;
  bool direct_buffered = false;
  bool direct_streamed = false;

  for (const auto shape : {StructuredShape::Wrapper, StructuredShape::Direct}) {
    venice::ChatRequest request;
    request.model = pick.chosen;
    request.messages = {venice::Message::user(prompt)};
    request.max_tokens = 32;
    request.parallel_tool_calls = false;
    request.response_format = structured_format(shape, schema);

    const auto label = shape == StructuredShape::Wrapper ? "wrapper" : "direct";
    const auto buffered = client.chat(request);
    report_structured_result(label, "buffered", buffered);
    const bool buffered_ok = buffered && structured_output_conforms(*buffered);

    venice::StreamAccumulator accumulator{/*keep_chunks=*/true};
    const auto streamed = client.chat_stream(request, accumulator);
    report_structured_result(label, "streamed", streamed);
    std::size_t string_content_frames = 0;
    std::size_t object_content_frames = 0;
    std::size_t other_content_frames = 0;
    for (const auto &chunk : accumulator.chunks()) {
      const auto *choices = venice::detail::opt_array(chunk, "choices");
      if (choices == nullptr)
        continue;
      for (const auto &choice : *choices) {
        const auto *delta = venice::detail::opt_object(choice, "delta");
        if (delta == nullptr)
          continue;
        const auto content = delta->find("content");
        if (content == delta->end())
          continue;
        if (content->is_string())
          ++string_content_frames;
        else if (content->is_object())
          ++object_content_frames;
        else
          ++other_content_frames;
      }
    }
    std::cerr << label
              << " streamed content frames: string=" << string_content_frames
              << "; object=" << object_content_frames
              << "; other=" << other_content_frames << '\n';
    const bool streamed_ok = streamed && structured_output_conforms(*streamed);

    if (shape == StructuredShape::Wrapper) {
      wrapper_buffered = buffered_ok;
      wrapper_streamed = streamed_ok;
    } else {
      direct_buffered = buffered_ok;
      direct_streamed = streamed_ok;
    }
  }

  std::cerr << "summary: wrapper="
            << (wrapper_buffered && wrapper_streamed ? "both" : "no")
            << "; direct="
            << (direct_buffered && direct_streamed ? "both" : "no") << '\n';
  return (wrapper_buffered && wrapper_streamed) ||
                 (direct_buffered && direct_streamed)
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}

// `--tools [model]`: the live check VC-08 could not run offline, for the same
// reason --stream exists. Every offline case in test/02request/ asserts what
// to_json_body *emits*; none of them can assert that Venice *accepts* it. The
// nesting under "function", the spelling of tool_choice, whether
// parallel_tool_calls is even honoured on this API — all of it is read off
// Venice's published docs.
//
//     VENICE_API_KEY=... venice-cpp --tools
//
// Two legs, and the second is the one that matters. Leg one offers a function
// and sees whether a tool_call comes back — that alone proves the `tools` shape
// parsed. Leg two answers the call and sends the whole history back, which is
// the only thing that proves the assistant turn plus a tool result is a
// conversation Venice will continue. A run that stops after leg one has checked
// half the round trip.
//
// If a field disagrees with the fixtures, the fixture is what needs correcting;
// ChatResponse::raw and Message::raw are why a wrong guess here is recoverable.
auto tools_report(const venice::Client& client, std::string_view model) -> int {
  // Picking by capability gives VC-03's supports_function_calling flag its
  // first live use — a flag nothing has ever branched on is a flag nobody knows
  // is parsed — and collecting the runners-up is the fix VC-18 paid for. That
  // logic lives in pick_by_capability now, because VC-17 found --stream had
  // never grown it and filed a wrong conclusion as a result.
  ModelPick pick;
  if (model.empty()) {
    auto picked = pick_by_capability(client, &venice::ModelCapabilities::supports_function_calling,
                                     "supportsFunctionCalling");
    if (!picked) return EXIT_FAILURE;
    pick = *std::move(picked);
  } else {
    pick.chosen = std::string{model};
  }
  const std::string& chosen = pick.chosen;
  const auto& alternates = pick.alternates;

  venice::ChatRequest req;
  req.model = chosen;
  req.messages = {venice::Message::user("What is the weather in San Francisco?")};
  req.tools = std::vector<nlohmann::json>{venice::tools::function(
      "get_weather", "Look up the current weather for a city",
      nlohmann::json::parse(R"({"type":"object",
                                "properties":{"city":{"type":"string"}},
                                "required":["city"]})"))};
  req.tool_choice = venice::tool_choice::automatic();
  req.parallel_tool_calls = false;

  report_pick(pick, "supportsFunctionCalling", "`venice-cpp --tools <id>`");
  std::cerr << "\n-- leg 1: the body actually sent --\n" << req.to_json_body(false).dump(2) << '\n';

  const auto first = client.chat(req);
  if (!first) {
    std::cerr << "\nchat failed [" << venice::to_string(first.error().kind) << "] "
              << first.error().message << "\nbody: " << first.error().body << '\n';
    return EXIT_FAILURE;
  }

  // Borrowed, not copied, and it outlives every use below — `first` is alive to
  // the end of the function. nullptr covers three different absences at once: no
  // message, no tool_calls, or an engaged-but-empty list, and that last one is
  // why the count rather than the optional decides. front() on an empty vector
  // would be UB, and a gateway echoing "tool_calls": [] is not far-fetched.
  const auto* calls = first->message && first->message->tool_calls
                          ? &*first->message->tool_calls
                          : nullptr;
  const auto count = calls != nullptr ? calls->size() : 0;

  std::cerr << "\n-- leg 1: what came back --\n"
            << "finish_reason      : " << first->finish_reason << '\n'
            << "tool calls         : " << count;
  if (count == 0) {
    std::cerr << "   <-- ZERO: the request was accepted but nothing was called."
                 " Either the model declined, or `tools` is not reaching it\n";
    return EXIT_SUCCESS;
  }
  std::cerr << '\n';

  const auto& call = calls->front();
  std::cerr << "id / name          : " << call.id << " / " << call.name << '\n'
            << "arguments          : " << call.arguments << '\n';
  if (const auto parsed = call.parsed_arguments(); !parsed)
    std::cerr << "arguments DID NOT PARSE: " << parsed.error().message << '\n';

  std::cerr << "thought_signature  : ";
  if (call.thought_signature)
    std::cerr << "present, " << call.thought_signature->size()
              << " chars  <-- this family requires it echoed on leg 2 (VC-18)\n";
  else
    std::cerr << "(none -- this family does not use one)\n";

  // Leg two. Replay the assistant turn verbatim, then answer the call.
  //
  // Read the echo off the SERIALIZED body, not off the struct. VC-18's defect
  // lived in ToolCall::to_json, downstream of the field — re-reading
  // call.thought_signature here would only restate what leg 1 just printed and
  // would have stayed quiet through the entire bug.
  auto assistant_turn = *first->message;
  const nlohmann::json replayed = assistant_turn;
  const bool echoed = replayed.contains("tool_calls") && !replayed.at("tool_calls").empty() &&
                      replayed.at("tool_calls").at(0).contains("thought_signature");
  const bool signature_lost = call.thought_signature.has_value() && !echoed;

  std::cerr << "\n-- the turn, as it would be replayed --\n" << replayed.dump(2) << '\n';
  if (signature_lost)
    std::cerr << "\nSIGNATURE SEEN BUT NOT ECHOED -- VC-18 has regressed; leg 2 will 400\n";

  req.messages.push_back(std::move(assistant_turn));
  req.messages.push_back(venice::Message::tool(call.id, R"({"temp_f":68,"sky":"fog"})"));

  const auto second = client.chat(req);
  if (!second) {
    std::cerr << "\n-- leg 2 REJECTED --\n[" << venice::to_string(second.error().kind) << "] "
              << second.error().message << "\nbody: " << second.error().body
              << "\n\nThis is the half no fixture can settle: the turn assembled here is not"
                 " something Venice will take back.\nRun the same two legs on another family"
                 " before reading this as a library bug:\n"
              // The runners-up are only on screen when this leg auto-picked. A
              // caller who named the model got no such line, so pointing at
              // "the list above" would be pointing at nothing.
              << (alternates.empty()
                      ? "`venice-cpp --tools` with no argument lists other models that claim it.\n"
                      : "`venice-cpp --tools <id>` with any id from the list above.\n");
    return EXIT_FAILURE;
  }

  std::cerr << "\n-- leg 2: the tool result was accepted --\n";
  std::cout << second->content << '\n';
  // A pass is not enough. A future model that tolerates a stripped signature
  // would let VC-18 regress silently behind a green leg 2, so a signature seen
  // and not carried back fails the run on its own.
  return signature_lost ? EXIT_FAILURE : EXIT_SUCCESS;
}

// ── `--usage [model]`: what Venice actually puts in a response ────────────
//
// The leg VC-17 (#28) needed and did not have, since VC-20 (#34) covering the
// response's billing metadata whole rather than only the `usage` object.
// `Usage` models two nested fields, `prompt_tokens_details.cached_tokens` and
// `completion_tokens_details.reasoning_tokens`, and no offline fixture can say
// whether the API sends them — #28 was filed claiming it does not, off a single
// model that does not.
//
// So this prints the **verbatim** objects from both the streaming and the
// non-streaming path, beside the typed structs parsed from them. Verbatim is
// the whole point: a typed field reading absent means either "the server did
// not send it" or "we are looking in the wrong place", and only the raw object
// distinguishes them. That ambiguity is what cost #28 a wrong premise.
//
// It is a check and not a printer. A modeled key present in the raw object but
// absent from the typed struct is a parse bug — case (c) in #28's own scope,
// the only one of the three that is — and fails the run.
//
// **And it reports the whole envelope, not only the sub-object it was written
// for.** That is #34's finding rather than its subject: `cost` is a sibling of
// `usage`, so the unmodeled-key report below could never have named it, and it
// rode untyped for three releases with nothing looking one level up. The
// envelope report found `service_tier` on four families and four more keys on
// llama-3.3-70b the first time it ran.

// Every key Usage reads, so anything else in the object can be named as
// unmodeled rather than disappearing silently into ChatResponse::raw.
constexpr std::array<std::string_view, 6> kModeledUsageKeys{
    "prompt_tokens",  "completion_tokens",        "total_tokens",
    "cached_tokens",  "prompt_tokens_details",    "completion_tokens_details"};

// True when the typed parse disagrees with the object it came from.
auto report_usage(const nlohmann::json& raw, const std::optional<venice::Usage>& typed) -> bool {
  std::cerr << "usage, verbatim    : " << raw.dump() << '\n';
  if (!typed) {
    std::cerr << "  TYPED USAGE ABSENT while a usage object arrived -- parse bug\n";
    return true;
  }

  // What the raw object says, independent of the parse. nullopt means the key
  // is not there at all; a present 0 is a real answer and not the same thing —
  // both deepseek-v4-pro and llama-3.3-70b report an explicit cached_tokens 0,
  // which is why Usage's fields are optional rather than defaulted.
  const auto nested = [&raw](const char* obj, const char* key) -> std::optional<int> {
    const auto o = raw.find(obj);
    if (o == raw.end() || !o->is_object()) return std::nullopt;
    const auto k = o->find(key);
    if (k == o->end()) return std::nullopt;
    return venice::detail::integer_if_representable<int>(*k);
  };
  const auto raw_cached = nested("prompt_tokens_details", "cached_tokens");
  const auto raw_cache_write =
      nested("prompt_tokens_details", "cache_creation_input_tokens");
  const auto raw_reasoning = nested("completion_tokens_details", "reasoning_tokens");

  const auto show = [](const char* label, std::optional<int> from_raw, std::optional<int> from_typed) {
    std::cerr << label;
    if (from_typed) std::cerr << *from_typed;
    else std::cerr << "(absent)";
    if (from_raw != from_typed)
      std::cerr << "   <-- RAW SAYS " << (from_raw ? std::to_string(*from_raw) : "(absent)")
                << ": the wire moved and the parse did not";
    std::cerr << '\n';
  };
  show("typed cached_tokens: ", raw_cached, typed->cached_tokens);
  show("typed cache writes : ", raw_cache_write, typed->cache_creation_input_tokens);
  show("typed reasoning    : ", raw_reasoning, typed->reasoning_tokens);

  // Name what we do not model. cache_read_input_tokens turned up this way — a
  // flat sibling mirroring the nested cached_tokens, redundant in all 21
  // captures of the VC-17 sweep, which is why it stays untyped.
  report_unmodeled("unmodeled usage keys: ", raw, kModeledUsageKeys, "ChatResponse::raw");

  return raw_cached != typed->cached_tokens ||
         raw_cache_write != typed->cache_creation_input_tokens ||
         raw_reasoning != typed->reasoning_tokens;
}

// Every key `ChatResponse` reads off the response *envelope* — id, model,
// created, system_fingerprint, venice_parameters, choices, usage, cost — plus
// `object`, which it does NOT read. That one is here deliberately and is the
// only entry that is not a modeled field: it is the discriminator
// ("chat.completion" vs "chat.completion.chunk"), it is on every body, and
// naming it every run would bury the keys this report exists to surface. Any
// OTHER known-but-untyped key belongs in the report, not in here — the value of
// the list is that it is short and every entry is accounted for.
//
// The list above is the same idea one level down, and the gap between them is
// VC-20 (#34): `cost` is a sibling of `usage`, not a key inside it, so
// report_usage's set-difference can never see it at any value of
// kModeledUsageKeys. It rode untyped for three releases not because anyone
// deferred it but because no leg ever named the keys *beside* the sub-object it
// was written for.
//
// Note this is the NON-STREAMED key set. A chunk envelope models strictly less
// — note_envelope takes only id and model, delta_from_chunk adds choices, usage
// and cost — so created / system_fingerprint / venice_parameters on a chunk are
// filtered out here while the accumulator does in fact drop them. Under-reports
// the stream, and said out loud rather than left to be found.
constexpr std::array<std::string_view, 10> kModeledBodyKeys{
    "id",      "object", "model", "created", "system_fingerprint",
    "choices", "usage",  "cost",  "venice_parameters", "prompt_logprobs"};

// Name what an envelope carries that nothing models. Separate from report_cost
// because the discovery check must not be gated on the thing being discovered:
// if Venice renames `cost`, this still runs and still names the new key.
void report_envelope_keys(const nlohmann::json& envelope) {
  report_unmodeled("unmodeled body keys: ", envelope, kModeledBodyKeys, "ChatResponse::raw");
}

// True when the typed parse disagrees with the envelope it came from.
//
// Takes the whole body or chunk, not a sub-object — that is the entire point.
auto report_cost(const nlohmann::json& envelope, const std::optional<venice::Price>& typed)
    -> bool {
  const auto c = envelope.find("cost");

  if (c == envelope.end()) {
    std::cerr << "cost, verbatim     : (no cost key in the body at all)\n";
    // Not a bug. Every family swept for VC-20 sent one, but reading an absence
    // as a parse error is exactly the mistake that filed #28 against a shape
    // that was merely per-family.
    return false;
  }
  if (!c->is_object()) {
    // A DIFFERENT answer from the one above, and collapsing the two would undo
    // this leg's reason for existing: the key is here and has changed shape, so
    // the typed field is disengaged because we are looking in the wrong place,
    // not because the server went quiet. Fails the run.
    std::cerr << "cost, verbatim     : " << c->dump()
              << "\n  COST IS PRESENT BUT NOT AN OBJECT -- the wire shape moved\n";
    return true;
  }
  std::cerr << "cost, verbatim     : " << c->dump() << '\n';
  if (!typed) {
    std::cerr << "  TYPED COST ABSENT while a cost object arrived -- parse bug\n";
    return true;
  }

  // What the envelope says, independent of the parse — and read through the
  // SAME predicate detail::opt_double uses. is_number(), not is_number_float():
  // the capture in #34 is `"usd":0`, a JSON *integer*, and a float-only reader
  // would call it absent and then report a mismatch against a parse that is
  // behaving exactly as documented.
  const auto wire = [&c](const char* key) -> std::optional<double> {
    const auto k = c->find(key);
    if (k == c->end() || !k->is_number()) return std::nullopt;
    return k->get<double>();
  };
  // The wire type is half the question the sweep exists to answer, so print it
  // rather than only the value.
  const auto kind = [&c](const char* key) -> const char* {
    const auto k = c->find(key);
    if (k == c->end()) return "missing";
    if (k->is_number_integer()) return "integer";
    if (k->is_number_float()) return "float";
    return "not a number";
  };
  std::cerr << "cost wire types    : usd " << kind("usd") << ", diem " << kind("diem") << '\n';

  // Read once each, so the numbers printed below and the numbers that decide
  // the exit status are the same reads rather than two a reader has to prove
  // equal. Matches report_usage's raw_cached / raw_reasoning above.
  const auto raw_usd = wire("usd");
  const auto raw_diem = wire("diem");

  // max_digits10 on both sides. Default ostream precision is 6 significant
  // digits and std::to_string is 6 *decimal places*, which on a tool whose one
  // job is reporting the verbatim wire value would print a diem of 1.7e-7 as
  // "0.000000" — a zero, in the leg whose entire finding is that a printed zero
  // must not be confused with a real one.
  const auto num = [](double v) {
    std::ostringstream os;
    os << std::setprecision(std::numeric_limits<double>::max_digits10) << v;
    return os.str();
  };
  const auto show = [&num](const char* label, std::optional<double> from_raw,
                           std::optional<double> from_typed) {
    std::cerr << label << (from_typed ? num(*from_typed) : "(absent)");
    if (from_raw != from_typed)
      std::cerr << "   <-- RAW SAYS " << (from_raw ? num(*from_raw) : "(absent)")
                << ": the wire moved and the parse did not";
    std::cerr << '\n';
  };
  show("typed usd          : ", raw_usd, typed->usd);
  show("typed diem         : ", raw_diem, typed->diem);

  // An engaged usd of 0 is NOT a failure, and this is the one line in the leg
  // that could most easily be written wrong. It has been 0 on every capture,
  // including a call whose rate-card value was $0.0645 — so 0 means "not
  // reported for this account", and a check that treated it as suspect would
  // fire on every run forever.
  return raw_usd != typed->usd || raw_diem != typed->diem;
}

auto usage_report(const venice::Client& client, std::string_view model) -> int {
  ModelPick pick;
  if (model.empty()) {
    auto picked =
        pick_by_capability(client, &venice::ModelCapabilities::supports_reasoning,
                           "supportsReasoning");
    if (!picked) return EXIT_FAILURE;
    pick = *std::move(picked);
  } else {
    pick.chosen = std::string{model};
  }
  report_pick(pick, "supportsReasoning", "`venice-cpp --usage <id>`");

  venice::ChatRequest req;
  req.model = pick.chosen;
  // Long enough to have something to cache: cached_tokens can only be non-zero
  // on a request that actually hit a prompt cache, and #28's original run had
  // no reason to. Re-running this leg back to back is the cache probe.
  req.messages = {venice::Message::user("Think step by step: what is 17 * 23?")};

  bool mismatch = false;

  std::cerr << "\n-- non-streaming --\n";
  const auto nonstream = client.chat(req);
  if (!nonstream) {
    std::cerr << "chat failed [" << venice::to_string(nonstream.error().kind) << "] "
              << nonstream.error().message << '\n';
    return EXIT_FAILURE;
  }
  if (const auto u = nonstream->raw.find("usage");
      u != nonstream->raw.end() && u->is_object())
    mismatch = report_usage(*u, nonstream->usage) || mismatch;
  else
    std::cerr << "usage, verbatim    : (no usage key in the body at all)\n";
  // The whole body, not the usage sub-object. `||` ordering is deliberate: both
  // reports run, so a cost mismatch does not hide a usage one or vice versa.
  report_envelope_keys(nonstream->raw);
  mismatch = report_cost(nonstream->raw, nonstream->cost) || mismatch;

  std::cerr << "\n-- streaming --\n";
  venice::StreamAccumulator acc;
  const auto streamed = client.chat_stream(req, acc, [](const venice::StreamDelta&) { return true; });
  if (!streamed) {
    std::cerr << "stream failed [" << venice::to_string(streamed.error().kind) << "] "
              << streamed.error().message << '\n';
    return EXIT_FAILURE;
  }

  // Off the retained chunks, not off the typed Usage — the accumulator parses
  // the frame and keeps the struct, so the struct is exactly what cannot answer
  // "is this key on the wire". chunks() is where the verbatim record lives, and
  // it needed no library change to reach.
  const nlohmann::json* frame = nullptr;
  std::size_t usage_frames = 0;
  // The cost frame is scanned separately and kept as the whole *chunk*, because
  // report_cost reads the envelope: `cost` is a sibling of `usage`, so handing
  // it the usage sub-object would be handing it the one object that cannot
  // contain the key. Counting them is not decoration either — a second cost
  // frame would mean last-wins is the wrong accumulation rule, and that is the
  // single thing the sweep can measure that changes a line of the library
  // rather than a line of prose.
  //
  // Envelope keys are reported over the UNION of every chunk, not just the
  // cost-bearing one: a new sibling could arrive on an opening or content frame,
  // and a discovery check that only looks where the last discovery happened is
  // how `cost` stayed invisible for three releases.
  const nlohmann::json* cost_chunk = nullptr;
  std::size_t cost_frames = 0;
  std::vector<nlohmann::json> seen_costs;
  nlohmann::json chunk_key_union = nlohmann::json::object();
  for (const auto& c : acc.chunks()) {
    if (!c.is_object()) continue;
    for (const auto& [k, v] : c.items()) chunk_key_union[k] = nullptr;
    if (const auto u = c.find("usage"); u != c.end() && u->is_object()) {
      ++usage_frames;
      frame = &*u;  // last wins, matching StreamAccumulator::ingest
    }
    if (const auto k = c.find("cost"); k != c.end()) {
      ++cost_frames;
      cost_chunk = &c;
      if (std::find(seen_costs.begin(), seen_costs.end(), *k) == seen_costs.end())
        seen_costs.push_back(*k);
    }
  }
  const std::size_t distinct_costs = seen_costs.size();
  report_envelope_keys(chunk_key_union);

  std::cerr << "usage frames       : " << usage_frames << '\n';
  if (frame != nullptr) mismatch = report_usage(*frame, streamed->usage) || mismatch;
  else std::cerr << "usage, verbatim    : (no chunk carried a usage object)\n";

  std::cerr << "cost frames        : " << cost_frames << '\n';
  if (cost_chunk != nullptr) mismatch = report_cost(*cost_chunk, streamed->cost) || mismatch;
  else std::cerr << "cost, verbatim     : (no chunk carried a cost object)\n";

  // The accumulation rule, checked rather than assumed. Every stream swept for
  // VC-20 carried exactly ONE cost frame, so StreamAccumulator's last-wins is
  // uncontradicted but also unproven.
  //
  // The trigger is a second frame, NOT a second *distinct value*, and that is
  // the whole point of the check. Requiring distinctness would pass the shape
  // most likely to mean "these are increments, sum them" — two frames each
  // carrying 0.0005 for a call that charged 0.001 — and last-wins would then
  // report half the true charge with the run exiting 0. Any repeat is news.
  if (cost_frames > 1) {
    std::cerr << "\nMULTIPLE COST FRAMES (" << cost_frames << ", " << distinct_costs
              << " distinct) -- StreamAccumulator takes the last and that may now be wrong.\n"
                 "VC-20 measured exactly one per stream. Equal values may be increments to\n"
                 "sum; differing ones may mean last-wins should be first. Either way the\n"
                 "accumulation rule needs re-opening against this capture.\n";
    mismatch = true;
  }

  if (mismatch)
    std::cerr << "\nRAW AND TYPED DISAGREE -- a from_json is reading the wrong place.\n"
                 "For usage this is case (c) in #28: a parse bug, not a per-family\n"
                 "absence. For cost see #34 -- it is a top-level sibling of usage.\n";
  return mismatch ? EXIT_FAILURE : EXIT_SUCCESS;
}

auto pick_for_responses(const venice::Client& client,
                        std::size_t max_alternates = 4) -> std::optional<ModelPick> {
  const auto models = client.models("text");
  if (!models) {
    std::cerr << "models(text) failed [" << venice::to_string(models.error().kind)
              << "] " << models.error().message << '\n';
    return std::nullopt;
  }
  ModelPick pick;
  for (const auto& model : *models) {
    if (model.id.empty() ||
        (model.capabilities && model.capabilities->supports_e2ee.value_or(false)))
      continue;
    if (pick.chosen.empty())
      pick.chosen = model.id;
    else if (pick.alternates.size() < max_alternates)
      pick.alternates.push_back(model.id);
    else
      break;
  }
  if (pick.chosen.empty()) {
    std::cerr << "no non-E2EE text model carried a usable id\n";
    return std::nullopt;
  }
  return pick;
}

constexpr std::array<std::string_view, 8> kModeledResponsesEnvelopeKeys{
    "id", "object", "created_at", "model", "status", "output", "usage", "error"};
constexpr std::array<std::string_view, 5> kModeledResponsesMessageKeys{
    "type", "id", "status", "role", "content"};
constexpr std::array<std::string_view, 6> kModeledResponsesFunctionKeys{
    "type", "id", "call_id", "name", "arguments", "status"};
constexpr std::array<std::string_view, 4> kModeledResponsesReasoningKeys{
    "type", "id", "summary", "encrypted_content"};
constexpr std::array<std::string_view, 3> kModeledResponsesWebSearchKeys{
    "type", "id", "status"};
constexpr std::array<std::string_view, 5> kModeledResponsesUsageKeys{
    "input_tokens", "input_tokens_details", "output_tokens", "output_tokens_details",
    "total_tokens"};

auto responses_report(const venice::Client& client, std::string_view requested) -> int {
  ModelPick pick;
  if (!requested.empty()) {
    pick.chosen = std::string{requested};
  } else {
    auto selected = pick_for_responses(client);
    if (!selected) return EXIT_FAILURE;
    pick = std::move(*selected);
  }
  report_pick(pick, "type=text and supportsE2EE!=true",
              "`venice-cpp --responses <id>`");

  venice::ResponsesRequest request;
  request.model = pick.chosen;
  request.input = venice::responses_input::text("Reply with exactly: ok");
  request.max_output_tokens = 8;
  request.venice_parameters = venice::VeniceParameters{};
  request.venice_parameters->enable_e2ee = false;

  const auto response = client.create_response(request);
  if (!response) {
    std::cerr << "responses failed [" << venice::to_string(response.error().kind) << "] "
              << response.error().message << "\nbody: " << response.error().body << '\n';
    return EXIT_FAILURE;
  }

  std::cerr << "status: " << response->status << "\noutput items: "
            << response->output.size() << "\noutput text: " << response->output_text() << '\n';
  report_unmodeled("unmodeled response envelope keys: ", response->raw,
                   kModeledResponsesEnvelopeKeys, "ResponsesResponse::raw");
  if (const auto* usage = venice::detail::opt_object(response->raw, "usage"))
    report_unmodeled("unmodeled responses usage keys: ", *usage,
                     kModeledResponsesUsageKeys, "ResponsesUsage");

  for (const auto& item : response->output) {
    const auto type = venice::detail::opt_string(item, "type");
    if (type == "message")
      report_unmodeled("unmodeled message output keys: ", item,
                       kModeledResponsesMessageKeys, "ResponsesResponse::output");
    else if (type == "function_call")
      report_unmodeled("unmodeled function output keys: ", item,
                       kModeledResponsesFunctionKeys, "ResponsesResponse::output");
    else if (type == "reasoning")
      report_unmodeled("unmodeled reasoning output keys: ", item,
                       kModeledResponsesReasoningKeys, "ResponsesResponse::output");
    else if (type == "web_search_call")
      report_unmodeled("unmodeled web-search output keys: ", item,
                       kModeledResponsesWebSearchKeys, "ResponsesResponse::output");
    else
      std::cerr << "unknown raw output item type: " << type.value_or("unknown")
                << "   (payload retained in ResponsesResponse::output)\n";
  }
  return EXIT_SUCCESS;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const std::string_view leg = argc > 1 ? std::string_view{argv[1]} : std::string_view{};
  const std::string_view arg = argc > 2 ? std::string_view{argv[2]} : std::string_view{};
  const std::string_view arg2 = argc > 3 ? std::string_view{argv[3]} : std::string_view{};
  const std::string_view arg3 = argc > 4 ? std::string_view{argv[4]} : std::string_view{};

  // The five legs that need no credential, dispatched above the key lookup
  // deliberately. Measured 2026-08-11: /models/traits and
  // /models/compatibility_mapping (VC-38, #59) and /models itself (VC-39, #60)
  // all answer 200 with no Authorization header at all, for every modality.
  // Leaving them below the guard would have made them unrunnable in exactly the
  // configuration they exist to prove, and would have left the guard's message
  // — "nothing to call" — saying something about this library that is no longer
  // true.
  //
  // They use public_access() even when a key IS set, so the leg tests the public
  // path rather than whatever the environment happens to hold.
  if (leg == "--traits")
    return show_model_traits(venice::Client{venice::Authentication::public_access()}, arg);
  if (leg == "--compat")
    return show_compatibility_mapping(venice::Client{venice::Authentication::public_access()},
                                      arg);
  if (leg == "--modality")
    return show_modality(venice::Client{venice::Authentication::public_access()}, arg);
  if (leg == "--styles")
    return image_styles_report(venice::Client{venice::Authentication::public_access()});
  if (leg == "--x402")
    return x402_report(venice::Client{venice::Authentication::public_access()});

  const char* key = std::getenv("VENICE_API_KEY");
  if (key == nullptr || *key == '\0') {
    std::cerr
        << "VENICE_API_KEY not set; nothing to call with a credential.\n"
           "(--traits, --compat, --modality, --styles and --x402 need no key;\n"
           " --embeddings, --image, --image-transform, --audio, --video, "
           "--augment, --crypto-rpc,\n"
           " --billing, --responses, --structured and\n"
           " --api-keys need a key.)\n";
    return EXIT_SUCCESS;
  }

  const venice::Client client{key};
  if (leg == "--models") return list_models(client, arg);
  if (leg == "--characters") return list_characters(client, arg);
  if (leg == "--character") return show_character(client, arg);
  // argc, not arg.empty(): every other leg treats "" as "no argument", but this
  // one has a default prompt, so conflating the two would make `--stream ""`
  // silently send 17*23 instead of the empty prompt it used to send. That is a
  // real case to be able to smoke — the server's 400 for it — and the
  // pre-VC-38 dispatch could reach it.
  if (leg == "--stream")
    return stream_report(client, argc > 2 ? std::string{arg}
                                          : std::string{"Think step by step: what is 17 * 23?"});
  if (leg == "--tools") return tools_report(client, arg);
  if (leg == "--structured")
    return structured_report(client, arg);
  if (leg == "--usage") return usage_report(client, arg);
  if (leg == "--embeddings") return embeddings_report(client, arg);
  if (leg == "--image") return image_report(client, arg);
  if (leg == "--image-transform") return image_transform_report(client, arg);
  if (leg == "--audio") return audio_report(client, arg, arg2, arg3);
  if (leg == "--video") return video_report(client, arg);
  if (leg == "--augment")
    return augment_report(client);
  if (leg == "--crypto-rpc") return crypto_rpc_report(client);
  if (leg == "--billing") return billing_report(client, arg);
  if (leg == "--api-keys") return api_keys_report(client);
  if (leg == "--responses") return responses_report(client, arg);

  const std::string prompt = argc > 1 ? argv[1] : "Say hello in one short sentence.";

  venice::ChatRequest req;
  req.model = "llama-3.3-70b";
  req.messages = {venice::Message::user(prompt)};

  auto res = client.chat(req);
  if (!res) {
    std::cerr << "chat failed [" << venice::to_string(res.error().kind) << "] "
              << res.error().message << '\n';
    return EXIT_FAILURE;
  }

  std::cout << res->content << '\n';
  if (res->usage) {
    std::cerr << "(" << res->usage->prompt_tokens << " prompt / "
              << res->usage->completion_tokens << " completion tokens)\n";
  }
  // What it actually cost, when the server says. Printed here and not only in
  // --usage so the field is visible without running the check leg. usd has been
  // 0 on every capture, so print whichever currency is engaged and let the
  // reader see which one answered.
  if (res->cost) {
    if (res->cost->usd || res->cost->diem) {
      std::cerr << "(cost";
      if (res->cost->usd) std::cerr << " usd " << *res->cost->usd;
      if (res->cost->diem) std::cerr << " diem " << *res->cost->diem;
      std::cerr << ")\n";
    } else {
      // A cost object with neither currency readable. Saying so is the point of
      // the tolerant parse — silence here would be the state it exists to avoid.
      std::cerr << "(cost object arrived but neither currency parsed)\n";
    }
  }
  return EXIT_SUCCESS;
}
