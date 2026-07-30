#include <neograph/program/program.h>

#include <cstdlib>
#include <iostream>

int main() {
    auto source = neograph::program::ProgramSource::from_canonical_json(
        "installed-consumer", R"({"program_schema_version":1,"steps":[]})");
    const auto round_trip = neograph::program::ProgramSource::parse(source.serialize_canonical());

    std::cout << round_trip.source_hash() << "\n";
    return round_trip.source_hash() == source.source_hash() ? EXIT_SUCCESS : EXIT_FAILURE;
}
