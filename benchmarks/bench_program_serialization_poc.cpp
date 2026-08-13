#define main bench_program_original_main
#include "bench_program.cpp"
#undef main

#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace neograph;
using namespace neograph::graph;
using namespace neograph::program;
using Clock = std::chrono::steady_clock;

volatile std::size_t blackhole = 0;
std::size_t byte_fingerprint(const void* input, std::size_t size) noexcept {
    if (size == 0) return 0;
    const auto* bytes = static_cast<const unsigned char*>(input);
    return size ^ static_cast<std::size_t>(bytes[0]) ^
           (static_cast<std::size_t>(bytes[size / 2]) << 8U) ^
           (static_cast<std::size_t>(bytes[size - 1]) << 16U);
}


class CaptureTransitionStore final : public ProgramTransitionStore {
public:
    explicit CaptureTransitionStore(std::shared_ptr<ProgramTransitionStore> inner)
        : inner_(std::move(inner)) {}

    std::optional<ProgramRunRecord> load(std::string_view owner_scope,
                                         std::string_view run_id) const override {
        return inner_->load(owner_scope, run_id);
    }
    std::optional<ProgramJournalRecord> latest(std::string_view owner_scope,
                                               std::string_view run_id) const override {
        return inner_->latest(owner_scope, run_id);
    }
    std::vector<ProgramEvent> load_events(std::string_view owner_scope,
                                          std::string_view run_id,
                                          std::uint64_t after_sequence) const override {
        return inner_->load_events(owner_scope, run_id, after_sequence);
    }
    std::vector<ProgramEffectOutboxEntry> load_effects(std::string_view owner_scope,
                                                       std::string_view run_id,
                                                       std::uint64_t after_sequence) const override {
        return inner_->load_effects(owner_scope, run_id, after_sequence);
    }
    std::vector<ProgramJavaScriptCommandJournalEntry> load_javascript_commands(
        std::string_view owner_scope,
        std::string_view run_id,
        std::uint64_t    after_sequence) const override {
        return inner_->load_javascript_commands(owner_scope, run_id, after_sequence);
    }
    std::optional<MigrationPlan> load_migration_plan(std::string_view owner_scope,
                                                     std::string_view run_id) const override {
        return inner_->load_migration_plan(owner_scope, run_id);
    }

    ProgramTransitionPublishResult compare_publish(
        std::string_view owner_scope,
        std::string_view expected_journal_head,
        ProgramTransitionPublication publication) override {
        {
            std::lock_guard lock(mutex_);
            publications_.push_back(publication);
        }
        return inner_->compare_publish(
            owner_scope, expected_journal_head, std::move(publication));
    }

    std::vector<ProgramTransitionPublication> snapshot() const {
        std::lock_guard lock(mutex_);
        return publications_;
    }
    void clear() {
        std::lock_guard lock(mutex_);
        publications_.clear();
    }


private:
    std::shared_ptr<ProgramTransitionStore> inner_;
    mutable std::mutex mutex_;
    std::vector<ProgramTransitionPublication> publications_;
};

double run_current_serializer(const std::vector<ProgramTransitionPublication>& publications,
                              int repetitions,
                              std::size_t& total_bytes) {
    total_bytes = 0;
    const auto started = Clock::now();
    for (int repetition = 0; repetition < repetitions; ++repetition) {
        for (const auto& publication : publications) {
            auto bytes = publication.serialize_canonical();
            total_bytes += bytes.size();
            blackhole += byte_fingerprint(bytes.data(), bytes.size());
        }
    }
    return std::chrono::duration<double, std::micro>(Clock::now() - started).count();
}

double run_cached_lookup(const std::vector<std::string>& cached,
                         int repetitions,
                         std::size_t& total_bytes) {
    total_bytes = 0;
    const auto started = Clock::now();
    for (int repetition = 0; repetition < repetitions; ++repetition) {
        for (const auto& bytes : cached) {
            std::string_view view(bytes);
            total_bytes += view.size();
            blackhole += byte_fingerprint(view.data(), view.size());
        }
    }
    return std::chrono::duration<double, std::micro>(Clock::now() - started).count();
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const int capture_runs = argc > 1 ? std::stoi(argv[1]) : 25;
        const int repetitions = argc > 2 ? std::stoi(argv[2]) : 100;
        if (capture_runs <= 0 || repetitions <= 0)
            throw std::invalid_argument("arguments must be positive");

        NodeFactory::instance().register_type(
            "bench-program-inc", [](const std::string& name, const json&, const NodeContext&) {
                return std::make_unique<IncrementNode>(name);
            });
        auto core = GraphEngine::compile(core_definition(), NodeContext{});
        const auto registry = registry_snapshot();
        const auto profile = admission_profile(registry);
        const auto policy = policy_snapshot(profile);
        ProgramCompiler compiler(registry, {"bench-program/v1"});
        const auto source = ProgramSource::from_cpp_builder(
            "bench:program", 1, program_document());
        const auto bundle = compiler.compile(source);
        auto store = std::make_shared<InMemoryProgramStore>();
        auto catalog = std::make_shared<ProgramCatalog>(
            CatalogConfig{store, registry, std::make_shared<EngineGenerationCache>(),
                          "bench-program/v1"});
        const auto version = catalog->admit(
            bundle, ProgramAdmission{"bench-program", profile, policy, {}});
        auto checkpoints = std::make_shared<InMemoryCheckpointStore>();
        auto inner = std::make_shared<InMemoryProgramTransitionStore>();
        auto capture = std::make_shared<CaptureTransitionStore>(inner);
        ProgramRuntime runtime(
            RuntimeConfig{catalog, checkpoints, {}, capture, 1});
        const RunBudget budget{10000, 0, 0, 1, 1, 20, 0, 0, 0};

        for (int i = 0; i < 5; ++i) {
            auto result = runtime.run(
                "bench-program", version,
                ProgramInvocation{json::object(), budget, "serialization-poc-warmup", {}});
            if (result.status() != ProgramTerminalStatus::Completed)
                throw std::runtime_error("warmup failed");
        }
        capture->clear();
        const auto capture_started = Clock::now();
        for (int i = 0; i < capture_runs; ++i) {
            auto result = runtime.run(
                "bench-program", version,
                ProgramInvocation{json::object(), budget, "serialization-poc", {}});
            if (result.status() != ProgramTerminalStatus::Completed)
                throw std::runtime_error("capture run failed");
        }
        const auto capture_us = std::chrono::duration<double, std::micro>(
            Clock::now() - capture_started).count();

        const auto publications = capture->snapshot();
        std::vector<std::string> cached;
        cached.reserve(publications.size());
        std::size_t cached_bytes = 0;
        for (const auto& publication : publications) {
            cached.push_back(publication.serialize_canonical());
            cached_bytes += cached.back().size();
        }

        std::size_t serialized_bytes = 0;
        const double current_us = run_current_serializer(
            publications, repetitions, serialized_bytes);
        std::size_t lookup_bytes = 0;
        const double cached_us = run_cached_lookup(cached, repetitions, lookup_bytes);
        const double total_ops = static_cast<double>(publications.size()) * repetitions;

        std::cout << std::fixed << std::setprecision(3)
                  << "config\tcapture_runs\t" << capture_runs << '\n'
                  << "config\trepetitions\t" << repetitions << '\n'
                  << "result\tcaptured_publications\t" << publications.size() << '\n'
                  << "result\tcapture_runtime_per_run_us\t"
                  << capture_us / capture_runs << '\n'
                  << "result\tcurrent_serializer_total_us\t" << current_us << '\n'
                  << "result\tcurrent_serializer_per_publication_us\t"
                  << current_us / total_ops << '\n'
                  << "result\tcached_lookup_total_us\t" << cached_us << '\n'
                  << "result\tcached_lookup_per_publication_us\t"
                  << cached_us / total_ops << '\n'
                  << "result\tcurrent_bytes_per_publication\t"
                  << static_cast<double>(serialized_bytes) / total_ops << '\n'
                  << "result\tcached_bytes_per_publication\t"
                  << static_cast<double>(cached_bytes) / publications.size() << '\n'
                  << "result\tblackhole\t" << blackhole << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}
