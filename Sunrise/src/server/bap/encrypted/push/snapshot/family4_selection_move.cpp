#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <optional>
#include <span>

#include "../../../../../core/logging/log.h"
#include "../../../../../middleware/datagen/family4/account/account_encoder.h"
#include "../../../../../middleware/datagen/family4/account/layout.h"
#include "../../../../../middleware/datagen/family4/account/selection_patch/\
account_selection_patch_encoder.h"
#include "../../../../../middleware/datagen/family4/character/character_encoder.h"
#include "../../../../../middleware/datagen/family4/character/layout.h"
#include "../../../../../middleware/datagen/family4/instance/instance_encoder.h"
#include "../../../../../middleware/datagen/family4/instance/layout.h"
#include "../../../../../state/runtime/runtime.h"
#include "internal.h"
#include "snapshot_storage.h"

namespace sunrise::server::bap::encrypted::push::snapshot {
namespace {

namespace family4_datagen = middleware::datagen::family4;
namespace selection_patch = middleware::datagen::family4::account::selection_patch;

/** Logs the exact resolved and encoded fields used by the opcode-403 character upsert. */
void report_equipment_object(const queuez::EquipmentSwap& swap,
                             const state::PendingEquipmentSwap& mutation,
                             const Resolved& selected,
                             const family4_datagen::character::layout::Object& object) noexcept {
    // Row sentinel for the log line only. Zero is a real row, so it cannot stand for "not found".
    constexpr std::size_t kMissing = (std::numeric_limits<std::size_t>::max)();
    std::size_t requestedRow = kMissing;
    std::size_t previousRow = kMissing;
    std::size_t nativeSlot = kMissing;
    bool requestedEquipped = false;
    bool previousEquipped = false;
    for (std::size_t index = 0; index < selected.loadout.itemCount; ++index) {
        const auto& item = selected.loadout.items[index];
        if (item.instance.instanceSoid == mutation.requestedInstanceSoid) {
            requestedRow = item.inventoryRow;
            nativeSlot = item.equipmentSlot;
            requestedEquipped = item.equipped;
        } else if (item.instance.instanceSoid == mutation.previousInstanceSoid) {
            previousRow = item.inventoryRow;
            previousEquipped = item.equipped;
        }
    }
    const std::uint64_t equippedSoid = nativeSlot < object.equippedInstanceSoids.size()
                                           ? object.equippedInstanceSoids[nativeSlot]
                                           : 0;
    const std::uint64_t requestedRowSoid = requestedRow < object.inventoryItems.size()
                                               ? object.inventoryItems[requestedRow].instanceSoid
                                               : 0;
    const std::uint64_t previousRowSoid = previousRow < object.inventoryItems.size()
                                              ? object.inventoryItems[previousRow].instanceSoid
                                              : 0;
    const std::int32_t requestedSerial = requestedRow < object.inventoryItems.size()
                                             ? object.inventoryItems[requestedRow].mutationSerial
                                             : -1;
    const std::int32_t previousSerial = previousRow < object.inventoryItems.size()
                                            ? object.inventoryItems[previousRow].mutationSerial
                                            : -1;
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=equip stage=character_object result=ok family_version=%d root=0x%llX definition=%u "
        "character=0x%llX items=%zu next_serial=%u requested=0x%llX requested_row=%zu "
        "requested_row_soid=0x%llX requested_equipped=%u native_slot=%zu equipped_soid=0x%llX "
        "requested_serial=%d previous=0x%llX previous_row=%zu previous_row_soid=0x%llX "
        "previous_equipped=%u previous_serial=%d",
        swap.after.family4Version,
        static_cast<unsigned long long>(swap.after.family4RootSoid),
        swap.characterDefinitionId,
        static_cast<unsigned long long>(mutation.characterSoid),
        selected.loadout.itemCount,
        object.nextInventorySerial,
        static_cast<unsigned long long>(mutation.requestedInstanceSoid),
        requestedRow,
        static_cast<unsigned long long>(requestedRowSoid),
        static_cast<unsigned>(requestedEquipped),
        nativeSlot,
        static_cast<unsigned long long>(equippedSoid),
        requestedSerial,
        static_cast<unsigned long long>(mutation.previousInstanceSoid),
        previousRow,
        static_cast<unsigned long long>(previousRowSoid),
        static_cast<unsigned>(previousEquipped),
        previousSerial);
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

} // namespace

/** Builds the Family-4 increment that moves the character object to the picked character. */
bool prepare_selection_move(Scratch& scratch,
                            const queuez::SelectCharacter& select,
                            Prepared& prepared) noexcept {
    const Reservation reservation = reserve_prior(scratch, prepared);
    if (reservation.rawWriteOffset > scratch.plaintext.size()
        || reservation.compressedWriteOffset > scratch.sealed.size()) {
        return report_failure("move_reservation");
    }
    const state::AccountState account = state::account_snapshot();
    const std::optional<std::size_t> selectedIndex = find_character_index(account);
    Resolved selected{};
    if (!state::account::valid(account) || !selectedIndex.has_value()
        || !resolve(account, *selectedIndex, selected)
        || account.characters[selected.characterIndex].soid != select.selectedCharacterSoid) {
        return report_failure("move_selection");
    }

    Prepared staged{};
    staged.rawClearSize = reservation.rawClearSize;
    staged.compressedClearSize = reservation.compressedClearSize;
    const auto rawStorage = std::span(scratch.plaintext).subspan(reservation.rawWriteOffset);
    std::size_t compressedExtent = reservation.compressedWriteOffset;
    std::size_t objectCount = 0;
    // The first select resends the character object even when the pick names the current one. With
    // no pending change all three operations go out. Only a pending change may skip it, and only
    // when the character did not move.
    if (!select.patchAccount || select.previousCharacterSoid != select.selectedCharacterSoid) {
        // A zero previous key means the snapshot published no character object. There is no slot
        // to release, so the pick adds one. Releasing a key the Client never held raises queuez
        // error 4.
        if (select.previousCharacterSoid != 0) {
            // The release carries no payload. The encoding field is never read for an empty
            // object, so it just matches its neighbours.
            staged.objects[objectCount++] = middleware::queuez::Object{
                select.characterDefinitionId,
                select.previousCharacterSoid,
                middleware::queuez::Encoding::oodle,
                {},
            };
        }
        if (family4_datagen::character::layout::kObjectSize > rawStorage.size()) {
            return report_failure("move_character_storage");
        }
        const auto characterBytes =
            rawStorage.first(family4_datagen::character::layout::kObjectSize);
        if (!family4_datagen::character::encode(account.characters[selected.characterIndex],
                                                selected.loadout,
                                                selected.lightEvaluation,
                                                characterBytes)) {
            return report_failure("move_character_object");
        }
        if (!append_object(scratch,
                           characterBytes,
                           select.characterDefinitionId,
                           select.selectedCharacterSoid,
                           staged.objects[objectCount++],
                           compressedExtent)) {
            return report_failure("move_character_object");
        }
        staged.rawClearSize = (std::max)(staged.rawClearSize,
                                         reservation.rawWriteOffset
                                             + family4_datagen::character::layout::kObjectSize);
    }

    // The character object is already compressed into sealed storage, so the raw span is free
    // for whichever account body this move carries.
    if (select.patchAccount) {
        std::size_t patchSize = 0;
        if (!selection_patch::encode(select.selectedCharacterSoid, rawStorage, patchSize)
            || patchSize != selection_patch::kPayloadSize) {
            return report_failure("move_account_patch");
        }
        staged.objects[objectCount++] = middleware::queuez::Object{
            select.accountDefinitionId,
            select.after.family4RootSoid,
            middleware::queuez::Encoding::tagReflection,
            rawStorage.first(patchSize),
        };
        staged.rawClearSize =
            (std::max)(staged.rawClearSize, reservation.rawWriteOffset + patchSize);
    } else {
        if (family4_datagen::account::layout::kObjectSize > rawStorage.size()) {
            return report_failure("move_account_storage");
        }
        const auto accountBytes = rawStorage.first(family4_datagen::account::layout::kObjectSize);
        if (!family4_datagen::account::encode(account, accountBytes)) {
            return report_failure("move_account_object");
        }
        if (!append_object(scratch,
                           accountBytes,
                           select.accountDefinitionId,
                           select.after.family4RootSoid,
                           staged.objects[objectCount++],
                           compressedExtent)) {
            return report_failure("move_account_object");
        }
        staged.rawClearSize =
            (std::max)(staged.rawClearSize,
                       reservation.rawWriteOffset + family4_datagen::account::layout::kObjectSize);
    }
    staged.compressedClearSize = (std::max)(reservation.compressedClearSize, compressedExtent);
    staged.family = middleware::queuez::Family{
        kAccountFamilyType,
        select.after.family4RootSoid,
        select.after.family4Version,
        0,
        std::span(staged.objects).first(objectCount),
    };
    if (!commit(staged, prepared)) {
        clear_after(scratch, reservation);
        return report_failure("move_commit");
    }
    return true;
}

/** Builds a single-character Family-4 upsert from an uncommitted equipment after-image. */
bool prepare_equipment_swap(Scratch& scratch,
                            const queuez::EquipmentSwap& swap,
                            const state::PendingEquipmentSwap& mutation,
                            Prepared& prepared) noexcept {
    const Reservation reservation = reserve_prior(scratch, prepared);
    if (reservation.rawWriteOffset > scratch.plaintext.size()
        || reservation.compressedWriteOffset > scratch.sealed.size()) {
        return report_failure("equip_reservation");
    }
    state::AccountState account = state::account_snapshot();
    if (!mutation.prepared || mutation.characterSoid == 0
        || mutation.characterSoid != swap.characterSoid
        || mutation.characterIndex >= account.characterCount
        || account.characters[mutation.characterIndex].soid != mutation.characterSoid) {
        return report_failure("equip_mutation");
    }
    account.characters[mutation.characterIndex] = mutation.afterCharacter;
    Resolved selected{};
    const std::optional<std::size_t> selectedIndex = find_character_index(account);
    if (!state::account::valid(account) || !selectedIndex.has_value()
        || *selectedIndex != mutation.characterIndex
        || !resolve(account, mutation.characterIndex, selected)
        || selected.characterObjectId != swap.characterDefinitionId) {
        return report_failure("equip_selection");
    }

    const auto rawStorage = std::span(scratch.plaintext).subspan(reservation.rawWriteOffset);
    if (family4_datagen::character::layout::kObjectSize > rawStorage.size()) {
        return report_failure("equip_character_storage");
    }
    const auto characterBytes = rawStorage.first(family4_datagen::character::layout::kObjectSize);
    if (!family4_datagen::character::encode(account.characters[mutation.characterIndex],
                                            selected.loadout,
                                            selected.lightEvaluation,
                                            characterBytes)) {
        return report_failure("equip_character_object");
    }
    report_equipment_object(swap,
                            mutation,
                            selected,
                            *reinterpret_cast<const family4_datagen::character::layout::Object*>(
                                characterBytes.data()));

    Prepared staged{};
    staged.rawClearSize =
        (std::max)(reservation.rawClearSize,
                   reservation.rawWriteOffset + family4_datagen::character::layout::kObjectSize);
    std::size_t compressedExtent = reservation.compressedWriteOffset;
    if (!append_object(scratch,
                       characterBytes,
                       swap.characterDefinitionId,
                       swap.characterSoid,
                       staged.objects.front(),
                       compressedExtent)) {
        return report_failure("equip_character_object");
    }
    staged.compressedClearSize = (std::max)(reservation.compressedClearSize, compressedExtent);
    staged.family = middleware::queuez::Family{
        kAccountFamilyType,
        swap.after.family4RootSoid,
        swap.after.family4Version,
        0,
        std::span(staged.objects).first(1),
    };
    if (!commit(staged, prepared)) {
        clear_after(scratch, reservation);
        return report_failure("equip_commit");
    }
    return true;
}

/** Builds a single-character Family-4 upsert from an uncommitted item-state after-image. */
bool prepare_item_state(Scratch& scratch,
                        const queuez::EquipmentSwap& update,
                        const state::PendingItemState& mutation,
                        Prepared& prepared) noexcept {
    const Reservation reservation = reserve_prior(scratch, prepared);
    if (reservation.rawWriteOffset > scratch.plaintext.size()
        || reservation.compressedWriteOffset > scratch.sealed.size()) {
        return report_failure("item_state_reservation");
    }
    state::AccountState account = state::account_snapshot();
    if (!mutation.prepared || mutation.characterSoid == 0
        || mutation.characterSoid != update.characterSoid
        || mutation.characterIndex >= account.characterCount
        || account.characters[mutation.characterIndex].soid != mutation.characterSoid) {
        return report_failure("item_state_mutation");
    }
    account.characters[mutation.characterIndex] = mutation.afterCharacter;
    Resolved selected{};
    const std::optional<std::size_t> selectedIndex = find_character_index(account);
    if (!state::account::valid(account) || !selectedIndex.has_value()
        || *selectedIndex != mutation.characterIndex
        || !resolve(account, mutation.characterIndex, selected)
        || selected.characterObjectId != update.characterDefinitionId) {
        return report_failure("item_state_selection");
    }

    const auto rawStorage = std::span(scratch.plaintext).subspan(reservation.rawWriteOffset);
    if (family4_datagen::character::layout::kObjectSize > rawStorage.size()) {
        return report_failure("item_state_character_storage");
    }
    const auto characterBytes = rawStorage.first(family4_datagen::character::layout::kObjectSize);
    if (!family4_datagen::character::encode(account.characters[mutation.characterIndex],
                                            selected.loadout,
                                            selected.lightEvaluation,
                                            characterBytes)) {
        return report_failure("item_state_character_object");
    }
    const auto& encoded =
        *reinterpret_cast<const family4_datagen::character::layout::Object*>(characterBytes.data());
    if (mutation.itemIndex >= mutation.afterCharacter.inventory.count && !mutation.targetEquipped) {
        return report_failure("item_state_inventory_index");
    }
    std::size_t matchingRows = 0;
    for (const auto& row : encoded.inventoryItems) {
        matchingRows +=
            static_cast<std::size_t>(row.instanceSoid == mutation.targetInstanceSoid
                                     && row.definitionIndex == mutation.targetDefinitionIndex
                                     && row.flags == mutation.afterFlags);
    }
    if (matchingRows != 1) {
        return report_failure("item_state_character_shape");
    }

    Prepared staged{};
    staged.rawClearSize =
        (std::max)(reservation.rawClearSize,
                   reservation.rawWriteOffset + family4_datagen::character::layout::kObjectSize);
    std::size_t compressedExtent = reservation.compressedWriteOffset;
    if (!append_object(scratch,
                       characterBytes,
                       update.characterDefinitionId,
                       update.characterSoid,
                       staged.objects.front(),
                       compressedExtent)) {
        return report_failure("item_state_character_object");
    }
    staged.compressedClearSize = (std::max)(reservation.compressedClearSize, compressedExtent);
    staged.family = middleware::queuez::Family{
        kAccountFamilyType,
        update.after.family4RootSoid,
        update.after.family4Version,
        0,
        std::span(staged.objects).first(1),
    };
    if (!commit(staged, prepared)) {
        clear_after(scratch, reservation);
        return report_failure("item_state_commit");
    }
    return true;
}

/** Builds a resident item upsert followed by charged account balances when the cost consumes. */
bool prepare_socket_plug(Scratch& scratch,
                         const queuez::SocketPlug& socketPlug,
                         const state::PendingSocketPlug& mutation,
                         Prepared& prepared) noexcept {
    const Reservation reservation = reserve_prior(scratch, prepared);
    if (reservation.rawWriteOffset > scratch.plaintext.size()
        || reservation.compressedWriteOffset > scratch.sealed.size()) {
        return report_failure("socket_plug_reservation");
    }

    state::AccountState account{};
    if (!mutation.prepared || mutation.accountSoid == 0 || mutation.characterSoid == 0
        || mutation.targetInstanceSoid == 0 || mutation.accountSoid != socketPlug.accountSoid
        || mutation.characterSoid != socketPlug.characterSoid
        || mutation.targetInstanceSoid != socketPlug.targetInstanceSoid
        || mutation.profileChanged != socketPlug.updatesAccount
        || mutation.accountSoid != socketPlug.after.family4RootSoid
        || socketPlug.accountDefinitionId == 0 || !state::preview_socket_plug(mutation, account)
        || mutation.characterIndex >= account.characterCount
        || account.primarySoid != mutation.accountSoid
        || account.characters[mutation.characterIndex].soid != mutation.characterSoid) {
        return report_failure("socket_plug_mutation");
    }

    Resolved selected{};
    const std::optional<std::size_t> selectedIndex = find_character_index(account);
    if (!state::account::valid(account) || !selectedIndex.has_value()
        || *selectedIndex != mutation.characterIndex
        || !resolve(account, mutation.characterIndex, selected)
        || selected.itemInstanceObjectId != socketPlug.itemInstanceDefinitionId) {
        return report_failure("socket_plug_selection");
    }

    family4_datagen::loadout::ResolvedInstances changed{};
    for (std::size_t index = 0; index < selected.loadout.itemCount; ++index) {
        const family4_datagen::loadout::ResolvedItem& item = selected.loadout.items[index];
        if (item.instance.instanceSoid != mutation.targetInstanceSoid) {
            continue;
        }
        if (changed.itemCount != 0
            || item.instance.baseDefinitionIndex != mutation.targetDefinitionIndex
            || mutation.socketLane >= item.instance.ordinarySockets.plugs.size()
            || item.instance.ordinarySockets.state
                   != family4_datagen::instance::OrdinarySocketBlockState::present
            || !item.instance.ordinarySockets.plugs[mutation.socketLane].has_value()
            || *item.instance.ordinarySockets.plugs[mutation.socketLane]
                   != mutation.plugDefinitionIndex) {
            return report_failure("socket_plug_item_shape");
        }
        changed.items[0] = {item.equipmentSlot, item.instance};
        changed.itemCount = 1;
    }
    if (changed.itemCount != 1) {
        return report_failure("socket_plug_item_missing");
    }

    const auto rawStorage = std::span(scratch.plaintext).subspan(reservation.rawWriteOffset);
    const std::size_t requiredRawSize = socketPlug.updatesAccount
                                            ? family4_datagen::account::layout::kObjectSize
                                            : family4_datagen::instance::layout::kObjectSize;
    if (requiredRawSize > rawStorage.size()) {
        return report_failure("socket_plug_item_storage");
    }
    Prepared staged{};
    staged.rawClearSize =
        (std::max)(reservation.rawClearSize,
                   reservation.rawWriteOffset + family4_datagen::instance::layout::kObjectSize);
    std::size_t compressedExtent = reservation.compressedWriteOffset;
    std::size_t itemCursor = 0;
    if (!append_items(scratch,
                      rawStorage,
                      socketPlug.itemInstanceDefinitionId,
                      changed,
                      0,
                      staged,
                      itemCursor,
                      compressedExtent)
        || itemCursor != 1) {
        clear_after(scratch, reservation);
        return report_failure("socket_plug_item_object");
    }

    std::size_t objectCount = 1;
    if (socketPlug.updatesAccount) {
        const auto accountBytes = rawStorage.first(family4_datagen::account::layout::kObjectSize);
        if (!family4_datagen::account::encode(account, accountBytes)
            || !append_object(scratch,
                              accountBytes,
                              socketPlug.accountDefinitionId,
                              socketPlug.accountSoid,
                              staged.objects[1],
                              compressedExtent)) {
            clear_after(scratch, reservation);
            return report_failure("socket_plug_account_object");
        }
        staged.rawClearSize =
            (std::max)(staged.rawClearSize,
                       reservation.rawWriteOffset + family4_datagen::account::layout::kObjectSize);
        objectCount = 2;
    }

    staged.compressedClearSize = (std::max)(reservation.compressedClearSize, compressedExtent);
    staged.family = middleware::queuez::Family{
        kAccountFamilyType,
        socketPlug.after.family4RootSoid,
        socketPlug.after.family4Version,
        0,
        std::span(staged.objects).first(objectCount),
    };
    if (!commit(staged, prepared)) {
        clear_after(scratch, reservation);
        return report_failure("socket_plug_commit");
    }

    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=socket_plug stage=family4_objects result=ok family_version=%d root=0x%llX "
        "character=0x%llX instance=0x%llX item_definition=%u target_definition=%u "
        "target_bucket=%u lane=%u plug_definition=%u plug_bucket=%u equipped=%u "
        "material_set=%u material_set_hash=0x%08X material_rows=%u account_update=%u objects=%zu "
        "order=%s",
        socketPlug.after.family4Version,
        static_cast<unsigned long long>(socketPlug.after.family4RootSoid),
        static_cast<unsigned long long>(socketPlug.characterSoid),
        static_cast<unsigned long long>(socketPlug.targetInstanceSoid),
        socketPlug.itemInstanceDefinitionId,
        static_cast<unsigned>(mutation.targetDefinitionIndex),
        static_cast<unsigned>(mutation.targetBucketId),
        static_cast<unsigned>(mutation.socketLane),
        static_cast<unsigned>(mutation.plugDefinitionIndex),
        static_cast<unsigned>(mutation.plugBucketId),
        static_cast<unsigned>(mutation.targetEquipped),
        static_cast<unsigned>(mutation.materialRequirementSetIndex),
        mutation.materialRequirementSetHash,
        static_cast<unsigned>(mutation.materialRequirementCount),
        static_cast<unsigned>(socketPlug.updatesAccount),
        objectCount,
        socketPlug.updatesAccount ? "item_account" : "item");
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(count)});
    }
    return true;
}

/** Builds the Family-4 subclass item-instance upsert for one prepared node selection. */
bool prepare_subclass_selection(Scratch& scratch,
                                const queuez::SubclassSelection& selection,
                                const state::PendingSubclassSelection& mutation,
                                Prepared& prepared) noexcept {
    const Reservation reservation = reserve_prior(scratch, prepared);
    if (reservation.rawWriteOffset > scratch.plaintext.size()
        || reservation.compressedWriteOffset > scratch.sealed.size()) {
        return report_failure("subclass_select_reservation");
    }

    state::AccountState account{};
    if (!mutation.prepared || mutation.accountSoid == 0 || mutation.characterSoid == 0
        || mutation.subclassInstanceSoid == 0 || mutation.accountSoid != selection.accountSoid
        || mutation.characterSoid != selection.characterSoid
        || mutation.subclassInstanceSoid != selection.subclassInstanceSoid
        || mutation.accountSoid != selection.after.family4RootSoid
        || selection.itemInstanceDefinitionId == 0
        || !state::preview_subclass_selection(mutation, account)
        || mutation.characterIndex >= account.characterCount
        || account.primarySoid != mutation.accountSoid
        || account.characters[mutation.characterIndex].soid != mutation.characterSoid) {
        return report_failure("subclass_select_mutation");
    }

    Resolved selected{};
    const std::optional<std::size_t> selectedIndex = find_character_index(account);
    if (!state::account::valid(account) || !selectedIndex.has_value()
        || *selectedIndex != mutation.characterIndex
        || !resolve(account, mutation.characterIndex, selected)
        || selected.itemInstanceObjectId != selection.itemInstanceDefinitionId) {
        return report_failure("subclass_select_selection");
    }

    family4_datagen::loadout::ResolvedInstances changed{};
    for (std::size_t index = 0; index < selected.loadout.itemCount; ++index) {
        const family4_datagen::loadout::ResolvedItem& item = selected.loadout.items[index];
        if (item.instance.instanceSoid != mutation.subclassInstanceSoid) {
            continue;
        }
        if (changed.itemCount != 0 || !item.equipped
            || item.instance.baseDefinitionIndex != mutation.subclassDefinitionIndex
            || item.instance.socketEntryListIndex != mutation.socketEntryListIndex
            || !item.instance.socketEntryContentsResolved
            || mutation.requestedEntry >= item.instance.socketEntryCount) {
            return report_failure("subclass_select_item_shape");
        }
        changed.items[0] = {item.equipmentSlot, item.instance};
        changed.itemCount = 1;
    }
    if (changed.itemCount != 1) {
        return report_failure("subclass_select_item_missing");
    }

    const auto rawStorage = std::span(scratch.plaintext).subspan(reservation.rawWriteOffset);
    if (family4_datagen::instance::layout::kObjectSize > rawStorage.size()) {
        return report_failure("subclass_select_item_storage");
    }
    Prepared staged{};
    staged.rawClearSize =
        (std::max)(reservation.rawClearSize,
                   reservation.rawWriteOffset + family4_datagen::instance::layout::kObjectSize);
    std::size_t compressedExtent = reservation.compressedWriteOffset;
    std::size_t itemCursor = 0;
    if (!append_items(scratch,
                      rawStorage,
                      selection.itemInstanceDefinitionId,
                      changed,
                      0,
                      staged,
                      itemCursor,
                      compressedExtent)
        || itemCursor != 1) {
        clear_after(scratch, reservation);
        return report_failure("subclass_select_item_object");
    }

    staged.compressedClearSize = (std::max)(reservation.compressedClearSize, compressedExtent);
    staged.family = middleware::queuez::Family{
        kAccountFamilyType,
        selection.after.family4RootSoid,
        selection.after.family4Version,
        0,
        std::span(staged.objects).first(1),
    };
    if (!commit(staged, prepared)) {
        clear_after(scratch, reservation);
        return report_failure("subclass_select_commit");
    }

    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=subclass_select stage=family4_object result=ok family_version=%d root=0x%llX "
        "character=0x%llX instance=0x%llX item_definition=%u target_definition=%u "
        "socket_list=%u requested_entry=%u objects=1",
        selection.after.family4Version,
        static_cast<unsigned long long>(selection.after.family4RootSoid),
        static_cast<unsigned long long>(selection.characterSoid),
        static_cast<unsigned long long>(selection.subclassInstanceSoid),
        selection.itemInstanceDefinitionId,
        static_cast<unsigned>(mutation.subclassDefinitionIndex),
        static_cast<unsigned>(mutation.socketEntryListIndex),
        static_cast<unsigned>(mutation.requestedEntry));
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(count)});
    }
    return true;
}

} // namespace sunrise::server::bap::encrypted::push::snapshot
