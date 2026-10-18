#include "apexmatch/extensions.hpp"
#include "test_support.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>

namespace {
using namespace apexmatch;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class F> void rejects(F&& function) {
    bool caught{};
    try { function(); } catch (const std::exception&) { caught = true; }
    require(caught, "expected extension error");
}
void queue_tests() {
    rejects([] { BoundedQueue<int> queue(0); });
    BoundedQueue<int> bounded(2);
    require(bounded.push(1) && bounded.push(2), "queue push failed");
    auto blocked = std::async(std::launch::async, [&] { return bounded.push(3); });
    const bool was_blocked = blocked.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout;
    bounded.close();
    const bool pushed_after_close = blocked.get();
    require(was_blocked && !pushed_after_close, "bounded push did not block/wake on close");
    require(bounded.high_water() == 2 && bounded.size() == 2, "capacity exceeded");
    require(bounded.pop() == 1 && bounded.pop() == 2 && !bounded.pop(), "close failed to drain FIFO");
    require(!bounded.push(4), "push accepted after close");
    bounded.close();
    BoundedQueue<int> empty(1);
    auto reader = std::async(std::launch::async, [&] { return empty.pop(); });
    const bool reader_blocked = reader.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout;
    empty.close();
    require(!reader.get() && reader_blocked, "closed empty queue did not wake reader");

    BoundedQueue<std::size_t> ordered(7);
    constexpr std::size_t count = 50000;
    std::jthread producer([&] { for (std::size_t i = 0; i < count; ++i) ordered.push(i); ordered.close(); });
    std::size_t expected{};
    bool correct = true;
    while (auto value = ordered.pop()) { if (*value != expected++) correct = false; }
    producer.join();
    require(correct && expected == count && ordered.high_water() <= 7, "FIFO lost or duplicated events");

    BoundedQueue<std::size_t> multiple(13);
    constexpr std::size_t each = 10000;
    std::jthread first([&] { for (std::size_t i = 0; i < each; ++i) multiple.push(i); });
    std::jthread second([&] { for (std::size_t i = 0; i < each; ++i) multiple.push(each + i); });
    std::jthread closer([&] { first.join(); second.join(); multiple.close(); });
    std::unordered_set<std::size_t> seen;
    while (auto value = multiple.pop()) seen.insert(*value);
    closer.join();
    require(seen.size() == 2 * each && multiple.high_water() <= 13, "multi-producer queue loss/duplication");

    BoundedQueue<std::size_t> shared(3);
    std::array<std::vector<std::size_t>, 2> consumed;
    std::jthread consumer_a([&] { while (auto value = shared.pop()) consumed[0].push_back(*value); });
    std::jthread consumer_b([&] { while (auto value = shared.pop()) consumed[1].push_back(*value); });
    std::jthread writer_a([&] { for (std::size_t i = 0; i < each; ++i) shared.push(i); });
    std::jthread writer_b([&] { for (std::size_t i = 0; i < each; ++i) shared.push(each + i); });
    writer_a.join(); writer_b.join(); shared.close();
    consumer_a.join(); consumer_b.join();
    seen.clear();
    for (const auto& batch : consumed) for (const auto value : batch)
        require(seen.insert(value).second, "multi-consumer queue duplicated a value");
    require(seen.size() == 2 * each && shared.high_water() <= 3, "multi-consumer queue lost a value");
}
void pipeline_tests() {
    const std::string source = "# fixture\nADD 1 BUY 100 9\nADD 2 SELL 99 3\nCANCEL 1\nADD x BUY 100 2\nBOOK\nSTATS\n";
    MatchingEngine engine;
    std::istringstream baseline_input(source);
    std::string expected, line;
    while (std::getline(baseline_input, line)) expected += process_line(engine, line);
    for (std::size_t capacity : {std::size_t{1}, std::size_t{2}, std::size_t{64}}) {
        std::istringstream input(source);
        std::ostringstream output;
        const auto report = replay_pipeline(input, output, capacity);
        require(output.str() == expected, "pipeline differs from sequential replay");
        require(report.lines == 7 && report.queue_high_water <= capacity, "pipeline counts invalid");
    }
    std::istringstream empty;
    std::ostringstream empty_output;
    require(replay_pipeline(empty, empty_output, 1).lines == 0, "empty pipeline did work");
    const std::string huge_line(100000, 'x');
    std::istringstream huge_input(huge_line + "\nBOOK\n");
    std::ostringstream huge_output;
    MatchingEngine oversized;
    const auto expected_oversized = process_line(oversized, huge_line) + process_line(oversized, "BOOK");
    require(replay_pipeline(huge_input, huge_output, 1).lines == 2 && huge_output.str() == expected_oversized,
            "pipeline oversized-line discard changed replay semantics");
    std::istringstream bad_input(std::string(10000, '\n'));
    std::ostringstream failed_output;
    failed_output.setstate(std::ios::badbit);
    rejects([&] { replay_pipeline(bad_input, failed_output, 1); });
    std::istringstream throwing_input;
    throwing_input.exceptions(std::ios::badbit);
    throwing_input.setstate(std::ios::eofbit);
    std::ostringstream normal_output;
    require(replay_pipeline(throwing_input, normal_output).lines == 0, "EOF should drain cleanly");

    struct FailingInput : std::stringbuf {
        FailingInput() : std::stringbuf("ADD 1 BUY 100 1\n") {}
        int_type underflow() override { throw std::runtime_error("injected input failure"); }
    } source_buffer;
    std::istream failed_input(&source_buffer);
    std::ostringstream drained_output;
    rejects([&] { replay_pipeline(failed_input, drained_output, 1); });
    MatchingEngine completed;
    require(drained_output.str() == process_line(completed, "ADD 1 BUY 100 1"),
            "producer input failure did not drain queued commands before propagating");
}
void market_data_tests() {
    MatchingEngine engine;
    MarketDataPublisher publisher(20);
    require(publisher.snapshot(engine.book()).find("\"md_sequence\":0") != std::string::npos, "initial MD sequence invalid");
    std::map<std::pair<Side, Price>, std::pair<std::uint64_t, std::size_t>> levels;
    Sequence updates{};
    const auto consume = [&](const ProcessResult& result) {
        bool changed{};
        for (const auto& event : result.events) if (event.kind == EventKind::BookUpdate) {
            changed = true; ++updates;
            const auto key = std::make_pair(event.side, event.price);
            if (!event.level_quantity) levels.erase(key);
            else levels[key] = {event.level_quantity, event.level_orders};
        }
        const auto output = publisher.publish(result, engine.book());
        require(publisher.sequence() == updates, "MD sequence skipped/duplicated");
        require(output.empty() == !changed, "MD published without a book change");
        if (changed) {
            require(output.find("\"type\":\"MD_LEVEL\"") != std::string::npos, "missing incremental MD");
            require(output.find("\"type\":\"MD_TOP\"") != std::string::npos, "missing top MD");
            require(output.ends_with(publisher.snapshot(engine.book())), "snapshot mismatch");
        }
        const auto depth = engine.book().depth(100);
        require(levels.size() == depth.bids.size() + depth.asks.size(), "MD level count disagrees with book");
        for (const auto& level : depth.bids) require(levels.at({Side::Buy, level.price}) == std::make_pair(level.quantity, level.orders), "bid MD differs");
        for (const auto& level : depth.asks) require(levels.at({Side::Sell, level.price}) == std::make_pair(level.quantity, level.orders), "ask MD differs");
    };
    consume(engine.add(1, Side::Buy, 100, 9));
    consume(engine.add(2, Side::Buy, 100, 3));
    consume(engine.add(3, Side::Buy, 99, 2));
    consume(engine.add(4, Side::Sell, 98, 11));
    consume(engine.cancel(2));
    consume(engine.cancel(3));
    consume(engine.cancel(999));
    consume(engine.add(1, Side::Sell, 101, 1));
}
void twap_tests() {
    TwapConfig config;
    const auto children = twap_schedule(config);
    require(children.size() == 5, "wrong TWAP child count");
    const std::array<Quantity, 5> quantities{5, 5, 5, 4, 4};
    for (std::size_t i = 0; i < children.size(); ++i)
        require(children[i].time == i * 25 && children[i].id == 1000 + i && children[i].quantity == quantities[i], "TWAP rounding or scheduling wrong");
    const std::vector<TimedCommand> market{{0, "ADD 1 SELL 100 8"}, {55, "ADD 2 SELL 101 5"}, {80, "ADD 3 SELL 102 20"}};
    MatchingEngine engine, repeated;
    const auto report = simulate_twap(engine, config, market);
    const auto again = simulate_twap(repeated, config, market);
    require(report.timeline == again.timeline && twap_report_json(report) == twap_report_json(again), "TWAP nondeterminism");
    require(report.submitted_quantity == 23 && report.accepted_quantity == 23 && report.executed_quantity == 13 &&
            report.unfilled_quantity == 10 && report.cancelled_quantity == 10 && report.rejected_quantity == 0, "TWAP fill report wrong");
    require(engine.book().active_orders() == 1 && engine.book().find(3)->remaining == 20, "TWAP child cancellation failed");
    TwapConfig single{1, 1, 10, 100, Side::Sell, 100, 100};
    MatchingEngine delayed;
    const auto late = simulate_twap(delayed, single, {{99, "ADD 1 BUY 100 1"}});
    require(late.executed_quantity == 1 && late.cancelled_quantity == 0 && late.completion_time == 100, "single TWAP child did not wait until end");
    MatchingEngine cancelled;
    const auto external_cancel = simulate_twap(cancelled, single, {{50, "CANCEL 100"}});
    require(external_cancel.cancelled_quantity == 1 && external_cancel.unfilled_quantity == 1, "external child cancellation report wrong");
    TwapConfig tied{2, 2, 10, 10, Side::Buy, 100, 1000};
    MatchingEngine ties;
    const auto simultaneous = simulate_twap(ties, tied, {{10, "ADD 1 SELL 100 1"},
        {10, "CANCEL 1"}, {10, "ADD 2 SELL 100 1"}});
    require(simultaneous.executed_quantity == 1 && simultaneous.cancelled_quantity == 1,
            "TWAP equal-time external ordering or zero-duration schedule changed");
    MatchingEngine conflicts;
    conflicts.add(1000, Side::Buy, 90, 1);
    TwapConfig collision{2, 2, 0, 10, Side::Buy, 100, 1000};
    const auto rejected = simulate_twap(conflicts, collision);
    require(rejected.submitted_quantity == 2 && rejected.accepted_quantity == 1 && rejected.rejected_quantity == 1 &&
            rejected.cancelled_quantity == 1 && conflicts.book().find(1000).has_value(), "TWAP rejection accounting wrong");
    auto invalid = config; invalid.slices = 0; rejects([&] { twap_schedule(invalid); });
    invalid = config; invalid.quantity = 0; rejects([&] { twap_schedule(invalid); });
    invalid = config; invalid.slices = 24; rejects([&] { twap_schedule(invalid); });
    invalid = config; invalid.end = 0; invalid.start = 1; rejects([&] { twap_schedule(invalid); });
    invalid = config; invalid.first_child_id = std::numeric_limits<OrderId>::max(); rejects([&] { twap_schedule(invalid); });
    TwapConfig extreme{std::numeric_limits<Quantity>::max(), 3, 0, std::numeric_limits<SimulationTime>::max(), Side::Buy, 1, std::numeric_limits<OrderId>::max() - 2};
    const auto huge = twap_schedule(extreme);
    require(huge.front().time == 0 && huge.back().time == extreme.end, "TWAP large time overflow");
    require(huge[0].quantity + huge[1].quantity + huge[2].quantity == extreme.quantity, "TWAP large quantity overflow");
    TwapConfig maximum{std::numeric_limits<Quantity>::max(), 1, 0,
        std::numeric_limits<SimulationTime>::max(), Side::Buy, 1, std::numeric_limits<OrderId>::max()};
    MatchingEngine boundary;
    const auto maximal = simulate_twap(boundary, maximum);
    require(maximal.submitted_quantity == maximum.quantity && maximal.accepted_quantity == maximum.quantity &&
            maximal.executed_quantity == 0 && maximal.cancelled_quantity == maximum.quantity &&
            maximal.unfilled_quantity == maximum.quantity && boundary.book().active_orders() == 0,
            "TWAP maximal quantity/ID/time accounting overflowed");
    MatchingEngine invalid_market;
    rejects([&] { simulate_twap(invalid_market, config, {{101, "ADD 1 SELL 100 1"}}); });
    rejects([&] { simulate_twap(invalid_market, config, {{0, "nonsense"}}); });
    std::istringstream timed("# comment\n0 ADD 1 SELL 100 3\n 25 CANCEL 1\n");
    const auto parsed = read_timed_commands(timed);
    require(parsed.size() == 2 && parsed[1].time == 25, "timed command parsing failed");
    std::istringstream malformed("-1 ADD 1 BUY 1 1\n");
    rejects([&] { read_timed_commands(malformed); });
}
}
void run_extension_tests() {
    apexmatch::test::run("bounded queue FIFO, capacity, multi-producer, and shutdown", queue_tests);
    apexmatch::test::run("pipeline equivalence, bounded lines, EOF, and output failure", pipeline_tests);
    apexmatch::test::run("market data incrementals, top, snapshots, and rejection silence", market_data_tests);
    apexmatch::test::run("TWAP scheduling, fills, cancellation, collisions, and overflow", twap_tests);
}
