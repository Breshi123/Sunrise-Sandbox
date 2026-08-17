#include "group_host_sessions.h"

#include <Windows.h>

#include <array>

#include "../../../state/activity/runtime.h"
#include "../gameplay_log.h"
#include "group_host.h"

namespace sunrise::server::gameplay::group {

namespace {

/** One region's activity host session, keyed by the group session that region advertises. */
struct HostSession {
    std::uint64_t groupSessionId{};
    std::uint64_t hostSessionId{};
    std::uint64_t lastUse{};
    /** Region the advertisement named. The interface reads it; no lookup uses it. */
    std::int32_t regionIndex{};
    bool occupied{};
};

/** Regions that may hold an activity host session at once. */
constexpr std::size_t kHostSessionCapacity = 8;
/** Guards the host-session table. It is never held across a State allocation. */
SRWLOCK g_hostSessionLock{SRWLOCK_INIT};
/**
 * Activity host sessions this host advertises, one per region.
 * The session id is the peer's only routing key. See `activity_host_session` in `group_host.h`.
 */
std::array<HostSession, kHostSessionCapacity> g_hostSessions{};
/** Rises on every lookup, so the least recently named region is the one an eviction takes. */
std::uint64_t g_useStamp = 0;
/** Sessions an eviction took the slot from. The service slice frees them. */
std::array<std::uint64_t, kHostSessionCapacity> g_evicted{};
std::size_t g_evictedCount = 0;

/**
 * Names one region in the table, taking a slot when it holds none. The caller holds the lock.
 * @param groupSessionId Group session the region advertises.
 * @param regionIndex Region the caller is advertising, kept on the row.
 * @param held Receives the session the region already holds, or the absent id.
 * @return True when a slot names the region afterwards.
 */
[[nodiscard]] bool
claim_locked(std::uint64_t groupSessionId, std::int32_t regionIndex, std::uint64_t& held) noexcept {
    held = state::activity::kAbsentSessionId;
    for (HostSession& entry : g_hostSessions) {
        if (entry.occupied && entry.groupSessionId == groupSessionId) {
            entry.lastUse = ++g_useStamp;
            // A caller with no region keeps the one the advertisement recorded.
            if (regionIndex != kUnknownRegion) {
                entry.regionIndex = regionIndex;
            }
            held = entry.hostSessionId;
            return true;
        }
    }
    for (HostSession& entry : g_hostSessions) {
        if (!entry.occupied) {
            entry = {
                groupSessionId, state::activity::kAbsentSessionId, ++g_useStamp, regionIndex, true};
            return true;
        }
    }
    if (g_evictedCount == g_evicted.size()) {
        return false;
    }
    // The evicted session is freed by the service slice, not here: this runs inside a staged push
    // and the release advances the state revision that push is committing against.
    std::size_t oldest = 0;
    for (std::size_t index = 1; index < g_hostSessions.size(); ++index) {
        if (g_hostSessions[index].lastUse < g_hostSessions[oldest].lastUse) {
            oldest = index;
        }
    }
    if (g_hostSessions[oldest].hostSessionId != state::activity::kAbsentSessionId) {
        g_evicted[g_evictedCount] = g_hostSessions[oldest].hostSessionId;
        ++g_evictedCount;
    }
    g_hostSessions[oldest] = {
        groupSessionId, state::activity::kAbsentSessionId, ++g_useStamp, regionIndex, true};
    return true;
}

/** Frees every session an eviction took a slot from. Callers hold no lock. */
void free_evicted_host_sessions() noexcept {
    std::array<std::uint64_t, kHostSessionCapacity> freed{};
    std::size_t count = 0;
    AcquireSRWLockExclusive(&g_hostSessionLock);
    freed = g_evicted;
    count = g_evictedCount;
    g_evicted = {};
    g_evictedCount = 0;
    ReleaseSRWLockExclusive(&g_hostSessionLock);
    for (std::size_t index = 0; index < count; ++index) {
        const bool released = state::activity::release_session(freed[index]);
        report(core::log::Level::info,
               "ev=gameplay stage=activityhost result=evicted session=0x%llX state=%s",
               static_cast<unsigned long long>(freed[index]),
               released ? "freed" : "absent");
    }
}

} // namespace

/** Reports the activity host session already held for one region, without claiming a slot. */
std::uint64_t held_host_session(std::uint64_t groupSessionId) noexcept {
    std::uint64_t held = state::activity::kAbsentSessionId;
    AcquireSRWLockShared(&g_hostSessionLock);
    for (const HostSession& entry : g_hostSessions) {
        if (entry.occupied && entry.groupSessionId == groupSessionId) {
            held = entry.hostSessionId;
            break;
        }
    }
    ReleaseSRWLockShared(&g_hostSessionLock);
    return held;
}

/** Copies every occupied host-session row. */
void snapshot_host_sessions(std::span<HostSessionRow> output, std::size_t& count) noexcept {
    count = 0;
    AcquireSRWLockShared(&g_hostSessionLock);
    for (const HostSession& entry : g_hostSessions) {
        if (!entry.occupied || count >= output.size()) {
            continue;
        }
        output[count] = {entry.groupSessionId, entry.hostSessionId, entry.regionIndex};
        ++count;
    }
    ReleaseSRWLockShared(&g_hostSessionLock);
}

/** Reports one region's activity session, asking the gameplay slice to allocate a missing one. */
std::uint64_t activity_host_session(std::uint64_t groupSessionId,
                                    std::int32_t regionIndex) noexcept {
    bool claimed = false;
    return activity_host_session(groupSessionId, regionIndex, claimed);
}

/** Reports one region's activity session and whether it holds a slot at all. */
std::uint64_t activity_host_session(std::uint64_t groupSessionId,
                                    std::int32_t regionIndex,
                                    bool& claimedSlot) noexcept {
    claimedSlot = false;
    if (groupSessionId == 0) {
        return state::activity::kAbsentSessionId;
    }
    // Never allocated here. The allocation advances the state revision and would fail the guard
    // of the push that called in. The slot is claimed now and `service` fills it for the next one.
    std::uint64_t held = state::activity::kAbsentSessionId;
    AcquireSRWLockExclusive(&g_hostSessionLock);
    const bool claimed = claim_locked(groupSessionId, regionIndex, held);
    ReleaseSRWLockExclusive(&g_hostSessionLock);
    claimedSlot = claimed;
    if (!claimed) {
        // No slot, so nothing will ever fill one. A caller waiting on this has to publish without
        // it rather than hold for an allocation that is not coming.
        report(core::log::Level::warn, "ev=gameplay stage=activityhost result=full");
    }
    return held;
}

/** Fills every claimed host-session slot that has no session yet. */
void allocate_claimed_host_sessions() noexcept {
    free_evicted_host_sessions();
    for (;;) {
        std::uint64_t groupSessionId = 0;
        AcquireSRWLockShared(&g_hostSessionLock);
        for (const HostSession& entry : g_hostSessions) {
            if (entry.occupied && entry.hostSessionId == state::activity::kAbsentSessionId) {
                groupSessionId = entry.groupSessionId;
                break;
            }
        }
        ReleaseSRWLockShared(&g_hostSessionLock);
        if (groupSessionId == 0) {
            return;
        }
        // Outside the table lock: the allocation takes the State lock and the two may not nest.
        std::uint64_t sessionId = state::activity::kAbsentSessionId;
        state::activity::PendingAllocation allocation{};
        if (!state::activity::prepare_session(sessionId, allocation)
            || !state::activity::commit(allocation)) {
            // The account half is not loaded yet on an early slice, so this retries next slice.
            return;
        }
        bool stored = false;
        std::size_t occupied = 0;
        AcquireSRWLockExclusive(&g_hostSessionLock);
        for (HostSession& entry : g_hostSessions) {
            if (!entry.occupied) {
                continue;
            }
            ++occupied;
            if (entry.groupSessionId == groupSessionId
                && entry.hostSessionId == state::activity::kAbsentSessionId) {
                entry.hostSessionId = sessionId;
                stored = true;
            }
        }
        ReleaseSRWLockExclusive(&g_hostSessionLock);
        if (!stored) {
            // The slot was evicted while this was allocating, so the session is spent.
            static_cast<void>(state::activity::release_session(sessionId));
            return;
        }
        report(core::log::Level::info,
               "ev=gameplay stage=activityhost result=allocated session=0x%llX group=0x%016llX "
               "held=%zu",
               static_cast<unsigned long long>(sessionId),
               static_cast<unsigned long long>(groupSessionId),
               occupied);
    }
}

/** Returns every held host session to State and clears the table. */
void reset_host_sessions() noexcept {
    // Dropping the table alone would strand its records in State, and nothing else can name them
    // once their group session is forgotten.
    std::array<std::uint64_t, kHostSessionCapacity> held{};
    std::size_t count = 0;
    AcquireSRWLockExclusive(&g_hostSessionLock);
    for (const HostSession& entry : g_hostSessions) {
        if (entry.occupied) {
            held[count] = entry.hostSessionId;
            ++count;
        }
    }
    g_hostSessions = {};
    ReleaseSRWLockExclusive(&g_hostSessionLock);
    for (std::size_t index = 0; index < count; ++index) {
        static_cast<void>(state::activity::release_session(held[index]));
    }
    free_evicted_host_sessions();
}

} // namespace sunrise::server::gameplay::group
