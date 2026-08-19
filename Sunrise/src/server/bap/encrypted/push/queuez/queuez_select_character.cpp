#include <array>
#include <cstdio>

#include "../../../../../core/logging/log.h"
#include "../../queuez/queuez_state_validation.h"
#include "../snapshot/internal.h"
#include "queuez_push_reporting.h"
#include "queuez_update_frame.h"

namespace sunrise::server::bap::encrypted::push {

/**
 * Appends the opcode-504 Family-4 move as one increment above the peer's current version.
 * @param scratch Lock-owned transform buffers.
 * @param select Staged after-image, object definitions, and both character keys.
 * @param key Active AES-GCM session key.
 * @param nonce Push-direction nonce after the correlated svc-11 response.
 * @param response Caller-owned output containing the existing response prefix.
 * @param written Existing byte count, updated after the complete push is appended.
 * @return True when the 3 object operations and the whole svc-123 frame fit.
 */
bool append_select_character_notification(Scratch& scratch,
                                          const queuez::SelectCharacter& select,
                                          std::span<const std::byte, state::kAesKeySize> key,
                                          std::span<const std::byte, state::kBapNonceSize> nonce,
                                          std::span<std::byte> response,
                                          std::size_t& written) noexcept {
    snapshot::Prepared prepared{};
    if (!snapshot::prepare_selection_move(scratch, select, prepared)) {
        return false;
    }
    const std::size_t objectCount = prepared.family.objects.size();
    const std::size_t beforeBytes = written;
    if (!queuez_frame::append(scratch,
                              prepared.family,
                              prepared.rawClearSize,
                              prepared.compressedClearSize,
                              key,
                              nonce,
                              response,
                              written)) {
        return false;
    }
    queuez_report::push(
        "select", queuez::kAccountFamilyType, objectCount, written - beforeBytes, 1);
    return true;
}

/** Appends the opcode-403 character upsert as one increment above the current peer version. */
bool append_equipment_swap_notification(Scratch& scratch,
                                        const queuez::EquipmentSwap& swap,
                                        const state::PendingEquipmentSwap& mutation,
                                        std::span<const std::byte, state::kAesKeySize> key,
                                        std::span<const std::byte, state::kBapNonceSize> nonce,
                                        std::span<std::byte> response,
                                        std::size_t& written) noexcept {
    snapshot::Prepared prepared{};
    if (!snapshot::prepare_equipment_swap(scratch, swap, mutation, prepared)) {
        return false;
    }
    const std::size_t objectCount = prepared.family.objects.size();
    if (objectCount == 1) {
        const middleware::queuez::Object& object = prepared.family.objects.front();
        std::array<char, core::log::kLineCapacity> line{};
        const int count =
            std::snprintf(line.data(),
                          line.size(),
                          "ev=equip stage=queuez_object result=ok family_version=%d object_id=%u "
                          "object_version=0x%llX encoding=%u payload_bytes=%zu",
                          prepared.family.version,
                          object.id,
                          static_cast<unsigned long long>(object.version),
                          static_cast<unsigned>(object.encoding),
                          object.payload.size());
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::debug,
                             {line.data(), static_cast<std::size_t>(count)});
        }
    }
    const std::size_t beforeBytes = written;
    if (!queuez_frame::append(scratch,
                              prepared.family,
                              prepared.rawClearSize,
                              prepared.compressedClearSize,
                              key,
                              nonce,
                              response,
                              written)) {
        return false;
    }
    queuez_report::push("equip", queuez::kAccountFamilyType, objectCount, written - beforeBytes, 1);
    return true;
}

/** Appends one opcode-406 selected-character item-state upsert. */
bool append_item_state_notification(Scratch& scratch,
                                    const queuez::EquipmentSwap& update,
                                    const state::PendingItemState& mutation,
                                    std::span<const std::byte, state::kAesKeySize> key,
                                    std::span<const std::byte, state::kBapNonceSize> nonce,
                                    std::span<std::byte> response,
                                    std::size_t& written) noexcept {
    snapshot::Prepared prepared{};
    if (!snapshot::prepare_item_state(scratch, update, mutation, prepared)) {
        return false;
    }
    const std::size_t objectCount = prepared.family.objects.size();
    const std::size_t beforeBytes = written;
    if (objectCount != 1 || prepared.family.objects.front().id != update.characterDefinitionId
        || prepared.family.objects.front().version != update.characterSoid
        || prepared.family.objects.front().encoding != middleware::queuez::Encoding::oodle
        || prepared.family.objects.front().payload.empty()
        || !queuez_frame::append(scratch,
                                 prepared.family,
                                 prepared.rawClearSize,
                                 prepared.compressedClearSize,
                                 key,
                                 nonce,
                                 response,
                                 written)) {
        return false;
    }
    queuez_report::push(
        "item_state", queuez::kAccountFamilyType, objectCount, written - beforeBytes, 1);
    return true;
}

/** Appends one socket item upsert and its charged account balances when required. */
bool append_socket_plug_notification(Scratch& scratch,
                                     const queuez::SocketPlug& socketPlug,
                                     const state::PendingSocketPlug& mutation,
                                     std::span<const std::byte, state::kAesKeySize> key,
                                     std::span<const std::byte, state::kBapNonceSize> nonce,
                                     std::span<std::byte> response,
                                     std::size_t& written) noexcept {
    snapshot::Prepared prepared{};
    if (!snapshot::prepare_socket_plug(scratch, socketPlug, mutation, prepared)) {
        return false;
    }
    const std::size_t objectCount = prepared.family.objects.size();
    const std::size_t expectedObjectCount = socketPlug.updatesAccount ? 2U : 1U;
    const std::size_t beforeBytes = written;
    if (objectCount != expectedObjectCount
        || prepared.family.objects.front().id != socketPlug.itemInstanceDefinitionId
        || prepared.family.objects.front().version != socketPlug.targetInstanceSoid
        || prepared.family.objects.front().encoding != middleware::queuez::Encoding::oodle
        || prepared.family.objects.front().payload.empty()
        || (socketPlug.updatesAccount
            && (prepared.family.objects[1].id != socketPlug.accountDefinitionId
                || prepared.family.objects[1].version != socketPlug.accountSoid
                || prepared.family.objects[1].encoding != middleware::queuez::Encoding::oodle
                || prepared.family.objects[1].payload.empty()))
        || !queuez_frame::append(scratch,
                                 prepared.family,
                                 prepared.rawClearSize,
                                 prepared.compressedClearSize,
                                 key,
                                 nonce,
                                 response,
                                 written)) {
        return false;
    }
    queuez_report::push(
        "socket_plug", queuez::kAccountFamilyType, objectCount, written - beforeBytes, 1);
    return true;
}

/** Appends one subclass item-instance upsert after an opcode-801 selection. */
bool append_subclass_selection_notification(Scratch& scratch,
                                            const queuez::SubclassSelection& selection,
                                            const state::PendingSubclassSelection& mutation,
                                            std::span<const std::byte, state::kAesKeySize> key,
                                            std::span<const std::byte, state::kBapNonceSize> nonce,
                                            std::span<std::byte> response,
                                            std::size_t& written) noexcept {
    snapshot::Prepared prepared{};
    if (!snapshot::prepare_subclass_selection(scratch, selection, mutation, prepared)) {
        return false;
    }
    const std::size_t objectCount = prepared.family.objects.size();
    const std::size_t beforeBytes = written;
    if (objectCount != 1 || prepared.family.objects.front().id != selection.itemInstanceDefinitionId
        || prepared.family.objects.front().version != selection.subclassInstanceSoid
        || prepared.family.objects.front().encoding != middleware::queuez::Encoding::oodle
        || prepared.family.objects.front().payload.empty()
        || !queuez_frame::append(scratch,
                                 prepared.family,
                                 prepared.rawClearSize,
                                 prepared.compressedClearSize,
                                 key,
                                 nonce,
                                 response,
                                 written)) {
        return false;
    }
    queuez_report::push(
        "subclass_select", queuez::kAccountFamilyType, objectCount, written - beforeBytes, 1);
    return true;
}

/** Appends one atomic new-instance-before-character Family-4 acquisition update. */
bool append_item_acquisition_notification(Scratch& scratch,
                                          const queuez::ItemAcquisition& acquisition,
                                          const state::PendingItemAcquisition& mutation,
                                          std::span<const std::byte, state::kAesKeySize> key,
                                          std::span<const std::byte, state::kBapNonceSize> nonce,
                                          std::span<std::byte> response,
                                          std::size_t& written) noexcept {
    snapshot::Prepared prepared{};
    if (!snapshot::prepare_item_acquisition(scratch, acquisition, mutation, prepared)) {
        return false;
    }
    const std::size_t objectCount = prepared.family.objects.size();
    const std::size_t beforeBytes = written;
    const std::size_t expectedObjectCount = acquisition.updatesAccount ? 3U : 2U;
    if (objectCount != expectedObjectCount
        || prepared.family.objects[0].id != acquisition.itemInstanceDefinitionId
        || prepared.family.objects[0].version != acquisition.acquiredInstanceSoid
        || prepared.family.objects[0].encoding != middleware::queuez::Encoding::oodle
        || prepared.family.objects[0].payload.empty()
        || prepared.family.objects[1].id != acquisition.characterDefinitionId
        || prepared.family.objects[1].version != acquisition.characterSoid
        || prepared.family.objects[1].encoding != middleware::queuez::Encoding::oodle
        || prepared.family.objects[1].payload.empty()
        || (acquisition.updatesAccount
            && (prepared.family.objects[2].id != acquisition.accountDefinitionId
                || prepared.family.objects[2].version != acquisition.accountSoid
                || prepared.family.objects[2].encoding != middleware::queuez::Encoding::oodle
                || prepared.family.objects[2].payload.empty()))
        || !queuez_frame::append(scratch,
                                 prepared.family,
                                 prepared.rawClearSize,
                                 prepared.compressedClearSize,
                                 key,
                                 nonce,
                                 response,
                                 written)) {
        return false;
    }
    queuez_report::push(
        "acquire", queuez::kAccountFamilyType, objectCount, written - beforeBytes, 1);
    return true;
}

/** Appends an optional new profile resident followed by the full account after-image. */
bool append_profile_item_acquisition_notification(
    Scratch& scratch,
    const queuez::ProfileItemAcquisition& acquisition,
    const state::PendingProfileItemAcquisition& mutation,
    std::span<const std::byte, state::kAesKeySize> key,
    std::span<const std::byte, state::kBapNonceSize> nonce,
    std::span<std::byte> response,
    std::size_t& written) noexcept {
    snapshot::Prepared prepared{};
    if (!snapshot::prepare_profile_item_acquisition(scratch, acquisition, mutation, prepared)) {
        return false;
    }
    const std::size_t objectCount = prepared.family.objects.size();
    const std::size_t beforeBytes = written;
    const std::size_t expectedObjectCount = acquisition.appendedResident ? 2U : 1U;
    const std::size_t accountIndex = acquisition.appendedResident ? 1U : 0U;
    if (prepared.family.type != queuez::kAccountFamilyType
        || prepared.family.rootSoid != acquisition.after.family4RootSoid
        || prepared.family.version != acquisition.after.family4Version || prepared.family.flags != 0
        || objectCount != expectedObjectCount || accountIndex >= objectCount
        || prepared.family.objects[accountIndex].id != acquisition.accountDefinitionId
        || prepared.family.objects[accountIndex].version != acquisition.accountSoid
        || prepared.family.objects[accountIndex].encoding != middleware::queuez::Encoding::oodle
        || prepared.family.objects[accountIndex].payload.empty()
        || (acquisition.appendedResident
            && (prepared.family.objects.front().id != acquisition.itemInstanceDefinitionId
                || prepared.family.objects.front().version != acquisition.acquiredInstanceSoid
                || prepared.family.objects.front().encoding != middleware::queuez::Encoding::oodle
                || prepared.family.objects.front().payload.empty()))
        || !queuez_frame::append(scratch,
                                 prepared.family,
                                 prepared.rawClearSize,
                                 prepared.compressedClearSize,
                                 key,
                                 nonce,
                                 response,
                                 written)) {
        return false;
    }
    queuez_report::push(
        "profile_acquire", queuez::kAccountFamilyType, objectCount, written - beforeBytes, 1);
    return true;
}

/** Appends one atomic dismantle update, including any account-wide material payout. */
bool append_item_dismantle_notification(Scratch& scratch,
                                        const queuez::ItemDismantle& dismantle,
                                        const state::PendingItemDismantle& mutation,
                                        std::span<const std::byte, state::kAesKeySize> key,
                                        std::span<const std::byte, state::kBapNonceSize> nonce,
                                        std::span<std::byte> response,
                                        std::size_t& written) noexcept {
    snapshot::Prepared prepared{};
    if (!snapshot::prepare_item_dismantle(scratch, dismantle, mutation, prepared)) {
        return false;
    }
    const std::size_t objectCount = prepared.family.objects.size();
    const std::size_t beforeBytes = written;
    const std::size_t expectedObjectCount = dismantle.updatesAccount ? 3U : 2U;
    if (objectCount != expectedObjectCount
        || prepared.family.objects[0].id != dismantle.characterDefinitionId
        || prepared.family.objects[0].version != dismantle.characterSoid
        || prepared.family.objects[1].id != dismantle.itemInstanceDefinitionId
        || prepared.family.objects[1].version != dismantle.dismantledInstanceSoid
        || prepared.family.objects[1].encoding != middleware::queuez::Encoding::oodle
        || !prepared.family.objects[1].payload.empty()
        || (dismantle.updatesAccount
            && (prepared.family.objects[2].id != dismantle.accountDefinitionId
                || prepared.family.objects[2].version != dismantle.accountSoid
                || prepared.family.objects[2].encoding != middleware::queuez::Encoding::oodle
                || prepared.family.objects[2].payload.empty()))
        || !queuez_frame::append(scratch,
                                 prepared.family,
                                 prepared.rawClearSize,
                                 prepared.compressedClearSize,
                                 key,
                                 nonce,
                                 response,
                                 written)) {
        return false;
    }
    queuez_report::push(
        "dismantle", queuez::kAccountFamilyType, objectCount, written - beforeBytes, 1);
    return true;
}

} // namespace sunrise::server::bap::encrypted::push
