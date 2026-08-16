#include "codec.h"

namespace sunrise::state::build_data::cache::records {

/** Encodes one exact item/lane rule only when its canonical padding is zero. */
bool encode(const items::socket_plugs::Rule& value, SocketPlugRuleRecord& record) noexcept {
    record = {};
    if (value.reserved != 0) {
        return false;
    }
    record.itemDefinitionIndex = value.itemDefinitionIndex;
    record.lane = value.lane;
    record.poolIndex = value.poolIndex;
    return true;
}

/** Decodes one exact item/lane rule after checking its reserved byte. */
bool decode(const SocketPlugRuleRecord& record, items::socket_plugs::Rule& value) noexcept {
    value = {};
    if (record.reserved != 0) {
        return false;
    }
    value.itemDefinitionIndex = record.itemDefinitionIndex;
    value.lane = record.lane;
    value.poolIndex = record.poolIndex;
    return true;
}

/** Encodes one pool range. Cross-row contiguity is checked at the domain boundary. */
bool encode(const items::socket_plugs::Pool& value, SocketPlugPoolRecord& record) noexcept {
    record = {value.memberOffset, value.memberCount};
    return true;
}

/** Decodes one pool range. Cross-row contiguity is checked at the domain boundary. */
bool decode(const SocketPlugPoolRecord& record, items::socket_plugs::Pool& value) noexcept {
    value = {record.memberOffset, record.memberCount};
    return true;
}

/** Encodes one native plug-definition index. */
bool encode(items::socket_plugs::Member value, SocketPlugMemberRecord& record) noexcept {
    record = {value};
    return true;
}

/** Decodes one native plug-definition index. */
bool decode(const SocketPlugMemberRecord& record, items::socket_plugs::Member& value) noexcept {
    value = record.itemDefinitionIndex;
    return true;
}

} // namespace sunrise::state::build_data::cache::records
