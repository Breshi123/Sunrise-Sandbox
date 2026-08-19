#include "../inventory/buckets/inventory_bucket_catalog.h"
#include "../items/details/definition.h"
#include "../items/item_catalog.h"
#include "../runtime.h"
#include "../socket_entry_lists/socket_entry_list_catalog.h"
#include "persistence/publication_transaction.h"

namespace sunrise::state::build_data {

/** @return True when the whole inventory-bucket descriptor table is in State. */
bool inventory_bucket_descriptors_ready() noexcept {
    return inventory::buckets::count() != 0;
}

/** Publishes one whole inventory-bucket descriptor table. */
bool publish_inventory_bucket_descriptors(
    std::span<const inventory::buckets::Descriptor> descriptors) noexcept {
    runtime::persistence::Transaction transaction;
    return transaction.active()
           && transaction.finish(inventory::buckets::replace(descriptors),
                                 inventory::buckets::clear);
}

/** Finds one inventory-bucket descriptor, once the whole table is in State. */
bool find_inventory_bucket_descriptor(std::uint8_t bucketId,
                                      inventory::buckets::Descriptor& descriptor) noexcept {
    descriptor = {};
    return inventory_bucket_descriptors_ready() && inventory::buckets::find(bucketId, descriptor);
}

/** @return True when the whole dense socket-entry-list table is in State. */
bool socket_entry_lists_ready() noexcept {
    return socket_entry_lists::count() != 0;
}

/** @return Dense item-definition row count, read under the lock. */
std::size_t item_definition_count() noexcept {
    return items::count();
}

/** @return Dense socket-entry-list row count, read under the lock. */
std::size_t socket_entry_list_count() noexcept {
    return socket_entry_lists::count();
}

/** Publishes one whole dense socket-entry-list table. */
bool publish_socket_entry_lists(
    std::span<const socket_entry_lists::Definition> definitions,
    std::span<const socket_entry_lists::EntryTable> entryTables) noexcept {
    runtime::persistence::Transaction transaction;
    if (!transaction.active() || !socket_entry_lists::valid(definitions)
        || !socket_entry_lists::valid_entry_tables(entryTables)) {
        return false;
    }
    return transaction.finish(socket_entry_lists::replace(definitions)
                                  && socket_entry_lists::replace_entry_tables(entryTables),
                              socket_entry_lists::clear);
}

/** Finds one list's per-entry selection inputs. */
bool find_socket_entry_table(std::uint16_t definitionIndex,
                             socket_entry_lists::EntryTable& table) noexcept {
    return socket_entry_lists::find_entry_table(definitionIndex, table);
}

/** Finds one socket-entry-list definition, once the whole table is in State. */
bool find_socket_entry_list(std::uint16_t definitionIndex,
                            socket_entry_lists::Definition& definition) noexcept {
    definition = {};
    return socket_entry_lists_ready() && socket_entry_lists::find(definitionIndex, definition);
}

/** Finds the 2 other subclasses sharing one character class with a known member. */
bool find_subclass_group(std::uint16_t memberDefinitionIndex,
                         std::array<std::uint16_t, kSubclassGroupSize>& group) noexcept {
    group.fill(0);
    const std::size_t itemCount = item_definition_count();
    if (itemCount == 0) {
        return false;
    }
    // Every subclass item (any item carrying a socket-entry-list), in native definition-index
    // order. The installed manifest lists these as one dense run of kSubclassGroupSize per class.
    std::array<std::uint16_t, socket_entry_lists::kEntryTableCapacity> subclasses{};
    std::size_t subclassCount = 0;
    for (std::size_t index = 0; index < itemCount && subclassCount < subclasses.size(); ++index) {
        items::details::Definition detail{};
        socket_entry_lists::EntryTable entries{};
        if (find_configured_item_detail(static_cast<std::uint16_t>(index), detail)
            && detail.definitionIndex == index
            && find_socket_entry_table(detail.socketEntryListIndex, entries)) {
            subclasses[subclassCount++] = static_cast<std::uint16_t>(index);
        }
    }
    for (std::size_t base = 0; base + kSubclassGroupSize <= subclassCount;
         base += kSubclassGroupSize) {
        const bool matches = subclasses[base] == memberDefinitionIndex
                             || subclasses[base + 1] == memberDefinitionIndex
                             || subclasses[base + 2] == memberDefinitionIndex;
        if (!matches) {
            continue;
        }
        group[0] = subclasses[base];
        group[1] = subclasses[base + 1];
        group[2] = subclasses[base + 2];
        return true;
    }
    return false;
}

} // namespace sunrise::state::build_data
