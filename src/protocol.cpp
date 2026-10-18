#include "apexmatch/protocol.hpp"
#include <charconv>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace apexmatch {
namespace {
bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
template<class T> bool integer(std::string_view token, T& value) {
    if (token.empty() || token.front() == '+') return false;
    auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
    return error == std::errc{} && end == token.data() + token.size();
}
std::string quote(std::string_view text) {
    const char* hex = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32 || c >= 127) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += static_cast<char>(c);
    }
    out += '"'; return out;
}
void levels_json(std::ostream& out, const std::vector<DepthLevel>& levels) {
    out << '[';
    bool first = true;
    for (const auto& level : levels) {
        if (!first) out << ',';
        first = false;
        out << "{\"price\":" << level.price << ",\"quantity\":" << level.quantity << ",\"orders\":" << level.orders << '}';
    }
    out << ']';
}
}
Command parse_command(std::string_view line) {
    Command command;
    if (line.size() > 4096) { command.error = "line_too_long"; return command; }
    std::vector<std::string_view> tokens;
    std::size_t position = 0;
    while (position < line.size()) {
        while (position < line.size() && space(line[position])) ++position;
        if (position == line.size()) break;
        auto start = position;
        while (position < line.size() && !space(line[position])) ++position;
        tokens.push_back(line.substr(start, position - start));
        if (tokens.size() > 5) break;
    }
    if (tokens.empty() || tokens.front().starts_with('#')) { command.kind = CommandKind::Ignore; return command; }
    command.error = "malformed_command";
    if (tokens[0] == "ADD" && tokens.size() == 5) {
        if (!integer(tokens[1], command.id) || !integer(tokens[3], command.price) || !integer(tokens[4], command.quantity)) {
            command.error = "invalid_integer"; return command;
        }
        if (tokens[2] == "BUY") command.side = Side::Buy;
        else if (tokens[2] == "SELL") command.side = Side::Sell;
        else { command.error = "invalid_side"; return command; }
        command.kind = CommandKind::Add;
    } else if (tokens[0] == "CANCEL" && tokens.size() == 2) {
        if (!integer(tokens[1], command.id)) { command.error = "invalid_integer"; return command; }
        command.kind = CommandKind::Cancel;
    } else if (tokens.size() == 1 && tokens[0] == "BOOK") command.kind = CommandKind::Book;
    else if (tokens.size() == 1 && tokens[0] == "STATS") command.kind = CommandKind::Stats;
    if (command.kind != CommandKind::Invalid) command.error.clear();
    return command;
}
ProcessResult apply_command(MatchingEngine& engine, const Command& command) {
    switch (command.kind) {
    case CommandKind::Add: return engine.add(command.id, command.side, command.price, command.quantity);
    case CommandKind::Cancel: return engine.cancel(command.id);
    case CommandKind::Invalid: return engine.reject(command.error);
    case CommandKind::Book: case CommandKind::Stats: case CommandKind::Ignore: return {};
    }
    throw std::invalid_argument("invalid command kind");
}
std::string process_line(MatchingEngine& engine, std::string_view line, std::size_t depth) {
    auto command = parse_command(line);
    if (command.kind == CommandKind::Ignore) return {};
    if (command.kind == CommandKind::Book) return depth_json(engine.book().depth(depth)) + '\n';
    if (command.kind == CommandKind::Stats) return stats_json(engine.stats(), engine.book().active_orders()) + '\n';
    auto result = apply_command(engine, command);
    std::string output;
    for (const auto& event : result.events) { output += event_json(event); output += '\n'; }
    return output;
}
std::string event_json(const OrderEvent& event) {
    static constexpr const char* names[]{"accepted", "rejected", "execution", "cancelled", "book_update"};
    const auto kind = static_cast<unsigned>(event.kind);
    if (kind >= sizeof(names) / sizeof(names[0])) throw std::invalid_argument("invalid event kind");
    std::ostringstream out; out.imbue(std::locale::classic());
    out << "{\"type\":\"" << names[kind] << "\",\"request_sequence\":" << event.request_sequence
        << ",\"event_sequence\":" << event.event_sequence;
    switch (event.kind) {
    case EventKind::Accepted: case EventKind::Cancelled:
        out << ",\"order_id\":" << event.order_id << ",\"side\":\"" << side_name(event.side) << "\",\"price\":" << event.price << ",\"quantity\":" << event.quantity; break;
    case EventKind::Rejected: out << ",\"order_id\":" << event.order_id << ",\"reason\":" << quote(event.reason); break;
    case EventKind::Execution:
        out << ",\"execution_sequence\":" << event.trade.execution_sequence << ",\"buy_id\":" << event.trade.buy_id
            << ",\"sell_id\":" << event.trade.sell_id << ",\"price\":" << event.trade.price << ",\"quantity\":" << event.trade.quantity; break;
    case EventKind::BookUpdate:
        out << ",\"side\":\"" << side_name(event.side) << "\",\"price\":" << event.price << ",\"quantity\":" << event.level_quantity << ",\"orders\":" << event.level_orders; break;
    }
    out << '}'; return out.str();
}
std::string depth_json(const Depth& depth) {
    std::ostringstream out; out.imbue(std::locale::classic());
    out << "{\"type\":\"book\",\"bids\":"; levels_json(out, depth.bids);
    out << ",\"asks\":"; levels_json(out, depth.asks); out << '}'; return out.str();
}
std::string stats_json(const Statistics& stats, std::size_t active_orders) {
    std::ostringstream out; out.imbue(std::locale::classic());
    out << "{\"type\":\"stats\",\"requests\":" << stats.requests << ",\"accepted\":" << stats.accepted
        << ",\"rejected\":" << stats.rejected << ",\"cancelled_orders\":" << stats.cancelled_orders << ",\"executions\":" << stats.executions
        << ",\"submitted_quantity\":" << stats.submitted_quantity << ",\"traded_quantity\":" << stats.traded_quantity
        << ",\"cancelled_quantity\":" << stats.cancelled_quantity << ",\"resting_quantity\":" << stats.resting_quantity
        << ",\"active_orders\":" << active_orders << '}'; return out.str();
}
} // namespace apexmatch
