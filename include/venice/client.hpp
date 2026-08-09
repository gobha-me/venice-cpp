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
//   * balance()                     -> expected<json>           (rate-limit/balance)
//
// A ChatResponse carries the whole assistant turn as a Message, so a reply can
// be appended to the next request's messages and nothing is lost — thinking and
// tool calls included (VC-05/VC-14). The streaming forms assemble into the same
// Message; see venice/stream.hpp.
//
// Every one of them takes a trailing venice::RequestOptions (defaulted) for
// per-call timeouts and cancellation — see venice/options.hpp (VC-06).
//
// Not in Phase 0 (later phases, fed by real use): image/audio/video, TTS,
// embeddings, retries/backoff, async.

#include <array>
#include <cmath>
#include <expected>
#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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

}  // namespace detail

class Client {
 public:
  // api_key: Venice API key (Bearer). base_url defaults to the public API;
  // honor VENICE_BASE_URL-style overrides by passing an explicit value.
  explicit Client(std::string api_key,
                  std::string base_url = "https://api.venice.ai/api/v1")
      : m_api_key(std::move(api_key)), m_base_url(std::move(base_url)) {}

  // ── chat (non-streaming) ──────────────────────────────────────────────
  //
  // `opts` bounds the call and can abort it from another thread; a request
  // aborted that way comes back ErrorKind::Cancelled, never a partial response.
  // See venice/options.hpp.
  [[nodiscard]] auto chat(const ChatRequest& req, const RequestOptions& opts = {}) const
      -> std::expected<ChatResponse, Error> {
    if (auto ok = validate(req); !ok) return std::unexpected{std::move(ok.error())};

    auto res = post_json("/chat/completions", req.to_json_body(/*stream=*/false), opts);
    if (!res) return std::unexpected{std::move(res.error())};

    try {
      return ChatResponse::from_json_body(*res);
    } catch (const std::exception& e) {
      return std::unexpected{Error{ErrorKind::Parse, 0, std::string{"chat parse: "} + e.what(), res->dump()}};
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
    acc.reset();

    const std::string payload = req.to_json_body(/*stream=*/true).dump();

    bool early_stop = false;  // on_delta said stop — NOT opts.cancel; see above
    std::string parse_err;
    detail::SseFramer framer;
    std::vector<ToolCall> frags;  // backing store for the span in each delta

    auto cli = make_transport(opts);
    // Declared after cli and never before: the guard's watcher thread holds a
    // reference to it, and destruction runs in reverse, so this ordering is what
    // guarantees the thread is joined while cli is still alive.
    const detail::CancelGuard guard{opts.cancel, cli};
    if (guard.cancelled()) return std::unexpected{cancel_error()};

    httplib::Headers headers = auth_headers();
    headers.emplace("Accept", "text/event-stream");

    httplib::Request hreq;
    hreq.method = "POST";
    hreq.path = path("/chat/completions");
    hreq.headers = headers;
    hreq.body = payload;
    hreq.set_header("Content-Type", "application/json");

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

    // The token is tested first, ahead of both the transport error and the
    // early stop. It has to be: a cancel is delivered by shutting the socket
    // down, so it *arrives* as a transport failure and would otherwise be
    // reported as a dead network. Ahead of early_stop as well, because a cancel
    // racing an on_delta stop is a caller who has abandoned the call — reading
    // that as "here is your partial answer" would hand back a response nobody
    // is waiting for. What `acc` holds is unaffected either way, which is the
    // whole point of it belonging to the caller.
    if (guard.cancelled()) return std::unexpected{cancel_error()};
    if (!sent && !early_stop)
      return std::unexpected{transport_error(herr)};
    if (early_stop) return acc.response();  // deliberate early stop
    if (hres.status < 200 || hres.status >= 300)
      return std::unexpected{http_error(hres.status, hres.body)};
    // "nothing arrived at all", not "no content arrived". The old test was
    // assembled.content.empty(), which reported ErrorKind::Parse on a
    // reasoning-only stream that had in fact been received perfectly.
    if (!parse_err.empty() && acc.empty())
      return std::unexpected{Error{ErrorKind::Parse, hres.status, "stream parse: " + parse_err, {}}};
    return acc.response();
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
    auto res = get_json(detail::with_query("/models", {{"type", type}}), opts);
    if (!res) return std::unexpected{std::move(res.error())};
    try {
      return models_from_json_body(*res);
    } catch (const std::exception& e) {
      return std::unexpected{Error{ErrorKind::Parse, 0, std::string{"models parse: "} + e.what(), res->dump()}};
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
  // Unlike /models, this endpoint needs a real key: it answers 402 to an
  // unauthenticated request and 401 to a junk bearer, both measured. Note the
  // 402 — `kind_for_status` maps 401/403 to ErrorKind::Auth and everything else
  // to ErrorKind::Http, so a *credential-less* call to this endpoint reports
  // Http, not Auth. Widening that mapping is an error-model change affecting
  // every endpoint and is filed separately rather than smuggled in here; until
  // then, check `status` as well as `kind`.
  //
  // Venice documents this as a preview API that may change, which is why
  // Character::raw and CharacterPage::raw matter more here than elsewhere.
  [[nodiscard]] auto characters(const CharacterQuery& query = {},
                                const RequestOptions& opts = {}) const
      -> std::expected<CharacterPage, Error> {
    const auto params = character_query_params(query);
    auto res = get_json(detail::with_query("/characters", params), opts);
    if (!res) return std::unexpected{std::move(res.error())};
    try {
      return characters_from_json_body(*res);
    } catch (const std::exception& e) {
      return std::unexpected{
          Error{ErrorKind::Parse, 0, std::string{"characters parse: "} + e.what(), res->dump()}};
    }
  }

  // ── balance / rate limits ─────────────────────────────────────────────
  [[nodiscard]] auto balance(const RequestOptions& opts = {}) const
      -> std::expected<nlohmann::json, Error> {
    return get_json("/api_keys/rate_limits", opts);
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

  // The three defaults are the Phase 0 values and stay the defaults: a chat
  // completion can legitimately take minutes, so a short read timeout would be
  // a worse bug than the one VC-06 fixes. What changed is that they are now a
  // floor a caller can move, per call, rather than a property of the library.
  //
  // No write timeout default — httplib's own applies. A request body here is a
  // JSON document measured in kilobytes; there has never been a case where the
  // write is the thing that hangs, and inventing a number for it would be
  // asserting knowledge we do not have.
  [[nodiscard]] auto make_transport(const RequestOptions& opts) const -> httplib::Client {
    httplib::Client cli{host()};
    cli.set_bearer_token_auth(m_api_key);
    cli.set_follow_location(true);
    cli.set_read_timeout(opts.read_timeout.value_or(std::chrono::seconds{300}));
    cli.set_connection_timeout(opts.connect_timeout.value_or(std::chrono::seconds{30}));
    if (opts.write_timeout) cli.set_write_timeout(*opts.write_timeout);
    return cli;
  }

  [[nodiscard]] auto auth_headers() const -> httplib::Headers {
    return {{"Authorization", "Bearer " + m_api_key}};
  }

  // Both buffered helpers keep using httplib's convenience Get/Post rather than
  // the lower-level send(): cancellation works by shutting the socket down from
  // another thread, which aborts whichever call is blocked on it. Routing these
  // through a content_receiver — one option #7 floated — would buy nothing,
  // because a receiver only runs when bytes arrive and the case worth
  // cancelling is precisely the one where none do.
  //
  // Guard placement and ordering are identical to chat_stream's; see there. The
  // post-call test comes before the `!res` test for the same reason it does
  // over there, and also covers the case where the cancel lands after a
  // perfectly good response: the caller stopped waiting, so they get Cancelled
  // rather than an answer nobody is holding a thread for.
  [[nodiscard]] auto get_json(std::string_view endpoint, const RequestOptions& opts) const
      -> std::expected<nlohmann::json, Error> {
    auto cli = make_transport(opts);
    const detail::CancelGuard guard{opts.cancel, cli};
    if (guard.cancelled()) return std::unexpected{cancel_error()};

    auto res = cli.Get(path(endpoint), auth_headers());
    if (guard.cancelled()) return std::unexpected{cancel_error()};
    if (!res) return std::unexpected{transport_error(res.error())};
    return decode_json(res->status, res->body);
  }

  [[nodiscard]] auto post_json(std::string_view endpoint, const nlohmann::json& body,
                               const RequestOptions& opts) const
      -> std::expected<nlohmann::json, Error> {
    auto cli = make_transport(opts);
    const detail::CancelGuard guard{opts.cancel, cli};
    if (guard.cancelled()) return std::unexpected{cancel_error()};

    auto res = cli.Post(path(endpoint), auth_headers(), body.dump(), "application/json");
    if (guard.cancelled()) return std::unexpected{cancel_error()};
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

  // No status and no body, and there never can be one: the socket was shut down
  // under the request, so whatever the server was going to say did not arrive.
  [[nodiscard]] static auto cancel_error() -> Error {
    return Error{ErrorKind::Cancelled, 0, "cancelled by caller", {}};
  }

  // SSE framing used to live here as a private static plus a "\n\n" loop inside
  // the content_receiver, which is why #6's own acceptance criteria — partial
  // frames across chunk boundaries, [DONE] — were unreachable without a socket.
  // It is venice::detail::SseFramer in stream.hpp now, and three defects it was
  // hiding are fixed there.
};

}  // namespace venice
