#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::crypto::ecc::curve {

/** A 224-bit value is 7 words of 32 bits, least significant word first. */
inline constexpr std::size_t kWords = 7;
/** Field elements and scalars share one representation. */
using Field = std::array<std::uint32_t, kWords>;

/** One affine point. Infinity has no affine form and is reported by a return value instead. */
struct Point {
    Field x{};
    Field y{};
};

/** @return The curve generator, which both sides multiply. */
[[nodiscard]] Point generator() noexcept;

/**
 * Checks that a point may be multiplied.
 * A peer key off the curve leaks the scalar, so an unchecked point is a key disclosure.
 * @param point Affine point, both coordinates in the range the field allows.
 * @return True when both coordinates are below p and satisfy the curve equation.
 */
[[nodiscard]] bool on_curve(const Point& point) noexcept;

/**
 * Checks one private key.
 * @param scalar Candidate key.
 * @return True when the scalar is in 1 to n-1, the range a private key must fall in.
 */
[[nodiscard]] bool valid_scalar(const Field& scalar) noexcept;

/**
 * Multiplies a point by a scalar.
 * @param scalar Multiplier, taken whole with no range check.
 * @param point Affine point on the curve.
 * @param output Receives the product only on success.
 * @return False when the product is infinity, which cannot be written as an affine point.
 */
[[nodiscard]] bool multiply(const Field& scalar, const Point& point, Point& output) noexcept;

/**
 * Reads a wire value into a field element.
 * @param bytes Exactly 28 bytes, high byte first.
 * @param output Receives the value, or zero when the width is wrong.
 */
void load(std::span<const std::byte> bytes, Field& output) noexcept;

/**
 * Writes a field element in its wire form.
 * @param value Field element.
 * @param bytes Exactly 28 bytes, high byte first. A wrong width writes nothing.
 */
void store(const Field& value, std::span<std::byte> bytes) noexcept;

} // namespace sunrise::middleware::crypto::ecc::curve
