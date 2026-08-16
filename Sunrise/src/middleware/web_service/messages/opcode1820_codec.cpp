#include <cstddef>

#include "../../encoding/bit_reader.h"
#include "opcode1820.h"

namespace sunrise::middleware::web_service::messages::opcode1820 {
namespace {

/** The reflected Collections pull request occupies exactly 24 bits. */
constexpr std::size_t kPayloadSize = 3;
/** The optional collectible carries one presence bit before its row. */
constexpr std::uint8_t kPresenceWidth = 1;
/** Native collectible rows are addressed by a fifteen-bit index. */
constexpr std::uint8_t kCollectibleIndexWidth = 15;
/** The descriptor pads its two payload bytes out to three. */
constexpr std::uint8_t kPaddingWidth = 8;

} // namespace

/** Parses the exact native Collections pull descriptor. */
bool parse_request(const Message& message, Request& request) noexcept {
    request = {};
    if (message.opcode != kOpcode || message.payload.size() != kPayloadSize) {
        return false;
    }
    encoding::bits::Reader reader(message.payload);
    std::uint64_t present = 0;
    std::uint64_t encodedCollectibleIndex = 0;
    std::uint64_t padding = 0;
    if (!reader.read(kPresenceWidth, present)
        || !reader.read(kCollectibleIndexWidth, encodedCollectibleIndex)
        || !reader.read(kPaddingWidth, padding) || reader.remaining_bits() != 0 || present == 0
        || padding != 0) {
        return false;
    }
    request.collectibleIndex = static_cast<std::uint16_t>(encodedCollectibleIndex);
    return true;
}

} // namespace sunrise::middleware::web_service::messages::opcode1820
