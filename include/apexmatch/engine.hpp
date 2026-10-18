#pragma once
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace apexmatch {
using OrderId = std::uint64_t;
using Sequence = std::uint64_t;
using Price = std::int64_t;
using Quantity = std::int64_t;
enum class Side { Buy, Sell };
enum class AllocationMode { Baseline, Pool };
// Typed query/serialization APIs throw invalid_argument for unknown enum values.
// Order submission instead reports an invalid Side as a normal rejection event.
const char* side_name(Side side);
struct Order { OrderId id{}; Side side{}; Price price{}; Quantity remaining{}; Sequence arrival{}; };
struct Trade { Sequence execution_sequence{}; OrderId buy_id{}, sell_id{}; Price price{}; Quantity quantity{}; bool operator==(const Trade&) const = default; };
struct DepthLevel { Price price{}; std::uint64_t quantity{}; std::size_t orders{}; bool operator==(const DepthLevel&) const = default; };
struct Depth { std::vector<DepthLevel> bids, asks; bool operator==(const Depth&) const = default; };
enum class EventKind { Accepted, Rejected, Execution, Cancelled, BookUpdate };
struct OrderEvent {
    EventKind kind{};
    Sequence request_sequence{}, event_sequence{};
    OrderId order_id{};
    Side side{};
    Price price{};
    Quantity quantity{};
    std::uint64_t level_quantity{};
    std::size_t level_orders{};
    Trade trade{};
    std::string reason;
};
struct ProcessResult { bool accepted{}; std::vector<OrderEvent> events; };
struct Statistics {
    std::uint64_t requests{}, accepted{}, rejected{}, cancelled_orders{}, executions{};
    std::uint64_t submitted_quantity{}, traded_quantity{}, cancelled_quantity{}, resting_quantity{};
};
class OrderValidator {
public:
    static std::string validate(OrderId id, Side side, Price price, Quantity quantity);
};
class MatchingEngine;
class OrderBook {
public:
    // An injected upstream is non-owning and must outlive the book.
    explicit OrderBook(AllocationMode mode = AllocationMode::Baseline,
                       std::pmr::memory_resource* upstream = std::pmr::new_delete_resource());
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    std::optional<DepthLevel> best_bid() const;
    std::optional<DepthLevel> best_ask() const;
    Depth depth(std::size_t count) const;
    std::uint64_t quantity_at(Side side, Price price) const;
    std::size_t active_orders() const { return active_.size(); }
    std::optional<Order> find(OrderId id) const;
    std::vector<Order> orders() const;
    void check_invariants() const;
private:
    friend class MatchingEngine;
    struct Level { explicit Level(std::pmr::memory_resource* r) : fifo(r) {} std::pmr::list<Order> fifo; std::uint64_t quantity{}; };
    struct Location { Side side; Price price; std::pmr::list<Order>::iterator order; };
    std::unique_ptr<std::pmr::unsynchronized_pool_resource> pool_;
    std::pmr::memory_resource* resource_;
    std::pmr::map<Price, Level> bids_, asks_;
    std::pmr::unordered_map<OrderId, Location> active_;
    std::pmr::unordered_set<OrderId> seen_;
};
class MatchingEngine {
public:
    explicit MatchingEngine(AllocationMode mode = AllocationMode::Baseline,
                            std::pmr::memory_resource* upstream = std::pmr::new_delete_resource()) : book_(mode, upstream) {}
    ProcessResult add(OrderId id, Side side, Price price, Quantity quantity);
    ProcessResult cancel(OrderId id);
    ProcessResult reject(std::string reason);
    const OrderBook& book() const { return book_; }
    const Statistics& stats() const { return stats_; }
    void check_invariants() const;
private:
    void begin_request();
    void emit(ProcessResult& result, OrderEvent event);
    void book_event(ProcessResult& result, Side side, Price price);
    ProcessResult rejected(OrderId id, std::string reason);
    OrderBook book_;
    Statistics stats_;
    Sequence event_sequence_{}, execution_sequence_{};
};
std::string event_json(const OrderEvent& event);
std::string depth_json(const Depth& depth);
std::string stats_json(const Statistics& stats, std::size_t active_orders);
} // namespace apexmatch
