#pragma once

#include <neograph/provider.h>
#include <pybind11/pybind11.h>
#include <optional>

namespace neograph::pybind {
struct ProviderOutcome {
    sp::runtime::Result value;
};

// The Python handle owns the real move-only admission. Taking it is one-shot;
// callers take while holding the GIL, before releasing it for dispatch.
struct PreparedHandle {
    std::optional<PreparedProviderRequest> value;
    explicit PreparedHandle(PreparedProviderRequest request) : value(std::move(request)) {}
    PreparedProviderRequest take() {
        if (!value) throw std::logic_error("Prepared provider request has already been consumed");
        auto request = std::move(*value);
        value.reset();
        return request;
    }
    const PreparedProviderRequest& get() const {
        if (!value) throw std::logic_error("Prepared provider request has already been consumed");
        return *value;
    }
};

// Copying this native shared_ptr also copies a lease on the Python override
// owner, so compiled NodeContext snapshots do not depend on mutable attributes.
std::shared_ptr<Provider> own_python_provider(pybind11::object provider);
std::function<void(const sp::Event&)> provider_observer(pybind11::object callback);
pybind11::object provider_observer_function(const std::function<void(const sp::Event&)>& observer);
pybind11::object provider_cause(const std::exception_ptr& cause);
void init_provider_values(pybind11::module_& module);
void init_provider_controls(pybind11::module_& module);
} // namespace neograph::pybind

// All binding units use the same ownership conversion for SDK outcomes. No
// dictionary or portable projection can enter an authority-bearing Result.
namespace pybind11::detail {
template <> struct type_caster<sp::runtime::Result> {
    PYBIND11_TYPE_CASTER(sp::runtime::Result, const_name("ProviderOutcome"));
    bool load(handle source, bool) {
        if (source.is_none()) { value.reset(); return true; }
        if (!isinstance<neograph::pybind::ProviderOutcome>(source)) return false;
        value = pybind11::cast<const neograph::pybind::ProviderOutcome&>(source).value;
        return true;
    }
    static handle cast(const sp::runtime::Result& source, return_value_policy, handle) {
        if (!source) return none().release();
        return pybind11::cast(neograph::pybind::ProviderOutcome{source}).release();
    }
};
} // namespace pybind11::detail
