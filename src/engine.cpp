#include "apexmatch/engine.hpp"
#include "sequence_capacity.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace apexmatch {
namespace {
std::pmr::memory_resource* checked_resource(std::pmr::memory_resource* resource) {
    if (!resource) throw std::invalid_argument("null upstream memory resource");
    return resource;
}
bool pooled_allocation(AllocationMode mode) {
    switch (mode) {
    case AllocationMode::Baseline: return false;
    case AllocationMode::Pool: return true;
    }
    throw std::invalid_argument("invalid allocation mode");
}
}
const char* side_name(Side side) {
    switch (side) {
    case Side::Buy: return "BUY";
    case Side::Sell: return "SELL";
    }
    throw std::invalid_argument("invalid side");
}
std::string OrderValidator::validate(OrderId id, Side side, Price price, Quantity quantity) {
    if (id == 0) return "invalid_id";
    if (side != Side::Buy && side != Side::Sell) return "invalid_side";
    if (price <= 0) return "invalid_price";
    if (quantity <= 0) return "invalid_quantity";
    return {};
}
OrderBook::OrderBook(AllocationMode mode, std::pmr::memory_resource* upstream)
    : pool_(pooled_allocation(mode) ? std::make_unique<std::pmr::unsynchronized_pool_resource>(checked_resource(upstream)) : nullptr),
      resource_(pool_ ? static_cast<std::pmr::memory_resource*>(pool_.get()) : checked_resource(upstream)),
      bids_(resource_), asks_(resource_), active_(resource_), seen_(resource_) {}
std::optional<DepthLevel> OrderBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    const auto& [price, level] = *bids_.rbegin();
    return DepthLevel{price, level.quantity, level.fifo.size()};
}
std::optional<DepthLevel> OrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    const auto& [price, level] = *asks_.begin();
    return DepthLevel{price, level.quantity, level.fifo.size()};
}
Depth OrderBook::depth(std::size_t count) const {
    Depth result;
    for (auto it = bids_.rbegin(); it != bids_.rend() && result.bids.size() < count; ++it)
        result.bids.push_back({it->first, it->second.quantity, it->second.fifo.size()});
    for (auto it = asks_.begin(); it != asks_.end() && result.asks.size() < count; ++it)
        result.asks.push_back({it->first, it->second.quantity, it->second.fifo.size()});
    return result;
}
std::uint64_t OrderBook::quantity_at(Side side, Price price) const {
    if (side != Side::Buy && side != Side::Sell) throw std::invalid_argument("invalid side");
    const auto& levels = side == Side::Buy ? bids_ : asks_;
    auto it = levels.find(price);
    return it == levels.end() ? 0 : it->second.quantity;
}
std::optional<Order> OrderBook::find(OrderId id) const {
    auto it = active_.find(id);
    if (it == active_.end()) return std::nullopt;
    return *it->second.order;
}
std::vector<Order> OrderBook::orders() const {
    std::vector<Order> result;
    result.reserve(active_.size());
    for (const auto& [id, location] : active_) { (void)id; result.push_back(*location.order); }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.arrival < b.arrival; });
    return result;
}
void OrderBook::check_invariants() const {
    auto require = [](bool condition, const char* message) { if (!condition) throw std::logic_error(message); };
    std::unordered_set<OrderId> ids;
    const auto inspect = [&](const auto& levels, Side side) {
        Price previous_price = 0;
        for (const auto& [price, level] : levels) {
            require(price > previous_price, "price ordering"); previous_price = price;
            require(!level.fifo.empty(), "empty price level");
            std::uint64_t total = 0;
            Sequence previous_arrival = 0;
            for (auto it = level.fifo.begin(); it != level.fifo.end(); ++it) {
                const auto& order = *it;
                require(order.id != 0 && order.side == side && order.price == price && order.remaining > 0, "invalid resting order");
                require(order.arrival > previous_arrival, "FIFO ordering"); previous_arrival = order.arrival;
                require(ids.insert(order.id).second && seen_.contains(order.id), "duplicate or unknown resting ID");
                auto active = active_.find(order.id);
                require(active != active_.end(), "unindexed order");
                require(active->second.side == side && active->second.price == price && active->second.order == it, "wrong active location");
                auto quantity = static_cast<std::uint64_t>(order.remaining);
                require(quantity <= std::numeric_limits<std::uint64_t>::max() - total, "aggregate overflow");
                total += quantity;
            }
            require(total == level.quantity, "level aggregate mismatch");
        }
    };
    inspect(bids_, Side::Buy); inspect(asks_, Side::Sell);
    require(ids.size() == active_.size(), "orphan active ID");
    require(bids_.empty() || asks_.empty() || bids_.rbegin()->first < asks_.begin()->first, "crossed book");
}
void MatchingEngine::begin_request() {
    // Reserve enough sequence space for the worst case before changing any state.
    if (!detail::sequence_capacity_available(stats_.requests, event_sequence_, execution_sequence_, book_.active_.size()))
        throw std::overflow_error("request, event or execution sequence exhausted");
    ++stats_.requests;
}
void MatchingEngine::emit(ProcessResult& result, OrderEvent event) {
    event.request_sequence = stats_.requests;
    event.event_sequence = ++event_sequence_;
    result.events.push_back(std::move(event));
}
ProcessResult MatchingEngine::rejected(OrderId id, std::string reason) {
    ++stats_.rejected;
    ProcessResult result;
    OrderEvent event; event.kind = EventKind::Rejected; event.order_id = id; event.reason = std::move(reason);
    emit(result, std::move(event));
    return result;
}
ProcessResult MatchingEngine::reject(std::string reason) {
    begin_request(); return rejected(0, std::move(reason));
}
void MatchingEngine::book_event(ProcessResult& result, Side side, Price price) {
    OrderEvent event; event.kind = EventKind::BookUpdate; event.side = side; event.price = price;
    const auto& levels = side == Side::Buy ? book_.bids_ : book_.asks_;
    auto it = levels.find(price);
    if (it != levels.end()) { event.level_quantity = it->second.quantity; event.level_orders = it->second.fifo.size(); }
    emit(result, std::move(event));
}
ProcessResult MatchingEngine::add(OrderId id, Side side, Price price, Quantity quantity) {
    begin_request();
    auto error = OrderValidator::validate(id, side, price, quantity);
    if (!error.empty()) return rejected(id, std::move(error));
    if (book_.seen_.contains(id)) return rejected(id, "duplicate_id");
    auto unsigned_quantity = static_cast<std::uint64_t>(quantity);
    if (unsigned_quantity > std::numeric_limits<std::uint64_t>::max() - stats_.submitted_quantity)
        return rejected(id, "quantity_overflow");
    book_.seen_.insert(id);
    ++stats_.accepted; stats_.submitted_quantity += unsigned_quantity; stats_.resting_quantity += unsigned_quantity;
    ProcessResult result; result.accepted = true;
    OrderEvent acknowledgement; acknowledgement.kind = EventKind::Accepted; acknowledgement.order_id = id;
    acknowledgement.side = side; acknowledgement.price = price; acknowledgement.quantity = quantity;
    emit(result, std::move(acknowledgement));
    Quantity remaining = quantity;
    auto& opposite = side == Side::Buy ? book_.asks_ : book_.bids_;
    while (remaining > 0 && !opposite.empty()) {
        auto level_it = side == Side::Buy ? opposite.begin() : std::prev(opposite.end());
        const auto execution_price = level_it->first;
        if ((side == Side::Buy && execution_price > price) || (side == Side::Sell && execution_price < price)) break;
        auto& level = level_it->second;
        auto& maker = level.fifo.front();
        auto executed = std::min(remaining, maker.remaining);
        Trade trade{++execution_sequence_, side == Side::Buy ? id : maker.id,
                    side == Side::Sell ? id : maker.id, execution_price, executed};
        remaining -= executed; maker.remaining -= executed;
        level.quantity -= static_cast<std::uint64_t>(executed);
        stats_.traded_quantity += static_cast<std::uint64_t>(executed);
        stats_.resting_quantity -= static_cast<std::uint64_t>(executed) * 2;
        ++stats_.executions;
        if (maker.remaining == 0) { book_.active_.erase(maker.id); level.fifo.pop_front(); }
        if (level.fifo.empty()) opposite.erase(level_it);
        OrderEvent event; event.kind = EventKind::Execution; event.trade = trade;
        emit(result, std::move(event));
        book_event(result, side == Side::Buy ? Side::Sell : Side::Buy, execution_price);
    }
    if (remaining > 0) {
        auto& levels = side == Side::Buy ? book_.bids_ : book_.asks_;
        auto [level_it, inserted] = levels.try_emplace(price, book_.resource_); (void)inserted;
        auto& level = level_it->second;
        level.fifo.push_back({id, side, price, remaining, stats_.requests});
        level.quantity += static_cast<std::uint64_t>(remaining);
        book_.active_.emplace(id, OrderBook::Location{side, price, std::prev(level.fifo.end())});
        book_event(result, side, price);
    }
    return result;
}
ProcessResult MatchingEngine::cancel(OrderId id) {
    begin_request();
    if (id == 0) return rejected(id, "invalid_id");
    auto active = book_.active_.find(id);
    if (active == book_.active_.end()) return rejected(id, "inactive_order");
    const auto location = active->second;
    const auto remaining = location.order->remaining;
    auto& levels = location.side == Side::Buy ? book_.bids_ : book_.asks_;
    auto level_it = levels.find(location.price);
    auto& level = level_it->second;
    level.quantity -= static_cast<std::uint64_t>(remaining);
    level.fifo.erase(location.order);
    book_.active_.erase(active);
    if (level.fifo.empty()) levels.erase(level_it);
    ++stats_.cancelled_orders; stats_.cancelled_quantity += static_cast<std::uint64_t>(remaining);
    stats_.resting_quantity -= static_cast<std::uint64_t>(remaining);
    ProcessResult result; result.accepted = true;
    OrderEvent event; event.kind = EventKind::Cancelled; event.order_id = id; event.side = location.side;
    event.price = location.price; event.quantity = remaining;
    emit(result, std::move(event)); book_event(result, location.side, location.price);
    return result;
}
void MatchingEngine::check_invariants() const {
    book_.check_invariants();
    std::uint64_t resting = 0;
    for (const auto& [price, level] : book_.bids_) { (void)price; resting += level.quantity; }
    for (const auto& [price, level] : book_.asks_) { (void)price; resting += level.quantity; }
    if (resting != stats_.resting_quantity || stats_.traded_quantity > stats_.submitted_quantity / 2 ||
        stats_.submitted_quantity - stats_.traded_quantity - stats_.traded_quantity != stats_.cancelled_quantity + resting ||
        stats_.requests != stats_.accepted + stats_.rejected + stats_.cancelled_orders || execution_sequence_ != stats_.executions)
        throw std::logic_error("engine conservation or counters");
}
} // namespace apexmatch
