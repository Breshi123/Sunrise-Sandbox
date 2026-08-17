/**
 * secp224r1 point arithmetic, in process and with no platform provider.
 * Windows offers this curve through CNG, Wine does not, so the agreement carries its own math.
 * Field values live in the Montgomery domain between the entry points.
 */

#include "ecc_p224_curve.h"

#include <algorithm>

namespace sunrise::middleware::crypto::ecc::curve {
namespace {

/** Bits in one field word. */
constexpr unsigned kWordBits = 32;
/** Bytes in one field word. */
constexpr std::size_t kWordBytes = 4;
/** Bits in one byte of the wire form. */
constexpr unsigned kByteBits = 8;
/** Every word of a value the mask selects. */
constexpr std::uint32_t kAllBits = 0xFFFFFFFFU;

/** p = 2^224 - 2^96 + 1, the secp224r1 field prime. */
constexpr Field kPrime{
    0x00000001, 0x00000000, 0x00000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
/** p - 2, the exponent that inverts a field element by Fermat's theorem. */
constexpr Field kPrimeMinusTwo{
    0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFE, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
/** n, the order of the generator. A private key must be below it. */
constexpr Field kOrder{
    0x5C5C2A3D, 0x13DD2945, 0xE0B8F03E, 0xFFFF16A2, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
/** b of the curve equation y^2 = x^3 - 3x + b. */
constexpr Field kCoefficientB{
    0x2355FFB4, 0x270B3943, 0xD7BFD8BA, 0x5044B0B7, 0xF5413256, 0x0C04B3AB, 0xB4050A85};
/** x of the generator. */
constexpr Field kGeneratorX{
    0x115C1D21, 0x343280D6, 0x56C21122, 0x4A03C1D3, 0x321390B9, 0x6BB4BF7F, 0xB70E0CBD};
/** y of the generator. */
constexpr Field kGeneratorY{
    0x85007E34, 0x44D58199, 0x5A074764, 0xCD4375A0, 0x4C22DFE6, 0xB5F723FB, 0xBD376388};
/** R mod p, which is the value one takes in the Montgomery domain. */
constexpr Field kMontgomeryOne{
    0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000000, 0x00000000, 0x00000000, 0x00000000};
/** R squared mod p, the multiplier that moves a plain value into the Montgomery domain. */
constexpr Field kMontgomeryR2{
    0x00000001, 0x00000000, 0x00000000, 0xFFFFFFFE, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000000};
/** -p^-1 mod 2^32, the word each reduction step multiplies by. p ends in 1, so this is all bits. */
constexpr std::uint32_t kMontgomeryFactor = 0xFFFFFFFFU;

/** One point in Jacobian coordinates, where x = X/Z^2 and y = Y/Z^3. Z of zero is infinity. */
struct Jacobian {
    Field x{};
    Field y{};
    Field z{};
};

/**
 * Adds two values as plain words.
 * @param augend Value added to.
 * @param addend Value added.
 * @param sum Receives the low 224 bits.
 * @return The carry out of the top word.
 */
[[nodiscard]] std::uint32_t
add_words(const Field& augend, const Field& addend, Field& sum) noexcept {
    std::uint64_t carry = 0;
    for (std::size_t index = 0; index < kWords; ++index) {
        const std::uint64_t total =
            static_cast<std::uint64_t>(augend[index]) + addend[index] + carry;
        sum[index] = static_cast<std::uint32_t>(total);
        carry = total >> kWordBits;
    }
    return static_cast<std::uint32_t>(carry);
}

/**
 * Subtracts one value from another as plain words.
 * @param minuend Value subtracted from.
 * @param subtrahend Value subtracted.
 * @param difference Receives the low 224 bits.
 * @return One when the subtraction borrowed, which means the minuend is the smaller value.
 */
[[nodiscard]] std::uint32_t
subtract_words(const Field& minuend, const Field& subtrahend, Field& difference) noexcept {
    std::uint64_t borrow = 0;
    for (std::size_t index = 0; index < kWords; ++index) {
        const std::uint64_t total =
            static_cast<std::uint64_t>(minuend[index]) - subtrahend[index] - borrow;
        difference[index] = static_cast<std::uint32_t>(total);
        borrow = (total >> kWordBits) & 1U;
    }
    return static_cast<std::uint32_t>(borrow);
}

/** @return True when every word is zero. */
[[nodiscard]] bool is_zero(const Field& value) noexcept {
    std::uint32_t bits = 0;
    for (const std::uint32_t word : value) {
        bits |= word;
    }
    return bits == 0;
}

/** @return True when left is below right. */
[[nodiscard]] bool less_than(const Field& left, const Field& right) noexcept {
    Field ignored{};
    return subtract_words(left, right, ignored) != 0;
}

/**
 * Picks one of two values without branching on the choice.
 * @param take True to take the first value.
 * @param first Value taken when the choice holds.
 * @param second Value taken otherwise.
 * @param output Receives the picked value.
 */
void select(bool take, const Field& first, const Field& second, Field& output) noexcept {
    const std::uint32_t mask = take ? kAllBits : 0U;
    for (std::size_t index = 0; index < kWords; ++index) {
        output[index] = (first[index] & mask) | (second[index] & ~mask);
    }
}

/**
 * Multiplies in the Montgomery domain and reduces in the same pass.
 * @param multiplicand First factor, below p.
 * @param multiplier Second factor, below p.
 * @param product Receives multiplicand * multiplier * R^-1 mod p. May alias either factor.
 */
void montgomery_multiply(const Field& multiplicand,
                         const Field& multiplier,
                         Field& product) noexcept {
    // Two words above the field hold the running carries the reduction consumes.
    std::array<std::uint32_t, kWords + 2> accumulator{};
    for (std::size_t step = 0; step < kWords; ++step) {
        std::uint64_t carry = 0;
        for (std::size_t index = 0; index < kWords; ++index) {
            const std::uint64_t sum =
                static_cast<std::uint64_t>(accumulator[index])
                + static_cast<std::uint64_t>(multiplicand[index]) * multiplier[step] + carry;
            accumulator[index] = static_cast<std::uint32_t>(sum);
            carry = sum >> kWordBits;
        }
        std::uint64_t top = static_cast<std::uint64_t>(accumulator[kWords]) + carry;
        accumulator[kWords] = static_cast<std::uint32_t>(top);
        accumulator[kWords + 1] = static_cast<std::uint32_t>(top >> kWordBits);

        // Clearing the low word by a multiple of p is what divides the result by R.
        const auto factor = static_cast<std::uint32_t>(static_cast<std::uint64_t>(accumulator[0])
                                                       * kMontgomeryFactor);
        carry = (static_cast<std::uint64_t>(accumulator[0])
                 + static_cast<std::uint64_t>(factor) * kPrime[0])
                >> kWordBits;
        for (std::size_t index = 1; index < kWords; ++index) {
            const std::uint64_t sum = static_cast<std::uint64_t>(accumulator[index])
                                      + static_cast<std::uint64_t>(factor) * kPrime[index] + carry;
            accumulator[index - 1] = static_cast<std::uint32_t>(sum);
            carry = sum >> kWordBits;
        }
        top = static_cast<std::uint64_t>(accumulator[kWords]) + carry;
        accumulator[kWords - 1] = static_cast<std::uint32_t>(top);
        accumulator[kWords] =
            accumulator[kWords + 1] + static_cast<std::uint32_t>(top >> kWordBits);
    }

    Field result{};
    std::copy_n(accumulator.begin(), kWords, result.begin());
    Field reduced{};
    const std::uint32_t borrow = subtract_words(result, kPrime, reduced);
    // The result is below 2p, so the one conditional subtraction always finishes it.
    select(accumulator[kWords] != 0 || borrow == 0, reduced, result, product);
}

/** Adds two field elements mod p. The sum may alias either input. */
void field_add(const Field& augend, const Field& addend, Field& sum) noexcept {
    Field total{};
    const std::uint32_t carry = add_words(augend, addend, total);
    Field reduced{};
    const std::uint32_t borrow = subtract_words(total, kPrime, reduced);
    select(carry != 0 || borrow == 0, reduced, total, sum);
}

/** Subtracts one field element from another mod p. The difference may alias either input. */
void field_subtract(const Field& minuend, const Field& subtrahend, Field& difference) noexcept {
    Field total{};
    const std::uint32_t borrow = subtract_words(minuend, subtrahend, total);
    Field wrapped{};
    (void)add_words(total, kPrime, wrapped);
    select(borrow != 0, wrapped, total, difference);
}

/** Moves a plain value into the Montgomery domain. */
void to_montgomery(const Field& value, Field& output) noexcept {
    montgomery_multiply(value, kMontgomeryR2, output);
}

/** Moves a Montgomery value back to its plain form. */
void from_montgomery(const Field& value, Field& output) noexcept {
    Field one{};
    one[0] = 1;
    montgomery_multiply(value, one, output);
}

/**
 * Inverts a field element by raising it to p - 2.
 * @param value Montgomery value, not zero.
 * @param output Receives the Montgomery inverse.
 */
void montgomery_inverse(const Field& value, Field& output) noexcept {
    Field result = kMontgomeryOne;
    for (std::size_t index = kWords; index-- > 0;) {
        for (unsigned bit = kWordBits; bit-- > 0;) {
            montgomery_multiply(result, result, result);
            if (((kPrimeMinusTwo[index] >> bit) & 1U) != 0) {
                montgomery_multiply(result, value, result);
            }
        }
    }
    output = result;
}

/**
 * Doubles one Jacobian point, using that the curve has a of -3.
 * @param point Point to double, infinity allowed.
 * @param output Receives the doubled point. May alias the input.
 */
void jacobian_double(const Jacobian& point, Jacobian& output) noexcept {
    Field delta{};
    Field gamma{};
    Field beta{};
    Field alpha{};
    Field first{};
    Field second{};
    Field third{};
    montgomery_multiply(point.z, point.z, delta);
    montgomery_multiply(point.y, point.y, gamma);
    montgomery_multiply(point.x, gamma, beta);
    field_subtract(point.x, delta, first);
    field_add(point.x, delta, second);
    montgomery_multiply(first, second, third);
    field_add(third, third, alpha);
    field_add(alpha, third, alpha);

    Jacobian result{};
    montgomery_multiply(alpha, alpha, first);
    field_add(beta, beta, second);
    field_add(second, second, second);
    field_add(second, second, third);
    field_subtract(first, third, result.x);

    field_add(point.y, point.z, first);
    montgomery_multiply(first, first, first);
    field_subtract(first, gamma, first);
    field_subtract(first, delta, result.z);

    field_subtract(second, result.x, first);
    montgomery_multiply(alpha, first, first);
    montgomery_multiply(gamma, gamma, third);
    field_add(third, third, third);
    field_add(third, third, third);
    field_add(third, third, third);
    field_subtract(first, third, result.y);
    output = result;
}

/**
 * Adds two Jacobian points.
 * @param left First point, infinity allowed.
 * @param right Second point, infinity allowed.
 * @param output Receives the sum. May alias either input.
 */
void jacobian_add(const Jacobian& left, const Jacobian& right, Jacobian& output) noexcept {
    if (is_zero(left.z)) {
        output = right;
        return;
    }
    if (is_zero(right.z)) {
        output = left;
        return;
    }

    Field leftSquare{};
    Field rightSquare{};
    Field leftScaled{};
    Field rightScaled{};
    Field leftLine{};
    Field rightLine{};
    Field first{};
    Field second{};
    montgomery_multiply(left.z, left.z, leftSquare);
    montgomery_multiply(right.z, right.z, rightSquare);
    montgomery_multiply(left.x, rightSquare, leftScaled);
    montgomery_multiply(right.x, leftSquare, rightScaled);
    montgomery_multiply(right.z, rightSquare, first);
    montgomery_multiply(left.y, first, leftLine);
    montgomery_multiply(left.z, leftSquare, second);
    montgomery_multiply(right.y, second, rightLine);

    Field difference{};
    Field slope{};
    field_subtract(rightScaled, leftScaled, difference);
    field_subtract(rightLine, leftLine, slope);
    if (is_zero(difference)) {
        // Equal points need the doubling formula; opposite points sum to infinity.
        if (is_zero(slope)) {
            jacobian_double(left, output);
        } else {
            output = {};
        }
        return;
    }
    field_add(slope, slope, slope);

    Field square{};
    Field cube{};
    Field scaled{};
    field_add(difference, difference, square);
    montgomery_multiply(square, square, square);
    montgomery_multiply(difference, square, cube);
    montgomery_multiply(leftScaled, square, scaled);

    Jacobian result{};
    montgomery_multiply(slope, slope, first);
    field_subtract(first, cube, first);
    field_add(scaled, scaled, second);
    field_subtract(first, second, result.x);

    field_subtract(scaled, result.x, first);
    montgomery_multiply(slope, first, first);
    montgomery_multiply(leftLine, cube, second);
    field_add(second, second, second);
    field_subtract(first, second, result.y);

    field_add(left.z, right.z, first);
    montgomery_multiply(first, first, first);
    field_subtract(first, leftSquare, first);
    field_subtract(first, rightSquare, first);
    montgomery_multiply(first, difference, result.z);
    output = result;
}

/**
 * Converts a Jacobian point to affine coordinates.
 * @param point Point with a non-zero z.
 * @param output Receives the plain affine coordinates.
 */
void to_affine(const Jacobian& point, Point& output) noexcept {
    Field inverse{};
    Field square{};
    Field cube{};
    Field value{};
    montgomery_inverse(point.z, inverse);
    montgomery_multiply(inverse, inverse, square);
    montgomery_multiply(square, inverse, cube);
    montgomery_multiply(point.x, square, value);
    from_montgomery(value, output.x);
    montgomery_multiply(point.y, cube, value);
    from_montgomery(value, output.y);
}

} // namespace

/** @return The curve generator, which both sides multiply. */
Point generator() noexcept {
    return Point{kGeneratorX, kGeneratorY};
}

/** Checks that a point may be multiplied. */
bool on_curve(const Point& point) noexcept {
    if (!less_than(point.x, kPrime) || !less_than(point.y, kPrime)) {
        return false;
    }
    Field x{};
    Field y{};
    to_montgomery(point.x, x);
    to_montgomery(point.y, y);

    Field left{};
    Field right{};
    Field term{};
    montgomery_multiply(y, y, left);
    montgomery_multiply(x, x, right);
    montgomery_multiply(right, x, right);
    field_add(x, x, term);
    field_add(term, x, term);
    field_subtract(right, term, right);
    to_montgomery(kCoefficientB, term);
    field_add(right, term, right);
    return left == right;
}

/** Checks one private key. */
bool valid_scalar(const Field& scalar) noexcept {
    return !is_zero(scalar) && less_than(scalar, kOrder);
}

/** Multiplies a point by a scalar. */
bool multiply(const Field& scalar, const Point& point, Point& output) noexcept {
    Jacobian base{};
    to_montgomery(point.x, base.x);
    to_montgomery(point.y, base.y);
    base.z = kMontgomeryOne;

    // Both branches of every bit run, so the scalar does not steer the work that is done.
    Jacobian accumulator{};
    for (std::size_t index = kWords; index-- > 0;) {
        for (unsigned bit = kWordBits; bit-- > 0;) {
            Jacobian doubled{};
            jacobian_double(accumulator, doubled);
            Jacobian summed{};
            jacobian_add(doubled, base, summed);
            const bool take = ((scalar[index] >> bit) & 1U) != 0;
            select(take, summed.x, doubled.x, accumulator.x);
            select(take, summed.y, doubled.y, accumulator.y);
            select(take, summed.z, doubled.z, accumulator.z);
        }
    }
    if (is_zero(accumulator.z)) {
        return false;
    }
    to_affine(accumulator, output);
    return true;
}

/** Reads a wire value into a field element. */
void load(std::span<const std::byte> bytes, Field& output) noexcept {
    output = {};
    if (bytes.size() != kWords * kWordBytes) {
        return;
    }
    for (std::size_t index = 0; index < kWords; ++index) {
        const std::size_t offset = (kWords - 1 - index) * kWordBytes;
        std::uint32_t word = 0;
        for (std::size_t step = 0; step < kWordBytes; ++step) {
            word = (word << kByteBits) | std::to_integer<std::uint32_t>(bytes[offset + step]);
        }
        output[index] = word;
    }
}

/** Writes a field element in its wire form. */
void store(const Field& value, std::span<std::byte> bytes) noexcept {
    if (bytes.size() != kWords * kWordBytes) {
        return;
    }
    for (std::size_t index = 0; index < kWords; ++index) {
        const std::size_t offset = (kWords - 1 - index) * kWordBytes;
        for (std::size_t step = 0; step < kWordBytes; ++step) {
            const auto shift = static_cast<unsigned>((kWordBytes - 1 - step) * kByteBits);
            bytes[offset + step] = static_cast<std::byte>((value[index] >> shift) & 0xFFU);
        }
    }
}

} // namespace sunrise::middleware::crypto::ecc::curve
