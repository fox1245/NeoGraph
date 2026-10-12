#include "json_bridge.h"

#include <pybind11/pybind11.h>

#include <string>

namespace neograph::pybind {

namespace py = ::pybind11;

std::string py_to_utf8(py::handle text) {
    py::object value = py::reinterpret_borrow<py::object>(text);
    if (!py::isinstance<py::str>(value)) value = py::str(value);
    Py_ssize_t size = 0;
    if (const char* data = PyUnicode_AsUTF8AndSize(value.ptr(), &size))
        return std::string(data, static_cast<std::size_t>(size));
    // Lone surrogates cannot be encoded strictly. A surrogateescape'd byte
    // (U+DC80..U+DCFF, as os.fsdecode and errors="surrogateescape" produce)
    // turns back into the byte it stands for; any other lone surrogate is
    // U+FFFD. The bytes then follow the one UTF-8 rule: each maximal
    // ill-formed subpart becomes U+FFFD (CPython's "replace" decoder
    // substitutes per maximal subpart, as text_safety.h does).
    PyErr_Clear();
    const auto kind = PyUnicode_KIND(value.ptr());
    const void* data = PyUnicode_DATA(value.ptr());
    const auto length = PyUnicode_GET_LENGTH(value.ptr());
    std::string bytes;
    bytes.reserve(static_cast<std::size_t>(length) * 2);
    for (Py_ssize_t i = 0; i < length; ++i) {
        const Py_UCS4 cp = PyUnicode_READ(kind, data, i);
        if (cp >= 0xDC80 && cp <= 0xDCFF) {
            bytes.push_back(static_cast<char>(cp - 0xDC00));
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            bytes.append("\xEF\xBF\xBD");
        } else if (cp < 0x80) {
            bytes.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            bytes.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            bytes.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            bytes.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            bytes.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            bytes.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            bytes.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            bytes.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            bytes.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            bytes.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    auto repaired = py::reinterpret_steal<py::object>(PyUnicode_DecodeUTF8(
        bytes.data(), static_cast<Py_ssize_t>(bytes.size()), "replace"));
    if (!repaired) throw py::error_already_set();
    const char* out = PyUnicode_AsUTF8AndSize(repaired.ptr(), &size);
    if (!out) throw py::error_already_set();
    return std::string(out, static_cast<std::size_t>(size));
}

py::str utf8_to_py(const std::string& text) {
    auto value = py::reinterpret_steal<py::object>(
        PyUnicode_DecodeUTF8(text.data(), static_cast<Py_ssize_t>(text.size()), "replace"));
    if (!value) throw py::error_already_set();
    return py::reinterpret_steal<py::str>(value.release());
}

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
        return neograph::json(py_to_utf8(obj));
    }
    if (py::isinstance<py::dict>(obj)) {
        auto out = neograph::json::object();
        for (auto item : obj.cast<py::dict>()) {
            // Keys are always stringified — JSON has no other key type.
            // Non-string keys (e.g. int) match Python's json.dumps()
            // behaviour by going through ``str()``.
            std::string key = py_to_utf8(item.first);
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
    return neograph::json(py_to_utf8(obj));
}

py::object json_to_py(const neograph::json& j) {
    if (j.is_null()) return py::none();
    if (j.is_boolean()) return py::bool_(j.get<bool>());
    if (j.is_number_integer() || j.is_number_unsigned()) {
        return py::int_(j.get<long long>());
    }
    if (j.is_number_float()) return py::float_(j.get<double>());
    if (j.is_string()) return utf8_to_py(j.get<std::string>());
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
            out[utf8_to_py(kv.first)] = json_to_py(kv.second);
        }
        return std::move(out);
    }
    // Unreachable for well-formed json values, but better to surface
    // an empty None than to fall off a switch.
    return py::none();
}

} // namespace neograph::pybind
