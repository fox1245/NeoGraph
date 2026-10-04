// Adapt Python OpenTelemetry objects to the real native prepared-dispatch
// tracer. Native OpenInference code owns projection/privacy/usage decisions.
#include <neograph/observability/openinference.h>
#include <pybind11/pybind11.h>

#include <memory>
#include <string>
#include <utility>

namespace py = pybind11;
namespace neograph::pybind {
namespace {

template <typename T>
std::shared_ptr<T> own_python(T value) {
    return std::shared_ptr<T>(new T(std::move(value)), [](T* object) {
        py::gil_scoped_acquire gil;
        delete object;
    });
}

struct OTelObjects {
    py::object tracer;
    py::object status_type;
    py::object status_ok;
    py::object status_error;
};

class PythonOTelSpan final : public observability::Span {
public:
    PythonOTelSpan(py::object span, std::shared_ptr<OTelObjects> objects)
        : span_(own_python(std::move(span))), objects_(std::move(objects)) {}

    void set_attribute(std::string_view key, std::string_view value) override {
        py::gil_scoped_acquire gil;
        span_->attr("set_attribute")(std::string(key), std::string(value));
    }
    void set_attribute(std::string_view key, std::int64_t value) override {
        py::gil_scoped_acquire gil;
        span_->attr("set_attribute")(std::string(key), value);
    }
    void set_attribute(std::string_view key, double value) override {
        py::gil_scoped_acquire gil;
        span_->attr("set_attribute")(std::string(key), value);
    }
    void set_attribute_bool(std::string_view key, bool value) override {
        py::gil_scoped_acquire gil;
        span_->attr("set_attribute")(std::string(key), value);
    }
    void add_event(std::string_view name, std::string_view payload) override {
        py::gil_scoped_acquire gil;
        py::dict attributes;
        attributes["chunk"] = std::string(payload);
        span_->attr("add_event")(std::string(name), std::move(attributes));
    }
    void set_status_ok() override {
        py::gil_scoped_acquire gil;
        span_->attr("set_status")(objects_->status_type(objects_->status_ok));
    }
    void set_status_error(std::string_view message) override {
        py::gil_scoped_acquire gil;
        span_->attr("set_status")(objects_->status_type(objects_->status_error, std::string(message)));
    }
    void end() override {
        py::gil_scoped_acquire gil;
        span_->attr("end")();
    }

private:
    std::shared_ptr<py::object> span_;
    std::shared_ptr<OTelObjects> objects_;
};

class PythonOTelTracer final : public observability::Tracer {
public:
    explicit PythonOTelTracer(py::object tracer) {
        // Construction runs under the GIL. Missing optional OpenTelemetry is
        // an honest import failure, not a tracing provider that silently vanishes.
        auto api = py::module_::import("opentelemetry.trace");
        objects_ = own_python(OTelObjects{std::move(tracer), api.attr("Status"),
            api.attr("StatusCode").attr("OK"), api.attr("StatusCode").attr("ERROR")});
    }
    std::unique_ptr<observability::Span> start_span(std::string_view name,
                                                  observability::Span*) override {
        py::gil_scoped_acquire gil;
        // The Python OTel tracer captures its active context. The constructor
        // below supplies no native parent span and never imports raw span pointers.
        return std::make_unique<PythonOTelSpan>(
            objects_->tracer.attr("start_span")(std::string(name)), objects_);
    }

private:
    std::shared_ptr<OTelObjects> objects_;
};

} // namespace

void init_openinference(py::module_& m) {
    py::class_<observability::OpenInferenceProvider, Provider,
               std::shared_ptr<observability::OpenInferenceProvider>>(m, "OpenInferenceProvider",
        "Native prepared-dispatch OpenInference wrapper using a Python OpenTelemetry tracer.")
        .def(py::init([](std::shared_ptr<Provider> inner, py::object tracer, std::string name) {
            auto adapter = std::make_shared<PythonOTelTracer>(std::move(tracer));
            // Native preparation copies parent_lookup into the operation's
            // before-dispatch hook alongside its borrowed Tracer*. Capturing
            // the adapter there keeps the tracer alive even after this wrapper
            // is collected and the owned prepared handle is dispatched later.
            std::function<observability::Span*()> retained = [adapter] {
                return static_cast<observability::Span*>(nullptr);
            };
            return std::make_shared<observability::OpenInferenceProvider>(
                std::move(inner), *adapter, std::move(retained), std::move(name));
        }), py::arg("inner"), py::arg("tracer"), py::arg("span_name") = "llm.complete",
            py::keep_alive<1, 2>());
}

} // namespace neograph::pybind
