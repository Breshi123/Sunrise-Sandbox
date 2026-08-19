#include <cstddef>

#include "../../encoding/bit_reader.h"
#include "opcode801.h"

namespace sunrise::middleware::web_service::messages::opcode801 {
namespace {

/** The reflected opcode-801 request occupies exactly 80 bits. */
constexpr std::size_t kPayloadSize = 10;
/** The subclass instance is a bare 64-bit SOID. */
constexpr std::uint8_t kInstanceWidth = 64;
/** A signed socket-entry index is biased from INT8_MIN into one 8-bit wire field. */
constexpr std::uint8_t kSocketEntryWidth = 8;
/** Two absent optional fields terminate the generic Web Service request descriptor. */
constexpr std::uint8_t kOuterTrailerWidth = 2;
/** The complete outer request is padded to its final byte. */
constexpr std::uint8_t kFinalPaddingWidth = 6;
/** Nonnegative signed 8-bit entries have this bit set after native descriptor biasing. */
constexpr std::uint64_t kSocketEntryBias = 0x80ULL;

} // namespace

/** Parses the complete native subclass socket-entry selection descriptor. */
bool parse_request(const Message& message, Request& request) noexcept {
    request = {};
    if (message.opcode != kOpcode || message.payload.size() != kPayloadSize) {
        return false;
    }

    encoding::bits::Reader reader(message.payload);
    std::uint64_t encodedSocketEntry = 0;
    std::uint64_t outerTrailer = 0;
    std::uint64_t finalPadding = 0;
    if (!reader.read(kInstanceWidth, request.subclassInstanceSoid)
        || !reader.read(kSocketEntryWidth, encodedSocketEntry)
        || !reader.read(kOuterTrailerWidth, outerTrailer)
        || !reader.read(kFinalPaddingWidth, finalPadding) || reader.remaining_bits() != 0
        || encodedSocketEntry < kSocketEntryBias || outerTrailer != 0 || finalPadding != 0) {
        request = {};
        return false;
    }
    request.socketEntry = static_cast<std::uint8_t>(encodedSocketEntry - kSocketEntryBias);
    return request.subclassInstanceSoid != 0;
}

} // namespace sunrise::middleware::web_service::messages::opcode801
