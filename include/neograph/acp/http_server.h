/**
 * @file acp/http_server.h
 * @brief EXPERIMENTAL remote transports for ACPServer: Streamable HTTP and
 *        WebSocket on one endpoint.
 *
 * **Status.** stdio (ACPServer::run) is the standard ACP transport and is
 * unchanged. The protocol has no remote transport yet: the Streamable HTTP
 * and WebSocket transport is a draft proposal (the "Streamable HTTP &
 * WebSocket Transport" RFD of the ACP project). This class follows that
 * draft where it is explicit, so its wire details may change when the draft
 * does. It is **off by default**: a separate library
 * (`neograph::acp_http_server`, CMake option
 * `NEOGRAPH_BUILD_ACP_HTTP_SERVER`, default OFF) that nothing links unless
 * asked, and a server that listens only after start().
 *
 * **Design.**
 *
 * One ACP connection is one ACPServer. Everything ACP keeps per connection
 * (initialize once, the negotiated client capabilities, sessions, in-flight
 * prompts, the ids of the agent's own requests) already lives in
 * ACPServer, so the transport builds one per connection with the factory and
 * drives it exactly as stdio does: ACPServer::run() over a queue-backed
 * stream pair. A message reaches the agent in the order the transport
 * received it, and a connection that ends is an end of input for run(), so
 * the prompts of a dropped connection are cancelled after ACPServer's EOF
 * grace (the transport shortens it to zero when it closes a connection on
 * purpose). No ACPServer code path changes.
 *
 * Streamable HTTP on `endpoint` (default `/acp`), per the draft:
 *   - `POST` an `initialize` request without `Acp-Connection-Id` creates a
 *     connection and is answered `200` with the JSON-RPC response as the
 *     body and the new id in the `Acp-Connection-Id` header. Every other
 *     POST needs `Acp-Connection-Id`, is answered `202` with an empty
 *     body, and its response, like every other message of the agent,
 *     travels on a stream.
 *   - `GET` with `Accept: text/event-stream` opens a server-sent-event
 *     stream: with `Acp-Connection-Id` alone, the connection stream (the
 *     responses to session/new and session/resume, and messages that belong
 *     to no session); with `Acp-Session-Id` too, the session stream
 *     (session/update, agent-to-client requests such as
 *     session/request_permission and fs/*, and the responses to the
 *     session's other requests, found by their JSON-RPC id). A session that
 *     session/resume brought back has no stream of its own until the client
 *     opens one; its messages use the connection stream meanwhile. A second
 *     stream on a scope replaces the first. Each message is one `data:`
 *     event; a quiet stream sends the comment `: keep-alive`.
 *   - `DELETE` with `Acp-Connection-Id` ends the connection (`202`).
 *   - A POST that carries `params.sessionId` (other than session/resume,
 *     which introduces a session) must carry the same `Acp-Session-Id`, and
 *     it must name a session of that connection, or the answer is 400 or
 *     404. Content type `application/json` (415), `Accept` (406), a batch
 *     (501) and unknown ids (404) are answered as the draft lists. The
 *     client's answer to a request of the agent is a POST of a response;
 *     the session header is optional on it.
 *   - A message the agent emits while no stream of its scope is open waits
 *     in a bounded queue and is sent when one opens; the draft defines no
 *     replay, so a message dropped with its connection is lost, and a
 *     connection whose queue overflows is closed.
 *
 * WebSocket on the same endpoint: a `GET` with `Upgrade: websocket` creates a
 * connection whose messages are text frames, one JSON-RPC message each, in
 * both directions; a binary frame is ignored. The client sends `initialize`
 * as its first message. Frames follow RFC 6455: client frames must be
 * masked, control frames are answered, a message may be fragmented, and a
 * message larger than `max_payload_bytes` closes the connection with code
 * 1009 (the HTTP library reads a frame of up to 16 MiB before this limit can
 * apply).
 * Closing the socket ends the connection; a peer that stops sending for more
 * than a few seconds in the middle of a frame is disconnected.
 *
 * Connections are independent and may run prompts concurrently on the same
 * engine. A session id belongs to one live connection: session/resume of an
 * id that another live connection holds is refused with the JSON-RPC error
 * that a busy session gets. A connection's sessions die with it; a client
 * that reconnects recovers them with session/resume from the engine's
 * checkpoint store, as it would after restarting a stdio agent. An
 * ACPClient made by client() reaches the connection that owns a session.
 *
 * **Where this deviates from the draft.**
 *   - The draft requires HTTP/2. This server speaks HTTP/1.1, so every
 *     stream and every POST is its own TCP connection. A deployment that
 *     needs multiplexing terminates HTTP/2 in a reverse proxy. WebSocket
 *     does not have this limit.
 *   - The 101 response of a WebSocket upgrade does not carry
 *     `Acp-Connection-Id` (the HTTP library does not let a handler add
 *     headers to it); the socket is the connection, and no later request
 *     needs the id.
 *   - No cookies are set, `Acp-Protocol-Version` is not checked, and there
 *     is no event id or resumption of a stream.
 *   - A stream for a session id the connection does not know is refused
 *     (404), as the draft's routing table says, so a session stream opens
 *     after session/new or session/resume has answered.
 *
 * **Security.** The listener binds a loopback address unless the caller
 * names another host, and a remote bind is refused unless
 * ACPHttpConfig::authenticator is set. Every request, including a stream,
 * an upgrade and a path the server does not serve, is checked before its
 * body is read: Host and Origin (the rules and the configuration names of
 * MCPHttpServerConfig and A2AHttpConfig), then the authenticator, then the
 * size limit. A connection belongs to the principal that created it. The
 * agent can read and write files and run commands in the client's name, so
 * treat a remote bind as exposing that. A client cannot make the server start
 * processes: `mcpServers` in `session/new` and `session/resume` is refused
 * unless ACPHttpConfig::allow_client_mcp_servers is set, and the `cwd` it
 * names reaches the graph as `_acp_cwd`, so a node that uses it on the server
 * has to check it. The bytes of messages waiting for the agent are bounded
 * (two messages of `max_payload_bytes`, at least 1 MiB; a POST beyond it is
 * answered 503 with `Retry-After`, and a WebSocket message beyond it with a
 * JSON-RPC error). Slow or idle TCP connections are not limited per peer: a
 * server that faces the internet belongs behind a reverse proxy that does
 * that.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/acp/server.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::acp {

/// What the factory of an ACPHttpServer returns for one new connection.
struct NEOGRAPH_API ACPHttpConnection {
    /// Optional lifetime owner for objects captured by `server`; declared
    /// first so that it is released after the server is destroyed.
    std::shared_ptr<void> owner;

    /// A fresh, uninitialized ACPServer. The transport drives it with
    /// ACPServer::run(); set its capabilities, adapter and limits before
    /// returning it. Do not call run() or set_notification_sink() on it.
    std::unique_ptr<ACPServer> server;
};

/**
 * @brief Limits, Host and Origin rules and authentication of an
 *        ACPHttpServer. The names and meanings follow MCPHttpServerConfig
 *        and A2AHttpConfig.
 */
struct NEOGRAPH_API ACPHttpConfig {
    /// Resolve the HTTP `Authorization` header (empty when the request has
    /// none) into the principal that made the request, or nullopt (or an
    /// empty string) to refuse it. The server never keeps the header.
    using Authenticator =
        std::function<std::optional<std::string>(std::string_view authorization_header)>;

    /// Address to bind. The default is loopback; any other host is a remote
    /// bind and needs `authenticator`.
    std::string host = "127.0.0.1";
    int         port = 0;  ///< 0 picks a free port; see ACPHttpServer::port().
    /// The one path the transport serves, for POST, GET (stream or upgrade)
    /// and DELETE.
    std::string endpoint = "/acp";

    /// Serve Streamable HTTP (POST, GET stream, DELETE) on `endpoint`.
    bool streamable_http = true;
    /// Serve the WebSocket upgrade on `endpoint`.
    bool websocket = true;

    /// Open connections at most; a new one beyond it is refused (HTTP 503).
    std::size_t max_connections = 16;
    /// The largest request body (HTTP) or message (WebSocket) accepted. A
    /// body announced larger by its Content-Length is answered with 413
    /// without being read, and a chunked one is cut off once it passes the
    /// limit. The WebSocket library reads one frame of up to 16 MiB before
    /// this limit can be applied, so a larger value is refused when
    /// `websocket` is on.
    std::size_t max_payload_bytes = 4 * 1024 * 1024;

    /// HTTP worker threads started with the server. A stream and a
    /// WebSocket connection hold one for as long as they are open, so more
    /// start on demand up to max_http_threads (they then stay), and at most
    /// half of max_http_threads are given to streams and WebSocket
    /// connections; a further stream is answered 503. The rest answer POST
    /// and DELETE.
    std::size_t http_threads        = 8;
    std::size_t max_http_threads    = 64;
    std::size_t max_queued_requests = 64;

    /// A connection with no open stream, no WebSocket and no request for
    /// this long is closed and its prompts are cancelled; its id then
    /// answers 404. This is how a dropped connection ends over HTTP. Zero
    /// keeps such connections until DELETE.
    std::chrono::milliseconds connection_idle_timeout = std::chrono::seconds(60);
    /// A quiet stream sends an SSE comment, and a WebSocket a ping, this
    /// often, so that a proxy does not cut it and a vanished peer is found.
    std::chrono::milliseconds keepalive_interval = std::chrono::seconds(15);

    /// Exact allowed Origin values. An Origin header is rejected unless
    /// listed, except that a loopback bind also admits its own loopback
    /// origin: `http://localhost:<port>`, `http://127.0.0.1:<port>` or
    /// `http://[::1]:<port>` at the port it is bound to (and writes a line
    /// to stderr the first time it admits each one). A request without an
    /// Origin header is not checked.
    std::vector<std::string> allowed_origins;

    /// Host header names the server answers: host names, IPv4 addresses and
    /// bracketed IPv6 addresses (`[::1]`), compared with the Host header's
    /// host without its port and ignoring case. `*` admits any Host. A
    /// loopback bind always admits `localhost`, `127.0.0.1` and `[::1]` and
    /// refuses every other Host unless it is listed, with or without an
    /// Origin. On a remote bind an empty list checks nothing, and a
    /// non-empty one admits only the listed names.
    std::vector<std::string> allowed_hosts;

    /// Accept stdio MCP servers named by the client in `session/new` and
    /// `session/resume` (`mcpServers`). ACPServer starts each as a process of
    /// this host, with the command line and environment entries the client
    /// sends, in the `cwd` the client names. For an editor that launched the
    /// agent on its own machine that is how it extends the agent; for a
    /// remote client it is command execution on the server. So a request that
    /// lists any is refused with JSON-RPC `-32602` (`data.reason`
    /// `"mcp_servers_not_accepted"`) unless this is true; an empty list is
    /// always accepted.
    bool allow_client_mcp_servers = false;

    /// Authenticates every request, stream and upgrade when set (HTTP 401 and
    /// `WWW-Authenticate: Bearer` otherwise). The principal it returns is
    /// passed to the factory, and the connection answers only to that
    /// principal afterwards. A browser cannot set `Authorization` on a
    /// WebSocket; authenticate those in a reverse proxy.
    Authenticator authenticator;
};

/**
 * @brief ACP over Streamable HTTP and WebSocket; see the file comment for
 *        the mapping, the deviations from the draft and the security rules.
 *
 * @code
 * acp::ACPHttpServer server([&](std::string_view) {
 *     acp::ACPHttpConnection connection;
 *     connection.server = std::make_unique<acp::ACPServer>(
 *         engine, neograph::json{{"name", "my-agent"}});
 *     return connection;
 * });
 * server.start_async();   // loopback, free port; see server.port()
 * @endcode
 */
class NEOGRAPH_API ACPHttpServer {
public:
    /// Called once per new connection, with the principal the authenticator
    /// returned (empty without one). It must return a fresh ACPServer; it may
    /// be called on any HTTP thread and concurrently.
    using ConnectionFactory = std::function<ACPHttpConnection(std::string_view principal)>;

    /// @throws std::invalid_argument for a missing factory, an invalid limit
    ///         or `endpoint`, an `allowed_hosts` entry that is not a host
    ///         name without a port, an IPv6 address in brackets or `*`, a
    ///         remote bind without `authenticator`, or no transport enabled.
    ACPHttpServer(ConnectionFactory factory, ACPHttpConfig config = {});
    ~ACPHttpServer();

    ACPHttpServer(const ACPHttpServer&)            = delete;
    ACPHttpServer& operator=(const ACPHttpServer&) = delete;

    /// Bind and serve until stop() is called. Returns false if the address
    /// cannot be bound.
    bool start();

    /// Bind and serve on a background listener thread.
    bool start_async();

    /// Stop accepting, close every connection (cancelling its prompts) and
    /// join the threads. The connections' ACPServers are destroyed here.
    void stop();

    bool is_running() const;
    /// The bound port; useful when ACPHttpConfig::port is 0.
    int  port() const;

    /// The number of open connections.
    std::size_t connection_count() const;

    /// A handle for agent-to-client requests (fs/*, permission) that reaches
    /// the connection owning the session named in each call, so graph nodes
    /// can capture it once, before any connection exists. A call for a
    /// session that no live connection holds throws std::runtime_error.
    std::shared_ptr<ACPClient> client();

    /// Bind a client that graph nodes captured before this server existed,
    /// as ACPServer::attach_client does for one connection: an unbound
    /// `std::make_shared<ACPClient>()` is captured when the engine is
    /// compiled and attached here once the server is built.
    void attach_client(std::shared_ptr<ACPClient> client);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph::acp
