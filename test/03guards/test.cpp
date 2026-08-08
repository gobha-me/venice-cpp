// Client precondition guards — offline, no API key, no socket.
//
// Charter: everything Client::chat / Client::chat_stream refuse *before* the
// transport. Wire-body serialization lives in test/02request/ — all of it — and
// nothing here inspects a body, so the two charters do not overlap.
//
// Nothing in this file opens a connection, and that is a property of the cases
// rather than of the fixture: every one trips a guard that returns first, so the
// fake key below is never used. The single case that must prove the finite path
// *passes* cannot be written as a real call — a valid request would go to
// api.venice.ai and the assertion would then depend on whether the runner has a
// network, which the offline rule in AGENTS.md forbids. It is written as a
// precedence assertion instead: Client::validate sweeps for non-finite doubles
// before it checks for an empty model, so a finite request with an empty model
// coming back "model is empty" is proof the sweep ran and let the values
// through.
//
// That inference depends on the ordering, so the ordering is itself pinned, by
// the second-to-last case. The two work as a pair and neither substitutes for
// the other — verified by moving the sweep after the emptiness checks, which
// turns the precedence case red while the happy-path case keeps passing for the
// wrong reason (it would then be asserting only that an empty model is an empty
// model). Delete the precedence case and the last one silently stops proving
// anything.
//
// Failure matrix first, happy path last.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <venice/venice.hpp>

using venice::ChatRequest;
using venice::Client;
using venice::ErrorKind;
using venice::Message;
using venice::VeniceParameters;

namespace {

// Syntactically valid, never used: every case below returns before
// make_transport() is reached.
const Client kClient{"not-a-real-key"};

auto minimal() -> ChatRequest {
  ChatRequest r;
  r.model = "m";
  r.messages = {Message::user("hi")};
  return r;
}

constexpr auto kQuietNaN = std::numeric_limits<double>::quiet_NaN();
constexpr auto kSignalingNaN = std::numeric_limits<double>::signaling_NaN();
constexpr auto kInf = std::numeric_limits<double>::infinity();

// Mirrors Client::validate's own table on purpose: if a fifth double field is
// added there and not here, the new field ships unguarded and this file still
// passes. The two lists are meant to be edited together.
struct Field {
  std::optional<double> ChatRequest::*ptr;
  const char* name;
};
constexpr std::array<Field, 4> kGuarded{{
    {&ChatRequest::temperature, "temperature"},
    {&ChatRequest::top_p, "top_p"},
    {&ChatRequest::frequency_penalty, "frequency_penalty"},
    {&ChatRequest::presence_penalty, "presence_penalty"},
}};

constexpr std::array<double, 4> kNonFinite{{kQuietNaN, kSignalingNaN, kInf, -kInf}};

}  // namespace

// ── the matrix: every guarded field × every non-finite value ──────────────

TEST_CASE("chat rejects every non-finite double, naming the field", "[guards][chat][failure]") {
  for (const auto& [ptr, name] : kGuarded) {
    for (const double bad : kNonFinite) {
      INFO(name << " = " << bad);
      auto r = minimal();
      r.*ptr = bad;

      const auto res = kClient.chat(r);
      REQUIRE_FALSE(res.has_value());
      REQUIRE(res.error().is(ErrorKind::InvalidArg));
      REQUIRE(res.error().status == 0);  // no HTTP happened
      REQUIRE(res.error().body.empty());
      // Naming the field is the point of the ticket: "invalid argument" on its
      // own sends the caller back to re-read four optionals.
      REQUIRE(res.error().message.find(name) != std::string::npos);
    }
  }
}

TEST_CASE("chat_stream rejects every non-finite double before the callback fires",
          "[guards][stream][failure]") {
  for (const auto& [ptr, name] : kGuarded) {
    for (const double bad : kNonFinite) {
      INFO(name << " = " << bad);
      auto r = minimal();
      r.*ptr = bad;

      bool invoked = false;
      const auto res = kClient.chat_stream(r, [&](std::string_view) {
        invoked = true;
        return true;
      });
      REQUIRE_FALSE(res.has_value());
      REQUIRE(res.error().is(ErrorKind::InvalidArg));
      REQUIRE(res.error().message.find(name) != std::string::npos);
      // The strongest proof available from outside that no transport ran: not
      // one byte was delivered to the caller's sink.
      REQUIRE_FALSE(invoked);
    }
  }
}

TEST_CASE("a non-finite double is rejected however rich the rest of the request is",
          "[guards][failure]") {
  // A guard that only holds for a bare request is not a guard.
  auto r = minimal();
  r.max_tokens = 64;
  r.seed = 7;
  r.stop = std::vector<std::string>{"\n"};
  r.response_format = venice::response_format::json_object();
  r.tools = std::vector<nlohmann::json>{venice::tools::function("f")};
  r.tool_choice = venice::tool_choice::required();
  r.parallel_tool_calls = false;
  r.venice_parameters = VeniceParameters{};
  r.venice_parameters->disable_thinking = true;
  r.extra = nlohmann::json::parse(R"({"top_k":40})");
  r.presence_penalty = -kInf;

  const auto res = kClient.chat(r);
  REQUIRE_FALSE(res.has_value());
  REQUIRE(res.error().is(ErrorKind::InvalidArg));
  REQUIRE(res.error().message.find("presence_penalty") != std::string::npos);
}

// ── the structural guards, on both entry points ───────────────────────────
//
// These were duplicated verbatim across chat and chat_stream before VC-10; one
// helper feeds both now, and these cases are what keep that honest.

TEST_CASE("both entry points refuse a structurally unsendable request", "[guards][failure]") {
  const auto sink = [](std::string_view) { return true; };

  SECTION("empty model — chat") {
    ChatRequest r;
    r.messages = {Message::user("hi")};
    const auto res = kClient.chat(r);
    REQUIRE_FALSE(res.has_value());
    REQUIRE(res.error().is(ErrorKind::InvalidArg));
    REQUIRE(res.error().message == "model is empty");
  }
  SECTION("empty model — chat_stream") {
    ChatRequest r;
    r.messages = {Message::user("hi")};
    const auto res = kClient.chat_stream(r, sink);
    REQUIRE_FALSE(res.has_value());
    REQUIRE(res.error().is(ErrorKind::InvalidArg));
    REQUIRE(res.error().message == "model is empty");
  }
  SECTION("empty messages — chat") {
    ChatRequest r;
    r.model = "m";
    const auto res = kClient.chat(r);
    REQUIRE_FALSE(res.has_value());
    REQUIRE(res.error().is(ErrorKind::InvalidArg));
    REQUIRE(res.error().message == "messages is empty");
  }
  SECTION("empty messages — chat_stream") {
    ChatRequest r;
    r.model = "m";
    const auto res = kClient.chat_stream(r, sink);
    REQUIRE_FALSE(res.has_value());
    REQUIRE(res.error().is(ErrorKind::InvalidArg));
    REQUIRE(res.error().message == "messages is empty");
  }
}

// ── extra is passthrough, in both directions ──────────────────────────────

TEST_CASE("extra cannot satisfy a guard it does not model", "[guards][extra][failure]") {
  // Moved here from test/02request/ (VC-10): it asserts on Client's return
  // value, not on a body, so it belongs to this file's charter. It gained the
  // chat_stream mirror on the way.
  SECTION("a model in extra is not a model — chat") {
    ChatRequest r;
    r.messages = {Message::user("hi")};
    r.extra = nlohmann::json::parse(R"({"model":"m"})");
    const auto res = kClient.chat(r);
    REQUIRE_FALSE(res.has_value());
    REQUIRE(res.error().is(ErrorKind::InvalidArg));
    REQUIRE(res.error().message == "model is empty");
  }
  SECTION("a model in extra is not a model — chat_stream") {
    ChatRequest r;
    r.messages = {Message::user("hi")};
    r.extra = nlohmann::json::parse(R"({"model":"m"})");
    const auto res = kClient.chat_stream(r, [](std::string_view) { return true; });
    REQUIRE_FALSE(res.has_value());
    REQUIRE(res.error().is(ErrorKind::InvalidArg));
    REQUIRE(res.error().message == "model is empty");
  }
}

TEST_CASE("a non-finite value inside extra is deliberately not inspected",
          "[guards][extra][failure]") {
  // extra is documented verbatim passthrough; walking an arbitrary tree on every
  // call is exactly the cost VC-11 removed, and the best message such a walk
  // could produce would be no more actionable than the 400 it replaces. This
  // pins the boundary as a decision rather than an oversight — the guard covers
  // modeled fields only. Paired with an empty model so the case stays offline:
  // what comes back is the structural message, never a complaint about min_p.
  ChatRequest r;
  r.messages = {Message::user("hi")};
  r.extra = nlohmann::json::object();
  r.extra["min_p"] = kQuietNaN;

  const auto res = kClient.chat(r);
  REQUIRE_FALSE(res.has_value());
  REQUIRE(res.error().message == "model is empty");
  REQUIRE(res.error().message.find("min_p") == std::string::npos);
}

TEST_CASE("tools is deliberately not walked either", "[guards][tools][failure]") {
  // Same boundary as extra above, and the same decision rather than a new one
  // (VC-08). Refusing a nameless tool looks like it belongs beside "model is
  // empty" — both are representable in JSON and rejected anyway — but
  // Client::validate checks `messages` for emptiness and never enters it, so
  // Message::role is unvalidated and the request below already sails through on
  // that count. Guarding tools[i].name while ignoring messages[i].role would be
  // a coin flip, not a line; and the server's 400 names the offending entry,
  // which is the property the isfinite sweep has and this would not.
  //
  // Paired with an empty model to stay offline, exactly as the extra case is:
  // what comes back is the structural message, never a complaint about tools.
  ChatRequest r;
  r.messages = {Message::user("hi")};
  r.tools = std::vector<nlohmann::json>{venice::tools::function("")};
  r.tool_choice = venice::tool_choice::function("also-not-declared");

  const auto res = kClient.chat(r);
  REQUIRE_FALSE(res.has_value());
  REQUIRE(res.error().message == "model is empty");
  REQUIRE(res.error().message.find("tool") == std::string::npos);
}

// ── precedence, and the happy path ────────────────────────────────────────

TEST_CASE("a non-finite double outranks the structural checks", "[guards][failure]") {
  // This is what pins Client::validate's ordering, and it is the only case that
  // does. The one below it reads that ordering as proof; without this, a reorder
  // would leave the whole suite green and the accept path unasserted.
  ChatRequest r;  // empty model AND empty messages AND a NaN
  r.temperature = kQuietNaN;

  const auto res = kClient.chat(r);
  REQUIRE_FALSE(res.has_value());
  REQUIRE(res.error().message.find("temperature") != std::string::npos);
}

TEST_CASE("finite doubles pass the guard, boundaries included", "[guards]") {
  // The happy path, asserted without a socket. See the file header: this only
  // proves anything because the sweep runs before the empty-model check, so
  // "model is empty" is unreachable unless all four values were accepted.
  auto r = minimal();
  r.model.clear();
  r.temperature = 0.0;                                             // engaged, not unset
  r.top_p = -0.0;                                                  // negative zero is finite
  r.frequency_penalty = std::numeric_limits<double>::max();        // DBL_MAX is finite
  r.presence_penalty = std::numeric_limits<double>::denorm_min();  // denormals too

  const auto res = kClient.chat(r);
  REQUIRE_FALSE(res.has_value());
  REQUIRE(res.error().message == "model is empty");

  const auto stream_res = kClient.chat_stream(r, [](std::string_view) { return true; });
  REQUIRE_FALSE(stream_res.has_value());
  REQUIRE(stream_res.error().message == "model is empty");
}
