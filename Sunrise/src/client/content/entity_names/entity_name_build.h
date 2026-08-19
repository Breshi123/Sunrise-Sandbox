#pragma once

#include "../../../middleware/content/packages/reader/reader.h"

namespace sunrise::client::content::entity_names {

[[nodiscard]] bool build(const middleware::content::packages::reader::Source& source,
                         middleware::content::packages::reader::Scratch& scratch) noexcept;

} // namespace sunrise::client::content::entity_names
