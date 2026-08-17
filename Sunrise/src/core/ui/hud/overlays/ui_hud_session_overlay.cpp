/**
 * The session overlay. One row per instance the player is in.
 * A row names the region, its group session, its activity host, its channel and its join.
 */

#include "ui_hud_session_overlay.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <imgui.h>

#include "../../../../middleware/content/packages/tables/region_reader.h"
#include "../../../../server/gameplay/group/group_host.h"
#include "../../../../server/gameplay/group/group_host_sessions.h"
#include "../../../../server/gameplay/peer/peer_transport.h"
#include "../../../../state/gameplay/definition.h"

namespace sunrise::core::ui::hud::overlays::session {
namespace {

namespace group = server::gameplay::group;
namespace peer = server::gameplay::peer;
namespace tables = middleware::content::packages::tables;

/** Instances one snapshot reads. The host table holds no more than this. */
constexpr std::size_t kRowCapacity = 8;
/** Admitted peers one snapshot reads. */
constexpr std::size_t kAdmittedCapacity = 8;
/** Columns of the instance table, in draw order. */
constexpr int kColumnCount = 6;
/** Shown while this host serves no instance. */
constexpr char kNoInstance[] = "no session instances";
/** Shown for a row no advertisement gave a region. */
constexpr char kNoBubble[] = "-";

/** Channel stage names, in PeerStage order. */
constexpr std::array<const char*, 6> kChannelStages{
    "absent", "allocated", "teardown", "connecting", "establishing", "connected"};

/** @param stage Link stage. @return Its name, or the absent one for a value out of range. */
[[nodiscard]] const char* channel_name(state::gameplay::PeerStage stage) noexcept {
    const auto index = static_cast<std::size_t>(stage);
    return index < kChannelStages.size() ? kChannelStages[index] : kChannelStages[0];
}

/** Names how far one join has got. @param count Admitted rows in use. @return The join word. */
[[nodiscard]] const char*
join_name(const std::array<group::AdmittedRow, kAdmittedCapacity>& admitted,
          std::size_t count,
          std::uint64_t sessionId) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        const group::AdmittedRow& row = admitted[index];
        if (row.sessionId != sessionId) {
            continue;
        }
        if (!row.joinComplete) {
            return "joining";
        }
        return row.activityHostPublished ? "ready" : "joined";
    }
    return "unjoined";
}

/** Draws one cell of hexadecimal identity. @param value Session id, or zero when there is none. */
void draw_session_cell(std::uint64_t value) noexcept {
    if (value == 0) {
        ImGui::TextDisabled("pending");
        return;
    }
    ImGui::Text("%016llX", static_cast<unsigned long long>(value));
}

/** Draws the region and bubble cells. @param region Region the advertisement named, or -1. */
void draw_region_cells(std::int32_t region) noexcept {
    if (region < 0) {
        ImGui::TextDisabled("%s", kNoBubble);
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", kNoBubble);
        return;
    }
    ImGui::Text("%d", static_cast<int>(region));
    ImGui::TableNextColumn();
    ImGui::Text("%u", static_cast<unsigned>(region) / tables::kSliceSetIndexFactor);
}

} // namespace

/** Draws the session instance table inside the overlay window the stack has already started. */
void draw() noexcept {
    std::array<group::HostSessionRow, kRowCapacity> rows{};
    std::size_t rowCount = 0;
    group::snapshot_host_sessions(rows, rowCount);
    // A host session outlives the instance on purpose: the peer may rotate back into a region it
    // never left, and a fresh id there is a hard error. So the link is what says it is still live.
    std::array<state::gameplay::PeerStage, kRowCapacity> stages{};
    std::size_t live = 0;
    for (std::size_t index = 0; index < rowCount; ++index) {
        if (peer::link_stage(rows[index].groupSessionId, stages[live])) {
            rows[live] = rows[index];
            ++live;
        }
    }
    if (live == 0) {
        ImGui::TextDisabled("%s", kNoInstance);
        return;
    }
    std::array<group::AdmittedRow, kAdmittedCapacity> admitted{};
    std::size_t admittedCount = 0;
    group::snapshot_admitted(admitted, admittedCount);

    if (!ImGui::BeginTable("##sunrise_hud_session_table", kColumnCount)) {
        return;
    }
    ImGui::TableSetupColumn("region");
    ImGui::TableSetupColumn("bubble");
    ImGui::TableSetupColumn("group session");
    ImGui::TableSetupColumn("activity host");
    ImGui::TableSetupColumn("channel");
    ImGui::TableSetupColumn("join");
    ImGui::TableHeadersRow();
    for (std::size_t index = 0; index < live; ++index) {
        const group::HostSessionRow& row = rows[index];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        draw_region_cells(row.regionIndex);
        ImGui::TableNextColumn();
        draw_session_cell(row.groupSessionId);
        ImGui::TableNextColumn();
        draw_session_cell(row.hostSessionId);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(channel_name(stages[index]));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(join_name(admitted, admittedCount, row.groupSessionId));
    }
    ImGui::EndTable();
}

} // namespace sunrise::core::ui::hud::overlays::session
