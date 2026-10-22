#include <neograph/program/store_call.h>

#include <neograph/graph/checkpoint.h>

#include <cstddef>
#include <memory>
#include <vector>

namespace neograph::program::detail {

namespace {
thread_local unsigned inline_store_call_depth = 0;
}

InlineStoreCalls::InlineStoreCalls() noexcept { ++inline_store_call_depth; }
InlineStoreCalls::~InlineStoreCalls() { --inline_store_call_depth; }
bool InlineStoreCalls::active() noexcept { return inline_store_call_depth != 0; }
bool offload_store_call(bool offloads) noexcept { return offloads && inline_store_call_depth == 0; }

namespace {
// The contexts this thread has finished with. A few are enough: waits nest only as deep as the synchronous calls do.
constexpr std::size_t kIdleContexts = 4;
std::vector<std::unique_ptr<asio::io_context>>& idle_contexts() {
    thread_local std::vector<std::unique_ptr<asio::io_context>> idle;
    return idle;
}
}  // namespace

BlockingIoContext::BlockingIoContext() {
    auto& idle = idle_contexts();
    if (!idle.empty()) {
        io_ = std::move(idle.back());
        idle.pop_back();
    } else {
        io_ = std::make_unique<asio::io_context>(1);
    }
}

BlockingIoContext::~BlockingIoContext() {
    auto& idle = idle_contexts();
    if (reusable_ && io_ && idle.size() < kIdleContexts) {
        io_->restart();
        idle.push_back(std::move(io_));
    }
}

asio::awaitable<void> offload_to_blocking_pool(std::function<void()> work) {
    co_await graph::detail::run_on_blocking_pool(std::move(work));
}

}  // namespace neograph::program::detail
