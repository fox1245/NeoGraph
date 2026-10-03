#include <neograph/llm/decisions_client.h>
#include <neograph/async/endpoint.h>
#include <neograph/async/http_client.h>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/co_spawn.hpp>
#include <asio/system_error.hpp>
#include <asio/post.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace neograph::llm {
namespace {
using Clock = std::chrono::steady_clock;

struct AdmissionError { EndpointFailureKind kind; const char* message; };
[[noreturn]] void invalid(const char* message) {
    throw AdmissionError{EndpointFailureKind::InvalidRequest, message};
}
void require(bool condition, const char* message) { if (!condition) invalid(message); }
EndpointFailure failure(EndpointFailureKind kind, const char* message,
                        bool sent = false, int status = 0) {
    return {kind, message, status, {}, sent};
}
bool guidance(const json& value) {
    return value.is_string() || value.is_object() || value.is_array();
}
void validate_json_value(const json& value) {
    if (value.is_number_float())
        require(std::isfinite(value.get<double>()), "Nonfinite Decisions JSON number");
    if (value.is_object() || value.is_array())
        for (const auto& child : value) validate_json_value(child);
}
void closed(const json& value, std::initializer_list<std::string_view> keys) {
    require(value.is_object(), "Decisions config must contain JSON objects");
    for (const auto& item : value.items())
        require(std::find(keys.begin(), keys.end(), item.first) != keys.end(),
                "Unknown Decisions config member");
}
std::uint64_t natural(const json& value) {
    require(value.is_number_integer() &&
        (value.is_number_unsigned() || value.get<std::int64_t>() >= 0),
        "Expected nonnegative integer");
    return value.get<std::uint64_t>();
}
std::size_t positive_size(const json& value) {
    const auto n = natural(value);
    require(n > 0 && n <= std::numeric_limits<std::size_t>::max(),
            "Expected positive representable limit");
    return static_cast<std::size_t>(n);
}
std::string nonempty_string(const json& value) {
    require(value.is_string() && !value.get<std::string_view>().empty(),
            "Expected nonempty string");
    return value.get<std::string>();
}
bool nonnegative(double value) { return std::isfinite(value) && value >= 0; }
bool probability(double value) { return nonnegative(value) && value <= 1; }
std::size_t codepoints(const std::string& text) {
    return static_cast<std::size_t>(std::count_if(text.begin(), text.end(),
        [](unsigned char c) { return (c & 0xc0) != 0x80; }));
}
bool header_safe(const std::string& text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return c >= 0x21 && c <= 0x7e;
    });
}
bool decimal_price(const std::string& text) {
    std::size_t index = 0;
    const auto digits = [&] {
        const auto start = index;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9') ++index;
        return index > start;
    };
    bool mantissa = digits();
    if (index < text.size() && text[index] == '.') {
        ++index;
        mantissa = digits() || mantissa;
    }
    if (!mantissa) return false;
    if (index < text.size() && (text[index] == 'e' || text[index] == 'E')) {
        ++index;
        if (index < text.size() && (text[index] == '+' || text[index] == '-')) ++index;
        if (!digits()) return false;
    }
    return index == text.size();
}
const char* sort_name(DecisionsSortBy value) {
    switch (value) {
        case DecisionsSortBy::Price: return "price";
        case DecisionsSortBy::Throughput: return "throughput";
        case DecisionsSortBy::Latency: return "latency";
        case DecisionsSortBy::Exacto: return "exacto";
    }
    invalid("Invalid provider sort enum");
}
const char* quantization_name(DecisionsQuantization value) {
    switch (value) {
        case DecisionsQuantization::Int4: return "int4";
        case DecisionsQuantization::Int8: return "int8";
        case DecisionsQuantization::Fp4: return "fp4";
        case DecisionsQuantization::Mxfp4: return "mxfp4";
        case DecisionsQuantization::Nvfp4: return "nvfp4";
        case DecisionsQuantization::Fp6: return "fp6";
        case DecisionsQuantization::Fp8: return "fp8";
        case DecisionsQuantization::Mxfp8: return "mxfp8";
        case DecisionsQuantization::Fp16: return "fp16";
        case DecisionsQuantization::Bf16: return "bf16";
        case DecisionsQuantization::Fp32: return "fp32";
        case DecisionsQuantization::Unknown: return "unknown";
    }
    invalid("Invalid provider quantization enum");
}
json preference_json(const DecisionsPreference& preference) {
    return std::visit([](const auto& value) -> json {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, double>) {
            require(nonnegative(value), "Invalid provider numeric preference");
            return value;
        } else {
            json result = json::object();
            const auto add = [&](const char* key, const std::optional<double>& number) {
                if (number) {
                    require(nonnegative(*number), "Invalid provider percentile preference");
                    result[key] = *number;
                }
            };
            add("p50", value.p50); add("p75", value.p75);
            add("p90", value.p90); add("p99", value.p99);
            return result;
        }
    }, preference);
}
json provider_json(const DecisionsProviderPreferences& value) {
    require(value.options.empty(), "Provider options are unsupported by the admitted Decisions models");
    json result = json::object();
    const auto boolean = [&](const char* key, const std::optional<bool>& entry) {
        if (entry) result[key] = *entry;
    };
    boolean("allow_fallbacks", value.allow_fallbacks);
    boolean("enforce_distillable_text", value.enforce_distillable_text);
    boolean("require_parameters", value.require_parameters);
    boolean("zdr", value.zdr);
    if (value.data_collection) {
        switch (*value.data_collection) {
            case DecisionsDataCollection::Allow: result["data_collection"] = "allow"; break;
            case DecisionsDataCollection::Deny: result["data_collection"] = "deny"; break;
            default: invalid("Invalid provider data collection enum");
        }
    }
    const auto names = [&](const char* key, const auto& entries) {
        if (!entries) return;
        for (const auto& entry : *entries)
            require(!entry.empty(), "Provider slugs must be nonempty");
        result[key] = *entries;
    };
    names("order", value.order); names("only", value.only); names("ignore", value.ignore);
    if (value.max_price) {
        json prices = json::object();
        const auto price = [&](const char* key, const std::optional<std::string>& entry) {
            if (entry) {
                require(decimal_price(*entry), "Invalid provider decimal price");
                prices[key] = *entry;
            }
        };
        price("prompt", value.max_price->prompt); price("completion", value.max_price->completion);
        price("image", value.max_price->image); price("audio", value.max_price->audio);
        price("request", value.max_price->request);
        result["max_price"] = std::move(prices);
    }
    if (value.quantizations) {
        auto entries = json::array();
        for (auto entry : *value.quantizations) entries.push_back(quantization_name(entry));
        result["quantizations"] = std::move(entries);
    }
    if (value.sort) {
        result["sort"] = std::visit([](const auto& sort) -> json {
            using T = std::decay_t<decltype(sort)>;
            if constexpr (std::is_same_v<T, DecisionsSortBy>) return sort_name(sort);
            else {
                json result = json::object();
                if (sort.by) result["by"] = sort_name(*sort.by);
                if (sort.partition) {
                    switch (*sort.partition) {
                        case DecisionsPartition::Model: result["partition"] = "model"; break;
                        case DecisionsPartition::None: result["partition"] = "none"; break;
                        default: invalid("Invalid provider partition enum");
                    }
                }
                return result;
            }
        }, *value.sort);
    }
    if (value.preferred_max_latency)
        result["preferred_max_latency"] = preference_json(*value.preferred_max_latency);
    if (value.preferred_min_throughput)
        result["preferred_min_throughput"] = preference_json(*value.preferred_min_throughput);
    return result;
}

std::optional<double> optional_number(const json& object, const char* key, bool unit) {
    const auto it = object.find(key);
    if (it == object.end()) return {};
    const auto field = it.value();
    if (field.is_null()) return {};
    require(field.is_number(), "Invalid numeric Decisions answer");
    const double value = field.get<double>();
    require(unit ? probability(value) : nonnegative(value), "Invalid numeric Decisions answer");
    return value;
}
std::optional<std::string> optional_string(const json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end()) return {};
    const auto field = it.value();
    if (field.is_null()) return {};
    require(field.is_string(), "Invalid Decisions response identifier");
    return field.get<std::string>();
}
std::optional<std::map<std::string, double>> probabilities(const json& answer) {
    const auto it = answer.find("probabilities");
    if (it == answer.end()) return {};
    const auto field = it.value();
    if (field.is_null()) return {};
    require(field.is_object(), "Invalid Decisions probabilities");
    std::map<std::string, double> values;
    for (auto [name, element] : field.items()) {
        require(element.is_number(), "Invalid Decisions probability");
        const auto value = element.get<double>();
        require(probability(value), "Invalid Decisions probability");
        values.emplace(std::move(name), value);
    }
    return values;
}
DecisionsResponse parse_response(json document, const DecisionsRequest& request) {
    require(document.is_object(), "Invalid Decisions response document");
    DecisionsResponse result;
    result.model = nonempty_string(document.at("model"));
    result.id = optional_string(document, "id");
    result.provider = optional_string(document, "provider");
    const auto& answers = document.at("answers");
    require(answers.is_object() && answers.size() == request.questions.size(),
            "Decisions response question set mismatch");
    for (const auto& [name, question] : request.questions) {
        const auto& answer = answers.at(name);
        require(answer.is_object(), "Invalid Decisions answer");
        const auto type = nonempty_string(answer.at("type"));
        std::visit([&](const auto& q) {
            using T = std::decay_t<decltype(q)>;
            if constexpr (std::is_same_v<T, DecisionsNoulQuestion>) {
                require(type == "noul", "Decisions answer kind mismatch");
                auto number = optional_number(answer, "noul", true);
                require(number.has_value(), "Missing Decisions noul probability");
                result.answers.emplace(name, DecisionsNoulAnswer{*number});
            } else if constexpr (std::is_same_v<T, DecisionsChoiceQuestion>) {
                require(type == "choice", "Decisions answer kind mismatch");
                DecisionsChoiceAnswer parsed;
                parsed.choice = nonempty_string(answer.at("choice"));
                require(q.criteria.contains(parsed.choice), "Decisions choice is not a requested criterion");
                parsed.confidence = optional_number(answer, "confidence", true);
                parsed.probabilities = probabilities(answer);
                if (parsed.probabilities)
                    for (const auto& [key, value] : *parsed.probabilities) {
                        (void)value;
                        require(q.criteria.contains(key), "Unknown Decisions choice probability criterion");
                    }
                result.answers.emplace(name, std::move(parsed));
            } else {
                require(type == "score", "Decisions answer kind mismatch");
                DecisionsScoreAnswer parsed;
                auto number = optional_number(answer, "score", false);
                require(number && *number <= static_cast<double>(q.criteria.size() - 1),
                        "Decisions score is outside requested scale");
                parsed.score = *number;
                parsed.confidence = optional_number(answer, "confidence", true);
                parsed.probabilities = probabilities(answer);
                const auto valid_index = [&](const std::string& key) {
                    if (key.empty() || (key.size() > 1 && key.front() == '0')) return false;
                    std::size_t index = 0;
                    const auto [end, error] = std::from_chars(key.data(), key.data() + key.size(), index);
                    return error == std::errc{} && end == key.data() + key.size() && index < q.criteria.size();
                };
                if (parsed.probabilities)
                    for (const auto& [key, value] : *parsed.probabilities) {
                        (void)value;
                        require(valid_index(key), "Unknown Decisions score probability criterion");
                    }
                const auto legend = answer.find("legend");
                if (legend != answer.end() && !legend.value().is_null()) {
                    const auto field = legend.value();
                    require(field.is_object(), "Invalid Decisions score legend");
                    parsed.legend.emplace();
                    for (auto [name, element] : field.items()) {
                        require(valid_index(name) && guidance(element),
                                "Invalid Decisions score legend entry");
                        parsed.legend->emplace(std::move(name), std::move(element));
                    }
                }
                result.answers.emplace(name, std::move(parsed));
            }
        }, question);
    }
    auto usage = document.find("usage");
    if (usage != document.end() && !usage.value().is_null()) {
        auto field = usage.value();
        require(field.is_object(), "Invalid Decisions usage");
        const auto count = [&](const char* name) -> std::optional<std::uint64_t> {
            const auto it = field.find(name);
            if (it == field.end() || it.value().is_null()) return {};
            return natural(it.value());
        };
        result.usage.input_tokens = count("input_tokens");
        result.usage.output_tokens = count("output_tokens");
        result.usage.total_tokens = count("total_tokens");
        result.usage.cached_input_tokens = count("cached_input_tokens");
        result.cost_usd = optional_number(field, "cost", false);
        result.usage.reported = std::make_shared<const json>(std::move(field));
    }
    return result;
}
} // namespace

struct DecisionsConfig::Data {
    async::AsyncEndpoint endpoint;
    std::string default_model;
    std::vector<std::string> models;
    std::chrono::milliseconds timeout;
    std::size_t max_questions, max_criteria, max_request_bytes;
    async::RequestOptions transport;
};
DecisionsConfig::DecisionsConfig(std::shared_ptr<const Data> data) : data_(std::move(data)) {}
std::variant<DecisionsConfig, EndpointFailure> DecisionsConfig::from_json(const json& document) {
    try {
        closed(document, {"version", "endpoint", "allow_insecure_loopback", "default_model", "models", "limits"});
        require(natural(document.at("version")) == 1, "Unsupported Decisions config version");
        const auto url = nonempty_string(document.at("endpoint"));
        require(header_safe(url) && url.find('?') == std::string::npos &&
            url.find('#') == std::string::npos && async::has_explicit_http_scheme(url),
            "Invalid Decisions endpoint");
        require(document.at("allow_insecure_loopback").is_boolean(), "Invalid loopback policy");
        auto data = std::make_shared<Data>();
        data->endpoint = async::validate_credential_endpoint(url, true,
            document.at("allow_insecure_loopback").get<bool>());
        require(data->endpoint.prefix == "/api/alpha/decisions", "Invalid Decisions route");
        data->default_model = nonempty_string(document.at("default_model"));
        const auto& models = document.at("models");
        require(models.is_array() && !models.empty(), "Decisions models must be a nonempty array");
        for (const auto& model : models) {
            closed(model, {"id", "prompt_usd_per_million_tokens", "output_usd_per_million_tokens", "implicit_caching"});
            auto id = nonempty_string(model.at("id"));
            require(id == "typesafe/jev-1.13" || id == "typesafe/jev-1.13-20260917",
                    "Unsupported Decisions model");
            require(std::find(data->models.begin(), data->models.end(), id) == data->models.end(),
                    "Duplicate Decisions model");
            for (const char* name : {"prompt_usd_per_million_tokens", "output_usd_per_million_tokens"}) {
                require(model.at(name).is_number() && nonnegative(model.at(name).get<double>()),
                        "Invalid Decisions catalogue price");
            }
            require(model.at("implicit_caching").is_boolean() && !model.at("implicit_caching").get<bool>(),
                    "Current Decisions models do not support implicit caching");
            data->models.push_back(std::move(id));
        }
        require(std::find(data->models.begin(), data->models.end(), data->default_model) != data->models.end(),
                "Default Decisions model is not admitted");
        const auto& limits = document.at("limits");
        closed(limits, {"timeout_ms", "max_questions", "max_criteria", "max_request_bytes",
            "max_response_header_bytes", "max_response_body_bytes", "max_response_chunk_bytes"});
        const auto timeout = positive_size(limits.at("timeout_ms"));
        const auto max_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::duration::max()).count();
        require(timeout <= static_cast<std::uint64_t>(max_timeout), "Unrepresentable Decisions timeout");
        data->timeout = std::chrono::milliseconds(timeout);
        data->max_questions = positive_size(limits.at("max_questions"));
        data->max_criteria = positive_size(limits.at("max_criteria"));
        data->max_request_bytes = positive_size(limits.at("max_request_bytes"));
        data->transport.max_response_header_bytes = positive_size(limits.at("max_response_header_bytes"));
        data->transport.max_response_body_bytes = positive_size(limits.at("max_response_body_bytes"));
        data->transport.max_response_chunk_bytes = positive_size(limits.at("max_response_chunk_bytes"));
        data->transport.max_redirects = 0;
        data->transport.allow_replay = false;
        return DecisionsConfig(std::move(data));
    } catch (...) {
        return failure(EndpointFailureKind::InvalidConfig, "Invalid closed version-1 Decisions configuration");
    }
}

struct DecisionsClient::Impl {
    DecisionsConfig config;
    std::string credential;
    Impl(DecisionsConfig value, std::string key) : config(std::move(value)), credential(std::move(key)) {}
};
DecisionsClient::DecisionsClient(DecisionsConfig config, std::string credential) {
    if (!config.data_ || credential.empty() || !header_safe(credential))
        throw std::invalid_argument("Invalid Decisions client configuration or credential");
    impl_ = std::make_shared<const Impl>(std::move(config), std::move(credential));
}
asio::awaitable<DecisionsResult> DecisionsClient::submit_async(
    DecisionsRequest request, std::shared_ptr<graph::CancelToken> cancellation,
    std::optional<Clock::time_point> deadline) const {
    auto effective = deadline;
    if (!effective) {
        const auto now = Clock::now();
        const auto timeout = std::chrono::duration_cast<Clock::duration>(impl_->config.data_->timeout);
        effective = timeout > Clock::time_point::max() - now ? Clock::time_point::max() : now + timeout;
    }
    return submit_owned(impl_, std::move(request), std::move(cancellation), *effective);
}
asio::awaitable<DecisionsResult> DecisionsClient::submit_owned(
    std::shared_ptr<const Impl> impl, DecisionsRequest request,
    std::shared_ptr<graph::CancelToken> cancellation, Clock::time_point deadline) {
    bool sent = false;
    try {
        if (cancellation && cancellation->is_cancelled())
            co_return failure(EndpointFailureKind::Cancelled, "Decisions request cancelled");
        if (Clock::now() >= deadline)
            co_return failure(EndpointFailureKind::DeadlineExceeded, "Decisions deadline exceeded");
        const auto& config = *impl->config.data_;
        const auto& model = request.model ? *request.model : config.default_model;
        require(std::find(config.models.begin(), config.models.end(), model) != config.models.end(),
                "Decisions model is not admitted");
        require(guidance(request.state), "Decisions state must be string, object, or array");
        require(!request.questions.empty() && request.questions.size() <= config.max_questions,
                "Invalid Decisions question count");
        json body = {{"model", model}, {"state", request.state}, {"questions", json::object()}};
        for (const auto& [name, question] : request.questions) {
            require(!name.empty(), "Decisions question names must be nonempty");
            body["questions"][name] = std::visit([&](const auto& q) -> json {
                require(guidance(q.instructions), "Invalid Decisions question instructions");
                using T = std::decay_t<decltype(q)>;
                json value = {{"instructions", q.instructions}};
                if constexpr (std::is_same_v<T, DecisionsNoulQuestion>) {
                    value["type"] = "noul";
                    if (q.criteria) {
                        require(guidance(q.criteria->when_false) && guidance(q.criteria->when_true),
                                "Invalid Decisions noul criteria");
                        value["criteria"] = {{"false", q.criteria->when_false}, {"true", q.criteria->when_true}};
                    }
                } else if constexpr (std::is_same_v<T, DecisionsChoiceQuestion>) {
                    require(!q.criteria.empty() && q.criteria.size() <= config.max_criteria,
                            "Invalid Decisions choice criteria count");
                    for (const auto& [key, criterion] : q.criteria)
                        require(!key.empty() && (guidance(criterion) || criterion.is_null()),
                                "Invalid Decisions choice criterion");
                    value["type"] = "choice";
                    value["criteria"] = json::object();
                    for (const auto& [key, criterion] : q.criteria)
                        value["criteria"][key] = criterion;
                } else {
                    require(!q.criteria.empty() && q.criteria.size() <= config.max_criteria,
                            "Invalid Decisions score criteria count");
                    for (const auto& criterion : q.criteria)
                        require(guidance(criterion), "Invalid Decisions score criterion");
                    value["type"] = "score";
                    value["criteria"] = json::array();
                    for (const auto& criterion : q.criteria)
                        value["criteria"].push_back(criterion);
                }
                return value;
            }, question);
        }
        if (request.provider) body["provider"] = provider_json(*request.provider);
        const auto bounded = [&](const char* key, const std::optional<std::string>& value) {
            if (value) {
                require(codepoints(*value) <= 256, "Decisions named identifier exceeds 256 characters");
                body[key] = *value;
            }
        };
        bounded("session_id", request.session_id); bounded("user", request.user);
        if (request.trace) {
            require(request.trace->is_object(), "Decisions trace must be an object");
            for (const char* key : {"trace_id", "trace_name", "span_name", "generation_name", "parent_span_id"}) {
                const auto it = request.trace->find(key);
                require(it == request.trace->end() || it.value().is_string(), "Invalid named Decisions trace control");
            }
            body["trace"] = *request.trace;
        }
        validate_json_value(body);
        auto wire = body.dump();
        if (wire.size() > config.max_request_bytes)
            co_return failure(EndpointFailureKind::ResourceLimit, "Decisions request exceeds configured byte limit");
        auto executor = co_await asio::this_coro::executor;
        auto operation = cancellation ? cancellation->fork() : std::shared_ptr<graph::CancelToken>{};
        graph::CancelExecutorLease lease(operation);
        if (operation) {
            executor = operation->bind_executor(executor);
            co_await asio::post(executor, asio::use_awaitable);
            if (operation->is_cancelled())
                co_return failure(EndpointFailureKind::Cancelled, "Decisions request cancelled");
        }
        const auto now = Clock::now();
        if (now >= deadline)
            co_return failure(EndpointFailureKind::DeadlineExceeded, "Decisions deadline exceeded");
        auto options = config.transport;
        // Floor to milliseconds, never extend the caller's absolute deadline.
        options.timeout = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        if (options.timeout.count() <= 0)
            co_return failure(EndpointFailureKind::DeadlineExceeded, "Decisions deadline exceeded");
        std::vector<std::pair<std::string, std::string>> headers{
            {"Content-Type", "application/json"}, {"Authorization", "Bearer " + impl->credential}};
        auto pending = async::async_post(executor, config.endpoint.host, config.endpoint.port,
            config.endpoint.prefix, wire, std::move(headers), config.endpoint.tls, options);
        sent = true;
        async::HttpResponse response;
        if (operation)
            response = co_await asio::co_spawn(executor, std::move(pending),
                asio::bind_cancellation_slot(operation->slot(), asio::use_awaitable));
        else response = co_await std::move(pending);
        if (cancellation && cancellation->is_cancelled())
            co_return failure(EndpointFailureKind::Cancelled, "Decisions request cancelled", sent);
        if (Clock::now() >= deadline)
            co_return failure(EndpointFailureKind::DeadlineExceeded, "Decisions deadline exceeded", sent);
        if (response.status != 200) {
            auto error = failure(EndpointFailureKind::Provider, "Decisions provider rejected the request", sent, response.status);
            // Retain only an integer service code, never message/metadata/body.
            try {
                const auto document = json::parse(response.body);
                if (document.is_object() && document.contains("error")) {
                    const auto e = document.at("error");
                    if (e.is_object() && e.contains("code")) {
                        const auto code = e.at("code");
                        if (code.is_number_integer()) error.provider_code = code.dump();
                    }
                }
            } catch (const json::parse_error&) {
                // An optional service code cannot replace the original HTTP failure.
            }
            co_return error;
        }
        try {
            auto document = json::parse(response.body);
            co_return parse_response(std::move(document), request);
        } catch (...) {
            co_return failure(EndpointFailureKind::Protocol, "Invalid typed Decisions response", sent, response.status);
        }
    } catch (const AdmissionError& error) {
        co_return failure(error.kind, error.message, sent);
    } catch (const asio::system_error& error) {
        if (cancellation && cancellation->is_cancelled())
            co_return failure(EndpointFailureKind::Cancelled, "Decisions request cancelled", sent);
        if (error.code() == asio::error::timed_out || Clock::now() >= deadline)
            co_return failure(EndpointFailureKind::DeadlineExceeded, "Decisions deadline exceeded", sent);
        if (error.code() == asio::error::operation_aborted)
            co_return failure(EndpointFailureKind::Cancelled, "Decisions request cancelled", sent);
        co_return failure(EndpointFailureKind::Transport, "Decisions transport failed", sent);
    } catch (const std::exception&) {
        if (cancellation && cancellation->is_cancelled())
            co_return failure(EndpointFailureKind::Cancelled, "Decisions request cancelled", sent);
        if (Clock::now() >= deadline)
            co_return failure(EndpointFailureKind::DeadlineExceeded, "Decisions deadline exceeded", sent);
        co_return failure(sent ? EndpointFailureKind::Transport : EndpointFailureKind::InvalidRequest,
            sent ? "Decisions transport failed" : "Invalid Decisions request", sent);
    }
}

} // namespace neograph::llm
