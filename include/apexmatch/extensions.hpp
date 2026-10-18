#pragma once
#include "apexmatch/protocol.hpp"
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <istream>
#include <mutex>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace apexmatch {
// Closing prevents further pushes, wakes waiters, and lets consumers drain FIFO.
template<class T> class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
        if (!capacity) throw std::invalid_argument("queue capacity must be positive");
    }
    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;
    bool push(T value) {
        std::unique_lock lock(mutex_);
        writable_.wait(lock, [&] { return closed_ || values_.size() < capacity_; });
        if (closed_) return false;
        values_.push_back(std::move(value));
        if (values_.size() > high_water_) high_water_ = values_.size();
        readable_.notify_one();
        return true;
    }
    std::optional<T> pop() {
        std::unique_lock lock(mutex_);
        readable_.wait(lock, [&] { return closed_ || !values_.empty(); });
        if (values_.empty()) return std::nullopt;
        T value = std::move(values_.front());
        values_.pop_front();
        writable_.notify_one();
        return value;
    }
    void close() {
        std::lock_guard lock(mutex_);
        closed_ = true;
        readable_.notify_all();
        writable_.notify_all();
    }
    std::size_t size() const { std::lock_guard lock(mutex_); return values_.size(); }
    std::size_t high_water() const { std::lock_guard lock(mutex_); return high_water_; }
    std::size_t capacity() const { return capacity_; }
private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable readable_, writable_;
    std::deque<T> values_;
    bool closed_{};
    std::size_t high_water_{};
};
struct PipelineReport { std::uint64_t lines{}; std::size_t queue_high_water{}; };
PipelineReport replay_pipeline(std::istream& input, std::ostream& output,
                              std::size_t capacity = 1024, std::size_t depth = 10);

// Market data has its own sequence; order-entry events retain engine sequences.
class MarketDataPublisher {
public:
    explicit MarketDataPublisher(std::size_t depth = 10) : depth_(depth) {}
    std::string snapshot(const OrderBook& book) const;
    std::string publish(const ProcessResult& result, const OrderBook& book);
    void publish_to(const ProcessResult& result, const OrderBook& book,
                    const std::function<void(const std::string&)>& sink);
    Sequence sequence() const { return sequence_; }
private:
    std::size_t depth_;
    Sequence sequence_{};
};

struct ServerOptions {
    std::uint16_t port{9000}; // 0 asks the OS for an available localhost port.
    std::size_t max_line{4096};
    std::size_t max_output{1024 * 1024};
    std::size_t max_clients{32};
    std::size_t depth{10};
    std::function<void(std::uint16_t)> listening;
    std::function<bool()> stop_requested;
};
// A single event-loop thread owns the engine and all connection buffers.
void run_local_server(const ServerOptions& options);
void run_local_client(std::uint16_t port, std::istream& commands, std::ostream& responses);

using SimulationTime = std::uint64_t;
struct TwapConfig {
    Quantity quantity{23};
    std::size_t slices{5};
    SimulationTime start{0}, end{100};
    Side side{Side::Buy};
    Price limit_price{101};
    OrderId first_child_id{1000};
};
struct ScheduledChild { SimulationTime time{}; OrderId id{}; Quantity quantity{}; };
struct TimedCommand { SimulationTime time{}; std::string command; };
struct TwapReport {
    Quantity submitted_quantity{}, accepted_quantity{}, rejected_quantity{};
    Quantity executed_quantity{}, unfilled_quantity{}, cancelled_quantity{};
    std::size_t children{};
    SimulationTime completion_time{};
    std::vector<std::string> timeline;
};
std::vector<ScheduledChild> twap_schedule(const TwapConfig& config);
// Stable external-event order; external events precede children at equal times.
// All surviving child orders are cancelled after events/children at end time.
TwapReport simulate_twap(MatchingEngine& engine, const TwapConfig& config,
                         std::vector<TimedCommand> market = {});
std::string twap_report_json(const TwapReport& report);
std::vector<TimedCommand> read_timed_commands(std::istream& input);
} // namespace apexmatch
