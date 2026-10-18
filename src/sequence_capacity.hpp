#pragma once
#include "apexmatch/engine.hpp"
#include <limits>

namespace apexmatch::detail {
// Internal pure preflight shared with boundary tests; no mutable test-only engine API.
constexpr bool sequence_capacity_available(Sequence requests, Sequence events, Sequence executions,
                                           std::size_t active_orders) noexcept {
    constexpr auto maximum = std::numeric_limits<Sequence>::max();
    if (requests == maximum || active_orders > (maximum - 3) / 2) return false;
    const auto active = static_cast<Sequence>(active_orders);
    const auto required_events = active * 2 + 3;
    return events <= maximum - required_events && executions <= maximum - active;
}
} // namespace apexmatch::detail
