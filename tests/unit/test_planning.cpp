// Unit tests for eligibility, dependency ordering, authority and the planner.

#include <algorithm>
#include <string>
#include <vector>

#include "dpu/fabric/dpu_fabric.hpp"
#include "support/harness.hpp"
#include "support/scenario.hpp"

namespace dpu::fabric {
namespace {

DpuRecord device(std::string_view id, std::string_view domain, std::uint64_t observed,
                 std::uint64_t validity) {
  return test::make_dpu(id, domain, DpuArchitecture::Aarch64, LogicalInstant{observed}, validity,
                        test::capacity(8000, 16000), CapabilityGeneration{1},
                        TopologyGeneration{1});
}

PolicyState policy(std::uint64_t observed, std::uint64_t validity) {
  return test::make_policy(PolicyGeneration{1}, LogicalInstant{observed}, validity);
}

DPUF_TEST(eligibility, fresh_compatible_device_is_eligible) {
  const DpuRecord dpu = device("dpu-1", "dom-a", 1, 100);
  const ServiceDefinition service = test::make_service("svc-a", 1, 2, test::capacity(100, 200));
  const Eligibility verdict = EligibilityEvaluator::evaluate(dpu, service, policy(1, 100),
                                                             LogicalInstant{10});
  DPUF_CHECK(verdict.eligible);
  DPUF_CHECK(!verdict.unknown);
  DPUF_CHECK(verdict.decisive());
  DPUF_CHECK_EQ(verdict.primary, ReasonCode::Ok);
  DPUF_CHECK(!verdict.evidence_digest.is_zero());
}

DPUF_TEST(eligibility, stale_evidence_makes_capability_state_unknown) {
  const DpuRecord dpu = device("dpu-1", "dom-a", 1, 5);
  ServiceDefinition service = test::make_service("svc-a", 1, 2, test::capacity(100, 200));
  service.compatibility.capabilities.push_back(test::requirement("crypto.aes", CapabilityOp::Present));
  const Eligibility verdict = EligibilityEvaluator::evaluate(dpu, service, policy(1, 5),
                                                             LogicalInstant{50});
  DPUF_CHECK(!verdict.eligible || verdict.unknown);
  DPUF_CHECK(verdict.unknown);
  DPUF_CHECK(!verdict.decisive());
  DPUF_CHECK_EQ(verdict.primary, ReasonCode::EvidenceStale);
}

DPUF_TEST(eligibility, missing_capability_blocks_with_unknown_not_false) {
  const DpuRecord dpu = device("dpu-1", "dom-a", 1, 100);
  ServiceDefinition service = test::make_service("svc-a", 1, 2, test::capacity(100, 200));
  service.compatibility.capabilities.push_back(
      test::requirement("never.observed", CapabilityOp::Present));
  const Eligibility verdict = EligibilityEvaluator::evaluate(dpu, service, policy(1, 100),
                                                             LogicalInstant{10});
  // The capability set is fresh, so the key is known absent: that is a
  // violation, not an unknown.
  DPUF_CHECK(!verdict.eligible);
  DPUF_CHECK(!verdict.unknown);
  DPUF_CHECK_EQ(verdict.primary, ReasonCode::CapabilityMissing);

  // Without fresh capability evidence the same query is unknown, and unknown
  // blocks just as firmly: eligibility is a positive determination, so a device
  // whose state is unknown is never eligible.
  DpuRecord blind = dpu;
  blind.device_evidence = EvidenceRef{};
  const Eligibility unknown = EligibilityEvaluator::evaluate(blind, service, policy(1, 100),
                                                             LogicalInstant{10});
  DPUF_CHECK(!unknown.eligible);
  DPUF_CHECK(unknown.unknown);
  DPUF_CHECK(!unknown.decisive());
  DPUF_CHECK_EQ(unknown.primary, ReasonCode::EvidenceMissing);
}

DPUF_TEST(eligibility, architecture_and_firmware_mismatch_is_explicit) {
  DpuRecord dpu = device("dpu-1", "dom-a", 1, 100);
  dpu.profile.architecture = DpuArchitecture::X86_64;
  const ServiceDefinition service = test::make_service("svc-a", 1, 2, test::capacity(100, 200));
  const Eligibility verdict = EligibilityEvaluator::evaluate(dpu, service, policy(1, 100),
                                                             LogicalInstant{10});
  DPUF_CHECK(!verdict.eligible);
  DPUF_CHECK_EQ(verdict.primary, ReasonCode::ArchitectureMismatch);

  DpuRecord old_firmware = device("dpu-2", "dom-a", 1, 100);
  old_firmware.profile.firmware = FirmwareApiLevel{1, 0};
  const Eligibility firmware = EligibilityEvaluator::evaluate(old_firmware, service, policy(1, 100),
                                                              LogicalInstant{10});
  DPUF_CHECK(!firmware.eligible);
  DPUF_CHECK_EQ(firmware.primary, ReasonCode::FirmwareLevelUnsupported);
}

DPUF_TEST(eligibility, capacity_is_unknown_without_evidence_and_exhausted_with_it) {
  DpuRecord dpu = device("dpu-1", "dom-a", 1, 100);
  dpu.capacity = ResourceVector{};
  const ServiceDefinition service = test::make_service("svc-a", 1, 2, test::capacity(100, 200));
  const Eligibility unknown = EligibilityEvaluator::evaluate(dpu, service, policy(1, 100),
                                                             LogicalInstant{10});
  DPUF_CHECK(unknown.unknown);
  DPUF_CHECK_EQ(unknown.primary, ReasonCode::CapacityUnknown);

  DpuRecord exhausted = device("dpu-1", "dom-a", 1, 100);
  exhausted.allocated = test::capacity(7950, 16000);
  const Eligibility full = EligibilityEvaluator::evaluate(exhausted, service, policy(1, 100),
                                                          LogicalInstant{10});
  DPUF_CHECK(!full.eligible);
  DPUF_CHECK_EQ(full.primary, ReasonCode::AllocationExhausted);
}

DPUF_TEST(eligibility, capability_operators_are_typed) {
  CapabilitySet set;
  set.entries.push_back(
      test::make_capability("crypto.throughput", test::integer_value(40000), CapabilityGeneration{1}));
  set.entries.push_back(test::make_capability("fw.level", test::version_value(2, 1, 0),
                                              CapabilityGeneration{1}));
  set.entries.push_back(test::make_capability("mode", test::flag_value(true), CapabilityGeneration{1}));
  DPUF_CHECK_OK(set.normalize(16));

  DPUF_CHECK(EligibilityEvaluator::evaluate_capability(
                 set, true,
                 test::requirement("crypto.throughput", CapabilityOp::AtLeast, test::integer_value(20000)),
                 test::capability_key("crypto.throughput"))
                 .satisfied());
  DPUF_CHECK(!EligibilityEvaluator::evaluate_capability(
                  set, true,
                  test::requirement("crypto.throughput", CapabilityOp::AtLeast,
                                 test::integer_value(50000)),
                  test::capability_key("crypto.throughput"))
                  .satisfied());
  DPUF_CHECK(EligibilityEvaluator::evaluate_capability(
                 set, true,
                 test::requirement("fw.level", CapabilityOp::AtLeast, test::version_value(2, 0, 0)),
                 test::capability_key("fw.level"))
                 .satisfied());
  DPUF_CHECK(!EligibilityEvaluator::evaluate_capability(
                  set, true,
                  test::requirement("fw.level", CapabilityOp::AtLeast, test::version_value(3, 0, 0)),
                  test::capability_key("fw.level"))
                  .satisfied());
  DPUF_CHECK(EligibilityEvaluator::evaluate_capability(
                 set, true, test::requirement("mode", CapabilityOp::Present),
                 test::capability_key("mode"))
                 .satisfied());
  DPUF_CHECK(EligibilityEvaluator::evaluate_capability(
                 set, true, test::requirement("absent.key", CapabilityOp::Absent),
                 test::capability_key("absent.key"))
                 .satisfied());
  DPUF_CHECK(!EligibilityEvaluator::evaluate_capability(
                  set, true, test::requirement("mode", CapabilityOp::Absent),
                  test::capability_key("mode"))
                  .satisfied());
  // Ordering an integer against a version is a requirement defect, not a
  // device defect, and is refused explicitly.
  const RequirementResult unordered = EligibilityEvaluator::evaluate_capability(
      set, true,
      test::requirement("mode", CapabilityOp::AtLeast, test::integer_value(1)),
      test::capability_key("mode"));
  DPUF_CHECK(unordered.outcome == RequirementOutcome::Violated);
  DPUF_CHECK_EQ(unordered.code, ReasonCode::UnsupportedValue);
  // Without fresh evidence every operator reports unknown.
  DPUF_CHECK(EligibilityEvaluator::evaluate_capability(
                 set, false, test::requirement("mode", CapabilityOp::Present),
                 test::capability_key("mode"))
                 .outcome == RequirementOutcome::Unknown);
}

DPUF_TEST(dependency, cycles_are_detected_with_a_path) {
  DependencyGraph graph;
  std::vector<Dependency> edges;
  const auto edge = [](std::string_view id, std::string_view from, std::string_view to) {
    Dependency dependency;
    dependency.id = DependencyId::literal(id);
    dependency.from = test::service_id(from);
    dependency.to = test::service_id(to);
    dependency.kind = DependencyKind::Requires;
    return dependency;
  };
  edges.push_back(edge("d1", "a", "b"));
  edges.push_back(edge("d2", "b", "c"));
  edges.push_back(edge("d3", "c", "a"));
  DPUF_CHECK_OK(graph.build(edges, 64));
  std::vector<ServiceId> cycle;
  const Result<std::vector<ServiceId>> order = graph.topological_order(
      {test::service_id("a"), test::service_id("b"), test::service_id("c")}, cycle);
  DPUF_CHECK(!order.has_value());
  DPUF_CHECK_CODE(order.status(), ReasonCode::DependencyCycle);
  DPUF_CHECK(cycle.size() >= 2);
  DPUF_CHECK_EQ(cycle.front().str(), std::string{"a"});

  DependencyGraph acyclic;
  DPUF_CHECK_OK(acyclic.build({edge("d1", "a", "b"), edge("d2", "b", "c")}, 64));
  std::vector<ServiceId> no_cycle;
  const Result<std::vector<ServiceId>> sorted = acyclic.topological_order(
      {test::service_id("c"), test::service_id("a"), test::service_id("b")}, no_cycle);
  DPUF_CHECK(sorted.has_value());
  DPUF_CHECK_EQ(sorted.value().size(), std::size_t{3});
  // "a requires b requires c" means c is deployed first: dependencies precede
  // their dependents.
  DPUF_CHECK_EQ(sorted.value()[0].str(), std::string{"c"});
  DPUF_CHECK_EQ(sorted.value()[1].str(), std::string{"b"});
  DPUF_CHECK_EQ(sorted.value()[2].str(), std::string{"a"});
  DPUF_CHECK(acyclic.requirements(test::service_id("b")).size() == 1);
}

DPUF_TEST(dependency, self_edges_and_duplicates_are_refused) {
  DependencyGraph graph;
  Dependency self;
  self.id = DependencyId::literal("d1");
  self.from = test::service_id("a");
  self.to = test::service_id("a");
  DPUF_CHECK_CODE(graph.build({self}, 64), ReasonCode::SelfDependency);
  Dependency first;
  first.id = DependencyId::literal("d1");
  first.from = test::service_id("a");
  first.to = test::service_id("b");
  Dependency second = first;
  second.id = DependencyId::literal("d2");
  DPUF_CHECK_CODE(graph.build({first, second}, 64), ReasonCode::DependencyDuplicate);
}

/// Builds a complete plan request over the standard synthetic world.
struct World {
  TopologySnapshot topology{};
  PolicyState policy{};
  std::vector<ServiceDefinition> services{};
  DependencyGraph dependencies{};
  ServiceGroup group{};
  PlacementIntent intent{};
  AuthorityRegistry authority{};
  RuntimeBounds bounds{};

  [[nodiscard]] PlanRequest request(DeploymentGeneration generation = DeploymentGeneration{1}) {
    PlanRequest out;
    out.topology = &topology;
    out.policy = &policy;
    out.services = &services;
    out.dependencies = &dependencies;
    out.group = &group;
    out.intent = &intent;
    out.authority = &authority;
    out.generation = generation;
    out.now = LogicalInstant{10};
    out.epoch = CoordinatorEpoch{1};
    out.boot = BootIncarnation{1};
    out.bounds = &bounds;
    return out;
  }
};

World make_world(std::uint32_t replicas = 2) {
  World world;
  world.topology.generation = TopologyGeneration{1};
  world.topology.observed_at = LogicalInstant{1};
  world.topology.dpus = test::standard_dpus(1, 100, CapabilityGeneration{1},
                                            TopologyGeneration{1});
  world.topology.evidence = test::make_evidence("ev-topology", EvidenceKind::Topology,
                                                LogicalInstant{1}, 100, EvidenceClass::Synthetic,
                                                CapabilityGeneration{1}, TopologyGeneration{1});
  world.policy = policy(1, 100);
  world.services.push_back(test::make_service("svc-a", 1, 4, test::capacity(1000, 2000)));
  world.services.push_back(test::make_service("svc-b", 1, 4, test::capacity(1000, 2000)));
  world.group.id = test::group_id("grp-1");
  world.group.members = {test::service_id("svc-a"), test::service_id("svc-b")};
  world.group.anti_affinity.spread_across_dpus = true;
  world.group.anti_affinity.max_replicas_per_dpu = 1;
  world.intent = test::standard_intent(10, replicas);
  return world;
}

DPUF_TEST(planner, plan_is_deterministic_and_repeatable) {
  World world = make_world(2);
  const PlanOutcome first = build_plan(world.request());
  const PlanOutcome second = build_plan(world.request());
  DPUF_CHECK(first.feasible);
  DPUF_CHECK(second.feasible);
  DPUF_CHECK_EQ(first.plan.digest, second.plan.digest);
  DPUF_CHECK_EQ(first.plan.id.str(), second.plan.id.str());
  DPUF_CHECK_EQ(encode_binary(first.plan), encode_binary(second.plan));
  DPUF_CHECK_EQ(first.plan.services.size(), std::size_t{1});
  DPUF_CHECK_EQ(first.plan.services[0].desired_replicas, std::uint32_t{2});
  DPUF_CHECK_EQ(first.plan.services[0].placed_replicas, std::uint32_t{2});
  DPUF_CHECK_EQ(first.plan.services[0].replicas.size(), std::size_t{2});
  // Staged rollout follows the intent's batch size and never exceeds it.
  DPUF_CHECK_EQ(first.plan.stages.size(), std::size_t{2});
  DPUF_CHECK_EQ(first.plan.stages[0].instances.size(), std::size_t{1});
  DPUF_CHECK_EQ(first.plan.stages[0].max_unavailable, std::uint32_t{0});
}

DPUF_TEST(planner, replica_accounting_is_exact) {
  World world = make_world(3);
  const PlanOutcome outcome = build_plan(world.request());
  DPUF_CHECK(outcome.feasible);
  const ServicePlan& service_plan = outcome.plan.services[0];
  DPUF_CHECK_EQ(service_plan.placed_replicas, std::uint32_t{3});
  DPUF_CHECK_EQ(service_plan.desired_replicas, service_plan.placed_replicas);
  DPUF_CHECK_EQ(service_plan.replicas.size(), static_cast<std::size_t>(service_plan.placed_replicas));
  std::vector<DpuId> placed;
  for (const PlannedReplica& replica : service_plan.replicas) placed.push_back(replica.dpu);
  std::sort(placed.begin(), placed.end());
  DPUF_CHECK_EQ(std::unique(placed.begin(), placed.end()) - placed.begin(),
                static_cast<std::ptrdiff_t>(placed.size()));
}

DPUF_TEST(planner, incompatible_devices_are_never_selected) {
  World world = make_world(2);
  // Two of the three devices are x86_64 and the third is stale.
  world.topology.dpus[0].profile.architecture = DpuArchitecture::X86_64;
  world.topology.dpus[1].profile.architecture = DpuArchitecture::X86_64;
  world.topology.dpus[2].device_evidence.valid_until = LogicalInstant{2};
  world.topology.dpus[2].health_evidence.valid_until = LogicalInstant{2};
  const PlanOutcome outcome = build_plan(world.request());
  DPUF_CHECK(!outcome.feasible);
  DPUF_CHECK_EQ(outcome.primary, ReasonCode::NoEligibleDpu);
  DPUF_CHECK(outcome.plan.id.str().empty());
  DPUF_CHECK(outcome.plan.digest.is_zero());
  bool saw_architecture = false;
  bool saw_stale = false;
  for (const Explanation& entry : outcome.explanations) {
    if (entry.code == ReasonCode::ArchitectureMismatch) saw_architecture = true;
    if (entry.code == ReasonCode::EvidenceStale) saw_stale = true;
  }
  DPUF_CHECK(saw_architecture);
  DPUF_CHECK(saw_stale);
}

DPUF_TEST(planner, anti_affinity_bounds_placement_and_refuses_when_impossible) {
  // The synthetic world has two isolation domains (dom-a twice, dom-b once), so
  // a per-domain cap of one admits exactly two replicas.
  World world = make_world(2);
  world.group.anti_affinity.max_replicas_per_domain = 1;
  const PlanOutcome limited = build_plan(world.request());
  DPUF_CHECK(limited.feasible);
  std::vector<std::string> domains;
  for (const PlannedReplica& replica : limited.plan.services[0].replicas) {
    domains.push_back(replica.domain.str());
  }
  DPUF_CHECK_EQ(domains.size(), std::size_t{2});
  DPUF_CHECK_EQ(std::unique(domains.begin(), domains.end()) - domains.begin(),
                static_cast<std::ptrdiff_t>(domains.size()));

  World over = make_world(3);
  over.group.anti_affinity.max_replicas_per_domain = 1;
  const PlanOutcome refused_by_domain = build_plan(over.request());
  DPUF_CHECK(!refused_by_domain.feasible);
  DPUF_CHECK_EQ(refused_by_domain.primary, ReasonCode::InsufficientEligibleDpus);

  World tight = make_world(4);
  DPUF_CHECK(!build_plan(tight.request()).feasible);
  const PlanOutcome refused = build_plan(tight.request());
  DPUF_CHECK_EQ(refused.primary, ReasonCode::InsufficientEligibleDpus);
}

DPUF_TEST(planner, unsatisfied_dependencies_are_explicit) {
  World world = make_world(1);
  Dependency requirement;
  requirement.id = DependencyId::literal("dep-1");
  requirement.from = test::service_id("svc-b");
  requirement.to = test::service_id("svc-a");
  requirement.kind = DependencyKind::Requires;
  DPUF_CHECK_OK(world.dependencies.build({requirement}, 16));
  world.intent.services.clear();
  ServiceReplicaIntent entry;
  entry.service = test::service_id("svc-b");
  entry.desired_replicas = 1;
  world.intent.services.push_back(entry);
  // svc-b requires svc-a, which is not part of this intent.
  const PlanOutcome outcome = build_plan(world.request());
  DPUF_CHECK(!outcome.feasible);
  DPUF_CHECK_EQ(outcome.primary, ReasonCode::DependencyUnsatisfied);
  bool saw = false;
  for (const Explanation& entry_explanation : outcome.explanations) {
    if (entry_explanation.code == ReasonCode::DependencyUnsatisfied) saw = true;
  }
  DPUF_CHECK(saw);

  // Pulling the dependency into the intent satisfies it and orders svc-a first.
  World ordered = make_world(1);
  ordered.intent.services.clear();
  ServiceReplicaIntent a;
  a.service = test::service_id("svc-a");
  a.desired_replicas = 1;
  ServiceReplicaIntent b;
  b.service = test::service_id("svc-b");
  b.desired_replicas = 1;
  ordered.intent.services.push_back(b);
  ordered.intent.services.push_back(a);
  Dependency dependency;
  dependency.id = DependencyId::literal("dep-1");
  dependency.from = test::service_id("svc-b");
  dependency.to = test::service_id("svc-a");
  dependency.kind = DependencyKind::Requires;
  DPUF_CHECK_OK(ordered.dependencies.build({dependency}, 16));
  const PlanOutcome sequenced = build_plan(ordered.request());
  DPUF_CHECK(sequenced.feasible);
  DPUF_CHECK_EQ(sequenced.plan.services.size(), std::size_t{2});
  DPUF_CHECK_EQ(sequenced.plan.services[0].service.str(), std::string{"svc-a"});
  DPUF_CHECK_EQ(sequenced.plan.services[1].service.str(), std::string{"svc-b"});
}

DPUF_TEST(planner, dependency_cycles_refuse_the_intent) {
  World world = make_world(1);
  Dependency forward;
  forward.id = DependencyId::literal("dep-1");
  forward.from = test::service_id("svc-a");
  forward.to = test::service_id("svc-b");
  forward.kind = DependencyKind::Requires;
  Dependency backward;
  backward.id = DependencyId::literal("dep-2");
  backward.from = test::service_id("svc-b");
  backward.to = test::service_id("svc-a");
  backward.kind = DependencyKind::Requires;
  DPUF_CHECK_OK(world.dependencies.build({forward, backward}, 16));
  world.intent.services.clear();
  ServiceReplicaIntent a;
  a.service = test::service_id("svc-a");
  a.desired_replicas = 1;
  ServiceReplicaIntent b;
  b.service = test::service_id("svc-b");
  b.desired_replicas = 1;
  world.intent.services.push_back(a);
  world.intent.services.push_back(b);
  const PlanOutcome outcome = build_plan(world.request());
  DPUF_CHECK(!outcome.feasible);
  DPUF_CHECK_EQ(outcome.primary, ReasonCode::DependencyCycle);
}

DPUF_TEST(planner, exclusive_scopes_do_not_double_own_a_device) {
  World world = make_world(2);
  world.services[0].isolation.kind = IsolationKind::DedicatedDevice;
  world.services[0].isolation.exclusive_scope = test::scope_id("scope-a");
  world.services[1].isolation.kind = IsolationKind::DedicatedDevice;
  world.services[1].isolation.exclusive_scope = test::scope_id("scope-b");
  world.intent.services.clear();
  ServiceReplicaIntent a;
  a.service = test::service_id("svc-a");
  a.desired_replicas = 2;
  ServiceReplicaIntent b;
  b.service = test::service_id("svc-b");
  b.desired_replicas = 1;
  world.intent.services.push_back(a);
  world.intent.services.push_back(b);

  // svc-a claims two devices exclusively; svc-b can then only use the third.
  const PlanOutcome outcome = build_plan(world.request());
  DPUF_CHECK(outcome.feasible);
  std::vector<std::string> used;
  for (const ServicePlan& service_plan : outcome.plan.services) {
    for (const PlannedReplica& replica : service_plan.replicas) used.push_back(replica.dpu.str());
  }
  DPUF_CHECK_EQ(used.size(), std::size_t{3});
  std::sort(used.begin(), used.end());
  DPUF_CHECK_EQ(std::unique(used.begin(), used.end()) - used.begin(),
                static_cast<std::ptrdiff_t>(used.size()));

  // Four exclusive replicas cannot fit on three devices: the planner refuses
  // instead of letting two scopes share a device.
  World oversubscribed = world;
  oversubscribed.intent.services[1].desired_replicas = 2;
  const PlanOutcome refused = build_plan(oversubscribed.request());
  DPUF_CHECK(!refused.feasible);

  // An existing grant held by another scope excludes the device outright.
  World conflicting = make_world(2);
  conflicting.services[0].isolation.kind = IsolationKind::DedicatedDevice;
  conflicting.services[0].isolation.exclusive_scope = test::scope_id("scope-a");
  DPUF_CHECK_OK(conflicting.authority.claim(
      test::scope_id("other-scope"), test::group_id("grp-other"), test::dpu_id("dpu-1"),
      CoordinatorEpoch{1}, BootIncarnation{1}, LogicalInstant{1}, 16));
  const PlanOutcome second = build_plan(conflicting.request());
  DPUF_CHECK(second.feasible);
  for (const PlannedReplica& replica : second.plan.services[0].replicas) {
    DPUF_CHECK_NE(replica.dpu.str(), std::string{"dpu-1"});
  }
  bool conflict_explained = false;
  for (const Explanation& entry : second.explanations) {
    if (entry.code == ReasonCode::ExclusiveScopeConflict &&
        entry.subject == std::string{"dpu-1"}) {
      conflict_explained = true;
    }
  }
  DPUF_CHECK(conflict_explained);
}

DPUF_TEST(planner, scoring_is_deterministic_and_ordered) {
  const DpuRecord full = device("dpu-1", "dom-a", 1, 100);
  DpuRecord busy = full;
  busy.id = test::dpu_id("dpu-2");
  busy.allocated = test::capacity(4000, 8000);
  const ServiceDefinition service = test::make_service("svc-a", 1, 2, test::capacity(1000, 2000));
  const std::uint64_t idle_score = score_placement(full, service, 0, 0, PlacementWeights{});
  const std::uint64_t busy_score = score_placement(busy, service, 0, 0, PlacementWeights{});
  DPUF_CHECK(idle_score > busy_score);
  DPUF_CHECK_EQ(idle_score, score_placement(full, service, 0, 0, PlacementWeights{}));
  const std::uint64_t pressured = score_placement(full, service, 3, 0, PlacementWeights{});
  DPUF_CHECK(pressured < idle_score);
  const std::uint64_t crowded = score_placement(full, service, 0, 9, PlacementWeights{});
  DPUF_CHECK(crowded < idle_score);
}

DPUF_TEST(planner, intent_validation_refuses_impossible_requests) {
  {
    World world = make_world(1);
    world.intent.generation = DeploymentGeneration{};
    const PlanOutcome outcome = build_plan(world.request());
    DPUF_CHECK(!outcome.feasible);
    DPUF_CHECK_EQ(outcome.primary, ReasonCode::MalformedInput);
  }
  {
    World world = make_world(1);
    world.intent.services[0].desired_replicas = 99;
    const PlanOutcome outcome = build_plan(world.request());
    DPUF_CHECK(!outcome.feasible);
    DPUF_CHECK_EQ(outcome.primary, ReasonCode::IntentNotAccepted);
  }
  {
    World world = make_world(1);
    world.intent.services[0].service = test::service_id("svc-missing");
    const PlanOutcome outcome = build_plan(world.request());
    DPUF_CHECK(!outcome.feasible);
    DPUF_CHECK_EQ(outcome.primary, ReasonCode::IntentNotAccepted);
  }
  {
    World world = make_world(1);
    world.intent.kind = IntentKind::Rollback;
    const PlanOutcome outcome = build_plan(world.request());
    DPUF_CHECK(!outcome.feasible);
    DPUF_CHECK_EQ(outcome.primary, ReasonCode::RollbackUnavailable);
  }
  {
    World world = make_world(1);
    world.intent.staging.batch_size = 0;
    const PlanOutcome outcome = build_plan(world.request());
    DPUF_CHECK(!outcome.feasible);
    DPUF_CHECK_EQ(outcome.primary, ReasonCode::ValueOutOfRange);
  }
  {
    PlanRequest empty;
    const PlanOutcome outcome = build_plan(empty);
    DPUF_CHECK(!outcome.feasible);
    DPUF_CHECK_EQ(outcome.primary, ReasonCode::MalformedInput);
  }
}

DPUF_TEST(planner, staging_bound_refuses_rather_than_dropping_stages) {
  World world = make_world(3);
  world.bounds.max_stages = 1;
  const PlanOutcome outcome = build_plan(world.request());
  DPUF_CHECK(!outcome.feasible);
  DPUF_CHECK_EQ(outcome.primary, ReasonCode::BoundExceeded);
  DPUF_CHECK_EQ(outcome.truncations.size(), std::size_t{1});
  DPUF_CHECK_EQ(outcome.truncations[0].dropped, std::uint64_t{2});
  DPUF_CHECK_EQ(outcome.truncations[0].requested,
                outcome.truncations[0].accepted + outcome.truncations[0].dropped);
}

DPUF_TEST(planner, explanation_growth_is_bounded_and_accounted) {
  World world = make_world(4);
  world.bounds.max_explanations = 2;
  // Two ineligible devices plus a replica shortfall produce three explanations,
  // which the bound cuts to two; the drop is accounted for.
  world.topology.dpus[0].profile.architecture = DpuArchitecture::X86_64;
  world.topology.dpus[1].profile.architecture = DpuArchitecture::X86_64;
  const PlanOutcome outcome = build_plan(world.request());
  DPUF_CHECK(!outcome.feasible);
  DPUF_CHECK_EQ(outcome.explanations.size(), std::size_t{2});
  DPUF_CHECK_EQ(outcome.truncations.size(), std::size_t{1});
  DPUF_CHECK_EQ(outcome.truncations[0].requested, std::uint64_t{3});
  DPUF_CHECK_EQ(outcome.truncations[0].accepted, std::uint64_t{2});
  DPUF_CHECK_EQ(outcome.truncations[0].dropped, std::uint64_t{1});
  DPUF_CHECK_EQ(outcome.truncations[0].requested,
                outcome.truncations[0].accepted + outcome.truncations[0].dropped);
}

DPUF_TEST(planner, instance_identity_is_stable_and_bounded) {
  const InstanceId first = make_instance_id(test::group_id("grp-1"), test::service_id("svc-a"),
                                            ReplicaIndex{0});
  const InstanceId again = make_instance_id(test::group_id("grp-1"), test::service_id("svc-a"),
                                            ReplicaIndex{0});
  const InstanceId other = make_instance_id(test::group_id("grp-1"), test::service_id("svc-a"),
                                            ReplicaIndex{1});
  DPUF_CHECK_EQ(first.str(), again.str());
  DPUF_CHECK_NE(first.str(), other.str());
  DPUF_CHECK(first.str().size() <= StrongId<struct InstanceIdTag>::kMaxLength);
  const InstanceId long_group = make_instance_id(
      test::group_id(std::string(64, 'g')), test::service_id(std::string(64, 's')),
      ReplicaIndex{12});
  DPUF_CHECK(long_group.str().size() <= StrongId<struct InstanceIdTag>::kMaxLength);
  DPUF_CHECK(long_group.valid());
}

}  // namespace
}  // namespace dpu::fabric
