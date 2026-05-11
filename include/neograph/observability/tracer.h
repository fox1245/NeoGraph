/**
 * @file observability/tracer.h
 * @brief Dep-free abstract Tracer / Span interface.
 *
 * NeoGraph does not link against opentelemetry-cpp directly. The
 * `openinference_tracer` and `OpenInferenceProvider` helpers in
 * `observability/openinference.h` drive this small abstract interface
 * instead, and downstream code provides an adapter wrapping its own
 * tracing backend (opentelemetry-cpp, a custom recorder, an in-memory
 * test fake, etc.).
 *
 * The interface intentionally stops short of OTel's full surface
 * (links, resources, span context propagation, sampler hooks). It
 * covers the minimum the OpenInference attribute-mapping layer needs:
 * named span open/close, attribute writes, status, and discrete
 * events for streaming-token diagnostics. Anything richer is the
 * adapter author's responsibility.
 */
#pragma once

#include <neograph/api.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace neograph::observability {

/**
 * @brief Opaque per-request span; lifetime is unique_ptr.
 *
 * Adapters create a Span by wrapping their backend's span handle
 * (e.g. `opentelemetry::trace::Span`). Calling `end()` is required
 * before destruction for proper export — the destructor does NOT
 * call `end()` on its own (an unended span signals "still in flight"
 * to the OpenInference helpers and lets `_close_all`-style cleanup
 * paths distinguish active vs. already-ended spans).
 *
 * Thread safety: implementations MAY be called from multiple threads
 * (engine event callback runs on whatever pool worker emitted the
 * event). Adapter authors must serialize internally if their backend
 * span isn't thread-safe.
 */
class NEOGRAPH_API Span {
public:
    virtual ~Span() = default;

    virtual void set_attribute(std::string_view key, std::string_view value) = 0;
    virtual void set_attribute(std::string_view key, int64_t value) = 0;
    virtual void set_attribute(std::string_view key, double value) = 0;

    /// Renamed off the `set_attribute` overload set on purpose: a
    /// `const char*` literal passed as the value would otherwise
    /// resolve to `(string_view, bool)` (pointer-to-bool standard
    /// conversion outranks the user-defined string_view conversion),
    /// silently dropping every string attribute. Keep bool out of the
    /// overload set entirely.
    virtual void set_attribute_bool(std::string_view key, bool value) = 0;

    /// Forwarding overload so string literals (`"CHAIN"`) reach the
    /// string_view sink rather than slipping into a different overload.
    /// Non-virtual; defined inline.
    void set_attribute(std::string_view key, const char* value) {
        set_attribute(key, std::string_view(value ? value : ""));
    }

    /// Record a discrete time-stamped event on this span (mapped to
    /// OTel's `Span::AddEvent`). Used by the openinference layer for
    /// `LLM_TOKEN` events so each streamed chunk is visible in the
    /// trace timeline without inflating the parent span's attribute
    /// payload.
    virtual void add_event(std::string_view name,
                           std::string_view payload = {}) = 0;

    virtual void set_status_ok() = 0;
    virtual void set_status_error(std::string_view message) = 0;

    /// Close the span. After end() returns the span is finalized;
    /// further attribute / event writes are no-ops on most backends.
    virtual void end() = 0;
};

/**
 * @brief Adapter facade for a tracing backend.
 *
 * The OpenInference helpers call `start_span` to open named spans
 * (root + per-node + per-LLM-call). The `parent` argument carries the
 * span hierarchy — a non-null parent means "open this as a child of
 * the supplied span", null means "open as a root".
 *
 * Adapter authors are free to ignore `parent` if their backend
 * captures parents implicitly via an OTel context propagation hook;
 * the OpenInference helpers DO supply the parent explicitly so a
 * dep-free adapter can link spans without relying on contextvars-
 * style implicit context.
 */
class NEOGRAPH_API Tracer {
public:
    virtual ~Tracer() = default;

    /// @param name   Span name (e.g. "graph.run", "node.researcher",
    ///               "llm.complete").
    /// @param parent Optional parent span. May be a span this tracer
    ///               itself returned from a previous `start_span`.
    ///               Adapters should treat null as "root span".
    /// @return Owned span; caller must call `end()` before drop.
    virtual std::unique_ptr<Span> start_span(std::string_view name,
                                             Span* parent = nullptr) = 0;
};

} // namespace neograph::observability
