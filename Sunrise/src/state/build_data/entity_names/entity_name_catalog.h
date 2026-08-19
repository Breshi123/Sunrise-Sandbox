#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "definition.h"

namespace sunrise::state::build_data::entity_names {

void clear() noexcept;
[[nodiscard]] bool valid(std::span<const Name> names) noexcept;
[[nodiscard]] bool replace(std::span<const Name> names) noexcept;
[[nodiscard]] bool find(std::uint32_t tag, Name& name) noexcept;
[[nodiscard]] bool snapshot(std::span<Name> output, std::size_t& count) noexcept;
[[nodiscard]] std::size_t count() noexcept;

} // namespace sunrise::state::build_data::entity_names
