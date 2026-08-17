#pragma once

#include <array>
#include <cstddef>
#include <span>

namespace sunrise::middleware::crypto::ecc {

/** secp224r1 coordinates and shared secrets are 28 bytes. */
inline constexpr std::size_t kFieldSize = 28;
/** An exported key travels in a fixed field, zero padded past the encoding it carries. */
inline constexpr std::size_t kExportedKeySize = 100;

/** One completed agreement. */
struct Agreement {
    /** Our public key, encoded the way the peer's importer expects. */
    std::array<std::byte, kExportedKeySize> publicKey{};
    /** The x coordinate of the agreed point, high byte first. */
    std::array<std::byte, kFieldSize> sharedSecret{};
};

/**
 * Generates one key pair and agrees a secret with the peer's exported public key.
 * The pair is ephemeral and is destroyed before this returns; only the agreement survives.
 * @param peerPublicKey Peer's exported key, padding included.
 * @param output Receives the public key and the secret only on success.
 * @return True when the peer key parsed, sits on the curve, and the agreement completed.
 */
[[nodiscard]] bool agree(std::span<const std::byte> peerPublicKey, Agreement& output) noexcept;

} // namespace sunrise::middleware::crypto::ecc
