#include <neograph/program/step_plan.h>

#include "canonical_json.h"
#include "task_graph_proposal_detail.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace neograph::program {
namespace {

using detail::task_graph::child_pointer;
using detail::task_graph::index_pointer;

constexpr std::string_view kSchemaName = "step-plan/v1";

// Words that name an authority the host holds. A key like this gets a more specific hint; the
// closed key set, not this list, is what keeps the authority out of the plan.
bool is_authority_word(std::string_view key) {
    static constexpr std::array<std::string_view, 13> words = {
        "provider", "endpoint", "url",   "api_key", "token",   "secret", "credential",
        "command",  "code",     "script", "model",  "budget",  "timeout"};
    return std::find(words.begin(), words.end(), key) != words.end();
}

// ----- Text that comes from the plan is cut and made printable before it is echoed. -----

std::string printable(std::string_view value, std::size_t limit = 64) {
    std::string result;
    result.reserve(std::min(value.size(), limit));
    for (std::size_t index = 0; index < value.size() && index < limit; ++index) {
        const auto byte = static_cast<unsigned char>(value[index]);
        result.push_back(byte >= 0x20 && byte < 0x7f ? static_cast<char>(byte) : '?');
    }
    return result;
}

std::string quoted(std::string_view value) { return "'" + printable(value) + "'"; }

std::string names_list(std::vector<std::string> names, std::size_t limit = 5) {
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    std::string result;
    for (std::size_t index = 0; index < names.size() && index < limit; ++index) {
        if (index != 0) result += ", ";
        result += names[index];
    }
    return result;
}

json names_json(std::vector<std::string> names, std::size_t limit = 5) {
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    json result = json::array();
    for (std::size_t index = 0; index < names.size() && index < limit; ++index)
        result.push_back(names[index]);
    return result;
}

std::size_t edit_distance(std::string_view lhs, std::string_view rhs) {
    std::vector<std::size_t> previous(rhs.size() + 1);
    std::vector<std::size_t> current(rhs.size() + 1);
    for (std::size_t j = 0; j <= rhs.size(); ++j) previous[j] = j;
    for (std::size_t i = 1; i <= lhs.size(); ++i) {
        current[0] = i;
        for (std::size_t j = 1; j <= rhs.size(); ++j) {
            const auto substitution = previous[j - 1] + (lhs[i - 1] == rhs[j - 1] ? 0 : 1);
            current[j] = std::min({previous[j] + 1, current[j - 1] + 1, substitution});
        }
        std::swap(previous, current);
    }
    return previous[rhs.size()];
}

// The nearest names by (edit distance, name), at most `count`.
std::vector<std::string> nearest(std::string_view given, const std::vector<std::string>& names,
                                 std::size_t count) {
    const auto shown = printable(given);
    std::vector<std::pair<std::size_t, std::string>> scored;
    for (const auto& name : names) scored.emplace_back(edit_distance(shown, name), name);
    std::sort(scored.begin(), scored.end());
    std::vector<std::string> result;
    for (std::size_t index = 0; index < scored.size() && index < count; ++index) {
        // A name that shares nothing useful with the given one is not a suggestion.
        if (scored[index].first > std::max<std::size_t>(2, shown.size() / 2)) break;
        result.push_back(scored[index].second);
    }
    return result;
}

// ----- Violations, ordered the way a planner reads them. -----

bool all_digits(std::string_view value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](char c) {
        return c >= '0' && c <= '9';
    });
}

std::vector<std::string_view> split_pointer(std::string_view path) {
    std::vector<std::string_view> segments;
    std::size_t                   begin = 0;
    while (begin <= path.size()) {
        const auto end = path.find('/', begin);
        segments.push_back(path.substr(begin, end == std::string_view::npos ? path.size() - begin
                                                                            : end - begin));
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return segments;
}

// Segment by segment; two numeric segments compare as integers, otherwise bytewise. So
// "/steps/2" sorts before "/steps/10", unlike a plain string sort.
int compare_paths(std::string_view lhs, std::string_view rhs) {
    const auto left  = split_pointer(lhs);
    const auto right = split_pointer(rhs);
    for (std::size_t index = 0; index < left.size() && index < right.size(); ++index) {
        const auto a = left[index];
        const auto b = right[index];
        if (a == b) continue;
        if (all_digits(a) && all_digits(b)) {
            const auto strip = [](std::string_view digits) {
                while (digits.size() > 1 && digits.front() == '0') digits.remove_prefix(1);
                return digits;
            };
            const auto x = strip(a);
            const auto y = strip(b);
            if (x.size() != y.size()) return x.size() < y.size() ? -1 : 1;
            if (x != y) return x < y ? -1 : 1;
            continue;
        }
        return a < b ? -1 : 1;
    }
    if (left.size() == right.size()) return 0;
    return left.size() < right.size() ? -1 : 1;
}

class Collector {
public:
    void add(std::string path, std::string code, std::string message, std::string hint,
             json detail = json::object()) {
        StepPlanViolation violation;
        violation.path    = std::move(path);
        violation.code    = std::move(code);
        violation.message = std::move(message);
        violation.hint    = std::move(hint);
        violation.detail  = std::move(detail);
        items_.push_back(std::move(violation));
    }
    bool empty() const noexcept { return items_.empty(); }
    std::size_t size() const noexcept { return items_.size(); }

    std::vector<StepPlanViolation> finish(std::uint32_t maximum) {
        std::vector<std::pair<std::string, StepPlanViolation>> keyed;
        keyed.reserve(items_.size());
        for (auto& item : items_) {
            auto key = detail::canonical_json_bytes(item.detail);
            keyed.emplace_back(std::move(key), std::move(item));
        }
        std::stable_sort(keyed.begin(), keyed.end(), [](const auto& lhs, const auto& rhs) {
            const auto path = compare_paths(lhs.second.path, rhs.second.path);
            if (path != 0) return path < 0;
            if (lhs.second.code != rhs.second.code) return lhs.second.code < rhs.second.code;
            return lhs.first < rhs.first;
        });
        std::vector<StepPlanViolation> result;
        const std::size_t keep = std::min<std::size_t>(keyed.size(), std::max<std::uint32_t>(maximum, 1));
        for (std::size_t index = 0; index < keep; ++index) result.push_back(std::move(keyed[index].second));
        if (keyed.size() > keep) {
            StepPlanViolation marker;
            marker.path    = "";
            marker.code    = "STEP_VIOLATIONS_TRUNCATED";
            marker.message = "More problems were found than are listed.";
            marker.hint    = "Fix the listed problems first; the rest are reported again afterwards.";
            marker.detail  = json{{"omitted", keyed.size() - keep}};
            result.push_back(std::move(marker));
        }
        return result;
    }

private:
    std::vector<StepPlanViolation> items_;
};

// ----- Text intake: find the object, and refuse a repeated key. -----

struct Scanner {
    std::string_view text;
    std::size_t      pos = 0;
    std::vector<std::string> duplicates;  // pointers of the second occurrence of a key
    bool                     failed = false;

    void skip_space() {
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' ||
                                     text[pos] == '\r'))
            ++pos;
    }

    static void append_utf8(std::string& out, std::uint32_t code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    std::optional<std::uint32_t> hex4() {
        if (pos + 4 > text.size()) return std::nullopt;
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text[pos++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return std::nullopt;
        }
        return value;
    }

    std::optional<std::string> string_value() {
        if (pos >= text.size() || text[pos] != '"') return std::nullopt;
        ++pos;
        std::string out;
        while (pos < text.size()) {
            const char c = text[pos++];
            if (c == '"') return out;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos >= text.size()) return std::nullopt;
            const char e = text[pos++];
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                auto code = hex4();
                if (!code) return std::nullopt;
                if (*code >= 0xD800 && *code < 0xDC00 && pos + 1 < text.size() &&
                    text[pos] == '\\' && text[pos + 1] == 'u') {
                    pos += 2;
                    const auto low = hex4();
                    if (!low) return std::nullopt;
                    code = 0x10000 + ((*code - 0xD800) << 10) + (*low - 0xDC00);
                }
                append_utf8(out, *code);
                break;
            }
            default: return std::nullopt;
            }
        }
        return std::nullopt;
    }

    bool value(const std::string& pointer, int depth) {
        if (depth > 64) return false;
        skip_space();
        if (pos >= text.size()) return false;
        const char c = text[pos];
        if (c == '{') {
            ++pos;
            std::set<std::string> keys;
            skip_space();
            if (pos < text.size() && text[pos] == '}') {
                ++pos;
                return true;
            }
            while (true) {
                skip_space();
                const auto key = string_value();
                if (!key) return false;
                const auto child = child_pointer(pointer, *key);
                if (!keys.insert(*key).second) duplicates.push_back(child);
                skip_space();
                if (pos >= text.size() || text[pos] != ':') return false;
                ++pos;
                if (!value(child, depth + 1)) return false;
                skip_space();
                if (pos >= text.size()) return false;
                if (text[pos] == ',') {
                    ++pos;
                    continue;
                }
                if (text[pos] == '}') {
                    ++pos;
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            ++pos;
            skip_space();
            if (pos < text.size() && text[pos] == ']') {
                ++pos;
                return true;
            }
            std::size_t index = 0;
            while (true) {
                if (!value(index_pointer(pointer, index++), depth + 1)) return false;
                skip_space();
                if (pos >= text.size()) return false;
                if (text[pos] == ',') {
                    ++pos;
                    continue;
                }
                if (text[pos] == ']') {
                    ++pos;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') return string_value().has_value();
        // number, true, false, null: any run of token characters; the real parser judges it.
        const auto begin = pos;
        while (pos < text.size() && text[pos] != ',' && text[pos] != '}' && text[pos] != ']' &&
               text[pos] != ' ' && text[pos] != '\t' && text[pos] != '\n' && text[pos] != '\r')
            ++pos;
        return pos > begin;
    }
};

// The end (exclusive) of the balanced object that starts at text[begin] == '{'.
std::optional<std::size_t> balanced_object_end(std::string_view text, std::size_t begin) {
    int  depth     = 0;
    bool in_string = false;
    bool escaped   = false;
    for (std::size_t index = begin; index < text.size(); ++index) {
        const char c = text[index];
        if (in_string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') in_string = true;
        else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) return index + 1;
    }
    return std::nullopt;
}

struct Extracted {
    std::string_view object;
    StepPlanEnvelope envelope = StepPlanEnvelope::None;
    bool             found    = false;
};

std::string_view trim(std::string_view text) {
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n' ||
                             text.front() == '\r'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' ||
                             text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}

Extracted extract_object(std::string_view text) {
    Extracted result;
    const auto trimmed = trim(text);
    if (trimmed.starts_with('{')) {
        if (const auto end = balanced_object_end(trimmed, 0)) {
            result.object   = trimmed.substr(0, *end);
            result.envelope = *end == trimmed.size() ? StepPlanEnvelope::Strict
                                                     : StepPlanEnvelope::Recovered;
            result.found    = true;
        }
        return result;
    }
    std::string_view region = trimmed;
    if (const auto fence = trimmed.find("```"); fence != std::string_view::npos) {
        auto body = trimmed.substr(fence + 3);
        if (const auto line_end = body.find('\n'); line_end != std::string_view::npos)
            body.remove_prefix(line_end + 1);
        else
            body = {};
        if (const auto close = body.find("```"); close != std::string_view::npos)
            body = body.substr(0, close);
        if (body.find('{') != std::string_view::npos) region = body;
    }
    if (const auto open = region.find('{'); open != std::string_view::npos) {
        if (const auto end = balanced_object_end(region, open)) {
            result.object   = region.substr(open, *end - open);
            result.envelope = StepPlanEnvelope::Recovered;
            result.found    = true;
        }
    }
    return result;
}

// ----- Validation -----

bool valid_step_id(std::string_view id) {
    if (id.empty() || id.size() > 32) return false;
    if (id.front() < 'a' || id.front() > 'z') return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

std::string type_name(const json& value) {
    if (value.is_null()) return "null";
    if (value.is_boolean()) return "boolean";
    if (value.is_number_integer() || value.is_number_unsigned()) return "integer";
    if (value.is_number()) return "number";
    if (value.is_string()) return "string";
    if (value.is_array()) return "array";
    return "object";
}

std::string without_slash(std::string_view pointer) {
    return std::string(pointer.starts_with('/') ? pointer.substr(1) : pointer);
}

std::vector<std::string> input_names(const TaskGraphTemplateContract& contract) {
    std::vector<std::string> names;
    for (const auto& field : contract.input_fields) names.push_back(without_slash(field));
    std::sort(names.begin(), names.end());
    return names;
}

const TaskGraphArtifactContract* find_artifact(const TaskGraphTemplateContract& contract,
                                               std::string_view name) {
    for (const auto& artifact : contract.output_artifacts)
        if (artifact.artifact == name) return &artifact;
    return nullptr;
}

std::vector<std::string> artifact_names(const TaskGraphTemplateContract& contract) {
    std::vector<std::string> names;
    for (const auto& artifact : contract.output_artifacts) names.push_back(artifact.artifact);
    return names;
}

std::vector<std::string> field_names(const TaskGraphArtifactContract& artifact) {
    std::vector<std::string> names;
    for (const auto& field : artifact.fields) names.push_back(without_slash(field));
    return names;
}

std::size_t literal_depth(const json& value) {
    if (value.is_array()) {
        std::size_t deepest = 0;
        for (std::size_t index = 0; index < value.size(); ++index)
            deepest = std::max(deepest, literal_depth(value[index]));
        return deepest + 1;
    }
    if (value.is_object()) {
        std::size_t deepest = 0;
        for (const auto& [key, item] : value.items()) deepest = std::max(deepest, literal_depth(item));
        return deepest + 1;
    }
    return 0;
}

bool has_reserved_key(const json& value) {
    if (value.is_array()) {
        for (std::size_t index = 0; index < value.size(); ++index)
            if (has_reserved_key(value[index])) return true;
        return false;
    }
    if (value.is_object()) {
        for (const auto& [key, item] : value.items()) {
            if (key == "from_step" || has_reserved_key(item)) return true;
        }
    }
    return false;
}

bool has_non_finite(const json& value) {
    if (value.is_number_float()) return !std::isfinite(value.get<double>());
    if (value.is_array()) {
        for (std::size_t index = 0; index < value.size(); ++index)
            if (has_non_finite(value[index])) return true;
        return false;
    }
    if (value.is_object()) {
        for (const auto& [key, item] : value.items())
            if (has_non_finite(item)) return true;
    }
    return false;
}

struct ParamReference {
    std::string key;                // input name without '/'
    std::string pointer;            // /steps/i/params/<key>
    std::size_t producer = 0;       // step index
    std::string artifact;
    std::string field;              // without '/'
};

struct ParamLiteral {
    std::string key;
    std::string pointer;
    json        value;
};

struct StepFacts {
    bool                                   id_ok    = false;
    std::string                            id;
    const TaskGraphTemplateContract*       contract = nullptr;
    std::string                            out;     // declared artifact, when valid
    std::vector<ParamReference>            references;
    std::vector<ParamLiteral>              literals;
    std::vector<std::pair<std::size_t, std::string>> after;  // producer index, pointer
    std::uint32_t                          attempts = 1;
};

class Validator {
public:
    Validator(const StepPlanIntakeOptions& options, Collector& violations)
        : options_(options), violations_(violations) {
        for (const auto& contract : options.template_allowlist)
            contracts_.emplace(contract.template_id, &contract);
        for (const auto& info : options.template_info) infos_.emplace(info.template_id, &info);
        for (const auto& contract : options.template_allowlist)
            template_ids_.push_back(contract.template_id);
        std::sort(template_ids_.begin(), template_ids_.end());
    }

    void validate(const json& document);

    const std::vector<StepFacts>& steps() const noexcept { return steps_; }
    std::uint64_t literal_bytes() const noexcept { return literal_bytes_; }

private:
    void unknown_key(const std::string& pointer, std::string_view key,
                     const std::string& allowed) {
        if (is_authority_word(key)) {
            violations_.add(pointer, "STEP_FORBIDDEN_FIELD",
                            "The key " + quoted(key) + " names a setting the host controls.",
                            "Leave " + quoted(key) + " out; the host decides it.",
                            json{{"given", printable(key)}});
        } else {
            violations_.add(pointer, "STEP_UNKNOWN_FIELD", "The key " + quoted(key) + " is not part of the plan format.",
                            "Use only these keys here: " + allowed + ".",
                            json{{"given", printable(key)}, {"allowed", allowed}});
        }
    }

    void validate_step(const json& step, std::size_t index);
    void validate_params(const json& params, std::size_t index, StepFacts& facts);
    void validate_reference(const json& value, const std::string& pointer, std::size_t index,
                            const std::string& key, StepFacts& facts);
    void validate_literal(const json& value, const std::string& pointer, const std::string& key,
                          StepFacts& facts);
    void validate_after(const json& after, std::size_t index, StepFacts& facts);
    void validate_retry(const json& retry, std::size_t index, StepFacts& facts);

    std::uint32_t literal_ceiling() const {
        return std::min(options_.limits.max_literal_bytes, options_.proposal_limits.max_literal_bytes);
    }
    std::uint32_t literal_total_ceiling() const {
        return std::min(options_.limits.max_total_literal_bytes,
                        options_.proposal_limits.max_total_literal_bytes);
    }
    std::uint32_t attempts_ceiling() const {
        return std::min(options_.limits.max_retry_attempts,
                        std::max<std::uint32_t>(options_.proposal_limits.max_attempts, 1));
    }

    const StepPlanIntakeOptions&                                 options_;
    Collector&                                                   violations_;
    std::map<std::string, const TaskGraphTemplateContract*>      contracts_;
    std::map<std::string, const StepPlanTemplateInfo*>           infos_;
    std::vector<std::string>                                     template_ids_;
    std::vector<StepFacts>                                       steps_;
    std::map<std::string, std::size_t>                           id_index_;
    std::uint64_t                                                literal_bytes_ = 0;
};

void Validator::validate(const json& document) {
    if (!document.is_object()) {
        violations_.add("", "STEP_TYPE", "The plan must be a JSON object.",
                        "Answer with one JSON object that has 'schema' and 'steps'.",
                        json{{"expected", "object"}, {"given", type_name(document)}});
        return;
    }
    for (const auto& [key, value] : document.items()) {
        if (key == "schema" || key == "steps") continue;
        unknown_key(child_pointer("", key), key, "schema, steps");
    }
    if (!document.contains("schema") || !document["schema"].is_string() ||
        document["schema"].get<std::string>() != kSchemaName) {
        violations_.add("/schema", "STEP_SCHEMA", "The plan must declare the schema 'step-plan/v1'.",
                        "Set \"schema\" to \"step-plan/v1\".",
                        json{{"expected", std::string(kSchemaName)}});
    }
    if (!document.contains("steps")) {
        violations_.add("/steps", "STEP_EMPTY", "The plan has no 'steps'.",
                        "Add a non-empty 'steps' array.");
        return;
    }
    const auto steps = document["steps"];
    if (!steps.is_array()) {
        violations_.add("/steps", "STEP_TYPE", "'steps' must be an array.",
                        "Make 'steps' an array of step objects.",
                        json{{"expected", "array"}, {"given", type_name(steps)}});
        return;
    }
    if (steps.empty()) {
        violations_.add("/steps", "STEP_EMPTY", "The plan has no steps.",
                        "Add at least one step to 'steps'.");
        return;
    }
    if (steps.size() > options_.limits.max_steps) {
        violations_.add("/steps", "STEP_LIMIT_STEPS", "The plan has more steps than allowed.",
                        "Use at most " + std::to_string(options_.limits.max_steps) +
                            " steps; merge or drop steps.",
                        json{{"given", steps.size()}, {"max", options_.limits.max_steps}});
    }
    // Step ids first, so a reference can tell an unknown step from a later one.
    steps_.resize(steps.size());
    for (std::size_t index = 0; index < steps.size(); ++index) {
        const auto step = steps[index];
        if (!step.is_object() || !step.contains("id") || !step["id"].is_string()) continue;
        const auto id = step["id"].get<std::string>();
        if (!valid_step_id(id)) continue;
        id_index_.emplace(id, index);
    }
    for (std::size_t index = 0; index < steps.size(); ++index) validate_step(steps[index], index);
}

void Validator::validate_step(const json& step, std::size_t index) {
    const auto pointer = index_pointer("/steps", index);
    auto&      facts   = steps_[index];
    if (!step.is_object()) {
        violations_.add(pointer, "STEP_TYPE", "Each step must be a JSON object.",
                        "Write each step as an object with 'id' and 'use'.",
                        json{{"expected", "object"}, {"given", type_name(step)}});
        return;
    }
    for (const auto& [key, value] : step.items()) {
        if (key == "id" || key == "use" || key == "params" || key == "out" || key == "after" ||
            key == "retry")
            continue;
        unknown_key(child_pointer(pointer, key), key, "id, use, params, out, after, retry");
    }

    // id
    const auto id_pointer = child_pointer(pointer, "id");
    if (!step.contains("id")) {
        violations_.add(id_pointer, "STEP_ID_REQUIRED", "A step needs an 'id'.",
                        "Give the step an id such as \"s1\".");
    } else if (!step["id"].is_string()) {
        violations_.add(id_pointer, "STEP_TYPE", "'id' must be a string.",
                        "Write the id as a string such as \"s1\".",
                        json{{"expected", "string"}, {"given", type_name(step["id"])}});
    } else {
        const auto id = step["id"].get<std::string>();
        if (!valid_step_id(id)) {
            violations_.add(id_pointer, "STEP_ID_FORMAT", "The id " + quoted(id) + " has the wrong form.",
                            "Use 1 to 32 characters: a lowercase letter, then lowercase letters, digits or '_'.",
                            json{{"given", printable(id)}});
        } else {
            const auto first = id_index_.find(id);
            if (first != id_index_.end() && first->second != index) {
                violations_.add(id_pointer, "STEP_ID_DUPLICATE", "The id " + quoted(id) + " is used by an earlier step.",
                                "Give every step its own id.",
                                json{{"given", id}, {"first", first->second}});
            } else {
                facts.id_ok = true;
                facts.id    = id;
            }
        }
    }

    // use
    const auto use_pointer = child_pointer(pointer, "use");
    if (!step.contains("use")) {
        violations_.add(use_pointer, "STEP_USE_REQUIRED", "A step needs a 'use' template.",
                        "Set 'use' to one of: " + names_list(template_ids_) + ".",
                        json{{"choices", names_json(template_ids_)}});
    } else if (!step["use"].is_string()) {
        violations_.add(use_pointer, "STEP_TYPE", "'use' must be a string.",
                        "Write the template id as a string.",
                        json{{"expected", "string"}, {"given", type_name(step["use"])}});
    } else {
        const auto use   = step["use"].get<std::string>();
        const auto found = contracts_.find(use);
        if (found == contracts_.end()) {
            const auto close = nearest(use, template_ids_, 3);
            std::string hint = "Use one of: " + names_list(template_ids_) + ".";
            if (!close.empty()) hint += " Closest: " + close.front() + ".";
            json facts_json{{"given", printable(use)}, {"choices", names_json(template_ids_)}};
            violations_.add(use_pointer, "STEP_USE_UNKNOWN",
                            "Template " + quoted(use) + " is not available.", std::move(hint),
                            std::move(facts_json));
        } else {
            facts.contract = found->second;
        }
    }

    // out
    if (step.contains("out")) {
        const auto out_pointer = child_pointer(pointer, "out");
        if (!step["out"].is_string()) {
            violations_.add(out_pointer, "STEP_TYPE", "'out' must be a string.",
                            "Write the artifact name as a string.",
                            json{{"expected", "string"}, {"given", type_name(step["out"])}});
        } else if (facts.contract) {
            const auto out = step["out"].get<std::string>();
            if (!find_artifact(*facts.contract, out)) {
                const auto names = artifact_names(*facts.contract);
                violations_.add(out_pointer, "STEP_OUT_UNKNOWN",
                                "Template " + quoted(facts.contract->template_id) + " has no output " + quoted(out) + ".",
                                names.empty() ? "Leave 'out' out; this template declares no outputs."
                                              : "Use one of: " + names_list(names) + ".",
                                json{{"given", printable(out)}, {"choices", names_json(names)}});
            } else {
                facts.out = out;
            }
        }
    }

    if (step.contains("params")) {
        const auto params = step["params"];
        if (!params.is_object()) {
            violations_.add(child_pointer(pointer, "params"), "STEP_TYPE", "'params' must be an object.",
                            "Write 'params' as an object of input names and values.",
                            json{{"expected", "object"}, {"given", type_name(params)}});
        } else {
            validate_params(params, index, facts);
        }
    } else if (facts.contract) {
        // Required inputs are checked even when 'params' is absent.
        const auto info = infos_.find(facts.contract->template_id);
        if (info != infos_.end()) {
            for (const auto& required : info->second->required_params) {
                violations_.add(child_pointer(child_pointer(pointer, "params"), required),
                                "STEP_PARAM_MISSING",
                                "Template " + quoted(facts.contract->template_id) + " needs the input " +
                                    quoted(required) + ".",
                                "Add \"" + required + "\" to 'params'.",
                                json{{"name", required}});
            }
        }
    }

    if (step.contains("after")) validate_after(step["after"], index, facts);
    if (step.contains("retry")) validate_retry(step["retry"], index, facts);
}

void Validator::validate_params(const json& params, std::size_t index, StepFacts& facts) {
    const auto base = child_pointer(index_pointer("/steps", index), "params");
    if (params.size() > options_.limits.max_params_per_step) {
        violations_.add(base, "STEP_LIMIT_PARAMS", "The step has more inputs than allowed.",
                        "Give at most " + std::to_string(options_.limits.max_params_per_step) + " inputs.",
                        json{{"given", params.size()}, {"max", options_.limits.max_params_per_step}});
    }
    std::vector<std::string> accepted;
    if (facts.contract) accepted = input_names(*facts.contract);
    std::set<std::string> present;
    for (const auto& [key, value] : params.items()) {
        present.insert(key);
        const auto pointer = child_pointer(base, key);
        if (facts.contract && !std::binary_search(accepted.begin(), accepted.end(), key)) {
            if (is_authority_word(key)) {
                unknown_key(pointer, key, "");
            } else {
                violations_.add(pointer, "STEP_PARAM_UNKNOWN",
                                "Template " + quoted(facts.contract->template_id) + " has no input " + quoted(key) + ".",
                                accepted.empty() ? "This template takes no inputs; leave 'params' out."
                                                 : "Use one of: " + names_list(accepted) + ".",
                                json{{"given", printable(key)}, {"choices", names_json(accepted)}});
            }
            continue;
        }
        if (value.is_object() && value.contains("from_step")) {
            validate_reference(value, pointer, index, key, facts);
        } else if (facts.contract) {
            validate_literal(value, pointer, key, facts);
        }
    }
    if (facts.contract) {
        const auto info = infos_.find(facts.contract->template_id);
        if (info != infos_.end()) {
            for (const auto& required : info->second->required_params) {
                if (present.count(required) != 0) continue;
                violations_.add(child_pointer(base, required), "STEP_PARAM_MISSING",
                                "Template " + quoted(facts.contract->template_id) + " needs the input " +
                                    quoted(required) + ".",
                                "Add \"" + required + "\" to 'params'.", json{{"name", required}});
            }
        }
    }
}

void Validator::validate_literal(const json& value, const std::string& pointer,
                                 const std::string& key, StepFacts& facts) {
    if (has_non_finite(value)) {
        violations_.add(pointer, "STEP_LITERAL_NUMBER", "A number in this value is not finite.",
                        "Use only finite numbers.");
        return;
    }
    if (has_reserved_key(value)) {
        violations_.add(pointer, "STEP_LITERAL_RESERVED_KEY",
                        "A literal value may not contain the key 'from_step'.",
                        "Write a reference as {\"from_step\": id, ...} on its own, or rename the key.");
        return;
    }
    const auto depth = literal_depth(value);
    if (depth > options_.limits.max_literal_depth) {
        violations_.add(pointer, "STEP_LITERAL_DEPTH", "The value is nested too deeply.",
                        "Nest at most " + std::to_string(options_.limits.max_literal_depth) + " levels.",
                        json{{"given", depth}, {"max", options_.limits.max_literal_depth}});
        return;
    }
    const auto bytes = detail::canonical_json_bytes(value).size();
    const auto ceiling = literal_ceiling();
    if (ceiling == 0) {
        violations_.add(pointer, "STEP_LITERAL_TOO_LARGE", "This host accepts no literal values.",
                        "Take the value from an earlier step with {\"from_step\": id}.",
                        json{{"max", 0}});
        return;
    }
    if (bytes > ceiling) {
        violations_.add(pointer, "STEP_LITERAL_TOO_LARGE", "The value is too large.",
                        "Keep each literal under " + std::to_string(ceiling) + " bytes.",
                        json{{"given", bytes}, {"max", ceiling}});
        return;
    }
    literal_bytes_ += bytes;
    if (literal_bytes_ > literal_total_ceiling()) {
        violations_.add(pointer, "STEP_LITERAL_TOO_LARGE", "The literals of the plan are too large together.",
                        "Keep all literals under " + std::to_string(literal_total_ceiling()) + " bytes in total.",
                        json{{"given", literal_bytes_}, {"max", literal_total_ceiling()}});
        return;
    }
    facts.literals.push_back(ParamLiteral{key, pointer, value});
}

void Validator::validate_reference(const json& value, const std::string& pointer,
                                   std::size_t index, const std::string& key, StepFacts& facts) {
    bool malformed = false;
    for (const auto& [name, item] : value.items()) {
        if (name == "from_step" || name == "output" || name == "field") {
            if (!item.is_string()) {
                violations_.add(child_pointer(pointer, name), "STEP_REF_SYNTAX",
                                "'" + name + "' of a reference must be a string.",
                                "Write a reference as {\"from_step\": \"s1\"}, optionally with \"output\" and \"field\".",
                                json{{"expected", "string"}, {"given", type_name(item)}});
                malformed = true;
            }
        } else {
            violations_.add(child_pointer(pointer, name), "STEP_REF_SYNTAX",
                            "A reference may only have 'from_step', 'output' and 'field'.",
                            "Remove " + quoted(name) + " from the reference.",
                            json{{"given", printable(name)}});
            malformed = true;
        }
    }
    if (malformed || !value["from_step"].is_string()) return;

    const auto from      = value["from_step"].get<std::string>();
    const auto from_ptr  = child_pointer(pointer, "from_step");
    const auto producer  = id_index_.find(from);
    if (producer == id_index_.end()) {
        std::vector<std::string> ids;
        for (const auto& [id, position] : id_index_)
            if (position < index) ids.push_back(id);
        violations_.add(from_ptr, "STEP_REF_UNKNOWN_STEP", "There is no step " + quoted(from) + ".",
                        ids.empty() ? "Take the value from an earlier step, or write a literal."
                                    : "Use the id of an earlier step: " + names_list(ids) + ".",
                        json{{"given", printable(from)}, {"choices", names_json(ids)}});
        return;
    }
    if (producer->second >= index) {
        violations_.add(from_ptr, "STEP_REF_FORWARD",
                        "Step " + quoted(from) + " does not come before this step.",
                        "Reference only steps listed earlier in 'steps'; reorder the steps or drop the reference.",
                        json{{"given", from}});
        return;
    }
    const auto& producer_facts = steps_[producer->second];
    if (!producer_facts.contract) return;  // its own violation was reported
    const auto& contract = *producer_facts.contract;

    // output
    std::string output;
    if (value.contains("output")) {
        output = value["output"].get<std::string>();
        if (!find_artifact(contract, output)) {
            violations_.add(child_pointer(pointer, "output"), "STEP_REF_OUTPUT_UNKNOWN",
                            "Step " + quoted(from) + " has no output " + quoted(output) + ".",
                            "Use one of: " + names_list(artifact_names(contract)) + ".",
                            json{{"given", printable(output)}, {"choices", names_json(artifact_names(contract))}});
            return;
        }
        if (!producer_facts.out.empty() && producer_facts.out != output) {
            violations_.add(child_pointer(pointer, "output"), "STEP_REF_OUT_MISMATCH",
                            "Step " + quoted(from) + " produces " + quoted(producer_facts.out) + ", not " + quoted(output) + ".",
                            "Use \"output\": \"" + producer_facts.out + "\" or leave 'output' out.",
                            json{{"given", printable(output)}, {"expected", producer_facts.out}});
            return;
        }
    } else if (!producer_facts.out.empty()) {
        output = producer_facts.out;
    } else if (contract.output_artifacts.size() == 1) {
        output = contract.output_artifacts.front().artifact;
    } else {
        violations_.add(pointer, "STEP_REF_OUTPUT_UNKNOWN",
                        contract.output_artifacts.empty()
                            ? "Step " + quoted(from) + " has no output to take."
                            : "Step " + quoted(from) + " has several outputs and the reference does not say which.",
                        contract.output_artifacts.empty()
                            ? "Take the value from another step."
                            : "Add \"output\" with one of: " + names_list(artifact_names(contract)) + ".",
                        json{{"choices", names_json(artifact_names(contract))}});
        return;
    }
    const auto* artifact = find_artifact(contract, output);

    // field
    std::string field;
    if (value.contains("field")) {
        field = value["field"].get<std::string>();
        const auto fields = field_names(*artifact);
        if (std::find(fields.begin(), fields.end(), field) == fields.end()) {
            violations_.add(child_pointer(pointer, "field"), "STEP_REF_FIELD_UNKNOWN",
                            "Output " + quoted(output) + " of step " + quoted(from) + " has no field " + quoted(field) + ".",
                            "Use one of: " + names_list(fields) + ".",
                            json{{"given", printable(field)}, {"choices", names_json(fields)}});
            return;
        }
    } else if (artifact->fields.size() == 1) {
        field = without_slash(artifact->fields.front());
    } else {
        const auto fields = field_names(*artifact);
        violations_.add(pointer, "STEP_REF_FIELD_AMBIGUOUS",
                        "Output " + quoted(output) + " of step " + quoted(from) + " has several fields and the reference does not say which.",
                        "Add \"field\" with one of: " + names_list(fields) + ".",
                        json{{"choices", names_json(fields)}});
        return;
    }
    facts.references.push_back(ParamReference{key, pointer, producer->second, output, field});
}

void Validator::validate_after(const json& after, std::size_t index, StepFacts& facts) {
    const auto pointer = child_pointer(index_pointer("/steps", index), "after");
    if (!after.is_array()) {
        violations_.add(pointer, "STEP_TYPE", "'after' must be an array.",
                        "Write 'after' as an array of earlier step ids.",
                        json{{"expected", "array"}, {"given", type_name(after)}});
        return;
    }
    if (after.size() > options_.limits.max_after_per_step) {
        violations_.add(pointer, "STEP_LIMIT_AFTER", "The step waits for more steps than allowed.",
                        "List at most " + std::to_string(options_.limits.max_after_per_step) + " steps in 'after'.",
                        json{{"given", after.size()}, {"max", options_.limits.max_after_per_step}});
    }
    std::set<std::string> seen;
    for (std::size_t position = 0; position < after.size(); ++position) {
        const auto item_pointer = index_pointer(pointer, position);
        const auto item         = after[position];
        if (!item.is_string()) {
            violations_.add(item_pointer, "STEP_TYPE", "Each entry of 'after' must be a step id string.",
                            "Write the id of an earlier step.",
                            json{{"expected", "string"}, {"given", type_name(item)}});
            continue;
        }
        const auto id    = item.get<std::string>();
        const auto found = id_index_.find(id);
        if (found == id_index_.end()) {
            violations_.add(item_pointer, "STEP_AFTER_UNKNOWN", "There is no step " + quoted(id) + ".",
                            "Name an earlier step.", json{{"given", printable(id)}});
            continue;
        }
        if (found->second >= index) {
            violations_.add(item_pointer, "STEP_AFTER_FORWARD",
                            "Step " + quoted(id) + " does not come before this step.",
                            "Name only steps listed earlier in 'steps'.", json{{"given", id}});
            continue;
        }
        if (!seen.insert(id).second) {
            violations_.add(item_pointer, "STEP_AFTER_DUPLICATE", "Step " + quoted(id) + " is listed twice.",
                            "List each step once.", json{{"given", id}});
            continue;
        }
        facts.after.emplace_back(found->second, item_pointer);
    }
}

void Validator::validate_retry(const json& retry, std::size_t index, StepFacts& facts) {
    const auto pointer = child_pointer(index_pointer("/steps", index), "retry");
    if (!retry.is_object()) {
        violations_.add(pointer, "STEP_TYPE", "'retry' must be an object.",
                        "Write 'retry' as {\"max_attempts\": 2}.",
                        json{{"expected", "object"}, {"given", type_name(retry)}});
        return;
    }
    bool clean = true;
    for (const auto& [key, value] : retry.items()) {
        if (key == "max_attempts") continue;
        violations_.add(child_pointer(pointer, key), "STEP_RETRY_FIELD",
                        "'retry' has no key " + quoted(key) + ".", "Use only \"max_attempts\".",
                        json{{"given", printable(key)}});
        clean = false;
    }
    if (!retry.contains("max_attempts")) {
        if (clean)
            violations_.add(pointer, "STEP_RETRY_FIELD", "'retry' needs \"max_attempts\".",
                            "Write 'retry' as {\"max_attempts\": 2}.");
        return;
    }
    const auto value = retry["max_attempts"];
    const auto attempts_pointer = child_pointer(pointer, "max_attempts");
    if (!(value.is_number_integer() || value.is_number_unsigned())) {
        violations_.add(attempts_pointer, "STEP_TYPE", "'max_attempts' must be an integer.",
                        "Write a whole number.",
                        json{{"expected", "integer"}, {"given", type_name(value)}});
        return;
    }
    const auto ceiling = attempts_ceiling();
    const auto given   = value.is_number_unsigned() ? static_cast<std::int64_t>(value.get<std::uint64_t>())
                                                    : value.get<std::int64_t>();
    if (given < 1 || given > static_cast<std::int64_t>(ceiling)) {
        violations_.add(attempts_pointer, "STEP_RETRY_RANGE", "'max_attempts' must be between 1 and " + std::to_string(ceiling) + ".",
                        "Use a number from 1 to " + std::to_string(ceiling) + ".",
                        json{{"given", given}, {"min", 1}, {"max", ceiling}});
        return;
    }
    facts.attempts = static_cast<std::uint32_t>(given);
}

// ----- Lowering -----

std::string artifact_pointer(std::string_view field) { return "/" + std::string(field); }

TaskGraphBudget lowered_budget(const TaskGraphTemplateContract& contract,
                               const TaskGraphProposalLimits&   limits) {
    return TaskGraphBudget{
        std::min(contract.budget_ceiling.wall_time_ms, limits.per_task_budget_ceiling.wall_time_ms),
        std::min(contract.budget_ceiling.model_tokens, limits.per_task_budget_ceiling.model_tokens),
        std::min(contract.budget_ceiling.monetary_microunits,
                 limits.per_task_budget_ceiling.monetary_microunits),
        std::min(contract.budget_ceiling.output_bytes, limits.per_task_budget_ceiling.output_bytes)};
}

json budget_json(const TaskGraphBudget& budget) {
    return json{{"wall_time_ms", budget.wall_time_ms},
                {"model_tokens", budget.model_tokens},
                {"monetary_microunits", budget.monetary_microunits},
                {"output_bytes", budget.output_bytes}};
}

struct Lowered {
    json                         document;
    std::vector<SourceMapEntry>  source_map;
    json                         report = json::object();
};

Lowered lower(const std::vector<StepFacts>& steps, const StepPlanIntakeOptions& options,
              std::uint64_t literal_bytes) {
    Lowered lowered;
    json    tasks = json::array();
    const auto authored = [&](std::string_view pointer) {
        return SourceCoordinate{options.source_id, std::string(pointer), std::nullopt};
    };
    const auto map = [&](std::string generated, std::string_view source) {
        lowered.source_map.push_back({std::move(generated), authored(source)});
    };

    std::vector<std::set<std::size_t>> dependencies(steps.size());
    for (std::size_t index = 0; index < steps.size(); ++index) {
        const auto& step = steps[index];
        const auto  task_pointer = index_pointer("/tasks", index);
        const auto  step_pointer = index_pointer("/steps", index);
        map(task_pointer, step_pointer);
        map(child_pointer(task_pointer, "id"), child_pointer(step_pointer, "id"));
        map(child_pointer(task_pointer, "template"), child_pointer(step_pointer, "use"));
        // The budget comes from the template, so every part of it points at `use`.
        map(child_pointer(task_pointer, "budget"), child_pointer(step_pointer, "use"));
        for (const auto* dimension :
             {"wall_time_ms", "model_tokens", "monetary_microunits", "output_bytes"})
            map(child_pointer(child_pointer(task_pointer, "budget"), dimension),
                child_pointer(step_pointer, "use"));

        // Which authored pointer made each dependency, for diagnostics.
        std::map<std::size_t, std::string> cause;
        std::vector<const ParamReference*> references;
        for (const auto& reference : step.references) references.push_back(&reference);
        std::sort(references.begin(), references.end(), [](const auto* lhs, const auto* rhs) {
            return lhs->key < rhs->key;
        });
        json bindings = json::array();
        for (std::size_t position = 0; position < references.size(); ++position) {
            const auto& reference = *references[position];
            dependencies[index].insert(reference.producer);
            cause.emplace(reference.producer, child_pointer(reference.pointer, "from_step"));
            bindings.push_back(
                json{{"from", json{{"task", steps[reference.producer].id},
                                   {"artifact", reference.artifact},
                                   {"field", artifact_pointer(reference.field)}}},
                     {"to", json{{"field", artifact_pointer(reference.key)}}}});
            const auto binding_pointer = index_pointer(child_pointer(task_pointer, "input_bindings"), position);
            // Every part of a binding points at the parameter, apart from the producer.
            map(binding_pointer, reference.pointer);
            map(child_pointer(binding_pointer, "from"), reference.pointer);
            map(child_pointer(child_pointer(binding_pointer, "from"), "task"),
                child_pointer(reference.pointer, "from_step"));
            map(child_pointer(child_pointer(binding_pointer, "from"), "artifact"), reference.pointer);
            map(child_pointer(child_pointer(binding_pointer, "from"), "field"), reference.pointer);
            map(child_pointer(binding_pointer, "to"), reference.pointer);
            map(child_pointer(child_pointer(binding_pointer, "to"), "field"), reference.pointer);
        }
        for (const auto& [producer, pointer] : step.after) {
            dependencies[index].insert(producer);
            cause.emplace(producer, pointer);
        }
        std::vector<std::string> ids;
        for (const auto producer : dependencies[index]) ids.push_back(steps[producer].id);
        std::sort(ids.begin(), ids.end());
        json depends = json::array();
        for (const auto& id : ids) depends.push_back(id);
        {
            std::vector<std::pair<std::string, std::string>> by_id;
            for (const auto producer : dependencies[index])
                by_id.emplace_back(steps[producer].id, cause[producer]);
            std::sort(by_id.begin(), by_id.end());
            for (std::size_t position = 0; position < by_id.size(); ++position)
                map(index_pointer(child_pointer(task_pointer, "depends_on"), position), by_id[position].second);
        }

        json task{{"id", step.id},
                  {"template", step.contract->template_id},
                  {"input_bindings", std::move(bindings)},
                  {"depends_on", std::move(depends)},
                  {"budget", budget_json(lowered_budget(*step.contract, options.proposal_limits))}};
        if (!step.literals.empty()) {
            std::vector<const ParamLiteral*> literals;
            for (const auto& literal : step.literals) literals.push_back(&literal);
            std::sort(literals.begin(), literals.end(), [](const auto* lhs, const auto* rhs) {
                return lhs->key < rhs->key;
            });
            json encoded = json::array();
            for (std::size_t position = 0; position < literals.size(); ++position) {
                const auto& literal = *literals[position];
                encoded.push_back(json{{"to", json{{"field", artifact_pointer(literal.key)}}},
                                       {"value", literal.value}});
                const auto literal_pointer = index_pointer(child_pointer(task_pointer, "literals"), position);
                map(literal_pointer, literal.pointer);
                map(child_pointer(literal_pointer, "to"), literal.pointer);
                map(child_pointer(child_pointer(literal_pointer, "to"), "field"), literal.pointer);
                map(child_pointer(literal_pointer, "value"), literal.pointer);
            }
            task["literals"] = std::move(encoded);
        }
        if (step.attempts > 1) {
            task["max_attempts"] = step.attempts;
            map(child_pointer(task_pointer, "max_attempts"),
                child_pointer(child_pointer(step_pointer, "retry"), "max_attempts"));
        }
        tasks.push_back(std::move(task));
    }
    lowered.document = json{{"schema_version", TaskGraphProposal::SCHEMA_VERSION_2},
                            {"tasks", std::move(tasks)},
                            {"join", json{{"kind", "all"}}}};

    // The report: size and shape of the accepted plan.
    std::uint64_t edges = 0;
    std::vector<std::size_t> level(steps.size(), 1);
    for (std::size_t index = 0; index < steps.size(); ++index) {
        edges += dependencies[index].size();
        for (const auto producer : dependencies[index])
            level[index] = std::max(level[index], level[producer] + 1);
    }
    std::size_t depth = 0;
    std::map<std::size_t, std::size_t> per_level;
    for (const auto value : level) {
        depth = std::max(depth, value);
        ++per_level[value];
    }
    std::size_t width = 0;
    for (const auto& [value, count] : per_level) width = std::max(width, count);
    lowered.report = json{{"steps", steps.size()},
                          {"edges", edges},
                          {"depth", depth},
                          {"width", width},
                          {"literal_bytes", literal_bytes}};
    return lowered;
}

void check_plan_limits(const std::vector<StepFacts>& steps, const StepPlanIntakeOptions& options,
                       const json& report, Collector& violations) {
    const auto edges = report["edges"].get<std::uint64_t>();
    const auto depth = report["depth"].get<std::uint64_t>();
    if (edges > options.proposal_limits.max_edges) {
        violations.add("/steps", "STEP_LIMIT_EDGES", "The plan has more dependencies than allowed.",
                       "Use at most " + std::to_string(options.proposal_limits.max_edges) +
                           " dependencies; drop 'after' entries or references.",
                       json{{"given", edges}, {"max", options.proposal_limits.max_edges}});
    }
    if (depth > options.proposal_limits.max_depth) {
        violations.add("/steps", "STEP_LIMIT_DEPTH", "The plan is a longer chain than allowed.",
                       "Make at most " + std::to_string(options.proposal_limits.max_depth) +
                           " steps depend on each other in a row; run independent steps side by side.",
                       json{{"given", depth}, {"max", options.proposal_limits.max_depth}});
    }
    // Each attempt may spend the step's whole budget, so a retry multiplies its reservation.
    std::array<std::pair<std::string_view, std::uint64_t>, 4> sums = {{
        {"wall_time_ms", 0}, {"model_tokens", 0}, {"monetary_microunits", 0}, {"output_bytes", 0}}};
    std::array<bool, 4> overflow{};
    const auto add = [&](std::size_t dimension, std::uint64_t amount, std::uint32_t attempts) {
        if (attempts != 0 && amount > std::numeric_limits<std::uint64_t>::max() / attempts) {
            overflow[dimension] = true;
            return;
        }
        const auto total = amount * attempts;
        if (total > std::numeric_limits<std::uint64_t>::max() - sums[dimension].second) {
            overflow[dimension] = true;
            return;
        }
        sums[dimension].second += total;
    };
    for (const auto& step : steps) {
        const auto budget = lowered_budget(*step.contract, options.proposal_limits);
        add(0, budget.wall_time_ms, step.attempts);
        add(1, budget.model_tokens, step.attempts);
        add(2, budget.monetary_microunits, step.attempts);
        add(3, budget.output_bytes, step.attempts);
    }
    const std::array<std::uint64_t, 4> ceilings = {options.proposal_limits.total_budget_ceiling.wall_time_ms,
                                                  options.proposal_limits.total_budget_ceiling.model_tokens,
                                                  options.proposal_limits.total_budget_ceiling.monetary_microunits,
                                                  options.proposal_limits.total_budget_ceiling.output_bytes};
    for (std::size_t dimension = 0; dimension < 4; ++dimension) {
        if (!overflow[dimension] && sums[dimension].second <= ceilings[dimension]) continue;
        violations.add("/steps", "STEP_LIMIT_BUDGET", "The steps together reserve more than the plan may spend.",
                       "Use fewer or cheaper steps, or fewer retries.",
                       json{{"resource", std::string(sums[dimension].first)},
                            {"requested", overflow[dimension] ? json(nullptr) : json(sums[dimension].second)},
                            {"ceiling", ceilings[dimension]}});
    }
}

}  // namespace

std::string_view to_string(StepPlanEnvelope envelope) noexcept {
    switch (envelope) {
    case StepPlanEnvelope::None: return "none";
    case StepPlanEnvelope::Strict: return "strict";
    case StepPlanEnvelope::Recovered: return "recovered";
    }
    return "unknown";
}

namespace {

// Whether `value` nests deeper than `limit` levels, without recursion.
bool nests_deeper_than(const json& value, std::size_t limit) {
    std::vector<std::pair<json, std::size_t>> pending;
    pending.emplace_back(value, 1);
    while (!pending.empty()) {
        auto [current, depth] = std::move(pending.back());
        pending.pop_back();
        if (!current.is_array() && !current.is_object()) continue;
        if (depth > limit) return true;
        if (current.is_array()) {
            for (std::size_t index = 0; index < current.size(); ++index)
                pending.emplace_back(current[index], depth + 1);
        } else {
            for (const auto& [key, item] : current.items()) pending.emplace_back(item, depth + 1);
        }
    }
    return false;
}

constexpr std::size_t kMaxDocumentDepth = 64;

StepPlanIntakeResult run_intake(const json& document, StepPlanEnvelope envelope,
                                const StepPlanIntakeOptions& options) {
    StepPlanIntakeResult result;
    result.envelope = envelope;
    Collector violations;
    if (nests_deeper_than(document, kMaxDocumentDepth)) {
        violations.add("", "STEP_LIMIT_DOCUMENT", "The plan is nested too deeply.",
                       "Keep the plan within " + std::to_string(kMaxDocumentDepth) + " levels of nesting.",
                       json{{"max_depth", kMaxDocumentDepth}});
        result.violations = violations.finish(options.limits.max_violations);
        return result;
    }
    try {
        const auto canonical = detail::canonical_json_bytes(document);
        result.plan_digest   = detail::sha256_identity(kSchemaName, canonical);
        if (canonical.size() > options.limits.max_document_bytes) {
            violations.add("", "STEP_LIMIT_DOCUMENT", "The plan is larger than allowed.",
                           "Keep the plan under " + std::to_string(options.limits.max_document_bytes) + " bytes.",
                           json{{"max", options.limits.max_document_bytes}});
            result.violations = violations.finish(options.limits.max_violations);
            return result;
        }
    } catch (const std::exception&) {
        violations.add("", "STEP_NOT_JSON", "The plan is not valid JSON data.",
                       "Answer with one JSON object.");
        result.violations = violations.finish(options.limits.max_violations);
        result.plan_digest.clear();
        return result;
    }

    Validator validator(options, violations);
    validator.validate(document);
    if (violations.empty()) {
        auto lowered = lower(validator.steps(), options, validator.literal_bytes());
        check_plan_limits(validator.steps(), options, lowered.report, violations);
        if (violations.empty()) {
            TaskGraphProposalOptions proposal_options;
            proposal_options.source_id          = options.source_id;
            proposal_options.source_map         = lowered.source_map;
            proposal_options.limits             = options.proposal_limits;
            proposal_options.template_allowlist = options.template_allowlist;
            try {
                const auto proposal = TaskGraphProposal::parse(lowered.document, proposal_options);
                result.proposal      = proposal.to_json();
                result.proposal_hash = proposal.proposal_hash();
                result.source_map    = proposal.source_map();
                result.report        = std::move(lowered.report);
            } catch (const TaskGraphProposalError& error) {
                // The lowered form broke a rule intake did not check: a host policy that
                // differs from the limits above, or a gap in intake. Parse stays the authority.
                for (const auto& diagnostic : error.diagnostics()) {
                    violations.add(diagnostic.primary.json_pointer, "STEP_LOWERING_REJECTED",
                                   "The plan was rejected when it was turned into a task graph: " +
                                       printable(diagnostic.message, 160) + ".",
                                   "Change the step this points to, or ask the host to check its limits.",
                                   json{{"proposal_code", diagnostic.code}});
                }
            }
        }
    }
    result.violations = violations.finish(options.limits.max_violations);
    if (!result.violations.empty()) {
        result.proposal.reset();
        result.proposal_hash.reset();
        result.source_map.clear();
        result.report = json::object();
    }
    return result;
}

}  // namespace

StepPlanIntakeResult intake_step_plan_document(const json& document,
                                               const StepPlanIntakeOptions& options) {
    return run_intake(document, StepPlanEnvelope::None, options);
}

StepPlanIntakeResult intake_step_plan_text(std::string_view text,
                                           const StepPlanIntakeOptions& options) {
    StepPlanIntakeResult result;
    Collector            violations;
    if (text.size() > static_cast<std::size_t>(options.limits.max_document_bytes) * 4 + 4096) {
        violations.add("", "STEP_LIMIT_DOCUMENT", "The answer is larger than allowed.",
                       "Answer with one short JSON object.",
                       json{{"max", options.limits.max_document_bytes}});
        result.violations = violations.finish(options.limits.max_violations);
        return result;
    }
    const auto extracted = extract_object(text);
    if (!extracted.found) {
        violations.add("", "STEP_NOT_JSON", "No complete JSON object was found in the answer.",
                       "Answer with one JSON object that has 'schema' and 'steps'.");
        result.violations = violations.finish(options.limits.max_violations);
        return result;
    }
    if (extracted.object.size() > options.limits.max_document_bytes) {
        violations.add("", "STEP_LIMIT_DOCUMENT", "The plan is larger than allowed.",
                       "Keep the plan under " + std::to_string(options.limits.max_document_bytes) + " bytes.",
                       json{{"max", options.limits.max_document_bytes}});
        result.envelope   = extracted.envelope;
        result.violations = violations.finish(options.limits.max_violations);
        return result;
    }
    Scanner scanner{extracted.object};
    const bool structured = scanner.value("", 0);
    scanner.skip_space();
    if (!structured || scanner.pos != extracted.object.size()) {
        violations.add("", "STEP_NOT_JSON", "The JSON object in the answer is not valid.",
                       "Answer with one valid JSON object: quoted keys, no trailing commas, no comments.");
        result.envelope   = extracted.envelope;
        result.violations = violations.finish(options.limits.max_violations);
        return result;
    }
    if (!scanner.duplicates.empty()) {
        for (const auto& pointer : scanner.duplicates)
            violations.add(pointer, "STEP_DUPLICATE_KEY", "A key appears twice in the same object.",
                           "Write each key once per object.");
        result.envelope   = extracted.envelope;
        result.violations = violations.finish(options.limits.max_violations);
        return result;
    }
    json document;
    try {
        document = json::parse(extracted.object);
    } catch (const std::exception&) {
        violations.add("", "STEP_NOT_JSON", "The JSON object in the answer is not valid.",
                       "Answer with one valid JSON object: quoted keys, no trailing commas, no comments.");
        result.envelope   = extracted.envelope;
        result.violations = violations.finish(options.limits.max_violations);
        return result;
    }
    return run_intake(document, extracted.envelope, options);
}

// ----- Prompt text -----

std::string format_violations_for_model(const std::vector<StepPlanViolation>& violations,
                                        std::size_t max_bytes) {
    const std::string header =
        "The plan was not accepted. Fix every problem below and answer with the complete corrected plan "
        "as one JSON object.\n";
    std::vector<std::string> lines;
    lines.reserve(violations.size());
    for (std::size_t index = 0; index < violations.size(); ++index) {
        const auto& item = violations[index];
        std::string line = std::to_string(index + 1) + ". " + (item.path.empty() ? "plan" : item.path) +
                           " [" + item.code + "] " + item.message;
        if (!item.hint.empty()) line += " " + item.hint;
        line.push_back('\n');
        lines.push_back(std::move(line));
    }
    // The most lines that fit together with the note that says how many were left out.
    const auto note = [&](std::size_t shown) {
        return shown == lines.size()
                   ? std::string()
                   : "... and " + std::to_string(lines.size() - shown) + " more.\n";
    };
    std::size_t used = header.size();
    std::size_t best = 0;
    std::vector<std::size_t> prefix{used};
    for (const auto& line : lines) prefix.push_back(prefix.back() + line.size());
    for (std::size_t shown = lines.size() + 1; shown-- > 0;) {
        if (prefix[shown] + note(shown).size() <= max_bytes) {
            best = shown;
            break;
        }
    }
    std::string result = header;
    for (std::size_t index = 0; index < best; ++index) result += lines[index];
    result += note(best);
    if (result.size() > max_bytes) result.resize(max_bytes);
    return result;
}

// ----- JSON Schema for provider-enforced output -----

json step_plan_json_schema(const std::vector<TaskGraphTemplateContract>& templates,
                           const StepPlanLimits&                         limits) {
    json value_schema = json::object();
    json reference{{"type", "object"},
                   {"additionalProperties", false},
                   {"required", json::array({"from_step"})},
                   {"properties",
                    json{{"from_step", json{{"$ref", "#/$defs/step_id"}}},
                         {"output", json{{"type", "string"}}},
                         {"field", json{{"type", "string"}}}}}};
    json literal{{"not", json{{"type", "object"}, {"required", json::array({"from_step"})}}}};
    json any_value{{"anyOf", json::array({json{{"$ref", "#/$defs/reference"}},
                                          json{{"$ref", "#/$defs/literal"}}})}};

    std::vector<const TaskGraphTemplateContract*> ordered;
    for (const auto& contract : templates) ordered.push_back(&contract);
    std::sort(ordered.begin(), ordered.end(), [](const auto* lhs, const auto* rhs) {
        return lhs->template_id < rhs->template_id;
    });
    json variants = json::array();
    for (const auto* contract : ordered) {
        json params_properties = json::object();
        for (const auto& name : input_names(*contract)) params_properties[name] = any_value;
        json params{{"type", "object"},
                    {"additionalProperties", false},
                    {"maxProperties", limits.max_params_per_step},
                    {"properties", std::move(params_properties)}};
        json properties{{"id", json{{"$ref", "#/$defs/step_id"}}},
                        {"use", json{{"const", contract->template_id}}},
                        {"params", std::move(params)},
                        {"after", json{{"type", "array"},
                                       {"items", json{{"$ref", "#/$defs/step_id"}}},
                                       {"maxItems", limits.max_after_per_step},
                                       {"uniqueItems", true}}},
                        {"retry", json{{"type", "object"},
                                       {"additionalProperties", false},
                                       {"required", json::array({"max_attempts"})},
                                       {"properties",
                                        json{{"max_attempts",
                                              json{{"type", "integer"},
                                                   {"minimum", 1},
                                                   {"maximum", limits.max_retry_attempts}}}}}}}};
        const auto artifacts = artifact_names(*contract);
        if (!artifacts.empty()) {
            json names = json::array();
            auto sorted = artifacts;
            std::sort(sorted.begin(), sorted.end());
            for (const auto& name : sorted) names.push_back(name);
            properties["out"] = json{{"enum", std::move(names)}};
        }
        variants.push_back(json{{"type", "object"},
                                {"additionalProperties", false},
                                {"required", json::array({"id", "use"})},
                                {"properties", std::move(properties)}});
    }
    json steps{{"type", "array"},
               {"minItems", 1},
               {"maxItems", limits.max_steps},
               {"items", json{{"oneOf", std::move(variants)}}}};
    return json{{"$schema", "https://json-schema.org/draft/2020-12/schema"},
                {"title", "NeoGraph step-plan/v1 for one template catalog"},
                {"type", "object"},
                {"additionalProperties", false},
                {"required", json::array({"schema", "steps"})},
                {"properties", json{{"schema", json{{"const", std::string(kSchemaName)}}},
                                    {"steps", std::move(steps)}}},
                {"$defs", json{{"step_id", json{{"type", "string"},
                                                {"pattern", "^[a-z][a-z0-9_]{0,31}$"}}},
                               {"reference", std::move(reference)},
                               {"literal", std::move(literal)}}}};
}

}  // namespace neograph::program
