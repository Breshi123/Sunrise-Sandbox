/** The player module's interface. Every control saves at once, so a change survives a restart. */

#include "player_panel.h"

#include <imgui.h>

#include "../../../core/ui/components/toggle/ui_toggle_component.h"
#include "../../player/player_settings_store.h"

namespace sunrise::client::ui::player {

/** Draws the player module inside the active Core UI frame. */
void draw() noexcept {
    client::player::Settings settings = client::player::get();

    ImGui::TextUnformatted("Infinite Ammo");
    ImGui::Separator();
    ImGui::TextWrapped("Keep every weapon's reserves full.");
    ImGui::Spacing();

    const bool changed = core::ui::components::toggle::control("Enabled##infinite_ammo",
                                                               settings.infiniteAmmoEnabled);
    if (changed) {
        (void)client::player::publish(settings);
    }
}

} // namespace sunrise::client::ui::player
