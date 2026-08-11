// Transport-envelope benchmark only. It preserves every nested Program record
// as canonical JSON; neither codec becomes a persistent identity format.
#define NEOGRAPH_BENCH_BINARY_POC_NO_MAIN
#include "bench_program_binary_poc.cpp"
#undef NEOGRAPH_BENCH_BINARY_POC_NO_MAIN

#include "benchmarks/program_publication_poc.capnp.h"
#include "benchmarks/program_publication_poc.pb.h"

#include <capnp/common.h>
#include <capnp/message.h>
#include <capnp/serialize.h>
#include <google/protobuf/stubs/common.h>

#include <chrono>
#include <iomanip>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using ProtoEnvelope = neograph::bench::ProgramPublicationEnvelope;
using CapnpEnvelope = neograph::bench::capnp_poc::ProgramPublicationEnvelope;
using CapnpWords    = kj::Array<capnp::word>;

struct PublicationParts {
    std::string               run_record;
    std::string               journal_record;
    std::vector<std::string>  events;
    std::vector<std::string>  effects;
    std::optional<std::string> migration_plan;
};

int checked_protobuf_size(std::size_t value) {
    if (value > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::invalid_argument("protobuf wire payload exceeds parser limit");
    return static_cast<int>(value);
}

capnp::Data::Reader data_reader(std::string_view source) {
    return {reinterpret_cast<const capnp::byte*>(source.data()), source.size()};
}

std::string copy_data(capnp::Data::Reader source) {
    return {reinterpret_cast<const char*>(source.begin()), source.size()};
}

PublicationParts canonical_parts(const neograph::program::ProgramTransitionPublication& publication) {
    PublicationParts out;
    out.run_record     = publication.run_record.serialize_canonical();
    out.journal_record = publication.journal_record.serialize_canonical();
    out.events.reserve(publication.events.size());
    out.effects.reserve(publication.effects.size());
    for (const auto& event : publication.events)
        out.events.push_back(event.serialize_canonical());
    for (const auto& effect : publication.effects)
        out.effects.push_back(effect.serialize_canonical());
    if (publication.migration_plan)
        out.migration_plan = publication.migration_plan->serialize_canonical();
    return out;
}

neograph::program::ProgramTransitionPublication parse_parts(const PublicationParts& parts) {
    using namespace neograph::program;

    std::vector<ProgramEvent> events;
    events.reserve(parts.events.size());
    for (const auto& event : parts.events)
        events.push_back(ProgramEvent::parse(event));

    std::vector<ProgramEffectOutboxEntry> effects;
    effects.reserve(parts.effects.size());
    for (const auto& effect : parts.effects)
        effects.push_back(ProgramEffectOutboxEntry::parse(effect));

    std::optional<MigrationPlan> migration_plan;
    if (parts.migration_plan)
        migration_plan = MigrationPlan::parse(*parts.migration_plan);

    return {ProgramRunRecord::parse(parts.run_record),
            ProgramJournalRecord::parse(parts.journal_record), std::move(events),
            std::move(effects), std::move(migration_plan)};
}

std::string encode_protobuf(const PublicationParts& parts) {
    ProtoEnvelope envelope;
    envelope.set_run_record(parts.run_record);
    envelope.set_journal_record(parts.journal_record);
    for (const auto& event : parts.events)
        envelope.add_events(event);
    for (const auto& effect : parts.effects)
        envelope.add_effects(effect);
    if (parts.migration_plan)
        envelope.set_migration_plan(*parts.migration_plan);

    std::string wire;
    if (!envelope.SerializeToString(&wire))
        throw std::runtime_error("protobuf envelope serialization failed");
    return wire;
}

PublicationParts decode_protobuf(std::string_view wire) {
    ProtoEnvelope envelope;
    if (!envelope.ParseFromArray(wire.data(), checked_protobuf_size(wire.size())))
        throw std::invalid_argument("protobuf envelope parsing failed");

    PublicationParts out;
    out.run_record     = envelope.run_record();
    out.journal_record = envelope.journal_record();
    out.events.reserve(static_cast<std::size_t>(envelope.events_size()));
    out.effects.reserve(static_cast<std::size_t>(envelope.effects_size()));
    for (const auto& event : envelope.events())
        out.events.push_back(event);
    for (const auto& effect : envelope.effects())
        out.effects.push_back(effect);
    if (envelope.has_migration_plan())
        out.migration_plan = envelope.migration_plan();
    return out;
}

CapnpWords encode_capnp(const PublicationParts& parts) {
    capnp::MallocMessageBuilder message;
    auto root = message.initRoot<CapnpEnvelope>();
    root.setRunRecord(data_reader(parts.run_record));
    root.setJournalRecord(data_reader(parts.journal_record));

    std::vector<capnp::Data::Reader> events;
    events.reserve(parts.events.size());
    for (const auto& event : parts.events)
        events.push_back(data_reader(event));
    root.setEvents({events.data(), events.size()});

    std::vector<capnp::Data::Reader> effects;
    effects.reserve(parts.effects.size());
    for (const auto& effect : parts.effects)
        effects.push_back(data_reader(effect));
    root.setEffects({effects.data(), effects.size()});

    root.setHasMigrationPlan(parts.migration_plan.has_value());
    if (parts.migration_plan)
        root.setMigrationPlan(data_reader(*parts.migration_plan));
    return capnp::messageToFlatArray(message);
}

PublicationParts decode_capnp(kj::ArrayPtr<const capnp::word> wire) {
    capnp::FlatArrayMessageReader reader(wire);
    const auto root = reader.getRoot<CapnpEnvelope>();

    PublicationParts out;
    out.run_record     = copy_data(root.getRunRecord());
    out.journal_record = copy_data(root.getJournalRecord());
    const auto events  = root.getEvents();
    const auto effects = root.getEffects();
    out.events.reserve(events.size());
    out.effects.reserve(effects.size());
    for (const auto& event : events)
        out.events.push_back(copy_data(event));
    for (const auto& effect : effects)
        out.effects.push_back(copy_data(effect));
    if (root.getHasMigrationPlan()) {
        if (!root.hasMigrationPlan())
            throw std::invalid_argument("Cap'n Proto migration flag lacks its payload");
        out.migration_plan = copy_data(root.getMigrationPlan());
    }
    return out;
}


template <typename Values, typename Encoder, typename WireSize, typename WireFingerprint>
double measure_encode(const Values& values, int repetitions, Encoder&& encode,
                      WireSize&& wire_size, WireFingerprint&& wire_fingerprint,
                      std::size_t& total_bytes) {
    total_bytes = 0;
    const auto started = Clock::now();
    for (int repetition = 0; repetition < repetitions; ++repetition) {
        for (const auto& value : values) {
            auto wire = encode(value);
            const auto size = wire_size(wire);
            total_bytes += size;
            blackhole += wire_fingerprint(wire);
        }
    }
    return std::chrono::duration<double, std::micro>(Clock::now() - started).count();
}
std::size_t fingerprint_parts(const PublicationParts& parts) noexcept {
    std::size_t fingerprint = byte_fingerprint(parts.run_record.data(), parts.run_record.size()) ^
                              byte_fingerprint(parts.journal_record.data(),
                                               parts.journal_record.size());
    for (const auto& event : parts.events)
        fingerprint ^= byte_fingerprint(event.data(), event.size());
    for (const auto& effect : parts.effects)
        fingerprint ^= byte_fingerprint(effect.data(), effect.size());
    if (parts.migration_plan)
        fingerprint ^= byte_fingerprint(parts.migration_plan->data(),
                                        parts.migration_plan->size());
    return fingerprint;
}

double measure_canonical_parts(
    const std::vector<neograph::program::ProgramTransitionPublication>& publications,
    int repetitions) {
    const auto started = Clock::now();
    for (int repetition = 0; repetition < repetitions; ++repetition) {
        for (const auto& publication : publications) {
            const auto parts = canonical_parts(publication);
            blackhole += fingerprint_parts(parts);
        }
    }
    return std::chrono::duration<double, std::micro>(Clock::now() - started).count();
}

template <typename Wire, typename Decoder>
double measure_decode(const std::vector<Wire>& wire, int repetitions, Decoder&& decode) {
    const auto started = Clock::now();
    for (int repetition = 0; repetition < repetitions; ++repetition) {
        for (const auto& publication_wire : wire) {
            auto publication = decode(publication_wire);
            blackhole += publication.run_record.run_id().size();
            blackhole += publication.events.size();
            blackhole += publication.effects.size();
        }
    }
    return std::chrono::duration<double, std::micro>(Clock::now() - started).count();
}

void require_round_trip(const neograph::program::ProgramTransitionPublication& expected,
                        const PublicationParts& decoded, std::string_view codec) {
    if (parse_parts(decoded).serialize_canonical() != expected.serialize_canonical())
        throw std::runtime_error(std::string(codec) + " envelope changed canonical publication");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const int capture_runs = argc > 1 ? std::stoi(argv[1]) : 25;
        const int repetitions  = argc > 2 ? std::stoi(argv[2]) : 100;
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

        for (int index = 0; index < 5; ++index) {
            const auto run_id = "codec-poc-warmup-" + std::to_string(index);
            auto result = runtime.run(
                "bench-program", version,
                ProgramInvocation{json::object(), budget, run_id, {}, run_id});
            if (result.status() != ProgramTerminalStatus::Completed)
                throw std::runtime_error("warmup failed");
        }
        capture->clear();
        for (int index = 0; index < capture_runs; ++index) {
            const auto run_id = "codec-poc-" + std::to_string(index);
            auto result = runtime.run(
                "bench-program", version,
                ProgramInvocation{json::object(), budget, run_id, {}, run_id});
            if (result.status() != ProgramTerminalStatus::Completed)
                throw std::runtime_error("capture run failed");
        }

        const auto publications = capture->snapshot();
        if (publications.empty())
            throw std::runtime_error("capture produced no Program transition publications");

        std::vector<PublicationParts> canonical;
        std::vector<std::string> current_wire;
        std::vector<std::string> protobuf_wire;
        std::vector<CapnpWords> capnp_wire;
        canonical.reserve(publications.size());
        current_wire.reserve(publications.size());
        protobuf_wire.reserve(publications.size());
        capnp_wire.reserve(publications.size());
        for (const auto& publication : publications) {
            canonical.push_back(canonical_parts(publication));
            const auto& parts = canonical.back();
            current_wire.push_back(publication.serialize_canonical());
            protobuf_wire.push_back(encode_protobuf(parts));
            capnp_wire.push_back(encode_capnp(parts));
            require_round_trip(publication, decode_protobuf(protobuf_wire.back()), "protobuf");
            require_round_trip(publication, decode_capnp(capnp_wire.back().asPtr()), "Cap'n Proto");
        }

        std::size_t current_encode_bytes = 0;
        const auto current_encode_us = measure_encode(
            publications, repetitions,
            [](const auto& publication) { return publication.serialize_canonical(); },
            [](const std::string& wire) { return wire.size(); },
            [](const std::string& wire) {
                return byte_fingerprint(wire.data(), wire.size());
            },
            current_encode_bytes);
        const auto canonical_parts_us = measure_canonical_parts(publications, repetitions);

        std::size_t protobuf_encode_bytes = 0;
        const auto protobuf_envelope_encode_us = measure_encode(
            canonical, repetitions,
            [](const PublicationParts& parts) { return encode_protobuf(parts); },
            [](const std::string& wire) { return wire.size(); },
            [](const std::string& wire) {
                return byte_fingerprint(wire.data(), wire.size());
            },
            protobuf_encode_bytes);
        std::size_t protobuf_total_ignored_bytes = 0;
        const auto protobuf_total_encode_us = measure_encode(
            publications, repetitions,
            [](const auto& publication) { return encode_protobuf(canonical_parts(publication)); },
            [](const std::string& wire) { return wire.size(); },
            [](const std::string& wire) {
                return byte_fingerprint(wire.data(), wire.size());
            },
            protobuf_total_ignored_bytes);

        std::size_t capnp_encode_bytes = 0;
        const auto capnp_envelope_encode_us = measure_encode(
            canonical, repetitions,
            [](const PublicationParts& parts) { return encode_capnp(parts); },
            [](const CapnpWords& wire) { return wire.asBytes().size(); },
            [](const CapnpWords& wire) {
                const auto bytes = wire.asBytes();
                return byte_fingerprint(bytes.begin(), bytes.size());
            },
            capnp_encode_bytes);
        std::size_t capnp_total_ignored_bytes = 0;
        const auto capnp_total_encode_us = measure_encode(
            publications, repetitions,
            [](const auto& publication) { return encode_capnp(canonical_parts(publication)); },
            [](const CapnpWords& wire) { return wire.asBytes().size(); },
            [](const CapnpWords& wire) {
                const auto bytes = wire.asBytes();
                return byte_fingerprint(bytes.begin(), bytes.size());
            },
            capnp_total_ignored_bytes);

        const auto current_decode_us = measure_decode(
            current_wire, repetitions,
            [](const std::string& wire) { return ProgramTransitionPublication::parse(wire); });
        const auto protobuf_decode_us = measure_decode(
            protobuf_wire, repetitions,
            [](const std::string& wire) { return parse_parts(decode_protobuf(wire)); });
        const auto capnp_decode_us = measure_decode(
            capnp_wire, repetitions,
            [](const CapnpWords& wire) { return parse_parts(decode_capnp(wire.asPtr())); });

        const double operations = static_cast<double>(publications.size()) * repetitions;
        std::cout << std::fixed << std::setprecision(3)
                  << "config\tcapture_runs\t" << capture_runs << '\n'
                  << "config\trepetitions\t" << repetitions << '\n'
                  << "runtime\tprotobuf_header_version\t" << GOOGLE_PROTOBUF_VERSION << '\n'
                  << "runtime\tcapnp_header_version\t" << CAPNP_VERSION_MAJOR << '.'
                  << CAPNP_VERSION_MINOR << '.' << CAPNP_VERSION_MICRO << '\n'
                  << "config\ttransport_total_skips_outer_publication_validation\ttrue\n"
                  << "result\tcaptured_publications\t" << publications.size() << '\n'
                  << "result\tcurrent_canonical_encode_per_publication_us\t"
                  << current_encode_us / operations << '\n'
                  << "result\tnested_canonical_parts_per_publication_us\t"
                  << canonical_parts_us / operations << '\n'
                  << "result\tprotobuf_envelope_only_encode_per_publication_us\t"
                  << protobuf_envelope_encode_us / operations << '\n'
                  << "result\tcapnp_flat_envelope_only_encode_per_publication_us\t"
                  << capnp_envelope_encode_us / operations << '\n'
                  << "result\tprotobuf_transport_total_lower_bound_encode_per_publication_us\t"
                  << protobuf_total_encode_us / operations << '\n'
                  << "result\tcapnp_flat_transport_total_lower_bound_encode_per_publication_us\t"
                  << capnp_total_encode_us / operations << '\n'
                  << "result\tcurrent_canonical_decode_per_publication_us\t"
                  << current_decode_us / operations << '\n'
                  << "result\tprotobuf_transport_full_recovery_per_publication_us\t"
                  << protobuf_decode_us / operations << '\n'
                  << "result\tcapnp_flat_transport_full_recovery_per_publication_us\t"
                  << capnp_decode_us / operations << '\n'
                  << "result\tprotobuf_transport_total_lower_bound_speedup_vs_current\t"
                  << current_encode_us / protobuf_total_encode_us << '\n'
                  << "result\tcapnp_transport_total_lower_bound_speedup_vs_current\t"
                  << current_encode_us / capnp_total_encode_us << '\n'
                  << "result\tcurrent_canonical_bytes_per_publication\t"
                  << static_cast<double>(current_encode_bytes) / operations << '\n'
                  << "result\tprotobuf_envelope_bytes_per_publication\t"
                  << static_cast<double>(protobuf_encode_bytes) / operations << '\n'
                  << "result\tcapnp_flat_envelope_bytes_per_publication\t"
                  << static_cast<double>(capnp_encode_bytes) / operations << '\n'
                  << "result\tblackhole\t" << blackhole << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}
