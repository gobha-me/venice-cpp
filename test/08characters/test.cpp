// Character listing parse and query build — offline, no API key or network.
//
// Charter: everything venice::characters_from_json_body,
// venice::character_from_json_body and Character's from_json do with the two
// /characters response shapes, plus what
// venice::character_query_params flattens a CharacterQuery into. The transport
// half (Client::characters) is not exercised here and cannot be — that is the
// point of both halves being free functions. The *encoder* those pairs are then
// handed to is test/05query/ and is not re-tested here; model listings are
// test/04models/.
//
// **The fixtures below are not a capture, and that has to be said plainly.**
// /models answers 200 for any bearer token, so test/04models/ could pin itself
// to a real payload. /characters does not: measured on 2026-08-09 it answers
// 402 unauthenticated and 401 to a junk bearer, and the implementing
// environment had no VENICE_API_KEY. They are instead assembled from Venice's
// published OpenAPI document (api.venice.ai/doc/api/swagger.yaml, fetched the
// same day) — a machine-readable schema with a per-field example, so the keys
// and their types are the API's own rather than this file's guess, but the
// *combination* has never been seen on a wire. `venice-cpp --characters` is the
// live check; if it disagrees, the fixture is what is wrong, and Character::raw
// is why a wrong guess here is recoverable rather than lossy.
//
// One rule this file must not break, the same one test/04models/ states:
// never hand a std::optional to nlohmann. The pinned fallback is v3.11.3 and
// optional support landed in 3.12.0, so `j.get<std::optional<int>>()` fails to
// compile on the pin while passing on a newer system copy. Always dereference.
//
// Failure matrix first, happy path last.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

#include <venice/venice.hpp>

using venice::Character;
using venice::character_query_params;
using venice::character_from_json_body;
using venice::characters_from_json_body;
using venice::CharacterQuery;

namespace {

// The spec's own example entry, every documented key present. Key order and
// values as the document gives them.
constexpr auto kAlanWatts =
    R"({"adult":false,"author":"k3x9q","createdAt":"2024-12-20T21:28:08.934Z",)"
    R"("description":"Alan Watts was a British and American writer and speaker.",)"
    R"("featured":false,"id":"2f460055-7595-4640-9cb6-c442c4c869b0","name":"Alan Watts",)"
    R"("shareUrl":"https://venice.ai/c/alan-watts",)"
    R"("photoUrl":"https://outerface.venice.ai/api/characters/2f460055/photo",)"
    R"("slug":"alan-watts","stats":{"averageRating":4.7,"imports":112,"ratingCount":24,)"
    R"("ratingSum":113,"userRating":null},"tags":["AlanWatts","Philosophy","Buddhism"],)"
    R"("updatedAt":"2025-02-09T03:23:53.708Z","webEnabled":true,)"
    R"("modelId":"venice-uncensored-1-2"})";

// The smallest thing that is still a character.
constexpr auto kBare = R"({"slug":"s"})";

auto list_of(const std::string& entries) -> nlohmann::json {
  return nlohmann::json::parse(R"({"data":[)" + entries + "]}");
}

auto one(const std::string& entry) -> Character {
  auto page = characters_from_json_body(list_of(entry));
  REQUIRE(page.entries.size() == 1);
  return page.entries.front();
}

// The query string Client::characters would build from this query. Spelled out
// here rather than asserted pair-by-pair because the ordering is part of the
// contract and a whole-string comparison is the only assertion that sees it.
auto query_string(const CharacterQuery& q) -> std::string {
  return venice::detail::with_query("/characters", character_query_params(q));
}

}  // namespace

// ── §0 detail response: its container is one object, not a list ───────────

TEST_CASE("a character detail response that is not an object throws",
          "[character][failure]") {
  SECTION("array") {
    REQUIRE_THROWS(character_from_json_body(nlohmann::json::parse("[]")));
  }
  SECTION("null") {
    REQUIRE_THROWS(character_from_json_body(nlohmann::json::parse("null")));
  }
  SECTION("scalar") {
    REQUIRE_THROWS(character_from_json_body(nlohmann::json::parse(R"("nope")")));
  }
  SECTION("the error names the singular endpoint") {
    try {
      static_cast<void>(character_from_json_body(nlohmann::json::parse("[]")));
      FAIL("expected a throw");
    } catch (const std::exception& e) {
      REQUIRE(std::string{e.what()}.starts_with("character:"));
    }
  }
}

TEST_CASE("a partial character detail object stays partial", "[character]") {
  const auto c = character_from_json_body(nlohmann::json::parse(kBare));
  REQUIRE(c.slug == "s");
  REQUIRE_FALSE(c.name.has_value());
  REQUIRE(c.raw == nlohmann::json::parse(kBare));
}

TEST_CASE("character detail does not synthesize a missing slug", "[character][failure]") {
  const auto c = character_from_json_body(nlohmann::json::parse(R"({"name":"Partial"})"));
  REQUIRE(c.slug.empty());
  REQUIRE(c.name == "Partial");
  REQUIRE(c.raw == nlohmann::json::parse(R"({"name":"Partial"})"));
}

// ── §1 listing container: the only place the whole list may fail ──────────
//
// Everything below this section degrades. The second case is the one that
// earns the check: iterating a json *object* yields its values and iterating a
// scalar yields the scalar, so without is_array() a `{"data":{...}}` body
// silently becomes a vector of characters built out of whatever the values
// happened to be — reporting success the whole way.

TEST_CASE("a characters response that is not a list throws", "[characters][failure]") {
  SECTION("data is a number") {
    REQUIRE_THROWS(characters_from_json_body(nlohmann::json::parse(R"({"data":42})")));
  }
  SECTION("data is an object — the garbage-character-factory case") {
    REQUIRE_THROWS(characters_from_json_body(
        nlohmann::json::parse(R"({"data":{"a":{"slug":"x"},"b":{"slug":"y"}}})")));
  }
  SECTION("body is a bare scalar") {
    REQUIRE_THROWS(characters_from_json_body(nlohmann::json::parse(R"("nope")")));
  }
  SECTION("the error names the endpoint, not the generic one") {
    // Client::characters wraps this in ErrorKind::Parse with the message
    // attached; "models: ..." arriving from a characters() call would send a
    // reader to the wrong file.
    try {
      static_cast<void>(characters_from_json_body(nlohmann::json::parse(R"({"data":42})")));
      FAIL("expected a throw");
    } catch (const std::exception& e) {
      REQUIRE(std::string{e.what()}.starts_with("characters:"));
    }
  }
}

TEST_CASE("an empty character list is a list, not a failure", "[characters]") {
  SECTION("wrapped in data") {
    REQUIRE(characters_from_json_body(nlohmann::json::parse(R"({"data":[]})")).entries.empty());
  }
  SECTION("bare array body — the no-data branch") {
    REQUIRE(characters_from_json_body(nlohmann::json::parse("[]")).entries.empty());
  }
  SECTION("the envelope's other key is ignored, not required") {
    // The documented body carries `"object":"list"` beside `data`. Requiring it
    // would make this parse fail on the day Venice drops or renames it, for no
    // gain — nothing here reads it.
    REQUIRE(characters_from_json_body(
                nlohmann::json::parse(R"({"object":"list","data":[{"slug":"a"}]})"))
                .entries.size() == 1);
  }
}

// ── §2 entries degrade, never throw ───────────────────────────────────────
//
// These two cases are a pair held in tension and neither substitutes for the
// other. The first proves junk is skipped rather than fatal; the second proves
// a *partial* entry is kept rather than skipped. Delete the second and "skip
// what cannot be used" quietly degenerates into "drop anything unfamiliar",
// which is the failure this ticket is written against.

TEST_CASE("unusable character entries are skipped, not fatal", "[characters][failure]") {
  const auto page = characters_from_json_body(nlohmann::json::parse(
      R"({"data":[{"slug":"a","name":"A"},42,null,"x",[],)"
      R"({"name":"no slug"},{"slug":123},{"slug":""},{"slug":"b"}]})"));

  REQUIRE(page.entries.size() == 2);
  REQUIRE(page.entries[0].slug == "a");
  REQUIRE(page.entries[1].slug == "b");
  // And the skipping is *visible*: nine elements arrived, two survived.
  REQUIRE(page.returned == 9);
}

TEST_CASE("a character is skipped on slug, not on id", "[characters][failure]") {
  // `id` is a UUID no call in this library accepts; `slug` is what
  // venice_parameters.character_slug sends. An entry with an id and no slug is
  // unusable, and an entry with a slug and no id is perfectly usable — getting
  // this backwards would drop every character while keeping every one that
  // cannot be selected.
  const auto page = characters_from_json_body(nlohmann::json::parse(
      R"({"data":[{"id":"2f460055","name":"has id only"},{"slug":"s","name":"has slug only"}]})"));

  REQUIRE(page.entries.size() == 1);
  REQUIRE(page.entries[0].slug == "s");
  REQUIRE_FALSE(page.entries[0].id.has_value());
}

TEST_CASE("re-parsing into a used Character leaves nothing of the old one",
          "[characters][failure]") {
  // Every field but these two is assigned unconditionally, so a conditional
  // slug would survive a re-parse and pair the *previous* character's slug
  // with this one's name — and the slug is the field that selects a persona.
  // nlohmann's get_to on a live object is the reachable path.
  venice::Character c;
  nlohmann::json::parse(kAlanWatts).get_to(c);
  REQUIRE(c.slug == "alan-watts");
  REQUIRE(c.stats.has_value());

  nlohmann::json::parse(R"({"name":"Someone Else"})").get_to(c);
  REQUIRE(c.slug.empty());
  REQUIRE_FALSE(c.stats.has_value());
  REQUIRE(c.name == "Someone Else");
}

TEST_CASE("a partial character entry is kept", "[characters]") {
  const auto c = one(kBare);

  REQUIRE(c.slug == "s");
  REQUIRE_FALSE(c.name.has_value());
  REQUIRE_FALSE(c.description.has_value());
  REQUIRE_FALSE(c.stats.has_value());
  REQUIRE(c.tags.empty());
}

// ── §3 wrong-typed fields read as absent, field by field ──────────────────
//
// The case the tolerant readers exist for. A predicate is used rather than
// try/catch because get<int>() would not throw on most of these — it would
// return a confident wrong answer. Each field is checked separately: a loop
// over "everything is absent" would stay green if from_json stopped reading
// entirely.

TEST_CASE("wrong-typed character fields degrade to absent", "[characters][failure]") {
  SECTION("strings sent as other types") {
    const auto c = one(R"({"slug":"s","name":42,"description":null,"modelId":[],)"
                       R"("shareUrl":{},"photoUrl":false,"createdAt":1734729288})");
    REQUIRE(c.slug == "s");
    REQUIRE_FALSE(c.name.has_value());
    REQUIRE_FALSE(c.description.has_value());
    REQUIRE_FALSE(c.model_id.has_value());
    REQUIRE_FALSE(c.share_url.has_value());
    REQUIRE_FALSE(c.photo_url.has_value());
    REQUIRE_FALSE(c.created_at.has_value());
  }

  SECTION("booleans sent as strings — the ones a picker filters on") {
    const auto c = one(R"({"slug":"s","adult":"yes","featured":1,"webEnabled":"true"})");
    REQUIRE_FALSE(c.adult.has_value());
    REQUIRE_FALSE(c.featured.has_value());
    REQUIRE_FALSE(c.web_enabled.has_value());
  }

  SECTION("an unset boolean is not a false") {
    // The distinction Model's comment makes and the one a filter gets wrong:
    // `!*c.adult` on an absent field is UB, and treating absent as false marks
    // an unlabelled character safe.
    const auto c = one(kBare);
    REQUIRE_FALSE(c.adult.has_value());
    REQUIRE(c.adult != true);
    REQUIRE(c.adult != false);
  }

  SECTION("tags that are not an array of strings") {
    REQUIRE(one(R"({"slug":"s","tags":"helpful"})").tags.empty());
    REQUIRE(one(R"({"slug":"s","tags":{}})").tags.empty());
    // A mixed array keeps the strings and drops the rest, rather than losing
    // the whole field to one bad element.
    REQUIRE(one(R"({"slug":"s","tags":["a",42,null,"b"]})").tags == std::vector<std::string>{"a", "b"});
  }

  SECTION("stats that are not an object") {
    REQUIRE_FALSE(one(R"({"slug":"s","stats":[]})").stats.has_value());
    REQUIRE_FALSE(one(R"({"slug":"s","stats":4.7})").stats.has_value());
  }

  SECTION("stats fields degrade individually") {
    const auto c = one(R"({"slug":"s","stats":{"averageRating":"4.7","imports":112,)"
                       R"("ratingCount":null,"userRating":null}})");
    REQUIRE(c.stats.has_value());
    REQUIRE_FALSE(c.stats->average_rating.has_value());
    REQUIRE(c.stats->imports == 112.0);
    REQUIRE_FALSE(c.stats->rating_count.has_value());
    REQUIRE_FALSE(c.stats->rating_sum.has_value());
    // Documented nullable: the caller's own rating, absent when unrated.
    REQUIRE_FALSE(c.stats->user_rating.has_value());
  }

  SECTION("a rating quoted as a whole number is still a number") {
    // The mistake test/04models/ was written against: Venice sends whole
    // values as JSON integers, and is_number_float() would read 5 as absent.
    const auto c = one(R"({"slug":"s","stats":{"averageRating":5}})");
    REQUIRE(c.stats.has_value());
    REQUIRE(c.stats->average_rating == 5.0);
  }
}

// ── §3 the page, not just the list ────────────────────────────────────────
//
// The section this endpoint needs and /models does not. `characters()` returns
// a CharacterPage because a bare vector cannot answer "was that the last
// page?": the parse skips what it cannot use, so the usable count and the
// server's page size are different numbers, and paging on the wrong one ends
// the walk early and calls a truncated catalogue complete.

TEST_CASE("returned counts what the server sent, entries what survived",
          "[characters][page]") {
  SECTION("they agree when nothing is skipped") {
    const auto page = characters_from_json_body(
        nlohmann::json::parse(R"({"data":[{"slug":"a"},{"slug":"b"},{"slug":"c"}]})"));
    REQUIRE(page.returned == 3);
    REQUIRE(page.entries.size() == 3);
  }

  SECTION("they diverge on exactly one skipped entry — the paging bug") {
    // A "full" page of three with one unusable entry. Terminating a walk on
    // entries.size() < 3 stops here; on returned < 3 it correctly continues.
    const auto page = characters_from_json_body(
        nlohmann::json::parse(R"({"data":[{"slug":"a"},{"name":"no slug"},{"slug":"c"}]})"));
    REQUIRE(page.returned == 3);
    REQUIRE(page.entries.size() == 2);
  }

  SECTION("a page of entirely unusable entries is still a full page") {
    // The degenerate version, and the one a caller most needs: zero usable
    // characters must not read as "the catalogue ended".
    const auto page = characters_from_json_body(
        nlohmann::json::parse(R"({"data":[{"name":"x"},42,{"slug":""}]})"));
    REQUIRE(page.returned == 3);
    REQUIRE(page.entries.empty());
  }

  SECTION("an empty page is empty by both counts") {
    const auto page = characters_from_json_body(nlohmann::json::parse(R"({"data":[]})"));
    REQUIRE(page.returned == 0);
    REQUIRE(page.entries.empty());
  }
}

TEST_CASE("the page keeps the whole envelope", "[characters][page][raw]") {
  // The documented body is {data, object} with no total and no cursor — but
  // that shape has never been seen on a wire, so an envelope key that turns up
  // later has to be reachable without changing this signature.
  const auto page = characters_from_json_body(nlohmann::json::parse(
      R"({"object":"list","total":812,"data":[{"slug":"a"}]})"));

  REQUIRE(page.raw.at("object") == "list");
  REQUIRE(page.raw.at("total") == 812);
  REQUIRE(page.raw.at("data").size() == 1);
}

// ── §4 raw is a superset ──────────────────────────────────────────────────

TEST_CASE("Character::raw holds the whole entry, modeled keys included",
          "[characters][raw]") {
  const auto c = one(kAlanWatts);

  SECTION("a modeled key is still there — a subtractive hatch would break this") {
    REQUIRE(c.raw.at("slug") == "alan-watts");
    REQUIRE(c.raw.at("name") == "Alan Watts");
    REQUIRE(c.raw.at("stats").at("averageRating") == 4.7);
  }

  SECTION("an unmodeled key survives — the preview-API case") {
    const auto c2 = one(R"({"slug":"s","somethingNew":{"x":1}})");
    REQUIRE(c2.raw.at("somethingNew").at("x") == 1);
  }

  SECTION("an entry round-trips through raw without a to_json") {
    REQUIRE(c.raw == nlohmann::json::parse(kAlanWatts));
  }
}

// ── §5 the query build ────────────────────────────────────────────────────
//
// Flattening is asserted through the finished URL rather than the pair vector,
// because emission *order* is part of the contract — it is what makes the
// string deterministic enough to assert at all — and a pair-by-pair check
// cannot see it.

TEST_CASE("an unset character query sends no query string at all", "[characters][query]") {
  // The guarantee that matters most: characters() with no argument must send
  // the bare path, exactly as it would if CharacterQuery did not exist.
  REQUIRE(query_string(CharacterQuery{}) == "/characters");
  REQUIRE(character_query_params(CharacterQuery{}).empty());
}

TEST_CASE("empty character query values are not filters", "[characters][query][failure]") {
  SECTION("a set-but-empty string emits nothing") {
    CharacterQuery q;
    q.search = "";
    q.sort_by = "";
    REQUIRE(query_string(q) == "/characters");
  }

  SECTION("an empty vector emits nothing") {
    CharacterQuery q;
    q.tags = {};
    q.categories = {};
    REQUIRE(query_string(q) == "/characters");
  }

  SECTION("a vector of empty strings emits nothing, not bare keys") {
    CharacterQuery q;
    q.tags = {"", ""};
    REQUIRE(query_string(q) == "/characters");
  }

  SECTION("an empty element is dropped from the middle of a repetition") {
    CharacterQuery q;
    q.tags = {"a", "", "b"};
    REQUIRE(query_string(q) == "/characters?tags=a&tags=b");
  }
}

TEST_CASE("character query fields use the wire's own spelling", "[characters][query]") {
  SECTION("camelCase keys, not the struct's snake_case") {
    CharacterQuery q;
    q.sort_by = "featured";
    q.sort_order = "desc";
    q.is_adult = false;
    q.is_pro = true;
    q.is_web_enabled = true;
    q.model_id = {"venice-uncensored-1-2"};
    REQUIRE(query_string(q) ==
            "/characters?sortBy=featured&sortOrder=desc&isAdult=false&isPro=true"
            "&isWebEnabled=true&modelId=venice-uncensored-1-2");
  }

  SECTION("a bool is the word, not 1/0 — the schema says the strings") {
    CharacterQuery q;
    q.is_adult = true;
    REQUIRE(query_string(q) == "/characters?isAdult=true");
    q.is_adult = false;
    REQUIRE(query_string(q) == "/characters?isAdult=false");
  }

  SECTION("a false bool is sent, not skipped as if unset") {
    // isAdult=false is the interesting value, and an implementation that
    // tested truthiness rather than has_value() would silently drop it.
    CharacterQuery q;
    q.is_web_enabled = false;
    REQUIRE(query_string(q) == "/characters?isWebEnabled=false");
  }

  SECTION("numbers are decimal text") {
    CharacterQuery q;
    q.limit = 100;
    q.offset = 0;
    // offset=0 is a real value — page one — and must survive, unlike an empty
    // string. This is the pagination call the endpoint's default of 50 forces.
    REQUIRE(query_string(q) == "/characters?limit=100&offset=0");
  }

  SECTION("an out-of-range value is passed through to the server's 400") {
    // AGENTS.md, "range checking: none". The server documents 0 < limit <= 100;
    // refusing it here would be this client inventing a rule it cannot keep
    // current.
    CharacterQuery q;
    q.limit = 5000;
    q.offset = -1;
    REQUIRE(query_string(q) == "/characters?limit=5000&offset=-1");
  }

  SECTION("a multi-valued filter repeats the key, one element each") {
    CharacterQuery q;
    q.tags = {"helpful", "productivity"};
    q.categories = {"roleplay", "philosophy"};
    REQUIRE(query_string(q) ==
            "/characters?tags=helpful&tags=productivity"
            "&categories=roleplay&categories=philosophy");
  }

  SECTION("a value containing a comma is one parameter on the wire") {
    // What this pins is the *client* half only: `{"a, b"}` produces one
    // repetition and `{"a"," b"}` produces two, so the two are distinguishable
    // in what we send. It deliberately does NOT claim the server treats them
    // differently — measured 2026-08-09, it splits on commas inside a value
    // too, so a comma-containing tag is inexpressible either way. An earlier
    // version of this comment claimed the round-trip was preserved; it is not,
    // and the CharacterQuery::tags note carries the numbers.
    CharacterQuery q;
    q.categories = {"science, fiction"};
    REQUIRE(query_string(q) == "/characters?categories=science%2C%20fiction");

    CharacterQuery two;
    two.categories = {"science", " fiction"};
    REQUIRE(query_string(two) == "/characters?categories=science&categories=%20fiction");
    REQUIRE(query_string(two) != query_string(q));
  }

  SECTION("a search term is encoded, not concatenated") {
    CharacterQuery q;
    q.search = "alan watts&isAdult=true";
    REQUIRE(query_string(q) == "/characters?search=alan%20watts%26isAdult%3Dtrue");
  }
}

TEST_CASE("character query extra is additive and loses to a modeled field",
          "[characters][query][failure]") {
  SECTION("an unmodeled parameter is appended last") {
    CharacterQuery q;
    q.limit = 10;
    q.extra = {{"newFilter", "1"}};
    REQUIRE(query_string(q) == "/characters?limit=10&newFilter=1");
  }

  SECTION("a key a set field already emitted is dropped, not sent twice") {
    // The request-side hatch's contract is "modeled fields win", and duplicate
    // query keys are not something servers agree on — first-wins, last-wins and
    // 400 are all real. Sending one key once is the only unambiguous answer.
    CharacterQuery q;
    q.limit = 10;
    q.extra = {{"limit", "999"}};
    REQUIRE(query_string(q) == "/characters?limit=10");
  }

  SECTION("a key whose modeled field is *unset* still goes through") {
    // Only a field that actually emitted takes the key. Otherwise the hatch
    // would lose to a field nobody set, which is a rule with no upside.
    CharacterQuery q;
    q.extra = {{"limit", "999"}};
    REQUIRE(query_string(q) == "/characters?limit=999");
  }

  SECTION("an extra pair with an empty key or value is dropped") {
    CharacterQuery q;
    q.extra = {{"", "1"}, {"k", ""}};
    REQUIRE(query_string(q) == "/characters");
  }
}

// ── §7 happy path, last ───────────────────────────────────────────────────

TEST_CASE("the documented character entry parses whole", "[characters]") {
  const auto c = one(kAlanWatts);

  REQUIRE(c.slug == "alan-watts");
  REQUIRE(c.id == "2f460055-7595-4640-9cb6-c442c4c869b0");
  REQUIRE(c.name == "Alan Watts");
  REQUIRE(c.description.has_value());
  REQUIRE(c.author == "k3x9q");
  REQUIRE(c.model_id == "venice-uncensored-1-2");
  REQUIRE(c.share_url == "https://venice.ai/c/alan-watts");
  REQUIRE(c.photo_url.has_value());
  REQUIRE(c.created_at == "2024-12-20T21:28:08.934Z");
  REQUIRE(c.updated_at == "2025-02-09T03:23:53.708Z");
  REQUIRE(c.adult == false);
  REQUIRE(c.featured == false);
  REQUIRE(c.web_enabled == true);
  REQUIRE(c.tags == std::vector<std::string>{"AlanWatts", "Philosophy", "Buddhism"});

  REQUIRE(c.stats.has_value());
  REQUIRE(c.stats->average_rating == 4.7);
  REQUIRE(c.stats->imports == 112.0);
  REQUIRE(c.stats->rating_count == 24.0);
  REQUIRE(c.stats->rating_sum == 113.0);
}

TEST_CASE("the documented character detail object parses whole", "[character]") {
  const auto expected = nlohmann::json::parse(kAlanWatts);
  const auto c = character_from_json_body(expected);

  REQUIRE(c.slug == "alan-watts");
  REQUIRE(c.id == "2f460055-7595-4640-9cb6-c442c4c869b0");
  REQUIRE(c.name == "Alan Watts");
  REQUIRE(c.model_id == "venice-uncensored-1-2");
  REQUIRE(c.web_enabled == true);
  REQUIRE(c.stats.has_value());
  REQUIRE(c.stats->average_rating == 4.7);
  REQUIRE(c.raw == expected);
}

TEST_CASE("a full listing parses into a picker's worth of characters", "[characters]") {
  const auto page = characters_from_json_body(
      list_of(std::string{kAlanWatts} + "," + kBare + R"(,{"slug":"z","name":"Z"})"));

  REQUIRE(page.entries.size() == 3);
  REQUIRE(page.returned == 3);
  REQUIRE(page.entries[0].name == "Alan Watts");
  REQUIRE_FALSE(page.entries[1].name.has_value());
  REQUIRE(page.entries[2].name == "Z");
}

TEST_CASE("the pagination query is what a full walk would send", "[characters][query]") {
  // Mirrors the loop in Client::characters's comment: ask for the cap, page by
  // it, stop on a short page.
  CharacterQuery q;
  q.limit = 100;
  q.offset = 0;
  REQUIRE(query_string(q) == "/characters?limit=100&offset=0");
  *q.offset += 100;
  REQUIRE(query_string(q) == "/characters?limit=100&offset=100");
}
