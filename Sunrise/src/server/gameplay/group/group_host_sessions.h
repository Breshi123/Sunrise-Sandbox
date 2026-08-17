#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::server::gameplay::group {

/** Region of a caller that knows none. Such a call keeps the region already on the row. */
inline constexpr std::int32_t kUnknownRegion = -1;

/** One region's advertised group session and the activity host session it holds. */
struct HostSessionRow {
    std::uint64_t groupSessionId{};
    std::uint64_t hostSessionId{};
    std::int32_t regionIndex{};
};

/**
 * Reports the activity host session already held for one region, without claiming a slot.
 * @param groupSessionId Group session the region advertises.
 * @return The host session id, or the absent id when none is held yet.
 */
[[nodiscard]] std::uint64_t held_host_session(std::uint64_t groupSessionId) noexcept;

/** Copies every occupied host-session row. @param count Receives the copied row count. */
void snapshot_host_sessions(std::span<HostSessionRow> output, std::size_t& count) noexcept;

/**
 * Fills every claimed host-session slot that has no session yet, and frees every evicted session.
 * The allocation advances the state revision, so it must never run inside a staged push. That
 * push would fail its own revision guard. Callers hold no lock.
 */
void allocate_claimed_host_sessions() noexcept;

/** Returns every held host session to State and clears the table. */
void reset_host_sessions() noexcept;

} // namespace sunrise::server::gameplay::group
