#include "dpu/fabric/core/bounded.hpp"

#include "dpu/fabric/core/types.hpp"

namespace dpu::fabric {

Status TruncationLedger::record(std::string_view container, std::uint64_t requested,
                                std::uint64_t accepted, ReasonCode reason) {
  if (accepted > requested) {
    return refuse(ReasonCode::InvalidAccounting, "accepted exceeds requested");
  }
  const std::uint64_t dropped = requested - accepted;
  if (container.empty() || container.size() > 64) {
    return refuse(ReasonCode::ValueOutOfRange, "container label length");
  }
  for (TruncationRecord& existing : records_) {
    if (existing.container == container && existing.reason == reason) {
      std::uint64_t total_requested = 0;
      std::uint64_t total_accepted = 0;
      if (!checked_add(existing.requested, requested, total_requested) ||
          !checked_add(existing.accepted, accepted, total_accepted)) {
        return refuse(ReasonCode::ArithmeticOverflow, "truncation accounting overflow");
      }
      existing.requested = total_requested;
      existing.accepted = total_accepted;
      existing.dropped = total_requested - total_accepted;
      return Status::success();
    }
  }
  if (records_.size() >= kMaxRecords) {
    // The ledger itself is bounded; overflow is counted rather than lost.
    if (!checked_add(ledger_overflow_, std::uint64_t{1}, ledger_overflow_)) {
      return refuse(ReasonCode::ArithmeticOverflow, "ledger overflow counter");
    }
    return Status{ReasonCode::BoundExceeded, "truncation ledger full"};
  }
  TruncationRecord record;
  record.container.assign(container);
  record.requested = requested;
  record.accepted = accepted;
  record.dropped = dropped;
  record.reason = reason;
  records_.push_back(std::move(record));
  return Status::success();
}

std::uint64_t TruncationLedger::total_dropped() const noexcept {
  std::uint64_t total = 0;
  for (const TruncationRecord& record : records_) total = saturating_add(total, record.dropped);
  return total;
}

std::uint64_t TruncationLedger::total_requested() const noexcept {
  std::uint64_t total = 0;
  for (const TruncationRecord& record : records_) total = saturating_add(total, record.requested);
  return total;
}

std::uint64_t TruncationLedger::total_accepted() const noexcept {
  std::uint64_t total = 0;
  for (const TruncationRecord& record : records_) total = saturating_add(total, record.accepted);
  return total;
}

void TruncationLedger::clear() {
  records_.clear();
  ledger_overflow_ = 0;
}

}  // namespace dpu::fabric
