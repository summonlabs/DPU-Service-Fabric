#include "dpu/fabric/core/reason.hpp"

#include <array>

namespace dpu::fabric {
namespace {

struct ReasonEntry {
  ReasonCode code;
  std::string_view name;
};

constexpr std::array<ReasonEntry, 0
#define DPUF_REASON_COUNT(name, value) +1
                             DPUF_REASON_CODES(DPUF_REASON_COUNT)
#undef DPUF_REASON_COUNT
                         >
    kReasonTable = {{
#define DPUF_REASON_ROW(name, value) ReasonEntry{ReasonCode::name, #name},
        DPUF_REASON_CODES(DPUF_REASON_ROW)
#undef DPUF_REASON_ROW
    }};

}  // namespace

std::string_view to_string(ReasonCode code) noexcept {
  for (const ReasonEntry& entry : kReasonTable) {
    if (entry.code == code) return entry.name;
  }
  return "Unknown";
}

bool reason_from_string(std::string_view text, ReasonCode& out) noexcept {
  for (const ReasonEntry& entry : kReasonTable) {
    if (entry.name == text) {
      out = entry.code;
      return true;
    }
  }
  return false;
}

std::string_view reason_band(ReasonCode code) noexcept {
  const auto value = static_cast<std::uint16_t>(code);
  if (value == 0) return "success";
  if (value < 100) return "input";
  if (value < 200) return "authority";
  if (value < 300) return "capability";
  if (value < 400) return "dependency";
  if (value < 500) return "placement";
  if (value < 600) return "lifecycle";
  if (value < 800) return "persistence";
  if (value < 900) return "transport";
  if (value < 1000) return "bounds";
  return "unknown";
}

}  // namespace dpu::fabric
