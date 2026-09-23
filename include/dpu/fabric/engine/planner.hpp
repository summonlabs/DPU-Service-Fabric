#pragma once

// Deterministic, dependency-aware placement planning.
//
// The planner is a pure function of accepted state: the same topology,
// capabilities, policy, declarations and intent always produce the same plan,
// byte for byte, and therefore the same plan identity. Nothing in the planner
// consults the wall clock, a random source, container iteration order or the
// order in which intents happened to arrive.

#include <cstdint>
#include <string>
#include <vector>

#include "dpu/fabric/core/archive.hpp"
#include "dpu/fabric/engine/authority.hpp"
#include "dpu/fabric/engine/dependency.hpp"
#include "dpu/fabric/engine/eligibility.hpp"
#include "dpu/fabric/model/plan.hpp"

namespace dpu::fabric {

/// Everything the planner is allowed to look at.
struct PlanRequest {
  const TopologySnapshot* topology{nullptr};
  const PolicyState* policy{nullptr};
  const std::vector<ServiceDefinition>* services{nullptr};
  const DependencyGraph* dependencies{nullptr};
  const ServiceGroup* group{nullptr};
  const PlacementIntent* intent{nullptr};
  const AuthorityRegistry* authority{nullptr};
  /// Deployment generation the plan must carry. Must not regress.
  DeploymentGeneration generation{};
  LogicalInstant now{};
  CoordinatorEpoch epoch{};
  BootIncarnation boot{};
  const RuntimeBounds* bounds{nullptr};
};

/// Deterministic scoring weights. Documented so that a reader can reproduce a
/// placement decision by hand.
struct PlacementWeights {
  /// Multiplied by the resource headroom permille (0..1000).
  std::uint64_t capacity{1000};
  /// Added when the DPU supports the exact isolation kind requested.
  std::uint64_t isolation_match{100000};
  /// Multiplied by the remaining anti-affinity slack in the DPU's domain.
  std::uint64_t domain_spread{100};
  /// Subtracted per replica already planned on the same DPU.
  std::uint64_t dpu_pressure{10};
};

/// Computes a plan, or refuses with explicit reasons.
///
/// A refused intent produces \c feasible == false and no usable plan identity;
/// an infeasible intent never yields something that looks deployable.
[[nodiscard]] PlanOutcome build_plan(const PlanRequest& request,
                                     const PlacementWeights& weights = {});

/// Canonical instance identity for one replica of one service in one group.
/// Stable across deployment generations so that a redeploy reuses the instance
/// and bumps its incarnation instead of inventing a new identity.
[[nodiscard]] InstanceId make_instance_id(const ServiceGroupId& group, const ServiceId& service,
                                          ReplicaIndex index);

/// Recomputes the canonical placement for a single (DPU, service) pair. Exposed
/// for tests and for the explanation surface.
[[nodiscard]] std::uint64_t score_placement(const DpuRecord& dpu,
                                            const ServiceDefinition& service,
                                            std::uint32_t replicas_on_dpu,
                                            std::uint32_t replicas_in_domain,
                                            const PlacementWeights& weights);

}  // namespace dpu::fabric
