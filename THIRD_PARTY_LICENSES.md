# Third-Party Licenses

NeoGraph itself is MIT-licensed (see `LICENSE`). This file enumerates
third-party code vendored under `deps/` and the license each ships
under.

Each entry names the directory/file, the upstream project, and the
license classification. License texts live in the header comments of
the vendored sources — grep for "Copyright" / "License" at the top of
each file for the canonical wording.

Taskflow was removed in 3.0 (`deps/taskflow/` deleted; sync super-step
loop now routes through `neograph::async::run_sync(execute_graph_async)`).

| Path | Upstream | License | Used by |
|---|---|---|---|
| `deps/asio/` | [chriskohlhoff/asio](https://github.com/chriskohlhoff/asio) | Boost Software License 1.0 | `neograph::core` (coroutine runtime), `neograph::async` (HTTP + SSE), `neograph::mcp` (stdio/HTTP RPC) |
| `deps/yyjson/` | [ibireme/yyjson](https://github.com/ibireme/yyjson) | MIT | `neograph::core` (JSON parse/dump) |
| `deps/httplib.h` | [yhirose/cpp-httplib](https://github.com/yhirose/cpp-httplib) | MIT | `neograph::llm` (OpenAI-compatible HTTP), `neograph::mcp` (HTTP transport) |
| `deps/concurrentqueue.h` | [cameron314/concurrentqueue](https://github.com/cameron314/concurrentqueue) | Simplified BSD (2-clause) | `neograph::util` (`RequestQueue` — lock-free MPMC) |
| `deps/cppdotenv/` | [theskumar/python-dotenv](https://github.com/theskumar/python-dotenv) (C++ port) | MIT | `examples/13_deep_research` (`.env` loader) |
| `deps/clay.h`, `deps/clay_renderer_raylib.c` | [nicbarker/clay](https://github.com/nicbarker/clay) | zlib | `examples/99_clay_chatbot` (opt-in UI) |

## OpenSSL (system, linked dynamically)

`neograph::llm` and `neograph::mcp` link against the platform-provided
OpenSSL at runtime (not vendored). OpenSSL 3.x is under the Apache
License 2.0.

## libpq / libsqlite3 (system, opt-in)

`neograph::postgres` links libpq (PostgreSQL Licence — MIT-like) when
built with `NEOGRAPH_BUILD_POSTGRES=ON`. `neograph::sqlite` links
libsqlite3 (public domain) when built with `NEOGRAPH_BUILD_SQLITE=ON`.
Neither is vendored under `deps/` — both are expected on the system.

## Test dependencies (not shipped in release binaries)

`tests/` fetches GoogleTest at configure time via CMake `FetchContent`
(BSD 3-Clause). GoogleTest sources never ship in downstream
installations of NeoGraph; they exist only during local test builds.

---

## License compatibility summary

All vendored and system dependencies use permissive licenses (MIT,
BSD, zlib, Boost, Apache 2.0, PostgreSQL). None impose copyleft
obligations on downstream consumers of a NeoGraph binary. A
redistributor must, at minimum, preserve the copyright notices listed
in the vendored source file headers when shipping a binary that
statically links these components.
