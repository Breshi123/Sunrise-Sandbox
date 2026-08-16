#include "../runtime.h"
#include "../runtime/persistence/publication_transaction.h"
#include "material_requirement_catalog.h"

namespace sunrise::state::build_data {
namespace {

[[nodiscard]] bool valid_material_publication(
    std::span<const material_requirements::Definition> definitions) noexcept {
    // The package extractor has already checked every native material index against the exact
    // dense item-table count used to publish this catalog. Re-probing the live item lookup here
    // made publication depend on a second catalog boundary even though no stronger invariant was
    // established. Keep this publication gate structural; the complete cache validator repeats
    // the cross-domain bounds check before anything is persisted or restored.
    return item_definitions_ready() && material_requirements::valid(definitions);
}

} // namespace

bool material_requirement_sets_ready() noexcept {
    return material_requirements::count() != 0;
}

bool publish_material_requirement_sets(
    std::span<const material_requirements::Definition> definitions) noexcept {
    runtime::persistence::Transaction transaction;
    return transaction.active() && valid_material_publication(definitions)
           && transaction.finish(material_requirements::replace(definitions),
                                 material_requirements::clear);
}

bool find_material_requirement_set(std::uint16_t requirementSetIndex,
                                   material_requirements::Definition& definition) noexcept {
    definition = {};
    return material_requirement_sets_ready()
           && material_requirements::find(requirementSetIndex, definition)
           && definition.requirementSetIndex == requirementSetIndex;
}

} // namespace sunrise::state::build_data
