// Capabilities that existed in C++ and were simply unreachable from Python — the
// Store subsystem and the graph validator (issue #97).
//
// Not here, deliberately: RetryPolicy. The issue listed it as a gap and the
// issue was wrong. A graph definition already carries `"retry_policy": {...}`
// and the engine already honours it from Python; binding a RetryPolicy *class*
// would give one concept two homes. test_retry_policy_already_works_from_the_definition
// pins the capability instead, so nobody re-files it.

#include "json_bridge.h"

#include <neograph/graph/compiler.h>
#include <neograph/graph/node.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/store.h>
#include <neograph/graph/validator.h>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <memory>
#include <string>
#include <vector>

namespace py = pybind11;

namespace neograph::pybind {

namespace {

class PyStore : public neograph::graph::Store {
public:
    using Store::Store;

    void put(const neograph::graph::Namespace& ns, const std::string& key,
             const neograph::json& value) override {
        PYBIND11_OVERRIDE_PURE(void, Store, put, ns, key, json_to_py(value));
    }

    std::optional<neograph::graph::StoreItem>
    get(const neograph::graph::Namespace& ns, const std::string& key) const override {
        PYBIND11_OVERRIDE_PURE(std::optional<neograph::graph::StoreItem>,
                               Store, get, ns, key);
    }

    std::vector<neograph::graph::StoreItem>
    search(const neograph::graph::Namespace& ns_prefix, int limit) const override {
        PYBIND11_OVERRIDE_PURE(std::vector<neograph::graph::StoreItem>,
                               Store, search, ns_prefix, limit);
    }

    void delete_item(const neograph::graph::Namespace& ns,
                     const std::string& key) override {
        PYBIND11_OVERRIDE_PURE(void, Store, delete_item, ns, key);
    }

    std::vector<neograph::graph::Namespace>
    list_namespaces(const neograph::graph::Namespace& prefix) const override {
        PYBIND11_OVERRIDE_PURE(std::vector<neograph::graph::Namespace>,
                               Store, list_namespaces, prefix);
    }
};

}  // namespace

void init_parity(py::module_& m) {
    using namespace neograph::graph;

    // ── Store ────────────────────────────────────────────────────────────
    //
    // A checkpoint remembers one conversation. The Store remembers the user
    // across all of them — which is the thing anyone building an agent reaches
    // for on about day two, and which had no symbol at all in Python.

    py::class_<StoreItem>(m, "StoreItem",
        "One item in the Store: its namespace, its key, its value, and when it "
        "was written.")
        .def(py::init([]() { return StoreItem{}; }))
        .def_readwrite("ns",  &StoreItem::ns,
            "Hierarchical namespace, e.g. [\"users\", \"u1\", \"prefs\"].")
        .def_readwrite("key", &StoreItem::key)
        .def_property("value",
            [](const StoreItem& i) { return json_to_py(i.value); },
            [](StoreItem& i, py::object value) { i.value = py_to_json(value); })
        .def_readwrite("created_at", &StoreItem::created_at,
            "Unix epoch milliseconds.")
        .def_readwrite("updated_at", &StoreItem::updated_at,
            "Unix epoch milliseconds.")
        .def("__repr__", [](const StoreItem& i) {
            std::string ns;
            for (const auto& part : i.ns) { ns += "/" + part; }
            return "<StoreItem " + ns + "/" + i.key + ">";
        });

    py::class_<Store, PyStore, std::shared_ptr<Store>>(m, "Store",
        "Cross-thread long-term memory, namespaced key/value.\n\n"
        "Where a checkpoint remembers one conversation, a Store remembers the "
        "user across all of them. Install one with ``engine.set_store(store)``; "
        "node bodies then reach it through ``input.ctx.store``.\n\n"
        "Abstract — use InMemoryStore, or subclass this to back it with a "
        "database.")
        .def(py::init<>())
        .def("put",
            [](Store& self, const Namespace& ns, const std::string& key,
               py::object value) { self.put(ns, key, py_to_json(value)); },
            py::arg("ns"), py::arg("key"), py::arg("value"))
        .def("get", &Store::get, py::arg("ns"), py::arg("key"))
        .def("search", &Store::search,
            py::arg("ns_prefix"), py::arg("limit") = 100)
        .def("delete_item", &Store::delete_item,
            py::arg("ns"), py::arg("key"))
        .def("list_namespaces", &Store::list_namespaces,
            py::arg("prefix") = Namespace{});

    py::class_<InMemoryStore, Store, std::shared_ptr<InMemoryStore>>(
        m, "InMemoryStore",
        "Reference Store: everything in a process-lifetime map, thread-safe.")
        .def(py::init<>())
        .def("put",
            [](InMemoryStore& self, const Namespace& ns,
               const std::string& key, py::object value) {
                self.put(ns, key, py_to_json(value));
            },
            py::arg("ns"), py::arg("key"), py::arg("value"),
            "Write a value. Overwrites whatever was there.")
        .def("get",
            [](const InMemoryStore& self, const Namespace& ns,
               const std::string& key) -> py::object {
                auto item = self.get(ns, key);
                if (!item) return py::none();     // absent, not an exception
                return py::cast(*item);
            },
            py::arg("ns"), py::arg("key"),
            "Read a value. Returns None when there is nothing there — absence "
            "is an answer, not an error.")
        .def("search", &InMemoryStore::search,
            py::arg("ns_prefix"), py::arg("limit") = 100,
            "Every item whose namespace starts with this prefix. "
            "``search([\"users\"])`` finds everything under every user.")
        .def("delete_item", &InMemoryStore::delete_item,
            py::arg("ns"), py::arg("key"))
        .def("list_namespaces", &InMemoryStore::list_namespaces,
            py::arg("prefix") = Namespace{},
            "Namespaces that currently hold at least one item.")
        .def("__len__", &InMemoryStore::size);

    // ── GraphValidator ───────────────────────────────────────────────────
    //
    // compile() throws on a broken graph. That tells you it broke; it does not
    // hand you the list. The validator does, before anything runs.

    py::class_<Diagnostic>(m, "Diagnostic",
        "One thing the validator found.")
        .def_readonly("code",     &Diagnostic::code,
            "Catalog code, E3..E11 — see docs/dsl §5.")
        .def_readonly("severity", &Diagnostic::severity, "\"error\" | \"warning\"")
        .def_readonly("path",     &Diagnostic::path,
            "Where in the definition, JSON-path-ish.")
        .def_readonly("message",  &Diagnostic::message,
            "Human-readable and self-contained.")
        .def_property_readonly("witness",
            [](const Diagnostic& d) { return json_to_py(d.witness); },
            "A machine-readable counterexample — the thing that is actually "
            "wrong, not a description of it.")
        .def("__repr__", [](const Diagnostic& d) {
            return "<" + d.severity + " " + d.code + " " + d.path + ": "
                 + d.message + ">";
        });

    py::class_<ValidationReport>(m, "ValidationReport",
        "What the validator found. Errors will make compile() throw; warnings "
        "will not, and are worth reading anyway.")
        .def("has_errors", &ValidationReport::has_errors)
        .def_readonly("diagnostics", &ValidationReport::diagnostics)
        .def("errors",
            [](const ValidationReport& r) {
                std::vector<Diagnostic> out;
                for (const auto* d : r.errors()) out.push_back(*d);
                return out;
            },
            "The diagnostics that are errors.")
        .def("warnings",
            [](const ValidationReport& r) {
                std::vector<Diagnostic> out;
                for (const auto* d : r.warnings()) out.push_back(*d);
                return out;
            })
        .def("summary", &ValidationReport::summary,
            "Every diagnostic, as one block of text.")
        .def("__bool__", [](const ValidationReport& r) { return !r.has_errors(); },
            "True when the graph is clean, so ``if not validate(defn): ...`` "
            "reads the way you would say it.");

    m.def("validate",
        [](py::object definition) {
            NodeContext ctx;   // the checks are static: no provider, no tools
            auto cg = GraphCompiler::compile(py_to_json(definition), ctx);
            return GraphValidator::validate(cg);
        },
        py::arg("definition"),
        "Check a graph definition and hand back the report — dangling edges, "
        "unreachable nodes, dead barriers — instead of finding out when "
        "compile() throws.\n\n"
        "    report = ng.validate(definition)\n"
        "    if report.has_errors():\n"
        "        print(report.summary())\n");
}

}  // namespace neograph::pybind
