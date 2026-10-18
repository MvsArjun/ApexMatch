#include "apexmatch/engine.hpp"
#include "apexmatch/protocol.hpp"
#include "reference_model.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace apexmatch;
namespace {
std::vector<Trade> trades(const ProcessResult& result) {
    std::vector<Trade> answer;
    for (const auto& event : result.events) if (event.kind == EventKind::Execution) answer.push_back(event.trade);
    return answer;
}
void compare_statistics(const Statistics& a, const Statistics& b) {
    REQUIRE(a.requests == b.requests);
    REQUIRE(a.accepted == b.accepted);
    REQUIRE(a.rejected == b.rejected);
    REQUIRE(a.cancelled_orders == b.cancelled_orders);
    REQUIRE(a.executions == b.executions);
    REQUIRE(a.submitted_quantity == b.submitted_quantity);
    REQUIRE(a.traded_quantity == b.traded_quantity);
    REQUIRE(a.cancelled_quantity == b.cancelled_quantity);
    REQUIRE(a.resting_quantity == b.resting_quantity);
}
void compare_events(const ProcessResult& actual, const ProcessResult& expected) {
    REQUIRE(actual.accepted == expected.accepted);
    REQUIRE(actual.events.size() == expected.events.size());
    for (std::size_t i = 0; i < actual.events.size(); ++i) {
        const auto& a = actual.events[i];
        const auto& e = expected.events[i];
        REQUIRE(a.kind == e.kind);
        REQUIRE(a.request_sequence == e.request_sequence);
        REQUIRE(a.event_sequence == e.event_sequence);
        switch (a.kind) {
        case EventKind::Accepted:
        case EventKind::Cancelled:
            REQUIRE(a.order_id == e.order_id);
            REQUIRE(a.side == e.side);
            REQUIRE(a.price == e.price);
            REQUIRE(a.quantity == e.quantity);
            break;
        case EventKind::Rejected:
            REQUIRE(a.order_id == e.order_id);
            REQUIRE(!a.reason.empty());
            break;
        case EventKind::Execution:
            REQUIRE(a.trade == e.trade);
            REQUIRE(a.trade.quantity > 0);
            break;
        case EventKind::BookUpdate:
            REQUIRE(a.side == e.side);
            REQUIRE(a.price == e.price);
            REQUIRE(a.level_quantity == e.level_quantity);
            REQUIRE(a.level_orders == e.level_orders);
            break;
        }
    }
}
void compare_books(const MatchingEngine& engine, const test::ReferenceModel& reference) {
    engine.check_invariants();
    engine.book().check_invariants();
    compare_statistics(engine.stats(), reference.statistics);
    REQUIRE(engine.book().active_orders() == reference.orders.size());
    REQUIRE(engine.book().depth(std::numeric_limits<std::size_t>::max()) ==
            reference.depth(std::numeric_limits<std::size_t>::max()));
    for (const std::size_t count : {0U, 1U, 3U, 10U}) REQUIRE(engine.book().depth(count) == reference.depth(count));
    const auto full = reference.depth(std::numeric_limits<std::size_t>::max());
    REQUIRE(engine.book().best_bid() == (full.bids.empty() ? std::optional<DepthLevel>{} : full.bids.front()));
    REQUIRE(engine.book().best_ask() == (full.asks.empty() ? std::optional<DepthLevel>{} : full.asks.front()));
    auto actual_orders = engine.book().orders();
    auto expected_orders = reference.orders;
    const auto by_id = [](const Order& a, const Order& b) { return a.id < b.id; };
    std::sort(actual_orders.begin(), actual_orders.end(), by_id);
    std::sort(expected_orders.begin(), expected_orders.end(), by_id);
    REQUIRE(actual_orders.size() == expected_orders.size());
    for (std::size_t i = 0; i < actual_orders.size(); ++i) {
        const auto& a = actual_orders[i];
        const auto& e = expected_orders[i];
        REQUIRE(a.id == e.id);
        REQUIRE(a.side == e.side);
        REQUIRE(a.price == e.price);
        REQUIRE(a.remaining == e.remaining);
        REQUIRE(a.arrival == e.arrival);
        const auto lookup = engine.book().find(a.id);
        REQUIRE(lookup.has_value());
        REQUIRE(lookup->remaining == e.remaining);
        REQUIRE(lookup->arrival == e.arrival);
        REQUIRE(engine.book().quantity_at(a.side, a.price) > 0);
    }
}

void empty_and_single_order() {
    MatchingEngine engine;
    REQUIRE(!engine.book().best_bid());
    REQUIRE(!engine.book().best_ask());
    REQUIRE(engine.book().depth(10) == Depth{});
    REQUIRE(engine.book().active_orders() == 0);
    REQUIRE(!engine.book().find(1));
    REQUIRE(engine.book().quantity_at(Side::Buy, 100) == 0);
    REQUIRE(engine.add(1, Side::Buy, 100, 7).accepted);
    REQUIRE(engine.book().best_bid() == DepthLevel{100, 7, 1});
    REQUIRE(!engine.book().best_ask());
    REQUIRE(engine.book().depth(0) == Depth{});
    REQUIRE(engine.book().find(1)->arrival == 1);
    REQUIRE(engine.book().find(1)->remaining == 7);
    engine.check_invariants();
}
void single_match_at_maker_price() {
    for (const auto resting_side : {Side::Buy, Side::Sell}) {
        MatchingEngine engine;
        engine.add(1, resting_side, 100, 7);
        const auto result = engine.add(2, resting_side == Side::Buy ? Side::Sell : Side::Buy,
                                       resting_side == Side::Buy ? 90 : 110, 7);
        const auto fills = trades(result);
        REQUIRE(fills.size() == 1);
        REQUIRE(fills[0] == Trade{1, resting_side == Side::Buy ? 1U : 2U,
                                 resting_side == Side::Buy ? 2U : 1U, 100, 7});
        REQUIRE(engine.book().active_orders() == 0);
        REQUIRE(engine.book().depth(10) == Depth{});
        REQUIRE(engine.stats().traded_quantity == 7);
        engine.check_invariants();
    }
}
void noncrossing_and_depth_order() {
    MatchingEngine engine;
    engine.add(1, Side::Buy, 99, 2);
    engine.add(2, Side::Buy, 100, 3);
    engine.add(3, Side::Sell, 102, 5);
    const auto result = engine.add(4, Side::Sell, 101, 7);
    REQUIRE(trades(result).empty());
    REQUIRE(engine.book().depth(10) == Depth{{{100, 3, 1}, {99, 2, 1}}, {{101, 7, 1}, {102, 5, 1}}});
    REQUIRE(engine.book().depth(1) == Depth{{{100, 3, 1}}, {{101, 7, 1}}});
    engine.check_invariants();
}
void better_price_before_older_order() {
    for (const auto maker_side : {Side::Buy, Side::Sell}) {
        MatchingEngine engine;
        engine.add(1, maker_side, 100, 4);
        engine.add(2, maker_side, maker_side == Side::Sell ? 99 : 101, 4);
        const auto fills = trades(engine.add(3, maker_side == Side::Sell ? Side::Buy : Side::Sell, 100, 4));
        REQUIRE(fills.size() == 1);
        REQUIRE((maker_side == Side::Sell ? fills[0].sell_id : fills[0].buy_id) == 2);
        REQUIRE(fills[0].price == (maker_side == Side::Sell ? 99 : 101));
        REQUIRE(engine.book().find(1)->remaining == 4);
        REQUIRE(!engine.book().find(2));
        engine.check_invariants();
    }
}
void fifo_partial_keeps_priority() {
    MatchingEngine engine;
    engine.add(91, Side::Sell, 100, 10);
    engine.add(3, Side::Sell, 100, 10);
    const auto arrival = engine.book().find(91)->arrival;
    const auto first = trades(engine.add(11, Side::Buy, 100, 4));
    REQUIRE(first.size() == 1);
    REQUIRE(first[0].sell_id == 91);
    REQUIRE(engine.book().find(91)->remaining == 6);
    REQUIRE(engine.book().find(91)->arrival == arrival);
    engine.add(2, Side::Sell, 100, 8);
    const auto second = trades(engine.add(12, Side::Buy, 100, 18));
    REQUIRE(second.size() == 3);
    REQUIRE(second[0].sell_id == 91 && second[0].quantity == 6);
    REQUIRE(second[1].sell_id == 3 && second[1].quantity == 10);
    REQUIRE(second[2].sell_id == 2 && second[2].quantity == 2);
    REQUIRE(engine.book().find(2)->remaining == 6);
    REQUIRE(engine.book().quantity_at(Side::Sell, 100) == 6);
    engine.check_invariants();
}
void several_levels_and_incoming_residual() {
    MatchingEngine engine;
    engine.add(1, Side::Sell, 103, 9);
    engine.add(2, Side::Sell, 101, 3);
    engine.add(3, Side::Sell, 102, 4);
    engine.add(4, Side::Sell, 101, 2);
    const auto fills = trades(engine.add(5, Side::Buy, 102, 12));
    REQUIRE(fills == std::vector<Trade>{{1, 5, 2, 101, 3}, {2, 5, 4, 101, 2}, {3, 5, 3, 102, 4}});
    REQUIRE(engine.book().depth(10) == Depth{{{102, 3, 1}}, {{103, 9, 1}}});
    REQUIRE(engine.book().quantity_at(Side::Sell, 101) == 0);
    REQUIRE(engine.book().quantity_at(Side::Sell, 102) == 0);
    engine.check_invariants();
}
void cancellation_lifecycle_and_iterator_removal() {
    MatchingEngine engine;
    engine.add(1, Side::Buy, 100, 5);
    engine.add(2, Side::Buy, 100, 7);
    engine.add(3, Side::Buy, 100, 9);
    REQUIRE(engine.cancel(2).accepted); // Erase the middle FIFO node without disturbing its neighbours.
    REQUIRE(engine.book().quantity_at(Side::Buy, 100) == 14);
    REQUIRE(engine.cancel(1).accepted);
    engine.add(4, Side::Sell, 100, 4);
    REQUIRE(engine.book().find(3)->remaining == 5);
    const auto cancelled = engine.cancel(3);
    REQUIRE(cancelled.accepted);
    REQUIRE(cancelled.events[0].kind == EventKind::Cancelled);
    REQUIRE(cancelled.events[0].quantity == 5);
    REQUIRE(engine.book().active_orders() == 0);
    REQUIRE(engine.book().quantity_at(Side::Buy, 100) == 0);
    for (const auto id : {1U, 2U, 3U, 4U, 999U}) {
        const auto result = engine.cancel(id);
        REQUIRE(!result.accepted);
        REQUIRE(result.events.size() == 1);
        REQUIRE(result.events[0].reason == "inactive_order");
    }
    REQUIRE(engine.stats().cancelled_quantity == 17);
    REQUIRE(engine.stats().traded_quantity == 4);
    engine.check_invariants();
}
void duplicates_and_rejected_id_reuse() {
    MatchingEngine engine;
    REQUIRE(!engine.add(10, Side::Buy, 0, 5).accepted);
    REQUIRE(engine.add(10, Side::Buy, 100, 5).accepted);
    const auto before = engine.book().depth(10);
    REQUIRE(!engine.add(10, Side::Sell, 100, 8).accepted);
    REQUIRE(engine.book().depth(10) == before);
    REQUIRE(engine.cancel(10).accepted);
    REQUIRE(!engine.add(10, Side::Buy, 100, 5).accepted);
    engine.add(11, Side::Buy, 100, 5);
    engine.add(12, Side::Sell, 100, 5);
    REQUIRE(!engine.add(11, Side::Sell, 100, 2).accepted);
    REQUIRE(!engine.add(12, Side::Buy, 100, 2).accepted);
    engine.check_invariants();
}
void invalid_values_do_not_mutate_book() {
    MatchingEngine engine;
    engine.add(1, Side::Sell, 100, 7);
    const auto before = engine.book().depth(10);
    const auto expect_rejection = [&](OrderId id, Side side, Price price, Quantity quantity) {
        const auto submitted_before = engine.stats().submitted_quantity;
        const auto result = engine.add(id, side, price, quantity);
        REQUIRE(!result.accepted);
        REQUIRE(result.events.size() == 1);
        REQUIRE(result.events[0].kind == EventKind::Rejected);
        REQUIRE(!result.events[0].reason.empty());
        REQUIRE(engine.book().depth(10) == before);
        REQUIRE(engine.stats().submitted_quantity == submitted_before);
        REQUIRE(engine.book().find(1)->remaining == 7);
        engine.check_invariants();
    };
    expect_rejection(0, Side::Buy, 100, 5);
    expect_rejection(2, Side::Buy, 0, 5);
    expect_rejection(2, Side::Buy, -1, 5);
    expect_rejection(2, Side::Buy, std::numeric_limits<Price>::min(), 5);
    expect_rejection(2, Side::Buy, 100, 0);
    expect_rejection(2, Side::Buy, 100, -1);
    expect_rejection(2, Side::Buy, 100, std::numeric_limits<Quantity>::min());
    expect_rejection(2, static_cast<Side>(7), 100, 1);
    REQUIRE(engine.add(2, Side::Buy, 100, 5).accepted);
    REQUIRE(engine.book().find(1)->remaining == 2);
}
void arithmetic_boundaries() {
    constexpr auto maximum = std::numeric_limits<Quantity>::max();
    constexpr auto max_id = std::numeric_limits<OrderId>::max();
    MatchingEngine aggregate;
    REQUIRE(aggregate.add(1, Side::Buy, maximum, maximum).accepted);
    REQUIRE(aggregate.add(2, Side::Buy, maximum, maximum).accepted);
    REQUIRE(aggregate.book().quantity_at(Side::Buy, maximum) == std::numeric_limits<std::uint64_t>::max() - 1);
    REQUIRE(aggregate.add(max_id, Side::Buy, maximum, 1).accepted);
    REQUIRE(aggregate.book().quantity_at(Side::Buy, maximum) == std::numeric_limits<std::uint64_t>::max());
    REQUIRE(!aggregate.add(4, Side::Buy, maximum, 1).accepted);
    REQUIRE(aggregate.cancel(1).accepted);
    REQUIRE(!aggregate.add(4, Side::Buy, maximum, 1).accepted); // Lifetime cap survives cancellation.
    aggregate.check_invariants();
    MatchingEngine crossing;
    crossing.add(1, Side::Buy, maximum, maximum);
    const auto result = crossing.add(2, Side::Sell, maximum, maximum);
    REQUIRE(trades(result) == std::vector<Trade>{{1, 1, 2, maximum, maximum}});
    REQUIRE(crossing.add(3, Side::Buy, 1, 1).accepted);
    REQUIRE(crossing.stats().submitted_quantity == std::numeric_limits<std::uint64_t>::max());
    REQUIRE(crossing.stats().traded_quantity == static_cast<std::uint64_t>(maximum));
    REQUIRE(!crossing.add(4, Side::Sell, 1, 1).accepted);
    crossing.check_invariants();
}
void exact_event_order_and_sequence() {
    MatchingEngine engine;
    engine.add(1, Side::Sell, 100, 2);
    engine.add(2, Side::Sell, 101, 3);
    const auto result = engine.add(3, Side::Buy, 101, 7);
    const std::array expected{EventKind::Accepted, EventKind::Execution, EventKind::BookUpdate,
                              EventKind::Execution, EventKind::BookUpdate, EventKind::BookUpdate};
    REQUIRE(result.events.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        REQUIRE(result.events[i].kind == expected[i]);
        REQUIRE(result.events[i].request_sequence == 3);
        REQUIRE(result.events[i].event_sequence == 5 + i);
    }
    REQUIRE(result.events[2].price == 100 && result.events[2].level_quantity == 0 && result.events[2].level_orders == 0);
    REQUIRE(result.events[4].price == 101 && result.events[4].level_quantity == 0 && result.events[4].level_orders == 0);
    REQUIRE(result.events[5].side == Side::Buy && result.events[5].level_quantity == 2);
    const auto cancelled = engine.cancel(3);
    REQUIRE(cancelled.events.size() == 2);
    REQUIRE(cancelled.events[0].kind == EventKind::Cancelled);
    REQUIRE(cancelled.events[1].kind == EventKind::BookUpdate);
    REQUIRE(cancelled.events[0].request_sequence == 4 && cancelled.events[0].event_sequence == 11);
    const auto rejected = engine.cancel(3);
    REQUIRE(rejected.events[0].request_sequence == 5 && rejected.events[0].event_sequence == 13);
    REQUIRE(engine.add(4, Side::Buy, 1, 1).events[0].event_sequence == 14);
}
void strict_parser_boundaries() {
    REQUIRE(parse_command("ADD 18446744073709551615 BUY 9223372036854775807 9223372036854775807").kind == CommandKind::Add);
    REQUIRE(parse_command("CANCEL 18446744073709551615").kind == CommandKind::Cancel);
    REQUIRE(parse_command(" \tADD 1 SELL 1 2\r").kind == CommandKind::Add);
    REQUIRE(parse_command("BOOK").kind == CommandKind::Book);
    REQUIRE(parse_command("STATS").kind == CommandKind::Stats);
    REQUIRE(parse_command("").kind == CommandKind::Ignore);
    REQUIRE(parse_command(" \t\r").kind == CommandKind::Ignore);
    REQUIRE(parse_command(" # a whole-line comment").kind == CommandKind::Ignore);
    const std::vector<std::string> malformed{
        "ADD", "ADD 1 BUY 100", "ADD 1 BUY 100 2 extra", "ADD 1 BUY 100 2 # comment",
        "add 1 BUY 100 2", "ADD 1 buy 100 2", "ADD 1 BID 100 2", "ADD 1 SELL 1.2 2",
        "ADD -1 BUY 100 2", "ADD +1 BUY 100 2",
        "ADD 18446744073709551616 BUY 100 2", "ADD 1 BUY 9223372036854775808 2",
        "ADD 1 BUY 100 9223372036854775808", "ADD 1 BUY -9223372036854775809 2",
        "ADD 1 BUY +100 2", "ADD 1 BUY 100 +2", "ADD 1x BUY 100 2", "ADD 1 BUY 1e2 2",
        "CANCEL", "CANCEL 1 extra", "CANCEL -1", "CANCEL +1",
        "CANCEL 18446744073709551616", "BOOK extra", "STATS extra", "UNKNOWN",
        std::string("ADD 1 BUY 100 2\0junk", 19)};
    for (const auto& line : malformed) {
        const auto command = parse_command(line);
        REQUIRE(command.kind == CommandKind::Invalid);
        REQUIRE(!command.error.empty());
    }
    MatchingEngine engine;
    for (const auto* line : {"ADD 0 BUY 100 2", "ADD 1 BUY 0 2", "ADD 1 BUY -1 2",
                             "ADD 1 BUY 100 0", "ADD 1 BUY 100 -1", "CANCEL 0"}) {
        const auto result = apply_command(engine, parse_command(line));
        REQUIRE(!result.accepted);
        REQUIRE(result.events.size() == 1 && result.events[0].kind == EventKind::Rejected);
        REQUIRE(engine.book().active_orders() == 0);
    }
    REQUIRE(parse_command(std::string(4096, ' ')).kind == CommandKind::Ignore);
    REQUIRE(parse_command(std::string(4097, ' ')).kind == CommandKind::Invalid);
    REQUIRE(parse_command("#" + std::string(4096, 'x')).kind == CommandKind::Invalid);
}
void queries_rejections_and_json_escaping() {
    MatchingEngine engine;
    REQUIRE(process_line(engine, "# ignored").empty());
    REQUIRE(process_line(engine, "  ").empty());
    REQUIRE(!process_line(engine, "BOOK").empty());
    REQUIRE(!process_line(engine, "STATS").empty());
    REQUIRE(engine.stats().requests == 0);
    REQUIRE(!process_line(engine, "ADD 1 BUY 100 2 extra").empty());
    REQUIRE(engine.stats().requests == 1 && engine.stats().rejected == 1);
    const auto result = apply_command(engine, parse_command("ADD 1 BUY 100 2"));
    REQUIRE(result.accepted);
    REQUIRE(result.events.front().request_sequence == 2);
    REQUIRE(result.events.front().event_sequence == 2);
    const auto rejected = engine.reject("quote\" slash\\ newline\n tab\t");
    const auto json = event_json(rejected.events[0]);
    REQUIRE(json.find("quote\\\"") != std::string::npos);
    REQUIRE(json.find("slash\\\\") != std::string::npos);
    REQUIRE(json.find("newline\\n") != std::string::npos || json.find("newline\\u000a") != std::string::npos);
    REQUIRE(json.find("tab\\t") != std::string::npos || json.find("tab\\u0009") != std::string::npos);
    REQUIRE(json.find('\n') == std::string::npos);
    engine.check_invariants();
}
void deterministic_replay_between_allocators() {
    const std::vector<std::string> lines{
        "# deterministic stream", "BOOK", "ADD 1001 BUY 10500 10", "ADD 1002 SELL 10400 4",
        "ADD 1003 SELL 10500 3", "ADD 1004 SELL 10600 6", "ADD 1005 BUY 10700 8",
        "CANCEL 1004", "CANCEL 1001", "ADD 1001 BUY 10500 1", "bad input", "BOOK", "STATS"};
    MatchingEngine first, second, pool(AllocationMode::Pool);
    std::string all_first, all_second, all_pool;
    for (const auto& line : lines) {
        all_first += process_line(first, line);
        all_second += process_line(second, line);
        all_pool += process_line(pool, line);
    }
    REQUIRE(!all_first.empty());
    REQUIRE(all_first == all_second);
    REQUIRE(all_first == all_pool);
    REQUIRE(first.stats().requests == 9);
    first.check_invariants(); second.check_invariants(); pool.check_invariants();
}

struct Accounting { std::uint64_t submitted{}, executed{}, cancelled{}; };
void randomized_reference(std::uint64_t seed, AllocationMode mode) {
    MatchingEngine engine(mode);
    test::ReferenceModel reference;
    std::mt19937_64 random(seed);
    std::map<OrderId, Accounting> ledger;
    OrderId next_id = 1;
    for (std::size_t step = 0; step < 5000; ++step) {
        try {
            ProcessResult actual, expected;
            if (random() % 100 < 72) {
                const OrderId id = random() % 100 < 22 && next_id > 1 ? 1 + random() % (next_id - 1) : next_id++;
                Side side = random() % 2 == 0 ? Side::Buy : Side::Sell;
                Price price = 90 + static_cast<Price>(random() % 21);
                Quantity quantity = 1 + static_cast<Quantity>(random() % 100);
                switch (random() % 40) {
                case 0: price = 0; break;
                case 1: quantity = 0; break;
                case 2: quantity = -5; break;
                case 3: side = static_cast<Side>(17); break;
                default: break;
                }
                expected = reference.add(id, side, price, quantity);
                actual = engine.add(id, side, price, quantity);
                if (actual.accepted) {
                    REQUIRE(!ledger.contains(id));
                    ledger.emplace(id, Accounting{static_cast<std::uint64_t>(quantity), 0, 0});
                }
            } else {
                OrderId id = random() % (next_id + 10);
                // Exercise actual cancellation often, including middle FIFO positions.
                if (!reference.orders.empty() && random() % 2 == 0) id = reference.orders[random() % reference.orders.size()].id;
                expected = reference.cancel(id);
                actual = engine.cancel(id);
            }
            compare_events(actual, expected);
            for (const auto& event : actual.events) {
                if (event.kind == EventKind::Execution) {
                    REQUIRE(event.trade.quantity > 0);
                    REQUIRE(ledger.contains(event.trade.buy_id) && ledger.contains(event.trade.sell_id));
                    ledger.at(event.trade.buy_id).executed += static_cast<std::uint64_t>(event.trade.quantity);
                    ledger.at(event.trade.sell_id).executed += static_cast<std::uint64_t>(event.trade.quantity);
                } else if (event.kind == EventKind::Cancelled) {
                    ledger.at(event.order_id).cancelled += static_cast<std::uint64_t>(event.quantity);
                }
            }
            compare_books(engine, reference);
            std::uint64_t total_submitted = 0, total_executed = 0, total_cancelled = 0, total_resting = 0;
            for (const auto& [id, account] : ledger) {
                const auto order = engine.book().find(id);
                const auto remaining = order ? static_cast<std::uint64_t>(order->remaining) : 0;
                REQUIRE(account.executed <= account.submitted);
                REQUIRE(account.executed + account.cancelled + remaining == account.submitted);
                total_submitted += account.submitted;
                total_executed += account.executed;
                total_cancelled += account.cancelled;
                total_resting += remaining;
            }
            REQUIRE(total_submitted == engine.stats().submitted_quantity);
            REQUIRE(total_executed == 2 * engine.stats().traded_quantity);
            REQUIRE(total_cancelled == engine.stats().cancelled_quantity);
            REQUIRE(total_resting == engine.stats().resting_quantity);
            REQUIRE(total_submitted == total_executed + total_cancelled + total_resting);
        } catch (const std::exception& error) {
            throw std::runtime_error("seed=" + std::to_string(seed) + " step=" + std::to_string(step) +
                                     (mode == AllocationMode::Pool ? " pool: " : " baseline: ") + error.what());
        }
    }
}
} // namespace

void run_core_tests() {
    test::run("empty and single-order books", empty_and_single_order);
    test::run("single buy/sell fills at maker price", single_match_at_maker_price);
    test::run("non-crossing orders and ordered depth", noncrossing_and_depth_order);
    test::run("better price precedes older order", better_price_before_older_order);
    test::run("FIFO partial maker retains priority", fifo_partial_keeps_priority);
    test::run("multi-order multi-level sweep and taker residual", several_levels_and_incoming_residual);
    test::run("cancellation lifecycle and middle FIFO removal", cancellation_lifecycle_and_iterator_removal);
    test::run("duplicate IDs, completed IDs, and rejected ID reuse", duplicates_and_rejected_id_reuse);
    test::run("invalid requests preserve the book", invalid_values_do_not_mutate_book);
    test::run("int64 and uint64 arithmetic boundaries", arithmetic_boundaries);
    test::run("precise deterministic event order and sequences", exact_event_order_and_sequence);
    test::run("strict parser malformed and out-of-range input", strict_parser_boundaries);
    test::run("query sequencing and JSON escaping", queries_rejections_and_json_escaping);
    test::run("deterministic replay and allocator equivalence", deterministic_replay_between_allocators);
    for (const auto mode : {AllocationMode::Baseline, AllocationMode::Pool}) {
        for (const std::uint64_t seed : {0ULL, 42ULL, 20261018ULL, 0xA9E5C0FFEEULL}) {
            const std::string name = std::string(mode == AllocationMode::Pool ? "reference pool seed " : "reference baseline seed ") + std::to_string(seed);
            test::run(name.c_str(), [=] { randomized_reference(seed, mode); });
        }
    }
}
