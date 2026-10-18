#include "apexmatch/extensions.hpp"
#include <charconv>
#include <fstream>
#include <iostream>
#include <limits>

namespace {
std::uint64_t number(const std::string& text) {
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) throw std::invalid_argument("invalid numeric argument");
    return value;
}
}
int main(int argc, char** argv) {
    try {
        apexmatch::TwapConfig config;
        std::string market_file;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--help") {
                std::cout << "Usage: apex_twap [--quantity 23] [--slices 5] [--start 0] [--end 100] [--side BUY|SELL] [--price 101] [--first-id 1000] [--market FILE]\nWithout --market uses deterministic liquidity: time 0 SELL 8@100; time 55 SELL 5@101; time 80 SELL 20@102.\n";
                return 0;
            }
            if (i + 1 >= argc) throw std::invalid_argument("missing TWAP option value");
            const std::string value = argv[++i];
            if (argument == "--market") { market_file = value; continue; }
            if (argument == "--side") {
                if (value == "BUY") config.side = apexmatch::Side::Buy;
                else if (value == "SELL") config.side = apexmatch::Side::Sell;
                else throw std::invalid_argument("side must be BUY or SELL");
                continue;
            }
            const auto parsed = number(value);
            if ((argument == "--quantity" || argument == "--price") && parsed > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                throw std::invalid_argument("signed price or quantity overflow");
            if (argument == "--quantity") config.quantity = static_cast<apexmatch::Quantity>(parsed);
            else if (argument == "--slices") {
                if (parsed > 1000000) throw std::invalid_argument("too many slices");
                config.slices = static_cast<std::size_t>(parsed);
            }
            else if (argument == "--start") config.start = parsed;
            else if (argument == "--end") config.end = parsed;
            else if (argument == "--price") config.limit_price = static_cast<apexmatch::Price>(parsed);
            else if (argument == "--first-id") config.first_child_id = parsed;
            else throw std::invalid_argument("unknown TWAP option");
        }
        std::vector<apexmatch::TimedCommand> market;
        if (!market_file.empty()) {
            std::ifstream input(market_file);
            if (!input) throw std::runtime_error("cannot open timed market input");
            market = apexmatch::read_timed_commands(input);
        } else {
            const auto span = config.end >= config.start ? config.end - config.start : 0;
            const auto at = [&](std::uint64_t percent) { return config.start + (span / 100) * percent + (span % 100) * percent / 100; };
            market = {{config.start, "ADD 1 SELL 100 8"}, {at(55), "ADD 2 SELL 101 5"}, {at(80), "ADD 3 SELL 102 20"}};
        }
        apexmatch::MatchingEngine engine;
        const auto report = apexmatch::simulate_twap(engine, config, std::move(market));
        for (const auto& line : report.timeline) std::cout << line << '\n';
        std::cout << apexmatch::twap_report_json(report) << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << "apex_twap: " << error.what() << '\n'; return 1; }
}
