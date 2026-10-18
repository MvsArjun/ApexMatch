#pragma once
#include "apexmatch/engine.hpp"
#include <algorithm>
#include <limits>
#include <unordered_set>
#include <vector>

namespace apexmatch::test {
// Intentionally slow oracle: one unsorted vector, linear best-maker searches,
// linear cancellation, and depth recomputed from scratch. No production
// validator, matching helper, level structure, or active-order index is reused.
class ReferenceModel {
public:
    std::vector<Order> orders;
    Statistics statistics{};

    ProcessResult add(OrderId id, Side side, Price price, Quantity quantity) {
        ++statistics.requests;
        std::string reason;
        if (id == 0) reason = "invalid_id";
        else if (side != Side::Buy && side != Side::Sell) reason = "invalid_side";
        else if (price <= 0) reason = "invalid_price";
        else if (quantity <= 0) reason = "invalid_quantity";
        else if (seen_.contains(id)) reason = "duplicate_id";
        else if (static_cast<std::uint64_t>(quantity) > std::numeric_limits<std::uint64_t>::max() - statistics.submitted_quantity)
            reason = "quantity_overflow";
        if (!reason.empty()) {
            ++statistics.rejected;
            ProcessResult rejected;
            emit(rejected, OrderEvent{.kind = EventKind::Rejected, .order_id = id, .reason = reason});
            return rejected;
        }
        seen_.insert(id);
        ++statistics.accepted;
        statistics.submitted_quantity += static_cast<std::uint64_t>(quantity);
        ProcessResult result{true, {}};
        emit(result, OrderEvent{.kind = EventKind::Accepted, .order_id = id, .side = side,
                               .price = price, .quantity = quantity, .reason = {}});
        Order taker{id, side, price, quantity, statistics.requests};
        while (taker.remaining > 0) {
            auto chosen = orders.end();
            for (auto item = orders.begin(); item != orders.end(); ++item) {
                if (item->side == side) continue;
                if (side == Side::Buy ? item->price > price : item->price < price) continue;
                if (chosen == orders.end() ||
                    (side == Side::Buy ? item->price < chosen->price : item->price > chosen->price) ||
                    (item->price == chosen->price && item->arrival < chosen->arrival)) chosen = item;
            }
            if (chosen == orders.end()) break;
            const Quantity fill = std::min(taker.remaining, chosen->remaining);
            const Side maker_side = chosen->side;
            const Price maker_price = chosen->price;
            const Trade trade{++execution_sequence_, side == Side::Buy ? id : chosen->id,
                              side == Side::Sell ? id : chosen->id, maker_price, fill};
            taker.remaining -= fill;
            chosen->remaining -= fill;
            statistics.resting_quantity -= static_cast<std::uint64_t>(fill);
            statistics.traded_quantity += static_cast<std::uint64_t>(fill);
            ++statistics.executions;
            if (chosen->remaining == 0) orders.erase(chosen);
            emit(result, OrderEvent{.kind = EventKind::Execution, .trade = trade, .reason = {}});
            level_update(result, maker_side, maker_price);
        }
        if (taker.remaining > 0) {
            statistics.resting_quantity += static_cast<std::uint64_t>(taker.remaining);
            orders.push_back(taker);
            level_update(result, side, price);
        }
        return result;
    }

    ProcessResult cancel(OrderId id) {
        ++statistics.requests;
        const auto item = std::find_if(orders.begin(), orders.end(), [id](const Order& o) { return o.id == id; });
        if (item == orders.end()) {
            ++statistics.rejected;
            ProcessResult rejected;
            emit(rejected, OrderEvent{.kind = EventKind::Rejected, .order_id = id,
                                      .reason = id == 0 ? "invalid_id" : "inactive_order"});
            return rejected;
        }
        const Order cancelled = *item;
        orders.erase(item);
        ++statistics.cancelled_orders;
        statistics.cancelled_quantity += static_cast<std::uint64_t>(cancelled.remaining);
        statistics.resting_quantity -= static_cast<std::uint64_t>(cancelled.remaining);
        ProcessResult result{true, {}};
        emit(result, OrderEvent{.kind = EventKind::Cancelled, .order_id = id,
                               .side = cancelled.side, .price = cancelled.price,
                               .quantity = cancelled.remaining, .reason = {}});
        level_update(result, cancelled.side, cancelled.price);
        return result;
    }

    Depth depth(std::size_t count) const {
        Depth answer;
        for (const auto side : {Side::Buy, Side::Sell}) {
            std::vector<Order> selected;
            for (const auto& order : orders) if (order.side == side) selected.push_back(order);
            std::sort(selected.begin(), selected.end(), [side](const Order& a, const Order& b) {
                return side == Side::Buy ? a.price > b.price : a.price < b.price;
            });
            auto& levels = side == Side::Buy ? answer.bids : answer.asks;
            for (const auto& order : selected) {
                if (levels.empty() || levels.back().price != order.price) levels.push_back({order.price, 0, 0});
                levels.back().quantity += static_cast<std::uint64_t>(order.remaining);
                ++levels.back().orders;
            }
            if (levels.size() > count) levels.resize(count);
        }
        return answer;
    }

private:
    std::unordered_set<OrderId> seen_;
    Sequence event_sequence_{}, execution_sequence_{};
    void emit(ProcessResult& result, OrderEvent event) {
        event.request_sequence = statistics.requests;
        event.event_sequence = ++event_sequence_;
        result.events.push_back(std::move(event));
    }
    void level_update(ProcessResult& result, Side side, Price price) {
        OrderEvent event{.kind = EventKind::BookUpdate, .side = side, .price = price, .reason = {}};
        for (const auto& order : orders) {
            if (order.side == side && order.price == price) {
                event.level_quantity += static_cast<std::uint64_t>(order.remaining);
                ++event.level_orders;
            }
        }
        emit(result, std::move(event));
    }
};
} // namespace apexmatch::test
