# cpp-httplib 0.51 dependency contract (#120)

Write failures before adapting the transport. All peers and certificates are
synthetic loopback fixtures; no provider, credential or cluster is involved.

## Configuration and consumer failures

- Obsolete or unsupported-minor package, missing canonical target, compiled
  target, non-OpenSSL or alternate TLS backend, OpenSSL older than 3, detached
  resolver profile, exceptions disabled and incompatible header count, header-line
  or line-buffer growth limits refuse.
- Version metadata cannot substitute for compiling the selected header/API.
  A compatible package that resolves a different header refuses. A conflicting
  preexisting target is never silently replaced by a fetched copy.
- Rejected package hints cannot fall through to an unrelated installed package in
  negative fixtures. A separate positive fixture proves a compatible installed
  alternative is preferred. Positive installed tests require an imported target,
  so a seeded fallback cannot falsely satisfy their assertions.
- Parent cache options survive embedding and repeated configure. Source fallback
  creates only the canonical header-only OpenSSL target with synchronous resolver;
  tests/modules/install remain disabled in an embedded Venice build. Top-level
  installation exports the dependency and uses its own documentation directory.
- Install/export includes the compatibility check and its actual public
  dependencies. Required and optional package discovery both have honest failure
  behavior. CMake and compile-time guards reject incompatible actual headers.

## Existing runtime contracts to preserve

- Multipart filenames/media types reject controls before transport; fields and
  files are independently asserted, duplicate file entries retain payload bytes,
  and an empty text field is not fabricated into a file.
- Response byte ceilings, non-2xx classification before success-media parsing,
  deliberate early stop, malformed SSE and cancellation retain their semantics.
- Redirect responses never replay Venice credentials or bodies to another peer.
- Presigned retrieval retains exact escaped path/query bytes and uses its existing
  explicit c-ares lifetime and address pinning; no second DNS initialization owner.
- Real TLS fixture cancellation and teardown retain thread-scoped SIGPIPE
  protection with the application's disposition restored. TLS setup failures do
  not become successful plaintext tests.

## Positive smoke last

- Both supported compilers compile/link actual official 0.51.0 source and its real
  upstream installed/exported package, with the same canonical OpenSSL3 interface.
- Both Venice compiler suites and all add_subdirectory/FetchContent/installed
  consumers pass before delivery. Focused probes alone do not prove delivery.
