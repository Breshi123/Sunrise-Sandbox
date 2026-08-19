#include "entity_name_catalog.h"

#include <algorithm>
#include <string_view>

#include "../table.h"

namespace sunrise::state::build_data::entity_names {
namespace {

Lock g_lock;
Table<Name, kNameCapacity> g_names;

[[nodiscard]] std::string_view text_of(const Name& name) noexcept {
    return {name.text.data(), name.length};
}

[[nodiscard]] bool canonical(const Name& name) noexcept {
    if (name.tag == 0 || name.tag == 0xFFFFFFFFU || name.length == 0
        || name.length >= name.text.size()) {
        return false;
    }
    for (std::size_t index = 0; index < name.text.size(); ++index) {
        const unsigned char value = static_cast<unsigned char>(name.text[index]);
        if (index < name.length ? value < 0x20 : value != 0) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool less(const Name& left, const Name& right) noexcept {
    return left.tag != right.tag ? left.tag < right.tag : text_of(left) < text_of(right);
}

} // namespace

void clear() noexcept {
    const Lock::Exclusive guard(g_lock);
    g_names.clear();
}

bool valid(std::span<const Name> names) noexcept {
    if (names.size() > kNameCapacity) {
        return false;
    }
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (!canonical(names[index]) || (index != 0 && !less(names[index - 1], names[index]))) {
            return false;
        }
    }
    return true;
}

bool replace(std::span<const Name> names) noexcept {
    if (!valid(names)) {
        return false;
    }
    const Lock::Exclusive guard(g_lock);
    return g_names.replace(names);
}

bool find(std::uint32_t tag, Name& name) noexcept {
    name = {};
    const Lock::Shared guard(g_lock);
    const std::span<const Name> rows = g_names.rows();
    const auto found = std::lower_bound(rows.begin(), rows.end(), tag, [](const Name& row,
                                                                         std::uint32_t wanted) {
        return row.tag < wanted;
    });
    if (found == rows.end() || found->tag != tag) {
        return false;
    }
    name = *found;
    return true;
}

bool snapshot(std::span<Name> output, std::size_t& count) noexcept {
    const Lock::Shared guard(g_lock);
    return g_names.snapshot(output, count);
}

std::size_t count() noexcept {
    const Lock::Shared guard(g_lock);
    return g_names.count();
}

} // namespace sunrise::state::build_data::entity_names
