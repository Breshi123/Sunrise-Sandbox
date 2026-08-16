#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::state::build_data::material_requirements {

/** Installed build currently carries 229 dense sets; keep bounded headroom. */
inline constexpr std::size_t kDefinitionCapacity = 512;
/** Widest installed material-requirement set contains six rows. */
inline constexpr std::size_t kRequirementCapacity = 6;
/** All item-index bits set mean the row does not name an installed item. */
inline constexpr std::uint16_t kUnavailableItemDefinitionIndex = 0xFFFFU;
/** All requirement-index bits set mean an item/action declares no requirement set. */
inline constexpr std::uint16_t kUnavailableSetIndex = 0xFFFFU;
/** All condition bits set mark a requirement row that applies without a native variant gate. */
inline constexpr std::uint16_t kUnconditionalRequirement = 0xFFFFU;

/** One exact native material row. Quantity zero is authored by free cosmetic requirements. */
struct Requirement {
    std::uint32_t quantity{};
    std::uint16_t itemDefinitionIndex{kUnavailableItemDefinitionIndex};
    std::uint16_t condition{kUnconditionalRequirement};
    bool deleteOnAction{};
    bool omitFromRequirements{};
};

/** One dense native requirement set, addressed by its installed ordinal. Count zero is free. */
struct Definition {
    std::uint32_t requirementSetHash{};
    std::uint16_t requirementSetIndex{kUnavailableSetIndex};
    std::uint8_t requirementCount{};
    std::array<Requirement, kRequirementCapacity> requirements{};
};

void clear() noexcept;
[[nodiscard]] bool valid(std::span<const Definition> definitions) noexcept;
[[nodiscard]] bool replace(std::span<const Definition> definitions) noexcept;
[[nodiscard]] bool find(std::uint16_t requirementSetIndex, Definition& definition) noexcept;
[[nodiscard]] bool snapshot(std::span<Definition> output, std::size_t& count) noexcept;
[[nodiscard]] std::size_t count() noexcept;

} // namespace sunrise::state::build_data::material_requirements
