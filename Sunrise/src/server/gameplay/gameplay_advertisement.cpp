#include "gameplay_advertisement.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "../../middleware/gameplay/descriptor/join_descriptor.h"
#include "../../state/activity/definition.h"
#include "endpoint/gameplay_endpoint.h"
#include "gameplay_log.h"
#include "group/group_host.h"

namespace sunrise::server::gameplay {

namespace {

namespace message = middleware::bap::activity_message::replicate_membership;

/** Member-slot fields are 6 bits at bias 1, so the slot itself cannot exceed 62. */
constexpr std::uint8_t kMaximumMemberSlot = 62;
/** Slot a readiness query stands in with. Every publisher names the one member it publishes. */
constexpr std::uint8_t kQueriedMemberSlot = 0;
/** Odd multiplier that spreads one region index across the whole 64-bit space. */
constexpr std::uint64_t kRegionStride = 0x9E3779B97F4A7C15ULL;

/**
 * Derives one region's copy of an identity field.
 * It is derived rather than allocated so every push for a region names the same value with no
 * table.
 * @param base Whole-process identity field.
 * @param regionIndex Region the record belongs to.
 * @return Nonzero value for that region.
 */
[[nodiscard]] std::uint64_t region_identity(std::uint64_t base, std::int32_t regionIndex) noexcept {
    const auto region = static_cast<std::uint64_t>(static_cast<std::uint32_t>(regionIndex));
    const std::uint64_t derived = base ^ (kRegionStride * (region + 1U));
    // The descriptor refuses a zero machine id and a zero session id alike.
    return derived == 0 ? kRegionStride : derived;
}

/**
 * Derives the descriptor machine id one region advertises.
 * It is also the key of that region's activity host session, so a readiness query has to derive it
 * the same way the advertisement does.
 * @param regionIndex Region the record belongs to.
 * @return The region's machine id.
 */
[[nodiscard]] std::uint64_t region_machine_id(std::int32_t regionIndex) noexcept {
    return region_identity(endpoint::identity().machineId, regionIndex);
}

/** No outcome packs to this, so the first advertisement always reports. */
constexpr std::uint64_t kNoOutcome = ~0ULL;
/** Ambassador slot sits above the region index, which holds the low 32 bits. */
constexpr unsigned kSlotShift = 32;
/** Skip reason sits above the ambassador slot. */
constexpr unsigned kReasonShift = 40;
/** Region source sits above the skip reason. */
constexpr unsigned kSourceShift = 48;

/** Why an advertisement was not built. Reported so a silent skip cannot look like a send. */
enum class Skip : std::uint64_t {
    none,
    notReady,
    noRegion,
    slotRange,
    descriptor,
    noHostSession,
    hostSessionFull
};

std::atomic<std::uint64_t> g_reported{kNoOutcome};

/** Log names for RegionSource, in its declaration order. */
constexpr const char* kSources[] = {"reported", "arrival"};

/**
 * Reports one advertisement outcome, and only when it differs from the last.
 * A membership push runs every few seconds, so an unchanged outcome must stay silent.
 * @param skip Check that refused the build, or Skip::none.
 * @param regionIndex Region the outcome belongs to.
 * @param regionSource Where that index came from.
 * @param ambassadorSlot Slot the advertisement named, or zero when it did not build.
 */
void report_outcome(Skip skip,
                    std::int32_t regionIndex,
                    RegionSource regionSource,
                    std::uint8_t ambassadorSlot) noexcept {
    const auto outcome = static_cast<std::uint64_t>(static_cast<std::uint32_t>(regionIndex))
                         | (static_cast<std::uint64_t>(ambassadorSlot) << kSlotShift)
                         | (static_cast<std::uint64_t>(skip) << kReasonShift)
                         | (static_cast<std::uint64_t>(regionSource) << kSourceShift);
    if (g_reported.exchange(outcome, std::memory_order_relaxed) == outcome) {
        return;
    }
    const char* source = kSources[static_cast<std::size_t>(regionSource)];
    if (skip == Skip::none) {
        report(core::log::Level::info,
               "ev=gameplay stage=advertise result=ok region=%d region_source=%s "
               "ambassador_slot=%u",
               static_cast<int>(regionIndex),
               source,
               static_cast<unsigned>(ambassadorSlot));
        return;
    }
    // Log names for Skip, in its declaration order.
    static constexpr const char* kReasons[] = {"none",
                                               "not_ready",
                                               "no_region",
                                               "slot_range",
                                               "descriptor",
                                               "no_host_session",
                                               "host_session_full"};
    report(core::log::Level::info,
           "ev=gameplay stage=advertise result=skip reason=%s region=%d region_source=%s",
           kReasons[static_cast<std::size_t>(skip)],
           static_cast<int>(regionIndex),
           source);
}

/**
 * Picks an ambassador slot that is not the joining client's own.
 * An equal slot sends the peer into the local-ambassador stage, and it never reaches the citizen
 * join.
 * @param localMemberSlot Slot the joining client occupies.
 * @return A different, encodable member slot.
 */
[[nodiscard]] std::uint8_t ambassador_slot(std::uint8_t localMemberSlot) noexcept {
    return localMemberSlot == 0 ? 1 : 0;
}

/**
 * Builds one region's advertisement, or names the first check that refused it.
 * Every caller runs this same body, so a readiness query and the advertisement itself can never
 * disagree about whether a descriptor is coming.
 * @param regionIndex Region the record belongs to.
 * @param localMemberSlot Member slot of the joining client.
 * @param candidate Cleared, then filled when the whole advertisement builds.
 * @return Skip::none when it built, otherwise the check that refused.
 */
[[nodiscard]] Skip build_candidate(std::int32_t regionIndex,
                                   std::uint8_t localMemberSlot,
                                   message::CitizenAdvertisement& candidate) noexcept {
    candidate = {};
    if (!endpoint::ready()) {
        return Skip::notReady;
    }
    if (regionIndex < 0) {
        return Skip::noRegion;
    }
    if (localMemberSlot > kMaximumMemberSlot) {
        return Skip::slotRange;
    }
    const endpoint::Identity identity = endpoint::identity();
    const state::gameplay::Endpoint advertised = endpoint::advertised();
    middleware::gameplay::descriptor::JoinEndpoint join{};
    join.address = advertised.address;
    join.port = advertised.port;
    // The client keys its managed sessions by the descriptor's machine id, not by its session id.
    // Two regions may never share one.
    join.machineId = region_machine_id(regionIndex);
    join.onlineSessionId = region_identity(identity.onlineSessionId, regionIndex);
    // The region compares this against the `activity-host` parameter and errors out with
    // `public_activity_host_mismatch` when they disagree, so both carry the same allocated id.
    // The key is the machine id, because that is what the peer echoes in every later message.
    bool claimedSlot = false;
    const std::uint64_t hostSession =
        group::activity_host_session(join.machineId, regionIndex, claimedSlot);
    if (hostSession == state::activity::kAbsentSessionId) {
        // A claimed slot is filled by the next service slice. An unclaimed one never will be, so
        // the two must not read the same to a caller deciding whether to wait.
        return claimedSlot ? Skip::noHostSession : Skip::hostSessionFull;
    }
    if (!middleware::gameplay::descriptor::build(join, candidate.descriptor)) {
        candidate = {};
        return Skip::descriptor;
    }
    candidate.onlineSessionId = hostSession;
    candidate.regionIndex = regionIndex;
    candidate.ambassadorSlot = ambassador_slot(localMemberSlot);
    candidate.present = true;
    return Skip::none;
}

} // namespace

/** Builds the citizen advertisement for one region record. */
void build_advertisement(std::int32_t regionIndex,
                         RegionSource regionSource,
                         std::uint8_t localMemberSlot,
                         message::CitizenAdvertisement& output) noexcept {
    const Skip skip = build_candidate(regionIndex, localMemberSlot, output);
    report_outcome(skip, regionIndex, regionSource, output.ambassadorSlot);
}

/** Reports whether one region's advertisement can be built now. */
AdvertisementState advertisement_state(std::int32_t regionIndex) noexcept {
    message::CitizenAdvertisement candidate{};
    // The same body the advertisement runs, so it cannot promise a descriptor the build refuses.
    // It also claims the region's host-session slot, so the service slice allocates one.
    switch (build_candidate(regionIndex, kQueriedMemberSlot, candidate)) {
    case Skip::none:
        return AdvertisementState::ready;
    case Skip::noHostSession:
        return AdvertisementState::pending;
    default:
        return AdvertisementState::absent;
    }
}

} // namespace sunrise::server::gameplay
