#include "apexmatch/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Count only allocations routed through the book's PMR upstream. Ordinary
// result-event vectors and benchmark storage are intentionally excluded.
namespace allocation_probe {
struct Counts { std::uint64_t allocations{}, requested_bytes{}, live_bytes{}, peak_bytes{}; };
class Resource final : public std::pmr::memory_resource {
public:
    Counts counts;
private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        void* pointer = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        ++counts.allocations;
        counts.requested_bytes += bytes;
        counts.live_bytes += bytes;
        counts.peak_bytes = std::max(counts.peak_bytes, counts.live_bytes);
        return pointer;
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        counts.live_bytes -= bytes;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
};
} // namespace allocation_probe
namespace {
using namespace apexmatch;
using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;
struct Request { bool cancel{}; OrderId id{}; Side side{}; Price price{}; Quantity quantity{}; };
struct Random {
    std::uint64_t state;
    // SplitMix64 plus specified modulo reduction, independent of STL distributions.
    std::uint64_t next() {
        auto z = (state += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    std::uint64_t bounded(std::uint64_t n) { return next() % n; }
};
const std::vector<std::string> workload_names{
    "noncrossing", "immediate", "cancellation_bursts", "shared_level", "large_book", "asymmetric"
};
std::vector<Request> generate(std::string_view name, std::size_t n, std::uint64_t seed) {
    Random random{seed};
    std::vector<Request> result;
    result.reserve(n);
    std::vector<OrderId> cancellation_batch;
    cancellation_batch.reserve(256);
    OrderId id = 1;
    while (result.size() < n) {
        if (name == "cancellation_bursts") {
            cancellation_batch.clear();
            for (std::size_t j = 0; j < 256 && result.size() < n; ++j) {
                const auto side = random.bounded(2) == 0 ? Side::Buy : Side::Sell;
                const auto price = side == Side::Buy ? 99000 - static_cast<Price>(random.bounded(1000)) : 101000 + static_cast<Price>(random.bounded(1000));
                result.push_back({false, id, side, price, 1 + static_cast<Quantity>(random.bounded(100))});
                cancellation_batch.push_back(id++);
            }
            for (std::size_t j = cancellation_batch.size(); j > 1; --j) std::swap(cancellation_batch[j - 1], cancellation_batch[random.bounded(j)]);
            for (const auto cancel_id : cancellation_batch) {
                if (result.size() == n) break;
                result.push_back({true, cancel_id, Side::Buy, 0, 0});
            }
            continue;
        }
        const auto index = result.size();
        if (name == "noncrossing") {
            const auto side = random.bounded(2) == 0 ? Side::Buy : Side::Sell;
            const auto price = side == Side::Buy ? 99000 - static_cast<Price>(random.bounded(1000)) : 101000 + static_cast<Price>(random.bounded(1000));
            result.push_back({false, id++, side, price, 1 + static_cast<Quantity>(random.bounded(100))});
        } else if (name == "immediate") {
            // Every second request consumes the preceding maker, with no residual.
            result.push_back({false, id++, index % 2 == 0 ? Side::Sell : Side::Buy, 100000, 10});
        } else if (name == "shared_level") {
            result.push_back({false, id++, Side::Buy, 100000, 1 + static_cast<Quantity>(random.bounded(100))});
        } else if (name == "large_book") {
            // One order per distinct level, shuffled after generation below.
            const auto level = static_cast<Price>(index / 2);
            result.push_back({false, id++, index % 2 == 0 ? Side::Buy : Side::Sell,
                              index % 2 == 0 ? 1000000000 - level : 2000000000 + level, 10});
        } else if (name == "asymmetric") {
            const auto side = random.bounded(10) == 0 ? Side::Sell : Side::Buy;
            const auto price = side == Side::Buy ? 99000 + static_cast<Price>(random.bounded(1000)) : 99500;
            result.push_back({false, id++, side, price, 1 + static_cast<Quantity>(random.bounded(100))});
        } else throw std::runtime_error("unknown workload: " + std::string(name));
    }
    if (name == "large_book") {
        for (std::size_t j = result.size(); j > 1; --j) std::swap(result[j - 1], result[random.bounded(j)]);
    }
    return result;
}
ProcessResult apply(MatchingEngine& engine, const Request& request) {
    return request.cancel ? engine.cancel(request.id) : engine.add(request.id, request.side, request.price, request.quantity);
}
struct Outcome { std::uint64_t emitted{}, executions{}, accepted{}, active{}; bool operator==(const Outcome&) const = default; };
Outcome outcome(const MatchingEngine& engine, std::uint64_t emitted) {
    return {emitted, engine.stats().executions, engine.stats().accepted, engine.book().active_orders()};
}
struct Batch { std::int64_t nanoseconds{}; Outcome state; };
Batch batch(const std::vector<Request>& requests, AllocationMode mode) {
    MatchingEngine engine(mode);
    std::uint64_t emitted = 0;
    const auto start = Clock::now();
    for (const auto& request : requests) { const auto result = apply(engine, request); emitted += result.events.size(); }
    const auto end = Clock::now();
    const auto elapsed = std::chrono::duration_cast<Nanoseconds>(end - start).count();
    if (elapsed <= 0) throw std::runtime_error("batch too short for clock resolution; increase --size");
    engine.check_invariants();
    return {elapsed, outcome(engine, emitted)};
}
struct Latency { double mean{}; std::int64_t p50{}, p95{}, p99{}; Outcome state; };
Latency latencies(const std::vector<Request>& requests, AllocationMode mode) {
    std::vector<std::int64_t> samples(requests.size());
    MatchingEngine engine(mode);
    std::uint64_t emitted = 0;
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const auto start = Clock::now();
        {
            const auto result = apply(engine, requests[i]);
            emitted += result.events.size();
        } // Returned event destruction is included in both timing passes.
        const auto end = Clock::now();
        samples[i] = std::chrono::duration_cast<Nanoseconds>(end - start).count();
    }
    engine.check_invariants();
    const auto mean = std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(samples.size());
    std::sort(samples.begin(), samples.end());
    const auto percentile = [&](std::size_t percent) { return samples[(samples.size() * percent + 99) / 100 - 1]; };
    return {mean, percentile(50), percentile(95), percentile(99), outcome(engine, emitted)};
}
struct Memory { allocation_probe::Counts counts; std::uint64_t retained{}; Outcome state; };
Memory memory(const std::vector<Request>& requests, AllocationMode mode) {
    Memory measured;
    allocation_probe::Resource resource;
    {
        MatchingEngine engine(mode, &resource);
        std::uint64_t emitted = 0;
        for (const auto& request : requests) { const auto result = apply(engine, request); emitted += result.events.size(); }
        measured.counts = resource.counts;
        measured.retained = resource.counts.live_bytes;
        measured.state = outcome(engine, emitted);
    }
    if (resource.counts.live_bytes != 0) throw std::runtime_error("allocation probe observed live book memory after engine destruction");
    return measured;
}std::int64_t clock_overhead() {
    std::vector<std::int64_t> samples(10000);
    for (auto& sample : samples) { const auto a = Clock::now(); const auto b = Clock::now(); sample = std::chrono::duration_cast<Nanoseconds>(b - a).count(); }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}
std::uint64_t number(std::string_view value) {
    std::size_t used{};
    const auto result = std::stoull(std::string(value), &used, 10);
    if (used != value.size() || value.empty() || value.front() == '-') throw std::runtime_error("invalid nonnegative integer");
    return result;
}
struct Config { std::size_t size{10000}, warmups{1}, repeats{3}; std::uint64_t seed{305419896}; std::string workload{"all"}, mode{"both"}, format{"json"}, output; bool profile_only{}; };
Config arguments(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        const std::string_view option = argv[i];
        if (option == "--help") {
            std::cout << "apexmatch_bench [--size N] [--workload all|noncrossing|immediate|cancellation_bursts|shared_level|large_book|asymmetric]\n"
                         "  [--mode baseline|pool|both] [--warmups N] [--repeats N] [--seed N] [--format json|csv] [--output PATH] [--profile-only]\n";
            std::exit(0);
        }
        if (option == "--profile-only") { c.profile_only = true; continue; }
        if (++i == argc) throw std::runtime_error("missing option value");
        const std::string value = argv[i];
        if (option == "--size") c.size = static_cast<std::size_t>(number(value));
        else if (option == "--warmups") c.warmups = static_cast<std::size_t>(number(value));
        else if (option == "--repeats") c.repeats = static_cast<std::size_t>(number(value));
        else if (option == "--seed") c.seed = number(value);
        else if (option == "--workload") c.workload = value;
        else if (option == "--mode") c.mode = value;
        else if (option == "--format") c.format = value;
        else if (option == "--output") c.output = value;
        else throw std::runtime_error("unknown option: " + std::string(option));
    }
    if (c.size == 0 || c.size > 100000000 || c.repeats == 0) throw std::runtime_error("size must be 1..100000000 and repeats must be positive");
    if (c.mode != "baseline" && c.mode != "pool" && c.mode != "both") throw std::runtime_error("mode must be baseline, pool, or both");
    if (c.format != "json" && c.format != "csv") throw std::runtime_error("format must be json or csv");
    if (c.profile_only && c.format != "json") throw std::runtime_error("profile-only output requires json format");
    if (c.workload != "all" && std::find(workload_names.begin(), workload_names.end(), c.workload) == workload_names.end()) throw std::runtime_error("unknown workload");
    return c;
}
void emit(std::ostream& output, const Config& config, const std::string& workload, AllocationMode mode, std::size_t repeat,
          std::int64_t timer_ns, const Batch& b, const Latency& l, const Memory& m) {
    const auto* label = mode == AllocationMode::Baseline ? "baseline" : "pool";
    const double throughput = static_cast<double>(config.size) * 1e9 / static_cast<double>(b.nanoseconds);
    output << std::setprecision(12);
    if (config.format == "csv") {
        output << workload << ',' << label << ',' << config.size << ',' << config.seed << ',' << repeat << ',' << config.warmups << ','
               << b.nanoseconds << ',' << throughput << ',' << l.mean << ',' << l.p50 << ',' << l.p95 << ',' << l.p99 << ',' << timer_ns << ','
               << m.counts.allocations << ',' << m.counts.requested_bytes << ',' << m.counts.peak_bytes << ',' << m.retained << ','
               << b.state.emitted << ',' << b.state.executions << ',' << b.state.accepted << ',' << b.state.active << '\n';
    } else {
        output << "{\"schema_version\":1,\"workload\":\"" << workload << "\",\"mode\":\"" << label << "\",\"requests\":" << config.size
               << ",\"seed\":" << config.seed << ",\"repeat\":" << repeat << ",\"warmups\":" << config.warmups
               << ",\"batch_ns\":" << b.nanoseconds << ",\"events_per_second\":" << throughput << ",\"latency_mean_ns\":" << l.mean
               << ",\"latency_p50_ns\":" << l.p50 << ",\"latency_p95_ns\":" << l.p95 << ",\"latency_p99_ns\":" << l.p99
               << ",\"timer_pair_median_ns\":" << timer_ns << ",\"allocations\":" << m.counts.allocations
               << ",\"allocated_bytes_total\":" << m.counts.requested_bytes << ",\"peak_live_requested_bytes\":" << m.counts.peak_bytes
               << ",\"retained_requested_bytes\":" << m.retained << ",\"emitted_events\":" << b.state.emitted
               << ",\"executions\":" << b.state.executions << ",\"accepted_adds\":" << b.state.accepted << ",\"active_orders\":" << b.state.active << "}\n";
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        const auto config = arguments(argc, argv);
        std::ofstream file;
        if (!config.output.empty()) { file.open(config.output); if (!file) throw std::runtime_error("cannot open output file"); }
        std::ostream& output = config.output.empty() ? std::cout : file;
        if (config.format == "csv") output << "workload,mode,requests,seed,repeat,warmups,batch_ns,events_per_second,latency_mean_ns,latency_p50_ns,latency_p95_ns,latency_p99_ns,timer_pair_median_ns,allocations,allocated_bytes_total,peak_live_requested_bytes,retained_requested_bytes,emitted_events,executions,accepted_adds,active_orders\n";
        const auto timer_ns = clock_overhead();
        for (const auto& name : workload_names) {
            if (config.workload != "all" && config.workload != name) continue;
            const auto requests = generate(name, config.size, config.seed);
            for (const auto mode : {AllocationMode::Baseline, AllocationMode::Pool}) {
                if (config.mode != "both" && (config.mode == "baseline") != (mode == AllocationMode::Baseline)) continue;
                for (std::size_t warmup = 0; warmup < config.warmups; ++warmup) (void)batch(requests, mode);
                if (config.profile_only) {
                    std::uint64_t elapsed = 0;
                    Outcome state;
                    for (std::size_t repeat = 0; repeat < config.repeats; ++repeat) { const auto run = batch(requests, mode); elapsed += static_cast<std::uint64_t>(run.nanoseconds); state = run.state; }
                    output << "{\"profile_only\":true,\"workload\":\"" << name << "\",\"mode\":\"" << (mode == AllocationMode::Baseline ? "baseline" : "pool")
                           << "\",\"requests_per_repeat\":" << config.size << ",\"repeats\":" << config.repeats << ",\"batch_ns_total\":" << elapsed << ",\"emitted_events_per_repeat\":" << state.emitted << "}\n";
                    continue;
                }
                const auto allocations = memory(requests, mode);
                for (std::size_t repeat = 0; repeat < config.repeats; ++repeat) {
                    const auto throughput = batch(requests, mode);
                    const auto latency = latencies(requests, mode);
                    if (!(throughput.state == latency.state) || !(throughput.state == allocations.state)) throw std::runtime_error("benchmark pass outcomes differ");
                    emit(output, config, name, mode, repeat, timer_ns, throughput, latency, allocations);
                    output.flush(); // Outside every measured interval.
                }
            }
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << "benchmark: " << error.what() << '\n'; return 1; }
}
