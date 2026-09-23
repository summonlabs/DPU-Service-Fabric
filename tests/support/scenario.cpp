#include "scenario.hpp"

#include <algorithm>
#include <utility>

namespace dpu::fabric::test {

Scenario::Scenario(RuntimeConfig config) : config_(std::move(config)) {
  Result<std::unique_ptr<FabricRuntime>> created = FabricRuntime::open(config_);
  if (!created) {
    report_failure(__FILE__, __LINE__, "runtime open failed: " + created.status().message());
    runtime_ = nullptr;
    return;
  }
  runtime_ = std::move(created).value();
}

FabricEvent Scenario::make(FabricEventBody body, std::uint64_t at) {
  sequence_ += 1;
  const std::string id = "ev-" + std::to_string(sequence_);
  return make_event(id, "test-origin", sequence_, at, std::move(body), EvidenceClass::Synthetic);
}

Result<BatchReport> Scenario::tick(std::uint64_t delta) {
  const std::uint64_t base = now_ > released_ ? now_ : released_;
  now_ = base + delta;
  LogicalTimeAdvanced advance;
  advance.now = LogicalInstant{now_};
  return send(advance);
}

Result<BatchReport> Scenario::send(FabricEventBody body) {
  if (now_ <= released_) now_ = released_ + 1;
  return send_at(std::move(body), now_);
}

Result<BatchReport> Scenario::send_at(FabricEventBody body, std::uint64_t at) {
  if (runtime_ == nullptr) return refuse(ReasonCode::StoreClosed, "no runtime");
  FabricEvent event = make(std::move(body), at);
  Result<BatchReport> report = runtime_->submit({event});
  if (report) released_ = std::max(released_, report.value().upto.ticks);
  return report;
}

Result<BatchReport> Scenario::send_all(std::vector<FabricEvent> events) {
  if (runtime_ == nullptr) return refuse(ReasonCode::StoreClosed, "no runtime");
  Result<BatchReport> report = runtime_->submit(events);
  if (report) released_ = std::max(released_, report.value().upto.ticks);
  return report;
}

std::vector<DpuRecord> standard_dpus(std::uint64_t observed, std::uint64_t validity,
                                     CapabilityGeneration capability,
                                     TopologyGeneration topology) {
  std::vector<DpuRecord> dpus;
  dpus.push_back(make_dpu("dpu-1", "dom-a", DpuArchitecture::Aarch64, LogicalInstant{observed},
                          validity, capacity(8000, 16000), capability, topology));
  dpus.push_back(make_dpu("dpu-2", "dom-a", DpuArchitecture::Aarch64, LogicalInstant{observed},
                          validity, capacity(8000, 16000), capability, topology));
  dpus.push_back(make_dpu("dpu-3", "dom-b", DpuArchitecture::Aarch64, LogicalInstant{observed},
                          validity, capacity(8000, 16000), capability, topology));
  return dpus;
}

std::vector<FabricEvent> standard_world_events(std::uint64_t observed, std::uint64_t validity) {
  const CapabilityGeneration capability{1};
  const TopologyGeneration topology{1};
  std::vector<FabricEvent> events;
  std::uint64_t sequence = 0;
  const auto next = [&sequence]() {
    sequence += 1;
    return std::string{"w-"} + std::to_string(sequence);
  };

  TopologyObserved topology_event;
  topology_event.snapshot.generation = topology;
  topology_event.snapshot.observed_at = LogicalInstant{observed};
  topology_event.snapshot.dpus = standard_dpus(observed, validity, capability, topology);
  topology_event.snapshot.evidence =
      make_evidence("ev-topology", EvidenceKind::Topology, LogicalInstant{observed}, validity,
                    EvidenceClass::Synthetic, capability, topology);
  events.push_back(make_event(next(), "world-origin", 1, observed, topology_event,
                              EvidenceClass::Synthetic));

  PolicyAdvanced policy_event;
  policy_event.policy = make_policy(PolicyGeneration{1}, LogicalInstant{observed}, validity);
  events.push_back(make_event(next(), "world-origin", 2, observed, policy_event,
                              EvidenceClass::Synthetic));

  ServiceDefinition service_a = make_service("svc-a", 1, 4, capacity(1000, 2000));
  ServiceDeclared declare_a;
  declare_a.service = service_a;
  events.push_back(
      make_event(next(), "world-origin", 3, observed, declare_a, EvidenceClass::Synthetic));

  ServiceDefinition service_b = make_service("svc-b", 1, 4, capacity(1000, 2000));
  ServiceDeclared declare_b;
  declare_b.service = service_b;
  events.push_back(
      make_event(next(), "world-origin", 4, observed, declare_b, EvidenceClass::Synthetic));

  GroupDeclared group;
  group.group.id = group_id("grp-1");
  group.group.members = {service_id("svc-a"), service_id("svc-b")};
  group.group.anti_affinity.spread_across_dpus = true;
  group.group.anti_affinity.max_replicas_per_dpu = 1;
  events.push_back(
      make_event(next(), "world-origin", 5, observed, group, EvidenceClass::Synthetic));

  DependencyDeclared dependency;
  dependency.dependency.id = DependencyId::literal("dep-1");
  dependency.dependency.from = service_id("svc-b");
  dependency.dependency.to = service_id("svc-a");
  dependency.dependency.kind = DependencyKind::Requires;
  events.push_back(
      make_event(next(), "world-origin", 6, observed, dependency, EvidenceClass::Synthetic));
  return events;
}

Result<BatchReport> apply_standard_world(Scenario& scenario, std::uint64_t validity) {
  return scenario.send_all(standard_world_events(scenario.now(), validity));
}

PlacementIntent standard_intent(std::uint64_t at, std::uint32_t replicas) {
  PlacementIntent intent;
  intent.id = OperationId::literal("op-deploy-1");
  intent.group = group_id("grp-1");
  intent.kind = IntentKind::Deploy;
  intent.generation = DeploymentGeneration{1};
  ServiceReplicaIntent entry;
  entry.service = service_id("svc-a");
  entry.desired_replicas = replicas;
  intent.services.push_back(entry);
  intent.staging.batch_size = 1;
  intent.staging.max_unavailable = 0;
  intent.staging.verify_between_stages = true;
  intent.origin = origin_id("test-origin");
  intent.origin_epoch = OriginEpoch{1};
  intent.origin_seq = Sequence{100};
  intent.submitted_at = LogicalInstant{at};
  return intent;
}

}  // namespace dpu::fabric::test
