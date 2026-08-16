#define main bench_program_original_main
#include "bench_program.cpp"
#undef main

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace neograph;
using namespace neograph::graph;
using namespace neograph::program;
using Clock = std::chrono::steady_clock;
volatile std::size_t blackhole = 0;

class CaptureTransitionStore final : public ProgramTransitionStore {
public:
    explicit CaptureTransitionStore(std::shared_ptr<ProgramTransitionStore> inner)
        : inner_(std::move(inner)) {}
    std::optional<ProgramRunRecord> load(std::string_view owner, std::string_view run) const override {
        return inner_->load(owner, run);
    }
    std::optional<ProgramJournalRecord> latest(std::string_view owner, std::string_view run) const override {
        return inner_->latest(owner, run);
    }
    std::vector<ProgramEvent> load_events(std::string_view owner, std::string_view run,
                                          std::uint64_t after) const override {
        return inner_->load_events(owner, run, after);
    }
    std::vector<ProgramEffectOutboxEntry> load_effects(std::string_view owner, std::string_view run,
                                                       std::uint64_t after) const override {
        return inner_->load_effects(owner, run, after);
    }
    std::vector<ProgramJavaScriptCommandJournalEntry> load_javascript_commands(
        std::string_view owner, std::string_view run, std::uint64_t after) const override {
        return inner_->load_javascript_commands(owner, run, after);
    }
    std::optional<MigrationPlan> load_migration_plan(std::string_view owner,
                                                     std::string_view run) const override {
        return inner_->load_migration_plan(owner, run);
    }
    std::optional<ProgramRunLineage> load_lineage(std::string_view owner,
                                                    std::string_view lineage_id) const override {
        return inner_->load_lineage(owner, lineage_id);
    }
    std::optional<ProgramRunLineage> load_run_lineage(std::string_view owner,
                                                       std::string_view run_id) const override {
        return inner_->load_run_lineage(owner, run_id);
    }
    std::optional<ProgramRunLineage> load_lineage_head(
        std::string_view owner, std::string_view lineage_id,
        std::string_view head_id) const override {
        return inner_->load_lineage_head(owner, lineage_id, head_id);
    }
    std::optional<ProgramRunGeneration> load_generation(
        std::string_view owner, std::string_view lineage_id,
        std::uint64_t generation) const override {
        return inner_->load_generation(owner, lineage_id, generation);
    }
    ProgramTransitionPublishResult compare_publish(
        std::string_view owner, std::string_view expected,
        ProgramTransitionPublication publication) override {
        {
            std::lock_guard lock(mutex_);
            publications_.push_back(publication);
        }
        return inner_->compare_publish(owner, expected, std::move(publication));
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
std::size_t byte_fingerprint(const void* input, std::size_t size) noexcept {
    if (size == 0) return 0;
    const auto* bytes = static_cast<const unsigned char*>(input);
    return size ^ static_cast<std::size_t>(bytes[0]) ^
           (static_cast<std::size_t>(bytes[size / 2]) << 8U) ^
           (static_cast<std::size_t>(bytes[size - 1]) << 16U);
}


void append_u32(std::string& out, std::size_t value) {
    const auto encoded = static_cast<std::uint32_t>(value);
    for (unsigned shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<char>((encoded >> shift) & 0xffU));
}
void append_blob(std::string& out, std::string_view value) {
    append_u32(out, value.size());
    out.append(value);
}

std::string encode_binary_envelope(const ProgramTransitionPublication& publication) {
    // Lower-bound comparison only: preserve each component's canonical bytes,
    // but replace the JSON parse/sort/re-emit envelope with length-prefixed bytes.
    const auto run = publication.run_record.serialize_canonical();
    const auto journal = publication.journal_record.serialize_canonical();
    std::vector<std::string> events;
    events.reserve(publication.events.size());
    for (const auto& event : publication.events)
        events.push_back(event.serialize_canonical());
    std::vector<std::string> effects;
    effects.reserve(publication.effects.size());
    for (const auto& effect : publication.effects)
        effects.push_back(effect.serialize_canonical());

    std::string out;
    out.reserve(run.size() + journal.size());
    out.append("NGB1", 4);
    append_blob(out, run);
    append_blob(out, journal);
    append_u32(out, events.size());
    for (const auto& event : events)
        append_blob(out, event);
    append_u32(out, effects.size());
    for (const auto& effect : effects)
        append_blob(out, effect);
    append_u32(out, publication.migration_plan ? 1 : 0);
    if (publication.migration_plan)
        append_blob(out, publication.migration_plan->serialize_canonical());
    return out;
}

template <typename Encoder>
double measure(const std::vector<ProgramTransitionPublication>& publications,
              int repetitions, Encoder&& encoder, std::size_t& total_bytes) {
    total_bytes = 0;
    const auto started = Clock::now();
    for (int repetition = 0; repetition < repetitions; ++repetition) {
        for (const auto& publication : publications) {
            auto bytes = encoder(publication);
            total_bytes += bytes.size();
            blackhole += byte_fingerprint(bytes.data(), bytes.size());
        }
    }
    return std::chrono::duration<double, std::micro>(Clock::now() - started).count();
}

}  // namespace

#ifndef NEOGRAPH_BENCH_BINARY_POC_NO_MAIN
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
        const auto source = ProgramSource::from_cpp_builder("bench:program", 1, program_document());
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
        ProgramRuntime runtime(RuntimeConfig{catalog, checkpoints, {}, capture, 1});
        const RunBudget budget{10000, 0, 0, 1, 1, 20, 0, 0, 0};

        for (int i = 0; i < 5; ++i) {
            const auto run_id = "binary-poc-warmup-" + std::to_string(i);
            auto result = runtime.run(
                "bench-program", version,
                ProgramInvocation{json::object(), budget, run_id, {}, run_id});
            if (result.status() != ProgramTerminalStatus::Completed)
                throw std::runtime_error("warmup failed");
        }
        capture->clear();
        for (int i = 0; i < capture_runs; ++i) {
            const auto run_id = "binary-poc-" + std::to_string(i);
            auto result = runtime.run(
                "bench-program", version,
                ProgramInvocation{json::object(), budget, run_id, {}, run_id});
            if (result.status() != ProgramTerminalStatus::Completed)
                throw std::runtime_error("capture run failed");
        }
        const auto publications = capture->snapshot();
        std::size_t current_bytes = 0;
        const double current_us = measure(
            publications, repetitions,
            [](const auto& publication) { return publication.serialize_canonical(); },
            current_bytes);
        std::size_t binary_bytes = 0;
        const double binary_us = measure(
            publications, repetitions,
            [](const auto& publication) { return encode_binary_envelope(publication); },
            binary_bytes);
        const double operations = static_cast<double>(publications.size()) * repetitions;

        std::cout << std::fixed << std::setprecision(3)
                  << "config\tcapture_runs\t" << capture_runs << '\n'
                  << "config\trepetitions\t" << repetitions << '\n'
                  << "result\tcaptured_publications\t" << publications.size() << '\n'
                  << "result\tcurrent_serializer_per_publication_us\t"
                  << current_us / operations << '\n'
                  << "result\tbinary_envelope_per_publication_us\t"
                  << binary_us / operations << '\n'
                  << "result\tbinary_speedup_vs_current\t" << current_us / binary_us << '\n'
                  << "result\tcurrent_bytes_per_publication\t"
                  << static_cast<double>(current_bytes) / operations << '\n'
                  << "result\tbinary_bytes_per_publication\t"
                  << static_cast<double>(binary_bytes) / operations << '\n'
                  << "result\tblackhole\t" << blackhole << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}
#endif
