#pragma once

// Instance lifecycle: fences, attempts, verified effects, decisions and
// explanations.

#include <cstdint>
#include <string>
#include <vector>

#include "dpu/fabric/core/archive.hpp"
#include "dpu/fabric/core/bounded.hpp"
#include "dpu/fabric/core/types.hpp"
#include "dpu/fabric/model/capability.hpp"
#include "dpu/fabric/model/topology.hpp"

namespace dpu::fabric {

/// Authority token for one attempt on one instance.
///
/// A fence is valid only if it carries the runtime's current coordinator epoch
/// and boot incarnation and matches the instance's current serial. A token from
/// before a restart, or from a superseded attempt, can never publish an effect.
struct FenceToken {
  CoordinatorEpoch epoch{};
  BootIncarnation boot{};
  std::uint64_t serial{0};

  [[nodiscard]] bool valid() const noexcept { return serial != 0; }
  [[nodiscard]] bool from(CoordinatorEpoch other_epoch, BootIncarnation other_boot) const noexcept {
    return epoch == other_epoch && boot == other_boot;
  }

  friend bool operator==(const FenceToken&, const FenceToken&) = default;
  friend auto operator<=>(const FenceToken&, const FenceToken&) = default;

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("FenceToken");
    field(ar, "epoch", epoch);
    field(ar, "boot", boot);
    field(ar, "serial", serial);
    ar.end_object();
  }
};

/// One attempt to bring an instance to its intended state.
///
/// phase tracks where the attempt actually is: opening an attempt is not
/// completion, an acknowledgement is not a verified effect, and closing an
/// attempt records the outcome that was proven.
struct AttemptRecord {
  AttemptNumber number{};
  FenceToken fence{};
  LifecycleState phase{LifecycleState::Absent};
  AttemptOutcome outcome{AttemptOutcome::Open};
  LogicalInstant opened_at{};
  LogicalInstant closed_at{};
  Digest request_digest{};
  ReasonCode close_reason{ReasonCode::Ok};

  [[nodiscard]] bool open() const noexcept { return outcome == AttemptOutcome::Open; }

  friend bool operator==(const AttemptRecord&, const AttemptRecord&) = default;

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("AttemptRecord");
    field(ar, "number", number);
    field(ar, "fence", fence);
    field(ar, "phase", phase);
    field(ar, "outcome", outcome);
    field(ar, "opened_at", opened_at);
    field(ar, "closed_at", closed_at);
    field(ar, "request_digest", request_digest);
    field(ar, "close_reason", close_reason);
    ar.end_object();
  }
};

/// Why a decision was taken. Explanations name the evidence and the generations
/// that made a decision legal or illegal, so a reader can re-derive it.
struct Explanation {
  ReasonCode code{ReasonCode::Ok};
  DecisionKind kind{DecisionKind::Placement};
  std::string subject{};
  std::string note{};
  Digest evidence_digest{};
  TopologyGeneration topology{};
  CapabilityGeneration capability{};
  PolicyGeneration policy{};
  DeploymentGeneration deployment{};

  friend bool operator==(const Explanation&, const Explanation&) = default;
  friend bool operator<(const Explanation& lhs, const Explanation& rhs) {
    if (lhs.code != rhs.code) return lhs.code < rhs.code;
    if (lhs.subject != rhs.subject) return lhs.subject < rhs.subject;
    if (lhs.note != rhs.note) return lhs.note < rhs.note;
    return lhs.deployment < rhs.deployment;
  }

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("Explanation");
    field(ar, "code", code);
    field(ar, "kind", kind);
    field(ar, "subject", subject);
    field(ar, "note", note);
    field(ar, "evidence_digest", evidence_digest);
    field(ar, "topology", topology);
    field(ar, "capability", capability);
    field(ar, "policy", policy);
    field(ar, "deployment", deployment);
    ar.end_object();
  }
};

/// The runtime's record of one decision, with everything needed to audit it.
struct Decision {
  OperationId operation{};
  DecisionKind kind{DecisionKind::Placement};
  bool accepted{false};
  ReasonCode primary{ReasonCode::Ok};
  LogicalInstant at{};
  CoordinatorEpoch epoch{};
  BootIncarnation boot{};
  TopologyGeneration topology{};
  CapabilityGeneration capability{};
  PolicyGeneration policy{};
  DeploymentGeneration deployment{};
  Digest evidence_digest{};
  Digest policy_digest{};
  Digest decision_digest{};
  std::vector<Explanation> explanations{};
  std::vector<TruncationRecord> truncations{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("Decision");
    field(ar, "operation", operation);
    field(ar, "kind", kind);
    field(ar, "accepted", accepted);
    field(ar, "primary", primary);
    field(ar, "at", at);
    field(ar, "epoch", epoch);
    field(ar, "boot", boot);
    field(ar, "topology", topology);
    field(ar, "capability", capability);
    field(ar, "policy", policy);
    field(ar, "deployment", deployment);
    field(ar, "evidence_digest", evidence_digest);
    field(ar, "policy_digest", policy_digest);
    field(ar, "decision_digest", decision_digest);
    field(ar, "explanations", explanations);
    field(ar, "truncations", truncations);
    ar.end_object();
  }
};

/// One instance of one service replica.
///
/// state is the lifecycle position that has been *proven*; health is a separate
/// axis with its own freshness. A verified instance with stale health evidence
/// is never reported as healthy continuity.
struct InstanceState {
  InstanceId id{};
  ServiceId service{};
  ServiceGroupId group{};
  ServiceVersion version{};
  ReplicaIndex index{};
  Incarnation incarnation{};
  DpuId dpu{};
  HostAttachmentId attachment{};
  IsolationDomainId domain{};
  DeploymentGeneration deployment_generation{};
  LifecycleState state{LifecycleState::Absent};
  HealthState health{HealthState::Unknown};
  bool health_fresh{false};
  LogicalInstant health_observed_at{};
  TopologyGeneration topology_generation{};
  CapabilityGeneration capability_generation{};
  std::vector<AttemptRecord> attempts{};
  Digest verified_digest{};
  LogicalInstant verified_at{};
  EvidenceRef verified_evidence{};
  /// Incremented by every event that invalidates previously proven effects
  /// (DPU loss, restart fencing, replacement). An effect can only be verified
  /// against the current value.
  std::uint64_t effect_fence_serial{0};
  /// Instant before which effects can no longer be verified for this instance.
  /// Set by restart fencing, device loss and replacement.
  LogicalInstant effect_fenced_at{};
  ReasonCode fence_reason{ReasonCode::Ok};
  std::vector<Explanation> reasons{};

  [[nodiscard]] const AttemptRecord* current_attempt() const noexcept {
    return attempts.empty() ? nullptr : &attempts.back();
  }

  /// Serving requires a verified effect *and* fresh healthy evidence. Either
  /// alone is not enough.
  [[nodiscard]] bool serving() const noexcept {
    return state == LifecycleState::Verified && health == HealthState::Healthy && health_fresh;
  }

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("InstanceState");
    field(ar, "id", id);
    field(ar, "service", service);
    field(ar, "group", group);
    field(ar, "version", version);
    field(ar, "index", index);
    field(ar, "incarnation", incarnation);
    field(ar, "dpu", dpu);
    field(ar, "attachment", attachment);
    field(ar, "domain", domain);
    field(ar, "deployment_generation", deployment_generation);
    field(ar, "state", state);
    field(ar, "health", health);
    field(ar, "health_fresh", health_fresh);
    field(ar, "health_observed_at", health_observed_at);
    field(ar, "topology_generation", topology_generation);
    field(ar, "capability_generation", capability_generation);
    field(ar, "attempts", attempts);
    field(ar, "verified_digest", verified_digest);
    field(ar, "verified_at", verified_at);
    field(ar, "verified_evidence", verified_evidence);
    field(ar, "effect_fence_serial", effect_fence_serial);
    field(ar, "effect_fenced_at", effect_fenced_at);
    field(ar, "fence_reason", fence_reason);
    field(ar, "reasons", reasons);
    ar.end_object();
  }
};

/// An execution report from the runtime that actually applies effects. A report
/// is evidence; only a matching, fresh, successful report verifies an effect.
struct EffectReport {
  OperationId operation{};
  InstanceId instance{};
  AttemptNumber attempt{};
  FenceToken fence{};
  EffectKind kind{EffectKind::Deploy};
  EffectStatus status{EffectStatus::Failed};
  LogicalInstant reported_at{};
  EvidenceRef evidence{};
  Digest payload_digest{};
  Digest effect_digest{};
  bool partial{false};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("EffectReport");
    field(ar, "operation", operation);
    field(ar, "instance", instance);
    field(ar, "attempt", attempt);
    field(ar, "fence", fence);
    field(ar, "kind", kind);
    field(ar, "status", status);
    field(ar, "reported_at", reported_at);
    field(ar, "evidence", evidence);
    field(ar, "payload_digest", payload_digest);
    field(ar, "effect_digest", effect_digest);
    field(ar, "partial", partial);
    ar.end_object();
  }
};

}  // namespace dpu::fabric
