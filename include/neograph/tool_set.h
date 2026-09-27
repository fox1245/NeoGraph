#pragma once

#include <neograph/tool.h>

#include <algorithm>
#include <stdexcept>
#include <memory>
#include <utility>
#include <vector>

namespace neograph {

/**
 * @brief Copyable owner of a fixed collection of tools.
 *
 * Copies share ownership of the exact same pointees. Only an owned ToolSet
 * can enter NodeContext; view() is a temporary raw lookup for node factories.
 * Empty sets do not allocate. A compiled graph and its engine each retain
 * their own copy, so neither depends on the original context's lifetime.
 */
class ToolSet {
public:
    ToolSet() = default;

    explicit ToolSet(std::vector<std::unique_ptr<Tool>> tools) {
        if (std::any_of(tools.begin(), tools.end(),
                        [](const auto& tool) { return !tool; }))
            throw std::invalid_argument("ToolSet cannot own a null Tool");
        if (!tools.empty())
            unique_tools_ =
                std::make_shared<const std::vector<std::unique_ptr<Tool>>>(
                    std::move(tools));
    }

    explicit ToolSet(std::vector<std::shared_ptr<Tool>> tools) {
        if (std::any_of(tools.begin(), tools.end(),
                        [](const auto& tool) { return !tool; }))
            throw std::invalid_argument("ToolSet cannot own a null Tool");
        if (!tools.empty())
            shared_tools_ =
                std::make_shared<const std::vector<std::shared_ptr<Tool>>>(
                    std::move(tools));
    }

    std::vector<Tool*> view() const {
        if (selection_) return *selection_;
        std::vector<Tool*> tools;
        tools.reserve(size());
        if (unique_tools_)
            for (const auto& tool : *unique_tools_) tools.push_back(tool.get());
        if (shared_tools_)
            for (const auto& tool : *shared_tools_) tools.push_back(tool.get());
        return tools;
    }

    /// Retain ownership while exposing only a node's authorized subset.
    ToolSet select(std::vector<Tool*> tools) const {
        for (Tool* tool : tools) {
            const auto owns = [tool](const auto& owner) { return owner.get() == tool; };
            const bool found = selection_
                ? std::find(selection_->begin(), selection_->end(), tool) != selection_->end()
                : (unique_tools_ &&
                   std::any_of(unique_tools_->begin(), unique_tools_->end(), owns))
                      || (shared_tools_ &&
                          std::any_of(shared_tools_->begin(), shared_tools_->end(), owns));
            if (!found)
                throw std::invalid_argument("ToolSet selection contains an unowned Tool");
        }
        ToolSet result;
        if (!tools.empty()) {
            result.unique_tools_ = unique_tools_;
            result.shared_tools_ = shared_tools_;
            result.selection_ = std::make_shared<const std::vector<Tool*>>(std::move(tools));
        }
        return result;
    }

    std::size_t size() const noexcept {
        return selection_ ? selection_->size()
                          : (unique_tools_ ? unique_tools_->size() : 0)
                                + (shared_tools_ ? shared_tools_->size() : 0);
    }
    bool empty() const noexcept { return size() == 0; }

private:
    std::shared_ptr<const std::vector<std::unique_ptr<Tool>>> unique_tools_;
    std::shared_ptr<const std::vector<std::shared_ptr<Tool>>> shared_tools_;
    std::shared_ptr<const std::vector<Tool*>> selection_;
};

}  // namespace neograph
