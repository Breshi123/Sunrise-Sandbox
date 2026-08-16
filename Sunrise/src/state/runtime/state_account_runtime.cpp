#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string_view>
#include <utility>

#include "../../core/logging/log.h"
#include "../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include "../build_data/runtime.h"
#include "runtime.h"
#include "state.h"
#include "state_account_transaction_helpers.h"
#include "storage/internal.h"

namespace sunrise::state {
namespace runtime::detail {

namespace authored_inventory = account::inventory;
namespace item_details = build_data::items::details;
namespace inventory_buckets = build_data::inventory::buckets;
namespace family4_loadout = middleware::datagen::family4::loadout;

/** First SOID reserved for item instances created by this local runtime. */
constexpr std::uint64_t kFirstGeneratedItemSoid = 0x4000000000000001ULL;

/** Equipment slots 0-2 are weapons and 3-7 are class-specific armor. */
constexpr std::uint8_t kGearEquipmentSlotCount = 8;

/** Writes one exhaustive equipment-transaction checkpoint to the persistent diagnostic log. */
void report_equipment(std::string_view stage,
                      std::string_view result,
                      EquipmentMutationKind kind,
                      std::uint64_t characterSoid,
                      std::uint64_t previousSoid,
                      std::uint64_t requestedSoid,
                      std::size_t equipmentIndex,
                      std::size_t inventoryIndex,
                      std::uint8_t nativeSlot,
                      std::size_t movedItemCount,
                      std::uint32_t previousHash,
                      std::uint32_t requestedHash) noexcept {
    const std::string_view operation = kind == EquipmentMutationKind::unequip ? "unequip" : "equip";
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=equip operation=%.*s stage=%.*s result=%.*s character=0x%llX previous=0x%llX "
        "requested=0x%llX equipment_index=%zu inventory_index=%zu native_slot=%u "
        "moved_items=%zu previous_hash=0x%08X requested_hash=0x%08X",
        static_cast<int>(operation.size()),
        operation.data(),
        static_cast<int>(stage.size()),
        stage.data(),
        static_cast<int>(result.size()),
        result.data(),
        static_cast<unsigned long long>(characterSoid),
        static_cast<unsigned long long>(previousSoid),
        static_cast<unsigned long long>(requestedSoid),
        equipmentIndex,
        inventoryIndex,
        static_cast<unsigned>(nativeSlot),
        movedItemCount,
        previousHash,
        requestedHash);
    if (count > 0) {
        core::log::write(core::log::Channel::state,
                         result == "ok" ? core::log::Level::debug : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Writes one exhaustive item-acquisition transaction checkpoint. */
void report_acquisition(std::string_view stage,
                        std::string_view result,
                        std::string_view reason,
                        std::uint32_t definitionHash,
                        std::uint64_t characterSoid,
                        std::uint64_t instanceSoid,
                        std::size_t inventoryIndex,
                        std::uint16_t inventoryRow,
                        std::uint8_t equipmentSlot,
                        std::uint32_t nextInventorySerial) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=acquire stage=%.*s result=%.*s reason=%.*s definition_hash=0x%08X character=0x%llX "
        "instance=0x%llX inventory_index=%zu inventory_row=%u equipment_slot=%u next_serial=%u",
        static_cast<int>(stage.size()),
        stage.data(),
        static_cast<int>(result.size()),
        result.data(),
        static_cast<int>(reason.size()),
        reason.data(),
        definitionHash,
        static_cast<unsigned long long>(characterSoid),
        static_cast<unsigned long long>(instanceSoid),
        inventoryIndex,
        static_cast<unsigned>(inventoryRow),
        static_cast<unsigned>(equipmentSlot),
        nextInventorySerial);
    if (count > 0) {
        core::log::write(core::log::Channel::state,
                         result == "ok" ? core::log::Level::debug : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Writes one bounded profile-stack acquisition checkpoint. */
void report_profile_acquisition(std::string_view stage,
                                std::string_view result,
                                std::string_view reason,
                                std::uint32_t definitionHash,
                                std::uint64_t accountSoid,
                                std::uint64_t instanceSoid,
                                std::uint8_t bucketId,
                                std::size_t profileIndex,
                                std::size_t itemCount,
                                std::int32_t previousQuantity,
                                std::int32_t acquiredQuantity,
                                bool appended) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=profile_acquire stage=%.*s result=%.*s reason=%.*s definition_hash=0x%08X "
        "account=0x%llX instance=0x%llX bucket=%u profile_index=%zu item_count=%zu "
        "quantity_before=%d "
        "quantity_after=%d appended=%u",
        static_cast<int>(stage.size()),
        stage.data(),
        static_cast<int>(result.size()),
        result.data(),
        static_cast<int>(reason.size()),
        reason.data(),
        definitionHash,
        static_cast<unsigned long long>(accountSoid),
        static_cast<unsigned long long>(instanceSoid),
        static_cast<unsigned>(bucketId),
        profileIndex,
        itemCount,
        previousQuantity,
        acquiredQuantity,
        static_cast<unsigned>(appended));
    if (count > 0) {
        core::log::write(core::log::Channel::state,
                         result == "ok" ? core::log::Level::debug : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** @return True when two profile stack rows carry identical authored values. */
[[nodiscard]] bool same_profile_item(const authored_inventory::ProfileItem& left,
                                     const authored_inventory::ProfileItem& right) noexcept {
    return left.instanceSoid == right.instanceSoid && left.definitionHash == right.definitionHash
           && left.quantity == right.quantity && left.mutationSerial == right.mutationSerial;
}

/** @return True when a complete fixed profile inventory equals one captured view. */
[[nodiscard]] bool
same_profile_inventory(const AccountState& account,
                       const std::array<authored_inventory::ProfileItem,
                                        authored_inventory::kProfileItemCapacity>& expected,
                       std::size_t expectedCount) noexcept {
    if (account.profileItemCount != expectedCount) {
        return false;
    }
    for (std::size_t index = 0; index < account.profileItems.size(); ++index) {
        if (!same_profile_item(account.profileItems[index], expected[index])) {
            return false;
        }
    }
    return true;
}

/** @return True when two fixed profile views, including their empty tails, are identical. */
[[nodiscard]] bool same_profile_views(
    const std::array<authored_inventory::ProfileItem, authored_inventory::kProfileItemCapacity>&
        left,
    std::size_t leftCount,
    const std::array<authored_inventory::ProfileItem, authored_inventory::kProfileItemCapacity>&
        right,
    std::size_t rightCount) noexcept {
    if (leftCount != rightCount) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (!same_profile_item(left[index], right[index])) {
            return false;
        }
    }
    return true;
}

/**
 * Checks every dense profile stack and mirrors the account encoder's bucket-row placement.
 * This keeps State rejection independent of whether a later push happens to have scratch space.
 */
[[nodiscard]] bool valid_profile_inventory(const AccountState& account) noexcept {
    if (account.profileItemCount > account.profileItems.size()) {
        return false;
    }
    // The bucket identity is one byte on the wire, so 256 covers every value one can carry.
    constexpr std::size_t kBucketIdentityCapacity = 256;
    std::array<std::uint16_t, kBucketIdentityCapacity> taken{};
    std::array<bool, inventory_buckets::kProfileSlotCapacity> occupied{};
    std::size_t actionSourceCount = 0;
    for (std::size_t index = 0; index < account.profileItems.size(); ++index) {
        const authored_inventory::ProfileItem& item = account.profileItems[index];
        if (index >= account.profileItemCount) {
            if (item.instanceSoid != 0 || item.definitionHash != 0 || item.quantity != 0
                || item.mutationSerial != 0) {
                return false;
            }
            continue;
        }
        build_data::items::Definition definition{};
        item_details::Definition detail{};
        inventory_buckets::Descriptor bucket{};
        if (item.quantity <= 0 || item.mutationSerial < 0
            || !build_data::find_item_definition_hash(item.definitionHash, definition)
            || definition.definitionHash != item.definitionHash
            || !build_data::find_configured_item_detail(definition.definitionIndex, detail)
            || detail.definitionIndex != definition.definitionIndex
            || detail.definitionHash != definition.definitionHash
            || detail.bucketId != definition.bucketId
            || detail.instancedDefinitionState != item_details::InstancedDefinitionState::stackable
            || !build_data::find_inventory_bucket_descriptor(definition.bucketId, bucket)
            || bucket.arraySelector != inventory_buckets::ArraySelector::profile) {
            return false;
        }
        const bool actionSource =
            build_data::is_profile_action_source(definition.definitionIndex, definition.bucketId);
        if (actionSource != (item.instanceSoid != 0)
            || (actionSource
                && ++actionSourceCount > authored_inventory::kProfileActionSourceCapacity)) {
            return false;
        }
        const std::uint16_t used = taken[definition.bucketId];
        if (used >= bucket.slotCount) {
            return false;
        }
        const std::size_t row = static_cast<std::size_t>(bucket.firstSlot) + used;
        if (row >= occupied.size() || occupied[row]) {
            return false;
        }
        occupied[row] = true;
        taken[definition.bucketId] = static_cast<std::uint16_t>(used + 1U);
    }
    return true;
}

/** Resolved, aggregated material charge for one installed native requirement set. */
struct MaterialCharge {
    std::uint32_t definitionHash{};
    std::uint64_t quantity{};
    bool deleteOnAction{};
};

/**
 * Validates one native material requirement set and applies its deletions to a copied account.
 * Requirements which are not deleted still gate the action by balance. Material rows must be
 * ordinary non-resident profile stacks; removing an instance-backed action source would also owe
 * a resident release and is deliberately rejected here.
 */
template <typename Requirement>
[[nodiscard]] bool apply_material_requirements(const AccountState& before,
                                               std::span<const Requirement> requirements,
                                               AccountState& after,
                                               bool& changed) noexcept {
    after = before;
    changed = false;
    if (requirements.size() > build_data::material_requirements::kRequirementCapacity) {
        return false;
    }

    std::array<MaterialCharge, build_data::material_requirements::kRequirementCapacity> charges{};
    std::size_t chargeCount = 0;
    for (const Requirement& requirement : requirements) {
        if (requirement.quantity == 0) {
            continue;
        }
        build_data::items::Definition definition{};
        item_details::Definition detail{};
        inventory_buckets::Descriptor bucket{};
        if (requirement.itemDefinitionIndex
                == build_data::material_requirements::kUnavailableItemDefinitionIndex
            || !build_data::find_item_definition_index(requirement.itemDefinitionIndex, definition)
            || definition.definitionIndex != requirement.itemDefinitionIndex
            || !build_data::find_configured_item_detail(requirement.itemDefinitionIndex, detail)
            || detail.definitionIndex != requirement.itemDefinitionIndex
            || detail.definitionHash != definition.definitionHash
            || detail.bucketId != definition.bucketId
            || detail.instancedDefinitionState != item_details::InstancedDefinitionState::stackable
            || !build_data::find_inventory_bucket_descriptor(definition.bucketId, bucket)
            || bucket.arraySelector != inventory_buckets::ArraySelector::profile
            || build_data::is_profile_action_source(definition.definitionIndex,
                                                    definition.bucketId)) {
            return false;
        }
        std::size_t chargeIndex = chargeCount;
        for (std::size_t existing = 0; existing < chargeCount; ++existing) {
            if (charges[existing].definitionHash == definition.definitionHash
                && charges[existing].deleteOnAction == requirement.deleteOnAction) {
                chargeIndex = existing;
                break;
            }
        }
        if (chargeIndex == chargeCount) {
            if (chargeCount >= charges.size()) {
                return false;
            }
            charges[chargeCount].definitionHash = definition.definitionHash;
            charges[chargeCount].deleteOnAction = requirement.deleteOnAction;
            ++chargeCount;
        }
        if (charges[chargeIndex].quantity
            > (std::numeric_limits<std::uint64_t>::max)() - requirement.quantity) {
            return false;
        }
        charges[chargeIndex].quantity += requirement.quantity;
    }

    for (std::size_t charge = 0; charge < chargeCount; ++charge) {
        std::uint64_t available = 0;
        for (std::size_t index = 0; index < before.profileItemCount; ++index) {
            const authored_inventory::ProfileItem& item = before.profileItems[index];
            if (item.definitionHash != charges[charge].definitionHash) {
                continue;
            }
            if (item.instanceSoid != 0 || item.quantity <= 0
                || available > (std::numeric_limits<std::uint64_t>::max)()
                                   - static_cast<std::uint64_t>(item.quantity)) {
                return false;
            }
            available += static_cast<std::uint64_t>(item.quantity);
        }
        if (available < charges[charge].quantity) {
            return false;
        }
    }

    std::array<std::uint64_t, build_data::material_requirements::kRequirementCapacity> remaining{};
    bool hasDeletion = false;
    for (std::size_t charge = 0; charge < chargeCount; ++charge) {
        if (charges[charge].deleteOnAction) {
            remaining[charge] = charges[charge].quantity;
            hasDeletion = true;
        }
    }
    if (!hasDeletion) {
        return true;
    }

    std::array<authored_inventory::ProfileItem, authored_inventory::kProfileItemCapacity>
        compacted{};
    std::size_t compactedCount = 0;
    for (std::size_t index = 0; index < before.profileItemCount; ++index) {
        authored_inventory::ProfileItem item = before.profileItems[index];
        for (std::size_t charge = 0; charge < chargeCount; ++charge) {
            if (remaining[charge] == 0 || item.definitionHash != charges[charge].definitionHash) {
                continue;
            }
            const auto available = static_cast<std::uint64_t>(item.quantity);
            const auto consumed = (std::min)(available, remaining[charge]);
            item.quantity -= static_cast<std::int32_t>(consumed);
            remaining[charge] -= consumed;
        }
        if (item.quantity != 0) {
            if (compactedCount >= compacted.size()) {
                return false;
            }
            compacted[compactedCount++] = item;
        }
    }
    if (std::any_of(remaining.cbegin(),
                    remaining.cbegin() + static_cast<std::ptrdiff_t>(chargeCount),
                    [](std::uint64_t value) noexcept { return value != 0; })) {
        return false;
    }

    std::int32_t greatestMutationSerial = 0;
    for (std::size_t index = 0; index < before.profileItemCount; ++index) {
        greatestMutationSerial =
            (std::max)(greatestMutationSerial, before.profileItems[index].mutationSerial);
    }
    std::size_t changedRows = 0;
    for (std::size_t index = 0; index < compactedCount; ++index) {
        if (index >= before.profileItemCount
            || !same_profile_item(compacted[index], before.profileItems[index])) {
            ++changedRows;
        }
    }
    if (changedRows > static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)()
                                               - greatestMutationSerial)) {
        return false;
    }
    for (std::size_t index = 0; index < compactedCount; ++index) {
        if (index >= before.profileItemCount
            || !same_profile_item(compacted[index], before.profileItems[index])) {
            compacted[index].mutationSerial = ++greatestMutationSerial;
        }
    }
    after.profileItems = compacted;
    after.profileItemCount = compactedCount;
    changed = !same_profile_inventory(after, before.profileItems, before.profileItemCount);
    return changed && account::valid(after) && valid_profile_inventory(after);
}

/** Resolves one Collections row's installed cost without embedding any item or quantity policy. */
[[nodiscard]] bool
apply_collection_materials(const AccountState& before,
                           const build_data::collectibles::Definition& collectible,
                           AccountState& after,
                           bool& changed) noexcept {
    after = before;
    changed = false;
    if (collectible.materialRequirementCount == 0) {
        return collectible.materialRequirementSetIndex
                   == build_data::collectibles::kUnavailableMaterialRequirementSetIndex
               && collectible.materialRequirementSetHash == 0;
    }
    if (collectible.materialRequirementCount > collectible.materialRequirements.size()
        || collectible.materialRequirementSetIndex
               == build_data::collectibles::kUnavailableMaterialRequirementSetIndex
        || collectible.materialRequirementSetHash == 0) {
        return false;
    }
    return apply_material_requirements(
        before,
        std::span(collectible.materialRequirements)
            .first(static_cast<std::size_t>(collectible.materialRequirementCount)),
        after,
        changed);
}

/**
 * Answers whether the account holds one applicable stack of a socket action source.
 * @param account Account whose profile stacks are searched.
 * @param definitionHash Plug definition the Client asked to apply.
 * @return True when a profile stack of that definition holds at least one unit.
 */
[[nodiscard]] bool holds_plug_source(const AccountState& account,
                                     std::uint32_t definitionHash) noexcept {
    for (std::size_t index = 0; index < account.profileItemCount; ++index) {
        const authored_inventory::ProfileItem& item = account.profileItems[index];
        if (item.definitionHash == definitionHash && item.quantity > 0) {
            return true;
        }
    }
    return false;
}

/**
 * Takes one unit of an owned socket action source, releasing the row when its last unit goes.
 *
 * An action source is an instanced profile row, so the authored-cost path cannot spend it. The
 * row keeps its identity and position while units remain, because the Client addresses it by that
 * identity. An emptied row is removed and the rows after it move up, which is the same shape the
 * authored-cost path leaves behind when a stack empties.
 *
 * @param account Account whose profile stacks are spent in place.
 * @param definitionHash Plug definition being applied.
 * @return True when one unit was taken.
 */
[[nodiscard]] bool spend_plug_source(AccountState& account, std::uint32_t definitionHash) noexcept {
    std::size_t row = account.profileItemCount;
    for (std::size_t index = 0; index < account.profileItemCount; ++index) {
        if (account.profileItems[index].definitionHash == definitionHash
            && account.profileItems[index].quantity > 0) {
            row = index;
            break;
        }
    }
    if (row >= account.profileItemCount) {
        return false;
    }
    if (--account.profileItems[row].quantity > 0) {
        return true;
    }
    for (std::size_t index = row; index + 1U < account.profileItemCount; ++index) {
        account.profileItems[index] = account.profileItems[index + 1U];
    }
    account.profileItems[--account.profileItemCount] = {};
    return true;
}

/** Applies one dense installed action-cost set resolved from the selected plug or action row. */
[[nodiscard]] bool
apply_action_materials(const AccountState& before,
                       const build_data::material_requirements::Definition& definition,
                       AccountState& after,
                       bool& changed) noexcept {
    if (definition.requirementSetHash == 0
        || definition.requirementSetIndex == build_data::material_requirements::kUnavailableSetIndex
        || definition.requirementCount == 0
        || definition.requirementCount > definition.requirements.size()) {
        after = {};
        changed = false;
        return false;
    }
    for (std::size_t index = 0; index < definition.requirementCount; ++index) {
        if (definition.requirements[index].condition
            != build_data::material_requirements::kUnconditionalRequirement) {
            after = {};
            changed = false;
            return false;
        }
    }
    return apply_material_requirements(
        before,
        std::span(definition.requirements)
            .first(static_cast<std::size_t>(definition.requirementCount)),
        after,
        changed);
}

/** @return True when a pending profile acquisition carries canonical dense before/after images. */
[[nodiscard]] bool
valid_profile_mutation_shape(const PendingProfileItemAcquisition& mutation) noexcept {
    if (!mutation.prepared || mutation.accountSoid == 0
        || mutation.actionSource != (mutation.acquiredInstanceSoid != 0)
        || mutation.acquiredDefinitionHash == authored_inventory::kNoDefinitionHash
        || mutation.expectedItemCount > authored_inventory::kProfileItemCapacity
        || mutation.afterItemCount > authored_inventory::kProfileItemCapacity
        || mutation.profileIndex >= mutation.afterItemCount || mutation.previousQuantity < 0
        || mutation.acquiredQuantity <= mutation.previousQuantity
        || mutation.acquiredQuantity - mutation.previousQuantity != 1
        || mutation.previousMutationSerial < 0
        || mutation.acquiredMutationSerial <= mutation.previousMutationSerial) {
        return false;
    }
    if (mutation.appended) {
        if (mutation.afterItemCount == 0 || mutation.previousQuantity != 0) {
            return false;
        }
    } else if (mutation.previousQuantity == 0) {
        return false;
    }

    bool foundBeforeTarget = mutation.appended;
    for (std::size_t index = 0; index < mutation.beforeItems.size(); ++index) {
        const authored_inventory::ProfileItem& before = mutation.beforeItems[index];
        const authored_inventory::ProfileItem& after = mutation.afterItems[index];
        if (index < mutation.expectedItemCount
            && before.mutationSerial >= mutation.acquiredMutationSerial) {
            return false;
        }
        if (index >= mutation.expectedItemCount
            && (before.instanceSoid != 0 || before.definitionHash != 0 || before.quantity != 0
                || before.mutationSerial != 0)) {
            return false;
        }
        if (index >= mutation.afterItemCount
            && (after.instanceSoid != 0 || after.definitionHash != 0 || after.quantity != 0
                || after.mutationSerial != 0)) {
            return false;
        }
        if (!mutation.appended && index < mutation.expectedItemCount
            && before.instanceSoid == mutation.acquiredInstanceSoid
            && before.definitionHash == mutation.acquiredDefinitionHash
            && before.quantity == mutation.previousQuantity
            && before.mutationSerial == mutation.previousMutationSerial) {
            if (foundBeforeTarget) {
                return false;
            }
            foundBeforeTarget = true;
        }
    }
    const authored_inventory::ProfileItem& acquired = mutation.afterItems[mutation.profileIndex];
    return foundBeforeTarget && acquired.instanceSoid == mutation.acquiredInstanceSoid
           && acquired.definitionHash == mutation.acquiredDefinitionHash
           && acquired.quantity == mutation.acquiredQuantity
           && acquired.mutationSerial == mutation.acquiredMutationSerial;
}

/** Applies one validated pending profile after-image over a current, matching account. */
[[nodiscard]] bool materialize_profile_acquisition(const AccountState& current,
                                                   const PendingProfileItemAcquisition& mutation,
                                                   AccountState& after) noexcept {
    if (!valid_profile_mutation_shape(mutation) || current.primarySoid != mutation.accountSoid
        || !same_profile_inventory(current, mutation.beforeItems, mutation.expectedItemCount)) {
        return false;
    }
    item_details::Definition detail{};
    inventory_buckets::Descriptor bucket{};
    build_data::items::Definition item{};
    build_data::collectibles::Definition collectible{};
    if (!build_data::find_collectible_definition(mutation.collectibleIndex, collectible)
        || collectible.itemDefinitionIndex
               == build_data::collectibles::kUnavailableItemDefinitionIndex
        || collectible.materialRequirementSetHash != mutation.materialRequirementSetHash
        || collectible.materialRequirementCount != mutation.materialRequirementCount
        || !build_data::find_item_definition_hash(mutation.acquiredDefinitionHash, item)
        || collectible.itemDefinitionIndex != item.definitionIndex
        || !build_data::find_configured_item_detail(item.definitionIndex, detail)
        || detail.definitionHash != mutation.acquiredDefinitionHash
        || detail.definitionIndex != item.definitionIndex || detail.bucketId != item.bucketId
        || detail.bucketId != mutation.bucketId
        || detail.instancedDefinitionState != item_details::InstancedDefinitionState::stackable
        || detail.maxStackSize <= 0 || mutation.acquiredQuantity > detail.maxStackSize
        || !build_data::find_inventory_bucket_descriptor(detail.bucketId, bucket)
        || bucket.arraySelector != inventory_buckets::ArraySelector::profile
        || build_data::is_profile_action_source(item.definitionIndex, item.bucketId)
               != mutation.actionSource) {
        return false;
    }
    after = current;
    after.profileItems = mutation.afterItems;
    after.profileItemCount = mutation.afterItemCount;
    return account::valid(after) && valid_profile_inventory(after);
}

/** Writes one exhaustive item-dismantle transaction checkpoint. */
void report_dismantle(std::string_view stage,
                      std::string_view result,
                      std::string_view reason,
                      std::uint32_t definitionHash,
                      std::uint64_t characterSoid,
                      std::uint64_t instanceSoid,
                      std::size_t inventoryIndex,
                      std::uint16_t inventoryRow,
                      std::uint8_t equipmentSlot,
                      std::size_t movedItemCount,
                      std::uint32_t nextInventorySerial) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=dismantle stage=%.*s result=%.*s reason=%.*s definition_hash=0x%08X "
                      "character=0x%llX instance=0x%llX inventory_index=%zu inventory_row=%u "
                      "equipment_slot=%u moved_items=%zu next_serial=%u",
                      static_cast<int>(stage.size()),
                      stage.data(),
                      static_cast<int>(result.size()),
                      result.data(),
                      static_cast<int>(reason.size()),
                      reason.data(),
                      definitionHash,
                      static_cast<unsigned long long>(characterSoid),
                      static_cast<unsigned long long>(instanceSoid),
                      inventoryIndex,
                      static_cast<unsigned>(inventoryRow),
                      static_cast<unsigned>(equipmentSlot),
                      movedItemCount,
                      nextInventorySerial);
    if (count > 0) {
        core::log::write(core::log::Channel::state,
                         result == "ok" ? core::log::Level::debug : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Writes one bounded opcode-903 socket-selection transaction checkpoint. */
void report_socket_plug(std::string_view stage,
                        std::string_view result,
                        std::string_view reason,
                        std::uint64_t characterSoid,
                        std::uint64_t targetInstanceSoid,
                        std::uint16_t targetDefinitionIndex,
                        std::uint8_t socketLane,
                        std::uint16_t plugDefinitionIndex,
                        std::uint8_t targetBucketId,
                        std::uint8_t plugBucketId,
                        bool targetEquipped,
                        std::size_t itemIndex) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=socket_plug stage=%.*s result=%.*s reason=%.*s character=0x%llX "
        "instance=0x%llX target_definition=%u target_bucket=%u lane=%u plug_definition=%u "
        "plug_bucket=%u equipped=%u item_index=%zu",
        static_cast<int>(stage.size()),
        stage.data(),
        static_cast<int>(result.size()),
        result.data(),
        static_cast<int>(reason.size()),
        reason.data(),
        static_cast<unsigned long long>(characterSoid),
        static_cast<unsigned long long>(targetInstanceSoid),
        static_cast<unsigned>(targetDefinitionIndex),
        static_cast<unsigned>(targetBucketId),
        static_cast<unsigned>(socketLane),
        static_cast<unsigned>(plugDefinitionIndex),
        static_cast<unsigned>(plugBucketId),
        static_cast<unsigned>(targetEquipped),
        itemIndex);
    if (count > 0) {
        core::log::write(core::log::Channel::state,
                         result == "ok" ? core::log::Level::debug : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Resolves the installed native equipment slot for one configured authored item. */
[[nodiscard]] bool native_equipment_slot(const authored_inventory::Item& item,
                                         std::uint8_t& slot) noexcept {
    build_data::items::Definition definition{};
    item_details::Definition detail{};
    if (!build_data::find_item_definition_hash(item.definitionHash, definition)
        || definition.definitionHash != item.definitionHash
        || !build_data::find_configured_item_detail(definition.definitionIndex, detail)
        || detail.definitionIndex != definition.definitionIndex
        || detail.definitionHash != definition.definitionHash
        || detail.bucketId != definition.bucketId || !detail.equipmentSlot.has_value()
        || *detail.equipmentSlot < 0
        || static_cast<std::size_t>(*detail.equipmentSlot) >= item_details::kEquipmentSlotCount) {
        return false;
    }
    slot = static_cast<std::uint8_t>(*detail.equipmentSlot);
    return true;
}

/** Resolves the installed physical inventory bucket for one configured authored item. */
[[nodiscard]] bool inventory_bucket_id(const authored_inventory::Item& item,
                                       std::uint8_t& bucketId) noexcept {
    build_data::items::Definition definition{};
    item_details::Definition detail{};
    if (!build_data::find_item_definition_hash(item.definitionHash, definition)
        || definition.definitionHash != item.definitionHash
        || !build_data::find_configured_item_detail(definition.definitionIndex, detail)
        || detail.definitionIndex != definition.definitionIndex
        || detail.definitionHash != definition.definitionHash
        || detail.bucketId != definition.bucketId) {
        return false;
    }
    bucketId = detail.bucketId;
    return true;
}

/** Maps the 16 proven native equipment positions onto their stable authored State slots. */
[[nodiscard]] bool semantic_equipment_slot(std::uint8_t nativeSlot,
                                           std::size_t& semanticIndex) noexcept {
    using EquipmentSlot = authored_inventory::EquipmentSlot;
    EquipmentSlot semanticSlot = EquipmentSlot::count;
    switch (nativeSlot) {
    case 0:
        semanticSlot = EquipmentSlot::subclass;
        break;
    case 1:
        semanticSlot = EquipmentSlot::helmet;
        break;
    case 2:
        semanticSlot = EquipmentSlot::gauntlets;
        break;
    case 4:
        semanticSlot = EquipmentSlot::chest;
        break;
    case 5:
        semanticSlot = EquipmentSlot::legs;
        break;
    case 6:
        semanticSlot = EquipmentSlot::classItem;
        break;
    case 7:
        semanticSlot = EquipmentSlot::kinetic;
        break;
    case 8:
        semanticSlot = EquipmentSlot::energy;
        break;
    case 9:
        semanticSlot = EquipmentSlot::heavy;
        break;
    case 10:
        semanticSlot = EquipmentSlot::ship;
        break;
    case 11:
        semanticSlot = EquipmentSlot::vehicle;
        break;
    case 12:
        semanticSlot = EquipmentSlot::ghost;
        break;
    case 13:
        semanticSlot = EquipmentSlot::emblem;
        break;
    case 14:
        semanticSlot = EquipmentSlot::emote;
        break;
    case 15:
        semanticSlot = EquipmentSlot::clanBanner;
        break;
    case 17:
        semanticSlot = EquipmentSlot::finisher;
        break;
    default:
        return false;
    }
    semanticIndex = static_cast<std::size_t>(semanticSlot);
    return semanticIndex < authored_inventory::kEquipmentSlotCount;
}

/** Finds one instance exactly once in a checked, row-sorted loadout. */
[[nodiscard]] bool find_resolved_position(const family4_loadout::ResolvedLoadout& loadout,
                                          std::uint64_t instanceSoid,
                                          ResolvedPosition& position) noexcept {
    bool found = false;
    for (std::size_t index = 0; index < loadout.itemCount; ++index) {
        const family4_loadout::ResolvedItem& item = loadout.items[index];
        if (item.instance.instanceSoid != instanceSoid) {
            continue;
        }
        if (found) {
            return false;
        }
        found = true;
        position.inventoryRow = item.inventoryRow;
        position.equipmentSlot = item.equipmentSlot;
        position.equipped = item.equipped;
        position.mutationSerial = item.mutationSerial;
    }
    return found;
}

/** @return True when the native placement and equipped marker are unchanged. */
[[nodiscard]] bool same_position(const ResolvedPosition& left,
                                 const ResolvedPosition& right) noexcept {
    return left.inventoryRow == right.inventoryRow && left.equipmentSlot == right.equipmentSlot
           && left.equipped == right.equipped;
}

/**
 * Applies canonical mutation generations after one shape-only equipment transition.
 *
 * Every surviving instance must preserve its native bucket. A generation advances exactly when
 * its published native row or equipped marker changes, and a second resolution proves that the
 * stamped after-image retained the staged placement.
 */
[[nodiscard]] bool
finalize_equipment_transition(const AccountState& account,
                              std::size_t characterIndex,
                              std::uint64_t requestedInstanceSoid,
                              EquipmentMutationKind kind,
                              std::uint8_t expectedNativeSlot,
                              const family4_loadout::ResolvedLoadout& beforeLoadout,
                              CharacterState& after,
                              std::size_t& movedItemCount) noexcept {
    movedItemCount = 0;
    if (characterIndex >= account.characterCount || kind == EquipmentMutationKind::none) {
        return false;
    }

    AccountState candidate = account;
    candidate.characters[characterIndex] = after;
    family4_loadout::ResolvedLoadout placedAfter{};
    if (!account::valid(candidate)
        || !family4_loadout::resolve(candidate, characterIndex, placedAfter)
        || placedAfter.itemCount != beforeLoadout.itemCount) {
        return false;
    }

    ResolvedPosition beforeRequested{};
    ResolvedPosition afterRequested{};
    if (!find_resolved_position(beforeLoadout, requestedInstanceSoid, beforeRequested)
        || !find_resolved_position(placedAfter, requestedInstanceSoid, afterRequested)
        || beforeRequested.equipmentSlot != expectedNativeSlot
        || afterRequested.equipmentSlot != expectedNativeSlot
        || (kind == EquipmentMutationKind::equip
            && (beforeRequested.equipped || !afterRequested.equipped))
        || (kind == EquipmentMutationKind::unequip
            && (!beforeRequested.equipped || afterRequested.equipped))) {
        return false;
    }

    const auto count_move = [&](const authored_inventory::Item& item) noexcept {
        ResolvedPosition beforePosition{};
        ResolvedPosition afterPosition{};
        if (!find_resolved_position(beforeLoadout, item.instanceSoid, beforePosition)
            || !find_resolved_position(placedAfter, item.instanceSoid, afterPosition)
            || beforePosition.equipmentSlot != afterPosition.equipmentSlot) {
            return false;
        }
        movedItemCount += static_cast<std::size_t>(!same_position(beforePosition, afterPosition));
        return true;
    };
    for (const auto& item : after.equipment.slots) {
        if (item.has_value() && !count_move(*item)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < after.inventory.count; ++index) {
        if (!count_move(after.inventory.values[index])) {
            return false;
        }
    }

    // The serial is signed on the wire, so it must stay inside the positive int32 range.
    constexpr std::uint32_t kMaximumInventorySerial =
        static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());
    if (movedItemCount == 0 || after.nextInventorySerial > kMaximumInventorySerial
        || movedItemCount > kMaximumInventorySerial - after.nextInventorySerial) {
        return false;
    }

    const auto stamp_move = [&](authored_inventory::Item& item) noexcept {
        ResolvedPosition beforePosition{};
        ResolvedPosition afterPosition{};
        if (!find_resolved_position(beforeLoadout, item.instanceSoid, beforePosition)
            || !find_resolved_position(placedAfter, item.instanceSoid, afterPosition)
            || beforePosition.equipmentSlot != afterPosition.equipmentSlot) {
            return false;
        }
        if (!same_position(beforePosition, afterPosition)) {
            item.mutationSerial = static_cast<std::int32_t>(after.nextInventorySerial++);
        }
        return true;
    };
    for (auto& item : after.equipment.slots) {
        if (item.has_value() && !stamp_move(*item)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < after.inventory.count; ++index) {
        if (!stamp_move(after.inventory.values[index])) {
            return false;
        }
    }

    candidate.characters[characterIndex] = after;
    family4_loadout::ResolvedLoadout checkedAfter{};
    if (!account::valid(candidate)
        || !family4_loadout::resolve(candidate, characterIndex, checkedAfter)
        || checkedAfter.itemCount != placedAfter.itemCount) {
        return false;
    }
    for (const auto& item : after.equipment.slots) {
        if (!item.has_value()) {
            continue;
        }
        ResolvedPosition placed{};
        ResolvedPosition checked{};
        if (!find_resolved_position(placedAfter, item->instanceSoid, placed)
            || !find_resolved_position(checkedAfter, item->instanceSoid, checked)
            || !same_position(placed, checked) || checked.mutationSerial != item->mutationSerial) {
            return false;
        }
    }
    for (std::size_t index = 0; index < after.inventory.count; ++index) {
        const authored_inventory::Item& item = after.inventory.values[index];
        ResolvedPosition placed{};
        ResolvedPosition checked{};
        if (!find_resolved_position(placedAfter, item.instanceSoid, placed)
            || !find_resolved_position(checkedAfter, item.instanceSoid, checked)
            || !same_position(placed, checked) || checked.mutationSerial != item.mutationSerial) {
            return false;
        }
    }
    return true;
}

/** @return True when two authored item values are identical, including socket policy and tail. */
[[nodiscard]] bool same_item(const authored_inventory::Item& left,
                             const authored_inventory::Item& right) noexcept {
    return left.instanceSoid == right.instanceSoid && left.definitionHash == right.definitionHash
           && left.level == right.level && left.quantity == right.quantity
           && left.flags == right.flags && left.sockets.policy == right.sockets.policy
           && left.sockets.plugCount == right.sockets.plugCount
           && left.sockets.plugs == right.sockets.plugs;
}

/** Records one checked native item-state transition. */
void report_item_state(std::string_view stage,
                       std::string_view result,
                       std::string_view reason,
                       std::uint64_t characterSoid,
                       std::uint64_t instanceSoid,
                       std::uint16_t definitionIndex,
                       std::uint32_t beforeFlags,
                       std::uint32_t afterFlags,
                       bool equipped,
                       std::size_t itemIndex) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=item_state stage=%.*s result=%.*s reason=%.*s character=0x%llX instance=0x%llX "
        "definition=%u flags_before=0x%X flags_after=0x%X equipped=%u item_index=%zu",
        static_cast<int>(stage.size()),
        stage.data(),
        static_cast<int>(result.size()),
        result.data(),
        static_cast<int>(reason.size()),
        reason.data(),
        static_cast<unsigned long long>(characterSoid),
        static_cast<unsigned long long>(instanceSoid),
        static_cast<unsigned>(definitionIndex),
        beforeFlags,
        afterFlags,
        equipped ? 1U : 0U,
        itemIndex);
    if (count > 0) {
        core::log::write(core::log::Channel::state,
                         result == "ok" ? core::log::Level::debug : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Exact stationary-item comparison, including its inventory mutation generation. */
[[nodiscard]] bool same_stationary_item(const authored_inventory::Item& left,
                                        const authored_inventory::Item& right) noexcept {
    return same_item(left, right) && left.mutationSerial == right.mutationSerial;
}

/** @return True when every item-bearing field of two character views is identical. */
[[nodiscard]] bool same_loadout(const CharacterState& left, const CharacterState& right) noexcept {
    if (left.soid != right.soid || left.selected != right.selected || left.race != right.race
        || left.gender != right.gender || left.characterClass != right.characterClass
        || left.level != right.level || left.accepted != right.accepted
        || left.previewAvailable != right.previewAvailable
        || left.appearanceValue != right.appearanceValue
        || left.lastOrbitedDestination != right.lastOrbitedDestination
        || left.contentBypass != right.contentBypass
        || left.movementAbilityEntry != right.movementAbilityEntry
        || left.grenadeAbilityEntry != right.grenadeAbilityEntry
        || left.superAbilityEntry != right.superAbilityEntry
        || left.meleeAbilityEntry != right.meleeAbilityEntry
        || left.classAbilityEntry != right.classAbilityEntry
        || left.nextInventorySerial != right.nextInventorySerial
        || left.inventory.count != right.inventory.count) {
        return false;
    }
    for (std::size_t index = 0; index < left.equipment.slots.size(); ++index) {
        const auto& leftItem = left.equipment.slots[index];
        const auto& rightItem = right.equipment.slots[index];
        if (leftItem.has_value() != rightItem.has_value()
            || (leftItem.has_value() && !same_stationary_item(*leftItem, *rightItem))) {
            return false;
        }
    }
    for (std::size_t index = 0; index < left.inventory.count; ++index) {
        if (!same_stationary_item(left.inventory.values[index], right.inventory.values[index])) {
            return false;
        }
    }
    return true;
}

/** Exact character comparison, including the canonical unused inventory tail. */
[[nodiscard]] bool same_character(const CharacterState& left,
                                  const CharacterState& right) noexcept {
    if (!same_loadout(left, right)) {
        return false;
    }
    for (std::size_t index = left.inventory.count; index < left.inventory.values.size(); ++index) {
        if (!same_stationary_item(left.inventory.values[index], right.inventory.values[index])) {
            return false;
        }
    }
    return true;
}

/** Stable location of one character-owned item inside authored State. */
struct CharacterItemLocation {
    std::size_t index{};
    bool equipped{};
};

/** Finds one item SOID exactly once in the character's equipment and dense inventory. */
[[nodiscard]] bool find_character_item_location(const CharacterState& character,
                                                std::uint64_t instanceSoid,
                                                CharacterItemLocation& location) noexcept {
    location = {};
    bool found = false;
    for (std::size_t index = 0; index < character.equipment.slots.size(); ++index) {
        const auto& item = character.equipment.slots[index];
        if (!item.has_value() || item->instanceSoid != instanceSoid) {
            continue;
        }
        if (found) {
            return false;
        }
        found = true;
        location = {index, true};
    }
    for (std::size_t index = 0; index < character.inventory.count; ++index) {
        if (character.inventory.values[index].instanceSoid != instanceSoid) {
            continue;
        }
        if (found) {
            return false;
        }
        found = true;
        location = {index, false};
    }
    return found;
}

/** Borrows an item at a previously validated authored location. */
[[nodiscard]] const authored_inventory::Item*
character_item_at(const CharacterState& character, const CharacterItemLocation& location) noexcept {
    if (location.equipped) {
        if (location.index >= character.equipment.slots.size()
            || !character.equipment.slots[location.index].has_value()) {
            return nullptr;
        }
        return &*character.equipment.slots[location.index];
    }
    if (location.index >= character.inventory.count) {
        return nullptr;
    }
    return &character.inventory.values[location.index];
}

/** Borrows a mutable item at a previously validated authored location. */
[[nodiscard]] authored_inventory::Item*
character_item_at(CharacterState& character, const CharacterItemLocation& location) noexcept {
    if (location.equipped) {
        if (location.index >= character.equipment.slots.size()
            || !character.equipment.slots[location.index].has_value()) {
            return nullptr;
        }
        return &*character.equipment.slots[location.index];
    }
    if (location.index >= character.inventory.count) {
        return nullptr;
    }
    return &character.inventory.values[location.index];
}

/** Materializes native initial plugs as a complete authored socket block. */
[[nodiscard]] bool materialize_native_sockets(const item_details::Definition& detail,
                                              authored_inventory::Sockets& sockets) noexcept {
    sockets = {};
    if (detail.ordinarySocketState != item_details::OrdinarySocketState::present
        || detail.ordinarySocketCount > sockets.plugs.size()) {
        return false;
    }
    sockets.policy = authored_inventory::SocketPolicy::authored;
    sockets.plugCount = detail.ordinarySocketCount;
    for (std::size_t lane = 0; lane < sockets.plugCount; ++lane) {
        const std::uint16_t plugIndex = detail.initialPlugIndices[lane];
        if (plugIndex == item_details::kUnavailableItemIndex) {
            continue;
        }
        build_data::items::Definition plug{};
        if (!build_data::find_item_definition_index(plugIndex, plug)
            || plug.definitionIndex != plugIndex
            || plug.definitionHash == authored_inventory::kNoDefinitionHash) {
            return false;
        }
        sockets.plugs[lane] = plug.definitionHash;
    }
    return authored_inventory::valid(sockets);
}

/** Stages the canonical socket-only after-image over one already validated account snapshot. */
[[nodiscard]] bool stage_socket_plug(const AccountState& snapshot,
                                     std::size_t characterIndex,
                                     std::uint64_t targetInstanceSoid,
                                     std::uint8_t socketLane,
                                     std::uint16_t plugDefinitionIndex,
                                     PendingSocketPlug& mutation) noexcept {
    mutation = {};
    CharacterItemLocation location{};
    build_data::items::Definition targetDefinition{};
    build_data::items::Definition plugDefinition{};
    const auto fail = [&](std::string_view reason) noexcept {
        const std::uint64_t characterSoid =
            characterIndex < snapshot.characterCount ? snapshot.characters[characterIndex].soid : 0;
        report_socket_plug("stage_internal",
                           "fail",
                           reason,
                           characterSoid,
                           targetInstanceSoid,
                           targetDefinition.definitionIndex,
                           socketLane,
                           plugDefinitionIndex,
                           targetDefinition.bucketId,
                           plugDefinition.bucketId,
                           location.equipped,
                           location.index);
        mutation = {};
        return false;
    };
    if (!account::valid(snapshot) || characterIndex >= snapshot.characterCount
        || targetInstanceSoid == 0 || socketLane >= authored_inventory::kPlugCapacity) {
        return fail("request_or_account");
    }
    const CharacterState& before = snapshot.characters[characterIndex];
    if (!before.selected || before.soid == 0) {
        return fail("selected_character");
    }

    family4_loadout::ResolvedLoadout beforeLoadout{};
    if (!find_character_item_location(before, targetInstanceSoid, location)
        || !family4_loadout::resolve(snapshot, characterIndex, beforeLoadout)) {
        return fail("target_or_before_loadout");
    }
    const authored_inventory::Item* target = character_item_at(before, location);
    item_details::Definition detail{};
    if (target == nullptr
        || !build_data::find_item_definition_hash(target->definitionHash, targetDefinition)
        || targetDefinition.definitionHash != target->definitionHash
        || !build_data::find_configured_item_detail(targetDefinition.definitionIndex, detail)
        || detail.definitionIndex != targetDefinition.definitionIndex
        || detail.definitionHash != targetDefinition.definitionHash
        || detail.bucketId != targetDefinition.bucketId
        || detail.ordinarySocketState != item_details::OrdinarySocketState::present
        || socketLane >= detail.ordinarySocketCount
        || detail.ordinarySocketCount > authored_inventory::kPlugCapacity
        || !build_data::find_item_definition_index(plugDefinitionIndex, plugDefinition)
        || plugDefinition.definitionIndex != plugDefinitionIndex
        || plugDefinition.definitionHash == authored_inventory::kNoDefinitionHash
        || !build_data::is_socket_plug_allowed(
            targetDefinition.definitionIndex, socketLane, plugDefinitionIndex)) {
        return fail("definition_or_compatibility");
    }

    // Ownership is only meaningful where the plug is a finite supply the account draws down. A
    // shader is one: it is pulled from Collections into a profile stack and spent by applying it.
    // An ornament is a permanent unlock the account holds once earned, not a stack it draws
    // down, which is why the Client offers every valid one for a socket. Requiring a stack for
    // one would refuse a plug the account already has.
    const bool consumesStack =
        build_data::is_profile_action_source(plugDefinitionIndex, plugDefinition.bucketId)
        && build_data::is_consumed_on_apply(plugDefinitionIndex, plugDefinition.bucketId)
        && !(socketLane < detail.initialPlugIndices.size()
             && detail.initialPlugIndices[socketLane] == plugDefinitionIndex);
    if (consumesStack && !holds_plug_source(snapshot, plugDefinition.definitionHash)) {
        return fail("plug_ownership");
    }

    AccountState chargedAccount = snapshot;
    build_data::material_requirements::Definition materialSet{};
    bool profileChanged = false;
    const std::uint16_t materialSetIndex = plugDefinition.insertionMaterialRequirementSetIndex;
    if (materialSetIndex != build_data::items::kUnavailableMaterialRequirementSetIndex
        && (!build_data::find_material_requirement_set(materialSetIndex, materialSet)
            || materialSet.requirementSetIndex != materialSetIndex
            || !apply_action_materials(snapshot, materialSet, chargedAccount, profileChanged))) {
        return fail("materials");
    }

    // Applying spends the stack the plug came from. The insertion cost above is a separate
    // authored charge that leaves the plug itself untouched, so the unit is taken here.
    //
    // The authored-cost path cannot do this. It refuses any row carrying an instance key, because
    // it exists for the non-instanced currency and material stacks, and an action source always
    // carries one. Spending one is therefore its own transition: the row keeps its identity while
    // any unit remains, and releases it with the row once the last unit goes.
    if (consumesStack && !spend_plug_source(chargedAccount, plugDefinition.definitionHash)) {
        return fail("plug_stack");
    }
    profileChanged = profileChanged || consumesStack;

    authored_inventory::Sockets authoredSockets{};
    if (target->sockets.policy == authored_inventory::SocketPolicy::nativeDefaults) {
        if (!materialize_native_sockets(detail, authoredSockets)) {
            return fail("native_sockets");
        }
    } else {
        authoredSockets = target->sockets;
        if (authoredSockets.policy != authored_inventory::SocketPolicy::authored
            || authoredSockets.plugCount != detail.ordinarySocketCount
            || !authored_inventory::valid(authoredSockets)) {
            return fail("authored_sockets");
        }
    }
    if (authoredSockets.plugs[socketLane].has_value()
        && *authoredSockets.plugs[socketLane] == plugDefinition.definitionHash) {
        return fail("already_applied");
    }
    authoredSockets.plugs[socketLane] = plugDefinition.definitionHash;

    CharacterState after = before;
    authored_inventory::Item* changed = character_item_at(after, location);
    if (changed == nullptr || changed->instanceSoid != target->instanceSoid
        || changed->definitionHash != target->definitionHash || changed->level != target->level
        || changed->quantity != target->quantity
        || changed->mutationSerial != target->mutationSerial) {
        return fail("target_copy");
    }
    changed->sockets = authoredSockets;

    AccountState candidate = chargedAccount;
    candidate.characters[characterIndex] = after;
    family4_loadout::ResolvedLoadout afterLoadout{};
    ResolvedPosition beforePosition{};
    ResolvedPosition afterPosition{};
    const family4_loadout::ResolvedItem* resolvedTarget = nullptr;
    for (std::size_t index = 0; index < beforeLoadout.itemCount; ++index) {
        const auto& resolved = beforeLoadout.items[index];
        if (resolved.instance.instanceSoid == targetInstanceSoid
            && resolved.instance.baseDefinitionIndex != targetDefinition.definitionIndex) {
            return fail("before_definition");
        }
    }
    if (!account::valid(candidate)
        || !family4_loadout::resolve(candidate, characterIndex, afterLoadout)
        || !find_resolved_position(beforeLoadout, targetInstanceSoid, beforePosition)
        || !find_resolved_position(afterLoadout, targetInstanceSoid, afterPosition)
        || !same_position(beforePosition, afterPosition)) {
        return fail("candidate_or_position");
    }
    for (std::size_t index = 0; index < afterLoadout.itemCount; ++index) {
        const auto& resolved = afterLoadout.items[index];
        if (resolved.instance.instanceSoid != targetInstanceSoid) {
            continue;
        }
        if (resolvedTarget != nullptr) {
            return fail("duplicate_target");
        }
        resolvedTarget = &resolved;
    }
    if (resolvedTarget == nullptr
        || resolvedTarget->instance.baseDefinitionIndex != targetDefinition.definitionIndex
        || resolvedTarget->instance.ordinarySockets.state
               != middleware::datagen::family4::instance::OrdinarySocketBlockState::present
        || !resolvedTarget->instance.ordinarySockets.plugs[socketLane].has_value()
        || *resolvedTarget->instance.ordinarySockets.plugs[socketLane] != plugDefinitionIndex) {
        return fail("after_socket");
    }

    mutation.beforeCharacter = before;
    mutation.afterCharacter = after;
    mutation.beforeProfileItems = snapshot.profileItems;
    mutation.afterProfileItems = chargedAccount.profileItems;
    mutation.accountSoid = snapshot.primarySoid;
    mutation.characterSoid = before.soid;
    mutation.targetInstanceSoid = targetInstanceSoid;
    mutation.targetDefinitionHash = targetDefinition.definitionHash;
    mutation.plugDefinitionHash = plugDefinition.definitionHash;
    mutation.materialRequirementSetHash = materialSet.requirementSetHash;
    mutation.characterIndex = characterIndex;
    mutation.expectedProfileItemCount = snapshot.profileItemCount;
    mutation.afterProfileItemCount = chargedAccount.profileItemCount;
    mutation.itemIndex = location.index;
    mutation.targetDefinitionIndex = targetDefinition.definitionIndex;
    mutation.plugDefinitionIndex = plugDefinitionIndex;
    mutation.materialRequirementSetIndex = materialSetIndex;
    mutation.socketLane = socketLane;
    mutation.targetBucketId = targetDefinition.bucketId;
    mutation.plugBucketId = plugDefinition.bucketId;
    mutation.materialRequirementCount = materialSet.requirementCount;
    mutation.profileChanged = profileChanged;
    mutation.targetEquipped = location.equipped;
    mutation.prepared = true;
    return true;
}

/** Stages one complete accumulated item-state value without moving or recreating the item. */
[[nodiscard]] bool stage_item_state(const AccountState& snapshot,
                                    std::size_t characterIndex,
                                    std::uint64_t targetInstanceSoid,
                                    std::uint16_t targetDefinitionIndex,
                                    std::uint32_t flags,
                                    PendingItemState& mutation) noexcept {
    mutation = {};
    // Bits 0 and 1 are the two states the client sends. Any other bit is a request we cannot
    // honour.
    constexpr std::uint32_t kSupportedItemStateMask = 0x3U;
    if (!account::valid(snapshot) || characterIndex >= snapshot.characterCount
        || targetInstanceSoid == 0 || (flags & ~kSupportedItemStateMask) != 0) {
        return false;
    }
    const CharacterState& before = snapshot.characters[characterIndex];
    if (!before.selected || before.soid == 0) {
        return false;
    }

    CharacterItemLocation location{};
    family4_loadout::ResolvedLoadout beforeLoadout{};
    if (!find_character_item_location(before, targetInstanceSoid, location)
        || !family4_loadout::resolve(snapshot, characterIndex, beforeLoadout)) {
        return false;
    }
    const authored_inventory::Item* target = character_item_at(before, location);
    build_data::items::Definition definition{};
    ResolvedPosition beforePosition{};
    if (target == nullptr || target->flags == flags
        || !build_data::find_item_definition_hash(target->definitionHash, definition)
        || definition.definitionHash != target->definitionHash
        || definition.definitionIndex != targetDefinitionIndex
        || !find_resolved_position(beforeLoadout, targetInstanceSoid, beforePosition)) {
        return false;
    }

    CharacterState after = before;
    authored_inventory::Item* changed = character_item_at(after, location);
    if (changed == nullptr || !same_stationary_item(*changed, *target)) {
        return false;
    }
    changed->flags = flags;

    AccountState candidate = snapshot;
    candidate.characters[characterIndex] = after;
    family4_loadout::ResolvedLoadout afterLoadout{};
    ResolvedPosition afterPosition{};
    if (!account::valid(candidate)
        || !family4_loadout::resolve(candidate, characterIndex, afterLoadout)
        || !find_resolved_position(afterLoadout, targetInstanceSoid, afterPosition)
        || !same_position(beforePosition, afterPosition)) {
        return false;
    }

    mutation.beforeCharacter = before;
    mutation.afterCharacter = after;
    mutation.characterSoid = before.soid;
    mutation.targetInstanceSoid = targetInstanceSoid;
    mutation.characterIndex = characterIndex;
    mutation.itemIndex = location.index;
    mutation.targetDefinitionIndex = targetDefinitionIndex;
    mutation.beforeFlags = target->flags;
    mutation.afterFlags = flags;
    mutation.targetEquipped = location.equipped;
    mutation.prepared = true;
    return true;
}

/** Returns one character-owned instance's definition hash for bounded transaction diagnostics. */
[[nodiscard]] std::uint32_t character_item_definition_hash(const CharacterState& character,
                                                           std::uint64_t instanceSoid) noexcept {
    for (const auto& item : character.equipment.slots) {
        if (item.has_value() && item->instanceSoid == instanceSoid) {
            return item->definitionHash;
        }
    }
    for (std::size_t index = 0; index < character.inventory.count; ++index) {
        if (character.inventory.values[index].instanceSoid == instanceSoid) {
            return character.inventory.values[index].definitionHash;
        }
    }
    return 0;
}

/** @return True when any account, character, profile-stack, or character-item key owns one SOID. */
[[nodiscard]] bool account_owns_soid(const AccountState& account, std::uint64_t soid) noexcept {
    if (soid == 0 || account.primarySoid == soid) {
        return true;
    }
    for (std::size_t index = 0; index < account.profileItemCount; ++index) {
        if (account.profileItems[index].instanceSoid == soid) {
            return true;
        }
    }
    for (std::size_t characterIndex = 0; characterIndex < account.characterCount;
         ++characterIndex) {
        const CharacterState& character = account.characters[characterIndex];
        if (character.soid == soid) {
            return true;
        }
        for (const auto& item : character.equipment.slots) {
            if (item.has_value() && item->instanceSoid == soid) {
                return true;
            }
        }
        for (std::size_t index = 0; index < character.inventory.count; ++index) {
            if (character.inventory.values[index].instanceSoid == soid) {
                return true;
            }
        }
    }
    return false;
}

/** Finds a fresh deterministic item-instance SOID without sharing any other identity key. */
[[nodiscard]] bool next_item_instance_soid(const AccountState& account,
                                           std::uint64_t& output) noexcept {
    std::uint64_t candidate = kFirstGeneratedItemSoid;
    for (std::size_t characterIndex = 0; characterIndex < account.characterCount;
         ++characterIndex) {
        const CharacterState& character = account.characters[characterIndex];
        for (const auto& item : character.equipment.slots) {
            if (!item.has_value() || item->instanceSoid < candidate) {
                continue;
            }
            if (item->instanceSoid == (std::numeric_limits<std::uint64_t>::max)()) {
                return false;
            }
            candidate = item->instanceSoid + 1U;
        }
        for (std::size_t index = 0; index < character.inventory.count; ++index) {
            const std::uint64_t instanceSoid = character.inventory.values[index].instanceSoid;
            if (instanceSoid < candidate) {
                continue;
            }
            if (instanceSoid == (std::numeric_limits<std::uint64_t>::max)()) {
                return false;
            }
            candidate = instanceSoid + 1U;
        }
    }
    while (account_owns_soid(account, candidate)) {
        if (candidate == (std::numeric_limits<std::uint64_t>::max)()) {
            return false;
        }
        ++candidate;
    }
    output = candidate;
    return output != 0;
}

/** Finds a collision-free SOID for one newly appended profile stack. */
[[nodiscard]] bool next_profile_item_instance_soid(const AccountState& account,
                                                   std::uint64_t& output) noexcept {
    std::uint64_t candidate = authored_inventory::kFirstProfileItemInstanceSoid;
    while (account_owns_soid(account, candidate)) {
        if (candidate == (std::numeric_limits<std::uint64_t>::max)()) {
            return false;
        }
        ++candidate;
    }
    output = candidate;
    return output != 0;
}

/** @return True when an account or character identity already owns the candidate object key. */
[[nodiscard]] bool identity_uses_soid(const AccountState& account, std::uint64_t soid) noexcept {
    if (soid == 0 || account.primarySoid == soid) {
        return true;
    }
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].soid == soid) {
            return true;
        }
    }
    return false;
}

/** Uses the character's strongest existing item as the neutral local Collections pull level. */
[[nodiscard]] std::int32_t acquisition_level(const CharacterState& character) noexcept {
    std::int32_t level = 0;
    for (const auto& item : character.equipment.slots) {
        if (item.has_value()) {
            level = (std::max)(level, item->level);
        }
    }
    for (std::size_t index = 0; index < character.inventory.count; ++index) {
        level = (std::max)(level, character.inventory.values[index].level);
    }
    return level;
}

/** Finds the one resolved unequipped row created by an acquisition candidate. */
[[nodiscard]] bool find_acquired_row(const family4_loadout::ResolvedLoadout& loadout,
                                     std::uint64_t instanceSoid,
                                     std::uint16_t& inventoryRow,
                                     std::uint8_t& equipmentSlot) noexcept {
    bool found = false;
    for (std::size_t index = 0; index < loadout.itemCount; ++index) {
        const family4_loadout::ResolvedItem& item = loadout.items[index];
        if (item.instance.instanceSoid != instanceSoid) {
            continue;
        }
        if (found || item.equipped) {
            return false;
        }
        found = true;
        inventoryRow = item.inventoryRow;
        equipmentSlot = item.equipmentSlot;
    }
    return found;
}

/** Finds the unique resolved unequipped position for one instance. */
[[nodiscard]] bool find_unequipped_row(const family4_loadout::ResolvedLoadout& loadout,
                                       std::uint64_t instanceSoid,
                                       std::uint16_t& inventoryRow,
                                       std::uint8_t& equipmentSlot) noexcept {
    bool found = false;
    for (std::size_t index = 0; index < loadout.itemCount; ++index) {
        const family4_loadout::ResolvedItem& item = loadout.items[index];
        if (item.instance.instanceSoid != instanceSoid) {
            continue;
        }
        if (found || item.equipped) {
            return false;
        }
        found = true;
        inventoryRow = item.inventoryRow;
        equipmentSlot = item.equipmentSlot;
    }
    return found;
}

/** @return True when the resolved loadout still carries an instance with this key. */
[[nodiscard]] bool loadout_contains(const family4_loadout::ResolvedLoadout& loadout,
                                    std::uint64_t instanceSoid) noexcept {
    for (std::size_t index = 0; index < loadout.itemCount; ++index) {
        if (loadout.items[index].instance.instanceSoid == instanceSoid) {
            return true;
        }
    }
    return false;
}

/**
 * Credits the supported client's ordinary weapon/armor dismantle payout.
 *
 * Capped stacks
 * lose only the overflowing part, matching normal profile-inventory behavior.
 * Every credited row
 * receives a new mutation serial so the account observer can display it.
 */
[[nodiscard]] bool
apply_dismantle_rewards(const AccountState& before,
                        std::uint8_t equipmentSlot,
                        AccountState& after,
                        std::array<DismantleReward, kDismantleRewardCapacity>& rewards,
                        std::size_t& rewardCount) noexcept {
    after = before;
    rewards = {};
    rewardCount = 0;
    if (!valid_profile_inventory(before)) {
        return false;
    }
    if (equipmentSlot >= kGearEquipmentSlotCount) {
        return true;
    }

    std::int32_t greatestMutationSerial = 0;
    for (std::size_t index = 0; index < before.profileItemCount; ++index) {
        greatestMutationSerial =
            (std::max)(greatestMutationSerial, before.profileItems[index].mutationSerial);
    }

    for (std::size_t policyIndex = 0; policyIndex < before.dismantleRewardCount; ++policyIndex) {
        const DismantleRewardPolicy& policy = before.dismantleRewards[policyIndex];
        build_data::items::Definition definition{};
        item_details::Definition detail{};
        inventory_buckets::Descriptor bucket{};
        if (policy.definitionHash == authored_inventory::kNoDefinitionHash || policy.quantity <= 0
            || !build_data::find_item_definition_hash(policy.definitionHash, definition)
            || definition.definitionHash != policy.definitionHash
            || !build_data::find_configured_item_detail(definition.definitionIndex, detail)
            || detail.definitionIndex != definition.definitionIndex
            || detail.definitionHash != definition.definitionHash
            || detail.bucketId != definition.bucketId
            || detail.instancedDefinitionState != item_details::InstancedDefinitionState::stackable
            || detail.maxStackSize <= 0
            || !build_data::find_inventory_bucket_descriptor(detail.bucketId, bucket)
            || bucket.arraySelector != inventory_buckets::ArraySelector::profile
            || build_data::is_profile_action_source(definition.definitionIndex,
                                                    definition.bucketId)) {
            return false;
        }

        std::size_t profileIndex = after.profileItemCount;
        for (std::size_t index = 0; index < after.profileItemCount; ++index) {
            const authored_inventory::ProfileItem& item = after.profileItems[index];
            if (item.definitionHash != policy.definitionHash) {
                continue;
            }
            if (item.instanceSoid != 0 || item.quantity <= 0
                || item.quantity > detail.maxStackSize) {
                return false;
            }
            if (profileIndex == after.profileItemCount && item.quantity < detail.maxStackSize) {
                profileIndex = index;
            }
        }

        const bool appended = profileIndex == after.profileItemCount;
        if ((appended && after.profileItemCount >= after.profileItems.size())
            || greatestMutationSerial == (std::numeric_limits<std::int32_t>::max)()) {
            continue;
        }
        const std::int32_t previousQuantity =
            appended ? 0 : after.profileItems[profileIndex].quantity;
        const std::int32_t available = detail.maxStackSize - previousQuantity;
        const std::int32_t credited = (std::min)(policy.quantity, available);
        if (credited <= 0) {
            continue;
        }

        AccountState candidate = after;
        const std::int32_t mutationSerial = greatestMutationSerial + 1;
        const std::int32_t afterQuantity = previousQuantity + credited;
        if (appended) {
            candidate.profileItems[profileIndex] = {
                0, policy.definitionHash, afterQuantity, mutationSerial};
            ++candidate.profileItemCount;
        } else {
            candidate.profileItems[profileIndex].quantity = afterQuantity;
            candidate.profileItems[profileIndex].mutationSerial = mutationSerial;
        }
        // A full native bucket drops this reward, but never blocks deletion of the source item.
        if (!account::valid(candidate) || !valid_profile_inventory(candidate)) {
            continue;
        }
        if (rewardCount >= rewards.size()) {
            return false;
        }
        after = candidate;
        greatestMutationSerial = mutationSerial;
        rewards[rewardCount++] = {
            policy.definitionHash, profileIndex, credited, afterQuantity, mutationSerial};
    }
    return account::valid(after) && valid_profile_inventory(after);
}

/**
 * Builds the one canonical dismantle transition for an exact account snapshot.
 *
 * Surviving authored entries keep their mutation generation unless installed row placement moves
 * them. Generation capacity is checked for every move before any survivor is changed.
 */
[[nodiscard]] bool stage_item_dismantle(const AccountState& account,
                                        std::size_t characterIndex,
                                        std::uint64_t instanceSoid,
                                        PendingItemDismantle& mutation) noexcept {
    mutation = {};
    if (instanceSoid == 0 || !account::valid(account) || characterIndex >= account.characterCount
        || !account.characters[characterIndex].selected) {
        return false;
    }

    const CharacterState& before = account.characters[characterIndex];
    std::size_t inventoryIndex = before.inventory.count;
    for (std::size_t index = 0; index < before.inventory.count; ++index) {
        if (before.inventory.values[index].instanceSoid == instanceSoid) {
            inventoryIndex = index;
            break;
        }
    }
    if (inventoryIndex >= before.inventory.count
        || (before.inventory.values[inventoryIndex].flags & authored_inventory::kLockedItemFlag)
               != 0) {
        return false;
    }

    family4_loadout::ResolvedLoadout beforeLoadout{};
    std::uint16_t dismantledRow = 0;
    std::uint8_t dismantledSlot = 0;
    if (!family4_loadout::resolve(account, characterIndex, beforeLoadout)
        || before.nextInventorySerial
               > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())
        || !find_unequipped_row(beforeLoadout, instanceSoid, dismantledRow, dismantledSlot)) {
        return false;
    }

    CharacterState after = before;
    const authored_inventory::Item dismantledItem = after.inventory.values[inventoryIndex];
    for (std::size_t index = inventoryIndex; index + 1U < after.inventory.count; ++index) {
        after.inventory.values[index] = after.inventory.values[index + 1U];
    }
    --after.inventory.count;
    after.inventory.values[after.inventory.count] = {};

    AccountState candidate = account;
    candidate.characters[characterIndex] = after;
    family4_loadout::ResolvedLoadout placedAfter{};
    if (!account::valid(candidate)
        || !family4_loadout::resolve(candidate, characterIndex, placedAfter)
        || loadout_contains(placedAfter, instanceSoid)
        || beforeLoadout.itemCount != placedAfter.itemCount + 1U) {
        return false;
    }

    std::size_t movedItemCount = 0;
    for (std::size_t index = 0; index < after.inventory.count; ++index) {
        const std::uint64_t survivorSoid = after.inventory.values[index].instanceSoid;
        std::uint16_t beforeRow = 0;
        std::uint16_t afterRow = 0;
        std::uint8_t beforeSlot = 0;
        std::uint8_t afterSlot = 0;
        if (!find_unequipped_row(beforeLoadout, survivorSoid, beforeRow, beforeSlot)
            || !find_unequipped_row(placedAfter, survivorSoid, afterRow, afterSlot)
            || beforeSlot != afterSlot) {
            return false;
        }
        movedItemCount += static_cast<std::size_t>(beforeRow != afterRow);
    }

    // The serial is signed on the wire, so it must stay inside the positive int32 range.
    constexpr std::uint32_t kMaximumInventorySerial =
        static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());
    if (after.nextInventorySerial > kMaximumInventorySerial
        || movedItemCount > kMaximumInventorySerial - after.nextInventorySerial) {
        return false;
    }

    for (std::size_t index = 0; index < after.inventory.count; ++index) {
        const std::uint64_t survivorSoid = after.inventory.values[index].instanceSoid;
        std::uint16_t beforeRow = 0;
        std::uint16_t afterRow = 0;
        std::uint8_t beforeSlot = 0;
        std::uint8_t afterSlot = 0;
        if (!find_unequipped_row(beforeLoadout, survivorSoid, beforeRow, beforeSlot)
            || !find_unequipped_row(placedAfter, survivorSoid, afterRow, afterSlot)
            || beforeSlot != afterSlot) {
            return false;
        }
        if (beforeRow != afterRow) {
            after.inventory.values[index].mutationSerial =
                static_cast<std::int32_t>(after.nextInventorySerial++);
        }
    }

    candidate.characters[characterIndex] = after;
    family4_loadout::ResolvedLoadout checkedAfter{};
    if (!account::valid(candidate)
        || !family4_loadout::resolve(candidate, characterIndex, checkedAfter)
        || checkedAfter.itemCount != placedAfter.itemCount
        || loadout_contains(checkedAfter, instanceSoid)) {
        return false;
    }
    for (std::size_t index = 0; index < after.inventory.count; ++index) {
        const std::uint64_t survivorSoid = after.inventory.values[index].instanceSoid;
        std::uint16_t placedRow = 0;
        std::uint16_t checkedRow = 0;
        std::uint8_t placedSlot = 0;
        std::uint8_t checkedSlot = 0;
        if (!find_unequipped_row(placedAfter, survivorSoid, placedRow, placedSlot)
            || !find_unequipped_row(checkedAfter, survivorSoid, checkedRow, checkedSlot)
            || placedRow != checkedRow || placedSlot != checkedSlot) {
            return false;
        }
    }

    build_data::items::Definition dismantledDefinition{};
    item_details::Definition dismantledDetail{};
    if (!build_data::find_item_definition_hash(dismantledItem.definitionHash, dismantledDefinition)
        || dismantledDefinition.definitionHash != dismantledItem.definitionHash
        || !build_data::find_configured_item_detail(dismantledDefinition.definitionIndex,
                                                    dismantledDetail)
        || dismantledDetail.definitionIndex != dismantledDefinition.definitionIndex
        || dismantledDetail.definitionHash != dismantledDefinition.definitionHash
        || dismantledDetail.bucketId != dismantledDefinition.bucketId
        || dismantledDetail.instancedDefinitionState
               != item_details::InstancedDefinitionState::instanced
        || !dismantledDetail.equipmentSlot.has_value()
        || static_cast<std::uint8_t>(*dismantledDetail.equipmentSlot) != dismantledSlot) {
        return false;
    }

    AccountState rewarded{};
    std::array<DismantleReward, kDismantleRewardCapacity> rewards{};
    std::size_t rewardCount = 0;
    if (!apply_dismantle_rewards(candidate, dismantledSlot, rewarded, rewards, rewardCount)) {
        return false;
    }
    candidate = rewarded;

    mutation.beforeCharacter = before;
    mutation.afterCharacter = after;
    mutation.beforeProfileItems = account.profileItems;
    mutation.afterProfileItems = candidate.profileItems;
    mutation.rewards = rewards;
    mutation.dismantledItem = dismantledItem;
    mutation.accountSoid = account.primarySoid;
    mutation.characterSoid = before.soid;
    mutation.dismantledInstanceSoid = instanceSoid;
    mutation.characterIndex = characterIndex;
    mutation.expectedInventoryCount = before.inventory.count;
    mutation.expectedProfileItemCount = account.profileItemCount;
    mutation.afterProfileItemCount = candidate.profileItemCount;
    mutation.inventoryIndex = inventoryIndex;
    mutation.movedInventoryItemCount = movedItemCount;
    mutation.rewardCount = rewardCount;
    mutation.inventoryRow = dismantledRow;
    mutation.equipmentSlot = dismantledSlot;
    mutation.profileChanged = rewardCount != 0;
    mutation.prepared = true;
    return true;
}

/** @return True when both descriptions name the same credited profile mutation. */
[[nodiscard]] bool same_dismantle_reward(const DismantleReward& left,
                                         const DismantleReward& right) noexcept {
    return left.definitionHash == right.definitionHash && left.profileIndex == right.profileIndex
           && left.quantity == right.quantity && left.afterQuantity == right.afterQuantity
           && left.mutationSerial == right.mutationSerial;
}

/** @return True when two independently staged dismantles carry the exact same after-images. */
[[nodiscard]] bool same_dismantle_transition(const PendingItemDismantle& left,
                                             const PendingItemDismantle& right) noexcept {
    if (left.prepared != right.prepared || left.accountSoid != right.accountSoid
        || left.characterSoid != right.characterSoid
        || left.dismantledInstanceSoid != right.dismantledInstanceSoid
        || left.characterIndex != right.characterIndex
        || left.expectedInventoryCount != right.expectedInventoryCount
        || left.expectedProfileItemCount != right.expectedProfileItemCount
        || left.afterProfileItemCount != right.afterProfileItemCount
        || left.inventoryIndex != right.inventoryIndex
        || left.movedInventoryItemCount != right.movedInventoryItemCount
        || left.rewardCount != right.rewardCount || left.inventoryRow != right.inventoryRow
        || left.equipmentSlot != right.equipmentSlot || left.profileChanged != right.profileChanged
        || !same_stationary_item(left.dismantledItem, right.dismantledItem)
        || !same_character(left.beforeCharacter, right.beforeCharacter)
        || !same_character(left.afterCharacter, right.afterCharacter)
        || !same_profile_views(left.beforeProfileItems,
                               left.expectedProfileItemCount,
                               right.beforeProfileItems,
                               right.expectedProfileItemCount)
        || !same_profile_views(left.afterProfileItems,
                               left.afterProfileItemCount,
                               right.afterProfileItems,
                               right.afterProfileItemCount)) {
        return false;
    }
    for (std::size_t index = 0; index < left.rewards.size(); ++index) {
        if (!same_dismantle_reward(left.rewards[index], right.rewards[index])) {
            return false;
        }
    }
    return true;
}

/** Applies a fully checked dismantle after-image over an exact current account view. */
[[nodiscard]] bool materialize_item_dismantle(const AccountState& current,
                                              const PendingItemDismantle& mutation,
                                              AccountState& after) noexcept {
    after = {};
    if (!mutation.prepared || mutation.accountSoid == 0 || mutation.characterSoid == 0
        || mutation.dismantledInstanceSoid == 0
        || mutation.dismantledItem.instanceSoid != mutation.dismantledInstanceSoid
        || mutation.dismantledItem.definitionHash == authored_inventory::kNoDefinitionHash
        || mutation.characterIndex >= current.characterCount || mutation.expectedInventoryCount == 0
        || mutation.expectedInventoryCount > authored_inventory::kCharacterItemCapacity
        || mutation.expectedProfileItemCount > authored_inventory::kProfileItemCapacity
        || mutation.afterProfileItemCount > authored_inventory::kProfileItemCapacity
        || mutation.inventoryIndex >= mutation.expectedInventoryCount
        || mutation.rewardCount > mutation.rewards.size()
        || mutation.profileChanged != (mutation.rewardCount != 0)
        || mutation.beforeCharacter.soid != mutation.characterSoid
        || mutation.afterCharacter.soid != mutation.characterSoid
        || mutation.beforeCharacter.inventory.count != mutation.expectedInventoryCount
        || mutation.afterCharacter.inventory.count + 1U != mutation.expectedInventoryCount
        || !same_stationary_item(mutation.beforeCharacter.inventory.values[mutation.inventoryIndex],
                                 mutation.dismantledItem)
        || current.primarySoid != mutation.accountSoid
        || !same_profile_inventory(
            current, mutation.beforeProfileItems, mutation.expectedProfileItemCount)) {
        return false;
    }
    const CharacterState& character = current.characters[mutation.characterIndex];
    if (!character.selected || character.soid != mutation.characterSoid
        || !same_character(character, mutation.beforeCharacter)) {
        return false;
    }
    for (std::size_t index = 0; index < mutation.rewards.size(); ++index) {
        const DismantleReward& reward = mutation.rewards[index];
        if (index < mutation.rewardCount) {
            if (reward.definitionHash == authored_inventory::kNoDefinitionHash
                || reward.profileIndex >= mutation.afterProfileItemCount || reward.quantity <= 0
                || reward.afterQuantity < reward.quantity || reward.mutationSerial <= 0) {
                return false;
            }
            const authored_inventory::ProfileItem& row =
                mutation.afterProfileItems[reward.profileIndex];
            if (row.instanceSoid != 0 || row.definitionHash != reward.definitionHash
                || row.quantity != reward.afterQuantity
                || row.mutationSerial != reward.mutationSerial) {
                return false;
            }
        } else if (reward.definitionHash != 0 || reward.profileIndex != 0 || reward.quantity != 0
                   || reward.afterQuantity != 0 || reward.mutationSerial != 0) {
            return false;
        }
    }
    if (!mutation.profileChanged
        && !same_profile_views(mutation.beforeProfileItems,
                               mutation.expectedProfileItemCount,
                               mutation.afterProfileItems,
                               mutation.afterProfileItemCount)) {
        return false;
    }

    PendingItemDismantle canonical{};
    if (!stage_item_dismantle(
            current, mutation.characterIndex, mutation.dismantledInstanceSoid, canonical)
        || !same_dismantle_transition(canonical, mutation)) {
        return false;
    }

    after = current;
    after.characters[mutation.characterIndex] = mutation.afterCharacter;
    after.profileItems = mutation.afterProfileItems;
    after.profileItemCount = mutation.afterProfileItemCount;
    return account::valid(after) && valid_profile_inventory(after)
           && !identity_uses_soid(after, mutation.dismantledInstanceSoid);
}

} // namespace runtime::detail

using namespace runtime::detail;

/** Stores the active account key without publishing an incomplete account. */
bool set_primary_soid(std::uint64_t primarySoid) noexcept {
    if (primarySoid == 0) {
        return false;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState candidate = runtime::storage::g_state.account;
    candidate.primarySoid = primarySoid;
    // Characters belong to the account key the Client uses. The reference account and its
    // characters differ only in the low byte, so the authored rows are rebased onto that key.
    for (std::size_t index = 0; index < candidate.characterCount; ++index) {
        candidate.characters[index].soid = primarySoid + 1U + index;
    }
    if (!account::valid(candidate)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    // Publish only after the settings and identity rules hold together.
    runtime::storage::g_state.account = candidate;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return true;
}

/** Moves the selection to one authored character. */
bool set_selected_character(std::uint64_t characterSoid, bool& changed) noexcept {
    changed = false;
    if (characterSoid == 0) {
        return false;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState candidate = runtime::storage::g_state.account;
    std::size_t picked = candidate.characterCount;
    for (std::size_t index = 0; index < candidate.characterCount; ++index) {
        if (candidate.characters[index].soid == characterSoid) {
            picked = index;
        }
    }
    if (picked == candidate.characterCount) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }

    const bool alreadySelected = candidate.characters[picked].selected;
    for (CharacterState& character : candidate.characters) {
        character.selected = false;
    }
    candidate.characters[picked].selected = true;
    if (!account::valid(candidate)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    // Publish only after the whole account still meets its identity rules.
    runtime::storage::g_state.account = candidate;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    changed = !alreadySelected;
    return true;
}

/** Prepares one checked equip transition without changing account State. */
bool prepare_equipment_swap(std::uint64_t requestedInstanceSoid,
                            PendingEquipmentSwap& mutation) noexcept {
    mutation = {};
    const AccountState account = account_snapshot();
    if (requestedInstanceSoid == 0 || !account::valid(account)) {
        return false;
    }

    std::size_t characterIndex = account.characterCount;
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].selected) {
            characterIndex = index;
            break;
        }
    }
    if (characterIndex == account.characterCount) {
        return false;
    }

    const CharacterState& before = account.characters[characterIndex];
    family4_loadout::ResolvedLoadout beforeLoadout{};
    if (!family4_loadout::resolve(account, characterIndex, beforeLoadout)) {
        return false;
    }

    std::size_t inventoryIndex = before.inventory.count;
    for (std::size_t index = 0; index < before.inventory.count; ++index) {
        if (before.inventory.values[index].instanceSoid != requestedInstanceSoid) {
            continue;
        }
        if (inventoryIndex != before.inventory.count) {
            return false;
        }
        inventoryIndex = index;
    }
    if (inventoryIndex == before.inventory.count) {
        return false;
    }

    const authored_inventory::Item& requested = before.inventory.values[inventoryIndex];
    std::uint8_t requestedNativeSlot = 0;
    std::size_t equipmentSlotIndex = authored_inventory::kEquipmentSlotCount;
    ResolvedPosition requestedPosition{};
    if (!native_equipment_slot(requested, requestedNativeSlot)
        || !semantic_equipment_slot(requestedNativeSlot, equipmentSlotIndex)
        || !find_resolved_position(beforeLoadout, requestedInstanceSoid, requestedPosition)
        || requestedPosition.equipped || requestedPosition.equipmentSlot != requestedNativeSlot) {
        return false;
    }

    CharacterState after = before;
    auto& equipped = after.equipment.slots[equipmentSlotIndex];
    std::uint64_t previousInstanceSoid = 0;
    std::uint32_t previousDefinitionHash = 0;
    if (equipped.has_value()) {
        std::uint8_t previousNativeSlot = 0;
        ResolvedPosition previousPosition{};
        if (!native_equipment_slot(*equipped, previousNativeSlot)
            || previousNativeSlot != requestedNativeSlot
            || !find_resolved_position(beforeLoadout, equipped->instanceSoid, previousPosition)
            || !previousPosition.equipped
            || previousPosition.equipmentSlot != requestedNativeSlot) {
            return false;
        }
        previousInstanceSoid = equipped->instanceSoid;
        previousDefinitionHash = equipped->definitionHash;
        std::swap(*equipped, after.inventory.values[inventoryIndex]);
    } else {
        equipped = after.inventory.values[inventoryIndex];
        for (std::size_t index = inventoryIndex; index + 1U < after.inventory.count; ++index) {
            after.inventory.values[index] = after.inventory.values[index + 1U];
        }
        --after.inventory.count;
        after.inventory.values[after.inventory.count] = {};
    }

    std::size_t movedItemCount = 0;
    if (!finalize_equipment_transition(account,
                                       characterIndex,
                                       requestedInstanceSoid,
                                       EquipmentMutationKind::equip,
                                       requestedNativeSlot,
                                       beforeLoadout,
                                       after,
                                       movedItemCount)) {
        return false;
    }

    mutation.beforeCharacter = before;
    mutation.afterCharacter = after;
    mutation.characterSoid = before.soid;
    mutation.requestedInstanceSoid = requestedInstanceSoid;
    mutation.previousInstanceSoid = previousInstanceSoid;
    mutation.characterIndex = characterIndex;
    mutation.equipmentSlotIndex = equipmentSlotIndex;
    mutation.inventoryIndex = inventoryIndex;
    mutation.movedItemCount = movedItemCount;
    mutation.nativeEquipmentSlot = requestedNativeSlot;
    mutation.kind = EquipmentMutationKind::equip;
    mutation.prepared = true;
    report_equipment("prepare",
                     "ok",
                     mutation.kind,
                     mutation.characterSoid,
                     mutation.previousInstanceSoid,
                     mutation.requestedInstanceSoid,
                     mutation.equipmentSlotIndex,
                     mutation.inventoryIndex,
                     mutation.nativeEquipmentSlot,
                     mutation.movedItemCount,
                     previousDefinitionHash,
                     requested.definitionHash);
    return true;
}

/** Prepares one checked equipped-to-inventory transition without changing account State. */
bool prepare_equipment_unequip(std::uint64_t requestedInstanceSoid,
                               PendingEquipmentSwap& mutation) noexcept {
    mutation = {};
    const AccountState account = account_snapshot();
    if (requestedInstanceSoid == 0 || !account::valid(account)) {
        return false;
    }

    std::size_t characterIndex = account.characterCount;
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].selected) {
            characterIndex = index;
            break;
        }
    }
    if (characterIndex == account.characterCount) {
        return false;
    }

    const CharacterState& before = account.characters[characterIndex];
    if (before.inventory.count >= before.inventory.values.size()) {
        return false;
    }
    family4_loadout::ResolvedLoadout beforeLoadout{};
    if (!family4_loadout::resolve(account, characterIndex, beforeLoadout)) {
        return false;
    }

    std::size_t equipmentSlotIndex = before.equipment.slots.size();
    for (std::size_t index = 0; index < before.equipment.slots.size(); ++index) {
        const auto& item = before.equipment.slots[index];
        if (!item.has_value() || item->instanceSoid != requestedInstanceSoid) {
            continue;
        }
        if (equipmentSlotIndex != before.equipment.slots.size()) {
            return false;
        }
        equipmentSlotIndex = index;
    }
    if (equipmentSlotIndex == before.equipment.slots.size()) {
        return false;
    }

    const authored_inventory::Item& requested = *before.equipment.slots[equipmentSlotIndex];
    std::uint8_t requestedNativeSlot = 0;
    std::uint8_t requestedBucketId = 0;
    std::size_t expectedSemanticIndex = authored_inventory::kEquipmentSlotCount;
    ResolvedPosition requestedPosition{};
    if (!native_equipment_slot(requested, requestedNativeSlot)
        || !inventory_bucket_id(requested, requestedBucketId)
        || !semantic_equipment_slot(requestedNativeSlot, expectedSemanticIndex)
        || expectedSemanticIndex != equipmentSlotIndex
        || !find_resolved_position(beforeLoadout, requestedInstanceSoid, requestedPosition)
        || !requestedPosition.equipped || requestedPosition.equipmentSlot != requestedNativeSlot) {
        return false;
    }

    std::size_t inventoryIndex = before.inventory.count;
    for (std::size_t index = 0; index < before.inventory.count; ++index) {
        std::uint8_t inventoryBucketId = 0;
        if (!inventory_bucket_id(before.inventory.values[index], inventoryBucketId)) {
            return false;
        }
        if (inventoryBucketId == requestedBucketId) {
            inventoryIndex = index;
            break;
        }
    }

    CharacterState after = before;
    const authored_inventory::Item unequipped = *after.equipment.slots[equipmentSlotIndex];
    for (std::size_t index = after.inventory.count; index > inventoryIndex; --index) {
        after.inventory.values[index] = after.inventory.values[index - 1U];
    }
    after.inventory.values[inventoryIndex] = unequipped;
    ++after.inventory.count;
    after.equipment.slots[equipmentSlotIndex].reset();

    std::size_t movedItemCount = 0;
    if (!finalize_equipment_transition(account,
                                       characterIndex,
                                       requestedInstanceSoid,
                                       EquipmentMutationKind::unequip,
                                       requestedNativeSlot,
                                       beforeLoadout,
                                       after,
                                       movedItemCount)) {
        return false;
    }

    mutation.beforeCharacter = before;
    mutation.afterCharacter = after;
    mutation.characterSoid = before.soid;
    mutation.requestedInstanceSoid = requestedInstanceSoid;
    mutation.characterIndex = characterIndex;
    mutation.equipmentSlotIndex = equipmentSlotIndex;
    mutation.inventoryIndex = inventoryIndex;
    mutation.movedItemCount = movedItemCount;
    mutation.nativeEquipmentSlot = requestedNativeSlot;
    mutation.kind = EquipmentMutationKind::unequip;
    mutation.prepared = true;
    report_equipment("prepare",
                     "ok",
                     mutation.kind,
                     mutation.characterSoid,
                     mutation.previousInstanceSoid,
                     mutation.requestedInstanceSoid,
                     mutation.equipmentSlotIndex,
                     mutation.inventoryIndex,
                     mutation.nativeEquipmentSlot,
                     mutation.movedItemCount,
                     0,
                     requested.definitionHash);
    return true;
}

/** Commits one prepared equipment after-image behind an exact character staleness guard. */
bool commit_equipment_swap(PendingEquipmentSwap& mutation) noexcept {
    const PendingEquipmentSwap prepared = mutation;
    mutation = {};
    if (!prepared.prepared || prepared.characterSoid == 0 || prepared.requestedInstanceSoid == 0
        || (prepared.kind != EquipmentMutationKind::equip
            && prepared.kind != EquipmentMutationKind::unequip)
        || prepared.characterIndex >= kCharacterCapacity
        || prepared.equipmentSlotIndex >= authored_inventory::kEquipmentSlotCount
        || prepared.inventoryIndex >= authored_inventory::kCharacterItemCapacity
        || prepared.nativeEquipmentSlot >= item_details::kEquipmentSlotCount
        || prepared.beforeCharacter.soid != prepared.characterSoid
        || prepared.afterCharacter.soid != prepared.characterSoid) {
        return false;
    }

    const std::uint32_t previousDefinitionHash =
        character_item_definition_hash(prepared.afterCharacter, prepared.previousInstanceSoid);
    const std::uint32_t requestedDefinitionHash =
        character_item_definition_hash(prepared.afterCharacter, prepared.requestedInstanceSoid);
    report_equipment("commit_begin",
                     "ok",
                     prepared.kind,
                     prepared.characterSoid,
                     prepared.previousInstanceSoid,
                     prepared.requestedInstanceSoid,
                     prepared.equipmentSlotIndex,
                     prepared.inventoryIndex,
                     prepared.nativeEquipmentSlot,
                     prepared.movedItemCount,
                     previousDefinitionHash,
                     requestedDefinitionHash);

    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState candidate = runtime::storage::g_state.account;
    if (prepared.characterIndex >= candidate.characterCount
        || !same_character(candidate.characters[prepared.characterIndex],
                           prepared.beforeCharacter)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    candidate.characters[prepared.characterIndex] = prepared.afterCharacter;
    family4_loadout::ResolvedLoadout checkedAfter{};
    if (!account::valid(candidate)
        || !family4_loadout::resolve(candidate, prepared.characterIndex, checkedAfter)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    ResolvedPosition requestedPosition{};
    const bool expectedEquipped = prepared.kind == EquipmentMutationKind::equip;
    if (!find_resolved_position(checkedAfter, prepared.requestedInstanceSoid, requestedPosition)
        || requestedPosition.equipmentSlot != prepared.nativeEquipmentSlot
        || requestedPosition.equipped != expectedEquipped) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    runtime::storage::g_state.account = candidate;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);

    report_equipment("commit_end",
                     "ok",
                     prepared.kind,
                     prepared.characterSoid,
                     prepared.previousInstanceSoid,
                     prepared.requestedInstanceSoid,
                     prepared.equipmentSlotIndex,
                     prepared.inventoryIndex,
                     prepared.nativeEquipmentSlot,
                     prepared.movedItemCount,
                     previousDefinitionHash,
                     requestedDefinitionHash);
    return true;
}

/** @return A copy of the active account state, read under the lock. */
AccountState account_snapshot() noexcept {
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const AccountState snapshot = runtime::storage::g_state.account;
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return snapshot;
}

} // namespace sunrise::state
