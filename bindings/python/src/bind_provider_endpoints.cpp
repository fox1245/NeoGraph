// SchemaProvider's non-chat endpoints on Provider (#531): the SDK request and result types, and the calls that run
// them on the provider's own client. A request carries caller data only; the provider's policy admits or refuses it
// at prepare. `from_dict` (neograph_engine.provider_endpoints) builds the request classes from plain dicts.
#include "async_runtime.h"
#include "json_bridge.h"
#include "provider_bridge.h"
#include "sdk_members.h"
#include <codecs/webhooks.h>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/post.hpp>
#include <pybind11/stl.h>
#include <chrono>
#include <tuple>
#include <type_traits>
#include <variant>

namespace py = pybind11;
namespace neograph::pybind {
namespace {
// The endpoint requests Provider::prepare_endpoint takes, and the preparations they make: one per outcome type.
using Requests = std::tuple<sp::images::Request, sp::decisions::Request, sp::embeddings::Request, sp::rerank::Request,
    sp::moderation::Request, sp::speech::Request, sp::transcription::Request, sp::videos::Request, sp::jobs::Poll,
    sp::jobs::Download, sp::files::Upload, sp::files::Get, sp::files::List, sp::files::Delete, sp::files::Download,
    sp::files::Start, sp::files::Part, sp::files::Complete, sp::files::Query, sp::files::Cancel, sp::batches::Submit,
    sp::batches::Get, sp::batches::Cancel, sp::batches::Delete, sp::batches::Results, sp::tokens::Count,
    sp::generations::Get>;
using Prepared = std::variant<
    sp::runtime::PreparedEndpoint<sp::images::Outcome>, sp::runtime::PreparedEndpoint<sp::decisions::Outcome>,
    sp::runtime::PreparedEndpoint<sp::embeddings::Outcome>, sp::runtime::PreparedEndpoint<sp::rerank::Outcome>,
    sp::runtime::PreparedEndpoint<sp::moderation::Outcome>, sp::runtime::PreparedEndpoint<sp::speech::Outcome>,
    sp::runtime::PreparedEndpoint<sp::transcription::Outcome>, sp::runtime::PreparedEndpoint<sp::jobs::Outcome>,
    sp::runtime::PreparedEndpoint<sp::jobs::DownloadOutcome>, sp::runtime::PreparedEndpoint<sp::files::Outcome>,
    sp::runtime::PreparedEndpoint<sp::files::PageOutcome>, sp::runtime::PreparedEndpoint<sp::files::DeleteOutcome>,
    sp::runtime::PreparedEndpoint<sp::files::ProgressOutcome>, sp::runtime::PreparedEndpoint<sp::batches::Outcome>,
    sp::runtime::PreparedEndpoint<sp::batches::DeleteOutcome>, sp::runtime::PreparedEndpoint<sp::tokens::Outcome>,
    sp::runtime::PreparedEndpoint<sp::generations::Outcome>>;
template <class... R> constexpr bool prepares_all(std::tuple<R...>*) {
    return (EndpointRequest<R> && ...) &&
           (std::is_constructible_v<Prepared, sp::runtime::PreparedEndpoint<EndpointOutcomeOf<R>>> && ...);
}
static_assert(prepares_all(static_cast<Requests*>(nullptr)));

// The Python handle owns the real move-only preparation. Completing it takes it, once, while the GIL is held and
// before it is released for the call. What the preparation names stays readable afterwards.
struct PreparedEndpointHandle {
    template <class Outcome>
    explicit PreparedEndpointHandle(sp::runtime::PreparedEndpoint<Outcome> prepared)
        : endpoint(prepared.prepared().endpoint()), model(prepared.prepared().model()) {
        if (const auto* refusal = prepared.error()) error = *refusal;
        value.emplace(std::move(prepared));
    }
    Prepared take() {
        if (!value) throw py::value_error("Prepared endpoint has already been completed");
        auto call = std::move(*value);
        value.reset();
        return call;
    }
    std::optional<Prepared> value;
    std::string endpoint, model;
    std::optional<sp::Error> error;
};

// Calls `visit` with a copy of the endpoint request `object` holds; false when it holds none.
template <class Visit, class... R>
bool visit_request(py::handle object, Visit&& visit, std::tuple<R...>*) {
    return ((py::isinstance<R>(object) ? (visit(object.cast<R>()), true) : false) || ...);
}
template <class Visit> bool visit_request(py::handle object, Visit&& visit) {
    return visit_request(object, std::forward<Visit>(visit), static_cast<Requests*>(nullptr));
}
PreparedEndpointHandle* prepared_of(py::handle object) {
    return py::isinstance<PreparedEndpointHandle>(object) ? &object.cast<PreparedEndpointHandle&>() : nullptr;
}
[[noreturn]] void refuse_call(py::handle object) {
    throw py::type_error(std::string("expected an endpoint request or a PreparedEndpoint, got ") +
                         py::str(py::type::of(object).attr("__name__")).cast<std::string>());
}
// timeout_seconds is the deadline of a call that is prepared with it, counted from now.
sp::runtime::RunOptions run_options(std::optional<double> timeout_seconds) {
    sp::runtime::RunOptions options;
    if (!timeout_seconds) return options;
    const auto now = std::chrono::steady_clock::now();
    const auto limit = std::chrono::duration<double>(sp::runtime::SteadyTime::max() - now).count();
    if (!(*timeout_seconds >= 0) || *timeout_seconds >= limit)
        throw py::value_error("timeout_seconds must be a representable nonnegative duration");
    options.deadline = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(*timeout_seconds));
    return options;
}
void refuse_timeout(std::optional<double> timeout_seconds) {
    if (timeout_seconds)
        throw py::value_error("timeout_seconds applies when a call is prepared; a PreparedEndpoint has its deadline");
}
// The outcome of a blocking call, converted once the GIL is back.
template <class Call> py::object released(Call&& call) {
    auto outcome = [&] {
        py::gil_scoped_release release;
        return call();
    }();
    return py::cast(std::move(outcome));
}
// A call that finds no admission slot is the SDK's failure outcome, as the async form returns it, not an exception.
template <class Request> auto complete_request(Provider& provider, Request request, sp::runtime::RunOptions options) {
    using Outcome = EndpointOutcomeOf<Request>;
    try {
        return provider.complete_endpoint(std::move(request), std::move(options));
    } catch (const sp::runtime::AdmissionError& error) {
        return sp::runtime::detail::typed_outcome(*error.outcome(), static_cast<const Outcome*>(nullptr));
    }
}
// Resolves an asyncio future with the call's outcome. Cancelling the future cancels the call; the SDK's Cancelled
// failure that then ends it is dropped, as is any outcome that arrives after the future is done.
template <class Outcome> py::object spawn(py::object loop, asio::awaitable<Outcome> call) {
    py::object future = loop.attr("create_future")();
    auto fut_h = hold_py(future);
    auto loop_h = hold_py(std::move(loop));
    const auto executor = AsyncRuntime::executor();
    // The signal is shared with the completion handler, so it outlives the coroutine that holds its slot.
    auto cancellation = std::make_shared<asio::cancellation_signal>();
    future.attr("add_done_callback")(py::cpp_function([cancellation, executor](py::object done) {
        try {
            if (done.attr("cancelled")().cast<bool>())
                asio::post(executor, [cancellation] { cancellation->emit(asio::cancellation_type::terminal); });
        } catch (const py::error_already_set&) {
            PyErr_Clear();
        }
    }));
    asio::co_spawn(executor, std::move(call),
        asio::bind_cancellation_slot(cancellation->slot(),
            [cancellation, fut_h, loop_h](std::exception_ptr error, Outcome outcome) {
                resolve_future_async(fut_h, loop_h, error, std::move(outcome));
            }));
    return future;
}
// A ProviderRequest as the chat request of its family, for the endpoints that carry one.
sp::FamilyRequest family_request(const ProviderRequest& request) {
    return std::visit([](const auto& payload) -> sp::FamilyRequest {
        if constexpr (std::is_same_v<std::decay_t<decltype(payload)>, sp::interactions::Request>)
            throw py::value_error("An Interactions request cannot be a batch item or be counted: the SDK has neither "
                                  "endpoint for its family");
        else return payload;
    }, request.payload);
}
[[noreturn]] void refuse(const sp::Error& error) {
    throw py::value_error(error.reason.empty() ? error.safe_message : error.safe_message + " [" + error.reason + "]");
}
// A member of a struct held in a member of `T`, flattened onto `T`.
template <class T, class N, class M>
void nested(py::class_<T>& cls, const char* name, N T::*outer, M N::*field) {
    cls.def_property(name, [outer, field](const T& v) { return (v.*outer).*field; },
                     [outer, field](T& v, M x) { (v.*outer).*field = std::move(x); });
}
// Usage and the served model, then the attempt and the wire, which every result of a model call carries.
template <class R> void metered(py::class_<R>& cls) {
    readonly(cls, "usage", &R::usage);
    readonly(cls, "model", &R::model);
}
template <class R> void observed(py::class_<R>& cls) {
    readonly(cls, "attempt", &R::attempt);
    readonly(cls, "raw_events", &R::raw_events);
}

void bind_models(py::module_& m) {
    namespace i = sp::images;
    auto images = py::class_<i::Request>(m, "ImagesRequest").def(py::init<>());
    member(images, "model", &i::Request::model);
    member(images, "prompt", &i::Request::prompt);
    member(images, "references", &i::Request::references);
    member(images, "count", &i::Request::count);
    member(images, "size", &i::Request::size);
    member(images, "quality", &i::Request::quality);
    member(images, "background", &i::Request::background);
    member(images, "output_format", &i::Request::output_format);
    member(images, "output_compression", &i::Request::output_compression);
    json_member(images, "extensions", &i::Request::extensions);
    auto images_result = py::class_<i::Result>(m, "ImagesResult");
    readonly(images_result, "images", &i::Result::images);
    readonly(images_result, "revised_prompts", &i::Result::revised_prompts);
    readonly(images_result, "created", &i::Result::created);
    metered(images_result);
    observed(images_result);

    namespace d = sp::decisions;
    py::enum_<d::Kind>(m, "DecisionKind")
        .value("Predicate", d::Kind::Predicate)
        .value("Choice", d::Kind::Choice)
        .value("Score", d::Kind::Score)
        .value("Box", d::Kind::Box);
    auto option = py::class_<d::Option>(m, "DecisionOption").def(py::init<>());
    member(option, "value", &d::Option::value);
    json_member(option, "guidance", &d::Option::guidance);
    auto question = py::class_<d::Question>(m, "DecisionQuestion").def(py::init<>());
    member(question, "name", &d::Question::name);
    member(question, "kind", &d::Question::kind);
    json_member(question, "instructions", &d::Question::instructions);
    member(question, "options", &d::Question::options);
    json_member(question, "when_true", &d::Question::when_true);
    json_member(question, "when_false", &d::Question::when_false);
    auto decisions = py::class_<d::Request>(m, "DecisionsRequest").def(py::init<>());
    member(decisions, "model", &d::Request::model);
    json_member(decisions, "input", &d::Request::input);
    member(decisions, "questions", &d::Request::questions);
    json_member(decisions, "extensions", &d::Request::extensions);
    auto answer = py::class_<d::Answer>(m, "DecisionAnswer");
    readonly(answer, "name", &d::Answer::name);
    readonly(answer, "kind", &d::Answer::kind);
    readonly(answer, "refused", &d::Answer::refused);
    readonly(answer, "probability", &d::Answer::probability);
    readonly(answer, "score", &d::Answer::score);
    readonly(answer, "confidence", &d::Answer::confidence);
    readonly(answer, "choice", &d::Answer::choice);
    // (value or level, probability) pairs, in the provider's order.
    readonly(answer, "probabilities", &d::Answer::probabilities);
    json_readonly(answer, "raw", &d::Answer::raw);
    auto decisions_result = py::class_<d::Result>(m, "DecisionsResult");
    readonly(decisions_result, "answers", &d::Result::answers);
    readonly(decisions_result, "id", &d::Result::id);
    readonly(decisions_result, "provider", &d::Result::provider);
    metered(decisions_result);
    observed(decisions_result);

    namespace e = sp::embeddings;
    auto embedding_input = py::class_<e::Input>(m, "EmbeddingInput").def(py::init<>());
    member(embedding_input, "text", &e::Input::text);
    member(embedding_input, "media", &e::Input::media);
    auto embeddings = py::class_<e::Request>(m, "EmbeddingsRequest").def(py::init<>());
    member(embeddings, "model", &e::Request::model);
    member(embeddings, "inputs", &e::Request::inputs);
    member(embeddings, "dimensions", &e::Request::dimensions);
    member(embeddings, "task", &e::Request::task);
    json_member(embeddings, "extensions", &e::Request::extensions);
    auto embeddings_result = py::class_<e::Result>(m, "EmbeddingsResult");
    // One list of floats per input, in input order.
    readonly(embeddings_result, "vectors", &e::Result::vectors);
    metered(embeddings_result);
    observed(embeddings_result);

    namespace r = sp::rerank;
    auto rerank = py::class_<r::Request>(m, "RerankRequest").def(py::init<>());
    member(rerank, "model", &r::Request::model);
    member(rerank, "query", &r::Request::query);
    member(rerank, "documents", &r::Request::documents);
    member(rerank, "top", &r::Request::top);
    json_member(rerank, "extensions", &r::Request::extensions);
    auto ranked = py::class_<r::Ranked>(m, "RerankRanked");
    readonly(ranked, "index", &r::Ranked::index);
    readonly(ranked, "relevance", &r::Ranked::relevance);
    auto rerank_result = py::class_<r::Result>(m, "RerankResult");
    readonly(rerank_result, "ranked", &r::Result::ranked);
    readonly(rerank_result, "id", &r::Result::id);
    readonly(rerank_result, "provider", &r::Result::provider);
    metered(rerank_result);
    observed(rerank_result);

    namespace o = sp::moderation;
    auto moderation_input = py::class_<o::Input>(m, "ModerationInput").def(py::init<>());
    member(moderation_input, "text", &o::Input::text);
    member(moderation_input, "media", &o::Input::media);
    auto moderation = py::class_<o::Request>(m, "ModerationRequest").def(py::init<>());
    member(moderation, "model", &o::Request::model);
    member(moderation, "inputs", &o::Request::inputs);
    json_member(moderation, "extensions", &o::Request::extensions);
    auto verdict = py::class_<o::Verdict>(m, "ModerationVerdict");
    readonly(verdict, "flagged", &o::Verdict::flagged);
    // (category, applies) and (category, score) pairs, in the provider's order.
    readonly(verdict, "categories", &o::Verdict::categories);
    readonly(verdict, "scores", &o::Verdict::scores);
    auto moderation_result = py::class_<o::Result>(m, "ModerationResult");
    readonly(moderation_result, "verdicts", &o::Result::verdicts);
    readonly(moderation_result, "id", &o::Result::id);
    metered(moderation_result);
    observed(moderation_result);

    namespace s = sp::speech;
    auto speech = py::class_<s::Request>(m, "SpeechRequest").def(py::init<>());
    member(speech, "model", &s::Request::model);
    member(speech, "input", &s::Request::input);
    member(speech, "voice", &s::Request::voice);
    member(speech, "instructions", &s::Request::instructions);
    member(speech, "format", &s::Request::format);
    member(speech, "speed", &s::Request::speed);
    json_member(speech, "extensions", &s::Request::extensions);
    auto speech_result = py::class_<s::Result>(m, "SpeechResult");
    readonly(speech_result, "audio", &s::Result::audio);
    readonly(speech_result, "id", &s::Result::id);
    metered(speech_result);
    observed(speech_result);

    namespace t = sp::transcription;
    auto transcription = py::class_<t::Request>(m, "TranscriptionRequest").def(py::init<>());
    member(transcription, "model", &t::Request::model);
    member(transcription, "audio", &t::Request::audio);
    member(transcription, "language", &t::Request::language);
    member(transcription, "prompt", &t::Request::prompt);
    member(transcription, "translate", &t::Request::translate);
    json_member(transcription, "extensions", &t::Request::extensions);
    auto transcription_result = py::class_<t::Result>(m, "TranscriptionResult");
    readonly(transcription_result, "text", &t::Result::text);
    readonly(transcription_result, "language", &t::Result::language);
    readonly(transcription_result, "seconds", &t::Result::seconds);
    metered(transcription_result);
    observed(transcription_result);
}

void bind_jobs(py::module_& m) {
    namespace j = sp::jobs;
    auto video = py::class_<sp::videos::Request>(m, "VideoRequest").def(py::init<>());
    member(video, "model", &sp::videos::Request::model);
    member(video, "prompt", &sp::videos::Request::prompt);
    member(video, "references", &sp::videos::Request::references);
    member(video, "duration", &sp::videos::Request::duration);
    member(video, "aspect_ratio", &sp::videos::Request::aspect_ratio);
    member(video, "resolution", &sp::videos::Request::resolution);
    member(video, "size", &sp::videos::Request::size);
    member(video, "audio", &sp::videos::Request::audio);
    json_member(video, "extensions", &sp::videos::Request::extensions);
    py::enum_<j::Status>(m, "JobStatus")
        .value("Pending", j::Status::Pending)
        .value("Running", j::Status::Running)
        .value("Completed", j::Status::Completed)
        .value("Failed", j::Status::Failed)
        .value("Cancelled", j::Status::Cancelled)
        .value("Expired", j::Status::Expired)
        .value("Unknown", j::Status::Unknown);
    auto handle = py::class_<j::Handle>(m, "JobHandle",
        "A provider job: a value a host journals and resumes from. Rebuild it from its four members to poll or "
        "download after a restart.").def(py::init<>());
    member(handle, "endpoint", &j::Handle::endpoint);
    member(handle, "origin", &j::Handle::origin);
    member(handle, "id", &j::Handle::id);
    member(handle, "model", &j::Handle::model);
    auto state = py::class_<j::State>(m, "JobState");
    readonly(state, "handle", &j::State::handle);
    readonly(state, "status", &j::State::status);
    readonly(state, "raw_status", &j::State::raw_status);
    readonly(state, "outputs", &j::State::outputs);
    readonly(state, "expires_at", &j::State::expires_at);
    readonly(state, "generation", &j::State::generation);
    metered(state);
    observed(state);
    auto poll = py::class_<j::Poll>(m, "JobPoll").def(py::init<>());
    member(poll, "handle", &j::Poll::handle);
    auto download = py::class_<j::Download>(m, "JobDownload").def(py::init<>());
    member(download, "handle", &j::Download::handle);
    member(download, "index", &j::Download::index);
    member(download, "path", &j::Download::path);
    member(download, "max_bytes", &j::Download::max_bytes);
    // Media in memory, or empty when the content was written to `path`.
    auto content = py::class_<j::Content>(m, "JobContent");
    readonly(content, "media", &j::Content::media);
    readonly(content, "path", &j::Content::path);
    readonly(content, "content_type", &j::Content::content_type);
    readonly(content, "sha256", &j::Content::sha256);
    readonly(content, "bytes", &j::Content::bytes);
    observed(content);
}

void bind_files(py::module_& m) {
    namespace f = sp::files;
    py::enum_<f::State>(m, "FileState")
        .value("Unknown", f::State::Unknown)
        .value("Processing", f::State::Processing)
        .value("Active", f::State::Active)
        .value("Failed", f::State::Failed);
    auto file = py::class_<f::File>(m, "ProviderFile");
    readonly(file, "origin", &f::File::origin);
    readonly(file, "id", &f::File::id);
    readonly(file, "uri", &f::File::uri);
    readonly(file, "name", &f::File::name);
    readonly(file, "mime", &f::File::mime);
    readonly(file, "bytes", &f::File::bytes);
    readonly(file, "created_at", &f::File::created_at);
    readonly(file, "expires_at", &f::File::expires_at);
    readonly(file, "state", &f::File::state);
    readonly(file, "purpose", &f::File::purpose);
    readonly(file, "downloadable", &f::File::downloadable);
    readonly(file, "sha256", &f::File::sha256);
    readonly(file, "uploaded_sha256", &f::File::uploaded_sha256);
    json_readonly(file, "raw", &f::File::raw);
    observed(file);
    // Upload bytes: `bytes` in memory, or `size` bytes of the local file `path` from `offset`, read while they
    // are sent. `sha256` is the digest of those bytes (lower-case hex) when the caller knows it.
    auto source = py::class_<f::Source>(m, "FileSource").def(py::init<>());
    source.def_property("bytes",
        [](const f::Source& v) -> py::object { return v.bytes ? py::object(py::bytes(*v.bytes)) : py::none(); },
        [](f::Source& v, std::optional<py::bytes> x) {
            v.bytes = x ? std::make_shared<const std::string>(static_cast<std::string>(*x)) : nullptr;
        });
    nested(source, "path", &f::Source::file, &sp::endpoints::FileSlice::path);
    nested(source, "offset", &f::Source::file, &sp::endpoints::FileSlice::offset);
    nested(source, "size", &f::Source::file, &sp::endpoints::FileSlice::size);
    nested(source, "sha256", &f::Source::file, &sp::endpoints::FileSlice::sha256);
    auto upload = py::class_<f::Upload>(m, "FileUpload").def(py::init<>());
    member(upload, "source", &f::Upload::source);
    member(upload, "filename", &f::Upload::filename);
    member(upload, "mime", &f::Upload::mime);
    member(upload, "purpose", &f::Upload::purpose);
    member(upload, "expires_after", &f::Upload::expires_after);
    auto get = py::class_<f::Get>(m, "FileGet").def(py::init<>());
    member(get, "id", &f::Get::id);
    auto remove = py::class_<f::Delete>(m, "FileDelete").def(py::init<>());
    member(remove, "id", &f::Delete::id);
    auto list = py::class_<f::List>(m, "FileList").def(py::init<>());
    member(list, "limit", &f::List::limit);
    member(list, "after", &f::List::after);
    member(list, "purpose", &f::List::purpose);
    auto page = py::class_<f::Page>(m, "FilePage");
    readonly(page, "files", &f::Page::files);
    readonly(page, "next", &f::Page::next);
    observed(page);
    auto deleted = py::class_<f::Deleted>(m, "FileDeleted");
    readonly(deleted, "id", &f::Deleted::id);
    observed(deleted);
    auto download = py::class_<f::Download>(m, "FileDownload").def(py::init<>());
    member(download, "id", &f::Download::id);
    member(download, "path", &f::Download::path);
    member(download, "max_bytes", &f::Download::max_bytes);

    // A chunked upload is a session a host journals between its steps and resumes after a restart.
    auto session = py::class_<f::Session>(m, "FileUploadSession",
        "A chunked upload in progress. Google's `url` lets anyone write the file without the API key: keep it as "
        "private as a credential.").def(py::init<>());
    member(session, "origin", &f::Session::origin);
    member(session, "shape", &f::Session::shape);
    member(session, "id", &f::Session::id);
    member(session, "url", &f::Session::url);
    member(session, "size", &f::Session::size);
    member(session, "sent", &f::Session::sent);
    member(session, "granularity", &f::Session::granularity);
    member(session, "parts", &f::Session::parts);
    member(session, "expires_at", &f::Session::expires_at);
    member(session, "status", &f::Session::status);
    auto progress = py::class_<f::Progress>(m, "FileUploadProgress");
    readonly(progress, "session", &f::Progress::session);
    readonly(progress, "file", &f::Progress::file);
    observed(progress);
    auto start = py::class_<f::Start>(m, "FileUploadStart").def(py::init<>());
    member(start, "filename", &f::Start::filename);
    member(start, "mime", &f::Start::mime);
    member(start, "size", &f::Start::size);
    member(start, "purpose", &f::Start::purpose);
    member(start, "expires_after", &f::Start::expires_after);
    auto part = py::class_<f::Part>(m, "FileUploadPart").def(py::init<>());
    member(part, "session", &f::Part::session);
    member(part, "source", &f::Part::source);
    auto complete = py::class_<f::Complete>(m, "FileUploadComplete").def(py::init<>());
    member(complete, "session", &f::Complete::session);
    auto query = py::class_<f::Query>(m, "FileUploadQuery").def(py::init<>());
    member(query, "session", &f::Query::session);
    auto cancel = py::class_<f::Cancel>(m, "FileUploadCancel").def(py::init<>());
    member(cancel, "session", &f::Cancel::session);
    m.def("files_media", [](const f::File& file, const std::string& origin, std::uint64_t now) {
        auto media = f::media(file, origin, now);
        if (auto* error = std::get_if<sp::Error>(&media)) refuse(*error);
        return std::get<sp::Media>(std::move(media));
    }, py::arg("file"), py::arg("origin"), py::arg("now"),
       "The file as a chat request's ProviderMedia reference. Refused (ValueError) for another origin than the "
       "file's, while the file is not active, and once it expired (`now` in Unix seconds).");
}

void bind_batches(py::module_& m) {
    namespace b = sp::batches;
    auto item = py::class_<b::Item>(m, "BatchItem",
        "One request of a batch. `request` is set from a ProviderRequest (its payload; an Interactions payload "
        "raises ValueError) and cannot be read back.").def(py::init<>());
    member(item, "custom_id", &b::Item::custom_id);
    item.def_property("request", py::cpp_function(), py::cpp_function([](b::Item& v, const ProviderRequest& request) {
        v.request = family_request(request);
    }));
    auto submit = py::class_<b::Submit>(m, "BatchSubmit").def(py::init<>());
    member(submit, "items", &b::Submit::items);
    member(submit, "input_file", &b::Submit::input_file);
    member(submit, "endpoint", &b::Submit::endpoint);
    member(submit, "display_name", &b::Submit::display_name);
    json_member(submit, "extensions", &b::Submit::extensions);
    auto counts = py::class_<b::Counts>(m, "BatchCounts");
    readonly(counts, "total", &b::Counts::total);
    readonly(counts, "pending", &b::Counts::pending);
    readonly(counts, "succeeded", &b::Counts::succeeded);
    readonly(counts, "failed", &b::Counts::failed);
    readonly(counts, "cancelled", &b::Counts::cancelled);
    readonly(counts, "expired", &b::Counts::expired);
    auto batch = py::class_<b::Batch>(m, "Batch");
    readonly(batch, "origin", &b::Batch::origin);
    readonly(batch, "shape", &b::Batch::shape);
    readonly(batch, "id", &b::Batch::id);
    readonly(batch, "status", &b::Batch::status);
    readonly(batch, "raw_status", &b::Batch::raw_status);
    readonly(batch, "counts", &b::Batch::counts);
    readonly(batch, "output_file", &b::Batch::output_file);
    readonly(batch, "error_file", &b::Batch::error_file);
    readonly(batch, "results_ready", &b::Batch::results_ready);
    readonly(batch, "created_at", &b::Batch::created_at);
    readonly(batch, "expires_at", &b::Batch::expires_at);
    readonly(batch, "ended_at", &b::Batch::ended_at);
    readonly(batch, "usage", &b::Batch::usage);
    json_readonly(batch, "raw", &b::Batch::raw);
    observed(batch);
    auto get = py::class_<b::Get>(m, "BatchGet").def(py::init<>());
    member(get, "id", &b::Get::id);
    auto cancel = py::class_<b::Cancel>(m, "BatchCancel").def(py::init<>());
    member(cancel, "id", &b::Cancel::id);
    auto remove = py::class_<b::Delete>(m, "BatchDelete").def(py::init<>());
    member(remove, "id", &b::Delete::id);
    auto deleted = py::class_<b::Deleted>(m, "BatchDeleted");
    readonly(deleted, "id", &b::Deleted::id);
    observed(deleted);
    auto results = py::class_<b::Results>(m, "BatchResults").def(py::init<>());
    member(results, "batch", &b::Results::batch);
    member(results, "path", &b::Results::path);
    member(results, "max_bytes", &b::Results::max_bytes);
    member(results, "errors", &b::Results::errors);

    // One result line decoded as the outcome its request would have had.
    auto line = py::class_<b::Line>(m, "BatchLine");
    readonly(line, "custom_id", &b::Line::custom_id);
    line.def_property_readonly("outcome", [](const b::Line& v) -> sp::runtime::Result {
        return std::make_shared<const sp::Outcome>(v.outcome);
    });
    m.def("batch_jsonl", [](const sp::descriptor::ValidatedDescriptor& family, const std::vector<b::Item>& items) {
        auto jsonl = b::jsonl(family, items);
        if (auto* error = std::get_if<sp::Error>(&jsonl)) refuse(*error);
        return std::get<std::string>(std::move(jsonl));
    }, py::arg("descriptor"), py::arg("items"),
       "OpenAI's batch input file: one JSONL line per item, encoded for the Chat or Responses family descriptor.");
    m.def("batch_custom_id", [](const std::string& line) { return b::custom_id(line); }, py::arg("line"),
          "The custom id a result line answers, or None.");
    m.def("batch_inline_lines", &b::inline_lines, py::arg("batch"),
          "A Google batch's inline results, one JSONL line each.");
    const auto decode = [](const sp::descriptor::ValidatedDescriptor& family, const sp::FamilyRequest& original,
                           const std::string& line) {
        auto decoded = b::decode(family, original, line);
        if (auto* error = std::get_if<sp::Error>(&decoded)) refuse(*error);
        return std::get<b::Line>(std::move(decoded));
    };
    m.def("batch_decode_line", [decode](const sp::descriptor::ValidatedDescriptor& family,
                                       const ProviderRequest& original, const std::string& line) {
        return decode(family, family_request(original), line);
    }, py::arg("descriptor"), py::arg("request"), py::arg("line"),
       "One result line decoded as the outcome `request` would have had; `descriptor` is the family descriptor it "
       "is encoded for.");
    m.def("batch_decode_line", [decode](const sp::descriptor::ValidatedDescriptor& family, const b::Item& original,
                                       const std::string& line) {
        return decode(family, original.request, line);
    }, py::arg("descriptor"), py::arg("item"), py::arg("line"));
}

void bind_lookups(py::module_& m) {
    namespace t = sp::tokens;
    auto count = py::class_<t::Count>(m, "TokenCount",
        "Counts the input tokens of a chat request. `request` is set from a ProviderRequest (its payload; an "
        "Interactions payload raises ValueError) and cannot be read back.").def(py::init<>());
    count.def_property("request", py::cpp_function(), py::cpp_function([](t::Count& v, const ProviderRequest& request) {
        v.request = family_request(request);
    }));
    auto count_result = py::class_<t::Result>(m, "TokenCountResult");
    readonly(count_result, "input", &t::Result::input);
    readonly(count_result, "cached", &t::Result::cached);
    // (modality, tokens) pairs.
    readonly(count_result, "modalities", &t::Result::modalities);
    observed(count_result);

    namespace g = sp::generations;
    auto generation = py::class_<g::Get>(m, "GenerationGet").def(py::init<>());
    member(generation, "id", &g::Get::id);
    auto generation_result = py::class_<g::Result>(m, "GenerationResult");
    readonly(generation_result, "id", &g::Result::id);
    readonly(generation_result, "model", &g::Result::model);
    readonly(generation_result, "provider", &g::Result::provider);
    readonly(generation_result, "upstream_id", &g::Result::upstream_id);
    readonly(generation_result, "created_at", &g::Result::created_at);
    readonly(generation_result, "usage", &g::Result::usage);
    readonly(generation_result, "tokens_prompt", &g::Result::tokens_prompt);
    readonly(generation_result, "tokens_completion", &g::Result::tokens_completion);
    readonly(generation_result, "latency_ms", &g::Result::latency_ms);
    readonly(generation_result, "generation_ms", &g::Result::generation_ms);
    readonly(generation_result, "finish_reason", &g::Result::finish_reason);
    readonly(generation_result, "native_finish_reason", &g::Result::native_finish_reason);
    readonly(generation_result, "streamed", &g::Result::streamed);
    readonly(generation_result, "cancelled", &g::Result::cancelled);
    json_readonly(generation_result, "raw", &g::Result::raw);
    observed(generation_result);

    namespace w = sp::webhooks;
    auto event = py::class_<w::Event>(m, "WebhookEvent");
    readonly(event, "id", &w::Event::id);
    readonly(event, "event_id", &w::Event::event_id);
    readonly(event, "type", &w::Event::type);
    readonly(event, "created_at", &w::Event::created_at);
    readonly(event, "subject", &w::Event::subject);
    json_readonly(event, "raw", &w::Event::raw);
    m.def("verify_webhook", [](const std::string& secret, py::object headers, const std::string& body,
                               std::uint64_t now, std::uint64_t tolerance) {
        if (py::hasattr(headers, "items")) headers = py::list(headers.attr("items")());
        auto verified = w::verify(secret, headers.cast<std::vector<std::pair<std::string, std::string>>>(), body,
                                  now, tolerance);
        if (auto* error = std::get_if<sp::Error>(&verified)) refuse(*error);
        return std::get<w::Event>(std::move(verified));
    }, py::arg("secret"), py::arg("headers"), py::arg("body"), py::arg("now"), py::arg("tolerance") = 300,
       "Verifies a signed webhook (Standard Webhooks, as OpenAI sends them) and returns its WebhookEvent; an "
       "unsigned, altered or stale one raises ValueError. `headers` is a mapping or a list of (name, value) pairs, "
       "`body` the raw request body, `now` the host's clock in Unix seconds. The host remembers the delivery "
       "ids it has handled to refuse a replay.");
}
}  // namespace

py::object prepare_endpoint(Provider& provider, py::object request, std::optional<double> timeout_seconds) {
    const auto options = run_options(timeout_seconds);
    std::shared_ptr<PreparedEndpointHandle> prepared;
    if (!visit_request(request, [&](auto call) {
            prepared = std::make_shared<PreparedEndpointHandle>(provider.prepare_endpoint(std::move(call), options));
        })) refuse_call(request);
    return py::cast(std::move(prepared));
}

py::object complete_endpoint(Provider& provider, py::object request, std::optional<double> timeout_seconds) {
    if (auto* prepared = prepared_of(request)) {
        refuse_timeout(timeout_seconds);
        return std::visit([&](auto&& call) {
            return released([&] { return provider.complete_endpoint(std::move(call)); });
        }, prepared->take());
    }
    const auto options = run_options(timeout_seconds);
    py::object outcome;
    if (!visit_request(request, [&](auto call) {
            outcome = released([&] { return complete_request(provider, std::move(call), options); });
        })) refuse_call(request);
    return outcome;
}

py::object complete_endpoint_async(Provider& provider, py::object request, std::optional<double> timeout_seconds) {
    py::object loop = py::module_::import("asyncio").attr("get_running_loop")();
    if (auto* prepared = prepared_of(request)) {
        refuse_timeout(timeout_seconds);
        return std::visit([&](auto&& call) { return spawn(loop, provider.complete_endpoint_async(std::move(call))); },
                          prepared->take());
    }
    const auto options = run_options(timeout_seconds);
    py::object future;
    if (!visit_request(request, [&](auto call) {
            future = spawn(loop, provider.complete_endpoint_async(std::move(call), options));
        })) refuse_call(request);
    return future;
}

void init_provider_endpoints(py::module_& m) {
    py::class_<PreparedEndpointHandle, std::shared_ptr<PreparedEndpointHandle>>(m, "PreparedEndpoint",
        "One prepared SchemaProvider endpoint call. Completing it, with Provider.complete_endpoint or "
        "complete_endpoint_async, consumes it; a second completion raises ValueError.")
        .def_property_readonly("endpoint", [](const PreparedEndpointHandle& v) { return v.endpoint; })
        .def_property_readonly("model", [](const PreparedEndpointHandle& v) { return v.model; })
        .def_property_readonly("error", [](const PreparedEndpointHandle& v) { return v.error; })
        .def_property_readonly("consumed", [](const PreparedEndpointHandle& v) { return !v.value; });
    bind_models(m);
    bind_jobs(m);
    bind_files(m);
    bind_batches(m);
    bind_lookups(m);
}

}  // namespace neograph::pybind
