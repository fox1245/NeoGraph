#include "run_context_runtime.h"

#include <mutex>
#include <unordered_map>
#include <utility>

namespace neograph::graph::detail {

namespace {

std::mutex runtime_mutex;
std::unordered_map<const RunContext*, std::shared_ptr<const RunContextRuntime>> runtimes;

}  // namespace

std::shared_ptr<const RunContextRuntime> runtime_for(const RunContext& context) {
    std::lock_guard<std::mutex> lock(runtime_mutex);
    const auto it = runtimes.find(&context);
    return it == runtimes.end() ? nullptr : it->second;
}

std::shared_ptr<const RunContextRuntime> runtime_for_invocation(
    const RunContext& context, const std::string& invocation_id) {
    auto parent = runtime_for(context);
    auto runtime = parent
        ? std::make_shared<RunContextRuntime>(*parent)
        : std::make_shared<RunContextRuntime>();
    runtime->invocation_id = invocation_id;
    return runtime;
}

ScopedRunContextRuntime::ScopedRunContextRuntime(
    const RunContext& context,
    std::shared_ptr<const RunContextRuntime> runtime)
    : context_(&context) {
    if (!runtime) return;

    std::lock_guard<std::mutex> lock(runtime_mutex);
    const auto it = runtimes.find(context_);
    if (it != runtimes.end()) previous_ = it->second;
    runtime_ = std::move(runtime);
    runtimes[context_] = runtime_;
    installed_ = true;
}

ScopedRunContextRuntime::~ScopedRunContextRuntime() {
    if (!installed_) return;

    std::lock_guard<std::mutex> lock(runtime_mutex);
    const auto current = runtimes.find(context_);
    if (current == runtimes.end() || current->second != runtime_) return;
    if (previous_) {
        runtimes[context_] = std::move(previous_);
    } else {
        runtimes.erase(context_);
    }
}

}  // namespace neograph::graph::detail
