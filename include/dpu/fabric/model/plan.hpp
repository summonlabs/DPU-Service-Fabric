#pragma once

// Deployment plans: the deterministic, dependency-aware placement result.

#include <cstdint>
#include <optional>
#include <vector>

#include "dpu/fabric/core/archive.hpp"
#include "dpu/fabric/core/types.hpp"
#include "dpu/fabric/model/lifecycle.hpp"
#include "dpu/fabric/model/service.hpp"

namespace dpu::fabric {

/// One planned replica: which DPU, in which isolation domain, at which
/// deterministic score.
struct PlannedReplica {
  ReplicaIndex index{};
  DpuId dpu{};
  HostAttachmentId attachment{};
  IsolationDomainId domain{};
  std::uint64_t score{0};
  Digest eligibility_digest{};

  friend bool operator==(const PlannedReplica&, const PlannedReplica&) = default;

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("PlannedReplica");
    field(ar, "index", index);
    field(ar, "dpu", dpu);
    field(ar, "attachment", attachment);
    field(ar, "domain", domain);
    field(ar, "score", score);
    field(ar, "eligibility_digest", eligibility_digest);
    ar.end_object();
  }
};

struct ServicePlan {
  ServiceId service{};
  ServiceVersion version{};
  std::uint32_t desired_replicas{0};
  std::uint32_t placed_replicas{0};
  std::vector<PlannedReplica> replicas{};

  friend bool operator==(const ServicePlan&, const ServicePlan&) = default;

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("ServicePlan");
    field(ar, "service", service);
    field(ar, "version", version);
    field(ar, "desired_replicas", desired_replicas);
    field(ar, "placed_replicas", placed_replicas);
    field(ar, "replicas", replicas);
    ar.end_object();
  }
};

/// One stage of a staged rollout. Stages are ordered by the planner; the runtime
/// never advances one on a timer.
struct PlanStage {
  std::uint32_t index{0};
  std::vector<InstanceId> instances{};
  std::uint32_t max_unavailable{0};

  friend bool operator==(const PlanStage&, const PlanStage&) = default;

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("PlanStage");
    field(ar, "index", index);
    field(ar, "instances", instances);
    field(ar, "max_unavailable", max_unavailable);
    ar.end_object();
  }
};

/// A complete, canonical plan. Two runs over identical accepted state produce
/// byte-identical plans and identical digests.
struct DeploymentPlan {
  PlanId id{};
  DeploymentGeneration generation{};
  ServiceGroupId group{};
  IntentKind kind{IntentKind::Deploy};
  std::optional<DeploymentGeneration> rollback_target{};
  std::vector<ServicePlan> services{};
  std::vector<PlanStage> stages{};
  ReasonCode outcome{ReasonCode::Ok};
  TopologyGeneration topology{};
  CapabilityGeneration capability{};
  PolicyGeneration policy{};
  CoordinatorEpoch epoch{};
  BootIncarnation boot{};
  LogicalInstant created_at{};
  Digest digest{};
  std::vector<Explanation> explanations{};

  /// Recomputes the plan identity from its canonical body and assigns id/digest.
  void seal();

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("DeploymentPlan");
    field(ar, "generation", generation);
    field(ar, "group", group);
    field(ar, "kind", kind);
    field(ar, "rollback_target", rollback_target);
    field(ar, "services", services);
    field(ar, "stages", stages);
    field(ar, "outcome", outcome);
    field(ar, "topology", topology);
    field(ar, "capability", capability);
    field(ar, "policy", policy);
    field(ar, "epoch", epoch);
    field(ar, "boot", boot);
    field(ar, "created_at", created_at);
    field(ar, "explanations", explanations);
    ar.end_object();
  }
};

/// The result of asking the planner for a plan. A refused outcome carries the
/// reasons and no plan identity: infeasible intents never produce a plan that
/// looks usable.
struct PlanOutcome {
  bool feasible{false};
  ReasonCode primary{ReasonCode::Ok};
  DeploymentPlan plan{};
  std::vector<Explanation> explanations{};
  std::vector<TruncationRecord> truncations{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("PlanOutcome");
    field(ar, "feasible", feasible);
    field(ar, "primary", primary);
    field(ar, "plan", plan);
    field(ar, "explanations", explanations);
    field(ar, "truncations", truncations);
    ar.end_object();
  }
};

}  // namespace dpu::fabric
