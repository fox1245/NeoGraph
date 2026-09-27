#include <neograph/llm/schema_primitive_registry.h>

#include <cctype>
#include <stdexcept>
#include <utility>

namespace neograph::llm {
namespace {

void check_registration(const std::string& name, bool exists,
                        SchemaPrimitiveRegistration policy,
                        bool has_factory) {
    if (!has_factory) {
        throw std::invalid_argument("schema primitive factory for '" + name + "' is empty");
    }
    if (exists && policy == SchemaPrimitiveRegistration::Reject) {
        throw std::invalid_argument("duplicate schema primitive name: " + name);
    }
}

template <typename Map, typename Factory>
void add(Map& map, std::string name, Factory factory,
         SchemaPrimitiveRegistration policy, std::string_view label) {
    SchemaPrimitiveRegistry::validate_name(name, label);
    const bool exists = map.find(name) != map.end();
    check_registration(name, exists, policy, static_cast<bool>(factory));
    map.insert_or_assign(std::move(name), std::move(factory));
}

template <typename Map>
const typename Map::mapped_type& find(const Map& map, std::string_view name,
                                      std::string_view label) {
    const auto it = map.find(std::string(name));
    if (it == map.end()) {
        throw std::invalid_argument("unknown schema " + std::string(label) + " primitive: " + std::string(name));
    }
    return it->second;
}

template <typename Map>
bool has(const Map& map, std::string_view name) {
    return map.find(std::string(name)) != map.end();
}

template <typename Map>
std::vector<std::string> list(const Map& map) {
    std::vector<std::string> result;
    result.reserve(map.size());
    for (const auto& item : map) result.push_back(item.first);
    return result;
}

} // namespace

SchemaPrimitiveRegistry::SchemaPrimitiveRegistry() {
    // Built-ins are markers. SchemaProvider supplies the reviewed standard
    // implementation when these names are selected; registration replacement
    // supplies an application callback for the same declarative name.
    transports_.emplace("http", SchemaTransportFactory{});
    execution_modes_.emplace("standard", SchemaExecutionFactory{});
    artifact_parsers_.emplace("rules", SchemaArtifactParserFactory{});
}

SchemaPrimitiveRegistry SchemaPrimitiveRegistry::standard() { return {}; }

void SchemaPrimitiveRegistry::register_transport(std::string name,
                                                 SchemaTransportFactory factory,
                                                 SchemaPrimitiveRegistration policy) {
    add(transports_, std::move(name), std::move(factory), policy, "transport");
}

void SchemaPrimitiveRegistry::register_execution_mode(std::string name,
                                                      SchemaExecutionFactory factory,
                                                      SchemaPrimitiveRegistration policy) {
    add(execution_modes_, std::move(name), std::move(factory), policy, "execution-mode");
}

void SchemaPrimitiveRegistry::register_artifact_parser(std::string name,
                                                       SchemaArtifactParserFactory factory,
                                                       SchemaPrimitiveRegistration policy) {
    add(artifact_parsers_, std::move(name), std::move(factory), policy, "artifact-parser");
}

void SchemaPrimitiveRegistry::replace_transport(std::string name, SchemaTransportFactory factory) {
    register_transport(std::move(name), std::move(factory), SchemaPrimitiveRegistration::Replace);
}
void SchemaPrimitiveRegistry::replace_execution_mode(std::string name, SchemaExecutionFactory factory) {
    register_execution_mode(std::move(name), std::move(factory), SchemaPrimitiveRegistration::Replace);
}
void SchemaPrimitiveRegistry::replace_artifact_parser(std::string name, SchemaArtifactParserFactory factory) {
    register_artifact_parser(std::move(name), std::move(factory), SchemaPrimitiveRegistration::Replace);
}

bool SchemaPrimitiveRegistry::contains(SchemaPrimitiveCategory category,
                                       std::string_view name) const {
    switch (category) {
        case SchemaPrimitiveCategory::Transport: return has(transports_, name);
        case SchemaPrimitiveCategory::ExecutionMode: return has(execution_modes_, name);
        case SchemaPrimitiveCategory::ArtifactParser: return has(artifact_parsers_, name);
    }
    return false;
}

std::vector<std::string> SchemaPrimitiveRegistry::names(SchemaPrimitiveCategory category) const {
    switch (category) {
        case SchemaPrimitiveCategory::Transport: return list(transports_);
        case SchemaPrimitiveCategory::ExecutionMode: return list(execution_modes_);
        case SchemaPrimitiveCategory::ArtifactParser: return list(artifact_parsers_);
    }
    return {};
}

const SchemaTransportFactory& SchemaPrimitiveRegistry::transport(std::string_view name) const {
    return find(transports_, name, "transport");
}
const SchemaExecutionFactory& SchemaPrimitiveRegistry::execution_mode(std::string_view name) const {
    return find(execution_modes_, name, "execution-mode");
}
const SchemaArtifactParserFactory& SchemaPrimitiveRegistry::artifact_parser(std::string_view name) const {
    return find(artifact_parsers_, name, "artifact-parser");
}

const char* SchemaPrimitiveRegistry::category_name(SchemaPrimitiveCategory category) noexcept {
    switch (category) {
        case SchemaPrimitiveCategory::Transport: return "transport";
        case SchemaPrimitiveCategory::ExecutionMode: return "execution-mode";
        case SchemaPrimitiveCategory::ArtifactParser: return "artifact-parser";
    }
    return "unknown";
}

void SchemaPrimitiveRegistry::validate_name(std::string_view name, std::string_view label) {
    if (name.empty() || name.size() > 128) {
        throw std::invalid_argument("invalid schema " + std::string(label) + " primitive name");
    }
    for (const unsigned char c : name) {
        if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == ':')) {
            throw std::invalid_argument("invalid schema " + std::string(label) + " primitive name: " + std::string(name));
        }
    }
}

} // namespace neograph::llm
