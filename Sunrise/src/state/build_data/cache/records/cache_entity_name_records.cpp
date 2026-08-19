#include "codec.h"

#include <algorithm>

namespace sunrise::state::build_data::cache::records {

bool encode(const entity_names::Name& value, EntityNameRecord& record) noexcept {
    record = {};
    if (value.length == 0 || value.length >= value.text.size()) {
        return false;
    }
    record.text = value.text;
    record.tag = value.tag;
    record.length = value.length;
    return true;
}

bool decode(const EntityNameRecord& record, entity_names::Name& value) noexcept {
    value = {};
    if (record.length == 0 || record.length >= record.text.size()
        || std::any_of(record.reserved.begin(), record.reserved.end(), [](std::uint8_t byte) {
               return byte != 0;
           })) {
        return false;
    }
    value.tag = record.tag;
    value.text = record.text;
    value.length = record.length;
    return true;
}

} // namespace sunrise::state::build_data::cache::records
