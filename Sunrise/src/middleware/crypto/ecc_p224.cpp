#include "ecc_p224.h"

#include <Windows.h>

#include <algorithm>
#include <vector>

#include "ecc_p224_curve.h"
#include "random_bytes.h"

namespace sunrise::middleware::crypto::ecc {

namespace {

/** DER tag for a SEQUENCE. */
constexpr std::byte kTagSequence{0x30};
/** DER tag for an INTEGER. */
constexpr std::byte kTagInteger{0x02};
/** DER tag for a BIT STRING. */
constexpr std::byte kTagBitString{0x03};
/** The key encoding opens with a one-bit flag that is set only on a private key. */
constexpr std::array<std::byte, 4> kPublicFlagBits{
    kTagBitString, std::byte{0x02}, std::byte{0x07}, std::byte{0x00}};
/** A DER length below this fits in the single byte that follows the tag. */
constexpr std::size_t kShortFormLimit = 0x80;
/**
 * Draws allowed while sampling a private key. A draw outside 1 to n-1 is rarer than one in 2^112,
 * so reaching the limit means the system randomness is broken, not that the sampling was unlucky.
 */
constexpr unsigned kScalarAttempts = 4;

/**
 * Appends one positive integer in the minimal form DER requires.
 * A leading zero goes in when the top bit is set, so the value never reads as negative.
 * @param output Growing encoding.
 * @param value Field element, high byte first.
 */
void append_integer(std::vector<std::byte>& output, std::span<const std::byte> value) noexcept {
    std::size_t first = 0;
    while (first + 1 < value.size() && value[first] == std::byte{0}) {
        ++first;
    }
    const std::span<const std::byte> trimmed = value.subspan(first);
    const bool pad = (std::to_integer<unsigned>(trimmed[0]) & 0x80U) != 0;
    output.push_back(kTagInteger);
    output.push_back(static_cast<std::byte>(trimmed.size() + (pad ? 1U : 0U)));
    if (pad) {
        output.push_back(std::byte{0});
    }
    output.insert(output.end(), trimmed.begin(), trimmed.end());
}

/**
 * Encodes one public key the way the peer's importer reads it.
 * @param x Affine x, high byte first.
 * @param y Affine y, high byte first.
 * @param output Receives the encoding, zero padded to the fixed field.
 * @return True when the encoding fits the field.
 */
[[nodiscard]] bool encode_public_key(std::span<const std::byte> x,
                                     std::span<const std::byte> y,
                                     std::array<std::byte, kExportedKeySize>& output) noexcept {
    std::vector<std::byte> body;
    body.insert(body.end(), kPublicFlagBits.begin(), kPublicFlagBits.end());
    // The curve is identified by its field size, encoded as a plain integer.
    body.push_back(kTagInteger);
    body.push_back(std::byte{1});
    body.push_back(static_cast<std::byte>(kFieldSize));
    append_integer(body, x);
    append_integer(body, y);
    if (body.size() >= kShortFormLimit || body.size() + 2 > kExportedKeySize) {
        return false;
    }
    output = {};
    output[0] = kTagSequence;
    output[1] = static_cast<std::byte>(body.size());
    std::copy(body.begin(), body.end(), output.begin() + 2);
    return true;
}

/**
 * Reads one DER element header.
 * @param input Encoding.
 * @param cursor Position of the tag; advanced past the header on success.
 * @param tag Tag the element must carry.
 * @param length Receives the content length.
 * @return True when the element is present and its content fits the input.
 */
[[nodiscard]] bool read_header(std::span<const std::byte> input,
                               std::size_t& cursor,
                               std::byte tag,
                               std::size_t& length) noexcept {
    if (cursor + 2 > input.size() || input[cursor] != tag) {
        return false;
    }
    const auto declared = std::to_integer<std::size_t>(input[cursor + 1]);
    // Only the short form is accepted. Every element here is far below the long-form threshold.
    if (declared >= kShortFormLimit || cursor + 2 + declared > input.size()) {
        return false;
    }
    cursor += 2;
    length = declared;
    return true;
}

/**
 * Reads one integer into a fixed field, right aligned.
 * @param input Encoding.
 * @param cursor Position of the tag; advanced past the element on success.
 * @param output Receives the value, high byte first.
 * @return True when the element is an integer no wider than the field.
 */
[[nodiscard]] bool read_field_integer(std::span<const std::byte> input,
                                      std::size_t& cursor,
                                      std::array<std::byte, kFieldSize>& output) noexcept {
    std::size_t length = 0;
    if (!read_header(input, cursor, kTagInteger, length) || length == 0) {
        return false;
    }
    std::span<const std::byte> value = input.subspan(cursor, length);
    if (!value.empty() && value[0] == std::byte{0}) {
        value = value.subspan(1);
    }
    if (value.size() > kFieldSize) {
        return false;
    }
    output = {};
    std::copy(value.begin(), value.end(), output.end() - static_cast<std::ptrdiff_t>(value.size()));
    cursor += length;
    return true;
}

/**
 * Reads the peer's exported public key.
 * @param input Peer key, padding included.
 * @param x Receives affine x.
 * @param y Receives affine y.
 * @return True when the encoding is a public key on this curve.
 */
[[nodiscard]] bool decode_public_key(std::span<const std::byte> input,
                                     std::array<std::byte, kFieldSize>& x,
                                     std::array<std::byte, kFieldSize>& y) noexcept {
    std::size_t cursor = 0;
    std::size_t length = 0;
    if (!read_header(input, cursor, kTagSequence, length)) {
        return false;
    }
    if (!read_header(input, cursor, kTagBitString, length)) {
        return false;
    }
    cursor += length;
    std::size_t sizeLength = 0;
    if (!read_header(input, cursor, kTagInteger, sizeLength) || sizeLength != 1
        || input[cursor] != static_cast<std::byte>(kFieldSize)) {
        return false;
    }
    cursor += sizeLength;
    return read_field_integer(input, cursor, x) && read_field_integer(input, cursor, y);
}

/**
 * Draws one private key from system randomness.
 * @param output Receives a scalar in 1 to n-1, or stays zero.
 * @return True when a usable scalar was drawn.
 */
[[nodiscard]] bool generate_scalar(curve::Field& output) noexcept {
    std::array<std::byte, kFieldSize> drawn{};
    for (unsigned attempt = 0; attempt < kScalarAttempts; ++attempt) {
        if (!random::fill(drawn)) {
            break;
        }
        curve::load(drawn, output);
        SecureZeroMemory(drawn.data(), drawn.size());
        if (curve::valid_scalar(output)) {
            return true;
        }
    }
    SecureZeroMemory(drawn.data(), drawn.size());
    output = {};
    return false;
}

} // namespace

/** Generates one key pair and agrees a secret with the peer's exported public key. */
bool agree(std::span<const std::byte> peerPublicKey, Agreement& output) noexcept {
    std::array<std::byte, kFieldSize> peerX{};
    std::array<std::byte, kFieldSize> peerY{};
    if (!decode_public_key(peerPublicKey, peerX, peerY)) {
        return false;
    }
    curve::Point peer{};
    curve::load(peerX, peer.x);
    curve::load(peerY, peer.y);
    if (!curve::on_curve(peer)) {
        return false;
    }

    curve::Field secret{};
    if (!generate_scalar(secret)) {
        return false;
    }
    curve::Point ours{};
    curve::Point agreed{};
    const bool multiplied =
        curve::multiply(secret, curve::generator(), ours) && curve::multiply(secret, peer, agreed);
    SecureZeroMemory(secret.data(), secret.size() * sizeof(secret[0]));
    if (!multiplied) {
        return false;
    }

    std::array<std::byte, kFieldSize> ourX{};
    std::array<std::byte, kFieldSize> ourY{};
    curve::store(ours.x, ourX);
    curve::store(ours.y, ourY);
    curve::store(agreed.x, output.sharedSecret);
    // The agreed point is secret material, so only the stored copy may outlive this call.
    SecureZeroMemory(&agreed, sizeof(agreed));
    if (!encode_public_key(ourX, ourY, output.publicKey)) {
        SecureZeroMemory(output.sharedSecret.data(), output.sharedSecret.size());
        output = {};
        return false;
    }
    return true;
}

} // namespace sunrise::middleware::crypto::ecc
