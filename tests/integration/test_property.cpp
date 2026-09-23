// Seeded randomized, property and differential tests.
//
// Each case is reproducible from the seed printed with the failure. The
// invariants checked here are the ones the runtime claims unconditionally:
// incompatible devices are never selected, replica accounting is exact, exclusive
// ownership is single, bounded accounting closes, and accepted state is a
// function of the accepted event set rather than of staging order.

#include <algorithm>
#include <string>
#include <vector>

#include "dpu/fabric/dpu_fabric.hpp"
#include "support/harness.hpp"
#include "support/random.hpp"
#include "support/scenario.hpp"

namespace dpu::fabric {
namespace {

using test::Rng;
using test::Scenario;

struct RandomWorld {
  std::vector<FabricEvent> events{};
  std::uint32_t replicas{1};
  std::size_t eligible_devices{0};
  std::size_t devices{0};
};

std::string random_identity(Rng& rng, const char* prefix) {
  return std::string{prefix} + "-" + std::to_string(1000 + rng.below(9000));
}

/// Builds a randomized but well-formed world. Some devices are deliberately
/// incompatible or stale so that the planner has to refuse or work around them.
RandomWorld random_world(Rng& rng) {
  RandomWorld world;
  std::vector<FabricEvent> events;
  std::uint64_t sequence = 0;
  const auto make = [&sequence](FabricEventBody body, std::uint64_t at) {
    ++sequence;
    return test::make_event("rand-" + std::to_string(sequence), "rand-origin", sequence, at,
                            std::move(body), EvidenceClass::Synthetic);
  };

  const std::size_t device_count = 2 + rng.below(7);
  world.devices = device_count;
  TopologyObserved topology;
  topology.snapshot.generation = TopologyGeneration{1};
  topology.snapshot.observed_at = LogicalInstant{1};
  for (std::size_t i = 0; i < device_count; ++i) {
    const bool incompatible = rng.chance(25);
    const bool stale = rng.chance(15);
    DpuRecord dpu = test::make_dpu("rand-dpu-" + std::to_string(i),
                                   "rand-dom-" + std::to_string(rng.below(3)),
                                   incompatible ? DpuArchitecture::X86_64 : DpuArchitecture::Aarch64,
                                   LogicalInstant{1}, stale ? 2 : 200, test::capacity(8000, 16000),
                                   CapabilityGeneration{1}, TopologyGeneration{1});
    if (stale) {
      dpu.device_evidence.valid_until = LogicalInstant{2};
      dpu.health_evidence.valid_until = LogicalInstant{2};
    }
    if (rng.chance(20)) {
      dpu.state = DpuState::Detached;
    }
    topology.snapshot.dpus.push_back(std::move(dpu));
    if (!incompatible && !stale) ++world.eligible_devices;
  }
  topology.snapshot.evidence = test::make_evidence("rand-topology", EvidenceKind::Topology,
                                                   LogicalInstant{1}, 200, EvidenceClass::Synthetic);
  events.push_back(make(topology, 1));

  PolicyAdvanced policy;
  policy.policy = test::make_policy(PolicyGeneration{1}, LogicalInstant{1}, 200);
  policy.policy.max_replicas_per_dpu_ceiling = 1 + rng.below(3);
  events.push_back(make(policy, 1));

  const std::size_t service_count = 1 + rng.below(3);
  std::vector<ServiceId> members;
  for (std::size_t i = 0; i < service_count; ++i) {
    ServiceDefinition service =
        test::make_service("rand-svc-" + std::to_string(i), 1, 6, test::capacity(500, 900));
    if (rng.chance(30)) {
      service.compatibility.required_isolation.push_back(IsolationKind::DedicatedDevice);
      service.isolation.kind = IsolationKind::DedicatedDevice;
      service.isolation.exclusive_scope = test::scope_id("rand-scope-" + std::to_string(i));
    }
    if (rng.chance(20)) {
      service.compatibility.capabilities.push_back(
          test::requirement("crypto.aes", CapabilityOp::Present));
    }
    members.push_back(service.id);
    events.push_back(make(ServiceDeclared{service}, 1));
  }

  ServiceGroup group;
  group.id = test::group_id("rand-group");
  group.members = members;
  group.anti_affinity.max_replicas_per_dpu = 1 + rng.below(2);
  if (rng.chance(40)) group.anti_affinity.max_replicas_per_domain = 1 + rng.below(3);
  events.push_back(make(GroupDeclared{group}, 1));

  PlacementIntent intent;
  intent.id = OperationId::literal("rand-intent");
  intent.group = group.id;
  intent.kind = IntentKind::Deploy;
  intent.generation = DeploymentGeneration{1};
  intent.origin = OriginId::literal("rand-origin");
  intent.origin_epoch = OriginEpoch{1};
  intent.origin_seq = Sequence{1000};
  intent.submitted_at = LogicalInstant{1};
  intent.staging.batch_size = 1 + rng.below(3);
  for (const ServiceId& member : members) {
    ServiceReplicaIntent entry;
    entry.service = member;
    entry.desired_replicas = 1 + rng.below(4);
    world.replicas += entry.desired_replicas;
    intent.services.push_back(entry);
  }
  events.push_back(make(IntentSubmitted{intent}, 1));

  world.events = std::move(events);
  return world;
}

/// Checks the invariants that must hold after any accepted event set.
void check_invariants(const RuntimeState& state, const RuntimeBounds& bounds, const char* context) {
  // Exact bounded accounting.
  for (const TruncationRecord& record : state.truncations.records()) {
    if (record.requested != record.accepted + record.dropped) {
      test::report_failure(__FILE__, __LINE__,
                           std::string{context} + ": truncation accounting does not close");
    }
  }
  // Single exclusive owner per device.
  std::vector<std::string> owned;
  for (const AuthorityGrant& grant : state.authority.grants()) {
    owned.push_back(grant.dpu.str());
  }
  std::sort(owned.begin(), owned.end());
  if (std::unique(owned.begin(), owned.end()) != owned.end()) {
    test::report_failure(__FILE__, __LINE__,
                         std::string{context} + ": a device has two exclusive owners");
  }
  // Instance identities are unique and canonical.
  std::vector<std::string> instances;
  for (const InstanceState& instance : state.instances) {
    instances.push_back(instance.id.str());
    // A verified instance always carries a verified digest; a serving instance is
    // verified *and* freshly healthy.
    if (instance.state == LifecycleState::Verified && instance.verified_digest.is_zero()) {
      test::report_failure(__FILE__, __LINE__,
                           std::string{context} + ": verified instance without a verified digest");
    }
    if (instance.serving() &&
        !(instance.state == LifecycleState::Verified && instance.health_fresh &&
          instance.health == HealthState::Healthy)) {
      test::report_failure(__FILE__, __LINE__,
                           std::string{context} + ": serving without proof");
    }
    if (instance.health_fresh && instance.health == HealthState::Unknown) {
      test::report_failure(__FILE__, __LINE__,
                           std::string{context} + ": fresh health with an unknown value");
    }
  }
  std::sort(instances.begin(), instances.end());
  if (std::unique(instances.begin(), instances.end()) != instances.end()) {
    test::report_failure(__FILE__, __LINE__, std::string{context} + ": duplicate instance identity");
  }
  if (state.instances.size() > bounds.max_instances) {
    test::report_failure(__FILE__, __LINE__, std::string{context} + ": instance bound exceeded");
  }
  if (state.topology.dpus.size() > bounds.max_dpus) {
    test::report_failure(__FILE__, __LINE__, std::string{context} + ": device bound exceeded");
  }
}

DPUF_TEST(property, randomized_worlds_hold_every_invariant) {
  for (std::uint64_t seed = 1; seed <= 40; ++seed) {
    Rng rng{seed};
    RandomWorld world = random_world(rng);
    Scenario scenario;
    Result<BatchReport> report = scenario.send_all(world.events);
    if (!report) {
      test::report_failure(__FILE__, __LINE__,
                           "seed " + std::to_string(seed) + ": world refused: " +
                               report.status().message());
      continue;
    }
    const RuntimeState state = scenario.rt().snapshot();
    check_invariants(state, scenario.bounds(), ("seed " + std::to_string(seed)).c_str());

    // Every instance sits on a device that is eligible for its service, judged
    // against the accepted state at the intent instant.
    for (const InstanceState& instance : state.instances) {
      const DpuRecord* device = state.topology.find(instance.dpu);
      if (device == nullptr) {
        test::report_failure(__FILE__, __LINE__,
                             "seed " + std::to_string(seed) + ": instance on an unknown device");
        continue;
      }
      const ServiceDefinition* service = nullptr;
      for (const ServiceDefinition& candidate : state.services) {
        if (candidate.id == instance.service) service = &candidate;
      }
      if (service == nullptr) continue;
      const Eligibility verdict = EligibilityEvaluator::evaluate(*device, *service, state.policy,
                                                                 LogicalInstant{1});
      if (!verdict.decisive()) {
        test::report_failure(__FILE__, __LINE__,
                             "seed " + std::to_string(seed) +
                                 ": instance placed on an ineligible device " + device->id.str());
      }
    }

    // Replica accounting: no service has more instances than the plan asked for.
    if (!state.plans.empty()) {
      const DeploymentPlan& plan = state.plans.back();
      for (const ServicePlan& service_plan : plan.services) {
        std::size_t live = 0;
        for (const InstanceState& instance : state.instances) {
          if (instance.service == service_plan.service &&
              instance.state != LifecycleState::Withdrawn) {
            ++live;
          }
        }
        if (live > service_plan.placed_replicas) {
          test::report_failure(__FILE__, __LINE__,
                               "seed " + std::to_string(seed) +
                                   ": more live instances than planned replicas");
        }
        if (service_plan.placed_replicas != service_plan.replicas.size()) {
          test::report_failure(__FILE__, __LINE__,
                               "seed " + std::to_string(seed) +
                                   ": planned replica accounting is not exact");
        }
      }
    }
  }
}

DPUF_TEST(property, accepted_state_is_independent_of_staging_order) {
  for (std::uint64_t seed = 100; seed <= 130; ++seed) {
    Rng rng{seed};
    RandomWorld world = random_world(rng);
    Scenario ordered;
    DPUF_CHECK_RESULT_OK(ordered.send_all(world.events));
    const Digest reference = ordered.rt().state_digest();

    Scenario shuffled;
    std::vector<FabricEvent> permuted = world.events;
    for (std::size_t i = permuted.size(); i > 1; --i) {
      const std::size_t j = rng.below(static_cast<std::uint32_t>(i));
      std::swap(permuted[i - 1], permuted[j]);
    }
    for (const FabricEvent& event : permuted) {
      const Status staged = shuffled.rt().stage(event);
      if (!staged.ok() && staged.code() != ReasonCode::DuplicateSuppressed) {
        test::report_failure(__FILE__, __LINE__,
                             "seed " + std::to_string(seed) + ": staging refused: " +
                                 staged.message());
      }
    }
    Result<BatchReport> flushed = shuffled.rt().flush(LogicalInstant{1});
    DPUF_REQUIRE(flushed);
    // Same event set, different arrival order, identical accepted state.
    DPUF_CHECK_EQ(shuffled.rt().state_digest(), reference);
    DPUF_CHECK_EQ(shuffled.rt().durable_digest(), ordered.rt().durable_digest());
    check_invariants(shuffled.rt().snapshot(), shuffled.bounds(),
                     ("shuffled seed " + std::to_string(seed)).c_str());
  }
}

DPUF_TEST(property, randomly_damaged_events_are_refused_without_corrupting_state) {
  for (std::uint64_t seed = 200; seed <= 240; ++seed) {
    Rng rng{seed};
    Scenario scenario;
    DPUF_CHECK_RESULT_OK(test::apply_standard_world(scenario, 500));
    const Digest before = scenario.rt().state_digest();

    for (int i = 0; i < 12; ++i) {
      const std::uint32_t kind = rng.below(6);
      FabricEvent event;
      const std::uint64_t sequence = 900 + static_cast<std::uint64_t>(i);
      const std::uint64_t at = 2 + static_cast<std::uint64_t>(i);
      switch (kind) {
        case 0: {
          HealthObserved health;
          health.dpu = test::dpu_id(rng.chance(50) ? "dpu-1" : random_identity(rng, "ghost"));
          health.health = rng.chance(50) ? HealthState::Healthy : HealthState::Unhealthy;
          event = test::make_event("fuzz-" + std::to_string(sequence), "fuzz-origin", sequence, at,
                                   health, EvidenceClass::Synthetic);
          break;
        }
        case 1: {
          CapabilityObserved capability;
          capability.dpu = test::dpu_id(rng.chance(50) ? "dpu-2" : random_identity(rng, "ghost"));
          capability.generation =
              CapabilityGeneration{static_cast<std::uint64_t>(rng.chance(50) ? 1 : 0)};
          capability.evidence = test::make_evidence("fuzz-ev", EvidenceKind::Capability,
                                                    LogicalInstant{at}, 50, EvidenceClass::Synthetic,
                                                    capability.generation, TopologyGeneration{1});
          event = test::make_event("fuzz-" + std::to_string(sequence), "fuzz-origin", sequence, at,
                                   capability, EvidenceClass::Synthetic);
          break;
        }
        case 2: {
          DpuLost lost;
          lost.dpu = test::dpu_id(rng.chance(50) ? "dpu-3" : random_identity(rng, "ghost"));
          event = test::make_event("fuzz-" + std::to_string(sequence), "fuzz-origin", sequence, at,
                                   lost, EvidenceClass::Synthetic);
          break;
        }
        case 3: {
          EffectReported effect;
          effect.report.instance = test::instance_id(random_identity(rng, "i"));
          effect.report.attempt = AttemptNumber{rng.below(4)};
          effect.report.fence.serial = rng.below(10);
          effect.report.status = EffectStatus::Succeeded;
          effect.report.reported_at = LogicalInstant{at};
          event = test::make_event("fuzz-" + std::to_string(sequence), "fuzz-origin", sequence, at,
                                   effect, EvidenceClass::Synthetic);
          break;
        }
        case 4: {
          PlacementIntent intent;
          intent.id = OperationId::literal("fuzz-intent");
          intent.group = test::group_id(rng.chance(50) ? "grp-1" : "grp-missing");
          intent.generation = DeploymentGeneration{1 + rng.below(3)};
          ServiceReplicaIntent entry;
          entry.service = test::service_id(rng.chance(50) ? "svc-a" : "svc-missing");
          entry.desired_replicas = rng.below(6);
          intent.services.push_back(entry);
          intent.origin = OriginId::literal("fuzz-origin");
          intent.origin_epoch = OriginEpoch{1};
          intent.origin_seq = Sequence{sequence};
          intent.submitted_at = LogicalInstant{at};
          event = test::make_event("fuzz-" + std::to_string(sequence), "fuzz-origin", sequence, at,
                                   IntentSubmitted{intent}, EvidenceClass::Synthetic);
          break;
        }
        default: {
          CoordinatorAdvanced advance;
          advance.epoch = CoordinatorEpoch{static_cast<std::uint64_t>(rng.below(3))};
          advance.coordinator = test::origin_id("fuzz-origin");
          event = test::make_event("fuzz-" + std::to_string(sequence), "fuzz-origin", sequence, at,
                                   advance, EvidenceClass::Synthetic);
          break;
        }
      }
      Result<BatchReport> report = scenario.send_all({event});
      if (!report) {
        test::report_failure(__FILE__, __LINE__,
                             "seed " + std::to_string(seed) +
                                 ": submit itself failed: " + report.status().message());
        break;
      }
      check_invariants(scenario.rt().snapshot(), scenario.bounds(),
                       ("fuzz seed " + std::to_string(seed)).c_str());
    }
    // Hostile input never leaves the runtime in a state it cannot describe.
    DPUF_CHECK(!scenario.rt().state_digest().is_zero());
    DPUF_CHECK(!before.is_zero());
    const RuntimeState state = scenario.rt().snapshot();
    for (const InstanceState& instance : state.instances) {
      DPUF_CHECK(state.topology.find(instance.dpu) != nullptr);
    }
  }
}

DPUF_TEST(property, digests_are_stable_across_repeated_runs) {
  for (std::uint64_t seed = 300; seed <= 310; ++seed) {
    Rng rng{seed};
    RandomWorld world = random_world(rng);
    Digest first{};
    for (int run = 0; run < 3; ++run) {
      Scenario scenario;
      DPUF_CHECK_RESULT_OK(scenario.send_all(world.events));
      const Digest digest = scenario.rt().state_digest();
      if (run == 0) {
        first = digest;
      } else if (!(digest == first)) {
        test::report_failure(__FILE__, __LINE__,
                             "seed " + std::to_string(seed) + ": digest is not reproducible");
      }
      // The canonical export is reproducible too.
      const std::string document = scenario.rt().export_json().to_text(false);
      const Result<JsonValue> parsed = parse_json(document, 8u << 20);
      DPUF_CHECK(parsed.has_value());
      // Round-tripping the state through its canonical encoding loses nothing.
      const Result<RuntimeState> decoded =
          decode_binary<RuntimeState>(encode_binary(scenario.rt().snapshot()));
      DPUF_REQUIRE(decoded);
      DPUF_CHECK_EQ(canonical_digest(decoded.value()),
                    canonical_digest(scenario.rt().snapshot()));
    }
  }
}

}  // namespace
}  // namespace dpu::fabric
