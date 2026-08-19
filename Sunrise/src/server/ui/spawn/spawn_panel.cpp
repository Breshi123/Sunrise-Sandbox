#include "spawn_panel.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <imgui.h>
#include <span>
#include <string_view>
#include <vector>

#include "../../../client/content/items/packages/internal.h"
#include "../../../client/hooks/spawn/spawn_runtime.h"
#include "../../../core/filesystem/path.h"
#include "../../../core/ui/components/picker/ui_picker_component.h"
#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../state/build_data/runtime.h"

namespace sunrise::server::ui::spawn {
namespace {

namespace native = client::hooks::spawn;
namespace spawn_keys = client::spawn;
namespace package_reader = middleware::content::packages::reader;
namespace picker = core::ui::components::picker;

constexpr std::uint32_t kEntityClass = 0x80809C0FU;

enum class ObjectType : std::uint8_t {
    Inherited = 0,
    StaticMesh = 1,                 // Interactable
    PropSimpleDeprecated = 2,
    PropExpensiveDeprecated = 3,
    PropCosmeticStatic = 4,         // World effects / decorations
    PropCosmeticMovable = 5,        // Moving props
    PropCosmeticMovableGarbage = 6,
    PropNetworkedStatic = 7,        // Ad spawns
    PropNetworkedMovable = 8,       // Explodable
    PropCinematic = 9,
    Speedtree = 10,
    Interactive = 11,
    Biped = 12,                     // Guardians, Enemies, NPCs
    Creature = 13,
    Weapon = 14,                    // Weapon props
    Vehicle = 15,                   // Sparrows, Pikes, Ships
    Turret = 16,                    // VehicleEntity
    Emitter = 17,                   // Effects, some interactive projectiles
    Projectile = 18,
    Item = 19,
    ItemAmmo = 20,
    ItemLoot = 21,
    Gear = 22,
    HopOn = 23,
    HopOnGearBiped = 24,
    HopOnGearWeapon = 25,
    HopOnGearShip = 26,
    HopOnGearSparrow = 27,
    System = 28,
    Invalid = 0xFF,
};

constexpr std::array<const char*, 29> kObjectTypeNames{
    "Inherited",
    "StaticMesh",
    "PropSimpleDeprecated",
    "PropExpensiveDeprecated",
    "PropCosmeticStatic",
    "PropCosmeticMovable",
    "PropCosmeticMovableGarbage",
    "PropNetworkedStatic",
    "PropNetworkedMovable",
    "PropCinematic",
    "Speedtree",
    "Interactive",
    "Biped",
    "Creature",
    "Weapon",
    "Vehicle",
    "Turret",
    "Emitter",
    "Projectile",
    "Item",
    "ItemAmmo",
    "ItemLoot",
    "Gear",
    "HopOn",
    "HopOnGearBiped",
    "HopOnGearWeapon",
    "HopOnGearShip",
    "HopOnGearSparrow",
    "System",
};

static_assert(kObjectTypeNames.size() == static_cast<std::uint8_t>(ObjectType::System) + 1);

enum class SpawnAllMode : std::uint8_t {
    none,
    all,
    selectedType,
};

struct Candidate {
    std::uint32_t tag{};
    ObjectType type{};
    bool named{};
    std::array<char, 224> label{};
};

struct Column {
    std::vector<Candidate> candidates{};
    std::vector<picker::Item> items{};
    std::size_t selected{};
    native::Settings settings{};
    int amount{1};
    int perRow{10};
    float spacing{1.0F};
};

Column g_main{};
Column g_projectile{};
Column g_loot{};
std::vector<Candidate> g_allMainCandidates{};
std::vector<state::build_data::entity_names::Name> g_names{};
bool g_scanned{};
std::size_t g_capturingKey{spawn_keys::kActionCount};

void key_name(std::uint32_t virtualKey, std::array<char, 64>& output) noexcept {
    if (virtualKey == spawn_keys::kNoKey) {
        (void)std::snprintf(output.data(), output.size(), "None");
        return;
    }
    const UINT scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
    std::array<wchar_t, 64> wide{};
    const int written = scanCode != 0 ? GetKeyNameTextW(static_cast<LONG>(scanCode << 16),
                                                        wide.data(),
                                                        static_cast<int>(wide.size()))
                                      : 0;
    if (written <= 0
        || WideCharToMultiByte(CP_UTF8,
                               0,
                               wide.data(),
                               written,
                               output.data(),
                               static_cast<int>(output.size() - 1),
                               nullptr,
                               nullptr)
               <= 0) {
        (void)std::snprintf(
            output.data(), output.size(), "Key 0x%02X", static_cast<unsigned>(virtualKey));
    }
}

[[nodiscard]] bool capture_key(std::uint32_t& output) noexcept {
    if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
        output = spawn_keys::kNoKey;
        return true;
    }
    for (int key = 7; key <= 254; ++key) {
        if ((GetAsyncKeyState(key) & 0x8000) != 0) {
            output = static_cast<std::uint32_t>(key);
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool key_picker(spawn_keys::Action action,
                              std::uint32_t& virtualKey,
                              float width) noexcept {
    const std::size_t index = static_cast<std::size_t>(action);
    ImGui::PushID(static_cast<int>(index));
    if (g_capturingKey == index) {
        if (ImGui::Button("...", ImVec2(width, 0.0F))) {
            g_capturingKey = spawn_keys::kActionCount;
        }
        ImGui::PopID();
        std::uint32_t picked = spawn_keys::kNoKey;
        if (capture_key(picked)) {
            virtualKey = picked;
            g_capturingKey = spawn_keys::kActionCount;
            return true;
        }
        return false;
    }
    std::array<char, 64> name{};
    key_name(virtualKey, name);
    const bool clicked = ImGui::Button(name.data(), ImVec2(width, 0.0F));
    ImGui::PopID();
    if (clicked) {
        g_capturingKey = index;
    }
    return false;
}

[[nodiscard]] std::span<const state::build_data::entity_names::Name>
names_of(std::uint32_t tag) noexcept {
    const auto first = std::lower_bound(
        g_names.begin(), g_names.end(), tag, [](const auto& name, std::uint32_t wanted) {
            return name.tag < wanted;
        });
    const auto last = std::upper_bound(
        first, g_names.end(), tag, [](std::uint32_t wanted, const auto& name) {
            return wanted < name.tag;
        });
    return {first, last};
}

[[nodiscard]] bool projectile_name(std::string_view name) noexcept {
    constexpr std::array<std::string_view, 12> markers{
        "projectile", "missile", "rocket", "grenade", "fireball", "mortar",
        "cannonball", "seeker", "tracer", "bullet", "plasma_bolt", "weapon_bolt",
    };
    return std::any_of(markers.begin(), markers.end(), [name](std::string_view marker) {
        return name.find(marker) != std::string_view::npos;
    });
}

void family_text(std::wstring_view family, std::array<char, 96>& output) noexcept {
    output = {};
    const std::size_t count = (std::min)(family.size(), output.size() - 1);
    for (std::size_t index = 0; index < count; ++index) {
        const wchar_t value = family[index];
        output[index] = value >= 32 && value <= 126 ? static_cast<char>(value) : '?';
    }
}

[[nodiscard]] constexpr const char* object_type_name(ObjectType type) noexcept {
    if (type == ObjectType::Invalid) {
        return "Invalid";
    }
    const std::size_t index = static_cast<std::uint8_t>(type);
    return index < kObjectTypeNames.size() ? kObjectTypeNames[index] : nullptr;
}

constexpr std::uint64_t kUnknownObjectTypeBit = 1ULL << 62;
constexpr std::uint64_t kInvalidObjectTypeBit = 1ULL << 63;

[[nodiscard]] constexpr std::uint64_t object_type_filter_bit(ObjectType type) noexcept {
    if (object_type_name(type) == nullptr) {
        return kUnknownObjectTypeBit;
    }
    if (type == ObjectType::Invalid) {
        return kInvalidObjectTypeBit;
    }
    return 1ULL << static_cast<std::uint8_t>(type);
}

void add_candidate(Column& column,
                   std::uint32_t tag,
                   std::uint8_t type,
                   std::wstring_view family,
                   const state::build_data::entity_names::Name* resolvedName) {
    std::array<char, 96> package{};
    family_text(family, package);
    Candidate value{};
    value.tag = tag;
    value.type = static_cast<ObjectType>(type);
    const char* const name = resolvedName != nullptr ? resolvedName->text.data() : nullptr;
    const char* typeName = object_type_name(value.type);
    std::array<char, 32> unknownType{};
    if (typeName == nullptr) {
        (void)std::snprintf(unknownType.data(), unknownType.size(), "Unknown(%u)", type);
        typeName = unknownType.data();
    }
    value.named = name != nullptr;
    if (name != nullptr) {
        (void)std::snprintf(value.label.data(),
                            value.label.size(),
                            "%s | %s | 0x%08X | %s",
                            name,
                            typeName,
                            tag,
                            package.data());
    } else {
        (void)std::snprintf(value.label.data(),
                            value.label.size(),
                            "0x%08X | %s | %s",
                            tag,
                            typeName,
                            package.data());
    }
    column.candidates.push_back(value);
}

bool collect_entity(void*, const package_reader::ClassEntry& entry) noexcept {
    if (!native::is_tag_resident(entry.tag)) {
        return true;
    }
    std::uint8_t type = 0;
    if (!native::object_type(entry.tag, type)) {
        return true;
    }
    const auto names = names_of(entry.tag);
    const auto objectType = static_cast<ObjectType>(type);
    const bool namedProjectile = std::any_of(names.begin(), names.end(), [](const auto& name) {
        return projectile_name({name.text.data(), name.length});
    });
    Column* const column = objectType == ObjectType::Projectile || namedProjectile
                               ? &g_projectile
                           : objectType == ObjectType::ItemAmmo || objectType == ObjectType::ItemLoot
                               ? &g_loot
                               : &g_main;
    if (names.empty()) {
        add_candidate(*column, entry.tag, type, entry.packageFamily, nullptr);
    } else {
        for (const auto& name : names) {
            add_candidate(*column, entry.tag, type, entry.packageFamily, &name);
        }
    }
    return true;
}

void finish_column(Column& column) {
    std::sort(column.candidates.begin(),
              column.candidates.end(),
              [](const Candidate& first, const Candidate& second) {
                  if (first.named != second.named) {
                      return first.named;
                  }
                  return std::string_view(first.label.data())
                         < std::string_view(second.label.data());
              });
    column.candidates.erase(
        std::unique(column.candidates.begin(),
                    column.candidates.end(),
                    [](const Candidate& first, const Candidate& second) {
                        return first.tag == second.tag
                               && std::string_view(first.label.data())
                                      == std::string_view(second.label.data());
                    }),
        column.candidates.end());
    column.items.clear();
    column.items.reserve(column.candidates.size());
    for (const Candidate& candidate : column.candidates) {
        column.items.push_back({candidate.label.data()});
    }
    column.selected = 0;
}

void apply_main_type_filter(std::uint64_t hiddenTypes) {
    g_main.candidates.clear();
    g_main.candidates.reserve(g_allMainCandidates.size());
    for (const Candidate& candidate : g_allMainCandidates) {
        if ((hiddenTypes & object_type_filter_bit(candidate.type)) == 0) {
            g_main.candidates.push_back(candidate);
        }
    }
    finish_column(g_main);
}

void refresh() noexcept {
    g_main.candidates.clear();
    g_projectile.candidates.clear();
    g_loot.candidates.clear();
    g_allMainCandidates.clear();
    g_names.resize(state::build_data::entity_name_count());
    std::size_t nameCount = 0;
    if (!state::build_data::snapshot_entity_names(g_names, nameCount)) {
        g_names.clear();
    } else {
        g_names.resize(nameCount);
    }
    core::path::Buffer directory{};
    const bool hasDirectory = client::content::items::packages::package_directory(directory);
    if (native::ready() && hasDirectory) {
        package_reader::ScanResult result{};
        (void)package_reader::scan_class_entries(
            directory.chars.data(), kEntityClass, &collect_entity, nullptr, result);
        package_reader::release_caches();
    }
    finish_column(g_main);
    g_allMainCandidates = g_main.candidates;
    apply_main_type_filter(spawn_keys::get().hiddenMainTypes);
    finish_column(g_projectile);
    finish_column(g_loot);
    g_scanned = true;
}

[[nodiscard]] const char* preview(const Column& column) noexcept {
    return column.selected < column.candidates.size()
               ? column.candidates[column.selected].label.data()
               : "[None]";
}

void draw_main_type_filter(spawn_keys::Keybinds& settings, bool& changed) noexcept {
    if (!ImGui::TreeNodeEx("Sort", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        return;
    }

    bool filterChanged = false;
    ImGui::TextDisabled("Checked types are visible");
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(ImGui::GetStyle().ItemSpacing.x, 2.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(ImGui::GetStyle().FramePadding.x, 1.0F));
    if (ImGui::BeginTable("type_visibility", 4, ImGuiTableFlags_SizingStretchSame)) {
        const auto drawType = [&](ObjectType type) {
            ImGui::TableNextColumn();
            const std::uint64_t bit = object_type_filter_bit(type);
            bool visible = (settings.hiddenMainTypes & bit) == 0;
            if (ImGui::Checkbox(object_type_name(type), &visible)) {
                settings.hiddenMainTypes = visible ? settings.hiddenMainTypes & ~bit
                                                   : settings.hiddenMainTypes | bit;
                filterChanged = true;
            }
        };
        for (std::size_t index = 0; index < kObjectTypeNames.size(); ++index) {
            drawType(static_cast<ObjectType>(index));
        }
        drawType(ObjectType::Invalid);
        ImGui::TableNextColumn();
        bool unknownVisible = (settings.hiddenMainTypes & kUnknownObjectTypeBit) == 0;
        if (ImGui::Checkbox("Unknown", &unknownVisible)) {
            settings.hiddenMainTypes = unknownVisible
                                           ? settings.hiddenMainTypes & ~kUnknownObjectTypeBit
                                           : settings.hiddenMainTypes | kUnknownObjectTypeBit;
            filterChanged = true;
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar(2);
    ImGui::TreePop();

    if (filterChanged) {
        apply_main_type_filter(settings.hiddenMainTypes);
        changed = true;
    }
}

void draw_settings(Column& column, const char* id, SpawnAllMode spawnAllMode) noexcept {
    ImGui::PushID(id);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float controlWidth = (ImGui::GetContentRegionAvail().x - spacing) * 0.5F;

    ImGui::BeginGroup();
    ImGui::TextUnformatted("Amount:");
    ImGui::SetNextItemWidth(controlWidth);
    ImGui::DragInt("##amount", &column.amount, 1.0F, 1, 4096, "%d");
    ImGui::EndGroup();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextUnformatted("Vertical lift:");
    ImGui::SetNextItemWidth(controlWidth);
    ImGui::DragFloat(
        "##vertical_lift", &column.settings.lift, 0.1F, -100.0F, 100.0F, "%.1f");
    ImGui::EndGroup();

    ImGui::BeginGroup();
    ImGui::TextUnformatted("Scale:");
    ImGui::SetNextItemWidth(controlWidth);
    ImGui::DragFloat("##scale", &column.settings.scale, 0.01F, 0.01F, 100.0F, "%.2f");
    ImGui::EndGroup();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextUnformatted("Ray distance:");
    ImGui::SetNextItemWidth(controlWidth);
    ImGui::DragFloat("##ray_distance",
                     &column.settings.rayDistance,
                     1.0F,
                     1.0F,
                     2000.0F,
                     "%.0f");
    ImGui::EndGroup();

    if (ImGui::TreeNodeEx("Transform", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        ImGui::Checkbox("Camera rotation", &column.settings.useCameraRotation);
        ImGui::Checkbox("Override rotation", &column.settings.overrideRotation);
        ImGui::TextUnformatted("Position offset:");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputFloat3("##position_offset", column.settings.offset.data(), "%.2f");
        if (column.settings.overrideRotation) {
            ImGui::TextUnformatted("Rotation quaternion:");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputFloat4("##rotation_quaternion", column.settings.rotation.data(), "%.3f");
        }
        ImGui::TreePop();
    }

    const bool filterByType = spawnAllMode == SpawnAllMode::selectedType;
    const char* const spawnAllLabel = filterByType ? "Spawn All of Type [unstable]"
                                                   : "Spawn All [unstable]";
    if (spawnAllMode != SpawnAllMode::none
        && ImGui::TreeNodeEx(spawnAllLabel, ImGuiTreeNodeFlags_SpanAvailWidth)) {
        ImGui::TextUnformatted("Items per row:");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragInt("##items_per_row", &column.perRow, 1.0F, 1, 4096, "%d");
        ImGui::TextUnformatted("Spacing:");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragFloat("##spacing", &column.spacing, 0.1F, 0.1F, 100.0F, "%.1f");
        const bool hasSelection = column.selected < column.candidates.size();
        ImGui::BeginDisabled(native::busy() || (filterByType && !hasSelection));
        if (ImGui::Button(filterByType ? "Spawn selected type at crosshair"
                                       : "Spawn all at crosshair",
                          ImVec2(-FLT_MIN, 0.0F))) {
            std::vector<std::uint32_t> tags{};
            tags.reserve(column.candidates.size());
            const ObjectType selectedType = hasSelection ? column.candidates[column.selected].type
                                                         : ObjectType::Invalid;
            for (const Candidate& candidate : column.candidates) {
                if (!filterByType || candidate.type == selectedType) {
                    tags.push_back(candidate.tag);
                }
            }
            std::sort(tags.begin(), tags.end());
            tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
            (void)native::request_line(tags,
                                       native::Origin::crosshair,
                                       static_cast<std::uint32_t>((std::max)(column.perRow, 1)),
                                       column.spacing,
                                       column.settings);
        }
        ImGui::EndDisabled();
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void draw_keybinds(spawn_keys::Action playerAction,
                   spawn_keys::Action crosshairAction,
                   spawn_keys::Keybinds& keybinds,
                   bool& changed) noexcept {
    if (!ImGui::TreeNodeEx("Keybinds", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        return;
    }
    const float labelWidth = ImGui::CalcTextSize("At crosshair").x
                             + ImGui::GetStyle().ItemSpacing.x * 2.0F;
    const float controlWidth = ImGui::GetContentRegionAvail().x - labelWidth;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("At player");
    ImGui::SameLine(labelWidth);
    changed = key_picker(playerAction,
                         keybinds.virtualKeys[static_cast<std::size_t>(playerAction)],
                         controlWidth)
              || changed;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("At crosshair");
    ImGui::SameLine(labelWidth);
    changed = key_picker(crosshairAction,
                         keybinds.virtualKeys[static_cast<std::size_t>(crosshairAction)],
                         controlWidth)
              || changed;
    ImGui::TreePop();
}

void draw_column(const char* title,
                 const char* id,
                 Column& column,
                 spawn_keys::Action playerAction,
                 spawn_keys::Action crosshairAction,
                 bool showTypeFilter,
                 SpawnAllMode spawnAllMode,
                 spawn_keys::Keybinds& keybinds,
                 bool& keybindsChanged) noexcept {
    ImGui::PushID(id);
    const std::uint32_t selectedTag = column.selected < column.candidates.size()
                                          ? column.candidates[column.selected].tag
                                          : 0xFFFFFFFFU;
    const std::uint32_t amount = static_cast<std::uint32_t>((std::max)(column.amount, 1));
    native::configure_shortcut(playerAction, selectedTag, amount, column.settings);
    native::configure_shortcut(crosshairAction, selectedTag, amount, column.settings);

    if (ImGui::TreeNodeEx(title, ImGuiTreeNodeFlags_SpanAvailWidth)) {
        if (showTypeFilter) {
            draw_main_type_filter(keybinds, keybindsChanged);
        }
        const std::span<const picker::Item> rows(column.items.data(), column.items.size());
        (void)picker::control("picker", preview(column), rows, column.selected);

        ImGui::BeginDisabled(column.selected >= column.candidates.size() || native::busy());
        if (ImGui::Button("At player", ImVec2(ImGui::GetContentRegionAvail().x * 0.49F, 0.0F))) {
            (void)native::request(column.candidates[column.selected].tag,
                                  native::Origin::player,
                                  static_cast<std::uint32_t>((std::max)(column.amount, 1)),
                                  column.settings);
        }
        ImGui::SameLine();
        if (ImGui::Button("At crosshair", ImVec2(-FLT_MIN, 0.0F))) {
            (void)native::request(column.candidates[column.selected].tag,
                                  native::Origin::crosshair,
                                  static_cast<std::uint32_t>((std::max)(column.amount, 1)),
                                  column.settings);
        }
        ImGui::EndDisabled();
        draw_keybinds(playerAction, crosshairAction, keybinds, keybindsChanged);
        draw_settings(column, "settings", spawnAllMode);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

} // namespace

void draw() noexcept {
    if (!g_scanned) {
        refresh();
    }

    if (ImGui::Button("Refresh loaded entities")) {
        refresh();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu main  |  %zu projectiles  |  %zu loot",
                        g_main.candidates.size(),
                        g_projectile.candidates.size(),
                        g_loot.candidates.size());
    if (native::busy()) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            native::cancel();
        }
    }

    spawn_keys::Keybinds keybinds = spawn_keys::get();
    bool keybindsChanged = false;
    draw_column("Main spawner",
                "main",
                g_main,
                spawn_keys::Action::mainPlayer,
                spawn_keys::Action::mainCrosshair,
                true,
                SpawnAllMode::selectedType,
                keybinds,
                keybindsChanged);
    draw_column("Projectile spawner",
                "projectile",
                g_projectile,
                spawn_keys::Action::projectilePlayer,
                spawn_keys::Action::projectileCrosshair,
                false,
                SpawnAllMode::all,
                keybinds,
                keybindsChanged);
    draw_column("Loot spawner",
                "loot",
                g_loot,
                spawn_keys::Action::lootPlayer,
                spawn_keys::Action::lootCrosshair,
                false,
                SpawnAllMode::all,
                keybinds,
                keybindsChanged);
    if (keybindsChanged) {
        (void)spawn_keys::publish(keybinds);
    }
}

} // namespace sunrise::server::ui::spawn
