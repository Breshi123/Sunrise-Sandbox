#include "host_environment.h"

#include <Windows.h>

namespace sunrise::core::runtime {

/** @return True when the loaded ntdll exports Wine's own version entry point. */
bool is_wine() noexcept {
    // Wine exports this from ntdll to name itself and Windows never does. The host cannot change
    // while the process lives, so the answer is resolved once.
    static const bool underWine = [] {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll != nullptr && GetProcAddress(ntdll, "wine_get_version") != nullptr;
    }();
    return underWine;
}

} // namespace sunrise::core::runtime
