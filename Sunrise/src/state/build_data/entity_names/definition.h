#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::state::build_data::entity_names {

inline constexpr std::size_t kNameCapacity = 16'384;
inline constexpr std::size_t kNameLength = 128;

struct Name {
    std::uint32_t tag{};
    std::array<char, kNameLength> text{};
    std::uint8_t length{};
};

} // namespace sunrise::state::build_data::entity_names
