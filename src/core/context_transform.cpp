#include <neograph/context_transform.h>

#include "canonical_json.h"

#include <algorithm>
#include <stdexcept>

namespace neograph {
namespace {

constexpr std::string_view kPreamble = "NeoGraph Context transform identity v1";

void require_sha(std::string_view value, std::string_view field) {
    if (!detail::is_sha256_identity(value)) {
        throw std::invalid_argument(std::string(field) + " must be a sha256 identity");
    }
}

void normalize(std::vector<std::string>& values, std::string_view field) {
    for (const auto& value : values) require_sha(value, field);
    std::sort(values.begin(), values.end());
    if (std::adjacent_find(values.begin(), values.end()) != values.end()) {
        throw std::invalid_argument(std::string(field) + " contains a duplicate");
    }
}

std::vector<std::string> ids(const std::vector<ContextArtifact>& artifacts) {
    std::vector<std::string> result;
    result.reserve(artifacts.size());
    for (const auto& artifact : artifacts) result.push_back(artifact.id());
    normalize(result, "Context transform artifact ids");
    return result;
}

std::vector<std::string> required_ids(
    const std::vector<ContextArtifact>& artifacts) {
    std::vector<std::string> result;
    for (const auto& artifact : artifacts) {
        if (artifact.required()) result.push_back(artifact.id());
    }
    normalize(result, "Context transform required artifact ids");
    return result;
}

std::string required_string(const json& value, std::string_view name) {
    const std::string key(name);
    if (!value.contains(key) || !value.at(key).is_string()) {
        throw std::invalid_argument("Stored ContextTransformReceipt field '" + key +
                                    "' must be a string");
    }
    return value.at(key).get<std::string>();
}

std::vector<std::string> required_array(const json& value,
                                        std::string_view name) {
    const std::string key(name);
    if (!value.contains(key) || !value.at(key).is_array()) {
        throw std::invalid_argument("Stored ContextTransformReceipt field '" + key +
                                    "' must be an array");
    }
    std::vector<std::string> result;
    for (const auto& item : value.at(key)) {
        if (!item.is_string()) {
            throw std::invalid_argument(
                "Stored ContextTransformReceipt identity must be a string");
        }
        result.push_back(item.get<std::string>());
    }
    return result;
}

}  // namespace

struct ContextTransformReceipt::Impl {
    ContextTransformReceiptData data;
    std::string id;
    std::string canonical;
};

ContextTransformReceipt::ContextTransformReceipt(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

ContextTransformReceipt ContextTransformReceipt::create_structural(
    ContextTransformReceiptData data) {
    require_sha(data.source_context_epoch_id,
                "Context transform source_context_epoch_id");
    require_sha(data.transformer_identity,
                "Context transform transformer_identity");
    normalize(data.input_artifact_ids, "Context transform input_artifact_ids");
    normalize(data.output_artifact_ids, "Context transform output_artifact_ids");
    normalize(data.preserved_required_artifact_ids,
              "Context transform preserved_required_artifact_ids");
    json body{{"format", "neograph-context-transform-receipt"},
              {"storage_schema_version", STORAGE_SCHEMA_VERSION},
              {"source_context_epoch_id", data.source_context_epoch_id},
              {"transformer_identity", data.transformer_identity},
              {"input_artifact_ids", data.input_artifact_ids},
              {"output_artifact_ids", data.output_artifact_ids},
              {"preserved_required_artifact_ids",
               data.preserved_required_artifact_ids}};
    auto impl = std::make_shared<Impl>();
    impl->id = detail::sha256_identity(
        kPreamble, "context-transform-receipt/v1",
        detail::canonical_json_bytes(body));
    body["id"] = impl->id;
    impl->data = std::move(data);
    impl->canonical = detail::canonical_json_bytes(body);
    return ContextTransformReceipt(std::move(impl));
}

ContextTransformReceipt ContextTransformReceipt::create(
    ContextTransformReceiptData data, const ContextEpoch& source_epoch,
    const std::vector<ContextArtifact>& input_artifacts,
    const std::vector<ContextArtifact>& output_artifacts) {
    auto receipt = create_structural(std::move(data));
    validate_context_transform_receipt(receipt, source_epoch, input_artifacts,
                                       output_artifacts);
    return receipt;
}

ContextTransformReceipt ContextTransformReceipt::parse(
    std::string_view stored_bytes, const ContextEpoch& source_epoch,
    const std::vector<ContextArtifact>& input_artifacts,
    const std::vector<ContextArtifact>& output_artifacts) {
    const auto value = detail::parse_json_strict(stored_bytes);
    if (!value.is_object()) {
        throw std::invalid_argument("Stored ContextTransformReceipt must be an object");
    }
    detail::reject_unknown_fields(
        value, "Stored ContextTransformReceipt",
        {"format", "storage_schema_version", "id", "source_context_epoch_id",
         "transformer_identity", "input_artifact_ids", "output_artifact_ids",
         "preserved_required_artifact_ids"});
    if (required_string(value, "format") !=
            "neograph-context-transform-receipt" ||
        !value.contains("storage_schema_version") ||
        !value.at("storage_schema_version").is_number_unsigned() ||
        value.at("storage_schema_version").get<std::uint32_t>() !=
            STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument(
            "Stored ContextTransformReceipt format is unsupported");
    }
    const auto stored_id = required_string(value, "id");
    ContextTransformReceiptData data;
    data.source_context_epoch_id =
        required_string(value, "source_context_epoch_id");
    data.transformer_identity = required_string(value, "transformer_identity");
    data.input_artifact_ids = required_array(value, "input_artifact_ids");
    data.output_artifact_ids = required_array(value, "output_artifact_ids");
    data.preserved_required_artifact_ids =
        required_array(value, "preserved_required_artifact_ids");
    auto receipt = create_structural(std::move(data));
    if (receipt.id() != stored_id) {
        throw std::invalid_argument("Stored ContextTransformReceipt id mismatch");
    }
    validate_context_transform_receipt(receipt, source_epoch, input_artifacts,
                                       output_artifacts);
    return receipt;
}

const std::string& ContextTransformReceipt::source_context_epoch_id() const noexcept {
    return impl_->data.source_context_epoch_id;
}
const std::string& ContextTransformReceipt::transformer_identity() const noexcept {
    return impl_->data.transformer_identity;
}
const std::vector<std::string>& ContextTransformReceipt::input_artifact_ids() const noexcept {
    return impl_->data.input_artifact_ids;
}
const std::vector<std::string>& ContextTransformReceipt::output_artifact_ids() const noexcept {
    return impl_->data.output_artifact_ids;
}
const std::vector<std::string>&
ContextTransformReceipt::preserved_required_artifact_ids() const noexcept {
    return impl_->data.preserved_required_artifact_ids;
}
const std::string& ContextTransformReceipt::id() const noexcept { return impl_->id; }
std::string ContextTransformReceipt::serialize_canonical() const {
    return impl_->canonical;
}

void validate_context_transform_receipt(
    const ContextTransformReceipt& receipt, const ContextEpoch& source_epoch,
    const std::vector<ContextArtifact>& input_artifacts,
    const std::vector<ContextArtifact>& output_artifacts) {
    if (receipt.source_context_epoch_id() != source_epoch.id() ||
        receipt.input_artifact_ids() != source_epoch.artifact_ids() ||
        receipt.input_artifact_ids() != ids(input_artifacts) ||
        receipt.output_artifact_ids() != ids(output_artifacts)) {
        throw std::invalid_argument(
            "Context transform receipt does not bind its exact source and outputs");
    }
    const auto required = required_ids(input_artifacts);
    if (receipt.preserved_required_artifact_ids() != required ||
        !std::includes(receipt.output_artifact_ids().begin(),
                       receipt.output_artifact_ids().end(), required.begin(),
                       required.end())) {
        throw std::invalid_argument(
            "Context transform did not preserve every required artifact exactly");
    }
}

}  // namespace neograph

