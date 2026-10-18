#include "apexmatch/extensions.hpp"
#include <charconv>
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    try {
        std::string file;
        std::size_t capacity = 1024;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--help") {
                std::cout << "Usage: apex_pipeline [--capacity N] [FILE]\nReads newline commands from FILE or stdin. JSON events go to stdout; queue statistics to stderr.\n";
                return 0;
            }
            if (argument == "--capacity" && i + 1 < argc) {
                const std::string value = argv[++i];
                const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), capacity);
                if (error != std::errc{} || end != value.data() + value.size() || !capacity)
                    throw std::invalid_argument("invalid queue capacity");
            } else if (!argument.empty() && argument[0] != '-' && file.empty()) file = argument;
            else throw std::invalid_argument("unknown or incomplete pipeline argument");
        }
        std::ifstream input;
        if (!file.empty()) { input.open(file); if (!input) throw std::runtime_error("cannot open input file"); }
        const auto report = apexmatch::replay_pipeline(file.empty() ? std::cin : input, std::cout, capacity);
        std::cerr << "lines=" << report.lines << " queue_high_water=" << report.queue_high_water << " capacity=" << capacity << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << "apex_pipeline: " << error.what() << '\n'; return 1; }
}
