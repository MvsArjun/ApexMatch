#pragma once
#include "apexmatch/engine.hpp"
#include <string_view>
namespace apexmatch {
enum class CommandKind { Add, Cancel, Book, Stats, Ignore, Invalid };
struct Command { CommandKind kind{CommandKind::Invalid}; OrderId id{}; Side side{}; Price price{}; Quantity quantity{}; std::string error; };
Command parse_command(std::string_view line);
// Unknown typed CommandKind values throw invalid_argument without changing engine state.
ProcessResult apply_command(MatchingEngine& engine, const Command& command);
// One newline-terminated JSON object per event; queries produce one object.
std::string process_line(MatchingEngine& engine, std::string_view line, std::size_t depth = 10);
} // namespace apexmatch
