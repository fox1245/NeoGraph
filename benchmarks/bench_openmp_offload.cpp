// OpenMP target-offload PoC for NeoGraph-style fan-out workloads.
//
// This is intentionally not part of GraphEngine. A GPU cannot execute arbitrary
// virtual GraphNode bodies, JSON state transitions, or blocking LLM calls. The
// benchmark isolates the narrower case OpenMP target offload can accelerate:
// many independent workers applying the same numeric kernel to contiguous data.
//
// Usage:
//   bench_openmp_offload [fanout] [items_per_worker] [work_rounds]
//                         [repetitions] [samples]

#include <omp.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

struct Config {
    std::size_t fanout = 5;
    std::size_t items_per_worker = 4096;
    std::size_t work_rounds = 256;
    std::size_t repetitions = 10;
    std::size_t samples = 5;
};

#pragma omp declare target
inline double numeric_kernel(double value, std::size_t rounds) {
    for (std::size_t round = 0; round < rounds; ++round) {
        const double bias = static_cast<double>((round & 7U) + 1U) * 1.0e-7;
        value = value * 1.00000011920928955078125 + bias;
    }
    return value;
}
#pragma omp end declare target

std::size_t parse_positive(std::string_view text, const char* name) {
    std::size_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0) {
        throw std::invalid_argument(std::string(name) + " must be a positive integer");
    }
    return value;
}

std::size_t checked_total_items(const Config& config) {
    if (config.fanout > std::numeric_limits<std::size_t>::max() /
                            config.items_per_worker) {
        throw std::invalid_argument("fanout * items_per_worker overflows size_t");
    }
    const auto total = config.fanout * config.items_per_worker;
    if (total > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument("total item count exceeds the OpenMP loop bound");
    }
    return total;
}

using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

template <class Run>
double median_ms(std::size_t samples, Run&& run) {
    std::vector<double> timings;
    timings.reserve(samples);
    for (std::size_t sample = 0; sample < samples; ++sample) {
        timings.push_back(run());
    }
    std::sort(timings.begin(), timings.end());
    return timings[timings.size() / 2];
}

double run_serial(const double* input, double* output, std::int64_t count,
                  std::size_t rounds, std::size_t repetitions) {
    const auto start = Clock::now();
    for (std::size_t repetition = 0; repetition < repetitions; ++repetition) {
        for (std::int64_t item = 0; item < count; ++item) {
            output[item] = numeric_kernel(input[item], rounds);
        }
    }
    return elapsed_ms(start);
}

double run_openmp_cpu(const double* input, double* output, std::int64_t count,
                      std::size_t rounds, std::size_t repetitions) {
    const auto start = Clock::now();
    for (std::size_t repetition = 0; repetition < repetitions; ++repetition) {
#pragma omp parallel for schedule(static)
        for (std::int64_t item = 0; item < count; ++item) {
            output[item] = numeric_kernel(input[item], rounds);
        }
    }
    return elapsed_ms(start);
}

double run_target_mapped(const double* input, double* output, std::int64_t count,
                         std::size_t rounds, std::size_t repetitions) {
    const auto start = Clock::now();
    for (std::size_t repetition = 0; repetition < repetitions; ++repetition) {
#pragma omp target teams distribute parallel for map(to : input[0:count])                \
    map(from : output[0:count]) firstprivate(rounds)
        for (std::int64_t item = 0; item < count; ++item) {
            output[item] = numeric_kernel(input[item], rounds);
        }
    }
    return elapsed_ms(start);
}

double run_target_resident(const double* input, double* output, std::int64_t count,
                           std::size_t rounds, std::size_t repetitions) {
    double kernel_ms = 0.0;
#pragma omp target data map(to : input[0:count]) map(alloc : output[0:count])
    {
        const auto start = Clock::now();
        for (std::size_t repetition = 0; repetition < repetitions; ++repetition) {
#pragma omp target teams distribute parallel for firstprivate(rounds)
            for (std::int64_t item = 0; item < count; ++item) {
                output[item] = numeric_kernel(input[item], rounds);
            }
        }
        kernel_ms = elapsed_ms(start);
#pragma omp target update from(output[0:count])
    }
    return kernel_ms;
}

bool target_uses_device() {
    int initial_device = 1;
#pragma omp target map(from : initial_device)
    { initial_device = omp_is_initial_device(); }
    return initial_device == 0;
}

double checksum(const std::vector<double>& values) {
    long double sum = 0.0L;
    for (const double value : values) sum += value;
    return static_cast<double>(sum);
}

struct Accuracy {
    double max_abs_error = 0.0;
    bool correct = true;
};

Accuracy compare(const std::vector<double>& reference,
                 const std::vector<double>& candidate) {
    Accuracy result;
    for (std::size_t index = 0; index < reference.size(); ++index) {
        const double error = std::abs(reference[index] - candidate[index]);
        result.max_abs_error = std::max(result.max_abs_error, error);
        const double tolerance = 1.0e-11 * std::max(1.0, std::abs(reference[index]));
        if (!std::isfinite(candidate[index]) || error > tolerance) {
            result.correct = false;
        }
    }
    return result;
}

void print_result(const char* mode, double median, double serial_median,
                  std::size_t repetitions, const std::vector<double>& values,
                  const Accuracy& accuracy, const char* timing_scope) {
    std::cout << "result\t" << mode << '\t' << median << '\t'
              << (median * 1000.0 / static_cast<double>(repetitions)) << '\t'
              << (serial_median / median) << '\t' << checksum(values) << '\t'
              << accuracy.max_abs_error << '\t' << (accuracy.correct ? 1 : 0) << '\t'
              << timing_scope << '\n';
}

} // namespace

int main(int argc, char** argv) try {
    if (argc > 6) {
        throw std::invalid_argument(
            "usage: bench_openmp_offload [fanout] [items_per_worker] "
            "[work_rounds] [repetitions] [samples]");
    }

    Config config;
    if (argc > 1) config.fanout = parse_positive(argv[1], "fanout");
    if (argc > 2) {
        config.items_per_worker = parse_positive(argv[2], "items_per_worker");
    }
    if (argc > 3) config.work_rounds = parse_positive(argv[3], "work_rounds");
    if (argc > 4) config.repetitions = parse_positive(argv[4], "repetitions");
    if (argc > 5) config.samples = parse_positive(argv[5], "samples");

    const std::size_t total_items = checked_total_items(config);
    const auto loop_count = static_cast<std::int64_t>(total_items);

    std::vector<double> input(total_items);
    for (std::size_t item = 0; item < total_items; ++item) {
        input[item] = 0.25 + static_cast<double>(item % 1024U) * 1.0e-4;
    }
    std::vector<double> serial_output(total_items);
    std::vector<double> cpu_output(total_items);
    std::vector<double> mapped_output(total_items);
    std::vector<double> resident_output(total_items);

    const bool offload_active = target_uses_device();

    // Prime CPU worker creation, device runtime initialization, device image
    // loading, and mappings before collecting samples.
    (void)run_serial(input.data(), serial_output.data(), loop_count,
                     config.work_rounds, 1);
    (void)run_openmp_cpu(input.data(), cpu_output.data(), loop_count,
                         config.work_rounds, 1);
    (void)run_target_mapped(input.data(), mapped_output.data(), loop_count,
                            config.work_rounds, 1);
    (void)run_target_resident(input.data(), resident_output.data(), loop_count,
                              config.work_rounds, 1);

    const double serial_median = median_ms(config.samples, [&] {
        return run_serial(input.data(), serial_output.data(), loop_count,
                          config.work_rounds, config.repetitions);
    });
    const double cpu_median = median_ms(config.samples, [&] {
        return run_openmp_cpu(input.data(), cpu_output.data(), loop_count,
                              config.work_rounds, config.repetitions);
    });
    const double mapped_median = median_ms(config.samples, [&] {
        return run_target_mapped(input.data(), mapped_output.data(), loop_count,
                                 config.work_rounds, config.repetitions);
    });
    const double resident_median = median_ms(config.samples, [&] {
        return run_target_resident(input.data(), resident_output.data(), loop_count,
                                   config.work_rounds, config.repetitions);
    });

    const Accuracy serial_accuracy{};
    const Accuracy cpu_accuracy = compare(serial_output, cpu_output);
    const Accuracy mapped_accuracy = compare(serial_output, mapped_output);
    const Accuracy resident_accuracy = compare(serial_output, resident_output);

    std::cout << std::setprecision(12);
    std::cout << "config\tfanout\t" << config.fanout << '\n';
    std::cout << "config\titems_per_worker\t" << config.items_per_worker << '\n';
    std::cout << "config\ttotal_items\t" << total_items << '\n';
    std::cout << "config\twork_rounds\t" << config.work_rounds << '\n';
    std::cout << "config\trepetitions\t" << config.repetitions << '\n';
    std::cout << "config\tsamples\t" << config.samples << '\n';
    std::cout << "runtime\topenmp_version\t" << _OPENMP << '\n';
    std::cout << "runtime\tcpu_max_threads\t" << omp_get_max_threads() << '\n';
    std::cout << "runtime\ttarget_devices\t" << omp_get_num_devices() << '\n';
    std::cout << "runtime\toffload_active\t" << (offload_active ? 1 : 0) << '\n';
    std::cout << "header\tmode\tmedian_ms\tper_repetition_us\t"
                 "speedup_vs_serial\tchecksum\tmax_abs_error\tcorrect\t"
                 "timing_scope\n";
    print_result("serial_host", serial_median, serial_median, config.repetitions,
                 serial_output, serial_accuracy, "compute");
    print_result("openmp_cpu_auto", cpu_median, serial_median, config.repetitions,
                 cpu_output, cpu_accuracy, "compute_plus_host_scheduling");
    print_result("target_mapped", mapped_median, serial_median, config.repetitions,
                 mapped_output, mapped_accuracy, "compute_plus_transfer");
    print_result("target_resident", resident_median, serial_median,
                 config.repetitions, resident_output, resident_accuracy,
                 "kernel_only");

    if (!cpu_accuracy.correct || !mapped_accuracy.correct ||
        !resident_accuracy.correct) {
        std::cerr << "benchmark output mismatch\n";
        return 2;
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
