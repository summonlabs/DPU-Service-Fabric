#pragma once

// Resource vectors and runtime bounds.

#include <cstddef>
#include <cstdint>

#include "dpu/fabric/core/types.hpp"

namespace dpu::fabric {

/// Additive resource requirements and capacities. Every operation is checked;
/// overflow is a refusal, never a wrap.
struct ResourceVector {
  std::uint64_t cpu_millicores{0};
  std::uint64_t memory_bytes{0};
  std::uint64_t network_bps{0};
  std::uint64_t crypto_ops_per_sec{0};
  std::uint64_t storage_bytes{0};

  friend constexpr bool operator==(const ResourceVector&, const ResourceVector&) = default;

  [[nodiscard]] static Result<ResourceVector> add(const ResourceVector& a,
                                                  const ResourceVector& b);
  /// Component-wise difference; refuses on underflow instead of wrapping.
  [[nodiscard]] static Result<ResourceVector> subtract(const ResourceVector& a,
                                                       const ResourceVector& b);
  [[nodiscard]] static Result<ResourceVector> scale(const ResourceVector& a, std::uint64_t count);
  /// True when every component of `required` is covered by `*this`.
  [[nodiscard]] bool covers(const ResourceVector& required) const noexcept;
  [[nodiscard]] bool is_zero() const noexcept;
  /// Remaining headroom; refuses when `other` is not covered.
  [[nodiscard]] Result<ResourceVector> headroom(const ResourceVector& other) const;
  /// Headroom of `this` against `other` expressed in permille (0..1000).
  /// Refuses when `other` is zero or not covered. Integer only: no floating
  /// point is used anywhere in a deterministic decision path.
  [[nodiscard]] Result<std::uint32_t> headroom_permille(const ResourceVector& other) const;

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("ResourceVector");
    field(ar, "cpu_millicores", cpu_millicores);
    field(ar, "memory_bytes", memory_bytes);
    field(ar, "network_bps", network_bps);
    field(ar, "crypto_ops_per_sec", crypto_ops_per_sec);
    field(ar, "storage_bytes", storage_bytes);
    ar.end_object();
  }
};

/// Hard ceilings for every externally influenced allocation. RuntimeBounds
/// validates against compile-time ceilings, so no configuration can lift a bound
/// into unbounded territory.
struct RuntimeBounds {
  static constexpr std::size_t kMaxDpusCeiling = 4096;
  static constexpr std::size_t kMaxServicesCeiling = 1024;
  static constexpr std::size_t kMaxInstancesCeiling = 16384;
  static constexpr std::size_t kMaxBatchEventsCeiling = 4096;
  static constexpr std::size_t kMaxDependenciesCeiling = 4096;
  static constexpr std::size_t kMaxCapabilitiesPerDpuCeiling = 256;
  static constexpr std::size_t kMaxReplicasPerServiceCeiling = 64;
  static constexpr std::size_t kMaxHistoryCeiling = 8192;
  static constexpr std::size_t kMaxJournalBytesCeiling = 64u * 1024u * 1024u;
  static constexpr std::size_t kMaxFrameBytesCeiling = 4u * 1024u * 1024u;
  static constexpr std::size_t kMaxPendingRequestsCeiling = 1024;
  static constexpr std::size_t kMaxPlansRetainedCeiling = 512;
  static constexpr std::size_t kMaxDecisionsRetainedCeiling = 4096;
  static constexpr std::size_t kMaxStagesCeiling = 256;
  static constexpr std::size_t kMaxWorkersCeiling = 64;

  std::size_t max_dpus{1024};
  std::size_t max_services{256};
  std::size_t max_instances{4096};
  std::size_t max_batch_events{1024};
  std::size_t max_dependencies{1024};
  std::size_t max_capabilities_per_dpu{64};
  std::size_t max_replicas_per_service{16};
  std::size_t max_history{2048};
  std::size_t max_plans_retained{64};
  std::size_t max_decisions_retained{512};
  std::size_t max_stages{64};
  std::size_t max_workers{8};
  std::size_t max_journal_bytes{4u * 1024u * 1024u};
  std::size_t max_frame_bytes{1024u * 1024u};
  std::size_t max_pending_requests{128};
  std::size_t max_duplicate_window{4096};
  std::size_t max_explanations{64};
  std::size_t max_pending_intents{64};
  std::size_t max_text_bytes{256};
  std::uint64_t evidence_validity_ticks{64};

  [[nodiscard]] Status validate() const;
};

}  // namespace dpu::fabric
