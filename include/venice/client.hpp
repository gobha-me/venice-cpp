#pragma once

// venice-cpp — header-only Venice API client (Phase 0).
//
// Transport: cpp-httplib over OpenSSL (HTTPS only — the API is TLS).
// Errors:   std::expected<T, venice::Error>; the client never throws across
//           its public API.
//
// Surface:
//   * chat(req)                     -> expected<ChatResponse>   (non-streaming)
//   * chat_stream(req, on_token)    -> expected<ChatResponse>   (content text)
//   * chat_stream(req, acc[, on_delta])                         (structured)
//   * models()                      -> expected<vector<Model>>
//   * characters(query)             -> expected<CharacterPage>
//   * character(slug)               -> expected<Character>
//   * character_reviews(slug, query)-> expected<CharacterReviewPage>
//   * balance()                     -> expected<json>           (rate-limit/balance)
//
// A ChatResponse carries the whole assistant turn as a Message, so a reply can
// be appended to the next request's messages and nothing is lost — thinking and
// tool calls included (VC-05/VC-14). The streaming forms assemble into the same
// Message; see venice/stream.hpp.
//
// Every one of them takes a trailing venice::RequestOptions (defaulted) for
// per-call timeouts, cancellation and authentication override — see
// venice/options.hpp (VC-06/VC-23).
//
// Not in Phase 0 (later phases, fed by real use): image/audio/video, TTS,
// embeddings, retries/backoff, async.

#include <array>
#include <cctype>
#include <cmath>
#include <expected>
#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "venice/error.hpp"
#include "venice/options.hpp"
#include "venice/stream.hpp"
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

// A path segment and a query component use the same RFC 3986 unreserved set,
// but they are different contracts at the call site. The distinct spelling
// makes any future query-specific encoding change confront the path contract
// explicitly. The implementation stays shared today so the signed-char and
// uppercase-hex fixes above cannot drift into two copies.
[[nodiscard]] inline auto path_segment_encode(std::string_view segment) -> std::string {
  return percent_encode(segment);
}

[[nodiscard]] inline auto with_path_segment(std::string_view path,
                                            std::string_view segment) -> std::string {
  std::string out{path};
  out.push_back('/');
  out += path_segment_encode(segment);
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
//
// The skip lives here, in one function, rather than in each with_query overload
// below. Two copies of it is exactly the drift that would let one endpoint keep
// the byte-identical guarantee while another quietly loses it.
inline void append_param(std::string& out, char& sep, std::string_view key,
                         std::string_view value) {
  if (value.empty()) return;
  out.push_back(sep);
  sep = '&';
  out += percent_encode(key);
  out.push_back('=');
  out += percent_encode(value);
}

[[nodiscard]] inline auto with_query(std::string_view path,
                                     std::initializer_list<QueryParam> params) -> std::string {
  std::string out{path};
  char sep = '?';
  for (const auto& [key, value] : params) append_param(out, sep, key, value);
  return out;
}

// The owning overload, for a query built at runtime rather than spelled out at
// the call site (VC-04, #5: venice::character_query_params).
//
// It takes owned strings and not QueryParam for a lifetime reason, not a
// stylistic one: a CharacterQuery's limit becomes a string only via
// std::to_string, and a QueryParam built from that temporary holds a
// string_view into a buffer that is already gone. Making the caller own the
// values is what makes that unrepresentable.
[[nodiscard]] inline auto with_query(std::string_view path,
                                     std::span<const std::pair<std::string, std::string>> params)
    -> std::string {
  std::string out{path};
  char sep = '?';
  for (const auto& [key, value] : params) append_param(out, sep, key, value);
  return out;
}

// ── buffered HTTP substrate (VC-22, #37) ─────────────────────────────────
//
// Endpoint methods stay typed. This is deliberately under detail rather than a
// public Client::send escape hatch: it is the one internal vocabulary every
// buffered endpoint uses for method, headers, body and response metadata.
// Keeping it at namespace scope makes the transport contract fixture-testable
// without adding a fake public endpoint solely for tests.

enum class HttpMethod { Get, Post, Patch, Delete };

struct ByteBody {
  // std::string is the byte container cpp-httplib accepts. Its size, rather
  // than a terminating NUL, is authoritative, so arbitrary binary data is
  // preserved without a text conversion.
  std::string bytes = {};
  std::string content_type = {};
};

struct MultipartPart {
  std::string name = {};
  std::string bytes = {};
  std::string filename = {};
  std::string content_type = {};
};

struct MultipartBody {
  std::vector<MultipartPart> parts = {};
};

using BufferedBody = std::variant<std::monostate, ByteBody, MultipartBody>;

struct BufferedRequest {
  HttpMethod method = HttpMethod::Get;
  std::string endpoint = {};
  httplib::Headers headers = {};
  BufferedBody body = std::monostate{};
};

struct BufferedResponse {
  int status = 0;
  httplib::Headers headers = {};
  // Normalized type/subtype only: lower-case, surrounding whitespace and
  // parameters removed. The verbatim header remains in headers.
  std::string content_type = {};
  std::string body = {};
};

struct JsonResponse {
  int status = 0;
  nlohmann::json body = {};
  std::string raw_body = {};
  ResponseMetadata metadata = {};
};

enum class AuthPolicy { PublicOrBearer, BearerOnly, BearerOrSignInWithX };

[[nodiscard]] inline auto authentication_headers(const Authentication& authentication)
    -> std::expected<httplib::Headers, Error> {
  const auto invalid = [](std::string_view name) {
    return std::unexpected{
        Error{ErrorKind::InvalidArg, 0, std::string{name} + " credential is empty", {}}};
  };

  switch (authentication.kind()) {
    case AuthenticationKind::Public: return httplib::Headers{};
    case AuthenticationKind::Bearer:
      if (authentication.value().empty()) return invalid("Bearer");
      return httplib::Headers{{"Authorization", "Bearer " + std::string{authentication.value()}}};
    case AuthenticationKind::SignInWithX:
      if (authentication.value().empty()) return invalid("Sign-In-With-X");
      return httplib::Headers{{"SIGN-IN-WITH-X", std::string{authentication.value()}}};
    case AuthenticationKind::X402Payment:
      if (authentication.value().empty()) return invalid("x402 payment");
      return httplib::Headers{{"PAYMENT-SIGNATURE", std::string{authentication.value()}}};
  }
  return std::unexpected{Error{ErrorKind::InvalidArg, 0, "unknown authentication mode", {}}};
}

[[nodiscard]] inline auto authentication_allowed(AuthenticationKind kind, AuthPolicy policy)
    -> bool {
  switch (policy) {
    case AuthPolicy::PublicOrBearer:
      return kind == AuthenticationKind::Public || kind == AuthenticationKind::Bearer;
    case AuthPolicy::BearerOnly: return kind == AuthenticationKind::Bearer;
    case AuthPolicy::BearerOrSignInWithX:
      return kind == AuthenticationKind::Bearer || kind == AuthenticationKind::SignInWithX;
  }
  return false;
}

[[nodiscard]] inline auto auth_policy_name(AuthPolicy policy) -> std::string_view {
  switch (policy) {
    case AuthPolicy::PublicOrBearer: return "public or Bearer";
    case AuthPolicy::BearerOnly: return "Bearer";
    case AuthPolicy::BearerOrSignInWithX: return "Bearer or Sign-In-With-X";
  }
  return "supported";
}

[[nodiscard]] inline auto host_from_base_url(std::string_view base_url) -> std::string {
  const auto pos = base_url.find("/api/");
  return pos == std::string_view::npos ? std::string{base_url} : std::string{base_url.substr(0, pos)};
}

[[nodiscard]] inline auto request_path(std::string_view base_url, std::string_view endpoint)
    -> std::string {
  const auto pos = base_url.find("/api/");
  const auto prefix = pos == std::string_view::npos ? std::string_view{} : base_url.substr(pos);
  return std::string{prefix} + std::string{endpoint};
}

[[nodiscard]] inline auto make_transport(std::string_view base_url, const RequestOptions& opts)
    -> httplib::Client {
  httplib::Client cli{host_from_base_url(base_url)};
  cli.set_follow_location(true);
  cli.set_read_timeout(opts.read_timeout.value_or(std::chrono::seconds{300}));
  cli.set_connection_timeout(opts.connect_timeout.value_or(std::chrono::seconds{30}));
  if (opts.write_timeout) cli.set_write_timeout(*opts.write_timeout);
  return cli;
}

[[nodiscard]] inline auto transport_error(httplib::Error error) -> Error {
  return Error{ErrorKind::Network, 0, "transport: " + httplib::to_string(error), {}};
}

[[nodiscard]] inline auto cancel_error() -> Error {
  return Error{ErrorKind::Cancelled, 0, "cancelled by caller", {}};
}

[[nodiscard]] inline auto http_error(int status, const std::string& body,
                                     ResponseMetadata metadata = {}) -> Error {
  return Error{kind_for_status(status), status, "HTTP " + std::to_string(status), body,
               std::move(metadata)};
}

[[nodiscard]] inline auto normalize_media_type(std::string_view value) -> std::string {
  value = value.substr(0, value.find(';'));
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
    value.remove_prefix(1);
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
    value.remove_suffix(1);

  std::string normalized;
  normalized.reserve(value.size());
  for (const char c : value)
    normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return normalized;
}

[[nodiscard]] inline auto response_from_httplib(httplib::Response response) -> BufferedResponse {
  std::string content_type;
  if (const auto it = response.headers.find("Content-Type"); it != response.headers.end())
    content_type = normalize_media_type(it->second);
  return BufferedResponse{response.status, std::move(response.headers), std::move(content_type),
                          std::move(response.body)};
}

[[nodiscard]] inline auto metadata_from_headers(const httplib::Headers& headers)
    -> ResponseMetadata {
  ResponseMetadata metadata;
  metadata.headers.reserve(headers.size());
  for (const auto& [name, value] : headers) metadata.headers.emplace_back(name, value);
  metadata.x_balance_remaining = metadata.header("X-Balance-Remaining");
  metadata.payment_required = metadata.header("PAYMENT-REQUIRED");
  metadata.payment_response = metadata.header("PAYMENT-RESPONSE");
  return metadata;
}

[[nodiscard]] inline auto method_name(HttpMethod method) -> const char* {
  switch (method) {
    case HttpMethod::Get: return "GET";
    case HttpMethod::Post: return "POST";
    case HttpMethod::Patch: return "PATCH";
    case HttpMethod::Delete: return "DELETE";
  }
  return "GET";
}

[[nodiscard]] inline auto send_buffered(std::string_view base_url, const BufferedRequest& request,
                                        const RequestOptions& opts = {})
    -> std::expected<BufferedResponse, Error> {
  // Every audited multipart operation is POST. Encoding through cpp-httplib's
  // public multipart API keeps boundary construction in the transport library;
  // rejecting any other method here prevents an internal caller from silently
  // sending a different body shape.
  if (std::holds_alternative<MultipartBody>(request.body) && request.method != HttpMethod::Post)
    return std::unexpected{
        Error{ErrorKind::InvalidArg, 0, "multipart requests require POST", {}}};

  auto cli = make_transport(base_url, opts);
  // Declared after cli: reverse destruction joins the watcher while the client
  // it may stop is still alive. This is the same ordering as the SSE path.
  const CancelGuard guard{opts.cancel, cli};
  if (guard.cancelled()) return std::unexpected{cancel_error()};

  httplib::Response response;
  httplib::Error transport_status = httplib::Error::Success;
  bool sent = false;

  if (const auto* multipart = std::get_if<MultipartBody>(&request.body)) {
    httplib::MultipartFormDataItems items;
    items.reserve(multipart->parts.size());
    for (const auto& part : multipart->parts)
      items.push_back({part.name, part.bytes, part.filename, part.content_type});

    auto result = cli.Post(request_path(base_url, request.endpoint), request.headers, items);
    transport_status = result.error();
    sent = static_cast<bool>(result);
    if (sent) response = std::move(result.value());
  } else {
    httplib::Request wire_request;
    wire_request.method = method_name(request.method);
    wire_request.path = request_path(base_url, request.endpoint);
    wire_request.headers = request.headers;
    if (const auto* bytes = std::get_if<ByteBody>(&request.body)) {
      wire_request.body = bytes->bytes;
      wire_request.headers.erase("Content-Type");
      wire_request.set_header("Content-Type", bytes->content_type);
    }
    sent = cli.send(wire_request, response, transport_status);
  }

  // Cancellation is tested before the transport result because stop() makes a
  // caller cancellation arrive from cpp-httplib as a failed socket operation.
  if (guard.cancelled()) return std::unexpected{cancel_error()};
  if (!sent) return std::unexpected{transport_error(transport_status)};
  return response_from_httplib(std::move(response));
}

[[nodiscard]] inline auto is_json_media_type(std::string_view content_type) -> bool {
  if (content_type == "application/json") return true;
  constexpr std::string_view kPrefix = "application/";
  constexpr std::string_view kSuffix = "+json";
  return content_type.starts_with(kPrefix) && content_type.ends_with(kSuffix) &&
         content_type.size() > kPrefix.size() + kSuffix.size();
}

[[nodiscard]] inline auto require_media_type(const BufferedResponse& response,
                                             std::span<const std::string_view> allowed)
    -> std::expected<void, Error> {
  if (response.status < 200 || response.status >= 300)
    return std::unexpected{
        http_error(response.status, response.body, metadata_from_headers(response.headers))};

  for (const auto candidate : allowed) {
    if (response.content_type == normalize_media_type(candidate)) return {};
  }

  const std::string actual = response.content_type.empty() ? "<missing>" : response.content_type;
  return std::unexpected{Error{ErrorKind::Parse, response.status,
                               "unexpected response content type: " + actual, response.body,
                               metadata_from_headers(response.headers)}};
}

[[nodiscard]] inline auto decode_json(const BufferedResponse& response)
    -> std::expected<nlohmann::json, Error> {
  if (response.status < 200 || response.status >= 300)
    return std::unexpected{
        http_error(response.status, response.body, metadata_from_headers(response.headers))};
  if (!is_json_media_type(response.content_type)) {
    const std::string actual = response.content_type.empty() ? "<missing>" : response.content_type;
    return std::unexpected{Error{ErrorKind::Parse, response.status,
                                 "expected JSON response, got " + actual, response.body,
                                 metadata_from_headers(response.headers)}};
  }
  try {
    return nlohmann::json::parse(response.body);
  } catch (const std::exception& e) {
    return std::unexpected{Error{ErrorKind::Parse, response.status,
                                 std::string{"json parse: "} + e.what(), response.body,
                                 metadata_from_headers(response.headers)}};
  }
}

[[nodiscard]] inline auto decode_json_response(const BufferedResponse& response)
    -> std::expected<JsonResponse, Error> {
  auto body = decode_json(response);
  if (!body) return std::unexpected{std::move(body.error())};
  return JsonResponse{response.status, std::move(*body), response.body,
                      metadata_from_headers(response.headers)};
}

}  // namespace detail

class Client {
 public:
  // The string constructor remains the source-compatible Bearer spelling.
  // Public and wallet-authenticated clients use the explicit Authentication
  // overload so an empty Bearer is never confused with no authentication.
  explicit Client(std::string api_key,
                  std::string base_url = "https://api.venice.ai/api/v1")
      : Client(Authentication::bearer(std::move(api_key)), std::move(base_url)) {}

  explicit Client(Authentication authentication,
                  std::string base_url = "https://api.venice.ai/api/v1")
      : m_authentication(std::move(authentication)), m_base_url(std::move(base_url)) {}

  // ── chat (non-streaming) ──────────────────────────────────────────────
  //
  // `opts` bounds the call and can abort it from another thread; a request
  // aborted that way comes back ErrorKind::Cancelled, never a partial response.
  // See venice/options.hpp.
  [[nodiscard]] auto chat(const ChatRequest& req, const RequestOptions& opts = {}) const
      -> std::expected<ChatResponse, Error> {
    if (auto ok = validate(req); !ok) return std::unexpected{std::move(ok.error())};

    auto res = post_json_response("/chat/completions", req.to_json_body(/*stream=*/false),
                                  detail::AuthPolicy::BearerOrSignInWithX, opts);
    if (!res) return std::unexpected{std::move(res.error())};

    try {
      auto response = ChatResponse::from_json_body(res->body);
      response.metadata = std::move(res->metadata);
      return response;
    } catch (const std::exception& e) {
      return std::unexpected{Error{ErrorKind::Parse, res->status,
                                   std::string{"chat parse: "} + e.what(), res->raw_body,
                                   std::move(res->metadata)}};
    }
  }

  // ── chat (streaming) ──────────────────────────────────────────────────
  // on_token is invoked with each content delta as it arrives. The full
  // assembled reply is returned at the end.
  //
  // There are two ways to stop a stream and they mean different things:
  //
  //   * return false from on_token — a deliberate early stop. The caller has
  //     what it wanted; the partial ChatResponse is returned as success. This
  //     is the original Phase 0 behaviour and is unchanged.
  //   * fire opts.cancel — the caller is abandoning the call. Returns
  //     ErrorKind::Cancelled and no response, because a caller that has stopped
  //     caring should not have to distinguish "the assembled text so far" from
  //     "the answer".
  //
  // The second exists because the first cannot cover the cases that matter:
  // on_token only runs when a *content* delta arrives, so a stop wanted before
  // the first delta, during a gap between frames, or while the server stalls
  // after headers is invisible to it and waits out the read timeout. That is
  // the gap #7 was filed for.
  [[nodiscard]] auto chat_stream(
      const ChatRequest& req,
      const std::function<bool(std::string_view /*delta*/)>& on_token,
      const RequestOptions& opts = {}) const -> std::expected<ChatResponse, Error> {
    // A thin adapter over the accumulator overload, and byte-identical in
    // behaviour: on_token fires exactly when a chunk carried a `content` key
    // that was a string — which is why StreamDelta::content is an optional
    // rather than a plain view. An empty-string content delta still calls back,
    // as it always did.
    StreamAccumulator acc{/*keep_chunks=*/false};
    return chat_stream(req, acc, [&](const StreamDelta& d) {
      if (!d.content) return true;
      return on_token ? on_token(*d.content) : true;
    }, opts);
  }

  // ── chat (streaming, structured) ──────────────────────────────────────
  //
  // The rich form. `acc` is the caller's storage and is the reason there is no
  // callback-only overload of this shape: a cancelled stream returns
  // ErrorKind::Cancelled and no response — VC-06 settled that and it is right —
  // but the accumulator is the caller's own object, so cancelling no longer
  // destroys what arrived. The return value says "you abandoned this"; `acc`
  // still holds every token, every thought, and every chunk.
  //
  // It also removes an ambiguity rather than adding one. A bare
  // `bool(const StreamDelta&)` overload beside the string_view one makes
  // `chat_stream(req, [](auto d){…})` and `chat_stream(req, nullptr, opts)` hard
  // errors — a source break for code that compiles today. With the accumulator
  // in position 2, every arity resolves.
  //
  // `acc` is reset only after validate() passes, so a rejected request does not
  // wipe data the caller already had.
  [[nodiscard]] auto chat_stream(const ChatRequest& req, StreamAccumulator& acc,
                                 const std::function<bool(const StreamDelta&)>& on_delta,
                                 const RequestOptions& opts = {}) const
      -> std::expected<ChatResponse, Error> {
    if (auto ok = validate(req); !ok) return std::unexpected{std::move(ok.error())};
    auto headers = request_headers(detail::AuthPolicy::BearerOrSignInWithX, opts);
    if (!headers) return std::unexpected{std::move(headers.error())};
    acc.reset();

    const std::string payload = req.to_json_body(/*stream=*/true).dump();

    bool early_stop = false;  // on_delta said stop — NOT opts.cancel; see above
    bool response_is_success = false;
    std::string error_body;
    std::string parse_err;
    detail::SseFramer framer;
    std::vector<ToolCall> frags;  // backing store for the span in each delta

    auto cli = detail::make_transport(m_base_url, opts);
    // Declared after cli and never before: the guard's watcher thread holds a
    // reference to it, and destruction runs in reverse, so this ordering is what
    // guarantees the thread is joined while cli is still alive.
    const detail::CancelGuard guard{opts.cancel, cli};
    if (guard.cancelled()) return std::unexpected{detail::cancel_error()};

    headers->emplace("Accept", "text/event-stream");

    httplib::Request hreq;
    hreq.method = "POST";
    hreq.path = detail::request_path(m_base_url, "/chat/completions");
    hreq.headers = *headers;
    hreq.body = payload;
    hreq.set_header("Content-Type", "application/json");
    hreq.response_handler = [&](const httplib::Response& response) {
      response_is_success = response.status >= 200 && response.status < 300;
      return true;
    };

    // One SSE payload -> one delta -> the accumulator, then the observer.
    // Ingest happens before the callback on purpose: a callback that stops the
    // stream must not cost the caller the frame that made it decide to.
    const auto on_payload = [&](std::string_view line) {
      if (early_stop) return;  // stop at the frame, not at the end of the chunk
      if (line == "[DONE]") return;
      try {
        const auto j = nlohmann::json::parse(line);
        frags.clear();
        const auto d = delta_from_chunk(j, frags);
        acc.note_envelope(j);
        acc.ingest(d);
        if (on_delta && !on_delta(d)) early_stop = true;
      } catch (const std::exception& e) {
        if (parse_err.empty()) parse_err = e.what();
      }
    };

    hreq.content_receiver = [&](const char* data, size_t len, size_t /*off*/, uint64_t /*total*/) {
      if (!response_is_success) {
        error_body.append(data, len);
        return true;
      }
      framer.feed(std::string_view{data, len}, on_payload);
      return !early_stop;  // false stops the transfer
    };

    httplib::Response hres;
    httplib::Error herr = httplib::Error::Success;
    const bool sent = cli.send(hreq, hres, herr);

    // Flush the tail. A final event not terminated by a blank line used to be
    // dropped on the floor here, and it is frequently the usage frame — so the
    // loss was a billing bug. Not flushed after an early stop or a cancel:
    // both mean nobody wants more frames.
    if (sent && !early_stop && !guard.cancelled()) framer.finish(on_payload);

    auto metadata = detail::metadata_from_headers(hres.headers);
    const auto assembled_response = [&] {
      auto response = acc.response();
      response.metadata = metadata;
      return response;
    };

    // The token is tested first, ahead of both the transport error and the
    // early stop. It has to be: a cancel is delivered by shutting the socket
    // down, so it *arrives* as a transport failure and would otherwise be
    // reported as a dead network. Ahead of early_stop as well, because a cancel
    // racing an on_delta stop is a caller who has abandoned the call — reading
    // that as "here is your partial answer" would hand back a response nobody
    // is waiting for. What `acc` holds is unaffected either way, which is the
    // whole point of it belonging to the caller.
    if (guard.cancelled()) return std::unexpected{detail::cancel_error()};
    if (!sent && !early_stop)
      return std::unexpected{detail::transport_error(herr)};
    if (early_stop) return assembled_response();  // deliberate early stop
    if (hres.status < 200 || hres.status >= 300)
      return std::unexpected{
          detail::http_error(hres.status, error_body, std::move(metadata))};
    // "nothing arrived at all", not "no content arrived". The old test was
    // assembled.content.empty(), which reported ErrorKind::Parse on a
    // reasoning-only stream that had in fact been received perfectly.
    if (!parse_err.empty() && acc.empty())
      return std::unexpected{Error{ErrorKind::Parse, hres.status, "stream parse: " + parse_err,
                                   {}, std::move(metadata)}};
    return assembled_response();
  }

  // Accumulate with no observer, for a caller that only wants the storage.
  [[nodiscard]] auto chat_stream(const ChatRequest& req, StreamAccumulator& acc,
                                 const RequestOptions& opts = {}) const
      -> std::expected<ChatResponse, Error> {
    return chat_stream(req, acc, std::function<bool(const StreamDelta&)>{}, opts);
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
  [[nodiscard]] auto models(std::string_view type = {}, const RequestOptions& opts = {}) const
      -> std::expected<std::vector<Model>, Error> {
    auto res = get_json_response(detail::with_query("/models", {{"type", type}}),
                                 detail::AuthPolicy::PublicOrBearer, opts);
    if (!res) return std::unexpected{std::move(res.error())};
    try {
      return models_from_json_body(res->body);
    } catch (const std::exception& e) {
      return std::unexpected{Error{ErrorKind::Parse, res->status,
                                   std::string{"models parse: "} + e.what(), res->raw_body,
                                   std::move(res->metadata)}};
    }
  }

  // ── characters ────────────────────────────────────────────────────────
  //
  // The discovery half of `venice_parameters.character_slug` (VC-04, #5). Same
  // division of labour as models(): this is the transport, the parse is
  // venice::characters_from_json_body and the query build is
  // venice::character_query_params, both free so test/08characters/ can reach
  // the whole failure matrix without a socket.
  //
  // A default-constructed query sends the bare /characters, byte-identical to
  // what a no-argument call would have sent if this type did not exist.
  //
  // **The endpoint pages, and the default page is 50.** It caps at 100, so
  // listing everything means asking for the next page until a short one comes
  // back:
  //
  //   venice::CharacterQuery q;
  //   q.limit = 100;
  //   for (q.offset = 0; ; *q.offset += 100) {
  //     const auto page = client.characters(q);
  //     if (!page || page->returned < 100) break;
  //   }
  //
  // `page->returned`, not `page->entries.size()`. They differ exactly when an
  // entry was skipped for want of a slug, and comparing the *usable* count
  // against the limit would then end the walk one page in and call a truncated
  // catalogue complete. That is why this returns a CharacterPage rather than
  // the vector the ticket asked for: a bare vector cannot express the
  // difference, so no caller could write this loop correctly.
  //
  // Not wrapped in an all-pages helper here: that loop needs a policy for a
  // failed page mid-walk — abandon, retry, or return what it has — and every
  // answer is wrong for someone. Retries are a later phase (AGENTS.md).
  //
  // Unlike /models, this endpoint is Bearer-only. A Public, SIWX or x402
  // payment selection is rejected before transport; 401/403 responses are
  // Auth and a server-side 402 is PaymentRequired (VC-23, #38 / VC-15, #25).
  //
  // Venice documents this as a preview API that may change, which is why
  // Character::raw and CharacterPage::raw matter more here than elsewhere.
  [[nodiscard]] auto characters(const CharacterQuery& query = {},
                                const RequestOptions& opts = {}) const
      -> std::expected<CharacterPage, Error> {
    const auto params = character_query_params(query);
    auto res = get_json_response(detail::with_query("/characters", params),
                                 detail::AuthPolicy::BearerOnly, opts);
    if (!res) return std::unexpected{std::move(res.error())};
    try {
      return characters_from_json_body(res->body);
    } catch (const std::exception& e) {
      return std::unexpected{Error{ErrorKind::Parse, res->status,
                                   std::string{"characters parse: "} + e.what(), res->raw_body,
                                   std::move(res->metadata)}};
    }
  }

  // Fetch one character by the slug a listing or stored configuration carries
  // (VC-16, #26). The slug is one path segment, never a path fragment: a slash
  // or other reserved byte is percent-encoded before transport, so caller data
  // cannot turn this into the reviews endpoint or any other route.
  //
  // Empty is structural invalidity rather than server-owned value policy. It
  // would produce /characters/, a different endpoint, so reject it before even
  // resolving authentication. Like the listing, this preview endpoint is
  // Bearer-only and the response retains every unmodeled field in Character::raw.
  [[nodiscard]] auto character(std::string_view slug,
                               const RequestOptions& opts = {}) const
      -> std::expected<Character, Error> {
    if (slug.empty())
      return std::unexpected{
          Error{ErrorKind::InvalidArg, 0, "character slug must not be empty", {}}};

    auto res = get_json_response(detail::with_path_segment("/characters", slug),
                                 detail::AuthPolicy::BearerOnly, opts);
    if (!res) return std::unexpected{std::move(res.error())};
    try {
      return character_from_json_body(res->body);
    } catch (const std::exception& e) {
      return std::unexpected{Error{ErrorKind::Parse, res->status,
                                   std::string{"character parse: "} + e.what(), res->raw_body,
                                   std::move(res->metadata)}};
    }
  }

  // The reviews behind a character's rating (VC-36, #56), and the operation
  // that completes the family. CharacterStats has reported averageRating since
  // VC-04 with no way to read a single review it averaged.
  //
  // Slug handling is character()'s exactly — one encoded path segment, empty
  // rejected before auth or transport — and it has to be, because this path has
  // something *after* the segment. An unencoded slash here would not merely
  // reach a different character; `a/b` would address
  // /characters/a/b/reviews, which is not this operation at all.
  //
  // The query is a separate type from CharacterQuery on purpose: this endpoint
  // pages by page/pageSize and the listing by offset/limit (see
  // CharacterReviewQuery). A default-constructed query sends the bare path with
  // no query string.
  //
  // Paging is honest here in a way characters() cannot be, because the response
  // carries pagination:
  //
  //   venice::CharacterReviewQuery q;
  //   q.page = 1;
  //   for (;;) {
  //     const auto page = client.character_reviews("alan-watts", q);
  //     if (!page) break;                                  // inspect the error
  //     for (const auto& r : page->entries) use(r);
  //     if (!page->pagination || !page->pagination->total_pages) break;
  //     if (*q.page >= *page->pagination->total_pages) break;
  //     ++*q.page;
  //   }
  //
  // Bearer-only, like the rest of the family, and the same preview-API caveat:
  // CharacterReview::raw and CharacterReviewPage::raw are where anything this
  // client does not model survives.
  [[nodiscard]] auto character_reviews(std::string_view slug,
                                       const CharacterReviewQuery& query = {},
                                       const RequestOptions& opts = {}) const
      -> std::expected<CharacterReviewPage, Error> {
    if (slug.empty())
      return std::unexpected{
          Error{ErrorKind::InvalidArg, 0, "character slug must not be empty", {}}};

    const auto params = character_review_query_params(query);
    const auto path = detail::with_path_segment("/characters", slug) + "/reviews";
    auto res = get_json_response(detail::with_query(path, params),
                                 detail::AuthPolicy::BearerOnly, opts);
    if (!res) return std::unexpected{std::move(res.error())};
    try {
      return character_reviews_from_json_body(res->body);
    } catch (const std::exception& e) {
      return std::unexpected{Error{ErrorKind::Parse, res->status,
                                   std::string{"character reviews parse: "} + e.what(),
                                   res->raw_body, std::move(res->metadata)}};
    }
  }

  // ── balance / rate limits ─────────────────────────────────────────────
  [[nodiscard]] auto balance(const RequestOptions& opts = {}) const
      -> std::expected<nlohmann::json, Error> {
    auto res = get_json_response("/api_keys/rate_limits", detail::AuthPolicy::BearerOnly, opts);
    if (!res) return std::unexpected{std::move(res.error())};
    return std::move(res->body);
  }

  [[nodiscard]] auto base_url() const noexcept -> const std::string& { return m_base_url; }

 private:
  Authentication m_authentication;
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
  //
  // `tools` is not walked either (VC-08), and that is the same decision rather
  // than a new one. Refusing a tool with an empty name looks like it belongs
  // beside "model is empty" — both are representable in JSON and rejected
  // anyway — but the decisive precedent is one line down: `messages` is checked
  // for emptiness and never entered, so Message::role is unvalidated and a
  // request carrying `{Message{}}` — role "", a guaranteed 400 — already sails
  // through. Guarding tools[i].name while ignoring messages[i].role is a coin
  // flip, not a line.
  //
  // The property everything above has and this would not: the server's 400
  // cannot tell you. nlohmann collapses NaN to null and Venice's rejection never
  // mentions NaN, which is the whole reason the sweep exists; a 400 for a
  // nameless tool says tools[0].function.name. And for a 0.x library the
  // asymmetry decides it — adding a guard later is additive, removing one is a
  // behaviour break. Pinned in test/03guards/ so it reads as a decision.
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

  [[nodiscard]] auto request_headers(detail::AuthPolicy policy,
                                     const RequestOptions& opts) const
      -> std::expected<httplib::Headers, Error> {
    const Authentication& authentication = opts.authentication ? *opts.authentication
                                                                : m_authentication;
    if (!detail::authentication_allowed(authentication.kind(), policy)) {
      return std::unexpected{Error{
          ErrorKind::InvalidArg, 0,
          "endpoint requires " + std::string{detail::auth_policy_name(policy)} +
              " authentication",
          {}}};
    }
    return detail::authentication_headers(authentication);
  }

  // Both typed JSON helpers route through detail::send_buffered and retain the
  // response metadata long enough for endpoint parsing to attach it to a
  // success or a shape error.
  //
  // Guard placement and ordering are identical to chat_stream's; see there. The
  // substrate's post-call token test comes before its transport-result test for
  // the same reason it does over there.
  [[nodiscard]] auto get_json_response(std::string_view endpoint, detail::AuthPolicy policy,
                                       const RequestOptions& opts) const
      -> std::expected<detail::JsonResponse, Error> {
    auto headers = request_headers(policy, opts);
    if (!headers) return std::unexpected{std::move(headers.error())};
    auto response = detail::send_buffered(
        m_base_url,
        detail::BufferedRequest{.method = detail::HttpMethod::Get,
                                .endpoint = std::string{endpoint},
                                .headers = std::move(*headers)},
        opts);
    if (!response) return std::unexpected{std::move(response.error())};
    return detail::decode_json_response(*response);
  }

  [[nodiscard]] auto post_json_response(std::string_view endpoint,
                                        const nlohmann::json& body,
                                        detail::AuthPolicy policy,
                                        const RequestOptions& opts) const
      -> std::expected<detail::JsonResponse, Error> {
    auto headers = request_headers(policy, opts);
    if (!headers) return std::unexpected{std::move(headers.error())};
    auto response = detail::send_buffered(
        m_base_url,
        detail::BufferedRequest{.method = detail::HttpMethod::Post,
                                .endpoint = std::string{endpoint},
                                .headers = std::move(*headers),
                                .body = detail::ByteBody{body.dump(), "application/json"}},
        opts);
    if (!response) return std::unexpected{std::move(response.error())};
    return detail::decode_json_response(*response);
  }

  // SSE framing used to live here as a private static plus a "\n\n" loop inside
  // the content_receiver, which is why #6's own acceptance criteria — partial
  // frames across chunk boundaries, [DONE] — were unreachable without a socket.
  // It is venice::detail::SseFramer in stream.hpp now, and three defects it was
  // hiding are fixed there.
};

}  // namespace venice
