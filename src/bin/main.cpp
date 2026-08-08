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
  venice::ChatRequest req;
  req.model = "deepseek-r1-671b";  // a reasoning model, so thinking has somewhere to come from
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
  if (argc > 1 && std::string_view{argv[1]} == "--stream")
    return stream_report(client, argc > 2 ? argv[2]
                                          : "Think step by step: what is 17 * 23?");

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
