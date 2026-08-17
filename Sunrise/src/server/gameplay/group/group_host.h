#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../middleware/encoding/bit_reader.h"
#include "../../../state/gameplay/definition.h"

namespace sunrise::server::gameplay::group {

/**
 * Consumes one group-session message.
 * True means the whole body was read and the container may continue. False means the reader is
 * left part way through the body, so the caller must stop.
 * @param from Peer endpoint in host order.
 * @param sessionId Fallback for a message that names no session, or zero when the link carries
 *                  several. Every session-scoped message names its own and uses that instead.
 * @param id Registry message id.
 * @param reader Reader positioned at the message body.
 * @param now Monotonic tick count in milliseconds.
 * @return True when the body was decoded and the container may continue.
 */
[[nodiscard]] bool consume(const state::gameplay::Endpoint& from,
                           std::uint64_t sessionId,
                           std::uint8_t id,
                           middleware::encoding::bits::Reader& reader,
                           std::uint64_t now) noexcept;

/**
 * Publishes the membership snapshot that completes one peer's join.
 * It names the host and the peer, because the peer looks itself up by address. The peer accepts
 * it only while the state replica behind its hash is byte exact.
 * @param peer Endpoint of the admitted peer, in host order.
 * @param peerJoinId Join id the peer's join request carried. Its entry has to echo it.
 * @param sessionId Group-session id the peer named, which its parameter updates have to echo.
 * @return True when the snapshot was queued on the peer's reliable channel.
 */
[[nodiscard]] bool publish_membership(const state::gameplay::Endpoint& peer,
                                      std::uint64_t peerJoinId,
                                      std::uint64_t sessionId) noexcept;

/**
 * Reports one region's activity host session, allocating it on first use.
 * One id per region, because the peer routes every activity push by it alone. The id must not
 * change while the region keeps its slot, or the peer errors `public_activity_host_mismatch`.
 * @param groupSessionId Session the peer names for this region, which is the descriptor's machine
 *        id and not its session id.
 * @param regionIndex Region being advertised, or kUnknownRegion.
 * @return Allocated session id, or zero while State cannot allocate one yet.
 */
[[nodiscard]] std::uint64_t activity_host_session(std::uint64_t groupSessionId,
                                                  std::int32_t regionIndex) noexcept;

/**
 * Reports one region's activity session and whether it holds a table slot at all.
 * A caller that waits for the session needs the difference. Without a slot nothing will ever
 * allocate one, so waiting never ends.
 * @param groupSessionId Session the peer names for this region.
 * @param regionIndex Region being advertised, or kUnknownRegion.
 * @param claimedSlot Set when a slot names this region, with or without a session in it yet.
 * @return Allocated session id, or zero while the slot has none.
 */
[[nodiscard]] std::uint64_t activity_host_session(std::uint64_t groupSessionId,
                                                  std::int32_t regionIndex,
                                                  bool& claimedSlot) noexcept;

/** One admitted peer's group session and how far its join has got. */
struct AdmittedRow {
    std::uint64_t sessionId{};
    state::gameplay::Endpoint endpoint{};
    /** Set once the peer reported its join finished. */
    bool joinComplete{};
    /** Set once the `activity-host` parameter is on the peer's reliable channel. */
    bool activityHostPublished{};
    /** Set once a snapshot naming the peer's player is on that channel. */
    bool playerPublished{};
};

/** Copies every admitted group-session record. @param count Receives the copied row count. */
void snapshot_admitted(std::span<AdmittedRow> output, std::size_t& count) noexcept;

/**
 * Retries any publish the reliable queue refused, on a timer.
 * The queue is fullest while the join promotion and the `activity-host` parameter are owed, so
 * both track what was queued and not what was asked for.
 * @param now Monotonic tick count in milliseconds.
 */
void service(std::uint64_t now) noexcept;

/**
 * Publishes the parameter update a joining peer needs before it will finish its join.
 * The peer's own tick refuses to complete until it has applied one, whatever the update names.
 * @param sessionId Group-session id the peer named in its join request.
 * @return True when the update was queued on the peer's reliable channel.
 */
[[nodiscard]] bool publish_join_parameters(std::uint64_t sessionId) noexcept;

/**
 * Reports whether replication may produce entity output for one peer.
 * View establishment is a hard gate: a mismatched signature means no entity output at all.
 * @param sessionId Group session the link carries.
 * @return True only once a compatible view is bound.
 */
[[nodiscard]] bool view_accepted(std::uint64_t sessionId) noexcept;

/**
 * Frees every admitted record at one endpoint.
 * Only an association timeout calls this. A connect-closed must not: the client rebuilds its
 * channel within 50 ms and keeps every group session it held.
 * @param endpoint Peer endpoint in host order, whose links the caller is already dropping.
 */
void release_endpoint(const state::gameplay::Endpoint& endpoint) noexcept;

/** Clears every group-session record. */
void reset() noexcept;

} // namespace sunrise::server::gameplay::group
