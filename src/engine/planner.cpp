#include "dpu/fabric/engine/planner.hpp"

#include <algorithm>
#include <map>
#include <string>

namespace dpu::fabric {
namespace {

/// Bounded explanation collector: every drop is accounted for.
class ExplanationSink {
 public:
  ExplanationSink(std::vector<Explanation>& out, std::vector<TruncationRecord>& truncations,
                  std::size_t limit)
      : out_(out), truncations_(truncations), limit_(limit) {}

  void add(Explanation explanation) {
    ++requested_;
    if (out_.size() >= limit_) {
      ++dropped_;
      return;
    }
    out_.push_back(std::move(explanation));
  }

  void flush() {
    if (dropped_ == 0) return;
    TruncationRecord record;
    record.container = "plan.explanations";
    record.requested = requested_;
    record.accepted = requested_ - dropped_;
    record.dropped = dropped_;
    record.reason = ReasonCode::HistoryTruncated;
    truncations_.push_back(std::move(record));
  }

 private:
  std::vector<Explanation>& out_;
  std::vector<TruncationRecord>& truncations_;
  std::size_t limit_;
  std::uint64_t requested_{0};
  std::uint64_t dropped_{0};
};

Explanation make_explanation(ReasonCode code, DecisionKind kind, std::string subject,
                             std::string note) {
  Explanation explanation;
  explanation.code = code;
  explanation.kind = kind;
  explanation.subject = std::move(subject);
  explanation.note = std::move(note);
  return explanation;
}

void refuse_outcome(PlanOutcome& outcome, ReasonCode code) {
  outcome.feasible = false;
  outcome.primary = code;
  outcome.plan = DeploymentPlan{};
}

}  // namespace

InstanceId make_instance_id(const ServiceGroupId& group, const ServiceId& service,
                                          ReplicaIndex index) {
  const std::string readable = "i-" + group.str() + "." + service.str() + "." +
                               std::to_string(index.value());
  if (readable.size() <= 64) return InstanceId::literal(readable);
  const Digest digest = sha256(group.str() + "." + service.str());
  return InstanceId::literal("i-" + digest.hex().substr(0, 24) + "." +
                             std::to_string(index.value()));
}


std::uint64_t score_placement(const DpuRecord& dpu, const ServiceDefinition& service,
                              std::uint32_t replicas_on_dpu, std::uint32_t replicas_in_domain,
                              const PlacementWeights& weights) {
  std::uint64_t score = 0;
  const Result<ResourceVector> free = dpu.capacity.headroom(dpu.allocated);
  if (free) {
    const Result<std::uint32_t> permille = free.value().headroom_permille(service.resources);
    if (permille) {
      score = saturating_add(score, saturating_mul(static_cast<std::uint64_t>(permille.value()),
                                                   weights.capacity));
    }
  }
  if (service.isolation.kind != IsolationKind::None &&
      dpu.profile.supports(service.isolation.kind)) {
    score = saturating_add(score, weights.isolation_match);
  }
  const std::uint64_t slack = replicas_in_domain >= 100 ? 0 : 100 - replicas_in_domain;
  score = saturating_add(score, saturating_mul(slack, weights.domain_spread));
  const std::uint64_t penalty = saturating_mul(static_cast<std::uint64_t>(replicas_on_dpu),
                                               weights.dpu_pressure);
  score = score > penalty ? score - penalty : 0;
  return score;
}

PlanOutcome build_plan(const PlanRequest& request, const PlacementWeights& weights) {
  PlanOutcome outcome;
  outcome.feasible = false;
  ExplanationSink sink{outcome.explanations, outcome.truncations,
                       request.bounds != nullptr ? request.bounds->max_explanations : 64};

  const auto fail = [&outcome](ReasonCode code) {
    outcome.feasible = false;
    outcome.primary = code;
    outcome.plan = DeploymentPlan{};
  };

  if (request.topology == nullptr || request.policy == nullptr || request.services == nullptr ||
      request.dependencies == nullptr || request.group == nullptr || request.intent == nullptr ||
      request.authority == nullptr || request.bounds == nullptr) {
    sink.add(make_explanation(ReasonCode::MalformedInput, DecisionKind::Placement, "<request>",
                              "incomplete plan request"));
    sink.flush();
    fail(ReasonCode::MalformedInput);
    return outcome;
  }

  const RuntimeBounds& bounds = *request.bounds;
  const TopologySnapshot& topology = *request.topology;
  const PolicyState& policy = *request.policy;
  const ServiceGroup& group = *request.group;
  const PlacementIntent& intent = *request.intent;
  const DependencyGraph& graph = *request.dependencies;
  const std::vector<ServiceDefinition>& services = *request.services;

  const auto find_service = [&services](const ServiceId& id) -> const ServiceDefinition* {
    for (const ServiceDefinition& service : services) {
      if (service.id == id) return &service;
    }
    return nullptr;
  };

  // ---- intent validation -------------------------------------------------
  if (!intent.id.valid() || !intent.group.valid() || !intent.generation.valid()) {
    sink.add(make_explanation(ReasonCode::MalformedInput, DecisionKind::Placement, "<intent>",
                              "intent identity or generation missing"));
    sink.flush();
    fail(ReasonCode::MalformedInput);
    return outcome;
  }
  if (!(intent.group == group.id)) {
    sink.add(make_explanation(ReasonCode::GroupNotDeclared, DecisionKind::Placement,
                              intent.group.str(), "intent names a group that is not declared"));
    sink.flush();
    fail(ReasonCode::GroupNotDeclared);
    return outcome;
  }
  if (intent.services.empty()) {
    sink.add(make_explanation(ReasonCode::MalformedInput, DecisionKind::Placement, intent.id.str(),
                              "intent carries no service replica intents"));
    sink.flush();
    fail(ReasonCode::MalformedInput);
    return outcome;
  }
  if (intent.services.size() > bounds.max_services) {
    sink.add(make_explanation(ReasonCode::BoundExceeded, DecisionKind::Placement, intent.id.str(),
                              "intent exceeds the service bound"));
    sink.flush();
    fail(ReasonCode::BoundExceeded);
    return outcome;
  }
  if (intent.kind == IntentKind::Rollback && !intent.rollback_target.has_value()) {
    sink.add(make_explanation(ReasonCode::RollbackUnavailable, DecisionKind::Rollback, intent.id.str(),
                              "rollback intent has no target generation"));
    sink.flush();
    fail(ReasonCode::RollbackUnavailable);
    return outcome;
  }
  {
    const Status staging = intent.staging.validate(policy.max_stage_batch);
    if (!staging.ok()) {
      sink.add(make_explanation(staging.code(), DecisionKind::Placement, intent.id.str(),
                                "invalid staging policy"));
      sink.flush();
      fail(staging.code());
      return outcome;
    }
  }

  // ---- service set and dependency closure --------------------------------
  std::vector<ServiceId> intent_services;
  std::map<std::string, std::uint32_t> desired_by_service;
  bool rejected = false;
  for (const ServiceReplicaIntent& entry : intent.services) {
    const ServiceDefinition* service = find_service(entry.service);
    if (service == nullptr) {
      sink.add(make_explanation(ReasonCode::ServiceNotDeclared, DecisionKind::Dependency,
                                entry.service.str(), "service is not declared"));
      rejected = true;
      continue;
    }
    const bool member = std::find(group.members.begin(), group.members.end(), entry.service) !=
                        group.members.end();
    if (!member) {
      sink.add(make_explanation(ReasonCode::IntentNotAccepted, DecisionKind::Placement,
                                entry.service.str(), "service is not a member of the group"));
      rejected = true;
      continue;
    }
    const Status policy_ok = service->replicas.validate(bounds.max_replicas_per_service);
    if (!policy_ok.ok()) {
      sink.add(make_explanation(policy_ok.code(), DecisionKind::Placement, entry.service.str(),
                                "service replica policy is invalid"));
      rejected = true;
      continue;
    }
    if (entry.desired_replicas < service->replicas.min ||
        entry.desired_replicas > service->replicas.max) {
      sink.add(make_explanation(ReasonCode::ReplicaBoundExceeded, DecisionKind::Placement,
                                entry.service.str(),
                                "desired replicas outside the declared replica policy"));
      rejected = true;
      continue;
    }
    if (entry.desired_replicas > policy.max_instances_per_group) {
      sink.add(make_explanation(ReasonCode::ReplicaBoundExceeded, DecisionKind::Placement,
                                entry.service.str(), "desired replicas exceed policy ceiling"));
      rejected = true;
      continue;
    }
    if (desired_by_service.count(entry.service.str()) != 0) {
      sink.add(make_explanation(ReasonCode::DuplicateIdentity, DecisionKind::Placement,
                                entry.service.str(), "service intent appears twice"));
      rejected = true;
      continue;
    }
    desired_by_service.emplace(entry.service.str(), entry.desired_replicas);
    intent_services.push_back(entry.service);
  }
  if (rejected) {
    sink.flush();
    fail(ReasonCode::IntentNotAccepted);
    return outcome;
  }

  std::vector<ServiceId> cycle_path;
  Result<std::vector<ServiceId>> order = graph.topological_order(intent_services, cycle_path);
  if (!order) {
    std::string path;
    for (std::size_t i = 0; i < cycle_path.size(); ++i) {
      if (i != 0) path.append(" -> ");
      path.append(cycle_path[i].str());
    }
    sink.add(make_explanation(ReasonCode::DependencyCycle, DecisionKind::Dependency, path,
                              "dependency cycle among intent services"));
    sink.flush();
    fail(ReasonCode::DependencyCycle);
    return outcome;
  }

  // Dependencies outside the intent must already be present.
  for (const ServiceId& service_id : intent_services) {
    for (const ServiceId& required : graph.requirements(service_id)) {
      if (std::find(intent_services.begin(), intent_services.end(), required) !=
          intent_services.end()) {
        continue;
      }
      const ServiceDefinition* dependency = find_service(required);
      if (dependency == nullptr) {
        sink.add(make_explanation(ReasonCode::ServiceNotDeclared, DecisionKind::Dependency,
                                  service_id.str(),
                                  "requires undeclared service " + required.str()));
        rejected = true;
        continue;
      }
      sink.add(make_explanation(ReasonCode::DependencyUnsatisfied, DecisionKind::Dependency,
                                service_id.str(),
                                "requires " + required.str() + " which is not part of the intent"));
      rejected = true;
    }
  }
  if (rejected) {
    sink.flush();
    fail(ReasonCode::DependencyUnsatisfied);
    return outcome;
  }

  // ---- candidate device order --------------------------------------------
  std::vector<const DpuRecord*> devices;
  devices.reserve(topology.dpus.size());
  for (const DpuRecord& dpu : topology.dpus) devices.push_back(&dpu);
  std::sort(devices.begin(), devices.end(),
            [](const DpuRecord* lhs, const DpuRecord* rhs) { return lhs->id < rhs->id; });
  if (devices.size() > bounds.max_dpus) {
    sink.add(make_explanation(ReasonCode::BoundExceeded, DecisionKind::Placement, "<topology>",
                              "topology exceeds the DPU bound"));
    sink.flush();
    fail(ReasonCode::BoundExceeded);
    return outcome;
  }

  // ---- placement ----------------------------------------------------------
  DeploymentPlan plan;
  plan.generation = intent.generation;
  plan.group = group.id;
  plan.kind = intent.kind;
  plan.rollback_target = intent.rollback_target;
  plan.topology = topology.generation;
  plan.policy = policy.generation;
  plan.epoch = request.epoch;
  plan.boot = request.boot;
  plan.created_at = request.now;

  std::map<std::string, std::uint32_t> replicas_on_dpu;
  std::map<std::string, std::uint32_t> replicas_in_domain;
  std::map<std::string, ResourceVector> remaining;
  // Local view of exclusive ownership: the planner must never mutate the live
  // authority registry, because a refused intent has to leave no trace.
  std::map<std::string, std::string> scope_owner;
  for (const AuthorityGrant& grant : request.authority->grants()) {
    scope_owner.emplace(grant.dpu.str(), grant.scope.str());
  }
  // domain_replicas is group-wide and enforces the per-domain cap across every
  // service of the group; domains_used is per service and enforces distinctness
  // within one service's replica set.
  std::map<std::string, std::uint32_t> domain_replicas;
  bool infeasible = false;
  ReasonCode infeasible_code = ReasonCode::Ok;

  for (const ServiceId& service_id : order.value()) {
    const ServiceDefinition* service = find_service(service_id);
    if (service == nullptr) continue;
    const std::uint32_t desired = desired_by_service[service_id.str()];

    struct Candidate {
      const DpuRecord* dpu;
      std::uint64_t score;
      Digest evidence;
    };
    std::vector<Candidate> candidates;
    for (const DpuRecord* dpu : devices) {
      const Eligibility verdict =
          EligibilityEvaluator::evaluate(*dpu, *service, policy, request.now);
      if (!verdict.eligible) {
        sink.add(make_explanation(verdict.primary, DecisionKind::Eligibility, dpu->id.str(),
                                  "ineligible for " + service->id.str()));
        continue;
      }
      if (verdict.unknown) {
        // Unknown capability or evidence state blocks a claim that needs it.
        sink.add(make_explanation(verdict.primary, DecisionKind::Eligibility, dpu->id.str(),
                                  "unknown capability state blocks " + service->id.str()));
        continue;
      }
      if (service->isolation.exclusive_scope.has_value()) {
        const auto owner = scope_owner.find(dpu->id.str());
        if (owner != scope_owner.end() && owner->second != service->isolation.exclusive_scope->str()) {
          sink.add(make_explanation(ReasonCode::ExclusiveScopeConflict, DecisionKind::Authority,
                                    dpu->id.str(),
                                    "DPU is exclusively owned by scope " + owner->second));
          continue;
        }
      }
      Candidate candidate;
      candidate.dpu = dpu;
      candidate.evidence = verdict.evidence_digest;
      candidate.score = score_placement(*dpu, *service, replicas_on_dpu[dpu->id.str()],
                                        replicas_in_domain[dpu->domain.str()], weights);
      candidates.push_back(candidate);
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& lhs, const Candidate& rhs) {
      if (lhs.score != rhs.score) return lhs.score > rhs.score;
      return lhs.dpu->id < rhs.dpu->id;
    });

    ServicePlan service_plan;
    service_plan.service = service->id;
    service_plan.version = service->version;
    service_plan.desired_replicas = desired;
    service_plan.placed_replicas = 0;

    std::map<std::string, std::uint32_t> domains_used;
    for (const Candidate& candidate : candidates) {
      if (service_plan.placed_replicas >= desired) break;
      const DpuRecord& dpu = *candidate.dpu;
      // The per-DPU cap is the tighter of the group's anti-affinity policy and
      // the policy ceiling. Both are positive by validation.
      const std::uint32_t max_per_dpu =
          std::min(group.anti_affinity.max_replicas_per_dpu,
                   std::max<std::uint32_t>(1, policy.max_replicas_per_dpu_ceiling));
      if (replicas_on_dpu[dpu.id.str()] >= max_per_dpu) continue;
      if (group.anti_affinity.max_replicas_per_domain.has_value() &&
          domain_replicas[dpu.domain.str()] >= *group.anti_affinity.max_replicas_per_domain) {
        continue;
      }
      if (group.anti_affinity.require_distinct_domains && domains_used[dpu.domain.str()] > 0) continue;
      const ResourceVector& free = remaining.count(dpu.id.str()) != 0
                                       ? remaining[dpu.id.str()]
                                       : dpu.capacity.headroom(dpu.allocated).value_or(ResourceVector{});
      if (!free.covers(service->resources)) continue;

      PlannedReplica replica;
      replica.index = ReplicaIndex{service_plan.placed_replicas};
      replica.dpu = dpu.id;
      replica.attachment = dpu.attachment;
      replica.domain = dpu.domain;
      replica.score = candidate.score;
      replica.eligibility_digest = candidate.evidence;
      service_plan.replicas.push_back(std::move(replica));
      service_plan.placed_replicas += 1;
      replicas_on_dpu[dpu.id.str()] += 1;
      replicas_in_domain[dpu.domain.str()] += 1;
      domain_replicas[dpu.domain.str()] += 1;
      domains_used[dpu.domain.str()] += 1;
      const Result<ResourceVector> after = ResourceVector::subtract(free, service->resources);
      if (after) remaining[dpu.id.str()] = after.value();
      if (service->isolation.exclusive_scope.has_value()) {
        scope_owner[dpu.id.str()] = service->isolation.exclusive_scope->str();
      }
    }

    if (service_plan.placed_replicas < desired) {
      const ReasonCode code = candidates.empty() ? ReasonCode::NoEligibleDpu
                                                 : ReasonCode::InsufficientEligibleDpus;
      sink.add(make_explanation(code, DecisionKind::Placement, service->id.str(),
                                "placed " + std::to_string(service_plan.placed_replicas) + " of " +
                                    std::to_string(desired) + " replicas"));
      infeasible = true;
      infeasible_code = code;
    }
    plan.services.push_back(std::move(service_plan));
  }

  if (infeasible) {
    sink.flush();
    fail(infeasible_code);
    return outcome;
  }

  // ---- staged rollout ------------------------------------------------------
  std::vector<InstanceId> rollout_order;
  for (const ServicePlan& service_plan : plan.services) {
    for (const PlannedReplica& replica : service_plan.replicas) {
      rollout_order.push_back(make_instance_id(group.id, service_plan.service, replica.index));
    }
  }
  const std::size_t batch = intent.staging.batch_size;
  for (std::size_t offset = 0; offset < rollout_order.size(); offset += batch) {
    if (plan.stages.size() >= bounds.max_stages) {
      TruncationRecord record;
      record.container = "plan.stages";
      record.requested = (rollout_order.size() + batch - 1) / batch;
      record.accepted = plan.stages.size();
      record.dropped = record.requested - record.accepted;
      record.reason = ReasonCode::BoundExceeded;
      outcome.truncations.push_back(std::move(record));
      sink.flush();
      fail(ReasonCode::BoundExceeded);
      return outcome;
    }
    PlanStage stage;
    stage.index = static_cast<std::uint32_t>(plan.stages.size());
    stage.max_unavailable = intent.staging.max_unavailable;
    const std::size_t end = std::min(offset + batch, rollout_order.size());
    for (std::size_t i = offset; i < end; ++i) stage.instances.push_back(rollout_order[i]);
    plan.stages.push_back(std::move(stage));
  }

  // The accepted reasons travel with the plan as well as with the outcome, so a
  // plan read back from persistence still explains itself. They are part of the
  // sealed body, and they are deterministic, so the digest stays reproducible.
  for (const ServicePlan& service_plan : plan.services) {
    Explanation placed = make_explanation(
        ReasonCode::Ok, DecisionKind::Placement, service_plan.service.str(),
        "placed " + std::to_string(service_plan.placed_replicas) + " replicas at generation " +
            std::to_string(plan.generation.value()));
    sink.add(placed);
    if (plan.explanations.size() < bounds.max_explanations) {
      plan.explanations.push_back(std::move(placed));
    }
  }
  sink.flush();
  plan.outcome = ReasonCode::Ok;
  plan.seal();
  outcome.feasible = true;
  outcome.primary = ReasonCode::Ok;
  outcome.plan = std::move(plan);
  return outcome;
}

}  // namespace dpu::fabric
