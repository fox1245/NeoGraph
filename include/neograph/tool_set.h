#pragma once

#include <neograph/tool.h>

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace neograph {

/**
 * @brief Move-only owner for a fixed collection of tools.
 *
 * ToolSet closes the lifetime gap of NodeContext::tools: build an owned set,
 * pass it through EngineResources, and GraphEngine keeps the exact pointees
 * alive for every run and resume. The collection is immutable after
 * construction, while the Tool objects retain their normal mutable interface.
 */
class ToolSet {
public:
    ToolSet() = default;

    explicit ToolSet(std::vector<std::unique_ptr<Tool>> tools) : tools_(std::move(tools)) {}

    ToolSet(const ToolSet&)                = delete;
    ToolSet& operator=(const ToolSet&)     = delete;
    ToolSet(ToolSet&&) noexcept            = default;
    ToolSet& operator=(ToolSet&&) noexcept = default;

    std::vector<Tool*> view() const {
        std::vector<Tool*> tools;
        tools.reserve(tools_.size());
        for (const auto& tool : tools_)
            tools.push_back(tool.get());
        return tools;
    }

    std::size_t size() const noexcept { return tools_.size(); }
    bool        empty() const noexcept { return size() == 0; }

    std::vector<std::unique_ptr<Tool>> release() && { return std::move(tools_); }

private:
    std::vector<std::unique_ptr<Tool>> tools_;
};

}  // namespace neograph
