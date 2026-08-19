#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../state/build_data/entity_names/definition.h"

namespace sunrise::client::content::entity_names::localized_aliases {

using Entry = state::build_data::entity_names::Name;
inline constexpr std::size_t kNameCapacity = state::build_data::entity_names::kNameLength;

struct Result {
    std::size_t wrappers{};
    std::size_t placements{};
    std::size_t resolved{};
};

[[nodiscard]] bool append(const middleware::content::packages::reader::Source& source,
                          middleware::content::packages::reader::Scratch& scratch,
                          std::vector<Entry>& output,
                          Result& result) noexcept;

} // namespace sunrise::client::content::entity_names::localized_aliases
