#pragma once

#include <array>
#include <cstdint>

#include "../../inactivity/inactivity_settings_store.h"

namespace sunrise::client::hooks::inactivity {

/** What the Client is holding in its lanes now, read back rather than assumed. */
struct Status {
    std::array<std::uint32_t, client::inactivity::kActivityCount> live{};
    bool liveValid{};
    /** Session grace. Zero stops the Client gating on it at all. */
    std::uint32_t liveGraceMs{};
    bool liveGraceValid{};
};

/** The Client's own two clocks. A lane fires once idle passes its milliseconds. */
struct Timers {
    /** Input resets this, so it does not track the session and the two can diverge widely. */
    std::uint64_t idleMs{};
    std::uint64_t sessionMs{};
    bool idleValid{};
    bool sessionValid{};
};

/**
 * Reads the clocks poll() last sampled and asks it to sample again next frame.
 * A sample enters Client code, so it is taken from poll() and never from a draw.
 * @return The clocks, with a validity flag for each.
 */
[[nodiscard]] Timers timers() noexcept;

/**
 * Resolves the activity config getter, which the lanes are reached through.
 * @return True when it was found.
 */
[[nodiscard]] bool install() noexcept;

/** Puts the Client's own lanes back and drops the resolved getter. */
void uninstall() noexcept;

/**
 * Holds the configured milliseconds, or puts back the ones the Client authored.
 * Enters Client code, so call it once a frame from a tick that holds no lock.
 */
void poll() noexcept;

/** @return A consistent copy of what the override reached. */
[[nodiscard]] Status status() noexcept;

} // namespace sunrise::client::hooks::inactivity
