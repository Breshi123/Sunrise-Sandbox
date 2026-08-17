#include "wine_compat.h"

#include <Windows.h>

namespace sunrise::client::graphics {

/** Opens and releases the screen device context, which is what makes Wine attach its display. */
void initialize_wine_display() noexcept {
    const HDC screen = GetDC(nullptr);
    if (screen != nullptr) {
        (void)ReleaseDC(nullptr, screen);
    }
}

} // namespace sunrise::client::graphics
