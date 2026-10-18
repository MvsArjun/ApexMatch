#include "apexmatch/protocol.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        apexmatch::AllocationMode mode = apexmatch::AllocationMode::Baseline;
        bool check = false;
        std::string path;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--pool") mode = apexmatch::AllocationMode::Pool;
            else if (arg == "--check") check = true;
            else if (arg == "--help") { std::cout << "Usage: apex_cli [--pool] [--check] [FILE]\nCommands: ADD id BUY|SELL price quantity; CANCEL id; BOOK; STATS\n"; return 0; }
            else if (path.empty()) path = std::move(arg);
            else throw std::runtime_error("unexpected argument");
        }
        std::ifstream file;
        std::istream* input = &std::cin;
        if (!path.empty()) { file.open(path); if (!file) throw std::runtime_error("cannot open input: " + path); input = &file; }
        apexmatch::MatchingEngine engine(mode);
        // Bound retained input storage even for an arbitrarily long malformed line.
        std::string line; line.reserve(4097);
        bool too_long = false;
        const auto submit = [&] {
            if (too_long) { for (const auto& event : engine.reject("line_too_long").events) std::cout << apexmatch::event_json(event) << '\n'; }
            else std::cout << apexmatch::process_line(engine, line);
            if (check) engine.check_invariants();
            line.clear(); too_long = false;
        };
        char c;
        while (input->get(c)) {
            if (c == '\n') submit();
            else if (line.size() < 4096) line += c;
            else too_long = true;
        }
        if (!line.empty() || too_long) submit();
        if (input->bad() || !std::cout) throw std::runtime_error("input/output failure");
        return 0;
    } catch (const std::exception& error) { std::cerr << "apex_cli: " << error.what() << '\n'; return 1; }
}
