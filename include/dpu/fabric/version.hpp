#pragma once

// DPU Service Fabric - version and build identity.
//
// The numeric version triple is part of the public contract: it is published in
// the exported CMake package, in the canonical machine-readable export, and in
// the persistence format's semantic version gate.

#include <cstdint>
#include <string>
#include <string_view>

namespace dpu::fabric {

inline constexpr std::uint32_t kVersionMajor = 1;
inline constexpr std::uint32_t kVersionMinor = 0;
inline constexpr std::uint32_t kVersionPatch = 0;

/// Semantic version of the runtime's persisted semantics. A store written by a
/// different semantic version carries meaning that this build cannot interpret,
/// so recovery refuses it rather than guessing.
inline constexpr std::uint32_t kSemanticVersion = 1;

/// Format revision of the on-disk container (framing, checksums, layout).
inline constexpr std::uint32_t kFormatVersion = 1;

inline constexpr std::string_view kProductName = "DPU Service Fabric";
inline constexpr std::string_view kVendorName = "Summon Software Labs";

/// "major.minor.patch"
[[nodiscard]] std::string_view version_string() noexcept;

/// Full identity string, e.g. "DPU Service Fabric 1.0.0 (format 1, semantics 1)".
[[nodiscard]] std::string version_banner();

}  // namespace dpu::fabric
