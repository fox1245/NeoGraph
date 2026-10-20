// The built-in SchemaProvider endpoint nodes (#531). Each reads its inputs from channels, makes one call on the
// node context's provider, on the path a chat call takes (make_endpoint_request), and writes the result to one
// channel. A video job takes two nodes: job_submit writes the job's handle, and job_collect polls it once per run
// of the node, interrupting until the job has finished.
#include "endpoint_nodes.h"

#include <neograph/graph/cancel.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/graph/run_context.h>
#include <neograph/provider_outcome_codec.h>
#include <neograph/runtime_interposition_consumer.h>

#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace neograph::graph {
namespace {

// One node's configuration. A strict compile has already checked the types its schema declares; a lenient one
// has not, so every read checks again.
class Settings {
public:
    Settings(const json& config, std::string type, std::string node)
        : config_(config), type_(std::move(type)), node_(std::move(node)) {}
    [[noreturn]] void fail(const std::string& message) const {
        throw std::invalid_argument(type_ + " node '" + node_ + "': " + message);
    }
    std::string model() const {
        auto value = text("model");
        if (!value || value->empty()) fail("'model' must be a non-empty string");
        return *value;
    }
    std::string channel(const char* key, const char* fallback) const {
        auto value = text(key);
        if (value && value->empty()) fail(std::string("'") + key + "' must name a channel");
        return value ? *value : fallback;
    }
    std::optional<std::string> text(const char* key) const {
        if (!config_.contains(key) || config_[key].is_null()) return std::nullopt;
        if (!config_[key].is_string()) fail(std::string("'") + key + "' must be a string");
        return config_[key].get<std::string>();
    }
    std::optional<std::uint32_t> count(const char* key) const {
        if (!config_.contains(key) || config_[key].is_null()) return std::nullopt;
        const auto& value = config_[key];
        if (!value.is_number_integer() || value.get<std::int64_t>() < 1 ||
            value.get<std::int64_t>() > std::numeric_limits<std::uint32_t>::max())
            fail(std::string("'") + key + "' must be a positive integer");
        return static_cast<std::uint32_t>(value.get<std::int64_t>());
    }
    std::optional<double> number(const char* key) const {
        if (!config_.contains(key) || config_[key].is_null()) return std::nullopt;
        if (!config_[key].is_number()) fail(std::string("'") + key + "' must be a number");
        return config_[key].get<double>();
    }
    std::optional<bool> flag(const char* key) const {
        if (!config_.contains(key) || config_[key].is_null()) return std::nullopt;
        if (!config_[key].is_boolean()) fail(std::string("'") + key + "' must be a boolean");
        return config_[key].get<bool>();
    }
    // A JSON member as the SDK's document; `object` requires a JSON object.
    std::shared_ptr<const sp::json::Document> document(const json& value, const std::string& what,
                                                       bool object = false) const {
        if (object && !value.is_object()) fail(what + " must be an object");
        auto parsed = sp::json::parse(value.dump());
        auto* document = std::get_if<sp::json::Document>(&parsed);
        if (!document) fail(what + " is not admitted JSON");
        return std::make_shared<const sp::json::Document>(std::move(*document));
    }
    std::shared_ptr<const sp::json::Document> extensions() const {
        if (!config_.contains("extensions") || config_["extensions"].is_null()) return {};
        return document(config_["extensions"], "'extensions'", true);
    }
    json raw(const char* key) const { return config_[key]; }
    bool has(const char* key) const { return config_.contains(key) && !config_[key].is_null(); }

private:
    json config_;
    std::string type_, node_;
};

json plain(const std::shared_ptr<const sp::json::Document>& value) {
    return value ? json::parse(value->root().dump()) : json(nullptr);
}

// Media from a channel: a base64 data URL, an https URL, or the media JSON of the provider outcome format v3.
sp::Media media(const json& value, sp::MediaKind kind, const Settings& settings, const std::string& what) {
    if (value.is_object()) {
        try { return provider_codec::detail::parse_media(value); }
        catch (const std::exception&) { settings.fail(what + " is not provider media JSON"); }
    }
    if (!value.is_string()) settings.fail(what + " must be a data URL, a URL or provider media JSON");
    const auto text = value.get<std::string>();
    if (!text.starts_with("data:")) return sp::Media::url(kind, text);
    const auto separator = text.find(";base64,");
    if (separator == std::string::npos) settings.fail(what + " must be a base64 data URL");
    return sp::Media::inline_data(kind, text.substr(5, separator - 5),
                                  std::make_shared<const std::string>(text.substr(separator + 8)));
}
std::vector<sp::Media> media_list(const json& value, sp::MediaKind kind, const Settings& settings,
                                  const std::string& what) {
    std::vector<sp::Media> result;
    if (value.is_null()) return result;
    if (!value.is_array()) {
        result.push_back(media(value, kind, settings, what));
        return result;
    }
    for (const auto& item : value) result.push_back(media(item, kind, settings, what));
    return result;
}
json media_json(const std::vector<sp::Media>& items) {
    auto result = json::array();
    for (const auto& item : items) result.push_back(provider_codec::detail::media_part(item));
    return result;
}
// A string, or an array of strings.
std::vector<std::string> texts(const json& value, const Settings& settings, const std::string& what) {
    std::vector<std::string> result;
    if (value.is_string()) {
        result.push_back(value.get<std::string>());
        return result;
    }
    if (!value.is_array()) settings.fail(what + " must be a string or an array of strings");
    for (const auto& item : value) {
        if (!item.is_string()) settings.fail(what + " must be a string or an array of strings");
        result.push_back(item.get<std::string>());
    }
    return result;
}

json handle_json(const sp::jobs::Handle& handle) {
    return {{"endpoint", handle.endpoint}, {"origin", handle.origin}, {"id", handle.id}, {"model", handle.model}};
}
sp::jobs::Handle handle_from(const json& value, const Settings& settings) {
    const auto member = [&](const char* key) {
        if (!value.is_object() || !value.contains(key) || !value[key].is_string())
            settings.fail(std::string("the job channel must hold a job handle with a string '") + key + "'");
        return value[key].get<std::string>();
    };
    return sp::jobs::Handle{member("endpoint"), member("origin"), member("id"), member("model")};
}
const char* status_name(sp::jobs::Status status) {
    switch (status) {
    case sp::jobs::Status::Pending: return "pending";
    case sp::jobs::Status::Running: return "running";
    case sp::jobs::Status::Completed: return "completed";
    case sp::jobs::Status::Failed: return "failed";
    case sp::jobs::Status::Cancelled: return "cancelled";
    case sp::jobs::Status::Expired: return "expired";
    case sp::jobs::Status::Unknown: break;
    }
    return "unknown";
}

// What every endpoint node shares: the provider, the output channel, and the checks before a call.
class EndpointNode : public GraphNode, public ::neograph::RuntimeInterpositionConsumer {
public:
    EndpointNode(std::string name, const NodeContext& context, const Settings& settings, std::string type,
                 const char* output)
        : name_(std::move(name)), type_(std::move(type)), provider_(context.provider),
          output_(settings.channel("output", output)) {}
    std::string get_name() const override { return name_; }
    void set_runtime_interposition(std::shared_ptr<RuntimeInterpositionController> controller) override {
        interposed_ = controller != nullptr;
        RuntimeInterpositionConsumer::set_runtime_interposition(std::move(controller));
    }

protected:
    void admit() const {
        if (!provider_)
            throw std::invalid_argument(type_ + " node '" + name_ +
                                        "' has no provider; set NodeContext::provider before compiling");
        if (!provider_->has_endpoints())
            throw std::invalid_argument(type_ + " node '" + name_ + "': the provider has no SchemaProvider endpoints");
    }
    // One endpoint call on the path a chat call takes: the run's provider call broker (a Program's journal), its
    // token budget, which refuses a call it cannot bound, or the provider itself. Strict Runtime interposition
    // takes chat calls only, so it refuses the node rather than be bypassed.
    template <EndpointRequest Request>
    asio::awaitable<EndpointOutcomeOf<Request>> call(Request request, const RunContext& ctx) const {
        admit();
        if (interposed_)
            throw std::invalid_argument(type_ + " node '" + name_ +
                                        "': Strict Runtime interposition does not take endpoint calls");
        auto provider_request = make_endpoint_request(std::move(request));
        provider_request.cancel_token = ctx.cancel_token;
        provider_request.options.deadline = ctx.deadline;
        auto result = co_await invoke_provider(provider_, std::move(provider_request), {}, {},
                                               provider_call_broker(ctx), make_provider_call_identity(ctx, name_));
        if (!result) throw std::runtime_error("Provider returned no outcome");
        co_return sp::runtime::detail::typed_outcome(*result, static_cast<const EndpointOutcomeOf<Request>*>(nullptr));
    }
    // The call's result, with the token usage it reports recorded on the run; a failure throws ProviderFailure.
    // As for a chat call, a budget or a broker accounts for the call itself. A call that reports no usage
    // (moderation, a download) adds nothing, so it does not erase the totals of the run's other calls.
    template <class Result, class Outcome>
    Result take(Outcome outcome, const RunContext& ctx) const {
        if (auto* failure = std::get_if<sp::Failure>(&outcome))
            throw ProviderFailure(std::make_shared<const sp::Outcome>(std::move(*failure)));
        auto result = std::get<Result>(std::move(outcome));
        if constexpr (requires { result.usage; })
            if (ctx.usage && ctx.model_token_budget == 0 && !provider_call_broker(ctx) &&
                result.usage.stage != sp::UsageStage::Missing)
                ctx.usage->add(result.usage);
        return result;
    }
    NodeOutput write(json value) const {
        NodeOutput out;
        out.writes.push_back(ChannelWrite{output_, std::move(value)});
        return out;
    }
    json read(const NodeInput& in, const std::string& channel) const {
        auto value = in.state.has_channel(channel) ? in.state.get(channel) : json(nullptr);
        if (value.is_null())
            throw std::invalid_argument(type_ + " node '" + name_ + "': channel '" + channel + "' has no value");
        return value;
    }

    // A node instance serves every concurrent run of its engine, so it holds configuration only.
    std::string name_, type_;
    std::shared_ptr<Provider> provider_;
    std::string output_;
    bool interposed_ = false;
};

class EmbedNode final : public EndpointNode {
public:
    EmbedNode(std::string name, const NodeContext& context, const Settings& settings)
        : EndpointNode(std::move(name), context, settings, "embed", "embeddings"),
          input_(settings.channel("input", "input")) {
        request_.model = settings.model();
        request_.dimensions = settings.count("dimensions");
        request_.task = settings.text("task");
        request_.extensions = settings.extensions();
    }
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto request = request_;
        const Settings checks(json::object(), type_, name_);
        for (auto& text : texts(read(in, input_), checks, "channel '" + input_ + "'"))
            request.inputs.push_back({std::move(text)});
        auto result = take<sp::embeddings::Result>(
            co_await call(std::move(request), in.ctx), in.ctx);
        auto vectors = json::array();
        for (const auto& vector : result.vectors) {
            auto values = json::array();
            for (const auto value : vector) values.push_back(static_cast<double>(value));
            vectors.push_back(std::move(values));
        }
        co_return write(std::move(vectors));
    }
private:
    std::string input_;
    sp::embeddings::Request request_;
};

class RerankNode final : public EndpointNode {
public:
    RerankNode(std::string name, const NodeContext& context, const Settings& settings)
        : EndpointNode(std::move(name), context, settings, "rerank", "ranking"),
          query_(settings.channel("query", "query")), documents_(settings.channel("documents", "documents")) {
        request_.model = settings.model();
        request_.top = settings.count("top");
        request_.extensions = settings.extensions();
    }
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        const Settings checks(json::object(), type_, name_);
        auto request = request_;
        const auto query = read(in, query_);
        if (!query.is_string()) checks.fail("channel '" + query_ + "' must hold a string");
        request.query = query.get<std::string>();
        request.documents = texts(read(in, documents_), checks, "channel '" + documents_ + "'");
        auto result = take<sp::rerank::Result>(
            co_await call(std::move(request), in.ctx), in.ctx);
        auto ranking = json::array();
        for (const auto& ranked : result.ranked)
            ranking.push_back({{"index", ranked.index}, {"relevance", ranked.relevance}});
        co_return write(std::move(ranking));
    }
private:
    std::string query_, documents_;
    sp::rerank::Request request_;
};

class ModerateNode final : public EndpointNode {
public:
    ModerateNode(std::string name, const NodeContext& context, const Settings& settings)
        : EndpointNode(std::move(name), context, settings, "moderate", "moderation"),
          input_(settings.channel("input", "input")) {
        request_.model = settings.model();
        request_.extensions = settings.extensions();
    }
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto request = request_;
        const Settings checks(json::object(), type_, name_);
        for (auto& text : texts(read(in, input_), checks, "channel '" + input_ + "'"))
            request.inputs.push_back({std::move(text)});
        auto result = take<sp::moderation::Result>(
            co_await call(std::move(request), in.ctx), in.ctx);
        auto verdicts = json::array();
        for (const auto& verdict : result.verdicts) {
            auto categories = json::object(), scores = json::object();
            for (const auto& [category, applies] : verdict.categories) categories[category] = applies;
            for (const auto& [category, score] : verdict.scores) scores[category] = score;
            verdicts.push_back({{"flagged", verdict.flagged}, {"categories", std::move(categories)},
                                {"scores", std::move(scores)}});
        }
        co_return write(std::move(verdicts));
    }
private:
    std::string input_;
    sp::moderation::Request request_;
};

// image_generate, and image_edit, which also reads the images to edit.
class ImageNode final : public EndpointNode {
public:
    ImageNode(std::string name, const NodeContext& context, const Settings& settings, bool edit)
        : EndpointNode(std::move(name), context, settings, edit ? "image_edit" : "image_generate", "images"),
          prompt_(settings.channel("prompt", "prompt")),
          references_(edit ? settings.channel("references", "references") : std::string()) {
        request_.model = settings.model();
        request_.count = settings.count("count");
        request_.size = settings.text("size");
        request_.quality = settings.text("quality");
        request_.background = settings.text("background");
        request_.output_format = settings.text("output_format");
        request_.output_compression = settings.count("output_compression");
        request_.extensions = settings.extensions();
    }
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        const Settings checks(json::object(), type_, name_);
        auto request = request_;
        const auto prompt = read(in, prompt_);
        if (!prompt.is_string()) checks.fail("channel '" + prompt_ + "' must hold a string");
        request.prompt = prompt.get<std::string>();
        if (!references_.empty())
            request.references = media_list(read(in, references_), sp::MediaKind::Image, checks,
                                            "channel '" + references_ + "'");
        auto result = take<sp::images::Result>(
            co_await call(std::move(request), in.ctx), in.ctx);
        co_return write(media_json(result.images));
    }
private:
    std::string prompt_, references_;
    sp::images::Request request_;
};

class SpeakNode final : public EndpointNode {
public:
    SpeakNode(std::string name, const NodeContext& context, const Settings& settings)
        : EndpointNode(std::move(name), context, settings, "speak", "audio"),
          input_(settings.channel("input", "text")) {
        request_.model = settings.model();
        auto voice = settings.text("voice");
        if (!voice || voice->empty()) settings.fail("'voice' must be a non-empty string");
        request_.voice = *voice;
        request_.instructions = settings.text("instructions");
        request_.format = settings.text("format");
        request_.speed = settings.number("speed");
        request_.extensions = settings.extensions();
    }
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto request = request_;
        const auto text = read(in, input_);
        if (!text.is_string()) Settings(json::object(), type_, name_).fail("channel '" + input_ + "' must hold a string");
        request.input = text.get<std::string>();
        auto result = take<sp::speech::Result>(
            co_await call(std::move(request), in.ctx), in.ctx);
        co_return write(provider_codec::detail::media_part(result.audio));
    }
private:
    std::string input_;
    sp::speech::Request request_;
};

// transcribe, and translate, which turns the speech into English text.
class TranscribeNode final : public EndpointNode {
public:
    TranscribeNode(std::string name, const NodeContext& context, const Settings& settings, bool translate)
        : EndpointNode(std::move(name), context, settings, translate ? "translate" : "transcribe",
                       translate ? "translation" : "transcript"),
          input_(settings.channel("input", "audio")) {
        request_.model = settings.model();
        request_.translate = translate;
        if (!translate) request_.language = settings.text("language");
        request_.prompt = settings.text("prompt");
        request_.extensions = settings.extensions();
    }
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto request = request_;
        request.audio = media(read(in, input_), sp::MediaKind::Audio, Settings(json::object(), type_, name_),
                              "channel '" + input_ + "'");
        auto result = take<sp::transcription::Result>(
            co_await call(std::move(request), in.ctx), in.ctx);
        co_return write(result.text);
    }
private:
    std::string input_;
    sp::transcription::Request request_;
};

sp::decisions::Kind decision_kind(const json& value, const Settings& settings) {
    const auto name = value.is_string() ? value.get<std::string>() : std::string();
    if (name == "predicate") return sp::decisions::Kind::Predicate;
    if (name == "choice") return sp::decisions::Kind::Choice;
    if (name == "score") return sp::decisions::Kind::Score;
    if (name == "box") return sp::decisions::Kind::Box;
    settings.fail("a question's 'kind' must be predicate, choice, score or box");
}
const char* decision_kind_name(sp::decisions::Kind kind) {
    switch (kind) {
    case sp::decisions::Kind::Predicate: return "predicate";
    case sp::decisions::Kind::Choice: return "choice";
    case sp::decisions::Kind::Score: return "score";
    case sp::decisions::Kind::Box: return "box";
    }
    return "predicate";
}

class DecideNode final : public EndpointNode {
public:
    DecideNode(std::string name, const NodeContext& context, const Settings& settings)
        : EndpointNode(std::move(name), context, settings, "decide", "decisions"),
          input_(settings.channel("input", "input")) {
        request_.model = settings.model();
        request_.extensions = settings.extensions();
        if (!settings.has("questions") || !settings.raw("questions").is_array() ||
            settings.raw("questions").empty())
            settings.fail("'questions' must be a non-empty array");
        for (const auto& item : settings.raw("questions")) {
            if (!item.is_object() || !item.contains("instructions"))
                settings.fail("each question must be an object with 'instructions'");
            sp::decisions::Question question;
            if (item.contains("name")) {
                if (!item["name"].is_string()) settings.fail("a question's 'name' must be a string");
                question.name = item["name"].get<std::string>();
            }
            question.kind = decision_kind(item.contains("kind") ? item["kind"] : json("predicate"), settings);
            question.instructions = settings.document(item["instructions"], "a question's 'instructions'");
            if (item.contains("options")) {
                if (!item["options"].is_array()) settings.fail("a question's 'options' must be an array");
                for (const auto& option : item["options"]) {
                    if (option.is_string()) {
                        question.options.push_back({option.get<std::string>()});
                    } else if (option.is_object() && option.contains("value") && option["value"].is_string()) {
                        sp::decisions::Option typed{option["value"].get<std::string>()};
                        if (option.contains("guidance"))
                            typed.guidance = settings.document(option["guidance"], "an option's 'guidance'");
                        question.options.push_back(std::move(typed));
                    } else {
                        settings.fail("an option must be a string or an object with a string 'value'");
                    }
                }
            }
            if (item.contains("when_true"))
                question.when_true = settings.document(item["when_true"], "a question's 'when_true'");
            if (item.contains("when_false"))
                question.when_false = settings.document(item["when_false"], "a question's 'when_false'");
            request_.questions.push_back(std::move(question));
        }
    }
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto request = request_;
        request.input = Settings(json::object(), type_, name_).document(read(in, input_),
                                                                        "channel '" + input_ + "'");
        auto result = take<sp::decisions::Result>(
            co_await call(std::move(request), in.ctx), in.ctx);
        auto answers = json::array();
        for (const auto& answer : result.answers) {
            auto probabilities = json::object();
            for (const auto& [value, probability] : answer.probabilities) probabilities[value] = probability;
            const auto optional = [](const std::optional<double>& value) { return value ? json(*value) : json(nullptr); };
            answers.push_back({{"name", answer.name},
                               {"kind", answer.kind ? json(decision_kind_name(*answer.kind)) : json(nullptr)},
                               {"refused", answer.refused}, {"probability", optional(answer.probability)},
                               {"score", optional(answer.score)}, {"confidence", optional(answer.confidence)},
                               {"choice", answer.choice}, {"probabilities", std::move(probabilities)},
                               {"raw", plain(answer.raw)}});
        }
        co_return write(std::move(answers));
    }
private:
    std::string input_;
    sp::decisions::Request request_;
};

class JobSubmitNode final : public EndpointNode {
public:
    JobSubmitNode(std::string name, const NodeContext& context, const Settings& settings)
        : EndpointNode(std::move(name), context, settings, "job_submit", "job"),
          prompt_(settings.channel("prompt", "prompt")), references_(settings.text("references").value_or("")) {
        request_.model = settings.model();
        request_.duration = settings.count("duration");
        request_.aspect_ratio = settings.text("aspect_ratio");
        request_.resolution = settings.text("resolution");
        request_.size = settings.text("size");
        request_.audio = settings.flag("audio");
        request_.extensions = settings.extensions();
    }
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        const Settings checks(json::object(), type_, name_);
        auto request = request_;
        const auto prompt = read(in, prompt_);
        if (!prompt.is_string()) checks.fail("channel '" + prompt_ + "' must hold a string");
        request.prompt = prompt.get<std::string>();
        if (!references_.empty())
            request.references = media_list(read(in, references_), sp::MediaKind::Image, checks,
                                            "channel '" + references_ + "'");
        auto state = take<sp::jobs::State>(
            co_await call(std::move(request), in.ctx), in.ctx);
        co_return write(handle_json(state.handle));
    }
private:
    std::string prompt_, references_;
    sp::videos::Request request_;
};

// Polls a job once per run of the node: a finished job's outputs are downloaded, and a job still running
// interrupts the run with its handle and status until the host resumes it.
class JobCollectNode final : public EndpointNode {
public:
    JobCollectNode(std::string name, const NodeContext& context, const Settings& settings)
        : EndpointNode(std::move(name), context, settings, "job_collect", "video"),
          input_(settings.channel("input", "job")) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        const Settings checks(json::object(), type_, name_);
        auto handle = handle_from(read(in, input_), checks);
        auto state = take<sp::jobs::State>(
            co_await call(sp::jobs::Poll{handle}, in.ctx), in.ctx);
        switch (state.status) {
        case sp::jobs::Status::Completed: break;
        case sp::jobs::Status::Failed:
        case sp::jobs::Status::Cancelled:
        case sp::jobs::Status::Expired:
            throw std::runtime_error(type_ + " node '" + name_ + "': job " + handle.id + " " +
                                     status_name(state.status) +
                                     (state.raw_status.empty() ? "" : " (" + state.raw_status + ")"));
        default:
            throw NodeInterrupt("job " + handle.id + " is " + status_name(state.status),
                                json{{"job", handle_json(handle)}, {"status", status_name(state.status)},
                                     {"raw_status", state.raw_status}});
        }
        std::vector<sp::Media> outputs;
        for (std::size_t index = 0; index < state.outputs; ++index) {
            auto content = take<sp::jobs::Content>(
                co_await call(sp::jobs::Download{handle, index}, in.ctx), in.ctx);
            if (!content.media) checks.fail("job output " + std::to_string(index) + " came back without media");
            outputs.push_back(std::move(*content.media));
        }
        co_return write(media_json(outputs));
    }
private:
    std::string input_;
};

json schema(const char* description, json properties, std::vector<std::string> required = {"model"}) {
    auto names = json::array();
    for (auto& name : required) names.push_back(std::move(name));
    properties["output"] = {{"type", "string"}, {"description", "Channel the node writes."}};
    return {{"type", "object"}, {"description", description}, {"properties", std::move(properties)},
            {"required", std::move(names)}, {"additionalProperties", false}};
}
json text_property(const char* description) { return {{"type", "string"}, {"description", description}}; }
json count_property(const char* description) { return {{"type", "integer"}, {"description", description}}; }
json model_property() { return text_property("Model id, admitted by the provider's descriptor policy."); }
json extensions_property() {
    return {{"type", "object"}, {"description", "Request members the provider policy admits, sent as given."}};
}

template <class Node, class... Arguments>
NodeFactoryFn factory(const char* type, Arguments... arguments) {
    return [type, arguments...](const std::string& name, const json& config, const NodeContext& context)
        -> std::unique_ptr<GraphNode> {
        return std::make_unique<Node>(name, context, Settings(config, type, name), arguments...);
    };
}
}  // namespace

void register_endpoint_nodes(NodeFactory& factory_registry) {
    const auto images = [](bool edit) {
        auto properties = json{{"model", model_property()},
            {"prompt", text_property("Channel holding the prompt (a string). Default \"prompt\".")},
            {"count", count_property("How many images to make.")},
            {"size", text_property("Image size token the provider defines.")},
            {"quality", text_property("Quality token the provider defines.")},
            {"background", text_property("Background token the provider defines.")},
            {"output_format", text_property("Output format token the provider defines.")},
            {"output_compression", count_property("Output compression, 0 to 100.")},
            {"extensions", extensions_property()}};
        if (edit)
            properties["references"] = text_property(
                "Channel holding the images to edit: a data URL, a URL, provider media JSON, or an array of them. "
                "Default \"references\".");
        return properties;
    };
    factory_registry.register_type("image_generate", factory<ImageNode>("image_generate", false),
        schema("Generates images. Writes provider media JSON (default channel \"images\").", images(false)));
    factory_registry.register_type("image_edit", factory<ImageNode>("image_edit", true),
        schema("Edits images. Writes provider media JSON (default channel \"images\").", images(true)));
    factory_registry.register_type("embed", factory<EmbedNode>("embed"),
        schema("Embeds the text of a channel (a string or an array of strings). Writes one vector per input "
               "(default channel \"embeddings\").",
               {{"model", model_property()},
                {"input", text_property("Channel holding the text. Default \"input\".")},
                {"dimensions", count_property("Output dimensions, for models that can shorten their vectors.")},
                {"task", text_property("What the vectors are for, a token the provider defines.")},
                {"extensions", extensions_property()}}));
    factory_registry.register_type("rerank", factory<RerankNode>("rerank"),
        schema("Ranks documents against a query. Writes [{index, relevance}], most relevant first "
               "(default channel \"ranking\").",
               {{"model", model_property()},
                {"query", text_property("Channel holding the query (a string). Default \"query\".")},
                {"documents", text_property("Channel holding the documents (an array of strings). "
                                            "Default \"documents\".")},
                {"top", count_property("How many results to return; all when unset.")},
                {"extensions", extensions_property()}}));
    factory_registry.register_type("moderate", factory<ModerateNode>("moderate"),
        schema("Checks text (a string or an array of strings). Writes [{flagged, categories, scores}] "
               "(default channel \"moderation\").",
               {{"model", model_property()},
                {"input", text_property("Channel holding the text. Default \"input\".")},
                {"extensions", extensions_property()}}));
    factory_registry.register_type("speak", factory<SpeakNode>("speak"),
        schema("Speaks text. Writes provider media JSON of the audio (default channel \"audio\").",
               {{"model", model_property()},
                {"input", text_property("Channel holding the text (a string). Default \"text\".")},
                {"voice", text_property("Voice the provider defines.")},
                {"instructions", text_property("How to speak, for models that take it.")},
                {"format", text_property("Audio format token the provider defines.")},
                {"speed", {{"type", "number"}, {"description", "Speaking speed."}}},
                {"extensions", extensions_property()}},
               {"model", "voice"}));
    const auto speech_input = text_property(
        "Channel holding the audio: a data URL, a URL or provider media JSON. Default \"audio\".");
    factory_registry.register_type("transcribe", factory<TranscribeNode>("transcribe", false),
        schema("Transcribes speech. Writes the text (default channel \"transcript\").",
               {{"model", model_property()}, {"input", speech_input},
                {"language", text_property("The spoken language, when known.")},
                {"prompt", text_property("Text that guides the transcript.")},
                {"extensions", extensions_property()}}));
    factory_registry.register_type("translate", factory<TranscribeNode>("translate", true),
        schema("Translates speech into English text. Writes the text (default channel \"translation\").",
               {{"model", model_property()}, {"input", speech_input},
                {"prompt", text_property("Text that guides the translation.")},
                {"extensions", extensions_property()}}));
    factory_registry.register_type("decide", factory<DecideNode>("decide"),
        schema("Answers structured questions about the JSON of a channel. Writes the answers "
               "(default channel \"decisions\").",
               {{"model", model_property()},
                {"input", text_property("Channel holding the evidence (any JSON). Default \"input\".")},
                {"questions", {{"type", "array"}, {"description",
                    "Questions: {name, kind (predicate, choice, score or box), instructions, options, "
                    "when_true, when_false}."}}},
                {"extensions", extensions_property()}},
               {"model", "questions"}));
    factory_registry.register_type("job_submit", factory<JobSubmitNode>("job_submit"),
        schema("Submits a video job. Writes the job's handle (default channel \"job\").",
               {{"model", model_property()},
                {"prompt", text_property("Channel holding the prompt (a string). Default \"prompt\".")},
                {"references", text_property("Channel holding reference images, if any.")},
                {"duration", count_property("Seconds of video.")},
                {"aspect_ratio", text_property("Aspect ratio token the provider defines.")},
                {"resolution", text_property("Resolution token the provider defines.")},
                {"size", text_property("Size token the provider defines.")},
                {"audio", {{"type", "boolean"}, {"description", "Whether the video has sound."}}},
                {"extensions", extensions_property()}}));
    factory_registry.register_type("job_collect", factory<JobCollectNode>("job_collect"),
        schema("Polls a job once. A finished job's outputs are written as provider media JSON (default channel "
               "\"video\"); a job still running interrupts the run with its handle and status.",
               {{"input", text_property("Channel holding the job handle. Default \"job\".")}}, {}));
}

}  // namespace neograph::graph
