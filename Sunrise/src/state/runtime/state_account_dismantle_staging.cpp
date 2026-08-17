/** Dismantle staging: the payout it credits and the after-image it is committed against. */

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

/** Equipment slots 0-2 are weapons and 3-7 are class-specific armor. */
constexpr std::uint8_t kGearEquipmentSlotCount = 8;

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
} // namespace sunrise::state
