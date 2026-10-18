#include "apexmatch/extensions.hpp"
#include <charconv>
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    try {
        unsigned port = 9000;
        std::string file;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--help") {
                std::cout << "Usage: apex_client [--port 9000] [FILE]\nSends FILE or stdin commands to 127.0.0.1, one at a time; prints newline JSON responses.\n";
                return 0;
            }
            if (argument == "--port" && i + 1 < argc) {
                const std::string value = argv[++i];
                const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), port);
                if (error != std::errc{} || end != value.data() + value.size() || !port || port > 65535)
                    throw std::invalid_argument("invalid client port");
            } else if (!argument.empty() && argument[0] != '-' && file.empty()) file = argument;
            else throw std::invalid_argument("unknown or incomplete client argument");
        }
        std::ifstream input;
        if (!file.empty()) { input.open(file); if (!input) throw std::runtime_error("cannot open command file"); }
        apexmatch::run_local_client(static_cast<std::uint16_t>(port), file.empty() ? std::cin : input, std::cout);
        return 0;
    } catch (const std::exception& error) { std::cerr << "apex_client: " << error.what() << '\n'; return 1; }
}
