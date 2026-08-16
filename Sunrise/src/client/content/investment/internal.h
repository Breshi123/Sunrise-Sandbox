#pragma once

#include "../../../state/account/account_state.h"
#include "source.h"

namespace sunrise::client::content::investment {

/** @return True when the next refresh slice needs one presented overlay before its package sweep.
 */
[[nodiscard]] bool requires_package_sweep() noexcept;

} // namespace sunrise::client::content::investment
