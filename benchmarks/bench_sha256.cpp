// Exact identity API benchmark, including its unchanged framing and hex encoding.
// The runner independently verifies the digest with Python hashlib.
#include "../src/core/canonical_json.h"

#include <charconv>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
std::size_t number(std::string_view text) {
    std::size_t value       = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value > 1024 * 1024)
        throw std::invalid_argument("argument outside benchmark bounds");
    return value;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::invalid_argument("usage: bench_sha256 BYTES ITERATIONS");
        const auto bytes = number(argv[1]), iterations = number(argv[2]);
        if (!iterations) throw std::invalid_argument("iterations must be positive");
        std::string input(bytes, '\0');
        for (std::size_t i = 0; i < bytes; ++i)
            input[i] = static_cast<char>(i % 251);
        neograph::json samples = neograph::json::array();
        std::string    digest;
        for (std::size_t i = 0; i < iterations + 20; ++i) {
            const auto begin = std::chrono::steady_clock::now();
            auto value = neograph::detail::sha256_identity("sha-benchmark/v1", "record", input);
            const auto elapsed =
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin)
                    .count();
            if (!digest.empty() && value != digest) throw std::runtime_error("unstable digest");
            digest = std::move(value);
            if (i >= 20) samples.push_back(elapsed);
        }
        std::cout << neograph::json{{"payload_bytes", bytes},
                                    {"iterations", iterations},
                                    {"warmup", 20},
                                    {"samples_us", samples},
                                    {"digest", digest},
                                    {"status", "ok"},
                                    {"build_type", NEOGRAPH_BENCH_BUILD_TYPE}}
                         .dump()
                  << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
