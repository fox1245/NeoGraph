#include <neograph/llm/veo_client.h>
#include <neograph/async/http_client.h>
#include <neograph/graph/cancel.h>

#include <asio/async_result.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/co_spawn.hpp>
#include <asio/error.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/system_error.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace neograph::llm {
namespace veo_detail {
using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;
struct Client {
    std::string origin, host, port, api_path, key, default_model;
    bool tls = true, adult_only = false;
    std::vector<std::string> models;
    std::vector<std::string> download_origins;
    unsigned download_redirects = 0;
    VeoAspectRatio aspect;
    VeoPersonGeneration person;
    VeoResolution resolution;
    unsigned duration;
    Ms deadline, interval;
    std::size_t prompt_limit, image_limit, json_limit, video_limit;
    std::uint64_t query_limit;
};
struct Operation {
    std::shared_ptr<const Client> client;
    std::shared_ptr<graph::CancelToken> cancellation;
    Clock::time_point deadline;
    std::string name, model;
    std::atomic<bool> busy{false};
    VeoResult result;
};
namespace {
EndpointFailure failure(EndpointFailureKind kind, const char* message,
                        bool dispatched = false, int status = 0) {
    return {kind, message, status, {}, dispatched};
}
void closed(const json& value, std::initializer_list<const char*> keys) {
    if (!value.is_object() || value.size() != keys.size())
        throw std::invalid_argument("Invalid Veo configuration object");
    for (const char* key : keys)
        if (!value.contains(key)) throw std::invalid_argument("Invalid Veo configuration member");
}
std::uint64_t positive(const json& value, std::uint64_t max) {
    if ((!value.is_number_unsigned() && !value.is_number_integer()) ||
        (value.is_number_integer() && !value.is_number_unsigned() && value.get<std::int64_t>() <= 0))
        throw std::invalid_argument("Invalid Veo positive limit");
    const auto n = value.get<std::uint64_t>();
    if (!n || n > max) throw std::invalid_argument("Invalid Veo limit range");
    return n;
}
bool model_supported(const std::string& value) {
    return value == "veo-3.1-lite-generate-preview" ||
           value == "veo-3.1-generate-preview" || value == "veo-3.1-fast-generate-preview";
}
bool component(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}
bool path_valid(std::string_view path) {
    if (path.empty() || path.front() != '/' || path.find("..") != path.npos ||
        path.find("//") != path.npos) return false;
    return std::all_of(path.begin(), path.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '_' || c == ':';
    });
}
std::shared_ptr<const Client> admit(const json& cfg, std::string key) {
    try {
        closed(cfg, {"version", "endpoint", "models", "defaults", "limits", "adult_only_region", "download"});
        if (!cfg.at("version").is_number_integer() || cfg.at("version") != 1 ||
            !cfg.at("adult_only_region").is_boolean() || key.empty() ||
            std::any_of(key.begin(), key.end(), [](unsigned char x) { return x <= 32 || x >= 127; }))
            throw std::invalid_argument("Invalid Veo config or credentials");
        auto c = std::make_shared<Client>();
        c->key = std::move(key);
        c->adult_only = cfg.at("adult_only_region").get<bool>();
        const auto& e = cfg.at("endpoint");
        closed(e, {"origin", "api_path", "allow_insecure_loopback"});
        c->origin = e.at("origin").get<std::string>();
        c->api_path = e.at("api_path").get<std::string>();
        const bool insecure = e.at("allow_insecure_loopback").get<bool>();
        std::string authority;
        if (c->origin.rfind("https://", 0) == 0) {
            authority = c->origin.substr(8);
            c->port = "443";
        } else if (c->origin.rfind("http://", 0) == 0 && insecure) {
            authority = c->origin.substr(7);
            c->tls = false;
            c->port = "80";
        } else throw std::invalid_argument("Invalid Veo origin scheme");
        const auto colon = authority.find(':');
        c->host = authority.substr(0, colon);
        if (colon != authority.npos) c->port = authority.substr(colon + 1);
        if ((!c->tls && c->host != "127.0.0.1" && c->host != "localhost") ||
            (c->tls && c->host != "generativelanguage.googleapis.com" &&
             c->host != "127.0.0.1" && c->host != "localhost") ||
            c->port.empty() || c->port.size() > 5 ||
            !std::all_of(c->port.begin(), c->port.end(), [](char x) { return x >= '0' && x <= '9'; }) ||
            std::stoul(c->port) == 0 || std::stoul(c->port) > 65535 ||
            !path_valid(c->api_path) || c->api_path.back() == '/')
            throw std::invalid_argument("Invalid Veo endpoint");
        const auto& models = cfg.at("models");
        if (!models.is_array() || models.empty()) throw std::invalid_argument("Invalid Veo models");
        for (const auto& m : models) {
            auto name = m.get<std::string>();
            if (!model_supported(name) || std::find(c->models.begin(), c->models.end(), name) != c->models.end())
                throw std::invalid_argument("Invalid Veo model catalogue");
            c->models.push_back(std::move(name));
        }
        const auto& d = cfg.at("defaults");
        closed(d, {"model", "aspect_ratio", "person_generation", "duration_seconds", "resolution"});
        c->default_model = d.at("model").get<std::string>();
        if (std::find(c->models.begin(), c->models.end(), c->default_model) == c->models.end())
            throw std::invalid_argument("Invalid Veo default model");
        const auto aspect = d.at("aspect_ratio").get<std::string>();
        if (aspect != "16:9" && aspect != "9:16") throw std::invalid_argument("Invalid Veo aspect");
        c->aspect = aspect == "16:9" ? VeoAspectRatio::Landscape : VeoAspectRatio::Portrait;
        const auto person = d.at("person_generation").get<std::string>();
        if (person != "allow_all" && person != "allow_adult") throw std::invalid_argument("Invalid Veo person policy");
        c->person = person == "allow_all" ? VeoPersonGeneration::AllowAll : VeoPersonGeneration::AllowAdult;
        if (c->adult_only && c->person != VeoPersonGeneration::AllowAdult)
            throw std::invalid_argument("Invalid Veo regional person policy");
        c->duration = static_cast<unsigned>(positive(d.at("duration_seconds"), 8));
        const auto resolution = d.at("resolution").get<std::string>();
        if (resolution != "720p" && resolution != "1080p" && resolution != "4k")
            throw std::invalid_argument("Invalid Veo resolution");
        c->resolution = resolution == "720p" ? VeoResolution::P720 :
                        resolution == "1080p" ? VeoResolution::P1080 : VeoResolution::P4K;
        if ((c->duration != 4 && c->duration != 6 && c->duration != 8) ||
            (c->resolution != VeoResolution::P720 && c->duration != 8) ||
            (c->resolution == VeoResolution::P4K && c->default_model == "veo-3.1-lite-generate-preview"))
            throw std::invalid_argument("Invalid Veo default combination");
        const auto& l = cfg.at("limits");
        closed(l, {"deadline_ms", "poll_interval_ms", "max_prompt_bytes", "max_image_bytes",
                   "max_json_bytes", "max_video_bytes", "max_status_queries"});
        c->deadline = Ms(positive(l.at("deadline_ms"), 86400000));
        c->interval = Ms(positive(l.at("poll_interval_ms"), 86400000));
        c->prompt_limit = positive(l.at("max_prompt_bytes"), 1024 * 1024);
        c->image_limit = positive(l.at("max_image_bytes"), 64 * 1024 * 1024);
        c->json_limit = positive(l.at("max_json_bytes"), 256 * 1024 * 1024);
        c->video_limit = positive(l.at("max_video_bytes"), 1024ULL * 1024 * 1024);
        c->query_limit = positive(l.at("max_status_queries"), 1000000);
        const auto& download = cfg.at("download");
        closed(download, {"allowed_origins", "max_redirects"});
        c->download_redirects = static_cast<unsigned>(positive(download.at("max_redirects"), 10));
        const auto& origins = download.at("allowed_origins");
        if (!origins.is_array() || origins.empty())
            throw std::invalid_argument("Invalid Veo download origin allowlist");
        for (const auto& item : origins) {
            auto origin = item.get<std::string>();
            if (origin != c->origin && origin.rfind("https://", 0) != 0)
                throw std::invalid_argument("Invalid Veo download origin scheme");
            const auto authority_start = origin.find("://") + 3;
            const auto authority = origin.substr(authority_start);
            const auto separator = authority.find(':');
            const auto hostname = authority.substr(0, separator);
            if (hostname.empty() || hostname.front() == '.' || hostname.back() == '.' ||
                (separator != authority.npos && authority.find(':', separator + 1) != authority.npos))
                throw std::invalid_argument("Invalid Veo download host");
            if (separator != authority.npos) {
                const auto port = authority.substr(separator + 1);
                if (port.empty() || port.size() > 5 ||
                    !std::all_of(port.begin(), port.end(), [](char x) { return x >= '0' && x <= '9'; }) ||
                    std::stoul(port) == 0 || std::stoul(port) > 65535)
                    throw std::invalid_argument("Invalid Veo download port");
            }
            if (authority.empty() || !std::all_of(authority.begin(), authority.end(), [](unsigned char x) {
                    return (x >= 'a' && x <= 'z') || (x >= '0' && x <= '9') || x == '.' || x == '-' || x == ':';
                }) || authority.front() == ':' || authority.find("..") != authority.npos ||
                std::find(c->download_origins.begin(), c->download_origins.end(), origin) != c->download_origins.end())
                throw std::invalid_argument("Invalid Veo download origin");
            c->download_origins.push_back(std::move(origin));
        }
        if (std::find(c->download_origins.begin(), c->download_origins.end(), c->origin) == c->download_origins.end())
            throw std::invalid_argument("Veo API origin must be admitted for downloads");
        return c;
    } catch (const std::exception&) {
        throw std::invalid_argument("Invalid Veo configuration or credentials");
    }
}
const char* aspect_name(VeoAspectRatio x) {
    switch (x) { case VeoAspectRatio::Landscape: return "16:9";
                 case VeoAspectRatio::Portrait: return "9:16"; }
    return nullptr;
}
const char* person_name(VeoPersonGeneration x) {
    switch (x) { case VeoPersonGeneration::AllowAll: return "allow_all";
                 case VeoPersonGeneration::AllowAdult: return "allow_adult";
                 case VeoPersonGeneration::DontAllow: return "dont_allow"; }
    return nullptr;
}
const char* resolution_name(VeoResolution x) {
    switch (x) { case VeoResolution::P720: return "720p";
                 case VeoResolution::P1080: return "1080p";
                 case VeoResolution::P4K: return "4k"; }
    return nullptr;
}
std::string base64(const std::vector<std::byte>& data) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out((data.size() + 2) / 3 * 4, '=');
    for (std::size_t i = 0, j = 0; i < data.size(); i += 3, j += 4) {
        const auto a = std::to_integer<unsigned>(data[i]);
        const auto b = i + 1 < data.size() ? std::to_integer<unsigned>(data[i + 1]) : 0;
        const auto c = i + 2 < data.size() ? std::to_integer<unsigned>(data[i + 2]) : 0;
        out[j] = alphabet[a >> 2]; out[j + 1] = alphabet[((a & 3) << 4) | (b >> 4)];
        if (i + 1 < data.size()) out[j + 2] = alphabet[((b & 15) << 2) | (c >> 6)];
        if (i + 2 < data.size()) out[j + 3] = alphabet[c & 63];
    }
    return out;
}
json image_json(const VeoImage& image) {
    return {{"mimeType", image.mime_type}, {"bytesBase64Encoded", base64(*image.bytes)}};
}
std::shared_ptr<const std::vector<std::byte>> inline_video(std::string_view text, std::size_t limit) {
    if (text.empty() || text.size() % 4) throw std::runtime_error("video base64");
    const std::size_t padding = (text.back() == '=') + (text[text.size() - 2] == '=');
    const auto size = text.size() / 4 * 3 - padding;
    if (size > limit || size < 12) throw std::runtime_error("video size");
    auto output = std::make_shared<std::vector<std::byte>>(size);
    auto digit = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    for (std::size_t i = 0, j = 0; i < text.size(); i += 4, j += 3) {
        const int a = digit(text[i]), b = digit(text[i + 1]);
        const int c = text[i + 2] == '=' ? 0 : digit(text[i + 2]);
        const int d = text[i + 3] == '=' ? 0 : digit(text[i + 3]);
        const bool final = i + 4 == text.size();
        if (a < 0 || b < 0 || c < 0 || d < 0 ||
            (!final && (text[i + 2] == '=' || text[i + 3] == '=')) ||
            (text[i + 2] == '=' && text[i + 3] != '=') ||
            (final && padding == 2 && (b & 15)) || (final && padding == 1 && (c & 3)))
            throw std::runtime_error("video base64");
        (*output)[j] = std::byte((a << 2) | (b >> 4));
        if (j + 1 < size) (*output)[j + 1] = std::byte((b << 4) | (c >> 2));
        if (j + 2 < size) (*output)[j + 2] = std::byte((c << 6) | d);
    }
    if (std::memcmp(output->data() + 4, "ftyp", 4) != 0)
        throw std::runtime_error("inline video MP4");
    return output;
}
std::optional<EndpointFailure> request_body(const Client& c, const VeoRequest& r,
                                           std::string& model, std::string& body) {
    model = r.model.value_or(c.default_model);
    const auto aspect = r.aspect_ratio.value_or(c.aspect);
    const auto person = r.person_generation.value_or(c.person);
    const auto duration = r.duration_seconds.value_or(c.duration);
    const auto resolution = r.resolution.value_or(c.resolution);
    const bool lite = model == "veo-3.1-lite-generate-preview";
    if (std::find(c.models.begin(), c.models.end(), model) == c.models.end() || r.extension_video ||
        r.number_of_videos != 1 || person == VeoPersonGeneration::DontAllow ||
        (lite && (resolution == VeoResolution::P4K || !r.reference_images.empty())))
        return failure(EndpointFailureKind::Unsupported, "Unsupported Veo model or capability");
    if (r.prompt.empty() || r.prompt.size() > c.prompt_limit || !aspect_name(aspect) ||
        !person_name(person) || !resolution_name(resolution) ||
        (duration != 4 && duration != 6 && duration != 8) ||
        (resolution != VeoResolution::P720 && duration != 8) ||
        (r.last_frame && !r.image) || r.reference_images.size() > 3 ||
        (!r.reference_images.empty() && (duration != 8 || r.image || r.last_frame)) ||
        ((r.image || !r.reference_images.empty() || c.adult_only) && person != VeoPersonGeneration::AllowAdult) ||
        (!r.image && r.reference_images.empty() && !c.adult_only && person != VeoPersonGeneration::AllowAll))
        return failure(EndpointFailureKind::InvalidRequest, "Invalid Veo controls or input combination");
    auto valid_image = [&](const VeoImage& image) {
        return image.bytes && !image.bytes->empty() && image.bytes->size() <= c.image_limit &&
               (image.mime_type == "image/png" || image.mime_type == "image/jpeg");
    };
    if ((r.image && !valid_image(*r.image)) || (r.last_frame && !valid_image(*r.last_frame)) ||
        !std::all_of(r.reference_images.begin(), r.reference_images.end(), valid_image))
        return failure(EndpointFailureKind::InvalidRequest, "Invalid Veo image input");
    json instance = {{"prompt", r.prompt}};
    if (r.image) instance["image"] = image_json(*r.image);
    if (r.last_frame) instance["lastFrame"] = image_json(*r.last_frame);
    if (!r.reference_images.empty()) {
        instance["referenceImages"] = json::array();
        for (const auto& image : r.reference_images)
            instance["referenceImages"].push_back({{"image", image_json(image)}, {"referenceType", "asset"}});
    }
    body = json{{"instances", json::array({std::move(instance)})},
                {"parameters", {{"aspectRatio", aspect_name(aspect)}, {"personGeneration", person_name(person)},
                                {"durationSeconds", duration}, {"resolution", resolution_name(resolution)},
                                {"sampleCount", 1}}}}.dump();
    return {};
}
void fail(Operation& op, EndpointFailure f) {
    op.result.status = f.kind == EndpointFailureKind::Cancelled ? VeoStatus::Cancelled :
                       f.kind == EndpointFailureKind::DeadlineExceeded ? VeoStatus::DeadlineExceeded : VeoStatus::Failed;
    op.result.failure = std::move(f);
    op.result.artifacts.clear();
}
std::optional<EndpointFailure> checkpoint(const Operation& op) {
    const bool sent = op.result.generation_dispatches != 0;
    if (op.cancellation->is_cancelled())
        return failure(EndpointFailureKind::Cancelled, "Veo operation cancelled locally", sent);
    if (Clock::now() >= op.deadline)
        return failure(EndpointFailureKind::DeadlineExceeded, "Veo operation deadline exceeded", sent);
    return {};
}
async::RequestOptions options(const Operation& op, bool video = false) {
    async::RequestOptions opts;
    opts.timeout = std::max(Ms(1), std::chrono::ceil<Ms>(op.deadline - Clock::now()));
    opts.allow_replay = false;
    opts.max_redirects = 0;
    opts.max_response_body_bytes = video ? op.client->video_limit : op.client->json_limit;
    return opts;
}
std::vector<std::pair<std::string, std::string>> headers(const Client& c) {
    return {{"x-goog-api-key", c.key}, {"Content-Type", "application/json"}};
}
std::optional<std::string> video_path(const Client& c, const std::string& uri) {
    if (uri.rfind(c.origin + '/', 0) != 0 || uri.find(c.key) != uri.npos) return {};
    auto path = uri.substr(c.origin.size());
    const auto question = path.find('?');
    if (question != path.npos) {
        if (path.substr(question) != "?alt=media") return {};
        if (!path_valid(std::string_view(path).substr(0, question))) return {};
    } else if (!path_valid(path)) return {};
    // The provider owns file IDs, but not a free-form authenticated GET authority.
    if (path.rfind(c.api_path + "/files/", 0) != 0 &&
        path.rfind("/download" + c.api_path + "/files/", 0) != 0) return {};
    return path;
}
struct DownloadTarget {
    std::string origin, host, port, path;
    bool tls = true;
};
std::optional<DownloadTarget> download_target(const Client& c, const std::string& uri,
                                              const std::string& previous_origin = {}) {
    std::string absolute;
    if (!uri.empty() && uri.front() == '/' && uri.rfind("//", 0) != 0 && !previous_origin.empty())
        absolute = previous_origin + uri;
    else absolute = uri;
    if (absolute.find(c.key) != absolute.npos || absolute.find('#') != absolute.npos ||
        absolute.find('\\') != absolute.npos ||
        !std::all_of(absolute.begin(), absolute.end(), [](unsigned char x) { return x > 32 && x < 127; }))
        return {};
    const bool tls = absolute.rfind("https://", 0) == 0;
    const bool local = absolute.rfind(c.origin + '/', 0) == 0 && !c.tls;
    if (!tls && !local) return {};
    const auto authority_start = tls ? 8u : 7u;
    const auto slash = absolute.find('/', authority_start);
    if (slash == absolute.npos) return {};
    DownloadTarget target;
    target.origin = absolute.substr(0, slash);
    if (std::find(c.download_origins.begin(), c.download_origins.end(), target.origin) == c.download_origins.end())
        return {};
    const auto authority = absolute.substr(authority_start, slash - authority_start);
    const auto colon = authority.find(':');
    target.host = authority.substr(0, colon);
    target.port = colon == authority.npos ? (tls ? "443" : "80") : authority.substr(colon + 1);
    if (target.port.empty() || target.port.size() > 5 ||
        !std::all_of(target.port.begin(), target.port.end(), [](char x) { return x >= '0' && x <= '9'; }) ||
        std::stoul(target.port) == 0 || std::stoul(target.port) > 65535) return {};
    target.tls = tls;
    target.path = absolute.substr(slash);
    return target;
}
void decode_status(Operation& op, const async::HttpResponse& response, bool submission) {
    if (response.status < 200 || response.status >= 300) {
        fail(op, failure(EndpointFailureKind::Provider, "Veo HTTP request failed", true, response.status));
        return;
    }
    try {
        const auto value = json::parse(response.body);
        if (!value.is_object()) throw std::runtime_error("object");
        if (value.contains("error")) {
            auto f = failure(EndpointFailureKind::Provider, "Veo provider reported failure", true, response.status);
            // Never copy the provider's message: it may echo credentials or URLs.
            if (value.at("error").is_object() && value.at("error").contains("code")) {
                const auto& code = value.at("error").at("code");
                if (code.is_number_integer()) f.provider_code = code.dump();
            }
            fail(op, std::move(f)); return;
        }
        if (value.contains("name")) {
            const auto name = value.at("name").get<std::string>();
            const auto prefix = "models/" + op.model + "/operations/";
            if (name.rfind(prefix, 0) != 0 || !component(std::string_view(name).substr(prefix.size())))
                throw std::runtime_error("operation authority");
            if (!op.name.empty() && op.name != name) throw std::runtime_error("operation mismatch");
            if (submission) op.name = name;
        } else if (submission) throw std::runtime_error("operation name");
        if (!value.contains("done")) return; // protobuf JSON omits default false.
        if (!value.at("done").is_boolean()) throw std::runtime_error("done type");
        if (!value.at("done").get<bool>()) return;
        const auto& generated = value.at("response").at("generateVideoResponse");
        const auto& samples = generated.at("generatedSamples");
        if (!samples.is_array() || samples.size() != 1) throw std::runtime_error("missing video");
        const auto& video = samples.at(0).at("video");
        if (!video.is_object()) throw std::runtime_error("video type");
        EndpointArtifact artifact;
        if (video.contains("uri")) {
            artifact.uri = video.at("uri").get<std::string>();
            if (!video_path(*op.client, artifact.uri)) throw std::runtime_error("video origin");
        }
        if (video.contains("encodedVideo"))
            artifact.bytes = inline_video(video.at("encodedVideo").get<std::string_view>(), op.client->video_limit);
        if (artifact.uri.empty() && !artifact.bytes) throw std::runtime_error("missing video");
        if (video.contains("encoding")) artifact.mime_type = video.at("encoding").get<std::string>();
        else if (video.contains("mimeType")) artifact.mime_type = video.at("mimeType").get<std::string>();
        else artifact.mime_type = "video/mp4"; // Veo returns MP4 even if MIME is omitted.
        if (artifact.mime_type != "video/mp4") throw std::runtime_error("video MIME");
        if (video.contains("fileId")) artifact.file_id = video.at("fileId").get<std::string>();
        // Retain admitted metadata, not arbitrary provider JSON containing unsafe URLs.
        json metadata = {{"mimeType", artifact.mime_type}, {"fileId", artifact.file_id}};
        if (video.contains("durationSeconds")) {
            const auto& duration = video.at("durationSeconds");
            if (!duration.is_number_integer() || duration.get<std::int64_t>() < 0)
                throw std::runtime_error("video duration");
            metadata["durationSeconds"] = duration;
        }
        artifact.metadata = std::make_shared<const json>(std::move(metadata));
        if (value.at("response").contains("usageMetadata")) {
            const auto& usage = value.at("response").at("usageMetadata");
            if (!usage.is_object()) throw std::runtime_error("usage type");
            auto count = [&](const char* field) -> std::optional<std::uint64_t> {
                if (!usage.contains(field)) return {};
                const auto& n = usage.at(field);
                if ((!n.is_number_unsigned() && !n.is_number_integer()) ||
                    (n.is_number_integer() && !n.is_number_unsigned() && n.get<std::int64_t>() < 0))
                    throw std::runtime_error("usage count");
                return n.get<std::uint64_t>();
            };
            op.result.usage.input_tokens = count("promptTokenCount");
            op.result.usage.output_tokens = count("candidatesTokenCount");
            op.result.usage.total_tokens = count("totalTokenCount");
            op.result.usage.cached_input_tokens = count("cachedContentTokenCount");
            op.result.usage.reported = std::make_shared<const json>(usage);
        }
        op.result.artifacts.push_back(std::move(artifact));
        op.result.status = VeoStatus::Succeeded;
    } catch (const std::exception&) {
        fail(op, failure(EndpointFailureKind::Protocol, "Invalid Veo operation response", true, response.status));
    }
}
void transport_failure(Operation& op, const std::exception& e) {
    if (const auto f = checkpoint(op)) { fail(op, *f); return; }
    const auto* system = dynamic_cast<const asio::system_error*>(&e);
    if (system && system->code() == asio::error::operation_aborted)
        fail(op, failure(EndpointFailureKind::Cancelled, "Veo request cancelled", true));
    else if (system && system->code() == asio::error::timed_out)
        fail(op, failure(EndpointFailureKind::DeadlineExceeded, "Veo operation deadline exceeded", true));
    else fail(op, failure(EndpointFailureKind::Transport, "Veo transport failed; generation is not retried", true));
}
struct BusyLease {
    Operation& op;
    ~BusyLease() { op.busy.store(false, std::memory_order_release); }
};
// A separate fork owns each active coroutine's one-slot cancellation signal.
// Setup/emit/teardown all run on its serial executor; handles retain no executor
// after a call finishes, so callers may safely destroy their io_context.
template<class T>
asio::awaitable<T> cancellable(std::shared_ptr<Operation> op, asio::awaitable<T> work) {
    auto child = op->cancellation->fork();
    auto ex = child->bind_executor(co_await asio::this_coro::executor);
    graph::CancelExecutorLease lease(child);
    auto initiate = [ex, child, work = std::move(work)](auto handler) mutable {
        asio::post(ex, [ex, child, work = std::move(work), handler = std::move(handler)]() mutable {
            asio::co_spawn(ex, std::move(work),
                           asio::bind_cancellation_slot(child->slot(), std::move(handler)));
        });
    };
    auto token = asio::use_awaitable;
    co_return co_await asio::async_initiate<decltype(token), void(std::exception_ptr, T)>(std::move(initiate), token);
}
asio::awaitable<VeoResult> poll_work(std::shared_ptr<Operation> op, bool waiting) {
    try {
        for (;;) {
            if (op->result.status != VeoStatus::Pending) co_return op->result;
            if (auto f = checkpoint(*op)) { fail(*op, *f); co_return op->result; }
            if (waiting) {
                asio::steady_timer timer(co_await asio::this_coro::executor);
                timer.expires_at(std::min(op->deadline, Clock::now() + op->client->interval));
                co_await timer.async_wait(asio::use_awaitable);
                if (auto f = checkpoint(*op)) { fail(*op, *f); co_return op->result; }
            }
            if (op->result.status_queries >= op->client->query_limit) {
                fail(*op, failure(EndpointFailureKind::ResourceLimit, "Veo status-query limit exhausted", true));
                co_return op->result;
            }
            ++op->result.status_queries;
            const auto response = co_await async::async_get(co_await asio::this_coro::executor,
                op->client->host, op->client->port, op->client->api_path + '/' + op->name,
                headers(*op->client), op->client->tls, options(*op));
            if (auto f = checkpoint(*op)) fail(*op, *f);
            else decode_status(*op, response, false);
            if (!waiting) co_return op->result;
        }
    } catch (const std::exception& e) { transport_failure(*op, e); }
    co_return op->result;
}
asio::awaitable<VeoResult> lifecycle(std::shared_ptr<const Client> client,
                                    std::shared_ptr<Operation> op, bool waiting) {
    if (!op || op->client != client) {
        VeoResult r; r.status = VeoStatus::Failed;
        r.failure = failure(EndpointFailureKind::InvalidRequest, "Veo handle belongs to another client");
        co_return r;
    }
    if (op->busy.exchange(true, std::memory_order_acq_rel)) {
        VeoResult r; r.status = VeoStatus::Failed;
        r.failure = failure(EndpointFailureKind::InvalidRequest, "Veo operation already has an active call");
        co_return r;
    }
    BusyLease busy{*op};
    // Terminal provider failures cannot be replaced by a later local cancellation.
    if (op->result.status != VeoStatus::Pending) co_return op->result;
    if (auto f = checkpoint(*op)) {
        fail(*op, *f);
        co_return op->result;
    }
    try { co_return co_await cancellable(op, poll_work(op, waiting)); }
    catch (const std::exception& e) { transport_failure(*op, e); co_return op->result; }
}
asio::awaitable<VeoSubmitResult> submit_work(std::shared_ptr<Operation> op, std::string body) {
    try {
        if (auto f = checkpoint(*op)) { fail(*op, *f); co_return VeoSubmitResult{{}, op->result.failure}; }
        ++op->result.generation_dispatches;
        const auto response = co_await async::async_post(co_await asio::this_coro::executor,
            op->client->host, op->client->port,
            op->client->api_path + "/models/" + op->model + ":predictLongRunning", body,
            headers(*op->client), op->client->tls, options(*op));
        if (auto f = checkpoint(*op)) fail(*op, *f);
        else decode_status(*op, response, true);
    } catch (const std::exception& e) { transport_failure(*op, e); }
    co_return VeoSubmitResult{{}, op->result.failure};
}
asio::awaitable<VeoDownloadResult> download_work(std::shared_ptr<Operation> op, std::size_t index) {
    try {
        if (auto f = checkpoint(*op)) co_return VeoDownloadResult{{}, *f};
        if (op->result.status != VeoStatus::Succeeded || index >= op->result.artifacts.size())
            co_return VeoDownloadResult{{}, failure(EndpointFailureKind::InvalidRequest, "Veo video is not ready")};
        auto artifact = op->result.artifacts[index];
        if (artifact.bytes) co_return VeoDownloadResult{std::move(artifact), {}};
        const auto path = video_path(*op->client, artifact.uri);
        if (!path) co_return VeoDownloadResult{{}, failure(EndpointFailureKind::Protocol, "Invalid Veo video origin", true)};
        std::string uri = artifact.uri;
        std::vector<std::string> visited;
        async::HttpResponse response;
        for (unsigned hop = 0;; ++hop) {
            if (auto f = checkpoint(*op)) co_return VeoDownloadResult{{}, *f};
            const auto target = download_target(*op->client, uri);
            if (!target || (!visited.empty() && !target->tls && op->client->tls))
                co_return VeoDownloadResult{{}, failure(EndpointFailureKind::Protocol, "Unadmitted Veo download redirect", true)};
            if (std::find(visited.begin(), visited.end(), uri) != visited.end())
                co_return VeoDownloadResult{{}, failure(EndpointFailureKind::Protocol, "Veo download redirect loop", true)};
            visited.push_back(uri);
            ++op->result.downloads;
            // Generic transport redirect handling remains disabled. Only the exact
            // original API origin receives the key; signed storage GETs do not.
            auto auth = target->origin == op->client->origin ? headers(*op->client) :
                std::vector<std::pair<std::string, std::string>>{};
            response = co_await async::async_get(co_await asio::this_coro::executor,
                target->host, target->port, target->path, std::move(auth), target->tls, options(*op, true));
            if (auto f = checkpoint(*op)) co_return VeoDownloadResult{{}, *f};
            const bool redirect = response.status == 301 || response.status == 302 || response.status == 303 ||
                                  response.status == 307 || response.status == 308;
            if (!redirect) break;
            if (hop >= op->client->download_redirects)
                co_return VeoDownloadResult{{}, failure(EndpointFailureKind::ResourceLimit, "Veo download redirect bound exceeded", true)};
            const auto next = download_target(*op->client, response.location, target->origin);
            if (!next || (target->tls && !next->tls))
                co_return VeoDownloadResult{{}, failure(EndpointFailureKind::Protocol, "Unsafe Veo download redirect", true)};
            uri = next->origin + next->path;
        }
        if (response.status < 200 || response.status >= 300)
            co_return VeoDownloadResult{{}, failure(EndpointFailureKind::Provider, "Veo download HTTP request failed", true, response.status)};
        auto mime = response.get_header("Content-Type");
        const auto semicolon = mime.find(';');
        mime = mime.substr(0, semicolon);
        if (mime != "video/mp4" || response.body.size() < 12 || response.body.compare(4, 4, "ftyp") != 0)
            co_return VeoDownloadResult{{}, failure(EndpointFailureKind::Protocol, "Veo download is not an MP4 video", true)};
        auto bytes = std::make_shared<std::vector<std::byte>>(response.body.size());
        std::memcpy(bytes->data(), response.body.data(), response.body.size());
        artifact.bytes = std::move(bytes);
        op->result.artifacts[index] = artifact;
        co_return VeoDownloadResult{std::move(artifact), {}};
    } catch (const std::exception& e) {
        if (auto f = checkpoint(*op)) co_return VeoDownloadResult{{}, *f};
        const auto* system = dynamic_cast<const asio::system_error*>(&e);
        const auto kind = system && system->code() == asio::error::operation_aborted ? EndpointFailureKind::Cancelled :
                          system && system->code() == asio::error::timed_out ? EndpointFailureKind::DeadlineExceeded : EndpointFailureKind::Transport;
        co_return VeoDownloadResult{{}, failure(kind, "Veo download failed without retry", true)};
    }
}
asio::awaitable<VeoDownloadResult> download_call(std::shared_ptr<const Client> client,
                                                std::shared_ptr<Operation> op, std::size_t index) {
    if (!op || op->client != client)
        co_return VeoDownloadResult{{}, failure(EndpointFailureKind::InvalidRequest, "Veo handle belongs to another client")};
    if (op->busy.exchange(true, std::memory_order_acq_rel))
        co_return VeoDownloadResult{{}, failure(EndpointFailureKind::InvalidRequest, "Veo operation already has an active call")};
    BusyLease busy{*op};
    try { co_return co_await cancellable(op, download_work(op, index)); }
    catch (const std::exception&) {
        if (auto f = checkpoint(*op)) co_return VeoDownloadResult{{}, *f};
        co_return VeoDownloadResult{{}, failure(EndpointFailureKind::Cancelled, "Veo download cancelled", true)};
    }
}
} // namespace
} // namespace veo_detail

VeoOperation::VeoOperation(std::shared_ptr<veo_detail::Operation> state) : state_(std::move(state)) {}
std::string VeoOperation::name() const { return state_ ? state_->name : std::string{}; }
std::string VeoOperation::model() const { return state_ ? state_->model : std::string{}; }
std::chrono::steady_clock::time_point VeoOperation::deadline() const noexcept {
    return state_ ? state_->deadline : std::chrono::steady_clock::time_point{};
}
VeoClient::VeoClient(const json& config, std::string api_key)
    : client_(veo_detail::admit(config, std::move(api_key))) {}
namespace {
asio::awaitable<VeoSubmitResult> rejected_submit(EndpointFailure f) {
    co_return VeoSubmitResult{{}, std::move(f)};
}
}
asio::awaitable<VeoSubmitResult> VeoClient::submit(asio::any_io_executor ex, VeoRequest request,
    std::optional<std::chrono::steady_clock::time_point> deadline) const {
    // This wrapper intentionally isn't a coroutine: admission and owning inputs
    // occur at invocation, before the caller can destroy this client or request.
    auto op = std::make_shared<veo_detail::Operation>();
    op->client = client_;
    const auto now = veo_detail::Clock::now();
    op->deadline = deadline.value_or(now + client_->deadline);
    if (op->deadline > now + client_->deadline)
        return rejected_submit(veo_detail::failure(EndpointFailureKind::InvalidRequest, "Veo deadline exceeds configured maximum"));
    op->cancellation = request.cancel_token ? request.cancel_token->fork() : std::make_shared<graph::CancelToken>();
    std::string body;
    if (auto f = veo_detail::request_body(*client_, request, op->model, body)) return rejected_submit(*f);
    // Capture the operation in a coroutine parameter, not a temporary coroutine lambda.
    auto execute = [](asio::any_io_executor target, std::shared_ptr<veo_detail::Operation> state,
                      std::string payload) -> asio::awaitable<VeoSubmitResult> {
        // Hop through the supplied executor, while cancellation itself uses a private strand.
        auto job = [](std::shared_ptr<veo_detail::Operation> s, std::string p) -> asio::awaitable<VeoSubmitResult> {
            auto result = co_await veo_detail::cancellable(s, veo_detail::submit_work(s, std::move(p)));
            if (!result.failure) result.operation = VeoOperation(s);
            co_return result;
        };
        try { co_return co_await asio::co_spawn(target, job(state, std::move(payload)), asio::use_awaitable); }
        catch (const std::exception& e) {
            veo_detail::transport_failure(*state, e);
            co_return VeoSubmitResult{{}, state->result.failure};
        }
    };
    return execute(std::move(ex), std::move(op), std::move(body));
}
asio::awaitable<VeoResult> VeoClient::poll(VeoOperation operation) const {
    return veo_detail::lifecycle(client_, std::move(operation.state_), false);
}
asio::awaitable<VeoResult> VeoClient::wait(VeoOperation operation) const {
    return veo_detail::lifecycle(client_, std::move(operation.state_), true);
}
void VeoClient::cancel(const VeoOperation& operation) const noexcept {
    if (operation.state_ && operation.state_->client == client_) operation.state_->cancellation->cancel();
}
asio::awaitable<VeoDownloadResult> VeoClient::download(VeoOperation operation, std::size_t index) const {
    return veo_detail::download_call(client_, std::move(operation.state_), index);
}
} // namespace neograph::llm

namespace neograph::llm {
asio::awaitable<VeoSubmitResult> VeoClient::lookup(asio::any_io_executor ex, std::string name,
    std::optional<std::chrono::steady_clock::time_point> deadline,
    std::shared_ptr<graph::CancelToken> cancellation) const {
    auto op = std::make_shared<veo_detail::Operation>();
    op->client = client_;
    const auto now = veo_detail::Clock::now();
    op->deadline = deadline.value_or(now + client_->deadline);
    if (op->deadline > now + client_->deadline)
        return rejected_submit(veo_detail::failure(EndpointFailureKind::InvalidRequest, "Veo deadline exceeds configured maximum"));
    for (const auto& model : client_->models) {
        const auto prefix = "models/" + model + "/operations/";
        if (name.rfind(prefix, 0) == 0 && veo_detail::component(std::string_view(name).substr(prefix.size()))) {
            op->model = model;
            break;
        }
    }
    if (op->model.empty())
        return rejected_submit(veo_detail::failure(EndpointFailureKind::InvalidRequest, "Invalid Veo lookup operation name"));
    op->name = std::move(name);
    op->cancellation = cancellation ? cancellation->fork() : std::make_shared<graph::CancelToken>();
    auto execute = [](asio::any_io_executor target, std::shared_ptr<veo_detail::Operation> state)
        -> asio::awaitable<VeoSubmitResult> {
        auto job = [](std::shared_ptr<veo_detail::Operation> s) -> asio::awaitable<VeoSubmitResult> {
            auto result = co_await veo_detail::cancellable(s, veo_detail::poll_work(s, false));
            if (result.failure) co_return VeoSubmitResult{{}, result.failure};
            co_return VeoSubmitResult{VeoOperation(s), {}};
        };
        try { co_return co_await asio::co_spawn(target, job(state), asio::use_awaitable); }
        catch (const std::exception& e) {
            veo_detail::transport_failure(*state, e);
            co_return VeoSubmitResult{{}, state->result.failure};
        }
    };
    return execute(std::move(ex), std::move(op));
}
} // namespace neograph::llm
