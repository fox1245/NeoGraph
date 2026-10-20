// Members of SDK structs as Python properties. A member is read and written as the type it has, a JSON member as
// plain Python data, and a result's member as a detached, read-only copy.
#pragma once

#include "json_bridge.h"
#include "provider_bridge.h"
#include <pybind11/stl.h>

namespace neograph::pybind {

inline py::object document_view(const std::shared_ptr<const sp::json::Document>& value) {
    if (!value) return py::none();
    return py::module_::import("json").attr("loads")(value->root().dump());
}
inline std::shared_ptr<const sp::json::Document> document_from(py::object value) {
    if (value.is_none()) return {};
    auto parsed = sp::json::parse(py_to_json(value).dump());
    auto* document = std::get_if<sp::json::Document>(&parsed);
    if (!document) throw py::value_error("Invalid or oversized SDK JSON data");
    return std::make_shared<const sp::json::Document>(std::move(*document));
}
// A JSON member of an SDK struct, read and written as plain Python data.
template <class T>
void json_member(py::class_<T>& cls, const char* name, std::shared_ptr<const sp::json::Document> T::*member) {
    cls.def_property(name, [member](const T& v) { return document_view(v.*member); },
                     [member](T& v, py::object x) { v.*member = document_from(std::move(x)); });
}
template <class T, class M> void member(py::class_<T>& cls, const char* name, M T::*field) {
    cls.def_property(name, [field](const T& v) { return v.*field; },
                     [field](T& v, M x) { v.*field = std::move(x); });
}
// A member of a result: every read returns a copy, so changing it cannot change the result.
template <class T, class M> void readonly(py::class_<T>& cls, const char* name, M T::*field) {
    cls.def_property_readonly(name, [field](const T& v) { return v.*field; });
}
template <class T>
void json_readonly(py::class_<T>& cls, const char* name, std::shared_ptr<const sp::json::Document> T::*member) {
    cls.def_property_readonly(name, [member](const T& v) { return document_view(v.*member); });
}

}  // namespace neograph::pybind
