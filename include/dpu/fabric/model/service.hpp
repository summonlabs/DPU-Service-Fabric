#pragma once

// Service, group, dependency and placement-intent declarations.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dpu/fabric/core/archive.hpp"
#include "dpu/fabric/core/resources.hpp"
#include "dpu/fabric/core/types.hpp"
#include "dpu/fabric/model/capability.hpp"
#include "dpu/fabric/model/topology.hpp"

namespace dpu::fabric {

/// A service this runtime deploys. The runtime never implements the service; it
/// validates, places, authorizes and verifies.
struct ServiceDefinition {
  ServiceId id{};
  ServiceVersion version{};
  std::string display_name{};
  CompatibilityRequirement compatibility{};
  ResourceVector resources{};
  IsolationRequirement isolation{};
  ReplicaPolicy replicas{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("ServiceDefinition");
    field(ar, "id", id);
    field(ar, "version", version);
    field(ar, "display_name", display_name);
    field(ar, "compatibility", compatibility);
    field(ar, "resources", resources);
    field(ar, "isolation", isolation);
    field(ar, "replicas", replicas);
    ar.end_object();
  }
};

/// A typed dependency edge between two declared services.
struct Dependency {
  DependencyId id{};
  ServiceId from{};
  ServiceId to{};
  DependencyKind kind{DependencyKind::Requires};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("Dependency");
    field(ar, "id", id);
    field(ar, "from", from);
    field(ar, "to", to);
    field(ar, "kind", kind);
    ar.end_object();
  }
};

/// A set of services deployed and governed together, optionally owning an
/// exclusive scope over the DPUs it uses.
struct ServiceGroup {
  ServiceGroupId id{};
  std::vector<ServiceId> members{};
  std::optional<ExclusiveScopeId> exclusive_scope{};
  AntiAffinityPolicy anti_affinity{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("ServiceGroup");
    field(ar, "id", id);
    field(ar, "members", members);
    field(ar, "exclusive_scope", exclusive_scope);
    field(ar, "anti_affinity", anti_affinity);
    ar.end_object();
  }
};

struct ServiceReplicaIntent {
  ServiceId service{};
  std::uint32_t desired_replicas{0};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("ServiceReplicaIntent");
    field(ar, "service", service);
    field(ar, "desired_replicas", desired_replicas);
    ar.end_object();
  }
};

/// How a rollout is broken into stages. The runtime computes stage membership;
/// it never advances a stage on a timer.
struct StagedRolloutPolicy {
  std::uint32_t batch_size{1};
  std::uint32_t max_unavailable{0};
  bool verify_between_stages{true};

  [[nodiscard]] Status validate(std::size_t ceiling) const;

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("StagedRolloutPolicy");
    field(ar, "batch_size", batch_size);
    field(ar, "max_unavailable", max_unavailable);
    field(ar, "verify_between_stages", verify_between_stages);
    ar.end_object();
  }
};

/// An operator's or controller's stated placement intent. An intent is a
/// request; it becomes authoritative only when the runtime accepts it.
struct PlacementIntent {
  OperationId id{};
  ServiceGroupId group{};
  IntentKind kind{IntentKind::Deploy};
  DeploymentGeneration generation{};
  std::optional<DeploymentGeneration> rollback_target{};
  std::vector<ServiceReplicaIntent> services{};
  StagedRolloutPolicy staging{};
  OriginId origin{};
  OriginEpoch origin_epoch{};
  Sequence origin_seq{};
  LogicalInstant submitted_at{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("PlacementIntent");
    field(ar, "id", id);
    field(ar, "group", group);
    field(ar, "kind", kind);
    field(ar, "generation", generation);
    field(ar, "rollback_target", rollback_target);
    field(ar, "services", services);
    field(ar, "staging", staging);
    field(ar, "origin", origin);
    field(ar, "origin_epoch", origin_epoch);
    field(ar, "origin_seq", origin_seq);
    field(ar, "submitted_at", submitted_at);
    ar.end_object();
  }
};

/// Policy supplied by an adjacent authority runtime. The fabric consumes policy
/// as typed values with an explicit generation; it never invents policy.
struct PolicyState {
  PolicyGeneration generation{};
  std::uint32_t max_replicas_per_dpu_ceiling{4};
  std::uint32_t max_instances_per_group{64};
  std::uint32_t max_stage_batch{16};
  bool allow_synthetic_evidence{true};
  bool require_exclusive_scope_for_dedicated_device{true};
  bool allow_rollback_across_policy{false};
  std::uint64_t evidence_validity_ticks{64};
  Digest policy_digest{};
  EvidenceRef evidence{};

  [[nodiscard]] bool valid() const noexcept { return generation.valid(); }

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("PolicyState");
    field(ar, "generation", generation);
    field(ar, "max_replicas_per_dpu_ceiling", max_replicas_per_dpu_ceiling);
    field(ar, "max_instances_per_group", max_instances_per_group);
    field(ar, "max_stage_batch", max_stage_batch);
    field(ar, "allow_synthetic_evidence", allow_synthetic_evidence);
    field(ar, "require_exclusive_scope_for_dedicated_device", require_exclusive_scope_for_dedicated_device);
    field(ar, "allow_rollback_across_policy", allow_rollback_across_policy);
    field(ar, "evidence_validity_ticks", evidence_validity_ticks);
    field(ar, "policy_digest", policy_digest);
    field(ar, "evidence", evidence);
    ar.end_object();
  }
};

}  // namespace dpu::fabric
