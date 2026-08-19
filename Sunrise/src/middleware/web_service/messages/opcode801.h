#pragma once

#include <cstdint>

#include "../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode801 {

/** Web Service opcode used to select one subclass socket-entry node. */
inline constexpr std::uint16_t kOpcode = 801;

/** Exact logical fields carried by the native 80-bit subclass selection descriptor. */
struct Request {
    std::uint64_t subclassInstanceSoid{};
    std::uint8_t socketEntry{};
};

/**
 * Parses the exact reflected opcode-801 descriptor observed for class, grenade and path nodes.
 * @param message Parsed Web Service envelope.
 * @param request Receives the subclass instance and zero-based socket-entry index.
 * @return True only for the complete canonical 10-byte request.
 */
[[nodiscard]] bool parse_request(const Message& message, Request& request) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode801
