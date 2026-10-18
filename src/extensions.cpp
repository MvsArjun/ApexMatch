#include "apexmatch/extensions.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <exception>
#include <limits>
#include <memory>
#include <sstream>
#include <thread>
#include <unordered_set>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace apexmatch {
PipelineReport replay_pipeline(std::istream& input, std::ostream& output,
                              std::size_t capacity, std::size_t depth) {
    BoundedQueue<std::string> queue(capacity);
    std::exception_ptr producer_error;
    std::jthread producer([&] {
        try {
            std::string line;
            bool stopped_early{};
            char character{};
            while (input.get(character)) {
                if (character == '\n') {
                    if (!queue.push(std::move(line))) { stopped_early = true; break; }
                    line.clear();
                } else if (line.size() < 4097) line.push_back(character);
                // Retain a 4097-byte sentinel for one parser rejection, discarding
                // the remainder until newline without unbounded producer storage.
            }
            if (!stopped_early && !line.empty()) queue.push(std::move(line));
            if (input.bad()) throw std::runtime_error("pipeline input read failed");
        } catch (...) { producer_error = std::current_exception(); }
        queue.close();
    });
    PipelineReport report;
    MatchingEngine engine;
    try {
        while (auto line = queue.pop()) {
            output << process_line(engine, *line, depth);
            if (!output) throw std::runtime_error("pipeline output write failed");
            ++report.lines;
        }
        engine.check_invariants();
    } catch (...) {
        queue.close();
        producer.join();
        throw;
    }
    producer.join(); // join publishes producer_error to this thread.
    if (producer_error) std::rethrow_exception(producer_error);
    report.queue_high_water = queue.high_water();
    return report;
}

namespace {
std::string top_json(const std::optional<DepthLevel>& level) {
    if (!level) return "null";
    return "{\"price\":" + std::to_string(level->price) + ",\"quantity\":" +
        std::to_string(level->quantity) + ",\"orders\":" + std::to_string(level->orders) + "}";
}
}
std::string MarketDataPublisher::snapshot(const OrderBook& book) const {
    return "{\"type\":\"MD_SNAPSHOT\",\"md_sequence\":" + std::to_string(sequence_) +
        ",\"depth\":" + depth_json(book.depth(depth_)) + "}\n";
}
std::string MarketDataPublisher::publish(const ProcessResult& result, const OrderBook& book) {
    std::string output;
    publish_to(result, book, [&](const std::string& message) { output += message; });
    return output;
}
void MarketDataPublisher::publish_to(const ProcessResult& result, const OrderBook& book,
                                     const std::function<void(const std::string&)>& sink) {
    bool changed{};
    Sequence request{};
    for (const auto& event : result.events) {
        if (event.kind != EventKind::BookUpdate) continue;
        if (sequence_ == std::numeric_limits<Sequence>::max())
            throw std::overflow_error("market-data sequence exhausted");
        ++sequence_;
        changed = true;
        request = event.request_sequence;
        sink("{\"type\":\"MD_LEVEL\",\"md_sequence\":" + std::to_string(sequence_) +
            ",\"request_sequence\":" + std::to_string(request) + ",\"side\":\"" + side_name(event.side) +
            "\",\"price\":" + std::to_string(event.price) + ",\"quantity\":" +
            std::to_string(event.level_quantity) + ",\"orders\":" + std::to_string(event.level_orders) + "}\n");
    }
    if (changed) {
        sink("{\"type\":\"MD_TOP\",\"md_sequence\":" + std::to_string(sequence_) +
            ",\"request_sequence\":" + std::to_string(request) + ",\"bid\":" + top_json(book.best_bid()) +
            ",\"ask\":" + top_json(book.best_ask()) + "}\n");
        sink(snapshot(book));
    }
}

std::vector<ScheduledChild> twap_schedule(const TwapConfig& config) {
    const auto reason = OrderValidator::validate(config.first_child_id, config.side,
                                                config.limit_price, config.quantity);
    if (!reason.empty()) throw std::invalid_argument("invalid TWAP: " + reason);
    if (!config.slices || config.slices > 1000000 ||
        config.slices > static_cast<std::uint64_t>(config.quantity))
        throw std::invalid_argument("TWAP slices must be 1..min(quantity,1000000)");
    if (config.end < config.start) throw std::invalid_argument("TWAP end precedes start");
    if (config.first_child_id > std::numeric_limits<OrderId>::max() - (config.slices - 1))
        throw std::invalid_argument("TWAP child ID range overflows");
    std::vector<ScheduledChild> children;
    children.reserve(config.slices);
    const auto count = static_cast<Quantity>(config.slices);
    const Quantity base = config.quantity / count, extra = config.quantity % count;
    const auto intervals = config.slices > 1 ? config.slices - 1 : 1;
    const auto duration = config.end - config.start;
    for (std::size_t i = 0; i < config.slices; ++i) {
        // Each product is bounded: quotient*i <= duration; remainder*i < 10^12.
        const auto offset = (duration / intervals) * i + ((duration % intervals) * i) / intervals;
        children.push_back({config.start + offset, config.first_child_id + i,
                            base + (i < static_cast<std::size_t>(extra) ? 1 : 0)});
    }
    return children;
}

TwapReport simulate_twap(MatchingEngine& engine, const TwapConfig& config,
                         std::vector<TimedCommand> market) {
    const auto children = twap_schedule(config);
    for (const auto& event : market) {
        if (event.time < config.start || event.time > config.end)
            throw std::invalid_argument("market event outside TWAP simulation interval");
        const auto command = parse_command(event.command);
        if (command.kind != CommandKind::Add && command.kind != CommandKind::Cancel)
            throw std::invalid_argument("timed market input requires valid ADD or CANCEL commands");
    }
    std::stable_sort(market.begin(), market.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    std::unordered_set<OrderId> accepted_children;
    TwapReport report;
    report.children = children.size();
    report.completion_time = config.end;
    const auto record = [&](SimulationTime time, const ProcessResult& result) {
        for (const auto& event : result.events) {
            report.timeline.push_back("{\"time\":" + std::to_string(time) + ",\"event\":" + event_json(event) + "}");
            if (event.kind == EventKind::Execution) {
                const auto id = config.side == Side::Buy ? event.trade.buy_id : event.trade.sell_id;
                if (accepted_children.contains(id)) report.executed_quantity += event.trade.quantity;
            }
            if (event.kind == EventKind::Cancelled && accepted_children.contains(event.order_id))
                report.cancelled_quantity += event.quantity;
        }
    };
    std::size_t external{};
    for (const auto& child : children) {
        while (external < market.size() && market[external].time <= child.time) {
            const auto& event = market[external++];
            record(event.time, apply_command(engine, parse_command(event.command)));
        }
        report.submitted_quantity += child.quantity;
        auto result = engine.add(child.id, config.side, config.limit_price, child.quantity);
        if (result.accepted) {
            accepted_children.insert(child.id);
            report.accepted_quantity += child.quantity;
        } else report.rejected_quantity += child.quantity;
        record(child.time, result);
    }
    while (external < market.size()) {
        const auto& event = market[external++];
        record(event.time, apply_command(engine, parse_command(event.command)));
    }
    for (const auto& child : children) {
        if (accepted_children.contains(child.id)) {
            if (const auto order = engine.book().find(child.id)) {
                record(config.end, engine.cancel(child.id));
            }
        }
    }
    report.unfilled_quantity = report.accepted_quantity - report.executed_quantity;
    engine.check_invariants();
    return report;
}
std::string twap_report_json(const TwapReport& report) {
    return "{\"type\":\"TWAP_REPORT\",\"children\":" + std::to_string(report.children) +
        ",\"completion_time\":" + std::to_string(report.completion_time) +
        ",\"submitted_quantity\":" + std::to_string(report.submitted_quantity) +
        ",\"accepted_quantity\":" + std::to_string(report.accepted_quantity) +
        ",\"rejected_quantity\":" + std::to_string(report.rejected_quantity) +
        ",\"executed_quantity\":" + std::to_string(report.executed_quantity) +
        ",\"unfilled_quantity\":" + std::to_string(report.unfilled_quantity) +
        ",\"cancelled_quantity\":" + std::to_string(report.cancelled_quantity) + "}";
}
std::vector<TimedCommand> read_timed_commands(std::istream& input) {
    std::vector<TimedCommand> result;
    std::string line;
    std::size_t line_number{};
    while (std::getline(input, line)) {
        ++line_number;
        const auto begin = line.find_first_not_of(" \t\r");
        if (begin == std::string::npos || line[begin] == '#') continue;
        const auto separator = line.find_first_of(" \t", begin);
        SimulationTime time{};
        const auto end = separator == std::string::npos ? line.size() : separator;
        const auto [ptr, error] = std::from_chars(line.data() + begin, line.data() + end, time);
        if (separator == std::string::npos || error != std::errc{} || ptr != line.data() + end)
            throw std::runtime_error("invalid simulated time at line " + std::to_string(line_number));
        result.push_back({time, line.substr(separator + 1)});
    }
    if (input.bad()) throw std::runtime_error("timed command read failed");
    return result;
}

namespace {
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
class SocketRuntime {
public:
    SocketRuntime() { WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data)) throw std::runtime_error("WSAStartup failed"); }
    ~SocketRuntime() { WSACleanup(); }
};
void close_socket(Socket socket) { closesocket(socket); }
int socket_error() { return WSAGetLastError(); }
bool would_block(int error) { return error == WSAEWOULDBLOCK; }
bool interrupted(int error) { return error == WSAEINTR; }
void nonblocking(Socket socket) { u_long mode = 1; if (ioctlsocket(socket, FIONBIO, &mode)) throw std::runtime_error("nonblocking socket failed"); }
using SocketLength = int;
#else
using Socket = int;
constexpr Socket invalid_socket = -1;
struct SocketRuntime {};
void close_socket(Socket socket) { ::close(socket); }
int socket_error() { return errno; }
bool would_block(int error) { return error == EAGAIN || error == EWOULDBLOCK; }
bool interrupted(int error) { return error == EINTR; }
void nonblocking(Socket socket) {
    const int flags = fcntl(socket, F_GETFL, 0);
    if (flags < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK) < 0) throw std::runtime_error("nonblocking socket failed");
}
using SocketLength = socklen_t;
#endif
struct SocketHandle {
    Socket value{invalid_socket};
    explicit SocketHandle(Socket socket = invalid_socket) : value(socket) {}
    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;
    ~SocketHandle() { if (value != invalid_socket) close_socket(value); }
    void close() { if (value != invalid_socket) { close_socket(value); value = invalid_socket; } }
};
int receive(Socket socket, char* buffer, int count) { return static_cast<int>(recv(socket, buffer, count, 0)); }
int transmit(Socket socket, const char* buffer, int count) {
#ifdef MSG_NOSIGNAL
    return static_cast<int>(send(socket, buffer, count, MSG_NOSIGNAL));
#else
    return static_cast<int>(send(socket, buffer, count, 0));
#endif
}
sockaddr_in localhost(std::uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return address;
}
struct Connection {
    SocketHandle socket;
    std::string input, output;
    std::size_t sent{};
    std::uint64_t commands{};
    bool subscribed{}, closing{}, dead{};
    explicit Connection(Socket value) : socket(value) {}
    std::size_t pending() const { return output.size() - sent; }
};
void queue_output(Connection& client, const std::string& text, std::size_t bound) {
    if (client.dead) return;
    if (text.size() > bound - client.pending()) { client.dead = true; client.output.clear(); client.sent = 0; return; }
    if (client.sent) { client.output.erase(0, client.sent); client.sent = 0; }
    client.output += text;
}
std::string response_end(std::uint64_t command) {
    return "{\"type\":\"RESPONSE_END\",\"command_sequence\":" + std::to_string(command) + "}\n";
}
}

void run_local_server(const ServerOptions& options) {
    if (!options.max_line || options.max_line > 1024 * 1024 || options.max_output < 256 ||
        !options.max_clients || options.max_clients >= FD_SETSIZE || options.depth > 1000)
        throw std::invalid_argument("invalid server buffer or client limit");
    [[maybe_unused]] SocketRuntime runtime;
    SocketHandle listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (listener.value == invalid_socket) throw std::runtime_error("socket creation failed");
    int reuse = 1;
    setsockopt(listener.value, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    const auto address = localhost(options.port);
    if (bind(listener.value, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener.value, static_cast<int>(options.max_clients)) != 0)
        throw std::runtime_error("localhost bind/listen failed");
#ifndef _WIN32
    if (listener.value >= FD_SETSIZE) throw std::runtime_error("listener exceeds select descriptor limit");
#endif
    nonblocking(listener.value);
    sockaddr_in actual{};
    SocketLength length = sizeof(actual);
    if (getsockname(listener.value, reinterpret_cast<sockaddr*>(&actual), &length))
        throw std::runtime_error("getsockname failed");
    if (options.listening) options.listening(ntohs(actual.sin_port));
    std::vector<std::unique_ptr<Connection>> clients;
    MatchingEngine engine;
    MarketDataPublisher publisher(options.depth);
    bool stopping{};
    auto deadline = std::chrono::steady_clock::time_point::max();
    const auto stop = [&] {
        if (stopping) return;
        stopping = true;
        listener.close();
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        for (auto& client : clients) client->closing = true;
    };
    const auto process = [&](Connection& client, const std::string& line) {
        ++client.commands;
        if (line == "SUBSCRIBE" || line == "SUBSCRIBE\r") {
            client.subscribed = true;
            queue_output(client, publisher.snapshot(engine.book()), options.max_output);
        } else if (line == "UNSUBSCRIBE" || line == "UNSUBSCRIBE\r") {
            client.subscribed = false;
            queue_output(client, "{\"type\":\"UNSUBSCRIBED\"}\n", options.max_output);
        } else if (line == "SHUTDOWN" || line == "SHUTDOWN\r") {
            queue_output(client, "{\"type\":\"SHUTDOWN\"}\n", options.max_output);
            stop();
        } else {
            const auto command = parse_command(line);
            if (command.kind == CommandKind::Book || command.kind == CommandKind::Stats || command.kind == CommandKind::Ignore) {
                queue_output(client, process_line(engine, line, options.depth), options.max_output);
            } else {
                auto result = apply_command(engine, command);
                for (const auto& event : result.events) queue_output(client, event_json(event) + "\n", options.max_output);
                publisher.publish_to(result, engine.book(), [&](const std::string& message) {
                    for (auto& peer : clients)
                        if (peer->subscribed && !peer->closing) queue_output(*peer, message, options.max_output);
                });
            }
        }
        queue_output(client, response_end(client.commands), options.max_output);
    };
    while (true) {
        if (options.stop_requested && options.stop_requested()) stop();
        clients.erase(std::remove_if(clients.begin(), clients.end(), [](const auto& client) {
            return client->dead || (client->closing && !client->pending());
        }), clients.end());
        if (stopping && (clients.empty() || std::chrono::steady_clock::now() >= deadline)) break;
        fd_set reads, writes;
        FD_ZERO(&reads); FD_ZERO(&writes);
        Socket maximum{};
        bool any{};
        if (!stopping && clients.size() < options.max_clients) {
            FD_SET(listener.value, &reads); maximum = listener.value; any = true;
        }
        for (const auto& client : clients) {
            if (!client->closing && client->pending() < options.max_output / 2) {
                FD_SET(client->socket.value, &reads); any = true;
            }
            if (client->pending()) { FD_SET(client->socket.value, &writes); any = true; }
            maximum = std::max(maximum, client->socket.value);
        }
        if (!any) { if (stopping) break; throw std::runtime_error("server has no selectable socket"); }
        timeval timeout{}; timeout.tv_usec = 100000;
        const int selected = select(static_cast<int>(maximum + 1), &reads, &writes, nullptr, &timeout);
        if (selected < 0) { if (interrupted(socket_error())) continue; throw std::runtime_error("select failed"); }
        if (!selected) continue;
        if (!stopping && FD_ISSET(listener.value, &reads) && clients.size() < options.max_clients) {
            Socket accepted = accept(listener.value, nullptr, nullptr);
            if (accepted != invalid_socket) {
#ifndef _WIN32
                if (accepted >= FD_SETSIZE) { close_socket(accepted); continue; }
#endif
                auto client = std::make_unique<Connection>(accepted);
                nonblocking(accepted);
                queue_output(*client, "{\"type\":\"HELLO\",\"protocol\":\"apexmatch-json-lines-v1\"}\n", options.max_output);
                clients.push_back(std::move(client));
            } else if (!would_block(socket_error()) && !interrupted(socket_error())) throw std::runtime_error("accept failed");
        }
        for (auto& pointer : clients) {
            auto& client = *pointer;
            if (!client.dead && FD_ISSET(client.socket.value, &writes) && client.pending()) {
                const auto count = static_cast<int>(std::min<std::size_t>(client.pending(), 65536));
                const int written = transmit(client.socket.value, client.output.data() + client.sent, count);
                if (written > 0) { client.sent += static_cast<std::size_t>(written); if (!client.pending()) { client.output.clear(); client.sent = 0; } }
                else if (written == 0 || (!would_block(socket_error()) && !interrupted(socket_error()))) client.dead = true;
            }
            if (!client.dead && !client.closing && FD_ISSET(client.socket.value, &reads)) {
                std::array<char, 4096> buffer{};
                // One bounded read per select turn keeps other clients responsive.
                const int received = receive(client.socket.value, buffer.data(), static_cast<int>(buffer.size()));
                if (received > 0) {
                    for (int i = 0; i < received && !client.dead && !client.closing; ++i) {
                        const char ch = buffer[static_cast<std::size_t>(i)];
                        if (ch == '\n') {
                            auto line = std::move(client.input); client.input.clear();
                            process(client, line);
                        } else if (client.input.size() == options.max_line) {
                            ++client.commands;
                            const auto result = engine.reject("line_too_long");
                            for (const auto& event : result.events) queue_output(client, event_json(event) + "\n", options.max_output);
                            queue_output(client, response_end(client.commands), options.max_output);
                            client.input.clear(); client.closing = true;
                        } else client.input.push_back(ch);
                    }
                } else if (received == 0) {
                    if (!client.input.empty()) {
                        ++client.commands;
                        const auto result = engine.reject("unterminated_line");
                        for (const auto& event : result.events) queue_output(client, event_json(event) + "\n", options.max_output);
                        queue_output(client, response_end(client.commands), options.max_output);
                    }
                    client.closing = true;
                } else if (!would_block(socket_error()) && !interrupted(socket_error())) client.dead = true;
            }
        }
    }
    engine.check_invariants();
}

void run_local_client(std::uint16_t port, std::istream& commands, std::ostream& responses) {
    if (!port) throw std::invalid_argument("client port must be positive");
    [[maybe_unused]] SocketRuntime runtime;
    SocketHandle connection(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (connection.value == invalid_socket) throw std::runtime_error("socket creation failed");
#ifdef _WIN32
    DWORD timeout = 5000;
#else
    timeval timeout{}; timeout.tv_sec = 5;
#endif
    setsockopt(connection.value, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(connection.value, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    const auto address = localhost(port);
    if (connect(connection.value, reinterpret_cast<const sockaddr*>(&address), sizeof(address)))
        throw std::runtime_error("localhost connect failed");
    std::string pending;
    const auto read_line = [&]() {
        while (true) {
            const auto end = pending.find('\n');
            if (end != std::string::npos) {
                auto line = pending.substr(0, end); pending.erase(0, end + 1); return line;
            }
            if (pending.size() > 1024 * 1024) throw std::runtime_error("server response line exceeds 1 MiB");
            std::array<char, 4096> buffer{};
            const int count = receive(connection.value, buffer.data(), static_cast<int>(buffer.size()));
            if (count > 0) pending.append(buffer.data(), static_cast<std::size_t>(count));
            else if (count < 0 && interrupted(socket_error())) continue;
            else throw std::runtime_error(count == 0 ? "server closed before response completed" : "server receive failed or timed out");
        }
    };
    responses << read_line() << '\n'; // HELLO is framed independently of commands.
    std::string command;
    while (std::getline(commands, command)) {
        if (command.size() > 4096) throw std::runtime_error("client command exceeds 4096 bytes");
        command += '\n';
        std::size_t offset{};
        while (offset < command.size()) {
            const int written = transmit(connection.value, command.data() + offset, static_cast<int>(command.size() - offset));
            if (written > 0) offset += static_cast<std::size_t>(written);
            else if (written < 0 && interrupted(socket_error())) continue;
            else throw std::runtime_error("client send failed or timed out");
        }
        while (true) {
            const auto line = read_line();
            responses << line << '\n';
            if (line.find("\"type\":\"RESPONSE_END\"") != std::string::npos) break;
        }
        responses.flush();
        if (!responses) throw std::runtime_error("client response output failed");
        if (command == "SHUTDOWN\n" || command == "SHUTDOWN\r\n") break;
    }
    if (commands.bad()) throw std::runtime_error("client command input failed");
}
} // namespace apexmatch
