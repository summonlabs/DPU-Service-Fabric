#include "dpu/fabric/core/resources.hpp"

#include <algorithm>
#include <cctype>

namespace dpu::fabric {

bool validate_identity_text(std::string_view text) noexcept {
  if (text.empty() || text.size() > 64) return false;
  const auto is_alpha = [](char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0;
  };
  const auto is_digit = [](char c) {
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
  };
  if (!is_alpha(text.front()) && !is_digit(text.front())) return false;
  for (char c : text) {
    if (is_alpha(c) || is_digit(c)) continue;
    if (c == '.' || c == '_' || c == ':' || c == '-') continue;
    return false;
  }
  return true;
}

Result<ServiceVersion> ServiceVersion::parse(std::string_view text) {
  if (text.empty()) return refuse(ReasonCode::MissingField, "empty version");
  if (text.size() > 64) return refuse(ReasonCode::OversizedInput, "version text too long");

  std::uint32_t parts[3] = {0, 0, 0};
  std::string prerelease;
  std::string_view body = text;
  const std::size_t dash = text.find('-');
  if (dash != std::string_view::npos) {
    body = text.substr(0, dash);
    prerelease.assign(text.substr(dash + 1));
    if (prerelease.empty()) return refuse(ReasonCode::ValueOutOfRange, "empty prerelease");
    if (prerelease.size() > kMaxPrerelease) {
      return refuse(ReasonCode::OversizedInput, "prerelease too long");
    }
    for (char c : prerelease) {
      if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '.') {
        return refuse(ReasonCode::InvalidEncoding, "prerelease charset");
      }
    }
  }

  std::size_t index = 0;
  std::size_t part_index = 0;
  while (index < body.size()) {
    if (part_index >= 3) return refuse(ReasonCode::ValueOutOfRange, "too many version components");
    std::size_t end = body.find('.', index);
    if (end == std::string_view::npos) end = body.size();
    const std::string_view component = body.substr(index, end - index);
    if (component.empty() || component.size() > 9) {
      return refuse(ReasonCode::ValueOutOfRange, "invalid version component");
    }
    std::uint32_t value = 0;
    for (char c : component) {
      if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
        return refuse(ReasonCode::InvalidEncoding, "non-numeric version component");
      }
      value = static_cast<std::uint32_t>(value * 10u + static_cast<std::uint32_t>(c - '0'));
    }
    parts[part_index] = value;
    ++part_index;
    index = end + 1;
  }
  if (part_index != 3) return refuse(ReasonCode::MalformedInput, "version requires major.minor.patch");
  return ServiceVersion{parts[0], parts[1], parts[2], std::move(prerelease)};
}

std::string ServiceVersion::to_string() const {
  std::string out;
  out.reserve(24);
  out.append(std::to_string(major_));
  out.push_back('.');
  out.append(std::to_string(minor_));
  out.push_back('.');
  out.append(std::to_string(patch_));
  if (!prerelease_.empty()) {
    out.push_back('-');
    out.append(prerelease_);
  }
  return out;
}

std::strong_ordering operator<=>(const ServiceVersion& lhs, const ServiceVersion& rhs) {
  if (const auto cmp = lhs.major_ <=> rhs.major_; cmp != 0) return cmp;
  if (const auto cmp = lhs.minor_ <=> rhs.minor_; cmp != 0) return cmp;
  if (const auto cmp = lhs.patch_ <=> rhs.patch_; cmp != 0) return cmp;
  if (lhs.prerelease_ == rhs.prerelease_) return std::strong_ordering::equal;
  if (lhs.prerelease_.empty()) return std::strong_ordering::greater;
  if (rhs.prerelease_.empty()) return std::strong_ordering::less;
  return lhs.prerelease_ <=> rhs.prerelease_;
}

std::string FirmwareApiLevel::to_string() const {
  std::string out;
  out.reserve(16);
  out.append(std::to_string(major));
  out.push_back('.');
  out.append(std::to_string(minor));
  return out;
}

bool ResourceVector::is_zero() const noexcept {
  return cpu_millicores == 0 && memory_bytes == 0 && network_bps == 0 &&
         crypto_ops_per_sec == 0 && storage_bytes == 0;
}

bool ResourceVector::covers(const ResourceVector& required) const noexcept {
  return cpu_millicores >= required.cpu_millicores && memory_bytes >= required.memory_bytes &&
         network_bps >= required.network_bps &&
         crypto_ops_per_sec >= required.crypto_ops_per_sec &&
         storage_bytes >= required.storage_bytes;
}

Result<ResourceVector> ResourceVector::add(const ResourceVector& a, const ResourceVector& b) {
  ResourceVector out;
  const std::uint64_t* lhs = &a.cpu_millicores;
  const std::uint64_t* rhs = &b.cpu_millicores;
  std::uint64_t* dst = &out.cpu_millicores;
  constexpr std::size_t kComponents = 5;
  const char* names[kComponents] = {"cpu_millicores", "memory_bytes", "network_bps",
                                    "crypto_ops_per_sec", "storage_bytes"};
  for (std::size_t i = 0; i < kComponents; ++i) {
    if (!checked_add(lhs[i], rhs[i], dst[i])) {
      return refuse(ReasonCode::ArithmeticOverflow, names[i]);
    }
  }
  return out;
}

Result<ResourceVector> ResourceVector::subtract(const ResourceVector& a, const ResourceVector& b) {
  ResourceVector out;
  const std::uint64_t* lhs = &a.cpu_millicores;
  const std::uint64_t* rhs = &b.cpu_millicores;
  std::uint64_t* dst = &out.cpu_millicores;
  constexpr std::size_t kComponents = 5;
  const char* names[kComponents] = {"cpu_millicores", "memory_bytes", "network_bps",
                                    "crypto_ops_per_sec", "storage_bytes"};
  for (std::size_t i = 0; i < kComponents; ++i) {
    if (!checked_sub(lhs[i], rhs[i], dst[i])) {
      return refuse(ReasonCode::ValueOutOfRange, names[i]);
    }
  }
  return out;
}

Result<ResourceVector> ResourceVector::scale(const ResourceVector& a, std::uint64_t count) {
  ResourceVector out;
  const std::uint64_t* src = &a.cpu_millicores;
  std::uint64_t* dst = &out.cpu_millicores;
  constexpr std::size_t kComponents = 5;
  const char* names[kComponents] = {"cpu_millicores", "memory_bytes", "network_bps",
                                    "crypto_ops_per_sec", "storage_bytes"};
  for (std::size_t i = 0; i < kComponents; ++i) {
    if (!checked_mul(src[i], count, dst[i])) {
      return refuse(ReasonCode::ArithmeticOverflow, names[i]);
    }
  }
  return out;
}

Result<ResourceVector> ResourceVector::headroom(const ResourceVector& other) const {
  if (!covers(other)) return refuse(ReasonCode::AllocationExhausted, "capacity not covered");
  return subtract(*this, other);
}

Result<std::uint32_t> ResourceVector::headroom_permille(const ResourceVector& other) const {
  if (other.is_zero()) return refuse(ReasonCode::ValueOutOfRange, "zero requirement");
  if (!covers(other)) return refuse(ReasonCode::AllocationExhausted, "capacity not covered");
  const std::uint64_t lhs[5] = {cpu_millicores, memory_bytes, network_bps, crypto_ops_per_sec,
                                storage_bytes};
  const std::uint64_t rhs[5] = {other.cpu_millicores, other.memory_bytes, other.network_bps,
                                other.crypto_ops_per_sec, other.storage_bytes};
  std::uint64_t worst = 1000;
  for (std::size_t i = 0; i < 5; ++i) {
    if (rhs[i] == 0) continue;
    // (lhs - rhs) / lhs * 1000, integer only.
    const std::uint64_t free_units = lhs[i] - rhs[i];
    const std::uint64_t ratio = (free_units * 1000u) / lhs[i];
    worst = std::min(worst, ratio);
  }
  std::uint32_t out = 0;
  if (!checked_narrow(worst, out)) return refuse(ReasonCode::ArithmeticOverflow, "permille");
  return out;
}

Status RuntimeBounds::validate() const {
  struct Check {
    std::size_t value;
    std::size_t ceiling;
    const char* name;
  };
  const Check checks[] = {
      {max_dpus, kMaxDpusCeiling, "max_dpus"},
      {max_services, kMaxServicesCeiling, "max_services"},
      {max_instances, kMaxInstancesCeiling, "max_instances"},
      {max_batch_events, kMaxBatchEventsCeiling, "max_batch_events"},
      {max_dependencies, kMaxDependenciesCeiling, "max_dependencies"},
      {max_capabilities_per_dpu, kMaxCapabilitiesPerDpuCeiling, "max_capabilities_per_dpu"},
      {max_replicas_per_service, kMaxReplicasPerServiceCeiling, "max_replicas_per_service"},
      {max_history, kMaxHistoryCeiling, "max_history"},
      {max_plans_retained, kMaxPlansRetainedCeiling, "max_plans_retained"},
      {max_decisions_retained, kMaxDecisionsRetainedCeiling, "max_decisions_retained"},
      {max_stages, kMaxStagesCeiling, "max_stages"},
      {max_workers, kMaxWorkersCeiling, "max_workers"},
      {max_journal_bytes, kMaxJournalBytesCeiling, "max_journal_bytes"},
      {max_frame_bytes, kMaxFrameBytesCeiling, "max_frame_bytes"},
      {max_pending_requests, kMaxPendingRequestsCeiling, "max_pending_requests"},
  };
  for (const Check& check : checks) {
    if (check.value == 0) return refuse(ReasonCode::ValueOutOfRange, check.name);
    if (check.value > check.ceiling) {
      return refuse(ReasonCode::BoundExceeded, check.name);
    }
  }
  if (max_duplicate_window == 0 || max_explanations == 0 || max_pending_intents == 0 ||
      max_text_bytes == 0) {
    return refuse(ReasonCode::ValueOutOfRange, "zero bound");
  }
  if (max_text_bytes > 4096) return refuse(ReasonCode::BoundExceeded, "max_text_bytes");
  if (max_duplicate_window > kMaxHistoryCeiling) {
    return refuse(ReasonCode::BoundExceeded, "max_duplicate_window");
  }
  if (max_explanations > kMaxHistoryCeiling) {
    return refuse(ReasonCode::BoundExceeded, "max_explanations");
  }
  if (max_pending_intents > kMaxServicesCeiling) {
    return refuse(ReasonCode::BoundExceeded, "max_pending_intents");
  }
  if (evidence_validity_ticks == 0) {
    return refuse(ReasonCode::ValueOutOfRange, "evidence_validity_ticks");
  }
  return Status::success();
}

}  // namespace dpu::fabric
