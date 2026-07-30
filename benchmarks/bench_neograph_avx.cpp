// End-to-end NeoGraph benchmark for split versus fused float transforms.
//
// The measured work is clamp(x * scale + bias, low, high). Every variant is
// executed through GraphEngine::build() and SyncGraphNode; only the graph shape
// and fused kernel change.

#include <neograph/neograph.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#define NEOGRAPH_AVX_BENCH_X86 1
#include <immintrin.h>
#else
#define NEOGRAPH_AVX_BENCH_X86 0
#endif

#if NEOGRAPH_AVX_BENCH_X86 && (defined(__GNUC__) || defined(__clang__))
#define NEOGRAPH_AVX_BENCH_TARGET_AVAILABLE 1
#define NEOGRAPH_AVX_BENCH_TARGET_AVX2_FMA \
    __attribute__((target("avx2,fma"), noinline))
#else
#define NEOGRAPH_AVX_BENCH_TARGET_AVAILABLE 0
#define NEOGRAPH_AVX_BENCH_TARGET_AVX2_FMA
#endif

namespace {

using neograph::graph::ChannelWrite;
using neograph::graph::EngineConfig;
using neograph::graph::EngineResources;
using neograph::graph::GraphEngine;
using neograph::graph::GraphRegistry;
using neograph::graph::NodeContext;
using neograph::graph::NodeInput;
using neograph::graph::NodeOutput;
using neograph::graph::RunConfig;
using neograph::graph::RunResult;
using neograph::graph::RunStatus;
using neograph::graph::SyncGraphNode;
using neograph::json;

struct Parameters {
    float scale = 1.001953125F;
    float bias  = -0.125F;
    float low   = -3.5F;
    float high  = 4.25F;
};

// Benchmark-only sidecar ownership: the topology JSON deliberately carries no
// float arrays. Local GraphRegistry factories and the nodes they create share
// these vectors, keeping their lifetime tied to this benchmark's engine without
// adding JSON serialization/copying to the timed path.
struct FloatSidecar {
    explicit FloatSidecar(const std::vector<float>& source, bool needs_split_buffers)
        : input(source), output(source.size()) {
        if (needs_split_buffers) {
            scaled.resize(source.size());
            biased.resize(source.size());
        }
    }

    std::vector<float> input;
    std::vector<float> scaled;
    std::vector<float> biased;
    std::vector<float> output;
    Parameters         parameters;
};

using Kernel = void (*)(const float*, float*, std::size_t, const Parameters&);

inline float clamp_value(float value, float low, float high) noexcept {
    return std::min(std::max(value, low), high);
}

void fused_portable_kernel(const float* input, float* output, std::size_t count,
                           const Parameters& parameters) {
    for (std::size_t i = 0; i < count; ++i) {
        output[i] = clamp_value(input[i] * parameters.scale + parameters.bias,
                                parameters.low, parameters.high);
    }
}

#if NEOGRAPH_AVX_BENCH_TARGET_AVAILABLE
// This intentionally uses the same ordinary C++ loop as fused_portable_kernel.
// Only the function-local target changes, isolating ISA and auto-vectorization.
NEOGRAPH_AVX_BENCH_TARGET_AVX2_FMA
void fused_target_auto_kernel(const float* input, float* output, std::size_t count,
                              const Parameters& parameters) {
    for (std::size_t i = 0; i < count; ++i) {
        output[i] = clamp_value(input[i] * parameters.scale + parameters.bias,
                                parameters.low, parameters.high);
    }
}

NEOGRAPH_AVX_BENCH_TARGET_AVX2_FMA
void fused_avx2_fma_kernel(const float* input, float* output, std::size_t count,
                           const Parameters& parameters) {
    const __m256 scale = _mm256_set1_ps(parameters.scale);
    const __m256 bias  = _mm256_set1_ps(parameters.bias);
    const __m256 low   = _mm256_set1_ps(parameters.low);
    const __m256 high  = _mm256_set1_ps(parameters.high);
    std::size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m256 x       = _mm256_loadu_ps(input + i);
        const __m256 affine  = _mm256_fmadd_ps(x, scale, bias);
        const __m256 clamped = _mm256_min_ps(_mm256_max_ps(affine, low), high);
        _mm256_storeu_ps(output + i, clamped);
    }
    // Scalar tail handles item counts that are not divisible by eight.
    for (; i < count; ++i) {
        output[i] = clamp_value(input[i] * parameters.scale + parameters.bias,
                                parameters.low, parameters.high);
    }
}
#endif

class SplitNode final : public SyncGraphNode {
public:
    enum class Stage { Scale, Bias, Clamp };
    SplitNode(std::string name, std::shared_ptr<FloatSidecar> sidecar, Stage stage)
        : SyncGraphNode(std::move(name)), sidecar_(std::move(sidecar)), stage_(stage) {}
protected:
    NodeOutput run_sync(NodeInput) override {
        const auto& p = sidecar_->parameters;
        const auto n = sidecar_->input.size();
        switch (stage_) {
        case Stage::Scale:
            for (std::size_t i = 0; i < n; ++i) sidecar_->scaled[i] = sidecar_->input[i] * p.scale;
            return {};
        case Stage::Bias:
            for (std::size_t i = 0; i < n; ++i) sidecar_->biased[i] = sidecar_->scaled[i] + p.bias;
            return {};
        case Stage::Clamp:
            for (std::size_t i = 0; i < n; ++i)
                sidecar_->output[i] = clamp_value(sidecar_->biased[i], p.low, p.high);
            return NodeOutput{{ChannelWrite{"done", json(true)}}};
        }
        throw std::logic_error("unknown split stage");
    }
private:
    std::shared_ptr<FloatSidecar> sidecar_;
    Stage stage_;
};

class FusedNode final : public SyncGraphNode {
public:
    FusedNode(std::string name, std::shared_ptr<FloatSidecar> sidecar, Kernel kernel)
        : SyncGraphNode(std::move(name)), sidecar_(std::move(sidecar)), kernel_(kernel) {}
protected:
    NodeOutput run_sync(NodeInput) override {
        kernel_(sidecar_->input.data(), sidecar_->output.data(), sidecar_->input.size(),
                sidecar_->parameters);
        return NodeOutput{{ChannelWrite{"done", json(true)}}};
    }
private:
    std::shared_ptr<FloatSidecar> sidecar_;
    Kernel kernel_;
};

class NoopNode final : public SyncGraphNode {
public:
    NoopNode(std::string name, bool writes_done)
        : SyncGraphNode(std::move(name)), writes_done_(writes_done) {}
protected:
    NodeOutput run_sync(NodeInput) override {
        return writes_done_ ? NodeOutput{{ChannelWrite{"done", json(true)}}} : NodeOutput{};
    }
private:
    bool writes_done_;
};

json split_topology(std::string_view name, std::string_view type) {
    return {{"name", std::string(name)},
            {"channels", {{"done", {{"reducer", "overwrite"}}}}},
            {"nodes", {{"scale", {{"type", std::string(type)}, {"stage", "scale"}}},
                       {"bias", {{"type", std::string(type)}, {"stage", "bias"}}},
                       {"clamp", {{"type", std::string(type)}, {"stage", "clamp"}}}}},
            {"edges", json::array({{{"from", "__start__"}, {"to", "scale"}},
                                   {{"from", "scale"}, {"to", "bias"}},
                                   {{"from", "bias"}, {"to", "clamp"}},
                                   {{"from", "clamp"}, {"to", "__end__"}}})}};
}

json fused_topology(std::string_view name, std::string_view type) {
    return {{"name", std::string(name)},
            {"channels", {{"done", {{"reducer", "overwrite"}}}}},
            {"nodes", {{"fused", {{"type", std::string(type)}}}}},
            {"edges", json::array({{{"from", "__start__"}, {"to", "fused"}},
                                   {{"from", "fused"}, {"to", "__end__"}}})}};
}

struct BuiltGraph {
    std::unique_ptr<GraphEngine> engine;
    std::shared_ptr<FloatSidecar> sidecar;
};

BuiltGraph build_split_graph(const std::vector<float>& input) {
    auto sidecar = std::make_shared<FloatSidecar>(input, true);
    auto registry = std::make_shared<GraphRegistry>();
    registry->register_type("bench_split",
        [sidecar](const std::string& name, const json& cfg, const NodeContext&) {
            const auto stage = cfg.value("stage", std::string{});
            if (stage == "scale") return std::unique_ptr<neograph::graph::GraphNode>(
                std::make_unique<SplitNode>(name, sidecar, SplitNode::Stage::Scale));
            if (stage == "bias") return std::unique_ptr<neograph::graph::GraphNode>(
                std::make_unique<SplitNode>(name, sidecar, SplitNode::Stage::Bias));
            if (stage == "clamp") return std::unique_ptr<neograph::graph::GraphNode>(
                std::make_unique<SplitNode>(name, sidecar, SplitNode::Stage::Clamp));
            throw std::invalid_argument("unknown split stage: " + stage);
        });
    EngineResources resources;
    resources.registry = registry;
    auto engine = GraphEngine::build(split_topology("split_graph", "bench_split"),
                                     EngineConfig{}, std::move(resources));
    return {std::move(engine), std::move(sidecar)};
}

BuiltGraph build_fused_graph(const std::vector<float>& input, std::string_view name,
                             Kernel kernel) {
    auto sidecar = std::make_shared<FloatSidecar>(input, false);
    auto registry = std::make_shared<GraphRegistry>();
    registry->register_type("bench_fused",
        [sidecar, kernel](const std::string& node_name, const json&, const NodeContext&) {
            return std::make_unique<FusedNode>(node_name, sidecar, kernel);
        });
    EngineResources resources;
    resources.registry = registry;
    auto engine = GraphEngine::build(fused_topology(name, "bench_fused"), EngineConfig{},
                                     std::move(resources));
    return {std::move(engine), std::move(sidecar)};
}

BuiltGraph build_noop_graph(bool split_shape) {
    auto registry = std::make_shared<GraphRegistry>();
    registry->register_type("bench_noop",
        [](const std::string& name, const json& cfg, const NodeContext&) {
            return std::make_unique<NoopNode>(name,
                cfg.value("stage", std::string{}) == "clamp" || name == "fused");
        });
    EngineResources resources;
    resources.registry = registry;
    const auto topology = split_shape ? split_topology("split_noop", "bench_noop")
                                      : fused_topology("fused_noop", "bench_noop");
    auto engine = GraphEngine::build(topology, EngineConfig{}, std::move(resources));
    return {std::move(engine), nullptr};
}

struct CpuFeatures { bool query = false; bool avx2 = false; bool fma = false; };
CpuFeatures detect_cpu_features() noexcept {
#if NEOGRAPH_AVX_BENCH_TARGET_AVAILABLE
    __builtin_cpu_init();
    return {true, __builtin_cpu_supports("avx2") != 0,
            __builtin_cpu_supports("fma") != 0};
#else
    return {};
#endif
}

struct ArgumentError : std::invalid_argument { using std::invalid_argument::invalid_argument; };
struct Arguments { std::size_t items = 1U << 20U; std::size_t repetitions = 5; std::size_t samples = 9; };

std::size_t positive(std::string_view text, std::string_view name) {
    std::size_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size() || value == 0)
        throw ArgumentError(std::string(name) + " must be a positive integer");
    return value;
}
Arguments parse_arguments(int argc, char** argv) {
    if (argc > 4) throw ArgumentError("usage: bench_neograph_avx [items] [repetitions] [samples]");
    Arguments a;
    if (argc > 1) a.items = positive(argv[1], "items");
    if (argc > 2) a.repetitions = positive(argv[2], "repetitions");
    if (argc > 3) a.samples = positive(argv[3], "samples");
    return a;
}

std::vector<float> make_input(std::size_t count) {
    std::vector<float> values(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto bits = (static_cast<std::uint64_t>(i) * 48271ULL + 17ULL) % 200003ULL;
        values[i] = static_cast<float>(static_cast<std::int64_t>(bits) - 100001LL) / 10000.0F;
    }
    return values;
}

void require_done(GraphEngine& engine, std::string_view name) {
    const RunResult result = engine.run(RunConfig{});
    if (result.status() != RunStatus::Completed || !result.has_channel("done")
        || !result.channel<bool>("done"))
        throw std::runtime_error(std::string(name) + " did not complete with done=true");
}
double measure_ms(GraphEngine& engine, std::string_view name, std::size_t repetitions) {
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < repetitions; ++i) require_done(engine, name);
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

struct ErrorMetrics { bool finite = true; bool within = true; double max_abs = 0; double max_rel = 0; };
ErrorMetrics compare(const std::vector<float>& reference, const std::vector<float>& candidate,
                     double abs_tolerance, double rel_tolerance) {
    if (reference.size() != candidate.size()) return {false, false, INFINITY, INFINITY};
    ErrorMetrics m;
    for (std::size_t i = 0; i < reference.size(); ++i) {
        const double expected = reference[i], actual = candidate[i];
        if (!std::isfinite(expected) || !std::isfinite(actual)) { m.finite = m.within = false; continue; }
        const double absolute = std::abs(actual - expected);
        const double relative = absolute / std::max(std::abs(expected), 1.0e-12);
        m.max_abs = std::max(m.max_abs, absolute); m.max_rel = std::max(m.max_rel, relative);
        if (absolute > abs_tolerance + rel_tolerance * std::abs(expected)) m.within = false;
    }
    return m;
}

struct Sample { std::size_t sample; std::size_t order; bool compute_first; double gross; double noop; double corrected; };
struct Statistics { double median; double mean; double stddev; double minimum; double maximum; };
Statistics summarize(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto n = values.size();
    const double median = n % 2 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2;
    const double mean = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(n);
    double sum = 0;
    for (double value : values) { const double d = value - mean; sum += d * d; }
    return {median, mean, std::sqrt(sum / static_cast<double>(n)), values.front(), values.back()};
}
struct ModeStatistics { Statistics gross; Statistics noop; Statistics corrected; };
struct Mode {
    std::string name; bool available; std::string reason;
    std::unique_ptr<GraphEngine> engine; std::shared_ptr<FloatSidecar> sidecar;
    GraphEngine* noop_engine; std::string noop_name; std::vector<Sample> samples;
};
ModeStatistics summarize_mode(const Mode& mode) {
    std::vector<double> gross, noop, corrected;
    for (const auto& s : mode.samples) { gross.push_back(s.gross); noop.push_back(s.noop); corrected.push_back(s.corrected); }
    return {summarize(std::move(gross)), summarize(std::move(noop)), summarize(std::move(corrected))};
}
void print_statistics(std::string_view mode, std::string_view metric, const Statistics& s) {
    std::cout << "summary\t" << mode << '\t' << metric << '\t' << s.median << '\t'
              << s.mean << '\t' << s.stddev << '\t' << s.minimum << '\t' << s.maximum << '\n';
}
void print_ratio(std::string_view label, const Mode& from_mode, const Mode& to_mode,
                 const std::map<std::string, ModeStatistics>& all) {
    if (!from_mode.available || !to_mode.available) {
        std::cout << "ratio\t" << label << "\tunavailable\tNA\tNA\n"; return;
    }
    const auto& from = all.at(from_mode.name); const auto& to = all.at(to_mode.name);
    const double gross = from.gross.median / to.gross.median;
    if (from.corrected.median <= 0 || to.corrected.median <= 0) {
        std::cout << "ratio\t" << label << "\tnonpositive_corrected_time\t" << gross << "\tNA\n"; return;
    }
    std::cout << "ratio\t" << label << "\tok\t" << gross << '\t'
              << from.corrected.median / to.corrected.median << '\n';
}

int run_benchmark(const Arguments& a) {
    constexpr std::size_t warmup_runs = 2;
    constexpr double abs_tolerance = 1e-6, rel_tolerance = 1e-5;
    const CpuFeatures cpu = detect_cpu_features();
    const bool avx_available = NEOGRAPH_AVX_BENCH_TARGET_AVAILABLE && cpu.avx2 && cpu.fma;
    const std::string avx_reason = !NEOGRAPH_AVX_BENCH_TARGET_AVAILABLE ? "compiler_target_path_unavailable"
        : (!cpu.avx2 ? "cpu_lacks_avx2" : (!cpu.fma ? "cpu_lacks_fma" : "available"));
    const auto input = make_input(a.items);
    BuiltGraph split = build_split_graph(input);
    BuiltGraph portable = build_fused_graph(input, "fused_portable", fused_portable_kernel);
    BuiltGraph split_noop = build_noop_graph(true), fused_noop = build_noop_graph(false);
    std::vector<Mode> modes;
    modes.push_back({"split_graph", true, "available", std::move(split.engine), std::move(split.sidecar), split_noop.engine.get(), "split_noop", {}});
    modes.push_back({"fused_portable", true, "available", std::move(portable.engine), std::move(portable.sidecar), fused_noop.engine.get(), "fused_noop", {}});
#if NEOGRAPH_AVX_BENCH_TARGET_AVAILABLE
    if (avx_available) {
        auto target = build_fused_graph(input, "fused_target_auto", fused_target_auto_kernel);
        auto explicit_avx = build_fused_graph(input, "fused_avx2_fma", fused_avx2_fma_kernel);
        modes.push_back({"fused_target_auto", true, "available", std::move(target.engine), std::move(target.sidecar), fused_noop.engine.get(), "fused_noop", {}});
        modes.push_back({"fused_avx2_fma", true, "available", std::move(explicit_avx.engine), std::move(explicit_avx.sidecar), fused_noop.engine.get(), "fused_noop", {}});
    } else
#endif
    {
        modes.push_back({"fused_target_auto", false, avx_reason, nullptr, nullptr, fused_noop.engine.get(), "fused_noop", {}});
        modes.push_back({"fused_avx2_fma", false, avx_reason, nullptr, nullptr, fused_noop.engine.get(), "fused_noop", {}});
    }

    std::cout << std::setprecision(12)
              << "meta\tformat_version\t1\nmeta\titems\t" << a.items
              << "\nmeta\trepetitions\t" << a.repetitions << "\nmeta\tsamples\t" << a.samples
              << "\nmeta\twarmup_runs\t" << warmup_runs
              << "\nmeta\tformula\tclamp(x*scale+bias,low,high)"
              << "\nmeta\tinput_transport\tbenchmark_sidecar_no_json_arrays"
              << "\nmeta\tarchitecture\t" << (NEOGRAPH_AVX_BENCH_X86 ? "x86" : "non_x86")
              << "\nmeta\tcompiler_path\t" << (NEOGRAPH_AVX_BENCH_TARGET_AVAILABLE ? "gcc_or_clang_function_target_avx2_fma" : "portable_only")
              << "\nmeta\tcpu_query_supported\t" << cpu.query << "\nmeta\tcpu_avx2\t" << cpu.avx2
              << "\nmeta\tcpu_fma\t" << cpu.fma << '\n';
    std::cout << "schema\tmode\tname\tavailable\treason\n";
    for (const auto& mode : modes) std::cout << "mode\t" << mode.name << '\t' << mode.available << '\t' << mode.reason << '\n';

    for (auto& mode : modes) if (mode.available) require_done(*mode.engine, mode.name);
    const auto& reference = modes[1].sidecar->output;
    bool accuracy_ok = true;
    std::cout << "schema\taccuracy\tmode\tavailable\tfinite\tmax_abs_error\tmax_rel_error\tabs_tolerance\trel_tolerance\tpass\n";
    for (const auto& mode : modes) {
        if (!mode.available) { std::cout << "accuracy\t" << mode.name << "\t0\tNA\tNA\tNA\t" << abs_tolerance << '\t' << rel_tolerance << "\tNA\n"; continue; }
        const auto m = compare(reference, mode.sidecar->output, abs_tolerance, rel_tolerance);
        const bool pass = m.finite && m.within; accuracy_ok &= pass;
        std::cout << "accuracy\t" << mode.name << "\t1\t" << m.finite << '\t' << m.max_abs << '\t' << m.max_rel << '\t' << abs_tolerance << '\t' << rel_tolerance << '\t' << pass << '\n';
    }
    if (!accuracy_ok) { std::cerr << "error\taccuracy\toutput_tolerance_exceeded\n"; return 2; }

    for (auto& mode : modes) if (mode.available)
        for (std::size_t i = 0; i < warmup_runs; ++i) { require_done(*mode.engine, mode.name); require_done(*mode.noop_engine, mode.noop_name); }
    std::vector<std::size_t> available;
    for (std::size_t i = 0; i < modes.size(); ++i) if (modes[i].available) available.push_back(i);
    std::cout << "schema\tsample\tmode\tsample_index\torder_index\tpair_order\tgross_ms\tnoop_ms\tcorrected_ms\n";
    for (std::size_t sample = 0; sample < a.samples; ++sample) {
        for (std::size_t order = 0; order < available.size(); ++order) {
            const std::size_t mode_index = available[(sample + order) % available.size()];
            Mode& mode = modes[mode_index];
            const bool compute_first = (sample + mode_index) % 2 == 0;
            double gross, noop;
            if (compute_first) { gross = measure_ms(*mode.engine, mode.name, a.repetitions); noop = measure_ms(*mode.noop_engine, mode.noop_name, a.repetitions); }
            else { noop = measure_ms(*mode.noop_engine, mode.noop_name, a.repetitions); gross = measure_ms(*mode.engine, mode.name, a.repetitions); }
            mode.samples.push_back({sample, order, compute_first, gross, noop, gross - noop});
            std::cout << "sample\t" << mode.name << '\t' << sample << '\t' << order << '\t' << (compute_first ? "compute_first" : "noop_first") << '\t' << gross << '\t' << noop << '\t' << gross - noop << '\n';
        }
    }
    std::map<std::string, ModeStatistics> statistics;
    std::cout << "schema\tsummary\tmode\tmetric\tmedian_ms\tmean_ms\tstddev_ms\tmin_ms\tmax_ms\n";
    for (const auto& mode : modes) if (mode.available) {
        auto s = summarize_mode(mode); statistics.emplace(mode.name, s);
        print_statistics(mode.name, "gross", s.gross); print_statistics(mode.name, "noop", s.noop); print_statistics(mode.name, "corrected", s.corrected);
    }
    std::cout << "schema\tratio\tcomparison\tstatus\tgross_speedup\tcorrected_speedup\n";
    print_ratio("split_graph_to_fused_portable", modes[0], modes[1], statistics);
    print_ratio("fused_portable_to_fused_target_auto", modes[1], modes[2], statistics);
    print_ratio("fused_target_auto_to_fused_avx2_fma", modes[2], modes[3], statistics);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try { return run_benchmark(parse_arguments(argc, argv)); }
    catch (const ArgumentError& e) { std::cerr << "error\targument\t" << e.what() << '\n'; return 1; }
    catch (const std::exception& e) { std::cerr << "error\truntime\t" << e.what() << '\n'; return 3; }
}
