// Schema-declared token-usage decoding, shared by SchemaProvider (non-stream,
// SSE, WebSocket) and OpenAIProvider. Internal to neograph_llm.
//
// Vendors disagree about what "prompt" and "completion" tokens include, so the
// mapping is data in the schema's `response` section instead of a C++ branch:
//   prompt_tokens_field / completion_tokens_field / total_tokens_field
//                        the three basic counters (paths inside the usage object)
//   prompt_extra_fields  more usage fields that are ADDED to prompt tokens.
//                        Anthropic reports `input_tokens` without the cached
//                        prefix; `cache_read_input_tokens` and
//                        `cache_creation_input_tokens` make it the real prompt.
//   cached_tokens_path   the prompt-token subset served from cache
//   reasoning_tokens_path the completion-token subset spent on reasoning
//   completion_includes_reasoning
//                        false when the vendor's completion counter excludes
//                        reasoning (Gemini `candidatesTokenCount` vs
//                        `thoughtsTokenCount`): reasoning is then added, so
//                        `completion_tokens` is what the model produced and
//                        `reasoning_tokens` stays a subset of it.
#pragma once

#include <neograph/llm/json_path.h>
#include <neograph/types.h>

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

namespace neograph::llm::detail {

// Missing or non-numeric -> 0. Values are clamped into int.
inline int usage_int_at(const json& usage, const std::string& path) {
    if (path.empty()) return 0;
    const auto value = json_path::at_path(usage, path);
    if (!value) return 0;
    long long n = 0;
    if (value->is_number_integer()) {
        n = value->template get<long long>();
    } else if (value->is_number_float()) {
        n = static_cast<long long>(value->template get<double>());
    } else {
        return 0;
    }
    if (n < 0) return 0;
    return static_cast<int>(std::min<long long>(n, std::numeric_limits<int>::max()));
}

inline int saturating_add(int a, int b) {
    const long long sum = static_cast<long long>(a) + b;
    return static_cast<int>(std::min<long long>(sum, std::numeric_limits<int>::max()));
}

// Duck-typed over the schema's response config (ResponseConfig, or the small
// struct OpenAIProvider fills from the built-in `openai` schema).
template <class Spec>
ChatCompletion::Usage parse_usage_object(const json& usage, const Spec& spec) {
    ChatCompletion::Usage out;
    if (!usage.is_object()) return out;
    out.prompt_tokens = usage_int_at(usage, spec.prompt_tokens_field);
    for (const auto& field : spec.prompt_extra_fields) {
        out.prompt_tokens = saturating_add(out.prompt_tokens, usage_int_at(usage, field));
    }
    out.completion_tokens = usage_int_at(usage, spec.completion_tokens_field);
    out.cached_prompt_tokens = usage_int_at(usage, spec.cached_tokens_path);
    out.reasoning_tokens = usage_int_at(usage, spec.reasoning_tokens_path);
    if (!spec.completion_includes_reasoning) {
        out.completion_tokens = saturating_add(out.completion_tokens, out.reasoning_tokens);
    }
    // An empty total field means the vendor reports none; the components are
    // the total. A reported total is kept as is (the accumulator promotes a
    // total that is below the component sum).
    out.total_tokens = spec.total_tokens_field.empty()
        ? saturating_add(out.prompt_tokens, out.completion_tokens)
        : usage_int_at(usage, spec.total_tokens_field);
    return out;
}

// Streams repeat or refine usage (`usageMetadata` on every chunk, Anthropic
// `message_start` then `message_delta`): a positive field of `latest`
// overwrites, a zero one keeps what was seen before.
inline void merge_stream_usage(ChatCompletion::Usage& into, const ChatCompletion::Usage& latest) {
    if (latest.prompt_tokens > 0) into.prompt_tokens = latest.prompt_tokens;
    if (latest.completion_tokens > 0) into.completion_tokens = latest.completion_tokens;
    if (latest.total_tokens > 0) into.total_tokens = latest.total_tokens;
    if (latest.cached_prompt_tokens > 0) into.cached_prompt_tokens = latest.cached_prompt_tokens;
    if (latest.reasoning_tokens > 0) into.reasoning_tokens = latest.reasoning_tokens;
}

}  // namespace neograph::llm::detail
