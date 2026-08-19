#pragma once

#include <cstddef>
#include <span>

#include "definition.h"

namespace sunrise::state::build_data::socket_entry_buckets {

/** Clears every resolved entry-bucket row. */
void clear() noexcept;

/**
 * Checks that no two rows name the same socket-entry list.
 * @param definitions Candidate rows.
 * @return True when the rows fit storage and every key is unique.
 */
[[nodiscard]] bool valid(std::span<const Definition> definitions) noexcept;

/**
 * Replaces the resolved entry-bucket rows in one step.
 * @param definitions Complete rows in any order.
 * @return True when the rows pass the checks and fit fixed State storage.
 */
[[nodiscard]] bool replace(std::span<const Definition> definitions) noexcept;

/**
 * Finds one socket-entry list's resolved per-entry ability-bucket destinations.
 * @param socketEntryListIndex Native socket-entry-list index of the subclass.
 * @param definition Receives the matching row.
 * @return True when a row carries that exact key.
 */
[[nodiscard]] bool find(std::uint16_t socketEntryListIndex, Definition& definition) noexcept;

/** @return Number of resolved entry-bucket rows, read under the lock. */
[[nodiscard]] std::size_t count() noexcept;

} // namespace sunrise::state::build_data::socket_entry_buckets
