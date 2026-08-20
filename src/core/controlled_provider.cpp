#include <neograph/controlled_provider.h>

#include <neograph/runtime_turn_assembler.h>
#include <neograph/graph/cancel.h>

#include <neograph/async/run_sync.h>

#include "canonical_json.h"

#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace neograph {
namespace {

constexpr std::string_view IDENTITY_PREAMBLE = "NeoGraph Provider dispatch identity v1";

std::string mode_name(CompletionMode mode) {
    switch (mode) {
        case CompletionMode::COLLECT: return "collect";
        case CompletionMode::STREAM: return "stream";
    }
    throw std::invalid_argument("Provider dispatch mode is invalid");
}

CompletionMode mode_from_name(std::string_view value) {
    if (value == "collect") return CompletionMode::COLLECT;
    if (value == "stream") return CompletionMode::STREAM;
    throw std::invalid_argument("Provider dispatch mode is invalid");
}

std::string state_name(ProviderDispatchState state) {
    switch (state) {
        case ProviderDispatchState::Succeeded: return "succeeded";
        case ProviderDispatchState::Failed: return "failed";
        case ProviderDispatchState::ReconciliationRequired: return "reconciliation_required";
        default: break;
    }
    throw std::invalid_argument("Provider dispatch outcome state is not terminal");
}

ProviderDispatchState state_from_name(std::string_view value) {
    if (value == "succeeded") return ProviderDispatchState::Succeeded;
    if (value == "failed") return ProviderDispatchState::Failed;
    if (value == "reconciliation_required") return ProviderDispatchState::ReconciliationRequired;
    throw std::invalid_argument("Provider dispatch outcome state is invalid");
}

std::string required_string(const json& value, std::string_view field) {
    const std::string key(field);
    if (!value.contains(key) || !value.at(key).is_string()) {
        throw std::invalid_argument("Stored ProviderDispatchReceipt field '" + key + "' must be a string");
    }
    return value.at(key).get<std::string>();
}

std::uint64_t required_u64(const json& value, std::string_view field) {
    const std::string key(field);
    if (!value.contains(key) || !value.at(key).is_number_unsigned()) {
        throw std::invalid_argument("Stored ProviderDispatchReceipt field '" + key + "' must be unsigned");
    }
    return value.at(key).get<unsigned long long>();
}

void require_identity(std::string_view value, std::string_view field) {
    if (!detail::is_sha256_identity(value)) {
        throw std::invalid_argument(std::string(field) + " must be a sha256 identity");
    }
}

void validate_data(const ProviderDispatchReceiptData& data) {
    detail::validate_token(data.dispatch_id, "Provider dispatch dispatch_id");
    require_identity(data.provider_binding_identity, "Provider dispatch provider_binding_identity");
    require_identity(data.assembly_receipt_id, "Provider dispatch assembly_receipt_id");
    require_identity(data.normalized_request_digest, "Provider dispatch normalized_request_digest");
    detail::validate_token(data.model, "Provider dispatch model");
    (void)mode_name(data.mode);
}

void validate_outcome_data(const ProviderDispatchOutcomeReceiptData& data) {
    detail::validate_token(data.dispatch_id, "Provider dispatch outcome dispatch_id");
    require_identity(data.dispatch_receipt_id,
                     "Provider dispatch outcome dispatch_receipt_id");
    (void)state_name(data.state);
    if (data.error.size() > 4096) {
        throw std::invalid_argument("Provider dispatch outcome error is too large");
    }
    if (data.state == ProviderDispatchState::Succeeded) {
        require_identity(data.response_digest, "Provider dispatch outcome response_digest");
        if (!data.error.empty()) {
            throw std::invalid_argument("Successful provider dispatch outcome cannot carry an error");
        }
    } else {
        if (!data.response_digest.empty() || data.error.empty()) {
            throw std::invalid_argument(
                "Non-success provider dispatch outcome requires only an error");
        }
    }
}

std::string completion_digest(const ChatCompletion& completion) {
    json message;
    to_json(message, completion.message);
    return detail::sha256_identity(
        IDENTITY_PREAMBLE, "provider-completion/v1",
        detail::canonical_json_bytes(json{
            {"message", std::move(message)},
            {"stop_reason", completion.stop_reason},
            {"usage", {{"prompt_tokens", completion.usage.prompt_tokens},
                       {"completion_tokens", completion.usage.completion_tokens},
                       {"total_tokens", completion.usage.total_tokens}}}}));
}

std::string bounded_error(std::string_view value) {
    return std::string(value.substr(0, 4096));
}

}  // namespace

struct ProviderDispatchReceipt::Impl {
    ProviderDispatchReceiptData data;
    std::string id;
    std::string canonical;
};

ProviderDispatchReceipt::ProviderDispatchReceipt(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

ProviderDispatchReceipt ProviderDispatchReceipt::create(ProviderDispatchReceiptData data) {
    validate_data(data);
    json body{{"format", "neograph-provider-dispatch-receipt"},
              {"storage_schema_version", STORAGE_SCHEMA_VERSION},
              {"dispatch_id", data.dispatch_id},
              {"provider_binding_identity", data.provider_binding_identity},
              {"assembly_receipt_id", data.assembly_receipt_id},
              {"normalized_request_digest", data.normalized_request_digest},
              {"model", data.model},
              {"mode", mode_name(data.mode)}};
    auto impl = std::make_shared<Impl>();
    impl->id = detail::sha256_identity(IDENTITY_PREAMBLE, "provider-dispatch-receipt/v1",
                                       detail::canonical_json_bytes(body));
    body["id"] = impl->id;
    impl->data = std::move(data);
    impl->canonical = detail::canonical_json_bytes(body);
    return ProviderDispatchReceipt(std::move(impl));
}

ProviderDispatchReceipt ProviderDispatchReceipt::parse(std::string_view stored_bytes) {
    const auto value = detail::parse_json_strict(stored_bytes);
    if (!value.is_object()) throw std::invalid_argument("Stored ProviderDispatchReceipt must be an object");
    detail::reject_unknown_fields(value, "Stored ProviderDispatchReceipt",
                                   {"format", "storage_schema_version", "id", "assembly_receipt_id",
                                    "dispatch_id", "provider_binding_identity", "normalized_request_digest",
                                    "model", "mode"});
    if (required_string(value, "format") != "neograph-provider-dispatch-receipt" ||
        required_u64(value, "storage_schema_version") != STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument("Stored ProviderDispatchReceipt format is unsupported");
    }
    const auto stored_id = required_string(value, "id");
    ProviderDispatchReceiptData data;
    data.dispatch_id = required_string(value, "dispatch_id");
    data.provider_binding_identity = required_string(value, "provider_binding_identity");
    data.assembly_receipt_id = required_string(value, "assembly_receipt_id");
    data.normalized_request_digest = required_string(value, "normalized_request_digest");
    data.model = required_string(value, "model");
    data.mode = mode_from_name(required_string(value, "mode"));
    auto result = create(std::move(data));
    if (result.id() != stored_id) throw std::invalid_argument("Stored ProviderDispatchReceipt id mismatch");
    return result;
}

const std::string& ProviderDispatchReceipt::dispatch_id() const noexcept { return impl_->data.dispatch_id; }
const std::string& ProviderDispatchReceipt::provider_binding_identity() const noexcept { return impl_->data.provider_binding_identity; }
const std::string& ProviderDispatchReceipt::assembly_receipt_id() const noexcept { return impl_->data.assembly_receipt_id; }
const std::string& ProviderDispatchReceipt::normalized_request_digest() const noexcept { return impl_->data.normalized_request_digest; }
const std::string& ProviderDispatchReceipt::model() const noexcept { return impl_->data.model; }
CompletionMode ProviderDispatchReceipt::mode() const noexcept { return impl_->data.mode; }
const std::string& ProviderDispatchReceipt::id() const noexcept { return impl_->id; }
std::string ProviderDispatchReceipt::serialize_canonical() const { return impl_->canonical; }

struct ProviderDispatchOutcomeReceipt::Impl {
    ProviderDispatchOutcomeReceiptData data;
    std::string id;
    std::string canonical;
};

ProviderDispatchOutcomeReceipt::ProviderDispatchOutcomeReceipt(
    std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

ProviderDispatchOutcomeReceipt ProviderDispatchOutcomeReceipt::create(
    ProviderDispatchOutcomeReceiptData data) {
    validate_outcome_data(data);
    json body{{"format", "neograph-provider-dispatch-outcome-receipt"},
              {"storage_schema_version", STORAGE_SCHEMA_VERSION},
              {"dispatch_id", data.dispatch_id},
              {"dispatch_receipt_id", data.dispatch_receipt_id},
              {"state", state_name(data.state)},
              {"response_digest", data.response_digest},
              {"error", data.error}};
    auto impl = std::make_shared<Impl>();
    impl->id = detail::sha256_identity(
        IDENTITY_PREAMBLE, "provider-dispatch-outcome-receipt/v1",
        detail::canonical_json_bytes(body));
    body["id"] = impl->id;
    impl->data = std::move(data);
    impl->canonical = detail::canonical_json_bytes(body);
    return ProviderDispatchOutcomeReceipt(std::move(impl));
}

ProviderDispatchOutcomeReceipt ProviderDispatchOutcomeReceipt::parse(
    std::string_view stored_bytes) {
    const auto value = detail::parse_json_strict(stored_bytes);
    if (!value.is_object()) {
        throw std::invalid_argument("Stored ProviderDispatchOutcomeReceipt must be an object");
    }
    detail::reject_unknown_fields(
        value, "Stored ProviderDispatchOutcomeReceipt",
        {"format", "storage_schema_version", "id", "dispatch_id",
         "dispatch_receipt_id", "state", "response_digest", "error"});
    if (required_string(value, "format") !=
            "neograph-provider-dispatch-outcome-receipt" ||
        required_u64(value, "storage_schema_version") != STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument(
            "Stored ProviderDispatchOutcomeReceipt format is unsupported");
    }
    const auto stored_id = required_string(value, "id");
    ProviderDispatchOutcomeReceiptData data;
    data.dispatch_id = required_string(value, "dispatch_id");
    data.dispatch_receipt_id = required_string(value, "dispatch_receipt_id");
    data.state = state_from_name(required_string(value, "state"));
    data.response_digest = required_string(value, "response_digest");
    data.error = required_string(value, "error");
    auto result = create(std::move(data));
    if (result.id() != stored_id) {
        throw std::invalid_argument("Stored ProviderDispatchOutcomeReceipt id mismatch");
    }
    return result;
}

const std::string& ProviderDispatchOutcomeReceipt::dispatch_id() const noexcept {
    return impl_->data.dispatch_id;
}
const std::string& ProviderDispatchOutcomeReceipt::dispatch_receipt_id() const noexcept {
    return impl_->data.dispatch_receipt_id;
}
ProviderDispatchState ProviderDispatchOutcomeReceipt::state() const noexcept {
    return impl_->data.state;
}
const std::string& ProviderDispatchOutcomeReceipt::response_digest() const noexcept {
    return impl_->data.response_digest;
}
const std::string& ProviderDispatchOutcomeReceipt::error() const noexcept {
    return impl_->data.error;
}
const std::string& ProviderDispatchOutcomeReceipt::id() const noexcept { return impl_->id; }
std::string ProviderDispatchOutcomeReceipt::serialize_canonical() const {
    return impl_->canonical;
}

struct InMemoryProviderDispatchReceiptStore::Impl {
    mutable std::mutex mutex;
    std::map<std::pair<std::string, std::string>, std::string> receipts;
    std::map<std::pair<std::string, std::string>, std::string> outcomes;
};

InMemoryProviderDispatchReceiptStore::InMemoryProviderDispatchReceiptStore()
    : impl_(std::make_unique<Impl>()) {}
InMemoryProviderDispatchReceiptStore::~InMemoryProviderDispatchReceiptStore() = default;
InMemoryProviderDispatchReceiptStore::InMemoryProviderDispatchReceiptStore(
    InMemoryProviderDispatchReceiptStore&&) noexcept = default;
InMemoryProviderDispatchReceiptStore& InMemoryProviderDispatchReceiptStore::operator=(
    InMemoryProviderDispatchReceiptStore&&) noexcept = default;

ProviderDispatchReceiptPutResult InMemoryProviderDispatchReceiptStore::persist(
    const ProviderDispatchReceipt& receipt) {
    return persist({}, receipt);
}

ProviderDispatchReceiptPutResult InMemoryProviderDispatchReceiptStore::persist(
    std::string_view owner_scope, const ProviderDispatchReceipt& receipt) {
    const auto canonical = receipt.serialize_canonical();
    std::lock_guard lock(impl_->mutex);
    const auto key = std::make_pair(std::string(owner_scope), receipt.dispatch_id());
    const auto found = impl_->receipts.find(key);
    if (found == impl_->receipts.end()) {
        impl_->receipts.emplace(std::move(key), canonical);
        return ProviderDispatchReceiptPutResult::Stored;
    }
    return found->second == canonical ? ProviderDispatchReceiptPutResult::AlreadyPresent
                                      : ProviderDispatchReceiptPutResult::Conflict;
}

ProviderDispatchState InMemoryProviderDispatchReceiptStore::state(
    std::string_view dispatch_id) const {
    return state({}, dispatch_id);
}

ProviderDispatchState InMemoryProviderDispatchReceiptStore::state(
    std::string_view owner_scope, std::string_view dispatch_id) const {
    std::lock_guard lock(impl_->mutex);
    const auto key = std::make_pair(std::string(owner_scope), std::string(dispatch_id));
    const auto terminal = impl_->outcomes.find(key);
    if (terminal != impl_->outcomes.end()) {
        return ProviderDispatchOutcomeReceipt::parse(terminal->second).state();
    }
    return impl_->receipts.contains(key) ? ProviderDispatchState::AdmittedPending
                                         : ProviderDispatchState::Missing;
}

ProviderDispatchOutcomePutResult InMemoryProviderDispatchReceiptStore::settle(
    std::string_view owner_scope, const ProviderDispatchOutcomeReceipt& outcome_receipt) {
    const auto key =
        std::make_pair(std::string(owner_scope), outcome_receipt.dispatch_id());
    const auto canonical = outcome_receipt.serialize_canonical();
    std::lock_guard lock(impl_->mutex);
    const auto receipt = impl_->receipts.find(key);
    if (receipt == impl_->receipts.end()) {
        return ProviderDispatchOutcomePutResult::MissingDispatch;
    }
    if (ProviderDispatchReceipt::parse(receipt->second).id() !=
        outcome_receipt.dispatch_receipt_id()) {
        return ProviderDispatchOutcomePutResult::Conflict;
    }
    const auto found = impl_->outcomes.find(key);
    if (found != impl_->outcomes.end()) {
        return found->second == canonical
                   ? ProviderDispatchOutcomePutResult::AlreadyPresent
                   : ProviderDispatchOutcomePutResult::Conflict;
    }
    impl_->outcomes.emplace(key, canonical);
    return ProviderDispatchOutcomePutResult::Stored;
}

std::optional<ProviderDispatchOutcomeReceipt>
InMemoryProviderDispatchReceiptStore::outcome(
    std::string_view owner_scope, std::string_view dispatch_id) const {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->outcomes.find(
        {std::string(owner_scope), std::string(dispatch_id)});
    if (found == impl_->outcomes.end()) return std::nullopt;
    return ProviderDispatchOutcomeReceipt::parse(found->second);
}

struct ControlledProvider::Impl {
    std::shared_ptr<Provider> provider;
    std::shared_ptr<ProviderDispatchReceiptStore> receipts;
    std::string provider_binding_identity;
};

ControlledProvider::ControlledProvider(std::shared_ptr<Provider> provider,
                                       std::shared_ptr<ProviderDispatchReceiptStore> receipts,
                                       std::string provider_binding_identity) {
    if (!provider || !receipts) {
        throw std::invalid_argument("Controlled provider dependencies must not be null");
    }
    require_identity(provider_binding_identity, "Controlled provider binding identity");
    impl_ = std::make_shared<Impl>(
        Impl{std::move(provider), std::move(receipts), std::move(provider_binding_identity)});
}
ControlledProvider::~ControlledProvider() = default;
ControlledProvider::ControlledProvider(ControlledProvider&&) noexcept = default;
ControlledProvider& ControlledProvider::operator=(ControlledProvider&&) noexcept = default;

asio::awaitable<ChatCompletion> ControlledProvider::dispatch_async(
    std::string dispatch_id, const ContextAssemblyReceipt& assembly, CompletionRequest request) {
    return dispatch_impl(impl_, {}, std::move(dispatch_id), assembly, std::move(request));
}

asio::awaitable<ChatCompletion> ControlledProvider::dispatch_async(
    std::string owner_scope, std::string dispatch_id, const ContextAssemblyReceipt& assembly,
    CompletionRequest request) {
    return dispatch_impl(impl_, std::move(owner_scope), std::move(dispatch_id), assembly,
                         std::move(request));
}

asio::awaitable<ChatCompletion> ControlledProvider::dispatch_impl(
    std::shared_ptr<Impl> impl, std::string owner_scope, std::string dispatch_id,
    ContextAssemblyReceipt assembly, CompletionRequest request) {
    detail::validate_token(dispatch_id, "Controlled provider dispatch_id");
    if (request.params().cancel_token && request.params().cancel_token->is_cancelled()) {
        throw graph::CancelledException("provider dispatch cancelled before admission");
    }
    if (request.params().model.empty()) {
        throw std::invalid_argument("Controlled provider requires a nonempty model");
    }
    detail::validate_token(request.params().model, "Controlled provider model");
    if (RuntimeTurnAssembler::normalized_request_digest(request) !=
        assembly.normalized_request_digest()) {
        throw std::invalid_argument("Provider request does not match its assembly receipt");
    }
    ProviderDispatchReceipt receipt = ProviderDispatchReceipt::create(
        {std::move(dispatch_id), impl->provider_binding_identity, assembly.id(),
          assembly.normalized_request_digest(), request.params().model, request.mode()});
    const auto persisted = impl->receipts->persist(owner_scope, receipt);
    if (persisted != ProviderDispatchReceiptPutResult::Stored) {
        if (persisted == ProviderDispatchReceiptPutResult::AlreadyPresent) {
            throw std::runtime_error(
                "reconciliation_required: provider dispatch is already admitted; durable state is " +
                std::to_string(static_cast<unsigned>(impl->receipts->state(owner_scope, receipt.dispatch_id()))));
        }
        throw std::runtime_error("Provider dispatch receipt persistence conflicted");
    }
    auto* outcomes = dynamic_cast<ProviderDispatchOutcomeStore*>(impl->receipts.get());
    try {
        auto completion = co_await invoke_completion(*impl->provider, std::move(request));
        if (outcomes) {
            const auto settled = outcomes->settle(
                owner_scope,
                ProviderDispatchOutcomeReceipt::create(
                    {receipt.dispatch_id(), receipt.id(), ProviderDispatchState::Succeeded,
                     completion_digest(completion), {}}));
            if (settled != ProviderDispatchOutcomePutResult::Stored &&
                settled != ProviderDispatchOutcomePutResult::AlreadyPresent) {
                throw std::runtime_error(
                    "Provider dispatch succeeded but its terminal receipt could not be persisted");
            }
        }
        co_return completion;
    } catch (const std::exception& error) {
        if (outcomes && outcomes->outcome(owner_scope, receipt.dispatch_id()) == std::nullopt) {
            const auto settled = outcomes->settle(
                owner_scope,
                ProviderDispatchOutcomeReceipt::create(
                    {receipt.dispatch_id(), receipt.id(),
                     ProviderDispatchState::ReconciliationRequired, {},
                     bounded_error(error.what())}));
            if (settled == ProviderDispatchOutcomePutResult::Conflict ||
                settled == ProviderDispatchOutcomePutResult::MissingDispatch) {
                throw std::runtime_error(
                    "Provider dispatch failed and its reconciliation receipt conflicted");
            }
        }
        throw;
    }
}

ChatCompletion ControlledProvider::dispatch(std::string dispatch_id,
                                              const ContextAssemblyReceipt& assembly,
                                              CompletionRequest request) {
    auto* token = request.params().cancel_token ? request.params().cancel_token.get() : nullptr;
    return async::run_sync(dispatch_async(std::move(dispatch_id), assembly, std::move(request)), token);
}

ChatCompletion ControlledProvider::dispatch(std::string owner_scope, std::string dispatch_id,
                                             const ContextAssemblyReceipt& assembly,
                                             CompletionRequest request) {
    auto* token = request.params().cancel_token ? request.params().cancel_token.get() : nullptr;
    return async::run_sync(dispatch_async(std::move(owner_scope), std::move(dispatch_id), assembly,
                                          std::move(request)), token);
}

}  // namespace neograph
