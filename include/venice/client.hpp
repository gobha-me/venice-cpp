#pragma once

// venice-cpp — header-only Venice API client (Phase 0).
//
// Transport: cpp-httplib over OpenSSL (HTTPS only — the API is TLS).
// Errors:   std::expected<T, venice::Error>; the client never throws across
//           its public API.
//
// Phase 0 surface (the ~20% everything else builds on):
//   * chat(req)                -> expected<ChatResponse>   (non-streaming)
//   * chat_stream(req, on_token) -> expected<ChatResponse> (SSE via callback)
//   * models()                 -> expected<vector<Model>>
//   * balance()                -> expected<json>           (rate-limit/balance)
//
// Not in Phase 0 (later phases, fed by real use): image/audio/video, TTS,
// embeddings, characters, retries/backoff, async.

#include <array>
#include <cmath>
#include <expected>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "venice/error.hpp"
#include "venice/types.hpp"

namespace venice {

namespace detail {

// ── query strings ─────────────────────────────────────────────────────────
//
// The client had no query encoder before VC-13 (#19): every Phase 0 endpoint
// was a bare path, and Client::path() joins the /api/v1 prefix to one without
// looking at it. These two sit at namespace scope rather than becoming private
// members of Client for the reason models_from_json_body did — a private helper
// is reachable only through a socket, while everything worth checking about
// these is a string transform. test/05query/ is the unit.

// Percent-encode per RFC 3986: the unreserved set (ALPHA / DIGIT / "-._~")
// passes through, every other byte becomes %XX.
//
// The cast to unsigned char is the whole point of this function. Plain `char`
// is signed on every platform this builds for, so a byte above 0x7F is
// *negative*, and both uses below then go wrong: the unreserved test compares
// against the wrong end of the range, and `byte >> 4` indexes kHex at a
// negative offset, reading whatever the linker put before the table.
//
// Dropping the cast was measured rather than imagined, and the three results
// are worth recording because they disagree:
//
//   * default build — silent. 0xFF encodes as "% F" and "é" as "%s3%t9".
//     Nothing throws; the output is merely wrong.
//   * UBSan — also silent. The read is out of bounds but not *undefined* in a
//     way -fsanitize=undefined models, so it reports nothing at all.
//   * ASan — catches it: "global-buffer-overflow ... in percent_encode".
//
// So the honest guard is a test asserting exact output (test/05query/ has
// them), with ASan as a second net that only fires in the sanitizer jobs. Note
// which check does *not* work: the property-shaped case there — every high byte
// encodes to three characters — stays green straight through the bug, because
// the length is right and only the bytes are wrong.
//
// Uppercase hex: RFC 3986 §2.1 says producers should, though both cases decode.
[[nodiscard]] inline auto percent_encode(std::string_view s) -> std::string {
  static constexpr std::string_view kHex = "0123456789ABCDEF";

  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    const auto byte = static_cast<unsigned char>(c);
    const bool unreserved = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                            (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' ||
                            byte == '_' || byte == '~';
    if (unreserved) {
      out.push_back(c);
    } else {
      out.push_back('%');
      out.push_back(kHex[byte >> 4U]);
      out.push_back(kHex[byte & 0x0FU]);
    }
  }
  return out;
}

using QueryParam = std::pair<std::string_view, std::string_view>;

// Append a query string to an endpoint path.
//
// A pair whose *value* is empty is skipped entirely, so a list in which nothing
// is set returns `path` untouched — not `path + "?"`. That skip is the
// non-breaking guarantee behind Client::models's new parameter, not a
// convenience: it is what keeps a no-argument models() byte-identical on the
// wire to every version before the parameter existed. test/05query/ asserts it
// rather than taking this paragraph's word for it.
//
// Keys are encoded as well as values. No caller passes a key that needs it, and
// that is precisely why it happens here — the alternative is an assumption
// waiting for the first caller who does.
[[nodiscard]] inline auto with_query(std::string_view path,
                                     std::initializer_list<QueryParam> params) -> std::string {
  std::string out{path};
  char sep = '?';
  for (const auto& [key, value] : params) {
    if (value.empty()) continue;
    out.push_back(sep);
    sep = '&';
    out += percent_encode(key);
    out.push_back('=');
    out += percent_encode(value);
  }
  return out;
}

}  // namespace detail

class Client {
 public:
  // api_key: Venice API key (Bearer). base_url defaults to the public API;
  // honor VENICE_BASE_URL-style overrides by passing an explicit value.
  explicit Client(std::string api_key,
                  std::string base_url = "https://api.venice.ai/api/v1")
      : m_api_key(std::move(api_key)), m_base_url(std::move(base_url)) {}

  // ── chat (non-streaming) ──────────────────────────────────────────────
  [[nodiscard]] auto chat(const ChatRequest& req) const -> std::expected<ChatResponse, Error> {
    if (auto ok = validate(req); !ok) return std::unexpected{std::move(ok.error())};

    auto res = post_json("/chat/completions", req.to_json_body(/*stream=*/false));
    if (!res) return std::unexpected{std::move(res.error())};

    try {
      return ChatResponse::from_json_body(*res);
    } catch (const std::exception& e) {
      return std::unexpected{Error{ErrorKind::Parse, 0, std::string{"chat parse: "} + e.what(), res->dump()}};
    }
  }

  // ── chat (streaming) ──────────────────────────────────────────────────
  // on_token is invoked with each content delta as it arrives. The full
  // assembled reply is returned at the end. Cancellation: return false from
  // on_token to abort the stream early (result is still returned with what
  // accumulated).
  [[nodiscard]] auto chat_stream(
      const ChatRequest& req,
      const std::function<bool(std::string_view /*delta*/)>& on_token) const
      -> std::expected<ChatResponse, Error> {
    if (auto ok = validate(req); !ok) return std::unexpected{std::move(ok.error())};

    const std::string payload = req.to_json_body(/*stream=*/true).dump();

    ChatResponse assembled;
    std::string leftover;  // partial SSE line buffer across chunks
    bool cancelled = false;

    auto cli = make_transport();
    httplib::Headers headers = auth_headers();
    headers.emplace("Accept", "text/event-stream");

    httplib::Request hreq;
    hreq.method = "POST";
    hreq.path = path("/chat/completions");
    hreq.headers = headers;
    hreq.body = payload;
    hreq.set_header("Content-Type", "application/json");

    std::string parse_err;

    hreq.content_receiver = [&](const char* data, size_t len, size_t /*off*/, uint64_t /*total*/) {
      leftover.append(data, len);
      // Process complete SSE lines ("data: ...\n\n").
      size_t pos;
      while ((pos = leftover.find("\n\n")) != std::string::npos) {
        std::string event = leftover.substr(0, pos);
        leftover.erase(0, pos + 2);
        for_each_data_line(event, [&](std::string_view line) {
          if (line == "[DONE]") return;
          try {
            const auto j = nlohmann::json::parse(line);
            if (j.contains("choices") && !j["choices"].empty()) {
              const auto& c0 = j["choices"][0];
              if (c0.contains("delta") && c0["delta"].contains("content") &&
                  !c0["delta"]["content"].is_null()) {
                const std::string delta = c0["delta"]["content"].get<std::string>();
                assembled.content += delta;
                if (on_token && !on_token(delta)) cancelled = true;
              }
              if (c0.contains("finish_reason") && !c0["finish_reason"].is_null())
                assembled.finish_reason = c0["finish_reason"].get<std::string>();
            }
            if (j.contains("model")) assembled.model = j["model"].get<std::string>();
            if (j.contains("id")) assembled.id = j["id"].get<std::string>();
            if (j.contains("usage") && !j["usage"].is_null())
              assembled.usage = j["usage"].get<Usage>();
          } catch (const std::exception& e) {
            if (parse_err.empty()) parse_err = e.what();
          }
        });
      }
      return !cancelled;  // false stops the transfer
    };

    httplib::Response hres;
    httplib::Error herr = httplib::Error::Success;
    const bool sent = cli.send(hreq, hres, herr);

    if (!sent && !cancelled)
      return std::unexpected{transport_error(herr)};
    if (cancelled) return assembled;  // deliberate early stop
    if (hres.status < 200 || hres.status >= 300)
      return std::unexpected{http_error(hres.status, hres.body)};
    if (!parse_err.empty() && assembled.content.empty())
      return std::unexpected{Error{ErrorKind::Parse, hres.status, "stream parse: " + parse_err, {}}};
    return assembled;
  }

  // ── models ────────────────────────────────────────────────────────────
  //
  // Only the shape of the *response* can fail here; individual entries degrade
  // instead. The parse itself is venice::models_from_json_body, deliberately
  // outside this class: everything interesting about it — junk entries, absent
  // fields, wrong-typed numbers — is reachable offline in test/04models/ only
  // because it needs no socket. This method is the transport half.
  //
  // `type` filters by modality (VC-13, #19). Empty — the default — sends no
  // query string at all, which is what every release before this one did and
  // what Venice reads as type=text: roughly a third of the models it serves.
  // The rest (image, video, tts, embedding, inpaint, music, asr, upscale) could
  // not be listed through this library at all until this parameter existed.
  // "all" returns every one of them.
  //
  // No exact counts here on purpose — the catalogue moved by a dozen models in
  // the ten days between VC-03 and VC-13, so a number in a comment is wrong
  // within the month. STATUS.md carries a dated snapshot instead.
  //
  // A caller-supplied string rather than an enum, on the same reasoning that
  // made response_format raw json: the value set belongs to Venice, and a list
  // hardcoded here goes stale the day a modality is added — refusing, from
  // inside the client, a value the server would have accepted. An unrecognised
  // type is the server's 400 to give (AGENTS.md, "range checking: none").
  //
  // The typed surface stays the text shape (VC-03). A non-text entry parses
  // with most fields absent and keeps its type-specific keys in Model::raw;
  // test/04models/ pins that with a captured image entry.
  [[nodiscard]] auto models(std::string_view type = {}) const
      -> std::expected<std::vector<Model>, Error> {
    auto res = get_json(detail::with_query("/models", {{"type", type}}));
    if (!res) return std::unexpected{std::move(res.error())};
    try {
      return models_from_json_body(*res);
    } catch (const std::exception& e) {
      return std::unexpected{Error{ErrorKind::Parse, 0, std::string{"models parse: "} + e.what(), res->dump()}};
    }
  }

  // ── balance / rate limits ─────────────────────────────────────────────
  [[nodiscard]] auto balance() const -> std::expected<nlohmann::json, Error> {
    return get_json("/api_keys/rate_limits");
  }

  [[nodiscard]] auto base_url() const noexcept -> const std::string& { return m_base_url; }

 private:
  std::string m_api_key;
  std::string m_base_url;

  // ── preconditions ─────────────────────────────────────────────────────
  //
  // Everything both chat entry points refuse to send, in one place. A caller
  // that trips any of these has touched no socket: this runs before
  // make_transport().
  //
  // Non-finite doubles belong here rather than under "range checking, none
  // deliberately". JSON has no NaN and no infinity, so nlohmann's dump()
  // collapses such a value to null and Venice answers with a 400 that will
  // never mention NaN — the caller gets a confusing rejection for a bug in
  // their own arithmetic. That is unsendable *by construction*, which is the
  // line AGENTS.md draws for InvalidArg, not an opinion about what range
  // Venice accepts (VC-10).
  //
  // The sweep runs *before* the emptiness checks, and that ordering is load
  // bearing rather than cosmetic. Every failure case is offline either way,
  // but the passing path is not: a valid request would go to api.venice.ai and
  // the assertion would depend on whether the runner has a network. Checking
  // finiteness first means a finite request with an empty model comes back
  // "model is empty" — a message reachable only if the sweep ran and let the
  // values through. That is how test/03guards/ proves the accept path without
  // opening a connection, and the ordering itself is pinned there by a
  // precedence case — reorder these two blocks and it goes red.
  //
  // Modeled fields only. `extra` is documented verbatim passthrough and is not
  // walked: validating an arbitrary json tree per call is the exact cost VC-11
  // just removed, and "something in extra is not finite" would be no more
  // actionable than the 400 it replaces.
  [[nodiscard]] static auto validate(const ChatRequest& req) -> std::expected<void, Error> {
    // Pointer-to-member, so name and field travel together and a future double
    // field is one line. Members rather than references into `req`: the table
    // is then a compile-time constant with no lifetime relationship to any
    // request. test/03guards/ mirrors this list on purpose — a fifth field
    // added here and not there ships unguarded with the suite still green.
    struct DoubleField {
      std::optional<double> ChatRequest::*field;
      std::string_view name;
    };
    static constexpr std::array<DoubleField, 4> kDoubleFields{{
        {&ChatRequest::temperature, "temperature"},
        {&ChatRequest::top_p, "top_p"},
        {&ChatRequest::frequency_penalty, "frequency_penalty"},
        {&ChatRequest::presence_penalty, "presence_penalty"},
    }};

    for (const auto& [field, name] : kDoubleFields) {
      const auto& value = req.*field;
      if (value && !std::isfinite(*value))
        return std::unexpected{
            Error{ErrorKind::InvalidArg, 0, std::string{name} + " is not finite", {}}};
    }

    if (req.model.empty())
      return std::unexpected{Error{ErrorKind::InvalidArg, 0, "model is empty", {}}};
    if (req.messages.empty())
      return std::unexpected{Error{ErrorKind::InvalidArg, 0, "messages is empty", {}}};

    return {};
  }

  // Split base_url into scheme://host and the /api/v1 path prefix.
  [[nodiscard]] auto host() const -> std::string {
    const auto pos = m_base_url.find("/api/");
    return pos == std::string::npos ? m_base_url : m_base_url.substr(0, pos);
  }
  [[nodiscard]] auto path(std::string_view endpoint) const -> std::string {
    const auto pos = m_base_url.find("/api/");
    const std::string prefix = pos == std::string::npos ? "" : m_base_url.substr(pos);
    return prefix + std::string{endpoint};
  }

  [[nodiscard]] auto make_transport() const -> httplib::Client {
    httplib::Client cli{host()};
    cli.set_bearer_token_auth(m_api_key);
    cli.set_follow_location(true);
    cli.set_read_timeout(300, 0);
    cli.set_connection_timeout(30, 0);
    return cli;
  }

  [[nodiscard]] auto auth_headers() const -> httplib::Headers {
    return {{"Authorization", "Bearer " + m_api_key}};
  }

  [[nodiscard]] auto get_json(std::string_view endpoint) const -> std::expected<nlohmann::json, Error> {
    auto cli = make_transport();
    auto res = cli.Get(path(endpoint), auth_headers());
    if (!res) return std::unexpected{transport_error(res.error())};
    return decode_json(res->status, res->body);
  }

  [[nodiscard]] auto post_json(std::string_view endpoint, const nlohmann::json& body) const
      -> std::expected<nlohmann::json, Error> {
    auto cli = make_transport();
    auto res = cli.Post(path(endpoint), auth_headers(), body.dump(), "application/json");
    if (!res) return std::unexpected{transport_error(res.error())};
    return decode_json(res->status, res->body);
  }

  [[nodiscard]] static auto decode_json(int status, const std::string& body)
      -> std::expected<nlohmann::json, Error> {
    if (status < 200 || status >= 300)
      return std::unexpected{http_error(status, body)};
    try {
      return nlohmann::json::parse(body);
    } catch (const std::exception& e) {
      return std::unexpected{Error{ErrorKind::Parse, status, std::string{"json parse: "} + e.what(), body}};
    }
  }

  [[nodiscard]] static auto http_error(int status, const std::string& body) -> Error {
    return Error{kind_for_status(status), status, "HTTP " + std::to_string(status), body};
  }

  [[nodiscard]] static auto transport_error(httplib::Error e) -> Error {
    return Error{ErrorKind::Network, 0, "transport: " + httplib::to_string(e), {}};
  }

  // Invoke fn for each "data:" payload in an SSE event block.
  static void for_each_data_line(const std::string& event, const std::function<void(std::string_view)>& fn) {
    size_t start = 0;
    while (start < event.size()) {
      const auto nl = event.find('\n', start);
      const std::string_view line = nl == std::string::npos
                                        ? std::string_view{event}.substr(start)
                                        : std::string_view{event}.substr(start, nl - start);
      if (line.starts_with("data:")) {
        auto payload = line.substr(5);
        if (!payload.empty() && payload.front() == ' ') payload.remove_prefix(1);
        fn(payload);
      }
      if (nl == std::string::npos) break;
      start = nl + 1;
    }
  }
};

}  // namespace venice
