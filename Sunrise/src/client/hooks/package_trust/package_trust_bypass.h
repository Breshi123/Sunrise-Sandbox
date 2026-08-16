#pragma once

namespace sunrise::client::hooks::package_trust {

/**
 * Accepts package RSA, extended-header hash and cached-data hash authentication. Native package
 * parsing, decompression, file-size, table and bounds validation remain active.
 * @return True when the validator detour is attached.
 */
[[nodiscard]] bool install() noexcept;

/** @return True when the validator detour is detached. */
[[nodiscard]] bool uninstall() noexcept;

/** @return True while the validator detour is attached. */
[[nodiscard]] bool is_installed() noexcept;

} // namespace sunrise::client::hooks::package_trust
