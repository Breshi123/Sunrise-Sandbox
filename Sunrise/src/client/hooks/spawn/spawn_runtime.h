#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../spawn/spawn_keybind_store.h"

namespace sunrise::client::hooks::spawn {

enum class Origin : std::uint8_t {
    player,
    crosshair,
};

enum class SelectionList : std::uint8_t {
    main,
    projectile,
    loot,
    count,
};

struct Settings {
    float lift{1.0F};
    float rayDistance{100.0F};
    float scale{1.0F};
    std::array<float, 3> offset{};
    std::array<float, 4> rotation{0.0F, 0.0F, 0.0F, 1.0F};
    bool useCameraRotation{};
    bool overrideRotation{};
};

[[nodiscard]] bool install() noexcept;
void uninstall() noexcept;

[[nodiscard]] bool ready() noexcept;
[[nodiscard]] bool busy() noexcept;
[[nodiscard]] bool is_tag_resident(std::uint32_t tag) noexcept;
[[nodiscard]] bool object_type(std::uint32_t tag, std::uint8_t& type) noexcept;

[[nodiscard]] bool request(std::uint32_t tag,
                           Origin origin,
                           std::uint32_t amount,
                           const Settings& settings) noexcept;

[[nodiscard]] bool request_line(std::span<const std::uint32_t> tags,
                                Origin origin,
                                std::uint32_t itemsPerRow,
                                float spacing,
                                const Settings& settings) noexcept;

void configure_shortcut(client::spawn::Action action,
                        std::uint32_t tag,
                        std::uint32_t amount,
                        const Settings& settings) noexcept;

void configure_candidates(SelectionList list,
                          std::span<const std::uint32_t> tags,
                          std::size_t selected) noexcept;
[[nodiscard]] std::size_t selected_candidate(SelectionList list) noexcept;
void select_candidate(SelectionList list, std::size_t selected) noexcept;

[[nodiscard]] std::size_t spawned_enemy_count() noexcept;
void request_clear_spawned_enemies() noexcept;

void cancel() noexcept;

} // namespace sunrise::client::hooks::spawn
