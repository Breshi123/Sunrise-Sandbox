#pragma once

namespace sunrise::client::graphics {

/**
 * Opens and releases the screen device context once, which is what makes Wine attach its display.
 * Wine defers that attach until something asks for the display, and the renderer expects it to
 * have happened already. On Windows the call is harmless.
 */
void initialize_wine_display() noexcept;

} // namespace sunrise::client::graphics
