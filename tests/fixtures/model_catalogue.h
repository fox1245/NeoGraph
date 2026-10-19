#pragma once
// Synthetic fixture models. SchemaProvider interface 7 keeps model facts (limits, per-model defaults)
// in the model catalogue, not in the descriptor policy; these entries are layered over the embedded
// catalogue.

#include <neograph/json.h>

#include <descriptor/catalogue.h>
#include <descriptor/policy.h>
#include <sp/config_defaults.h>

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace neograph::test {

struct FixtureModel {
    std::string family, model;
    std::optional<std::uint64_t> input_limit{}, output_limit{};
    // Overlays the family defaults member by member; a null member leaves that control unset.
    neograph::json defaults = neograph::json::object();
    // Catalogue price units (decimal USD per unit), as a host's pricing layer gives them.
    neograph::json pricing = neograph::json::object();
    // Base URLs the entry is scoped to; empty means every origin.
    std::vector<std::string> origins{};
};

inline sp::descriptor::CatalogueSnapshot fixture_catalogue(const std::vector<FixtureModel>& models) {
    auto entries = neograph::json::array();
    for (const auto& model : models) {
        neograph::json entry{{"id", model.model}, {"families", neograph::json::array({model.family})}};
        if (model.input_limit) entry["input_limit"] = *model.input_limit;
        if (model.output_limit) entry["output_limit"] = *model.output_limit;
        if (!model.defaults.empty()) entry["defaults"] = neograph::json{{model.family, model.defaults}};
        if (!model.pricing.empty()) entry["pricing"] = model.pricing;
        if (!model.origins.empty()) entry["origins"] = model.origins;
        entries.push_back(std::move(entry));
    }
    auto loaded = sp::descriptor::load_catalogue(neograph::json{{"version", 1}, {"models", entries}}.dump());
    if (const auto* error = std::get_if<sp::descriptor::ConfigError>(&loaded))
        throw std::invalid_argument("fixture catalogue " + error->pointer + ": " + error->message);
    const std::array layers{sp::descriptor::builtin_catalogue(),
                            std::get<sp::descriptor::CatalogueSnapshot>(std::move(loaded))};
    auto merged = sp::descriptor::merge_catalogues(layers);
    if (const auto* error = std::get_if<sp::descriptor::ConfigError>(&merged))
        throw std::invalid_argument("fixture catalogue " + error->pointer + ": " + error->message);
    return std::get<sp::descriptor::CatalogueSnapshot>(std::move(merged));
}

// `policy` is a descriptor policy document (the embedded one, possibly edited).
inline sp::descriptor::PolicySnapshot policy_with_models(const neograph::json& policy,
                                                         const std::vector<FixtureModel>& models) {
    auto admitted = sp::descriptor::load_policy(policy.dump(), sp::config_defaults::codec_defaults_json,
                                                fixture_catalogue(models));
    if (const auto* error = std::get_if<sp::descriptor::ConfigError>(&admitted))
        throw std::invalid_argument(error->pointer + ": " + error->message);
    return std::get<sp::descriptor::PolicySnapshot>(std::move(admitted));
}

}  // namespace neograph::test
