#pragma once

// Bounded containers and truncation accounting.
//
// Every place where the runtime stops accepting externally influenced volume is
// accounted for here. The ledger aggregates by container and reason so that its
// own memory is bounded, and it enforces the accounting identity
// requested == accepted + dropped. A truncation that is not observable would be a
// silent loss of information, which this runtime refuses to have.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dpu/fabric/core/result.hpp"

namespace dpu::fabric {

struct TruncationRecord {
  std::string container{};
  std::uint64_t requested{0};
  std::uint64_t accepted{0};
  std::uint64_t dropped{0};
  ReasonCode reason{ReasonCode::TruncationDropped};

  friend bool operator==(const TruncationRecord&, const TruncationRecord&) = default;
  friend bool operator<(const TruncationRecord& lhs, const TruncationRecord& rhs) {
    if (lhs.container != rhs.container) return lhs.container < rhs.container;
    return static_cast<std::uint16_t>(lhs.reason) < static_cast<std::uint16_t>(rhs.reason);
  }

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("TruncationRecord");
    field(ar, "container", container);
    field(ar, "requested", requested);
    field(ar, "accepted", accepted);
    field(ar, "dropped", dropped);
    field(ar, "reason", reason);
    ar.end_object();
  }
};

/// Aggregating, bounded account of every truncation, refusal-by-bound and
/// eviction the runtime performed.
class TruncationLedger {
 public:
  static constexpr std::size_t kMaxRecords = 64;

  /// Records one bounded decision. Enforces requested == accepted + dropped.
  Status record(std::string_view container, std::uint64_t requested, std::uint64_t accepted,
                ReasonCode reason);

  [[nodiscard]] const std::vector<TruncationRecord>& records() const noexcept { return records_; }
  [[nodiscard]] std::uint64_t total_dropped() const noexcept;
  [[nodiscard]] std::uint64_t total_requested() const noexcept;
  [[nodiscard]] std::uint64_t total_accepted() const noexcept;
  [[nodiscard]] std::size_t distinct_containers() const noexcept { return records_.size(); }
  [[nodiscard]] bool empty() const noexcept { return records_.empty(); }
  /// Number of records that could not be retained because the ledger is full.
  [[nodiscard]] std::uint64_t ledger_overflow() const noexcept { return ledger_overflow_; }
  void clear();

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("TruncationLedger");
    field(ar, "records", records_);
    field(ar, "ledger_overflow", ledger_overflow_);
    ar.end_object();
  }

 private:
  std::vector<TruncationRecord> records_{};
  std::uint64_t ledger_overflow_{0};
};

/// A bounded FIFO with explicit eviction accounting.
template <class T>
class BoundedHistory {
 public:
  BoundedHistory() = default;
  explicit BoundedHistory(std::size_t capacity) : capacity_(capacity) {}

  void set_capacity(std::size_t capacity) {
    capacity_ = capacity;
    if (entries_.size() > capacity_) {
      const std::size_t excess = entries_.size() - capacity_;
      entries_.erase(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(excess));
      evicted_ += excess;
    }
  }

  /// Appends a value, evicting the oldest entry when the bound is reached.
  /// Returns the number of entries evicted by this call (0 or more).
  std::size_t push(T value) {
    if (capacity_ == 0) {
      ++rejected_;
      return 0;
    }
    std::size_t evicted = 0;
    while (entries_.size() >= capacity_) {
      entries_.erase(entries_.begin());
      ++evicted;
      ++evicted_;
    }
    entries_.push_back(std::move(value));
    return evicted;
  }

  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  [[nodiscard]] std::uint64_t evicted() const noexcept { return evicted_; }
  [[nodiscard]] std::uint64_t rejected() const noexcept { return rejected_; }
  [[nodiscard]] const std::vector<T>& entries() const noexcept { return entries_; }
  [[nodiscard]] std::vector<T>& entries() noexcept { return entries_; }
  [[nodiscard]] const T& back() const noexcept { return entries_.back(); }

  void clear() { entries_.clear(); }

 private:
  std::vector<T> entries_{};
  std::size_t capacity_{0};
  std::uint64_t evicted_{0};
  std::uint64_t rejected_{0};
};

/// Saturating conversion helper for externally derived sizes.
[[nodiscard]] inline std::uint64_t accounting_u64(std::size_t value) noexcept {
  return static_cast<std::uint64_t>(value);
}

}  // namespace dpu::fabric
