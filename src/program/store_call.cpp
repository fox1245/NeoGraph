#include <neograph/program/store_call.h>

#include <neograph/graph/checkpoint.h>

namespace neograph::program::detail {

asio::awaitable<void> offload_to_blocking_pool(std::function<void()> work) {
    co_await graph::detail::run_on_blocking_pool(std::move(work));
}

}  // namespace neograph::program::detail
