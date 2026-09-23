#pragma once

// The fabric runtime: the single place where accepted state lives and changes.
//
// Concurrency model. Every mutation arrives as a staged event. Staged events are
// held in a bounded reorder buffer and released by flush() in a deterministic
// total order over their event keys, so the accepted state is a function of the
// accepted event set rather than of arrival order or thread interleaving.
// Queries are read-only and never observe a partially applied event.
//
// Durability model. An event that is accepted is appended to the write-ahead
// journal before it is applied, so a crash after the append but before the reply
// leaves the event durable and replayed on recovery, while a crash before the
// append leaves no trace. Nothing is ever reported as applied that is not
// durable, and nothing is ever reported as verified that has not been proven by
// a matching, fresh effect report.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dpu/fabric/core/bounded.hpp"
#include "dpu/fabric/engine/authority.hpp"
#include "dpu/fabric/engine/dependency.hpp"
#include "dpu/fabric/engine/planner.hpp"
#include "dpu/fabric/model/event.hpp"
#include "dpu/fabric/persist/store.hpp"

namespace dpu::fabric {

struct RuntimeConfig {
  RuntimeBounds bounds{};
  StoreId store_id{};
  OriginId coordinator{};
  CoordinatorEpoch epoch{};
  BootIncarnation boot{};
  /// When set, state is journaled to and recovered from this directory.
  std::optional<std::string> store_directory{};
  bool fsync_journal{true};
  PlacementWeights weights{};
};

/// Outcome of one event.
struct EventOutcome {
  EventId id{};
  ReasonCode code{ReasonCode::Ok};
  bool applied{false};
  std::string detail{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("EventOutcome");
    field(ar, "id", id);
    field(ar, "code", code);
    field(ar, "applied", applied);
    field(ar, "detail", detail);
    ar.end_object();
  }
};

/// Outcome of one staged batch.
struct BatchReport {
  LogicalInstant upto{};
  std::uint64_t applied{0};
  std::uint64_t suppressed{0};
  std::uint64_t refused{0};
  std::uint64_t evicted{0};
  Digest state_digest{};
  std::vector<EventOutcome> outcomes{};
  std::vector<TruncationRecord> truncations{};
  ReasonCode primary{ReasonCode::Ok};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("BatchReport");
    field(ar, "upto", upto);
    field(ar, "applied", applied);
    field(ar, "suppressed", suppressed);
    field(ar, "refused", refused);
    field(ar, "evicted", evicted);
    field(ar, "state_digest", state_digest);
    field(ar, "outcomes", outcomes);
    field(ar, "truncations", truncations);
    field(ar, "primary", primary);
    ar.end_object();
  }
};

/// A dump of the runtime's accepted state. This is the canonical
/// machine-readable export and the unit of persistence and digest comparison.
struct RuntimeState {
  StoreId store_id{};
  OriginId coordinator{};
  CoordinatorEpoch epoch{};
  BootIncarnation boot{};
  LogicalInstant clock{};
  LogicalInstant watermark{};
  TopologyGeneration topology_generation{};
  CapabilityGeneration capability_generation{};
  PolicyGeneration policy_generation{};
  DeploymentGeneration deployment_generation{};
  PolicyState policy{};
  TopologySnapshot topology{};
  std::vector<ServiceDefinition> services{};
  std::vector<ServiceGroup> groups{};
  std::vector<Dependency> dependencies{};
  std::vector<InstanceState> instances{};
  std::vector<DeploymentPlan> plans{};
  AuthorityRegistry authority{};
  FenceIssuer fences{};
  TruncationLedger truncations{};
  std::vector<Decision> decisions{};
  /// Instances whose proven effects were invalidated by recovery.
  std::vector<InstanceId> restart_fenced{};
  /// Accepted events applied since the last checkpoint.
  std::uint64_t journaled_events{0};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("RuntimeState");
    field(ar, "store_id", store_id);
    field(ar, "coordinator", coordinator);
    field(ar, "epoch", epoch);
    field(ar, "boot", boot);
    field(ar, "clock", clock);
    field(ar, "watermark", watermark);
    field(ar, "topology_generation", topology_generation);
    field(ar, "capability_generation", capability_generation);
    field(ar, "policy_generation", policy_generation);
    field(ar, "deployment_generation", deployment_generation);
    field(ar, "policy", policy);
    field(ar, "topology", topology);
    field(ar, "services", services);
    field(ar, "groups", groups);
    field(ar, "dependencies", dependencies);
    field(ar, "instances", instances);
    field(ar, "plans", plans);
    field(ar, "authority", authority);
    field(ar, "fences", fences);
    field(ar, "truncations", truncations);
    field(ar, "decisions", decisions);
    field(ar, "restart_fenced", restart_fenced);
    field(ar, "journaled_events", journaled_events);
    ar.end_object();
  }
};

/// The part of the state that must be identical before and after a restart.
/// Proven liveness, health freshness, authority grants and fence serials are
/// deliberately excluded: those are expected to be invalidated by recovery.
struct PlacementProjection {
  StoreId store_id{};
  CoordinatorEpoch epoch{};
  LogicalInstant clock{};
  LogicalInstant watermark{};
  TopologyGeneration topology_generation{};
  CapabilityGeneration capability_generation{};
  PolicyGeneration policy_generation{};
  DeploymentGeneration deployment_generation{};
  PolicyState policy{};
  std::vector<ServiceDefinition> services{};
  std::vector<ServiceGroup> groups{};
  std::vector<Dependency> dependencies{};
  std::vector<DeploymentPlan> plans{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("PlacementProjection");
    field(ar, "store_id", store_id);
    field(ar, "epoch", epoch);
    field(ar, "clock", clock);
    field(ar, "watermark", watermark);
    field(ar, "topology_generation", topology_generation);
    field(ar, "capability_generation", capability_generation);
    field(ar, "policy_generation", policy_generation);
    field(ar, "deployment_generation", deployment_generation);
    field(ar, "policy", policy);
    field(ar, "services", services);
    field(ar, "groups", groups);
    field(ar, "dependencies", dependencies);
    field(ar, "plans", plans);
    ar.end_object();
  }
};

[[nodiscard]] PlacementProjection placement_projection(const RuntimeState& state);

/// Classification of what recovery did, reported rather than hidden.
struct RecoverySummary {
  RecoveryClass classification{RecoveryClass::Fresh};
  bool recovered_from_store{false};
  bool torn_tail{false};
  std::uint64_t journal_records_replayed{0};
  std::uint64_t records_dropped{0};
  CoordinatorEpoch previous_epoch{};
  BootIncarnation previous_boot{};
  std::vector<InstanceId> restart_fenced{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("RecoverySummary");
    field(ar, "classification", classification);
    field(ar, "recovered_from_store", recovered_from_store);
    field(ar, "torn_tail", torn_tail);
    field(ar, "journal_records_replayed", journal_records_replayed);
    field(ar, "records_dropped", records_dropped);
    field(ar, "previous_epoch", previous_epoch);
    field(ar, "previous_boot", previous_boot);
    field(ar, "restart_fenced", restart_fenced);
    ar.end_object();
  }
};

class FabricRuntime {
 public:
  /// Opens (or creates) a runtime. With a store directory configured this
  /// recovers durable state, advances the boot incarnation and fences every
  /// effect that was proven before the restart.
  [[nodiscard]] static Result<std::unique_ptr<FabricRuntime>> open(const RuntimeConfig& config);

  FabricRuntime(const FabricRuntime&) = delete;
  FabricRuntime& operator=(const FabricRuntime&) = delete;
  ~FabricRuntime();

  // -- ingest ---------------------------------------------------------------

  /// Stages one event. Staging never mutates accepted state; it orders input.
  [[nodiscard]] Status stage(const FabricEvent& event);

  /// Releases every staged event with \c at <= upto in deterministic order.
  [[nodiscard]] Result<BatchReport> flush(LogicalInstant upto);

  /// Stages and releases a whole batch. Equivalent to staging each event and
  /// flushing to the maximum logical instant in the batch.
  [[nodiscard]] Result<BatchReport> submit(const std::vector<FabricEvent>& events);

  // -- queries --------------------------------------------------------------

  /// Digest of the whole accepted state, including proven liveness, authority
  /// and fence serials. Changes across a restart, by design.
  [[nodiscard]] Digest state_digest() const;
  /// Digest of the durable placement projection: declared services, groups,
  /// dependencies, policy, generations, plans and instance placement identity.
  /// This is exactly the part of the state that must survive a restart
  /// unchanged, so a restart test asserts equality here.
  [[nodiscard]] Digest durable_digest() const;
  [[nodiscard]] RuntimeState snapshot() const;
  [[nodiscard]] JsonValue export_json() const;
  [[nodiscard]] const RecoverySummary& recovery() const noexcept { return recovery_; }
  [[nodiscard]] const RuntimeBounds& bounds() const noexcept { return config_.bounds; }
  [[nodiscard]] CoordinatorEpoch epoch() const;
  [[nodiscard]] BootIncarnation boot() const;
  [[nodiscard]] LogicalInstant clock() const;
  [[nodiscard]] std::size_t staged_count() const;
  [[nodiscard]] std::size_t instance_count() const;

  [[nodiscard]] std::optional<InstanceState> instance(const InstanceId& id) const;
  /// Device record by identity. Returning a value (rather than a pointer into a
  /// temporary snapshot) keeps callers away from a dangling reference.
  [[nodiscard]] std::optional<DpuRecord> device(const DpuId& id) const;
  [[nodiscard]] std::optional<DeploymentPlan> latest_plan() const;
  [[nodiscard]] std::optional<Decision> last_decision() const;
  /// Deterministic explanation of the most recent decision.
  [[nodiscard]] std::vector<Explanation> explain_last_decision() const;
  /// Deterministic explanation of why one instance is in its current state.
  [[nodiscard]] std::vector<Explanation> explain_instance(const InstanceId& id) const;
  /// Deterministic explanation of one (DPU, service) eligibility verdict.
  [[nodiscard]] std::optional<Eligibility> explain_eligibility(const DpuId& dpu,
                                                               const ServiceId& service) const;

  /// Persists a snapshot, truncates the journal and reports the classification.
  [[nodiscard]] Status checkpoint();
  [[nodiscard]] Status close();
  [[nodiscard]] bool closed() const noexcept;

 private:
  struct Impl;
  explicit FabricRuntime(RuntimeConfig config);
  [[nodiscard]] Status checkpoint_locked();

  RuntimeConfig config_{};
  std::unique_ptr<Impl> impl_{};
  RecoverySummary recovery_{};
};

}  // namespace dpu::fabric
