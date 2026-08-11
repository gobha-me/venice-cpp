// Model listing parse — offline, no API key or network.
//
// Charter: everything venice::models_from_json_body and Model's from_json do
// with a /models body. The transport half (Client::models) is not exercised
// here and cannot be — that is the point of the parse being a free function.
// Request-body serialization lives in test/02request/, precondition guards in
// test/03guards/; nothing here touches either.
//
// The two large fixtures below are captured verbatim from the live API on
// 2026-07-28 (`curl https://api.venice.ai/api/v1/models` and `?type=all`),
// compact form and key order as received. They are not hand-written, because
// the things most likely to be wrong in this parse are the things a
// hand-written fixture would get wrong in the same direction as the code —
// notably that Venice sends whole prices as JSON integers.
//
// One rule this file must not break, the same one test/02request/ states:
// never hand a std::optional to nlohmann. The pinned fallback is v3.11.3 and
// optional support landed in 3.12.0, so `j.get<std::optional<int>>()` fails to
// compile on the pin while passing on a newer system copy. Always dereference.
//
// Failure matrix first, happy path last.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include <venice/venice.hpp>

using venice::Model;
using venice::models_from_json_body;

namespace {

// A text model with every interesting shape at once: extended pricing past a
// threshold, a cache_input bucket but no cache_write, reasoning-effort
// options, maxImages, and all fourteen capability booleans.
constexpr auto kGrok = R"({"context_length":1000000,"created":1776470400,"id":"grok-4-3","model_spec":{"pricing":{"input":{"usd":1.42,"diem":1.42},"cache_input":{"usd":0.23,"diem":0.23},"output":{"usd":2.83,"diem":2.83},"extended":{"context_token_threshold":200000,"input":{"usd":2.83,"diem":2.83},"output":{"usd":5.67,"diem":5.67},"cache_input":{"usd":0.45,"diem":0.45}}},"availableContextTokens":1000000,"maxCompletionTokens":32000,"capabilities":{"optimizedForCode":false,"quantization":"not-available","supportsAudioInput":false,"supportsFunctionCalling":true,"supportsLogProbs":false,"supportsMultipleImages":true,"maxImages":10,"supportsReasoning":true,"supportsReasoningEffort":true,"reasoningEffortOptions":["none","low","medium","high"],"defaultReasoningEffort":"low","supportsResponseSchema":true,"supportsTeeAttestation":false,"supportsE2EE":false,"supportsVideoInput":false,"supportsVision":true,"supportsWebSearch":true,"supportsXSearch":true},"description":"Grok 4.3 is xAI's most intelligent and fastest reasoning model with function calling, structured outputs, and a 1M-token context window. Suited for agentic workflows, instruction-following tasks, and applications requiring high factual accuracy.","name":"Grok 4.3","modelSource":"https://docs.x.ai/developers/models","offline":false,"privacy":"private","traits":[]},"object":"model","owned_by":"venice.ai","type":"text"})";

// An image model. Its model_spec shares almost nothing with the text shape —
// pricing is generation/upscale, there are no capabilities and no context
// window — which is what makes it the check that this schema does not have to
// change when models() eventually grows a ?type= parameter.
constexpr auto kImage = R"({"created":1743099022,"id":"venice-sd35","model_spec":{"pricing":{"generation":{"usd":0.01,"diem":0.01},"upscale":{"2x":{"usd":0.02,"diem":0.02},"4x":{"usd":0.08,"diem":0.08}}},"constraints":{"promptCharacterLimit":1500,"steps":{"default":25,"max":30},"widthHeightDivisor":16},"supportsWebSearch":false,"supportsOptimizePromptThinking":false,"supportsStyleReferences":false,"name":"Venice SD35","modelSource":"https://huggingface.co/stabilityai/stable-diffusion-3.5-large","offline":false,"privacy":"private","traits":["eliza-default"]},"object":"model","owned_by":"venice.ai","type":"image"})";

// The smallest thing that is still a model.
constexpr auto kBare = R"({"id":"m"})";

auto list_of(const std::string& entries) -> nlohmann::json {
  return nlohmann::json::parse(R"({"data":[)" + entries + "]}");
}

auto one(const std::string& entry) -> Model {
  auto v = models_from_json_body(list_of(entry));
  REQUIRE(v.size() == 1);
  return v.front();
}

// A text entry whose model_spec keys can be overridden per case.
auto spec_entry(const std::string& spec_body) -> std::string {
  return R"({"id":"m","type":"text","model_spec":{)" + spec_body + "}}";
}

}  // namespace

// ── §0 the container: the only place the whole list may fail ──────────────
//
// Everything below this section degrades. These five cases fix the boundary,
// and the second is the one that matters: iterating a json *object* yields its
// values and iterating a scalar yields the scalar, so without an is_array()
// check a `{"data":{...}}` body silently becomes a vector of models built out
// of whatever the values happened to be — reporting success the whole way.

TEST_CASE("a response that is not a list throws", "[models][failure]") {
  SECTION("data is a number") {
    REQUIRE_THROWS(models_from_json_body(nlohmann::json::parse(R"({"data":42})")));
  }
  SECTION("data is an object — the garbage-model-factory case") {
    REQUIRE_THROWS(models_from_json_body(
        nlohmann::json::parse(R"({"data":{"a":{"id":"x"},"b":{"id":"y"}}})")));
  }
  SECTION("body is a bare scalar") {
    REQUIRE_THROWS(models_from_json_body(nlohmann::json::parse(R"("nope")")));
  }
}

TEST_CASE("an empty list is a list, not a failure", "[models]") {
  SECTION("wrapped in data") {
    REQUIRE(models_from_json_body(nlohmann::json::parse(R"({"data":[]})")).empty());
  }
  SECTION("bare array body — the no-data branch") {
    REQUIRE(models_from_json_body(nlohmann::json::parse("[]")).empty());
  }
}

// ── §1 entries degrade, never throw ───────────────────────────────────────
//
// The next two cases are a pair held in tension and neither substitutes for
// the other. The first proves junk is skipped rather than fatal; the second
// proves a *partial* entry is kept rather than skipped. Delete the second and
// "skip what cannot be used" quietly degenerates into "drop anything
// unfamiliar", which is the failure this ticket is written against.

TEST_CASE("unusable entries are skipped, not fatal", "[models][failure]") {
  const auto v = models_from_json_body(nlohmann::json::parse(
      R"({"data":[{"id":"a","type":"text"},42,null,"x",[],)"
      R"({"type":"text"},{"id":123},{"id":""},{"id":"b"}]})"));

  REQUIRE(v.size() == 2);
  REQUIRE(v[0].id == "a");
  REQUIRE(v[1].id == "b");
}

TEST_CASE("a partial entry is kept with everything else absent", "[models]") {
  const auto m = one(kBare);

  REQUIRE(m.id == "m");
  REQUIRE(m.type.empty());
  REQUIRE_FALSE(m.name.has_value());
  REQUIRE_FALSE(m.created.has_value());
  REQUIRE_FALSE(m.context_length.has_value());
  REQUIRE_FALSE(m.available_context_tokens.has_value());
  REQUIRE_FALSE(m.max_completion_tokens.has_value());
  REQUIRE_FALSE(m.offline.has_value());
  REQUIRE_FALSE(m.capabilities.has_value());
  REQUIRE_FALSE(m.pricing.has_value());
  REQUIRE(m.traits.empty());
}

// ── §2 wrong types degrade field-by-field ─────────────────────────────────
//
// The numeric cases here are the reason the helpers use type predicates rather
// than try/catch: measured against the pinned nlohmann, `get<int>()` returns 1
// for 1.9 and 276447231 for 99999999999999, throwing in neither case. A guard
// that only catches exceptions would turn each into a confident wrong number,
// and the second one does not even trip UBSan — the narrowing is well-defined.

TEST_CASE("a wrong-typed number is absent, never a truncated one", "[models][failure]") {
  SECTION("a string where a count belongs") {
    REQUIRE_FALSE(one(R"({"id":"m","context_length":"131072"})").context_length.has_value());
  }
  SECTION("a float where an integer belongs is not truncated to 1") {
    REQUIRE_FALSE(one(R"({"id":"m","context_length":1.9})").context_length.has_value());
  }
  SECTION("past INT_MAX is unparseable, not narrowed") {
    const auto m = one(R"({"id":"m","context_length":99999999999999})");
    REQUIRE_FALSE(m.context_length.has_value());
    // and the same number is fine where the field is wide enough
    REQUIRE(one(R"({"id":"m","created":99999999999999})").created == std::int64_t{99999999999999});
  }
  SECTION("explicit null") {
    REQUIRE_FALSE(one(R"({"id":"m","context_length":null})").context_length.has_value());
  }
}

TEST_CASE("one bad field does not cost the other fields", "[models][failure]") {
  SECTION("a non-boolean capability leaves its thirteen neighbours engaged") {
    const auto m = one(spec_entry(
        R"("capabilities":{"supportsVision":"yes","supportsFunctionCalling":true,)"
        R"("supportsMultipleImages":true,"supportsVideoInput":true,"supportsAudioInput":true,)"
        R"("supportsReasoning":true,"supportsReasoningEffort":true,"supportsResponseSchema":true,)"
        R"("supportsLogProbs":true,"supportsWebSearch":true,"supportsXSearch":true,)"
        R"("supportsTeeAttestation":true,"supportsE2EE":true,"optimizedForCode":true})"));

    REQUIRE(m.capabilities.has_value());
    REQUIRE_FALSE(m.capabilities->supports_vision.has_value());
    for (const auto& [field, key] : venice::detail::kCapabilityBoolFields) {
      if (field == &venice::ModelCapabilities::supports_vision) continue;
      INFO("capability dropped alongside the bad one: " << key);
      REQUIRE((m.capabilities.value().*field).has_value());
    }
  }
  SECTION("a non-numeric usd leaves diem engaged") {
    const auto m = one(spec_entry(R"("pricing":{"input":{"usd":"1.5","diem":0.2}})"));
    REQUIRE(m.pricing.has_value());
    REQUIRE(m.pricing->base.input.has_value());
    REQUIRE_FALSE(m.pricing->base.input->usd.has_value());
    REQUIRE(m.pricing->base.input->diem == 0.2);
  }
  SECTION("a malformed model_spec leaves the top-level fields parsed") {
    const auto m = one(R"({"id":"m","type":"text","context_length":8192,"model_spec":42})");
    REQUIRE(m.id == "m");
    REQUIRE(m.type == "text");
    REQUIRE(m.context_length == 8192);
    REQUIRE_FALSE(m.capabilities.has_value());
    REQUIRE_FALSE(m.name.has_value());
  }
  SECTION("wrong-typed sub-objects are absent, not throwing") {
    const auto m = one(spec_entry(R"("capabilities":[],"pricing":null)"));
    REQUIRE_FALSE(m.capabilities.has_value());
    REQUIRE_FALSE(m.pricing.has_value());
  }
}

TEST_CASE("traits keeps the strings and discards the rest", "[models][failure]") {
  SECTION("a string where an array belongs yields no traits") {
    REQUIRE(one(spec_entry(R"("traits":"default")")).traits.empty());
  }
  SECTION("non-string elements are dropped, neighbours kept") {
    const auto m = one(spec_entry(R"("traits":["a",7,null,"b"])"));
    REQUIRE(m.traits == std::vector<std::string>{"a", "b"});
  }
}

// ── §3 prices are numbers, whichever kind Venice felt like sending ────────
//
// Venice quotes a whole price as a JSON integer and a fractional one as a
// float, in the same payload. Narrowing the price reader to is_number_float()
// drops every integral price — twenty of the output prices in the captured
// listing — and nothing else in the parse notices.

TEST_CASE("integral and fractional prices both parse", "[models][pricing]") {
  const auto m = one(spec_entry(
      R"("pricing":{"input":{"usd":1.875},"output":{"usd":2},"cache_input":{"usd":0}})"));

  REQUIRE(m.pricing.has_value());
  REQUIRE(m.pricing->base.input->usd == 1.875);
  REQUIRE(m.pricing->base.output->usd == 2.0);  // JSON integer, not a float
  // Zero is a price, not an absence: the free tier is a real answer and the
  // engaged-vs-truthy confusion is what would erase it.
  REQUIRE(m.pricing->base.cache_input->usd.has_value());
  REQUIRE(*m.pricing->base.cache_input->usd == 0.0);
}

// ── §4 cache buckets stay distinct (AGENTS.md, venice-cli #75) ────────────

TEST_CASE("cache buckets do not collapse into each other", "[models][pricing]") {
  const auto m = one(spec_entry(
      R"("pricing":{"input":{"usd":1.42},"output":{"usd":2.83},"cache_input":{"usd":0.23}})"));

  REQUIRE(m.pricing->base.input->usd == 1.42);
  REQUIRE(m.pricing->base.cache_input->usd == 0.23);
  REQUIRE_FALSE(m.pricing->base.cache_write.has_value());  // absent, not defaulted to input
}

TEST_CASE("the extended tier is parsed as a second, whole rate card", "[models][pricing]") {
  const auto m = one(kGrok);
  const auto& p = *m.pricing;

  REQUIRE(p.extended_threshold_tokens == std::int64_t{200000});
  REQUIRE(p.extended.has_value());
  REQUIRE(p.base.input->usd == 1.42);
  REQUIRE(p.extended->input->usd == 2.83);   // twice the base rate
  REQUIRE(p.extended->output->usd == 5.67);
  REQUIRE(p.extended->cache_input->usd == 0.45);
  REQUIRE_FALSE(p.extended->cache_write.has_value());

  // base and extended are the same type, which is the whole reason to model
  // the tier rather than leave it in raw: selecting one is an expression, not
  // a second code path.
  const venice::PriceTier& t = p.extended && p.extended_threshold_tokens &&
                                       500000 > *p.extended_threshold_tokens
                                   ? *p.extended
                                   : p.base;
  REQUIRE(t.input->usd == 2.83);
}

// ── §5 the capability table names keys the API actually sends ─────────────
//
// A typo'd wire key yields nullopt forever and is invisible to a test that
// hand-copies the same typo, so the captured payload is the oracle rather than
// a second list written from the same misreading.

TEST_CASE("every capability key is one the live payload carries", "[models][capabilities]") {
  const auto entry = nlohmann::json::parse(kGrok);
  const auto& caps = entry.at("model_spec").at("capabilities");
  const auto m = one(kGrok);

  REQUIRE(m.capabilities.has_value());
  for (const auto& [field, key] : venice::detail::kCapabilityBoolFields) {
    INFO("capability key not present in the captured payload: " << key);
    REQUIRE(caps.contains(key));
    REQUIRE((m.capabilities.value().*field).has_value());
  }
}

// ── §6 the raw hatch is a superset and survives degradation ───────────────

TEST_CASE("raw round-trips the verbatim entry", "[models][raw]") {
  const auto entry = nlohmann::json::parse(kGrok);
  const auto m = one(kGrok);

  // Byte-exact, the models analogue of test/02request/'s baseline body. A
  // subtractive hatch would fail this, and that is the point: modeled fields
  // stay readable through raw, so graduating a field to typed never silently
  // breaks a caller reading it the old way.
  REQUIRE(m.raw == entry);
  REQUIRE(m.raw.dump() == entry.dump());
  REQUIRE(m.raw["model_spec"]["capabilities"]["supportsVision"] == true);
}

TEST_CASE("unmodeled keys stay reachable through raw", "[models][raw]") {
  const auto m = one(kGrok);
  REQUIRE(m.raw["object"] == "model");
  REQUIRE(m.raw["model_spec"]["capabilities"]["quantization"] == "not-available");
}

TEST_CASE("raw survives on a degraded entry too", "[models][raw]") {
  const auto m = one(R"({"id":"m","model_spec":42})");
  REQUIRE(m.raw["model_spec"] == 42);
}

// ── §7 happy path, last ───────────────────────────────────────────────────

TEST_CASE("a captured text entry parses field for field", "[models]") {
  const auto m = one(kGrok);

  REQUIRE(m.id == "grok-4-3");
  REQUIRE(m.type == "text");
  REQUIRE(m.name == "Grok 4.3");
  REQUIRE(m.owned_by == "venice.ai");
  REQUIRE(m.privacy == "private");
  REQUIRE(m.model_source == "https://docs.x.ai/developers/models");
  REQUIRE(m.created == std::int64_t{1776470400});
  REQUIRE(m.context_length == 1000000);
  REQUIRE(m.available_context_tokens == 1000000);
  REQUIRE(m.max_completion_tokens == 32000);
  REQUIRE(m.offline == false);
  REQUIRE(m.description.has_value());
  REQUIRE(m.traits.empty());

  const auto& c = *m.capabilities;
  REQUIRE(c.supports_function_calling == true);
  REQUIRE(c.supports_vision == true);
  REQUIRE(c.supports_x_search == true);
  REQUIRE(c.supports_e2ee == false);
  REQUIRE(c.optimized_for_code == false);
  REQUIRE(c.quantization == "not-available");
  REQUIRE(c.max_images == 10);
  REQUIRE(c.default_reasoning_effort == "low");
  REQUIRE(c.reasoning_effort_options ==
          std::vector<std::string>{"none", "low", "medium", "high"});
}

// A non-text model, last of all: the text-shaped half of this schema still
// holds one without changing, which is what keeps a ?type= parameter (VC-13)
// from being a breaking change to Model.
//
// Since VC-39 its constraints no longer degrade — see test/10modalities/,
// which owns the per-modality surface. What this case is worth keeping for is
// the cross-check: this capture is months older than the ones that file was
// built from, and the tables read it the same way. A key Venice renamed
// between the two captures would show up here and nowhere else.
TEST_CASE("a captured image entry degrades only where it should", "[models]") {
  const auto m = one(kImage);

  REQUIRE(m.id == "venice-sd35");
  REQUIRE(m.type == "image");
  REQUIRE(m.name == "Venice SD35");
  REQUIRE(m.traits == std::vector<std::string>{"eliza-default"});

  // No context window and no capability block on an image model. Still true,
  // and still not synthesised: the three supportsX flags this entry carries
  // live on the image view, not in a manufactured ModelCapabilities.
  REQUIRE_FALSE(m.context_length.has_value());
  REQUIRE_FALSE(m.available_context_tokens.has_value());
  REQUIRE_FALSE(m.capabilities.has_value());

  // Its pricing is generation/upscale, so every text bucket is absent — but
  // the object is still there and the real buckets are reachable through raw.
  REQUIRE(m.pricing.has_value());
  REQUIRE_FALSE(m.pricing->base.input.has_value());
  REQUIRE_FALSE(m.pricing->base.output.has_value());
  REQUIRE_FALSE(m.pricing->extended.has_value());
  REQUIRE(m.raw["model_spec"]["pricing"]["generation"]["usd"] == 0.01);
  REQUIRE(m.raw["model_spec"]["pricing"]["upscale"]["2x"]["usd"] == 0.02);

  // The half that stopped degrading. An older capture, read by the same
  // tables.
  REQUIRE(m.image.has_value());
  REQUIRE(m.image->constraints->prompt_character_limit == 1500);
  REQUIRE(m.image->constraints->steps->default_value == 25);
  REQUIRE(m.image->constraints->steps->max == 30);
  REQUIRE(m.image->constraints->width_height_divisor == 16);
  REQUIRE(m.image->supports_style_references == false);
  // This entry states no ratios at all, which is not the same as stating none.
  REQUIRE_FALSE(m.image->constraints->aspect_ratios.has_value());
  // And raw is still the whole entry, modeled keys included.
  REQUIRE(m.raw["model_spec"]["constraints"]["widthHeightDivisor"] == 16);
}

TEST_CASE("a text entry engages no per-modality view", "[models]") {
  // The complement of the case above, and the reason the dispatch keys on
  // `type` at all: every existing caller of this struct is a text caller, and
  // none of them should acquire an engaged image or video view.
  const auto m = one(kGrok);
  REQUIRE(m.type == "text");
  REQUIRE_FALSE(m.image.has_value());
  REQUIRE_FALSE(m.inpaint.has_value());
  REQUIRE_FALSE(m.video.has_value());
  REQUIRE_FALSE(m.tts.has_value());
  REQUIRE_FALSE(m.embedding.has_value());
  // This capture carries no constraints block; 5 of the 106 live text models
  // do, and test/10modalities/ holds one of them.
  REQUIRE_FALSE(m.text_constraints.has_value());
  REQUIRE_FALSE(m.deprecation.has_value());
  REQUIRE(m.model_sets.empty());
}
