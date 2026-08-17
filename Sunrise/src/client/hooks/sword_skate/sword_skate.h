#pragma once

namespace sunrise::client::hooks::sword_skate {

/**
 * Clears the glide refusal for one physics tick of the local player.
 *
 * Runs from the physics sync, because the flag it clears is written when the sword's air attack
 * starts and read by the glide in the same tick the jump is pressed. A frame-timed poll would
 * race with both.
 *
 * @param component Physics component being synced, tested for player ownership here.
 */
void apply(void* component) noexcept;

} // namespace sunrise::client::hooks::sword_skate
