// /models/traits and /models/compatibility_mapping parse — offline, no API key
// or network.
//
// Charter: everything venice::model_traits_from_json_body,
// venice::model_compatibility_mapping_from_json_body and the shared
// venice::detail::string_map_envelope_from_json_body behind them do with the
// three-key envelope both operations speak. The transport half
// (Client::model_traits, Client::model_compatibility_mapping) is not exercised
// here and cannot be — that is the point of both halves being free functions.
// Auth policy, wire targets and status classification are test/06transport/; the
// query encoder those paths are built with is test/05query/.
//
// **The fixtures at the bottom of this file ARE captures**, and unlike
// test/08characters/ this file does not have to apologise for them. Both of
// these operations answer 200 with no Authorization header at all — measured
// 2026-08-11 against api.venice.ai, from an environment with no VENICE_API_KEY —
// so the happy-path bodies below are verbatim wire, pasted, not assembled from
// the OpenAPI document. Where a case IS derived rather than captured (the
// failure matrix, which no server will produce on request) it says so.
//
// That distinction earns its keep immediately: the document says these two
// operations take the same `type` values and the wire says they do not. See §7.
//
// One rule this file must not break, the same one test/04models/ states: never
// hand a std::optional to nlohmann. The pinned fallback is v3.11.3 and optional
// support landed in 3.12.0, so `j.get<std::optional<int>>()` fails to compile on
// the pin while passing on a newer system copy. Always dereference.
//
// Failure matrix first, happy path last.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include <venice/venice.hpp>

using venice::model_compatibility_mapping_from_json_body;
using venice::model_traits_from_json_body;

namespace {

auto body(const std::string& text) -> nlohmann::json {
  return nlohmann::json::parse(text);
}

// Every case in the failure matrix is asserted against BOTH parsers. They share
// one implementation today; a future refactor that splits them must keep both
// halves honest, and a matrix that only ever ran against traits would not notice.
void both_throw(const std::string& text) {
  REQUIRE_THROWS_AS(model_traits_from_json_body(body(text)), std::exception);
  REQUIRE_THROWS_AS(model_compatibility_mapping_from_json_body(body(text)), std::exception);
}

}  // namespace

// ── §1 the body itself is not an envelope ─────────────────────────────────

TEST_CASE("a non-object body has no data to find", "[catalogue][failure]") {
  // Derived, not captured. One case per JSON shape rather than a representative
  // one: opt_object's guard is a single is_object() test today, and enumerating
  // the alternatives is what would survive someone replacing it with a chain.
  both_throw("[]");
  both_throw(R"(["default","x"])");
  both_throw(R"("default")");
  both_throw("3");
  both_throw("null");
  both_throw("true");
}

// ── §2 the case that must never be allowed to pass ────────────────────────

TEST_CASE("an envelope with no data is not a two-entry map", "[catalogue][failure]") {
  // THIS IS THE HEADLINE CASE. Read the comment before deleting it.
  //
  // Every list parser in types.hpp opens with opt_array(j,"data") and falls back
  // to the whole body when `data` is absent. Copying that idiom here is the
  // obvious refactor, and it is wrong: there the fallback demands an ARRAY while
  // the envelope is an OBJECT, so it is type-disjoint and can only fire on a body
  // that really is a bare list. Here both levels are objects, so the fallback
  // would be type-indistinguishable — the body below would parse into a
  // two-entry map {object -> "list", type -> "text"} and REPORT SUCCESS.
  //
  // If someone reintroduces the fallback, this is the only case in the suite
  // that goes red. Nothing else in this file can see it: the map would be
  // populated, `returned` would be 2, `raw` would be intact, and every other
  // assertion here would still pass.
  both_throw(R"({"object":"list","type":"text"})");

  // The same hazard from the other direction: a body carrying nothing but the
  // sibling keys a future API version might add.
  both_throw(R"({"object":"list","type":"text","next":"cursor-2"})");
}

TEST_CASE("data must be an object, not merely present", "[catalogue][failure]") {
  both_throw(R"({"data":[]})");
  both_throw(R"({"data":["default"]})");
  both_throw(R"({"data":"default"})");
  both_throw(R"({"data":null})");
  both_throw(R"({"data":3})");
  both_throw(R"({"data":true})");
}

TEST_CASE("the two parsers name themselves when they throw", "[catalogue][failure]") {
  // The message Client wraps into ErrorKind::Parse has to say which of the two
  // calls failed; they are otherwise indistinguishable to a caller reading a log.
  try {
    static_cast<void>(model_traits_from_json_body(body(R"({"object":"list"})")));
    FAIL("expected a throw");
  } catch (const std::exception& e) {
    REQUIRE(std::string{e.what()}.starts_with("model traits:"));
  }

  try {
    static_cast<void>(model_compatibility_mapping_from_json_body(body(R"({"object":"list"})")));
    FAIL("expected a throw");
  } catch (const std::exception& e) {
    REQUIRE(std::string{e.what()}.starts_with("model compatibility mapping:"));
  }
}

// ── §3 an empty catalogue is an answer, not a failure ─────────────────────

TEST_CASE("empty data is a success", "[catalogue]") {
  // Deliberately NOT in the failure matrix, and this is measured rather than
  // assumed: on 2026-08-11 traits?type=tts, ?type=video and ?type=embedding, and
  // compatibility_mapping?type=image, all returned 200 with "data":{} rather
  // than a 404. A modality with no traits is a modality with no traits.
  //
  // This is the jessica-2 lesson from VC-36 (#56) arriving a second time: the
  // first live run there found that a character with no reviews is an empty page,
  // not an error, and a leg that treated emptiness as failure would have been
  // wrong about a normal state of the world.
  const auto res = model_traits_from_json_body(body(R"({"data":{},"object":"list","type":"tts"})"));
  REQUIRE(res.entries.empty());
  REQUIRE(res.returned == 0);
  REQUIRE(res.find("default") == nullptr);
  REQUIRE(res.object.has_value());
  REQUIRE(*res.type == "tts");
  REQUIRE(res.raw.contains("data"));
}

// ── §4 per-entry degradation ──────────────────────────────────────────────

TEST_CASE("a non-string value is skipped, not fatal, and still counted",
          "[catalogue][failure]") {
  // Derived. A trait whose value arrives as null must not cost the caller the
  // other four — the listing rule. What makes the skip safe rather than lossy is
  // that it is detectable two ways, and both are asserted here.
  const auto res = model_traits_from_json_body(
      body(R"({"data":{"a":"m1","b":null,"c":3,"d":{},"e":[],"f":"m2"}})"));

  REQUIRE(res.entries.size() == 2);
  REQUIRE(*res.find("a") == "m1");
  REQUIRE(*res.find("f") == "m2");
  REQUIRE(res.find("b") == nullptr);
  REQUIRE(res.find("c") == nullptr);

  // Detection one: the count the server sent, before any skipping. A caller that
  // compares these two knows something arrived unusable without diffing raw.
  REQUIRE(res.returned == 6);
  REQUIRE(res.returned != res.entries.size());

  // Detection two: the skipped values are still reachable verbatim. Nothing is
  // destroyed by the skip — it is withheld from the typed view only.
  REQUIRE(res.raw["data"]["b"].is_null());
  REQUIRE(res.raw["data"]["c"] == 3);
  REQUIRE(res.raw["data"]["e"].is_array());
}

TEST_CASE("an empty key is kept", "[catalogue]") {
  // Pinned so it reads as a decision rather than an oversight. models_from_json_body
  // drops an entry with an empty `id` because the id is the handle you hand to
  // chat() and an entry that cannot fill it has no future but a 400 far from here.
  // Nothing is ever handed one of these keys, so an empty one is inert: it costs a
  // map slot and answers find("") truthfully.
  const auto res = model_traits_from_json_body(body(R"({"data":{"":"m1","default":"m2"}})"));
  REQUIRE(res.entries.size() == 2);
  REQUIRE(*res.find("") == "m1");
}

TEST_CASE("an empty value is kept", "[catalogue]") {
  // The mirror case. An empty string is a string; withholding it would be this
  // parser inventing a validity rule the server did not state.
  const auto res = model_traits_from_json_body(body(R"({"data":{"default":""}})"));
  REQUIRE(res.entries.size() == 1);
  REQUIRE(res.find("default") != nullptr);
  REQUIRE(res.find("default")->empty());
}

// ── §5 keys survive verbatim ──────────────────────────────────────────────

TEST_CASE("no key is normalized", "[catalogue]") {
  // Measured 2026-08-11: the spelling is not uniform. Traits are mostly
  // snake_case but `eliza-default` is hyphenated, and every compatibility key is
  // a foreign vendor's id, complete with dots and dates. Anyone tempted to
  // normalize case or separators to "tidy up" the map breaks lookup for every
  // caller; this case is the tripwire.
  const auto res = model_compatibility_mapping_from_json_body(body(
      R"({"data":{"eliza-default":"venice-sd35","gpt-4.1":"q3","o1-mini":"l3",)"
      R"("claude-3-5-sonnet-20241022":"l3","GPT-4o":"l3","gpt-4o":"l3"}})"));

  REQUIRE(res.entries.size() == 6);
  REQUIRE(*res.find("eliza-default") == "venice-sd35");
  REQUIRE(*res.find("gpt-4.1") == "q3");
  REQUIRE(*res.find("claude-3-5-sonnet-20241022") == "l3");
  // Case is significant: these are two distinct keys, not one seen twice.
  REQUIRE(res.find("GPT-4o") != nullptr);
  REQUIRE(res.find("gpt-4o") != nullptr);
  REQUIRE(res.returned == 6);

  // Lookup is heterogeneous — a string_view key allocates nothing. Asserted
  // because std::less<> is easy to drop in a refactor and nothing else notices.
  const std::string_view key{"gpt-4.1"};
  REQUIRE(res.find(key) != nullptr);
}

// ── §6 the envelope's own fields ──────────────────────────────────────────

TEST_CASE("object and type read strictly", "[catalogue][failure]") {
  // Wrong-typed: absent, not coerced. This is why they are optional rather than
  // plain strings — "the server did not say" and "the server sent a number" are
  // different facts, and a caller comparing type against what it requested must
  // not be told they are the same.
  const auto wrong =
      model_traits_from_json_body(body(R"({"data":{"a":"m"},"object":3,"type":[]})"));
  REQUIRE_FALSE(wrong.object.has_value());
  REQUIRE_FALSE(wrong.type.has_value());
  REQUIRE(wrong.entries.size() == 1);  // and the map still parsed

  const auto absent = model_traits_from_json_body(body(R"({"data":{"a":"m"}})"));
  REQUIRE_FALSE(absent.object.has_value());
  REQUIRE_FALSE(absent.type.has_value());
}

TEST_CASE("raw is the whole envelope and a superset", "[catalogue]") {
  const auto res = model_traits_from_json_body(
      body(R"({"data":{"a":"m"},"object":"list","type":"text","next":"cursor-2"})"));

  // Modeled fields are still in there — the subtractive-hatch trap Model::raw's
  // note describes: a reader of raw["object"] must not start getting null the
  // release `object` became typed.
  REQUIRE(res.raw["object"] == "list");
  REQUIRE(res.raw["type"] == "text");
  REQUIRE(res.raw["data"]["a"] == "m");
  // And an unmodeled sibling is ignored by the parse but reachable.
  REQUIRE(res.raw["next"] == "cursor-2");
  REQUIRE(res.find("next") == nullptr);
}

// ── §7 the type filter, and what the document gets wrong ──────────────────

TEST_CASE("the type echo is the filter the server actually applied", "[catalogue]") {
  // Captured 2026-08-11: an unfiltered /models/traits returns "type":"text". The
  // server has a default and it is not "everything", so a caller that sends no
  // filter is told which catalogue it received rather than left to assume.
  const auto unfiltered = model_traits_from_json_body(
      body(R"({"data":{"default":"zai-org-glm-4.7"},"object":"list","type":"text"})"));
  REQUIRE(*unfiltered.type == "text");

  // Captured the same day: traits?type=all is a 200. Note what this means —
  // /models/compatibility_mapping?type=all is a 400, verbatim
  //   "Invalid enum value. Expected 'asr' | 'embedding' | 'image' | 'music' |
  //    'text' | 'tts' | 'upscale' | 'inpaint' | 'video', received 'all'"
  // — despite the two operations having byte-identical `parameters` blocks in the
  // OpenAPI document. The document's request enum omits `all` and `code` for
  // both, which is wrong for traits and right for compatibility_mapping.
  //
  // That asymmetry is the whole argument for passing `type` through as a
  // caller-supplied string instead of validating it here: a hardcoded set would
  // have to encode a divergence the spec itself gets wrong, and would be wrong
  // again the day Venice adds `all` to compatibility_mapping — which its own
  // *response* enum already anticipates. The 400 path is pinned in
  // test/06transport/; this case pins the 200 half offline.
  const auto all = model_traits_from_json_body(
      body(R"({"data":{"default":"z-image-turbo","fastest":"z-image-turbo"},)"
           R"("object":"list","type":"all"})"));
  REQUIRE(*all.type == "all");
  REQUIRE(all.entries.size() == 2);
}

// ── §8 the captures ───────────────────────────────────────────────────────

TEST_CASE("the captured traits page parses whole", "[catalogue]") {
  // `curl https://api.venice.ai/api/v1/models/traits` — no Authorization header —
  // on 2026-08-11. Verbatim, byte for byte, including key order as the server
  // sent it.
  constexpr auto kTraitsCapture =
      R"({"data":{"default":"zai-org-glm-4.7","most_intelligent":"zai-org-glm-4.7",)"
      R"("function_calling_default":"zai-org-glm-4.7","most_uncensored":"venice-uncensored-1-2",)"
      R"("default_reasoning":"qwen3-235b-a22b-thinking-2507","default_vision":"qwen3-vl-235b-a22b",)"
      R"("default_code":"qwen3-coder-480b-a35b-instruct-turbo"},"object":"list","type":"text"})";

  const auto res = model_traits_from_json_body(body(kTraitsCapture));

  REQUIRE(res.entries.size() == 7);
  REQUIRE(res.returned == 7);
  // Nothing was skipped in the real payload — the reconciliation the live leg
  // performs, asserted here so a regression in the skip rule shows up offline.
  REQUIRE(res.returned == res.entries.size());

  REQUIRE(*res.find("default") == "zai-org-glm-4.7");
  REQUIRE(*res.find("most_uncensored") == "venice-uncensored-1-2");
  REQUIRE(*res.find("default_vision") == "qwen3-vl-235b-a22b");
  REQUIRE(*res.find("default_code") == "qwen3-coder-480b-a35b-instruct-turbo");
  REQUIRE(res.find("fastest") == nullptr);  // text has no `fastest`; image does

  REQUIRE(*res.object == "list");
  REQUIRE(*res.type == "text");
}

TEST_CASE("the captured compatibility mapping parses whole", "[catalogue]") {
  // Same run, same conditions: no Authorization header, 2026-08-11.
  constexpr auto kCompatCapture =
      R"({"data":{"venice-large-1.1":"qwen3-235b-a22b-instruct-2507",)"
      R"("gpt-4.1":"qwen3-235b-a22b-instruct-2507","qwen3-235b":"qwen3-235b-a22b-thinking-2507",)"
      R"("o1-mini":"llama-3.3-70b","o1-mini-2024-09-12":"llama-3.3-70b","o3-mini":"llama-3.3-70b",)"
      R"("chatgpt-4o-latest":"llama-3.3-70b","gpt-4o-mini":"llama-3.3-70b",)"
      R"("gpt-4o-mini-2024-07-18":"llama-3.3-70b","claude-3-5-haiku-20241022":"llama-3.3-70b",)"
      R"("claude-3-haiku-20240307":"llama-3.3-70b","gpt-3.5-turbo-1106":"llama-3.3-70b",)"
      R"("gpt-3.5-turbo-instruct":"llama-3.3-70b","gpt-3.5-turbo-instruct-0914":"llama-3.3-70b",)"
      R"("gpt-3.5-turbo-0125":"llama-3.3-70b","gpt-3.5-turbo":"llama-3.3-70b",)"
      R"("gpt-3.5-turbo-16k":"llama-3.3-70b","gpt-4o":"llama-3.3-70b",)"
      R"("gpt-4o-2024-08-06":"llama-3.3-70b","gpt-4o-2024-05-13":"llama-3.3-70b",)"
      R"("gpt-4o-2024-11-20":"llama-3.3-70b","claude-3-5-sonnet-20241022":"llama-3.3-70b",)"
      R"("claude-3-5-sonnet-20240620":"llama-3.3-70b"},"object":"list","type":"text"})";

  const auto res = model_compatibility_mapping_from_json_body(body(kCompatCapture));

  REQUIRE(res.entries.size() == 23);
  REQUIRE(res.returned == 23);
  REQUIRE(res.returned == res.entries.size());

  REQUIRE(*res.find("gpt-4o") == "llama-3.3-70b");
  REQUIRE(*res.find("claude-3-5-sonnet-20241022") == "llama-3.3-70b");
  REQUIRE(*res.find("gpt-4.1") == "qwen3-235b-a22b-instruct-2507");
  // A Venice-shaped alias sits in here too: the mapping is not exclusively
  // foreign, it also carries this API's own retired names.
  REQUIRE(*res.find("venice-large-1.1") == "qwen3-235b-a22b-instruct-2507");

  REQUIRE(*res.object == "list");
  REQUIRE(*res.type == "text");

  // Measured, and worth pinning because it is the property that makes the
  // mapping useful rather than ambiguous: no alias collides with a real
  // catalogue id. `models("text")` on the same day listed 106 models and not one
  // of these 23 keys was among them, so a lookup here can never shadow a model a
  // caller could have named directly. The live leg re-checks this against the
  // real catalogue on every run; here we pin the one direction a fixture can see.
  REQUIRE(res.find("llama-3.3-70b") == nullptr);
  REQUIRE(res.find("qwen3-235b-a22b-instruct-2507") == nullptr);
}
