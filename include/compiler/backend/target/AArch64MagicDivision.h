#pragma once

#include <cstdint>
#include <cstring>
#include <optional>

namespace compiler::backend::target::aarch64 {

struct SignedMagic32 {
  std::int32_t multiplier = 0;
  unsigned shift = 0;
};

// Compute the signed-multiply-high parameters from Hacker's Delight.  The
// emitted quotient still needs the standard add/sub adjustment and the final
// sign correction for truncation toward zero.
inline std::optional<SignedMagic32> signedMagic32(std::int32_t divisor) {
  if (divisor == 0 || divisor == 1 || divisor == -1) return std::nullopt;

  const std::uint64_t magnitude =
      divisor < 0
          ? static_cast<std::uint64_t>(-(static_cast<std::int64_t>(divisor)))
          : static_cast<std::uint64_t>(divisor);
  constexpr std::uint64_t signedMinimum = UINT64_C(1) << 31U;
  const std::uint64_t threshold = signedMinimum + (divisor < 0 ? 1U : 0U);
  const std::uint64_t adjusted = threshold - 1U - threshold % magnitude;

  unsigned precision = 31;
  std::uint64_t quotient1 = signedMinimum / adjusted;
  std::uint64_t remainder1 = signedMinimum % adjusted;
  std::uint64_t quotient2 = signedMinimum / magnitude;
  std::uint64_t remainder2 = signedMinimum % magnitude;
  std::uint64_t delta = 0;
  do {
    ++precision;
    quotient1 <<= 1U;
    remainder1 <<= 1U;
    if (remainder1 >= adjusted) {
      ++quotient1;
      remainder1 -= adjusted;
    }
    quotient2 <<= 1U;
    remainder2 <<= 1U;
    if (remainder2 >= magnitude) {
      ++quotient2;
      remainder2 -= magnitude;
    }
    delta = magnitude - remainder2;
  } while (quotient1 < delta ||
           (quotient1 == delta && remainder1 == 0));

  std::uint32_t multiplierBits =
      static_cast<std::uint32_t>(quotient2 + 1U);
  if (divisor < 0) multiplierBits = 0U - multiplierBits;
  std::int32_t multiplier = 0;
  std::memcpy(&multiplier, &multiplierBits, sizeof(multiplier));
  return SignedMagic32{multiplier, precision - 32U};
}

}  // namespace compiler::backend::target::aarch64
