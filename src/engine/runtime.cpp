#include "dpu/fabric/engine/runtime.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace dpu::fabric {
namespace {

/// One remembered delivery, used to make duplicate delivery idempotent.
struct DeliveryRecord {
  OriginId origin{};
  OriginEpoch origin_epoch{};
  Sequence origin_seq{};
  Digest digest{};

  friend bool operator==(const DeliveryRecord&, const DeliveryRecord&) = default;
  friend bool operator<(const DeliveryRecord& lhs, const DeliveryRecord& rhs) {
    if (!(lhs.origin == rhs.origin)) return lhs.origin < rhs.origin;
    if (!(lhs.origin_epoch == rhs.origin_epoch)) return lhs.origin_epoch < rhs.origin_epoch;
    return lhs.origin_seq < rhs.origin_seq;
  }
};

Explanation explanation(ReasonCode code, DecisionKind kind, std::string subject, std::string note) {
  Explanation out;
  out.code = code;
  out.kind = kind;
  out.subject = std::move(subject);
  out.note = std::move(note);
  return out;
}

bool instance_id_less(const InstanceState& lhs, const InstanceState& rhs) { return lhs.id < rhs.id; }

const InstanceState* find_instance(const std::vector<InstanceState>& instances,
                                   const InstanceId& id) {
  const auto it = std::lower_bound(instances.begin(), instances.end(), id,
                                   [](const InstanceState& entry, const InstanceId& probe) {
                                     return entry.id < probe;
                                   });
  if (it == instances.end() || !(it->id == id)) return nullptr;
  return &*it;
}

InstanceState* find_instance(std::vector<InstanceState>& instances, const InstanceId& id) {
  const auto it = std::lower_bound(instances.begin(), instances.end(), id,
                                   [](const InstanceState& entry, const InstanceId& probe) {
                                     return entry.id < probe;
                                   });
  if (it == instances.end() || !(it->id == id)) return nullptr;
  return &*it;
}

const ServiceDefinition* find_service(const std::vector<ServiceDefinition>& services,
                                      const ServiceId& id) {
  for (const ServiceDefinition& service : services) {
    if (service.id == id) return &service;
  }
  return nullptr;
}

const ServiceGroup* find_group(const std::vector<ServiceGroup>& groups, const ServiceGroupId& id) {
  for (const ServiceGroup& group : groups) {
    if (group.id == id) return &group;
  }
  return nullptr;
}

void sort_instances(std::vector<InstanceState>& instances) {
  std::sort(instances.begin(), instances.end(), instance_id_less);
}

/// Recomputes health freshness for every device against the logical clock.
/// Freshness is never inferred from wall time and never assumed.
void refresh_health(RuntimeState& state) {
  for (DpuRecord& dpu : state.topology.dpus) {
    if (dpu.state == DpuState::Lost || dpu.health == HealthState::Unknown) {
      dpu.health_fresh = false;
      continue;
    }
    const bool fresh = dpu.health_evidence.present() && dpu.health_evidence.fresh_at(state.clock) &&
                       dpu.health_evidence.observed_at >= dpu.health_observed_at;
    dpu.health_fresh = fresh;
    if (!fresh) {
      for (InstanceState& instance : state.instances) {
        if (instance.dpu == dpu.id) {
          instance.health_fresh = false;
        }
      }
    }
  }
  for (InstanceState& instance : state.instances) {
    const DpuRecord* dpu = state.topology.find(instance.dpu);
    if (dpu == nullptr || dpu->state != DpuState::Attached) {
      instance.health_fresh = false;
      continue;
    }
    instance.health = dpu->health;
    instance.health_fresh = dpu->health_fresh && dpu->health == HealthState::Healthy;
    if (instance.health_fresh) instance.health_observed_at = dpu->health_observed_at;
  }
}

void fence_instance(InstanceState& instance, LogicalInstant at, ReasonCode reason) {
  instance.effect_fence_serial = saturating_add(instance.effect_fence_serial, std::uint64_t{1});
  instance.effect_fenced_at = at;
  instance.fence_reason = reason;
  instance.health = HealthState::Unknown;
  instance.health_fresh = false;
  for (AttemptRecord& attempt : instance.attempts) {
    if (attempt.outcome == AttemptOutcome::Open) {
      attempt.outcome = AttemptOutcome::Cancelled;
      attempt.close_reason = reason;
      attempt.closed_at = at;
      attempt.phase = LifecycleState::Cancelled;
    }
  }
  if (instance.state == LifecycleState::Verified ||
      instance.state == LifecycleState::Acknowledged ||
      instance.state == LifecycleState::Deploying || instance.state == LifecycleState::Authorized ||
      instance.state == LifecycleState::Planned) {
    instance.state = LifecycleState::Unverified;
  }
  if (!instance.reasons.empty()) instance.reasons.clear();
  instance.reasons.push_back(explanation(reason, DecisionKind::Recovery, instance.id.str(),
                                         "previously proven effect is no longer authoritative"));
}

void cancel_open_attempts(InstanceState& instance, LogicalInstant at, ReasonCode reason,
                          LifecycleState next) {
  for (AttemptRecord& attempt : instance.attempts) {
    if (attempt.outcome == AttemptOutcome::Open) {
      attempt.outcome = AttemptOutcome::Cancelled;
      attempt.close_reason = reason;
      attempt.closed_at = at;
      attempt.phase = LifecycleState::Cancelled;
    }
  }
  instance.state = next;
}

}  // namespace

PlacementProjection placement_projection(const RuntimeState& state) {
  PlacementProjection projection;
  projection.store_id = state.store_id;
  projection.epoch = state.epoch;
  projection.clock = state.clock;
  projection.watermark = state.watermark;
  projection.topology_generation = state.topology_generation;
  projection.capability_generation = state.capability_generation;
  projection.policy_generation = state.policy_generation;
  projection.deployment_generation = state.deployment_generation;
  projection.policy = state.policy;
  projection.services = state.services;
  projection.groups = state.groups;
  projection.dependencies = state.dependencies;
  projection.plans = state.plans;
  return projection;
}

struct FabricRuntime::Impl {
  mutable std::mutex mutex{};
  RuntimeState state{};
  PolicyState policy{};
  std::vector<FabricEvent> pending{};
  BoundedHistory<DeliveryRecord> deliveries{};
  std::unique_ptr<Store> store{};
  bool closed{false};
  Digest state_digest{};
  Digest durable_digest{};
  std::vector<TruncationRecord> truncations{};
  std::uint64_t sequence{0};
};

namespace {

/// Result of applying one event.
struct ApplyResult {
  ReasonCode code{ReasonCode::Ok};
  bool applied{false};
  std::string detail{};
  std::vector<Explanation> explanations{};
  DecisionKind kind{DecisionKind::Ingest};
  bool accepted{false};
};

ApplyResult apply_event(RuntimeState& state, const FabricEvent& event, const RuntimeBounds& bounds);

// ---------------------------------------------------------------------------
// Reducers
// ---------------------------------------------------------------------------

ApplyResult reduce_time(RuntimeState& state, const LogicalTimeAdvanced& body) {
  ApplyResult result;
  result.kind = DecisionKind::Ingest;
  result.accepted = true;
  result.applied = true;
  result.code = ReasonCode::Ok;
  if (body.now < state.clock) {
    result.code = ReasonCode::StaleGeneration;
    result.accepted = false;
    result.applied = false;
    result.detail = "logical time cannot move backwards";
    result.explanations.push_back(explanation(ReasonCode::StaleGeneration, DecisionKind::Ingest,
                                              "<clock>", result.detail));
    return result;
  }
  state.clock = body.now;
  refresh_health(state);
  result.explanations.push_back(
      explanation(ReasonCode::Ok, DecisionKind::Ingest, "<clock>", "logical time advanced"));
  return result;
}

ApplyResult reduce_coordinator(RuntimeState& state, const FabricEvent& event,
                               const CoordinatorAdvanced& body) {
  ApplyResult result;
  result.kind = DecisionKind::Authority;
  if (body.epoch < state.epoch) {
    result.code = ReasonCode::EpochRegressed;
    result.detail = "coordinator epoch cannot regress";
    result.explanations.push_back(explanation(ReasonCode::EpochRegressed, DecisionKind::Authority,
                                              body.coordinator.str(), result.detail));
    return result;
  }
  const std::size_t released = state.authority.fence_all();
  const Status advanced = state.fences.advance_epoch(body.epoch, state.boot);
  if (!advanced.ok()) {
    result.code = advanced.code();
    result.detail = advanced.detail();
    return result;
  }
  state.epoch = body.epoch;
  state.coordinator = body.coordinator.valid() ? body.coordinator : state.coordinator;
  for (InstanceState& instance : state.instances) {
    fence_instance(instance, event.at, ReasonCode::StaleAuthority);
  }
  result.applied = true;
  result.accepted = true;
  result.code = ReasonCode::Ok;
  result.explanations.push_back(
      explanation(ReasonCode::Ok, DecisionKind::Authority, body.coordinator.str(),
                  "coordinator advanced to epoch " + std::to_string(body.epoch.value()) +
                      "; " + std::to_string(released) + " authority grants fenced"));
  return result;
}

ApplyResult reduce_service(RuntimeState& state, const ServiceDeclared& body,
                           const RuntimeBounds& bounds) {
  ApplyResult result;
  result.kind = DecisionKind::Lifecycle;
  const ServiceDefinition& service = body.service;
  if (!service.id.valid() || !service.version.valid()) {
    result.code = ReasonCode::InvalidIdentity;
    result.detail = "service identity or version missing";
    result.explanations.push_back(
        explanation(result.code, DecisionKind::Lifecycle, service.id.str(), result.detail));
    return result;
  }
  if (service.display_name.size() > bounds.max_text_bytes) {
    result.code = ReasonCode::OversizedInput;
    result.detail = "service display name exceeds bound";
    return result;
  }
  if (const Status ok = service.replicas.validate(bounds.max_replicas_per_service); !ok.ok()) {
    result.code = ok.code();
    result.detail = "invalid replica policy";
    result.explanations.push_back(
        explanation(result.code, DecisionKind::Lifecycle, service.id.str(), result.detail));
    return result;
  }
  if (service.compatibility.capabilities.size() > bounds.max_capabilities_per_dpu) {
    result.code = ReasonCode::BoundExceeded;
    result.detail = "capability requirement count exceeds bound";
    return result;
  }
  for (const CapabilityRequirement& requirement : service.compatibility.capabilities) {
    if (!requirement.key.valid()) {
      result.code = ReasonCode::InvalidIdentity;
      result.detail = "capability requirement without a key";
      return result;
    }
  }
  const ServiceDefinition* existing = find_service(state.services, service.id);
  if (existing != nullptr && service.version < existing->version) {
    result.code = ReasonCode::GenerationRegressed;
    result.detail = "service version cannot regress";
    result.explanations.push_back(
        explanation(result.code, DecisionKind::Lifecycle, service.id.str(), result.detail));
    return result;
  }
  if (existing == nullptr) {
    if (state.services.size() >= bounds.max_services) {
      result.code = ReasonCode::BoundExceeded;
      result.detail = "service table full";
      return result;
    }
    state.services.push_back(service);
  } else {
    for (ServiceDefinition& candidate : state.services) {
      if (candidate.id == service.id) candidate = service;
    }
  }
  std::sort(state.services.begin(), state.services.end(),
            [](const ServiceDefinition& lhs, const ServiceDefinition& rhs) { return lhs.id < rhs.id; });
  result.applied = true;
  result.accepted = true;
  result.explanations.push_back(explanation(ReasonCode::Ok, DecisionKind::Lifecycle, service.id.str(),
                                            "service declared at version " +
                                                service.version.to_string()));
  return result;
}

ApplyResult reduce_group(RuntimeState& state, const GroupDeclared& body, const RuntimeBounds& bounds) {
  ApplyResult result;
  result.kind = DecisionKind::Lifecycle;
  const ServiceGroup& group = body.group;
  if (!group.id.valid()) {
    result.code = ReasonCode::InvalidIdentity;
    result.detail = "group identity missing";
    return result;
  }
  if (group.members.size() > bounds.max_services) {
    result.code = ReasonCode::BoundExceeded;
    result.detail = "group membership exceeds bound";
    return result;
  }
  if (const Status ok = group.anti_affinity.validate(); !ok.ok()) {
    result.code = ok.code();
    result.detail = "invalid anti-affinity policy";
    result.explanations.push_back(
        explanation(result.code, DecisionKind::Lifecycle, group.id.str(), result.detail));
    return result;
  }
  for (const ServiceId& member : group.members) {
    if (find_service(state.services, member) == nullptr) {
      result.code = ReasonCode::ServiceNotDeclared;
      result.detail = "group member " + member.str() + " is not declared";
      result.explanations.push_back(
          explanation(result.code, DecisionKind::Lifecycle, group.id.str(), result.detail));
      return result;
    }
  }
  const ServiceGroup* existing = find_group(state.groups, group.id);
  if (existing == nullptr) {
    if (state.groups.size() >= bounds.max_services) {
      result.code = ReasonCode::BoundExceeded;
      result.detail = "group table full";
      return result;
    }
    state.groups.push_back(group);
  } else {
    for (ServiceGroup& candidate : state.groups) {
      if (candidate.id == group.id) candidate = group;
    }
  }
  std::sort(state.groups.begin(), state.groups.end(),
            [](const ServiceGroup& lhs, const ServiceGroup& rhs) { return lhs.id < rhs.id; });
  result.applied = true;
  result.accepted = true;
  result.explanations.push_back(
      explanation(ReasonCode::Ok, DecisionKind::Lifecycle, group.id.str(), "group declared"));
  return result;
}

ApplyResult reduce_dependency(RuntimeState& state, const DependencyDeclared& body,
                              const RuntimeBounds& bounds) {
  ApplyResult result;
  result.kind = DecisionKind::Dependency;
  std::vector<Dependency> edges = state.dependencies;
  edges.push_back(body.dependency);
  DependencyGraph graph;
  const Status built = graph.build(std::move(edges), bounds.max_dependencies);
  if (!built.ok()) {
    result.code = built.code();
    result.detail = built.detail();
    result.explanations.push_back(explanation(result.code, DecisionKind::Dependency,
                                              body.dependency.id.str(), result.detail));
    return result;
  }
  if (find_service(state.services, body.dependency.from) == nullptr ||
      find_service(state.services, body.dependency.to) == nullptr) {
    result.code = ReasonCode::ServiceNotDeclared;
    result.detail = "dependency endpoint is not declared";
    result.explanations.push_back(explanation(result.code, DecisionKind::Dependency,
                                              body.dependency.id.str(), result.detail));
    return result;
  }
  state.dependencies = graph.edges();
  result.applied = true;
  result.accepted = true;
  result.explanations.push_back(explanation(ReasonCode::Ok, DecisionKind::Dependency,
                                            body.dependency.id.str(), "dependency declared"));
  return result;
}

}  // namespace

namespace {

ApplyResult reduce_policy(RuntimeState& state, const FabricEvent& event, const PolicyAdvanced& body) {
  ApplyResult result;
  result.kind = DecisionKind::Eligibility;
  const PolicyState& policy = body.policy;
  if (!policy.generation.valid()) {
    result.code = ReasonCode::InvalidIdentity;
    result.detail = "policy generation missing";
    result.explanations.push_back(
        explanation(result.code, DecisionKind::Eligibility, "<policy>", result.detail));
    return result;
  }
  if (policy.generation < state.policy_generation) {
    result.code = ReasonCode::StaleGeneration;
    result.detail = "policy generation regressed";
    result.explanations.push_back(
        explanation(result.code, DecisionKind::Eligibility, "<policy>", result.detail));
    return result;
  }
  if (!policy.evidence.present()) {
    result.code = ReasonCode::EvidenceMissing;
    result.detail = "policy carries no evidence reference";
    result.explanations.push_back(
        explanation(result.code, DecisionKind::Eligibility, "<policy>", result.detail));
    return result;
  }
  if (!policy.evidence.fresh_at(event.at)) {
    result.code = ReasonCode::EvidenceStale;
    result.detail = "policy evidence is stale at the event instant";
    result.explanations.push_back(
        explanation(result.code, DecisionKind::Eligibility, "<policy>", result.detail));
    return result;
  }
  if (policy.max_stage_batch == 0 || policy.max_instances_per_group == 0 ||
      policy.max_replicas_per_dpu_ceiling == 0 || policy.evidence_validity_ticks == 0) {
    result.code = ReasonCode::ValueOutOfRange;
    result.detail = "policy carries an impossible zero bound";
    result.explanations.push_back(
        explanation(result.code, DecisionKind::Eligibility, "<policy>", result.detail));
    return result;
  }
  state.policy = policy;
  state.policy_generation = policy.generation;
  result.applied = true;
  result.accepted = true;
  result.explanations.push_back(
      explanation(ReasonCode::Ok, DecisionKind::Eligibility, "<policy>",
                  "policy generation " + std::to_string(policy.generation.value()) + " accepted"));
  return result;
}

ApplyResult reduce_topology(RuntimeState& state, const FabricEvent& event,
                            const TopologyObserved& body, const RuntimeBounds& bounds) {
  ApplyResult result;
  result.kind = DecisionKind::Eligibility;
  const TopologySnapshot& snapshot = body.snapshot;
  if (!snapshot.generation.valid()) {
    result.code = ReasonCode::InvalidIdentity;
    result.detail = "topology generation missing";
    return result;
  }
  if (snapshot.generation < state.topology_generation) {
    result.code = ReasonCode::StaleGeneration;
    result.detail = "topology generation regressed";
    result.explanations.push_back(explanation(result.code, DecisionKind::Eligibility, "<topology>",
                                              result.detail));
    return result;
  }
  if (snapshot.dpus.size() > bounds.max_dpus) {
    result.code = ReasonCode::BoundExceeded;
    result.detail = "topology exceeds the DPU bound";
    return result;
  }
  TopologySnapshot candidate = snapshot;
  std::sort(candidate.dpus.begin(), candidate.dpus.end(),
            [](const DpuRecord& lhs, const DpuRecord& rhs) { return lhs.id < rhs.id; });
  for (std::size_t i = 0; i < candidate.dpus.size(); ++i) {
    DpuRecord& dpu = candidate.dpus[i];
    if (!dpu.id.valid() || !dpu.attachment.valid() || !dpu.domain.valid()) {
      result.code = ReasonCode::InvalidIdentity;
      result.detail = "device record is missing an identity";
      result.explanations.push_back(
          explanation(result.code, DecisionKind::Eligibility, dpu.id.str(), result.detail));
      return result;
    }
    if (i > 0 && candidate.dpus[i - 1].id == dpu.id) {
      result.code = ReasonCode::DuplicateIdentity;
      result.detail = "duplicate device " + dpu.id.str();
      result.explanations.push_back(
          explanation(result.code, DecisionKind::Eligibility, dpu.id.str(), result.detail));
      return result;
    }
    if (const Status ok = dpu.capabilities.normalize(bounds.max_capabilities_per_dpu); !ok.ok()) {
      result.code = ok.code();
      result.detail = "invalid capability set for " + dpu.id.str();
      result.explanations.push_back(
          explanation(result.code, DecisionKind::Eligibility, dpu.id.str(), result.detail));
      return result;
    }
    if (!dpu.capability_generation.valid()) dpu.capability_generation = state.capability_generation;
    dpu.health_fresh = dpu.health_evidence.present() &&
                       dpu.health_evidence.fresh_at(event.at) &&
                       dpu.health_evidence.observed_at >= dpu.health_observed_at;
  }

  // Devices absent from the new topology cannot be presumed healthy: they are
  // detached, and every proven effect on them is fenced.
  std::vector<DpuId> removed;
  for (const DpuRecord& previous : state.topology.dpus) {
    if (candidate.find(previous.id) == nullptr) removed.push_back(previous.id);
  }
  state.topology = std::move(candidate);
  state.topology_generation = snapshot.generation;
  for (const DpuId& id : removed) {
    for (InstanceState& instance : state.instances) {
      if (instance.dpu == id) {
        fence_instance(instance, event.at, ReasonCode::DpuLost);
        instance.state = LifecycleState::Lost;
      }
    }
    std::vector<ExclusiveScopeId> scopes;
    for (const AuthorityGrant& grant : state.authority.grants()) {
      if (grant.dpu == id) scopes.push_back(grant.scope);
    }
    for (const ExclusiveScopeId& scope : scopes) state.authority.release(scope, id);
  }
  CapabilityGeneration highest = state.capability_generation;
  for (const DpuRecord& dpu : state.topology.dpus) {
    if (dpu.capability_generation > highest) highest = dpu.capability_generation;
  }
  state.capability_generation = highest;
  refresh_health(state);
  result.applied = true;
  result.accepted = true;
  result.explanations.push_back(explanation(
      ReasonCode::Ok, DecisionKind::Eligibility, "<topology>",
      "accepted topology generation " + std::to_string(snapshot.generation.value()) + " with " +
          std::to_string(state.topology.dpus.size()) + " devices"));
  return result;
}

ApplyResult reduce_capability(RuntimeState& state, const FabricEvent& event,
                              const CapabilityObserved& body, const RuntimeBounds& bounds) {
  ApplyResult result;
  result.kind = DecisionKind::Eligibility;
  DpuRecord* dpu = state.topology.find(body.dpu);
  if (dpu == nullptr) {
    result.code = ReasonCode::DpuUnavailable;
    result.detail = "capability evidence for an unknown device";
    result.explanations.push_back(
        explanation(result.code, DecisionKind::Eligibility, body.dpu.str(), result.detail));
    return result;
  }
  if (body.generation < dpu->capability_generation) {
    result.code = ReasonCode::StaleGeneration;
    result.detail = "capability generation regressed";
    result.explanations.push_back(
        explanation(result.code, DecisionKind::Eligibility, body.dpu.str(), result.detail));
    return result;
  }
  CapabilitySet candidate = body.capabilities;
  if (const Status ok = candidate.normalize(bounds.max_capabilities_per_dpu); !ok.ok()) {
    result.code = ok.code();
    result.detail = "invalid capability set";
    result.explanations.push_back(
        explanation(result.code, DecisionKind::Eligibility, body.dpu.str(), result.detail));
    return result;
  }
  if (!body.evidence.fresh_at(event.at)) {
    result.code = ReasonCode::EvidenceStale;
    result.detail = "capability evidence is stale at the event instant";
    result.explanations.push_back(
        explanation(result.code, DecisionKind::Eligibility, body.dpu.str(), result.detail));
    return result;
  }
  if (body.evidence.observed_at < dpu->health_observed_at &&
      dpu->state == DpuState::Lost) {
    result.code = ReasonCode::EvidenceStale;
    result.detail = "capability evidence predates device loss";
    result.explanations.push_back(
        explanation(result.code, DecisionKind::Eligibility, body.dpu.str(), result.detail));
    return result;
  }
  dpu->capabilities = std::move(candidate);
  dpu->capability_generation = body.generation;
  dpu->device_evidence = body.evidence;
  if (body.topology.valid() && body.topology > dpu->topology_generation) {
    dpu->topology_generation = body.topology;
  }
  if (body.generation > state.capability_generation) state.capability_generation = body.generation;
  result.applied = true;
  result.accepted = true;
  result.explanations.push_back(
      explanation(ReasonCode::Ok, DecisionKind::Eligibility, body.dpu.str(),
                  "capability generation " + std::to_string(body.generation.value()) + " accepted"));
  return result;
}

ApplyResult reduce_health(RuntimeState& state, const FabricEvent& event, const HealthObserved& body) {
  ApplyResult result;
  result.kind = DecisionKind::Eligibility;
  DpuRecord* dpu = state.topology.find(body.dpu);
  if (dpu == nullptr) {
    result.code = ReasonCode::DpuUnavailable;
    result.detail = "health evidence for an unknown device";
    result.explanations.push_back(
        explanation(result.code, DecisionKind::Eligibility, body.dpu.str(), result.detail));
    return result;
  }
  if (!body.evidence.present()) {
    result.code = ReasonCode::EvidenceMissing;
    result.detail = "health evidence has no reference";
    result.explanations.push_back(
        explanation(result.code, DecisionKind::Eligibility, body.dpu.str(), result.detail));
    return result;
  }
  if (body.evidence.observed_at < dpu->health_observed_at) {
    // Evidence observed before the last health invalidation cannot restore a
    // health claim, however fresh it claims to be.
    result.code = ReasonCode::EvidenceStale;
    result.detail = "health evidence predates the last invalidation";
    result.explanations.push_back(
        explanation(result.code, DecisionKind::Eligibility, body.dpu.str(), result.detail));
    return result;
  }
  if (!body.evidence.fresh_at(event.at)) {
    result.code = ReasonCode::EvidenceStale;
    result.detail = "health evidence is stale at the event instant";
    result.explanations.push_back(
        explanation(result.code, DecisionKind::Eligibility, body.dpu.str(), result.detail));
    return result;
  }
  dpu->health = body.health;
  dpu->health_evidence = body.evidence;
  dpu->health_observed_at = body.evidence.observed_at;
  dpu->health_fresh = body.evidence.fresh_at(state.clock) || event.at >= state.clock;
  if (dpu->state == DpuState::Lost) {
    // Health alone does not reattach a device; the attachment event does.
    dpu->health_fresh = false;
  }
  refresh_health(state);
  result.applied = true;
  result.accepted = true;
  result.explanations.push_back(explanation(ReasonCode::Ok, DecisionKind::Eligibility,
                                            body.dpu.str(),
                                            std::string{"health "} + std::string{to_string(body.health)} +
                                                " recorded with fresh evidence"));
  return result;
}

ApplyResult reduce_attached(RuntimeState& state, const FabricEvent& event, const DpuAttached& body) {
  ApplyResult result;
  result.kind = DecisionKind::Eligibility;
  DpuRecord* dpu = state.topology.find(body.dpu);
  if (dpu == nullptr) {
    result.code = ReasonCode::DpuUnavailable;
    result.detail = "attachment for an unknown device";
    result.explanations.push_back(
        explanation(result.code, DecisionKind::Eligibility, body.dpu.str(), result.detail));
    return result;
  }
  if (!body.evidence.present() || !body.evidence.fresh_at(event.at)) {
    result.code = body.evidence.present() ? ReasonCode::EvidenceStale : ReasonCode::EvidenceMissing;
    result.detail = "attachment requires fresh evidence";
    result.explanations.push_back(
        explanation(result.code, DecisionKind::Eligibility, body.dpu.str(), result.detail));
    return result;
  }
  if (body.evidence.observed_at < dpu->health_observed_at) {
    result.code = ReasonCode::EvidenceStale;
    result.detail = "attachment evidence predates the last invalidation";
    result.explanations.push_back(
        explanation(result.code, DecisionKind::Eligibility, body.dpu.str(), result.detail));
    return result;
  }
  if (body.topology.valid() && body.topology < state.topology_generation) {
    result.code = ReasonCode::StaleGeneration;
    result.detail = "attachment topology generation regressed";
    result.explanations.push_back(
        explanation(result.code, DecisionKind::Eligibility, body.dpu.str(), result.detail));
    return result;
  }
  dpu->state = DpuState::Attached;
  dpu->attachment = body.attachment.valid() ? body.attachment : dpu->attachment;
  dpu->domain = body.domain.valid() ? body.domain : dpu->domain;
  dpu->device_evidence = body.evidence;
  dpu->health = HealthState::Unknown;
  dpu->health_fresh = false;
  dpu->health_observed_at = body.evidence.observed_at;
  result.applied = true;
  result.accepted = true;
  result.explanations.push_back(explanation(ReasonCode::Ok, DecisionKind::Eligibility, body.dpu.str(),
                                            "device attached with fresh evidence"));
  return result;
}

ApplyResult reduce_lost(RuntimeState& state, const FabricEvent& event, const DpuLost& body) {
  ApplyResult result;
  result.kind = DecisionKind::Lifecycle;
  DpuRecord* dpu = state.topology.find(body.dpu);
  if (dpu == nullptr) {
    result.code = ReasonCode::DpuUnavailable;
    result.detail = "loss reported for an unknown device";
    result.explanations.push_back(
        explanation(result.code, DecisionKind::Lifecycle, body.dpu.str(), result.detail));
    return result;
  }
  if (dpu->state == DpuState::Lost) {
    result.code = ReasonCode::DuplicateSuppressed;
    result.detail = "device already reported lost";
    result.accepted = true;
    result.applied = false;
    return result;
  }
  dpu->state = DpuState::Lost;
  dpu->health = HealthState::Unknown;
  dpu->health_fresh = false;
  dpu->health_observed_at = event.at;
  std::size_t affected = 0;
  for (InstanceState& instance : state.instances) {
    if (instance.dpu != body.dpu) continue;
    fence_instance(instance, event.at, ReasonCode::DpuLost);
    instance.state = LifecycleState::Lost;
    ++affected;
  }
  std::vector<ExclusiveScopeId> scopes;
  for (const AuthorityGrant& grant : state.authority.grants()) {
    if (grant.dpu == body.dpu) scopes.push_back(grant.scope);
  }
  for (const ExclusiveScopeId& scope : scopes) state.authority.release(scope, body.dpu);
  result.applied = true;
  result.accepted = true;
  result.explanations.push_back(
      explanation(ReasonCode::DpuLost, DecisionKind::Lifecycle, body.dpu.str(),
                  "device lost; " + std::to_string(affected) +
                      " instances fenced and no continuity is claimed without fresh evidence"));
  return result;
}

ApplyResult reduce_quiesce(RuntimeState& state, const FabricEvent& event,
                           const QuiesceRequested& body) {
  ApplyResult result;
  result.kind = DecisionKind::Withdrawal;
  std::size_t affected = 0;
  for (InstanceState& instance : state.instances) {
    if (!(instance.service == body.service)) continue;
    if (instance.state == LifecycleState::Withdrawn) continue;
    cancel_open_attempts(instance, event.at, ReasonCode::QuiesceRequested,
                         LifecycleState::Quiescing);
    instance.health_fresh = false;
    ++affected;
  }
  result.applied = affected > 0;
  result.accepted = true;
  result.code = affected > 0 ? ReasonCode::Ok : ReasonCode::NoPlacementChange;
  result.explanations.push_back(
      explanation(ReasonCode::QuiesceRequested, DecisionKind::Withdrawal, body.service.str(),
                  std::to_string(affected) + " instances quiesced; withdrawal still requires a "
                  "verified effect"));
  return result;
}

ApplyResult reduce_acknowledged(RuntimeState& state, const ExecutionAcknowledged& body) {
  ApplyResult result;
  result.kind = DecisionKind::Lifecycle;
  InstanceState* instance = find_instance(state.instances, body.instance);
  if (instance == nullptr) {
    result.code = ReasonCode::InstanceNotFound;
    result.detail = "acknowledgement for an unknown instance";
    return result;
  }
  if (!state.fences.validate(body.instance, body.fence)) {
    result.code = ReasonCode::FenceMismatch;
    result.detail = "acknowledgement carries a stale fence";
    result.explanations.push_back(explanation(result.code, DecisionKind::Lifecycle,
                                              body.instance.str(), result.detail));
    return result;
  }
  const AttemptRecord* attempt = instance->current_attempt();
  if (attempt == nullptr || !(attempt->number == body.attempt) || !attempt->open()) {
    result.code = ReasonCode::AttemptSuperseded;
    result.detail = "acknowledgement targets a superseded attempt";
    result.explanations.push_back(explanation(result.code, DecisionKind::Lifecycle,
                                              body.instance.str(), result.detail));
    return result;
  }
  AttemptRecord* mutable_attempt = &instance->attempts.back();
  mutable_attempt->phase = LifecycleState::Acknowledged;
  instance->state = LifecycleState::Acknowledged;
  result.applied = true;
  result.accepted = true;
  result.code = ReasonCode::AcknowledgementOnly;
  result.explanations.push_back(
      explanation(ReasonCode::AcknowledgementOnly, DecisionKind::Lifecycle, body.instance.str(),
                  "acknowledged; an acknowledgement is not a verified effect"));
  return result;
}

/// Fences an instance without touching its lifecycle state. Used by verified
/// withdrawal, where the terminal state is the point of the operation.
void fence_instance_without_state(InstanceState& instance, LogicalInstant at, ReasonCode reason) {
  instance.effect_fence_serial = saturating_add(instance.effect_fence_serial, std::uint64_t{1});
  instance.effect_fenced_at = at;
  instance.fence_reason = reason;
}

ApplyResult reduce_effect(RuntimeState& state, const FabricEvent& event, const EffectReported& body) {
  ApplyResult result;
  result.kind = DecisionKind::Lifecycle;
  const EffectReport& report = body.report;
  InstanceState* instance = find_instance(state.instances, report.instance);
  if (instance == nullptr) {
    result.code = ReasonCode::InstanceNotFound;
    result.detail = "effect report for an unknown instance";
    result.explanations.push_back(explanation(result.code, DecisionKind::Lifecycle,
                                              report.instance.str(), result.detail));
    return result;
  }
  const AttemptRecord* attempt = instance->current_attempt();
  if (attempt == nullptr || !(attempt->number == report.attempt)) {
    result.code = ReasonCode::AttemptSuperseded;
    result.detail = "effect report targets a superseded attempt";
    result.explanations.push_back(explanation(result.code, DecisionKind::Lifecycle,
                                              report.instance.str(), result.detail));
    return result;
  }
  // Authority is checked before the attempt's own bookkeeping: a report that does
  // not hold current authority is refused for that reason, whatever else may also
  // be wrong with it.
  if (!state.fences.validate(report.instance, report.fence)) {
    result.code = ReasonCode::FenceMismatch;
    result.detail = "effect report carries a stale fence";
    result.explanations.push_back(explanation(result.code, DecisionKind::Lifecycle,
                                              report.instance.str(), result.detail));
    return result;
  }
  if (!attempt->open()) {
    result.code = ReasonCode::AttemptNotOpen;
    result.detail = "effect report targets a closed attempt";
    result.explanations.push_back(explanation(result.code, DecisionKind::Lifecycle,
                                              report.instance.str(), result.detail));
    return result;
  }
  if (!report.evidence.present()) {
    result.code = ReasonCode::EvidenceMissing;
    result.detail = "effect report carries no evidence";
    result.explanations.push_back(explanation(result.code, DecisionKind::Lifecycle,
                                              report.instance.str(), result.detail));
    return result;
  }
  if (report.evidence.observed_at < instance->effect_fenced_at) {
    // The effect was observed before the last fencing instant, so it cannot
    // justify the current state.
    result.code = ReasonCode::EvidenceStale;
    result.detail = "effect evidence predates the instance fencing instant";
    result.explanations.push_back(explanation(result.code, DecisionKind::Lifecycle,
                                              report.instance.str(), result.detail));
    return result;
  }
  if (!report.evidence.fresh_at(event.at)) {
    result.code = ReasonCode::EvidenceStale;
    result.detail = "effect evidence is stale at the report instant";
    result.explanations.push_back(explanation(result.code, DecisionKind::Lifecycle,
                                              report.instance.str(), result.detail));
    return result;
  }
  if (report.partial || report.status == EffectStatus::Partial) {
    result.code = ReasonCode::EffectPartial;
    result.detail = "partial effect cannot be reported as success";
    result.explanations.push_back(explanation(result.code, DecisionKind::Lifecycle,
                                              report.instance.str(), result.detail));
    return result;
  }
  if (report.status == EffectStatus::Refused) {
    result.code = ReasonCode::EffectReportUnmatched;
    result.detail = "executor refused the effect";
    result.explanations.push_back(explanation(result.code, DecisionKind::Lifecycle,
                                              report.instance.str(), result.detail));
    return result;
  }
  if (report.status == EffectStatus::Failed) {
    AttemptRecord* mutable_attempt = &instance->attempts.back();
    mutable_attempt->outcome = AttemptOutcome::Failed;
    mutable_attempt->close_reason = ReasonCode::EffectFailed;
    mutable_attempt->closed_at = event.at;
    mutable_attempt->phase = LifecycleState::Failed;
    instance->state = LifecycleState::Failed;
    instance->health = HealthState::Unknown;
    instance->health_fresh = false;
    result.applied = true;
    result.accepted = true;
    result.code = ReasonCode::EffectFailed;
    result.explanations.push_back(explanation(ReasonCode::EffectFailed, DecisionKind::Lifecycle,
                                              report.instance.str(),
                                              "executor reported failure; state is Failed"));
    return result;
  }

  AttemptRecord* mutable_attempt = &instance->attempts.back();
  mutable_attempt->outcome = AttemptOutcome::Verified;
  mutable_attempt->close_reason = ReasonCode::Ok;
  mutable_attempt->closed_at = event.at;
  mutable_attempt->phase = LifecycleState::Verified;
  if (report.kind == EffectKind::Withdraw) {
    instance->state = LifecycleState::Withdrawn;
    instance->health = HealthState::Unknown;
    instance->health_fresh = false;
    fence_instance_without_state(*instance, event.at, ReasonCode::Withdrawn);
    result.code = ReasonCode::Withdrawn;
    std::vector<ExclusiveScopeId> scopes;
    for (const AuthorityGrant& grant : state.authority.grants()) {
      if (grant.dpu == instance->dpu) scopes.push_back(grant.scope);
    }
    for (const ExclusiveScopeId& scope : scopes) state.authority.release(scope, instance->dpu);
    result.applied = true;
    result.accepted = true;
    result.explanations.push_back(explanation(ReasonCode::Withdrawn, DecisionKind::Withdrawal,
                                              report.instance.str(),
                                              "withdrawal verified; authority released"));
    return result;
  }
  instance->state = LifecycleState::Verified;
  instance->verified_digest = report.effect_digest;
  instance->verified_at = event.at;
  instance->verified_evidence = report.evidence;
  result.applied = true;
  result.accepted = true;
  result.explanations.push_back(explanation(ReasonCode::Ok, DecisionKind::Lifecycle,
                                            report.instance.str(),
                                            "effect verified against attempt " +
                                                std::to_string(report.attempt.value())));
  return result;
}

}  // namespace

namespace {

struct ReconcileStats {
  std::size_t created{0};
  std::size_t replaced{0};
  std::size_t unchanged{0};
  std::size_t attempts{0};
  std::size_t quiesced{0};
};

/// Brings instances in line with an accepted plan. Every mutation happens in the
/// caller's trial copy, so a refusal leaves no partial placement behind.
Status reconcile(RuntimeState& trial, const DeploymentPlan& plan, const ServiceGroup& group,
                 const FabricEvent& event, const RuntimeBounds& bounds, ReconcileStats& stats,
                 std::vector<Explanation>& explanations) {
  std::map<std::string, const PlannedReplica*> planned;
  std::map<std::string, const ServicePlan*> planned_service;
  for (const ServicePlan& service_plan : plan.services) {
    planned_service.emplace(service_plan.service.str(), &service_plan);
    for (const PlannedReplica& replica : service_plan.replicas) {
      planned.emplace(make_instance_id(group.id, service_plan.service, replica.index).str(),
                      &replica);
    }
  }

  for (const ServicePlan& service_plan : plan.services) {
    const ServiceDefinition* definition = find_service(trial.services, service_plan.service);
    const ServiceVersion& version = service_plan.version;
    for (const PlannedReplica& replica : service_plan.replicas) {
      const InstanceId id = make_instance_id(group.id, service_plan.service, replica.index);
      InstanceState* instance = find_instance(trial.instances, id);
      if (instance == nullptr) {
        if (trial.instances.size() >= bounds.max_instances) {
          return refuse(ReasonCode::BoundExceeded, "instance table full");
        }
        InstanceState created;
        created.id = id;
        created.service = service_plan.service;
        created.group = group.id;
        created.version = version;
        created.index = replica.index;
        created.incarnation = Incarnation{1};
        created.dpu = replica.dpu;
        created.attachment = replica.attachment;
        created.domain = replica.domain;
        created.deployment_generation = plan.generation;
        created.state = LifecycleState::Authorized;
        created.health = HealthState::Unknown;
        created.health_fresh = false;
        created.topology_generation = plan.topology;
        created.capability_generation = plan.capability;
        created.effect_fenced_at = event.at;
        created.fence_reason = ReasonCode::Ok;
        AttemptRecord attempt;
        attempt.number = AttemptNumber{1};
        attempt.phase = LifecycleState::Authorized;
        attempt.outcome = AttemptOutcome::Open;
        attempt.opened_at = event.at;
        attempt.request_digest = plan.digest;
        const Result<FenceToken> fence = trial.fences.issue(id, event.at);
        if (!fence) return fence.status();
        attempt.fence = fence.value();
        created.attempts.push_back(std::move(attempt));
        created.reasons.push_back(explanation(ReasonCode::Ok, DecisionKind::Placement, id.str(),
                                              "instance authorized at generation " +
                                                  std::to_string(plan.generation.value())));
        trial.instances.push_back(std::move(created));
        stats.created += 1;
        (void)definition;
        continue;
      }

      const bool placement_changed = !(instance->dpu == replica.dpu);
      const bool version_changed = !(instance->version == version);
      const bool terminal = instance->state == LifecycleState::Withdrawn ||
                            instance->state == LifecycleState::Failed ||
                            instance->state == LifecycleState::Lost;
      const bool stale_attempt = instance->current_attempt() == nullptr ||
                                 !instance->current_attempt()->open();
      if (placement_changed || version_changed || terminal) {
        instance->incarnation = instance->incarnation.saturating_next();
        instance->dpu = replica.dpu;
        instance->attachment = replica.attachment;
        instance->domain = replica.domain;
        instance->version = version;
        instance->deployment_generation = plan.generation;
        instance->topology_generation = plan.topology;
        instance->capability_generation = plan.capability;
        fence_instance(*instance, event.at,
                       terminal ? ReasonCode::Superseded : ReasonCode::AttemptSuperseded);
        AttemptRecord attempt;
        attempt.number = instance->attempts.empty()
                             ? AttemptNumber{1}
                             : instance->attempts.back().number.saturating_next();
        attempt.phase = LifecycleState::Authorized;
        attempt.outcome = AttemptOutcome::Open;
        attempt.opened_at = event.at;
        attempt.request_digest = plan.digest;
        const Result<FenceToken> fence = trial.fences.issue(id, event.at);
        if (!fence) return fence.status();
        attempt.fence = fence.value();
        instance->attempts.push_back(std::move(attempt));
        instance->state = LifecycleState::Authorized;
        stats.replaced += 1;
        continue;
      }
      if (stale_attempt) {
        // The instance holds no open attempt (recovered, cancelled or closed):
        // reissue authority rather than assuming the previous attempt still
        // stands.
        AttemptRecord attempt;
        attempt.number = instance->attempts.empty()
                             ? AttemptNumber{1}
                             : instance->attempts.back().number.saturating_next();
        attempt.phase = LifecycleState::Authorized;
        attempt.outcome = AttemptOutcome::Open;
        attempt.opened_at = event.at;
        attempt.request_digest = plan.digest;
        const Result<FenceToken> fence = trial.fences.issue(id, event.at);
        if (!fence) return fence.status();
        attempt.fence = fence.value();
        instance->attempts.push_back(std::move(attempt));
        instance->state = LifecycleState::Authorized;
        instance->deployment_generation = plan.generation;
        stats.attempts += 1;
        continue;
      }
      instance->deployment_generation = plan.generation;
      stats.unchanged += 1;
    }
  }

  // Instances that the plan no longer covers are quiesced. Withdrawal is only
  // complete once an executor reports a verified withdrawal effect, so a
  // withdrawal plan opens a dedicated attempt for it: cancelling the deployment
  // attempt would leave nothing that could ever prove the withdrawal.
  const bool withdrawal = plan.kind == IntentKind::Withdraw;
  for (InstanceState& instance : trial.instances) {
    if (!(instance.group == group.id)) continue;
    const bool uncovered = planned_service.count(instance.service.str()) == 0 ||
                           planned.count(instance.id.str()) == 0;
    if (!uncovered && !withdrawal) continue;
    if (instance.state == LifecycleState::Withdrawn) continue;
    cancel_open_attempts(instance, event.at, ReasonCode::QuiesceRequested,
                         LifecycleState::Quiescing);
    instance.health_fresh = false;
    stats.quiesced += 1;
    if (!withdrawal) continue;
    AttemptRecord attempt;
    attempt.number = instance.attempts.empty() ? AttemptNumber{1}
                                               : instance.attempts.back().number.saturating_next();
    attempt.phase = LifecycleState::Quiescing;
    attempt.outcome = AttemptOutcome::Open;
    attempt.opened_at = event.at;
    attempt.request_digest = plan.digest;
    const Result<FenceToken> fence = trial.fences.issue(instance.id, event.at);
    if (!fence) return fence.status();
    attempt.fence = fence.value();
    instance.attempts.push_back(std::move(attempt));
  }

  explanations.push_back(explanation(
      ReasonCode::Ok, DecisionKind::Placement, group.id.str(),
      "instances created=" + std::to_string(stats.created) + " replaced=" +
          std::to_string(stats.replaced) + " reattempted=" + std::to_string(stats.attempts) +
          " unchanged=" + std::to_string(stats.unchanged) + " quiesced=" +
          std::to_string(stats.quiesced)));
  return Status::success();
}

ApplyResult reduce_intent(RuntimeState& state, const FabricEvent& event, const IntentSubmitted& body,
                          const RuntimeBounds& bounds, const PlacementWeights& weights) {
  ApplyResult result;
  result.kind = DecisionKind::Placement;
  const PlacementIntent& intent = body.intent;
  const ServiceGroup* group = find_group(state.groups, intent.group);
  if (group == nullptr) {
    result.code = ReasonCode::GroupNotDeclared;
    result.detail = "intent names an undeclared group";
    result.explanations.push_back(explanation(result.code, DecisionKind::Placement,
                                              intent.group.str(), result.detail));
    return result;
  }
  if (!intent.generation.valid()) {
    result.code = ReasonCode::InvalidIdentity;
    result.detail = "intent generation missing";
    return result;
  }
  if (!state.topology_generation.valid()) {
    // No accepted topology means no device statement can be made. Planning is
    // refused rather than attempted against an empty world.
    result.code = ReasonCode::EvidenceMissing;
    result.detail = "no topology generation has been accepted";
    result.explanations.push_back(explanation(result.code, DecisionKind::Placement, intent.id.str(),
                                              result.detail));
    return result;
  }
  if (!state.policy.generation.valid()) {
    result.code = ReasonCode::EvidenceMissing;
    result.detail = "no policy generation has been accepted";
    result.explanations.push_back(explanation(result.code, DecisionKind::Placement, intent.id.str(),
                                              result.detail));
    return result;
  }
  if (intent.generation < state.deployment_generation) {
    result.code = ReasonCode::PlanGenerationRegressed;
    result.detail = "deployment generation regressed";
    result.explanations.push_back(explanation(result.code, DecisionKind::Placement, intent.id.str(),
                                              result.detail));
    return result;
  }
  if (intent.generation == state.deployment_generation && intent.kind != IntentKind::Withdraw) {
    result.code = ReasonCode::NoPlacementChange;
    result.detail = "deployment generation already accepted";
    result.explanations.push_back(explanation(result.code, DecisionKind::Placement, intent.id.str(),
                                              result.detail));
    return result;
  }

  RuntimeState trial = state;
  DeploymentPlan plan;

  if (intent.kind == IntentKind::Withdraw) {
    // Withdrawal needs no eligibility at all: a service must be withdrawable
    // even when every device it ran on has disappeared.
    plan.generation = intent.generation;
    plan.group = group->id;
    plan.kind = IntentKind::Withdraw;
    plan.topology = state.topology_generation;
    plan.policy = state.policy_generation;
    plan.epoch = state.epoch;
    plan.boot = state.boot;
    plan.created_at = event.at;
    for (const ServiceReplicaIntent& entry : intent.services) {
      const ServiceDefinition* definition = find_service(state.services, entry.service);
      if (definition == nullptr) {
        result.code = ReasonCode::ServiceNotDeclared;
        result.detail = "withdrawal names an undeclared service";
        result.explanations.push_back(explanation(result.code, DecisionKind::Withdrawal,
                                                  entry.service.str(), result.detail));
        return result;
      }
      ServicePlan service_plan;
      service_plan.service = definition->id;
      service_plan.version = definition->version;
      service_plan.desired_replicas = 0;
      service_plan.placed_replicas = 0;
      plan.services.push_back(std::move(service_plan));
    }
    plan.outcome = ReasonCode::Ok;
    plan.seal();
  } else if (intent.kind == IntentKind::Rollback) {
    const DeploymentGeneration target = *intent.rollback_target;
    const DeploymentPlan* source = nullptr;
    for (const DeploymentPlan& candidate : state.plans) {
      if (candidate.generation == target) source = &candidate;
    }
    if (source == nullptr) {
      result.code = ReasonCode::RollbackUnavailable;
      result.detail = "rollback target generation is not retained";
      result.explanations.push_back(explanation(result.code, DecisionKind::Rollback, intent.id.str(),
                                                result.detail));
      return result;
    }
    if (!state.policy.allow_rollback_across_policy &&
        !(source->policy == state.policy_generation)) {
      result.code = ReasonCode::RollbackUnavailable;
      result.detail = "rollback target was planned under a different policy generation";
      result.explanations.push_back(explanation(result.code, DecisionKind::Rollback, intent.id.str(),
                                                result.detail));
      return result;
    }
    plan = *source;
    plan.generation = intent.generation;
    plan.kind = IntentKind::Rollback;
    plan.rollback_target = target;
    plan.created_at = event.at;
    plan.epoch = state.epoch;
    plan.boot = state.boot;
    plan.topology = state.topology_generation;
    plan.policy = state.policy_generation;
    plan.explanations.clear();
    plan.explanations.push_back(explanation(ReasonCode::Ok, DecisionKind::Rollback, intent.id.str(),
                                            "rollback to generation " +
                                                std::to_string(target.value())));
    plan.seal();
  } else {
    PlanRequest request;
    request.topology = &state.topology;
    request.policy = &state.policy;
    request.services = &state.services;
    DependencyGraph graph;
    const Status built = graph.build(state.dependencies, bounds.max_dependencies);
    if (!built.ok()) {
      result.code = built.code();
      result.detail = built.detail();
      return result;
    }
    request.dependencies = &graph;
    request.group = group;
    request.intent = &intent;
    request.authority = &state.authority;
    request.generation = intent.generation;
    request.now = event.at;
    request.epoch = state.epoch;
    request.boot = state.boot;
    request.bounds = &bounds;
    PlanOutcome outcome = build_plan(request, weights);
    if (!outcome.feasible) {
      result.code = outcome.primary;
      result.detail = "plan is infeasible";
      result.explanations = outcome.explanations;
      return result;
    }
    plan = outcome.plan;
  }

  // Authority is claimed in the trial registry so that a conflict refuses the
  // whole intent instead of half-claiming it.
  AuthorityRegistry trial_authority = trial.authority;
  for (const ServicePlan& service_plan : plan.services) {
    const ServiceDefinition* definition = find_service(trial.services, service_plan.service);
    if (definition == nullptr || !definition->isolation.exclusive_scope.has_value()) continue;
    for (const PlannedReplica& replica : service_plan.replicas) {
      const Status claimed = trial_authority.claim(
          *definition->isolation.exclusive_scope, group->id, replica.dpu, state.epoch, state.boot,
          event.at, bounds.max_dpus);
      if (!claimed.ok()) {
        result.code = claimed.code();
        result.detail = claimed.detail();
        result.explanations.push_back(explanation(result.code, DecisionKind::Authority,
                                                  replica.dpu.str(), result.detail));
        return result;
      }
    }
  }

  ReconcileStats stats;
  std::vector<Explanation> reconcile_explanations;
  // Fences live in the trial too, so a refused reconciliation does not consume
  // serials.
  const Status reconciled = reconcile(trial, plan, *group, event, bounds, stats,
                                      reconcile_explanations);
  if (!reconciled.ok()) {
    result.code = reconciled.code();
    result.detail = reconciled.detail();
    return result;
  }
  trial.authority = std::move(trial_authority);

  if (plan.kind != IntentKind::Withdraw) {
    trial.plans.push_back(plan);
    if (trial.plans.size() > bounds.max_plans_retained) {
      const std::size_t excess = trial.plans.size() - bounds.max_plans_retained;
      trial.plans.erase(trial.plans.begin(), trial.plans.begin() + static_cast<std::ptrdiff_t>(excess));
      trial.truncations.record("plan.history", accounting_u64(excess + bounds.max_plans_retained),
                               accounting_u64(bounds.max_plans_retained),
                               ReasonCode::HistoryTruncated);
    }
    trial.deployment_generation = plan.generation;
  } else {
    trial.deployment_generation = plan.generation;
  }
  sort_instances(trial.instances);

  state = std::move(trial);
  result.applied = true;
  result.accepted = true;
  result.code = ReasonCode::Ok;
  result.explanations = std::move(reconcile_explanations);
  result.explanations.push_back(
      explanation(ReasonCode::Ok, DecisionKind::Placement, plan.id.str(),
                  "plan accepted at generation " + std::to_string(plan.generation.value()) +
                      " with " + std::to_string(plan.stages.size()) + " stages"));
  return result;
}

ApplyResult apply_event(RuntimeState& state, const FabricEvent& event, const RuntimeBounds& bounds,
                        const PlacementWeights& weights) {
  return std::visit(
      [&](const auto& body) -> ApplyResult {
        using B = std::decay_t<decltype(body)>;
        if constexpr (std::is_same_v<B, LogicalTimeAdvanced>) {
          return reduce_time(state, body);
        } else if constexpr (std::is_same_v<B, CoordinatorAdvanced>) {
          return reduce_coordinator(state, event, body);
        } else if constexpr (std::is_same_v<B, ServiceDeclared>) {
          return reduce_service(state, body, bounds);
        } else if constexpr (std::is_same_v<B, GroupDeclared>) {
          return reduce_group(state, body, bounds);
        } else if constexpr (std::is_same_v<B, DependencyDeclared>) {
          return reduce_dependency(state, body, bounds);
        } else if constexpr (std::is_same_v<B, PolicyAdvanced>) {
          return reduce_policy(state, event, body);
        } else if constexpr (std::is_same_v<B, TopologyObserved>) {
          return reduce_topology(state, event, body, bounds);
        } else if constexpr (std::is_same_v<B, CapabilityObserved>) {
          return reduce_capability(state, event, body, bounds);
        } else if constexpr (std::is_same_v<B, HealthObserved>) {
          return reduce_health(state, event, body);
        } else if constexpr (std::is_same_v<B, DpuAttached>) {
          return reduce_attached(state, event, body);
        } else if constexpr (std::is_same_v<B, DpuLost>) {
          return reduce_lost(state, event, body);
        } else if constexpr (std::is_same_v<B, QuiesceRequested>) {
          return reduce_quiesce(state, event, body);
        } else if constexpr (std::is_same_v<B, ExecutionAcknowledged>) {
          return reduce_acknowledged(state, body);
        } else if constexpr (std::is_same_v<B, EffectReported>) {
          return reduce_effect(state, event, body);
        } else if constexpr (std::is_same_v<B, IntentSubmitted>) {
          return reduce_intent(state, event, body, bounds, weights);
        } else {
          ApplyResult unsupported;
          unsupported.code = ReasonCode::UnsupportedOperation;
          unsupported.detail = "unhandled event kind";
          return unsupported;
        }
      },
      event.body);
}

}  // namespace

namespace {

/// Applies one replayed event without journaling it again. Replay uses exactly
/// the same reducer as live traffic, which is what makes recovery reproducible.
Status replay_event(RuntimeState& state, const FabricEvent& event, const RuntimeBounds& bounds,
                    const PlacementWeights& weights, BoundedHistory<DeliveryRecord>& deliveries,
                    TruncationLedger& ledger) {
  const DeliveryRecord key{event.origin, event.origin_epoch, event.origin_seq, {}};
  for (const DeliveryRecord& seen : deliveries.entries()) {
    if (seen.origin == key.origin && seen.origin_epoch == key.origin_epoch &&
        seen.origin_seq == key.origin_seq) {
      return Status{ReasonCode::DuplicateSuppressed, "duplicate suppressed during replay"};
    }
  }
  const std::size_t evicted = deliveries.push(DeliveryRecord{
      event.origin, event.origin_epoch, event.origin_seq, event_digest(event)});
  if (evicted > 0) {
    (void)ledger.record("delivery.window", accounting_u64(evicted + deliveries.size()),
                        accounting_u64(deliveries.size()), ReasonCode::DuplicateWindowEvicted);
  }
  (void)apply_event(state, event, bounds, weights);
  if (event.at > state.watermark) state.watermark = event.at;
  return Status::success();
}

void fence_after_recovery(RuntimeState& state, std::vector<InstanceId>& fenced) {
  for (InstanceState& instance : state.instances) {
    const bool claimed_liveness = instance.state == LifecycleState::Verified ||
                                  instance.state == LifecycleState::Acknowledged ||
                                  instance.state == LifecycleState::Deploying ||
                                  instance.state == LifecycleState::Authorized ||
                                  instance.state == LifecycleState::Planned ||
                                  instance.state == LifecycleState::Unverified;
    fence_instance(instance, state.clock, ReasonCode::RestartFenced);
    if (claimed_liveness) fenced.push_back(instance.id);
  }
  state.restart_fenced = fenced;
}

}  // namespace

FabricRuntime::FabricRuntime(RuntimeConfig config)
    : config_(std::move(config)), impl_(std::make_unique<Impl>()) {}

FabricRuntime::~FabricRuntime() { (void)close(); }

Result<std::unique_ptr<FabricRuntime>> FabricRuntime::open(const RuntimeConfig& config) {
  const Status bounds_ok = config.bounds.validate();
  if (!bounds_ok.ok()) return bounds_ok;
  if (!config.store_id.valid()) {
    return refuse(ReasonCode::InvalidIdentity, "store identity is required");
  }
  if (!config.coordinator.valid()) {
    return refuse(ReasonCode::InvalidIdentity, "coordinator identity is required");
  }
  if (!config.epoch.valid()) {
    return refuse(ReasonCode::InvalidIdentity, "coordinator epoch is required");
  }
  if (!config.boot.valid()) {
    return refuse(ReasonCode::InvalidIdentity, "boot incarnation is required");
  }

  auto runtime = std::unique_ptr<FabricRuntime>(new FabricRuntime(config));
  Impl& impl = *runtime->impl_;
  impl.deliveries.set_capacity(config.bounds.max_duplicate_window);
  RuntimeState& state = impl.state;
  state.store_id = config.store_id;
  state.coordinator = config.coordinator;
  state.epoch = config.epoch;
  state.boot = config.boot;
  state.clock = LogicalInstant{0};
  state.watermark = LogicalInstant{0};

  // The fence issuer must carry a valid epoch and boot incarnation before any
  // event is applied, including during journal replay: an issuer without an
  // epoch refuses to issue, and a replayed intent would then be refused for a
  // reason that has nothing to do with the original decision.
  state.fences.reset(state.epoch, state.boot);

  if (!config.store_directory.has_value()) {
    runtime->recovery_.classification = RecoveryClass::Fresh;
    impl.state_digest = canonical_digest(state);
    impl.durable_digest = canonical_digest(placement_projection(state));
    return runtime;
  }

  StoreOptions options;
  options.directory = *config.store_directory;
  options.store_id = config.store_id;
  options.max_journal_bytes = config.bounds.max_journal_bytes;
  options.max_record_bytes = config.bounds.max_frame_bytes;
  options.fsync = config.fsync_journal;

  Result<std::unique_ptr<Store>> store = Store::open(options);
  if (!store) return store.status();
  impl.store = std::move(store).value();

  std::vector<std::uint8_t> snapshot_payload;
  std::vector<std::vector<std::uint8_t>> journal_records;
  Result<StoreRecovery> loaded = impl.store->load(snapshot_payload, journal_records);
  if (!loaded) {
    runtime->recovery_.classification = RecoveryClass::RefusedCorrupt;
    return loaded.status();
  }
  StoreRecovery recovery = loaded.value();
  runtime->recovery_.classification = recovery.classification;
  runtime->recovery_.torn_tail = recovery.classification == RecoveryClass::TornTailTruncated;
  runtime->recovery_.records_dropped = recovery.records_dropped;

  const bool recovered = !snapshot_payload.empty() || !journal_records.empty();

  if (!snapshot_payload.empty()) {
    Result<RuntimeState> decoded =
        decode_binary<RuntimeState>(snapshot_payload, config.bounds.max_instances + 1024);
    if (!decoded) {
      return refuse(ReasonCode::StoreCorrupt,
                    "snapshot payload could not be decoded: " + decoded.status().message());
    }
    RuntimeState restored = std::move(decoded).value();
    if (restored.store_id.valid() && !(restored.store_id == config.store_id)) {
      return refuse(ReasonCode::StoreSemanticsIncompatible, "store identity does not match");
    }
    runtime->recovery_.previous_epoch = restored.epoch;
    runtime->recovery_.previous_boot = restored.boot;
    const LogicalInstant watermark = restored.watermark;
    state = std::move(restored);
    state.watermark = watermark;
    state.fences.reset(state.epoch, state.boot);
    runtime->recovery_.recovered_from_store = true;
  }

  // The filter is the watermark the snapshot was taken at. Comparing against a
  // moving watermark would skip records that are genuinely new.
  const LogicalInstant snapshot_watermark = state.watermark;
  for (const std::vector<std::uint8_t>& record : journal_records) {
    Result<FabricEvent> event =
        decode_binary<FabricEvent>(record, config.bounds.max_instances + 1024);
    if (!event) {
      return refuse(ReasonCode::StoreCorrupt,
                    "journal record could not be decoded: " + event.status().message());
    }
    const FabricEvent& decoded_event = event.value();
    if (decoded_event.at <= snapshot_watermark) {
      // Already covered by the snapshot watermark; replaying it would apply an
      // event twice.
      continue;
    }
    const Status replayed = replay_event(state, decoded_event, config.bounds, config.weights,
                                         impl.deliveries, state.truncations);
    if (!replayed.ok()) {
      return refuse(replayed.code(), "journal replay refused an event");
    }
    runtime->recovery_.journal_records_replayed += 1;
  }

  if (recovered) {
    BootIncarnation boot = state.boot;
    if (config.boot > boot) boot = config.boot;
    const Result<BootIncarnation> advanced = boot.next();
    BootIncarnation candidate = advanced ? advanced.value() : boot.saturating_next();
    if (candidate < boot) candidate = boot;
    state.boot = candidate;
    if (config.epoch > state.epoch) state.epoch = config.epoch;
    // Recovery establishes the fence issuer's authority under the new boot
    // incarnation and drops every serial issued before it. Resetting (rather
    // than merely advancing) is what makes the epoch explicit: an issuer with no
    // epoch refuses to issue at all, and a runtime that cannot issue authority
    // can never place anything.
    state.fences.reset(state.epoch, state.boot);
    std::vector<InstanceId> fenced_instances;
    fence_after_recovery(state, fenced_instances);
    runtime->recovery_.restart_fenced = fenced_instances;
    runtime->recovery_.classification =
        recovery.classification == RecoveryClass::EmptyStore ? RecoveryClass::CleanReopen
                                                            : recovery.classification;
    // Establish a new durable base that already contains the conservative
    // downgrades, so a later replay starts from exactly this state.
    const Status checkpointed = runtime->checkpoint();
    if (!checkpointed.ok()) return checkpointed;
  } else {
    state.fences.reset(state.epoch, state.boot);
  }

  impl.state_digest = canonical_digest(state);
  impl.durable_digest = canonical_digest(placement_projection(state));
  return runtime;
}

Status FabricRuntime::stage(const FabricEvent& event) {
  std::lock_guard<std::mutex> guard{impl_->mutex};
  if (impl_->closed) return refuse(ReasonCode::StoreClosed, "runtime is closed");
  if (!event.id.valid() || !event.origin.valid() || !event.origin_epoch.valid()) {
    return refuse(ReasonCode::InvalidIdentity, "event identity is incomplete");
  }
  const Digest computed = event.compute_payload_digest();
  if (!event.payload_digest.is_zero() && !(event.payload_digest == computed)) {
    return refuse(ReasonCode::InvalidDigest, "event payload digest does not match its body");
  }
  for (const DeliveryRecord& seen : impl_->deliveries.entries()) {
    if (seen.origin == event.origin && seen.origin_epoch == event.origin_epoch &&
        seen.origin_seq == event.origin_seq) {
      if (seen.digest == event_digest(event)) {
        return Status{ReasonCode::DuplicateSuppressed, "duplicate delivery suppressed"};
      }
      return refuse(ReasonCode::ConflictingDuplicate,
                    "same origin sequence delivered with different content");
    }
  }
  if (event.at <= impl_->state.watermark) {
    // The reorder window has closed for this instant: the event is too old to be
    // ordered correctly, so it is refused rather than applied out of order.
    impl_->state.truncations.record("ingest.late", 1, 0, ReasonCode::LateEventRejected);
    return refuse(ReasonCode::LateEventRejected, "event is older than the released watermark");
  }
  if (impl_->pending.size() >= config_.bounds.max_batch_events) {
    impl_->state.truncations.record("ingest.pending",
                                    accounting_u64(impl_->pending.size() + 1),
                                    accounting_u64(impl_->pending.size()),
                                    ReasonCode::QueueFull);
    return refuse(ReasonCode::QueueFull, "reorder buffer is full");
  }
  impl_->pending.push_back(event);
  return Status::success();
}

Result<BatchReport> FabricRuntime::flush(LogicalInstant upto) {
  std::lock_guard<std::mutex> guard{impl_->mutex};
  BatchReport report;
  report.upto = upto;
  if (impl_->closed) return refuse(ReasonCode::StoreClosed, "runtime is closed");

  std::vector<FabricEvent> released;
  std::vector<FabricEvent> retained;
  for (const FabricEvent& event : impl_->pending) {
    if (event.at <= upto) {
      released.push_back(event);
    } else {
      retained.push_back(event);
    }
  }
  impl_->pending = std::move(retained);
  std::stable_sort(released.begin(), released.end(), [](const FabricEvent& lhs, const FabricEvent& rhs) {
    return event_key_less(event_key(lhs), event_key(rhs));
  });

  for (const FabricEvent& event : released) {
    EventOutcome outcome;
    outcome.id = event.id;
    const DeliveryRecord probe{event.origin, event.origin_epoch, event.origin_seq, {}};
    bool duplicate = false;
    for (const DeliveryRecord& seen : impl_->deliveries.entries()) {
      if (seen.origin == probe.origin && seen.origin_epoch == probe.origin_epoch &&
          seen.origin_seq == probe.origin_seq) {
        duplicate = true;
        if (seen.digest == event_digest(event)) {
          outcome.code = ReasonCode::DuplicateSuppressed;
          outcome.applied = false;
          outcome.detail = "duplicate delivery suppressed";
        } else {
          outcome.code = ReasonCode::ConflictingDuplicate;
          outcome.detail = "same origin sequence with different content";
        }
        break;
      }
    }
    if (duplicate) {
      if (is_informational(outcome.code)) {
        report.suppressed += 1;
      } else {
        report.refused += 1;
      }
      report.outcomes.push_back(std::move(outcome));
      continue;
    }

    if (impl_->store != nullptr) {
      Status appended = impl_->store->append_event(event);
      if (appended.code() == ReasonCode::BoundExceeded) {
        const Status rotated = checkpoint_locked();
        if (!rotated.ok()) {
          outcome.code = rotated.code();
          outcome.detail = rotated.detail();
          report.refused += 1;
          report.outcomes.push_back(std::move(outcome));
          continue;
        }
        appended = impl_->store->append_event(event);
      }
      if (!appended.ok()) {
        outcome.code = appended.code();
        outcome.detail = appended.detail();
        report.refused += 1;
        report.outcomes.push_back(std::move(outcome));
        continue;
      }
      impl_->state.journaled_events += 1;
    }

    const std::size_t evicted = impl_->deliveries.push(
        DeliveryRecord{event.origin, event.origin_epoch, event.origin_seq, event_digest(event)});
    if (evicted > 0) {
      (void)impl_->state.truncations.record(
          "delivery.window", accounting_u64(evicted + impl_->deliveries.size()),
          accounting_u64(impl_->deliveries.size()), ReasonCode::DuplicateWindowEvicted);
    }

    const ApplyResult applied = apply_event(impl_->state, event, config_.bounds, config_.weights);
    if (event.at > impl_->state.watermark) impl_->state.watermark = event.at;
    if (event.at > impl_->state.clock) {
      // The runtime has observed this instant, so its notion of "now" moves with
      // the event stream. Freshness then means freshness at the instant the
      // runtime has actually reached.
      impl_->state.clock = event.at;
      refresh_health(impl_->state);
    }
    outcome.code = applied.code;
    outcome.applied = applied.applied;
    outcome.detail = applied.detail;
    if (is_informational(applied.code)) {
      if (applied.applied) {
        report.applied += 1;
      } else {
        report.suppressed += 1;
      }
    } else {
      report.refused += 1;
    }
    report.outcomes.push_back(std::move(outcome));

    Decision decision;
    const Result<OperationId> operation = event.id.valid() ? OperationId::parse(event.id.str())
                                                           : Result<OperationId>(OperationId{});
    decision.operation = operation ? operation.value() : OperationId{};
    decision.kind = applied.kind;
    decision.accepted = applied.accepted;
    decision.primary = applied.code;
    decision.at = event.at;
    decision.epoch = impl_->state.epoch;
    decision.boot = impl_->state.boot;
    decision.topology = impl_->state.topology_generation;
    decision.capability = impl_->state.capability_generation;
    decision.policy = impl_->state.policy_generation;
    decision.deployment = impl_->state.deployment_generation;
    decision.evidence_digest = event.compute_payload_digest();
    decision.policy_digest = impl_->state.policy.policy_digest;
    decision.explanations = applied.explanations;
    decision.explanations.push_back(explanation(applied.code, applied.kind, event.id.str(),
                                                std::string{event_kind_name(event)}));
    // Stamp every explanation with the generations in force when the decision
    // was taken, so a reader can tell which evidence and policy made it legal
    // without reconstructing the timeline.
    for (Explanation& entry : decision.explanations) {
      if (!entry.topology.valid()) entry.topology = decision.topology;
      if (!entry.capability.valid()) entry.capability = decision.capability;
      if (!entry.policy.valid()) entry.policy = decision.policy;
      if (!entry.deployment.valid()) entry.deployment = decision.deployment;
      if (entry.evidence_digest.is_zero()) entry.evidence_digest = decision.evidence_digest;
    }
    std::sort(decision.explanations.begin(), decision.explanations.end());
    if (decision.explanations.size() > config_.bounds.max_explanations) {
      const std::size_t dropped = decision.explanations.size() - config_.bounds.max_explanations;
      decision.explanations.resize(config_.bounds.max_explanations);
      TruncationRecord record;
      record.container = "decision.explanations";
      record.requested = accounting_u64(dropped + config_.bounds.max_explanations);
      record.accepted = accounting_u64(config_.bounds.max_explanations);
      record.dropped = accounting_u64(dropped);
      record.reason = ReasonCode::HistoryTruncated;
      decision.truncations.push_back(record);
    }
    decision.decision_digest = canonical_digest(decision);
    if (impl_->state.decisions.size() >= config_.bounds.max_decisions_retained) {
      impl_->state.decisions.erase(impl_->state.decisions.begin());
      (void)impl_->state.truncations.record(
          "decision.history", accounting_u64(config_.bounds.max_decisions_retained + 1),
          accounting_u64(config_.bounds.max_decisions_retained), ReasonCode::HistoryTruncated);
    }
    impl_->state.decisions.push_back(std::move(decision));
  }

  refresh_health(impl_->state);
  report.evicted = impl_->state.truncations.total_dropped();
  report.truncations = impl_->state.truncations.records();
  report.state_digest = canonical_digest(impl_->state);
  impl_->state_digest = report.state_digest;
  impl_->durable_digest = canonical_digest(placement_projection(impl_->state));
  for (const EventOutcome& outcome : report.outcomes) {
    if (is_refusal(outcome.code)) {
      report.primary = outcome.code;
      break;
    }
  }
  return report;
}

Result<BatchReport> FabricRuntime::submit(const std::vector<FabricEvent>& events) {
  if (events.size() > config_.bounds.max_batch_events) {
    return refuse(ReasonCode::BatchTooLarge, "batch exceeds the configured bound");
  }
  LogicalInstant upto{0};
  std::vector<EventOutcome> stage_failures;
  std::vector<EventOutcome> stage_duplicates;
  for (const FabricEvent& event : events) {
    if (event.at > upto) upto = event.at;
    const Status staged = stage(event);
    if (staged.code() == ReasonCode::DuplicateSuppressed) {
      EventOutcome outcome;
      outcome.id = event.id;
      outcome.code = ReasonCode::DuplicateSuppressed;
      outcome.applied = false;
      outcome.detail = staged.detail();
      stage_duplicates.push_back(std::move(outcome));
      continue;
    }
    if (!staged.ok()) {
      EventOutcome outcome;
      outcome.id = event.id;
      outcome.code = staged.code();
      outcome.applied = false;
      outcome.detail = staged.detail();
      stage_failures.push_back(std::move(outcome));
    }
  }
  Result<BatchReport> report = flush(upto);
  if (!report) return report;
  BatchReport merged = std::move(report).value();
  for (const EventOutcome& outcome : stage_duplicates) {
    merged.suppressed += 1;
    merged.outcomes.push_back(outcome);
  }
  for (const EventOutcome& outcome : stage_failures) {
    merged.refused += 1;
    if (!is_refusal(merged.primary)) merged.primary = outcome.code;
    merged.outcomes.push_back(outcome);
  }
  if (!stage_failures.empty() || !stage_duplicates.empty()) {
    std::sort(merged.outcomes.begin(), merged.outcomes.end(),
              [](const EventOutcome& lhs, const EventOutcome& rhs) { return lhs.id < rhs.id; });
  }
  return merged;
}

Digest FabricRuntime::state_digest() const {
  std::lock_guard<std::mutex> guard{impl_->mutex};
  return canonical_digest(impl_->state);
}

Digest FabricRuntime::durable_digest() const {
  std::lock_guard<std::mutex> guard{impl_->mutex};
  return canonical_digest(placement_projection(impl_->state));
}

RuntimeState FabricRuntime::snapshot() const {
  std::lock_guard<std::mutex> guard{impl_->mutex};
  return impl_->state;
}

JsonValue FabricRuntime::export_json() const {
  std::lock_guard<std::mutex> guard{impl_->mutex};
  JsonValue root = JsonValue::make_object();
  root.set("product", JsonValue{std::string{kProductName}});
  root.set("version", JsonValue{std::string{version_string()}});
  root.set("format_version", JsonValue{static_cast<std::int64_t>(kFormatVersion)});
  root.set("semantic_version", JsonValue{static_cast<std::int64_t>(kSemanticVersion)});
  root.set("state_digest", JsonValue{canonical_digest(impl_->state).hex()});
  root.set("durable_digest",
           JsonValue{canonical_digest(placement_projection(impl_->state)).hex()});
  RuntimeState state_copy = impl_->state;
  root.set("state", encode_json(state_copy));
  RecoverySummary recovery = recovery_;
  root.set("recovery", encode_json(recovery));
  root.set("journal_records",
           JsonValue{static_cast<std::int64_t>(impl_->store != nullptr ? impl_->store->journal_records() : 0)});
  return root;
}

CoordinatorEpoch FabricRuntime::epoch() const {
  std::lock_guard<std::mutex> guard{impl_->mutex};
  return impl_->state.epoch;
}

BootIncarnation FabricRuntime::boot() const {
  std::lock_guard<std::mutex> guard{impl_->mutex};
  return impl_->state.boot;
}

LogicalInstant FabricRuntime::clock() const {
  std::lock_guard<std::mutex> guard{impl_->mutex};
  return impl_->state.clock;
}

std::size_t FabricRuntime::staged_count() const {
  std::lock_guard<std::mutex> guard{impl_->mutex};
  return impl_->pending.size();
}

std::size_t FabricRuntime::instance_count() const {
  std::lock_guard<std::mutex> guard{impl_->mutex};
  return impl_->state.instances.size();
}

std::optional<InstanceState> FabricRuntime::instance(const InstanceId& id) const {
  std::lock_guard<std::mutex> guard{impl_->mutex};
  const InstanceState* found = find_instance(impl_->state.instances, id);
  if (found == nullptr) return std::nullopt;
  return *found;
}

std::optional<DeploymentPlan> FabricRuntime::latest_plan() const {
  std::lock_guard<std::mutex> guard{impl_->mutex};
  if (impl_->state.plans.empty()) return std::nullopt;
  return impl_->state.plans.back();
}

std::optional<Decision> FabricRuntime::last_decision() const {
  std::lock_guard<std::mutex> guard{impl_->mutex};
  if (impl_->state.decisions.empty()) return std::nullopt;
  return impl_->state.decisions.back();
}

std::vector<Explanation> FabricRuntime::explain_last_decision() const {
  std::lock_guard<std::mutex> guard{impl_->mutex};
  if (impl_->state.decisions.empty()) return {};
  return impl_->state.decisions.back().explanations;
}

std::vector<Explanation> FabricRuntime::explain_instance(const InstanceId& id) const {
  std::lock_guard<std::mutex> guard{impl_->mutex};
  const InstanceState* found = find_instance(impl_->state.instances, id);
  if (found == nullptr) {
    std::vector<Explanation> out;
    out.push_back(explanation(ReasonCode::InstanceNotFound, DecisionKind::Lifecycle, id.str(),
                              "no such instance"));
    return out;
  }
  std::vector<Explanation> out = found->reasons;
  out.push_back(explanation(ReasonCode::Ok, DecisionKind::Lifecycle, id.str(),
                            std::string{"state "} + std::string{to_string(found->state)} +
                                " health " + std::string{to_string(found->health)} +
                                (found->health_fresh ? " (fresh)" : " (not fresh)")));
  std::sort(out.begin(), out.end());
  return out;
}

std::optional<DpuRecord> FabricRuntime::device(const DpuId& id) const {
  std::lock_guard<std::mutex> guard{impl_->mutex};
  const DpuRecord* found = impl_->state.topology.find(id);
  if (found == nullptr) return std::nullopt;
  return *found;
}

std::optional<Eligibility> FabricRuntime::explain_eligibility(const DpuId& dpu,
                                                              const ServiceId& service) const {
  std::lock_guard<std::mutex> guard{impl_->mutex};
  const DpuRecord* record = impl_->state.topology.find(dpu);
  const ServiceDefinition* definition = find_service(impl_->state.services, service);
  if (record == nullptr || definition == nullptr) return std::nullopt;
  return EligibilityEvaluator::evaluate(*record, *definition, impl_->state.policy,
                                        impl_->state.clock);
}

Status FabricRuntime::checkpoint_locked() {
  if (impl_->store == nullptr) return Status::success();
  RuntimeState copy = impl_->state;
  const std::vector<std::uint8_t> payload = encode_binary(copy, 4096);
  const Status rotated = impl_->store->rotate(payload);
  if (!rotated.ok()) return rotated;
  impl_->state.journaled_events = 0;
  return Status::success();
}

Status FabricRuntime::checkpoint() {
  std::lock_guard<std::mutex> guard{impl_->mutex};
  if (impl_->closed) return refuse(ReasonCode::StoreClosed, "runtime is closed");
  return checkpoint_locked();
}

Status FabricRuntime::close() {
  std::lock_guard<std::mutex> guard{impl_->mutex};
  if (impl_->closed) return Status::success();
  impl_->closed = true;
  Status result = Status::success();
  if (impl_->store != nullptr) {
    impl_->closed = false;
    const Status checkpointed = checkpoint_locked();
    impl_->closed = true;
    if (!checkpointed.ok()) result = checkpointed;
    const Status store_closed = impl_->store->close();
    if (!store_closed.ok() && result.ok()) result = store_closed;
  }
  impl_->pending.clear();
  return result;
}

bool FabricRuntime::closed() const noexcept {
  std::lock_guard<std::mutex> guard{impl_->mutex};
  return impl_->closed;
}

}  // namespace dpu::fabric
