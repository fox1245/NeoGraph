/**
 * @file llm/schema_primitive_registry.h
 * @brief Explicitly injected extensions for schema-driven providers.
 *
 * A SchemaPrimitiveRegistry is a value assembled by the application and copied
 * into each SchemaProvider at creation. It is deliberately not process-global:
 * factories and any state they own live as long as the copied registry/provider.
 * Python bindings do not expose this callback API; the extension boundary is
 * C++ only until a separately reviewed Python lifetime/ABI contract exists.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/async/endpoint.h>
#include <neograph/async/http_client.h>
#include <neograph/types.h>

#include <asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::graph { class CancelToken; }

namespace neograph::llm {

/** A schema primitive category used in diagnostics and registry lookup. */
enum class SchemaPrimitiveCategory : std::uint8_t {
    Transport,
    ExecutionMode,
    ArtifactParser,
};

/**
 * Request data normalized once by SchemaProvider and passed to extensions.
 *
 * The strings/vectors are owned values. A factory may retain a copy for the
 * duration of an asynchronous operation, but must not retain references into
 * a provider. cancellation, deadline, and trace_metadata are the same
 * operation-local values selected by SchemaProvider.
 */
struct NEOGRAPH_API SchemaPrimitiveRequestContext {
    async::AsyncEndpoint endpoint;
    std::string path;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    int timeout_seconds = -1;
    bool get = false;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    std::shared_ptr<graph::CancelToken> cancellation;
    std::map<std::string, std::string> trace_metadata;
};

using SchemaTransportFactory = std::function<asio::awaitable<async::HttpResponse>(
    SchemaPrimitiveRequestContext)>;

/** Context supplied to a custom execution-mode factory. */
struct NEOGRAPH_API SchemaExecutionContext {
    std::string mode;
    SchemaPrimitiveRequestContext request;
    /// Dispatches one request using the selected transport primitive.
    SchemaTransportFactory transport;
};

using SchemaExecutionFactory = std::function<asio::awaitable<json>(
    SchemaExecutionContext)>;
using SchemaArtifactParserFactory = std::function<std::vector<GeneratedArtifact>(
    const json&, const SchemaPrimitiveRequestContext&)>;

/** Duplicate-name behavior when assembling a registry. */
enum class SchemaPrimitiveRegistration : std::uint8_t {
    Reject,
    Replace,
};

/**
 * Explicit, provider-scoped registry for schema extension primitives.
 *
 * Registration is deterministic: names are unique within a category;
 * duplicates reject by default and replacement is only possible through the
 * explicit Replace policy (or replace_* helpers). SchemaProvider copies the
 * registry before validation, so mutations after create() cannot affect a
 * live provider. Assemble a registry before publishing it to concurrent
 * callers; provider use itself is safe for concurrent calls.
 */
class NEOGRAPH_API SchemaPrimitiveRegistry {
public:
    SchemaPrimitiveRegistry();
    static SchemaPrimitiveRegistry standard();

    void register_transport(std::string name, SchemaTransportFactory factory,
                            SchemaPrimitiveRegistration policy = SchemaPrimitiveRegistration::Reject);
    void register_execution_mode(std::string name, SchemaExecutionFactory factory,
                                 SchemaPrimitiveRegistration policy = SchemaPrimitiveRegistration::Reject);
    void register_artifact_parser(std::string name, SchemaArtifactParserFactory factory,
                                  SchemaPrimitiveRegistration policy = SchemaPrimitiveRegistration::Reject);

    void replace_transport(std::string name, SchemaTransportFactory factory);
    void replace_execution_mode(std::string name, SchemaExecutionFactory factory);
    void replace_artifact_parser(std::string name, SchemaArtifactParserFactory factory);

    bool contains(SchemaPrimitiveCategory category, std::string_view name) const;
    std::vector<std::string> names(SchemaPrimitiveCategory category) const;
    const SchemaTransportFactory& transport(std::string_view name) const;
    const SchemaExecutionFactory& execution_mode(std::string_view name) const;
    const SchemaArtifactParserFactory& artifact_parser(std::string_view name) const;

    static const char* category_name(SchemaPrimitiveCategory category) noexcept;
    static void validate_name(std::string_view name, std::string_view label);

private:
    std::map<std::string, SchemaTransportFactory> transports_;
    std::map<std::string, SchemaExecutionFactory> execution_modes_;
    std::map<std::string, SchemaArtifactParserFactory> artifact_parsers_;

    // Name validation is shared by the registration helpers.
};

} // namespace neograph::llm
