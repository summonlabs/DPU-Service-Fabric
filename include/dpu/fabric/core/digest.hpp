#pragma once

// SHA-256 digests. Digests are the runtime's canonical identity for state,
// plans, evidence and persisted records: two values with equal digests are
// required to have equal canonical encodings.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "dpu/fabric/core/result.hpp"

namespace dpu::fabric {

class Digest {
 public:
  static constexpr std::size_t kBytes = 32;

  constexpr Digest() noexcept = default;

  [[nodiscard]] static Digest from_bytes(const std::uint8_t* data) noexcept;
  [[nodiscard]] static Result<Digest> parse_hex(std::string_view text);
  [[nodiscard]] static std::span<const std::uint8_t> zero_bytes() noexcept;

  [[nodiscard]] const std::array<std::uint8_t, kBytes>& bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::uint8_t* mutable_bytes() noexcept { return bytes_.data(); }
  [[nodiscard]] bool is_zero() const noexcept;
  [[nodiscard]] std::string hex() const;
  /// First 16 hex characters; used for compact operator-facing labels only.
  [[nodiscard]] std::string short_hex() const;

  friend bool operator==(const Digest&, const Digest&) = default;
  friend std::strong_ordering operator<=>(const Digest&, const Digest&) = default;

 private:
  std::array<std::uint8_t, kBytes> bytes_{};
};

/// Streaming SHA-256. Only used to digest bounded in-memory buffers; no
/// unbounded streaming source is ever fed into it without a bound.
class Sha256 {
 public:
  Sha256() noexcept;
  void update(std::span<const std::uint8_t> data) noexcept;
  void update(std::string_view data) noexcept;
  [[nodiscard]] Digest finish() noexcept;

 private:
  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffered_{0};
  std::uint64_t total_{0};
  bool finished_{false};
};

[[nodiscard]] Digest sha256(std::span<const std::uint8_t> data) noexcept;
[[nodiscard]] Digest sha256(std::string_view data) noexcept;

}  // namespace dpu::fabric
