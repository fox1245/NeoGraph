#include "json_bridge.h"

#include <pybind11/pybind11.h>

#include <string>

namespace neograph::pybind {

namespace py = ::pybind11;

neograph::json py_to_json(py::handle obj) {
    // Order matters: bool is a subclass of int in Python, so the bool
    // check must come first, otherwise ``True`` round-trips as ``1``.
    if (obj.is_none()) {
        return neograph::json(nullptr);
    }
    if (py::isinstance<py::bool_>(obj)) {
        return neograph::json(obj.cast<bool>());
    }
    if (py::isinstance<py::int_>(obj)) {
        // Use long long so values up to 2^63-1 survive. Python ints
        // larger than that fall back to double via the float branch
        // below — the json wrapper has no bigint type.
        try {
            return neograph::json(obj.cast<long long>());
        } catch (const py::cast_error&) {
            return neograph::json(obj.cast<double>());
        }
    }
    if (py::isinstance<py::float_>(obj)) {
        return neograph::json(obj.cast<double>());
    }
    if (py::isinstance<py::str>(obj)) {
        return neograph::json(obj.cast<std::string>());
    }
    if (py::isinstance<py::dict>(obj)) {
        auto out = neograph::json::object();
        for (auto item : obj.cast<py::dict>()) {
            // Keys are always stringified — JSON has no other key type.
            // Non-string keys (e.g. int) match Python's json.dumps()
            // behaviour by going through ``str()``.
            std::string key = py::str(item.first).cast<std::string>();
            out[key] = py_to_json(item.second);
        }
        return out;
    }
    if (py::isinstance<py::list>(obj) || py::isinstance<py::tuple>(obj)) {
        auto out = neograph::json::array();
        for (auto item : obj) {
            out.push_back(py_to_json(item));
        }
        return out;
    }
    // Fallback: str() the object. This catches numpy scalars, Path
    // objects, datetime, etc. Lossy but deterministic.
    return neograph::json(py::str(obj).cast<std::string>());
}

py::object json_to_py(const neograph::json& j) {
    if (j.is_null()) return py::none();
    if (j.is_boolean()) return py::bool_(j.get<bool>());
    if (j.is_number_integer() || j.is_number_unsigned()) {
        return py::int_(j.get<long long>());
    }
    if (j.is_number_float()) return py::float_(j.get<double>());
    if (j.is_string()) return py::str(j.get<std::string>());
    if (j.is_array()) {
        py::list out;
        for (auto item : j) {
            out.append(json_to_py(item));
        }
        return std::move(out);
    }
    if (j.is_object()) {
        py::dict out;
        for (auto kv : j.items()) {
            out[py::str(kv.first)] = json_to_py(kv.second);
        }
        return std::move(out);
    }
    // Unreachable for well-formed json values, but better to surface
    // an empty None than to fall off a switch.
    return py::none();
}

} // namespace neograph::pybind
