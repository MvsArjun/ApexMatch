#include "apexmatch/extensions.hpp"
#include <charconv>
#include <csignal>
#include <iostream>

namespace {
volatile std::sig_atomic_t stopped = 0;
void signal_stop(int) { stopped = 1; }
std::size_t number(const std::string& text) {
    std::size_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) throw std::invalid_argument("invalid numeric argument");
    return value;
}
}
int main(int argc, char** argv) {
    try {
        apexmatch::ServerOptions options;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--help") {
                std::cout << "Usage: apex_server [--port 9000] [--max-line 4096] [--max-output 1048576] [--max-clients 32]\nBinds only to 127.0.0.1; port 0 selects an available port. SHUTDOWN or Ctrl+C drains responses then exits.\n";
                return 0;
            }
            if (i + 1 >= argc) throw std::invalid_argument("missing server option value");
            const auto value = number(argv[++i]);
            if (argument == "--port" && value <= 65535) options.port = static_cast<std::uint16_t>(value);
            else if (argument == "--max-line") options.max_line = value;
            else if (argument == "--max-output") options.max_output = value;
            else if (argument == "--max-clients") options.max_clients = value;
            else throw std::invalid_argument("unknown server option or invalid port");
        }
        std::signal(SIGINT, signal_stop);
        std::signal(SIGTERM, signal_stop);
        options.stop_requested = [] { return stopped != 0; };
        options.listening = [](std::uint16_t port) { std::cout << "LISTENING 127.0.0.1:" << port << std::endl; };
        apexmatch::run_local_server(options);
        return 0;
    } catch (const std::exception& error) { std::cerr << "apex_server: " << error.what() << '\n'; return 1; }
}
