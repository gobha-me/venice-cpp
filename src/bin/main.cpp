// venice-cpp smoke binary — exercises the client against the live API when a
// key is present, else prints the library shape. Real usage comes later; for
// now this proves the header-only client links and runs.

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <venice/venice.hpp>

namespace {

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
  const auto models = client.models("text");
  if (!models) {
    std::cerr << "models failed [" << venice::to_string(models.error().kind) << "] "
              << models.error().message << '\n';
    return EXIT_FAILURE;
  }
  std::string chosen;
  for (const auto& m : *models)
    if (m.capabilities && m.capabilities->supports_reasoning &&
        *m.capabilities->supports_reasoning) {
      chosen = m.id;
      break;
    }
  if (chosen.empty()) {
    std::cerr << "no text model reported supports_reasoning -- either none does, or that"
                 " flag is not where Model expects it\n";
    return EXIT_FAILURE;
  }
  std::cerr << "(streaming from " << chosen << ")\n";

  venice::ChatRequest req;
  req.model = chosen;  // a reasoning model, so thinking has somewhere to come from
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

  if (res->usage) {
    std::cerr << "prompt/completion  : " << res->usage->prompt_tokens << '/'
              << res->usage->completion_tokens << '\n'
              << "reasoning_tokens   : ";
    if (res->usage->reasoning_tokens) std::cerr << *res->usage->reasoning_tokens << '\n';
    else std::cerr << "(absent -- check completion_tokens_details nesting)\n";
    std::cerr << "cached_tokens      : ";
    if (res->usage->cached_tokens) std::cerr << *res->usage->cached_tokens << '\n';
    else std::cerr << "(absent -- expected below prompt_tokens_details)\n";
  } else {
    std::cerr << "usage              : (no usage frame arrived at all)\n";
  }

  // The claim that matters most, and the one no fixture can settle: whether the
  // assembled turn is something Venice will accept back.
  std::cerr << "\n-- the turn, as it would be replayed --\n"
            << nlohmann::json(msg).dump(2) << '\n';
  return EXIT_SUCCESS;
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
  std::string chosen{model};
  if (chosen.empty()) {
    // Pick a model that claims to support this, which also gives VC-03's
    // supports_function_calling flag its first live use — a flag nothing has
    // ever branched on is a flag nobody knows is parsed.
    const auto models = client.models("text");
    if (!models) {
      std::cerr << "models failed [" << venice::to_string(models.error().kind) << "] "
                << models.error().message << '\n';
      return EXIT_FAILURE;
    }
    for (const auto& m : *models)
      if (m.capabilities && m.capabilities->supports_function_calling &&
          *m.capabilities->supports_function_calling) {
        chosen = m.id;
        break;
      }
    if (chosen.empty()) {
      std::cerr << "no text model reported supports_function_calling -- either none does,"
                   " or that flag is not where Model expects it\n";
      return EXIT_FAILURE;
    }
  }

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

  std::cerr << "model: " << chosen << "\n\n-- leg 1: the body actually sent --\n"
            << req.to_json_body(false).dump(2) << '\n';

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

  // Leg two. Replay the assistant turn verbatim, then answer the call.
  req.messages.push_back(*first->message);
  req.messages.push_back(venice::Message::tool(call.id, R"({"temp_f":68,"sky":"fog"})"));

  const auto second = client.chat(req);
  if (!second) {
    std::cerr << "\n-- leg 2 REJECTED --\n[" << venice::to_string(second.error().kind) << "] "
              << second.error().message << "\nbody: " << second.error().body
              << "\n\nThis is the half no fixture can settle: the turn assembled here is not"
                 " something Venice will take back.\n";
    return EXIT_FAILURE;
  }

  std::cerr << "\n-- leg 2: the tool result was accepted --\n";
  std::cout << second->content << '\n';
  return EXIT_SUCCESS;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const char* key = std::getenv("VENICE_API_KEY");
  if (key == nullptr || *key == '\0') {
    std::cerr << "VENICE_API_KEY not set; nothing to call (library links fine).\n";
    return EXIT_SUCCESS;
  }

  const venice::Client client{key};
  if (argc > 1 && std::string_view{argv[1]} == "--models")
    return list_models(client, argc > 2 ? argv[2] : "");
  if (argc > 1 && std::string_view{argv[1]} == "--characters")
    return list_characters(client, argc > 2 ? argv[2] : "");
  if (argc > 1 && std::string_view{argv[1]} == "--stream")
    return stream_report(client, argc > 2 ? argv[2]
                                          : "Think step by step: what is 17 * 23?");
  if (argc > 1 && std::string_view{argv[1]} == "--tools")
    return tools_report(client, argc > 2 ? argv[2] : "");

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
  return EXIT_SUCCESS;
}
