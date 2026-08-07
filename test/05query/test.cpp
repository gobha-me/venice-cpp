// URL query construction — offline, no API key or network.
//
// Charter: venice::detail::percent_encode and venice::detail::with_query, the
// request half of Client::models's type filter (VC-13, #19). The response half
// — what a /models body parses into — is test/04models/ and is not touched
// here; request *bodies* are test/02request/; precondition guards are
// test/03guards/. Nothing in this file builds a Model or opens a socket.
//
// These are free functions at namespace scope rather than Client members
// precisely so this file can exist. A private path helper would be reachable
// only by making a request, which would make the encoding testable only against
// api.venice.ai — and an encoder is the one thing in a client that is a pure
// string transform.
//
// Failure matrix first, happy path last.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

#include <venice/venice.hpp>

using venice::detail::percent_encode;
using venice::detail::with_query;

// ── §0 percent_encode: bytes that must NOT survive contact ────────────────
//
// The whole reason this function exists rather than string concatenation.

TEST_CASE("percent_encode escapes every byte outside the unreserved set", "[query][failure]") {
  SECTION("query and path delimiters") {
    REQUIRE(percent_encode("&") == "%26");
    REQUIRE(percent_encode("=") == "%3D");
    REQUIRE(percent_encode("?") == "%3F");
    REQUIRE(percent_encode("/") == "%2F");
    REQUIRE(percent_encode("#") == "%23");
    REQUIRE(percent_encode(" ") == "%20");
  }

  SECTION("the escape character itself is escaped — no double-decode") {
    REQUIRE(percent_encode("%") == "%25");
    REQUIRE(percent_encode("%26") == "%2526");
  }

  SECTION("'+' is escaped, not treated as an encoded space") {
    // Some decoders read a bare + in a query as a space (HTML form encoding,
    // not RFC 3986). Emitting %2B means a literal plus survives either reading.
    REQUIRE(percent_encode("a+b") == "a%2Bb");
  }

  SECTION("an embedded NUL does not terminate the output") {
    // string_view carries its own length; a char* would have stopped at the 0.
    REQUIRE(percent_encode(std::string_view{"a\0b", 3}) == "a%00b");
  }
}

// `char` is signed, so a byte above 0x7F is negative and indexes the hex table
// backwards. Measured with the cast removed: the default build and the UBSan
// build both stay silent and simply produce wrong output ("% F" for 0xFF);
// only ASan objects, and only in the sanitizer jobs. These exact-output
// assertions are therefore the primary guard, not a belt-and-braces one.
//
// Note that the second section below — a property, not a value — stays green
// through that bug: the length is right and the bytes are not. It is kept for
// the shape it does cover, but it is not what catches this.
TEST_CASE("percent_encode treats bytes above 0x7F as unsigned", "[query][failure]") {
  SECTION("0xFF is one escape, not a sign-extended int") {
    REQUIRE(percent_encode(std::string_view{"\xFF", 1}) == "%FF");
  }

  SECTION("every high byte encodes to exactly three characters") {
    for (int b = 0x80; b <= 0xFF; ++b) {
      const auto c = static_cast<char>(b);
      const auto out = percent_encode(std::string_view{&c, 1});
      INFO("byte 0x" << std::hex << b << " encoded as " << out);
      REQUIRE(out.size() == 3);
      REQUIRE(out.front() == '%');
    }
  }

  SECTION("a multi-byte UTF-8 sequence encodes byte by byte") {
    REQUIRE(percent_encode("é") == "%C3%A9");  // U+00E9, two bytes
  }
}

TEST_CASE("percent_encode emits uppercase hex", "[query]") {
  // RFC 3986 §2.1: producers should uppercase. Both decode, but a lowercase
  // emitter here would make every expectation in this file wrong at once.
  REQUIRE(percent_encode(std::string_view{"\xAB", 1}) == "%AB");
  REQUIRE(percent_encode(":") == "%3A");
}

// ── §1 percent_encode: what passes through untouched ──────────────────────

TEST_CASE("percent_encode leaves the unreserved set alone", "[query]") {
  SECTION("empty input") { REQUIRE(percent_encode("").empty()); }

  SECTION("alphanumerics and the four unreserved marks") {
    REQUIRE(percent_encode("abcXYZ019") == "abcXYZ019");
    REQUIRE(percent_encode("-._~") == "-._~");
  }

  SECTION("every Venice model type is unreserved — no escaping in practice") {
    for (const std::string_view type : {"text", "image", "video", "tts", "embedding", "inpaint",
                                        "music", "asr", "upscale", "all"}) {
      INFO("type " << type);
      REQUIRE(percent_encode(type) == std::string{type});
    }
  }
}

// ── §2 with_query: the empty-value skip ───────────────────────────────────
//
// The non-breaking guarantee, asserted rather than asserted-in-a-comment.
// Client::models(std::string_view type = {}) routes through with_query on every
// call including the no-argument one, so if an empty value produced a trailing
// "?" the default path would have changed shape for every existing caller.

TEST_CASE("with_query omits pairs whose value is empty", "[query][failure]") {
  SECTION("nothing set — the path is returned byte-for-byte, with no '?'") {
    REQUIRE(with_query("/models", {{"type", ""}}) == "/models");
  }

  SECTION("an empty parameter list is also inert") {
    REQUIRE(with_query("/models", {}) == "/models");
  }

  SECTION("a skipped leading pair does not consume the '?'") {
    // The separator is chosen per *emitted* pair, not per position: get this
    // wrong and the first surviving pair arrives as "&type=image".
    REQUIRE(with_query("/models", {{"unset", ""}, {"type", "image"}}) == "/models?type=image");
  }

  SECTION("a skipped trailing pair leaves no dangling '&'") {
    REQUIRE(with_query("/models", {{"type", "image"}, {"unset", ""}}) == "/models?type=image");
  }

  SECTION("an empty *key* with a value is still emitted") {
    // Only the value decides. A caller that passes an empty key has a bug, but
    // silently dropping it would hide the bug rather than send it somewhere it
    // can be seen.
    REQUIRE(with_query("/models", {{"", "image"}}) == "/models?=image");
  }
}

// ── §3 with_query: assembly ───────────────────────────────────────────────

TEST_CASE("with_query encodes both halves of every pair", "[query]") {
  SECTION("a value needing escapes") {
    REQUIRE(with_query("/models", {{"type", "a b&c"}}) == "/models?type=a%20b%26c");
  }

  SECTION("a key needing escapes — no caller does this yet, which is the point") {
    REQUIRE(with_query("/models", {{"o dd", "1"}}) == "/models?o%20dd=1");
  }

  SECTION("an injected separator cannot forge a second parameter") {
    REQUIRE(with_query("/models", {{"type", "image&admin=1"}}) ==
            "/models?type=image%26admin%3D1");
  }
}

TEST_CASE("with_query joins multiple pairs with '&'", "[query]") {
  REQUIRE(with_query("/models", {{"type", "image"}, {"page", "2"}}) ==
          "/models?type=image&page=2");
  REQUIRE(with_query("/models", {{"a", "1"}, {"b", "2"}, {"c", "3"}}) == "/models?a=1&b=2&c=3");
}

// ── §4 happy path: what Client::models actually builds ────────────────────

TEST_CASE("with_query builds the /models endpoints", "[query]") {
  // Mirrors Client::models's call site. The first line is the pre-VC-13
  // behaviour every existing caller already depends on.
  REQUIRE(with_query("/models", {{"type", ""}}) == "/models");
  REQUIRE(with_query("/models", {{"type", "all"}}) == "/models?type=all");
  REQUIRE(with_query("/models", {{"type", "image"}}) == "/models?type=image");
}
