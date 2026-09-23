// Benchmarks that measure completed work.
//
// Each benchmark states the completion condition it asserts before the clock is
// read: an event is only counted once it is durable and applied, a plan only once
// it is sealed, a query only once the response has been decoded. Reporting
// enqueue latency would measure nothing about this runtime.

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "dpu/fabric/dpu_fabric.hpp"

namespace {

using namespace dpu::fabric;
using Clock = std::chrono::steady_clock;

struct Measurement {
  std::string name;
  std::uint64_t operations{0};
  double seconds{0};
  std::string completion;
  bool complete{false};

  void report() const {
    const double per_operation = operations == 0 ? 0.0 : seconds / static_cast<double>(operations);
    std::printf("%-34s ops=%-6llu seconds=%8.4f us/op=%9.3f  %s\n", name.c_str(),
                static_cast<unsigned long long>(operations), seconds, per_operation * 1e6,
                completion.c_str());
  }
};

RuntimeConfig config_for(bool persistence) {
  RuntimeConfig config;
  config.store_id = StoreId::literal("benchmark-store");
  config.coordinator = OriginId::literal("benchmark-coordinator");
  config.epoch = CoordinatorEpoch{1};
  config.boot = BootIncarnation{1};
  config.bounds.max_replicas_per_service = 64;
  config.bounds.max_instances = 4096;
  config.bounds.max_dpus = 128;
  config.bounds.max_services = 64;
  if (persistence) config.store_directory = "dpuf-benchmark-store";
  return config;
}

CapabilitySet synthetic_capabilities(CapabilityGeneration generation) {
  CapabilitySet set;
  CapabilityValue flag;
  flag.kind = CapabilityValueKind::Flag;
  flag.flag = true;
  Capability capability;
  capability.key = CapabilityKey::literal("crypto.aes");
  capability.value = flag;
  capability.generation = generation;
  set.entries.push_back(capability);
  return set;
}

EvidenceRef synthetic_evidence(std::uint64_t observed) {
  EvidenceRef evidence;
  evidence.id = EvidenceId::literal("bench-evidence");
  evidence.kind = EvidenceKind::Capability;
  evidence.observed_at = LogicalInstant{observed};
  evidence.valid_until = LogicalInstant{observed + 1000000};
  evidence.provenance.evidence_class = EvidenceClass::Synthetic;
  evidence.provenance.source = "synthetic.benchmark";
  evidence.digest = sha256("bench");
  return evidence;
}

/// A synthetic world with \p dpus devices, one service and one intent.
std::vector<FabricEvent> world_events(std::size_t dpus, std::uint32_t replicas) {
  std::vector<FabricEvent> events;
  std::uint64_t sequence = 0;
  const auto make = [&sequence](FabricEventBody body) {
    ++sequence;
    FabricEvent event;
    event.id = EventId::literal("bench-" + std::to_string(sequence));
    event.origin = OriginId::literal("benchmark-origin");
    event.origin_epoch = OriginEpoch{1};
    event.origin_seq = Sequence{sequence};
    event.at = LogicalInstant{1};
    event.evidence_class = EvidenceClass::Synthetic;
    event.body = std::move(body);
    event.payload_digest = event.compute_payload_digest();
    return event;
  };

  TopologyObserved topology;
  topology.snapshot.generation = TopologyGeneration{1};
  topology.snapshot.observed_at = LogicalInstant{1};
  for (std::size_t i = 0; i < dpus; ++i) {
    DpuRecord dpu;
    dpu.id = DpuId::literal("bench-dpu-" + std::to_string(i));
    dpu.attachment = HostAttachmentId::literal("att-" + std::to_string(i));
    dpu.domain = IsolationDomainId::literal("dom-" + std::to_string(i % 8));
    dpu.profile.architecture = DpuArchitecture::Aarch64;
    dpu.profile.firmware = FirmwareApiLevel{2, 0};
    dpu.profile.isolation = {IsolationKind::Process};
    dpu.profile.datapath = DatapathClass::PacketProcessor;
    dpu.capabilities = synthetic_capabilities(CapabilityGeneration{1});
    dpu.capacity.cpu_millicores = 100000;
    dpu.capacity.memory_bytes = 1000000;
    dpu.capacity.network_bps = 100000;
    dpu.capacity.crypto_ops_per_sec = 100000;
    dpu.capacity.storage_bytes = 1000000;
    dpu.state = DpuState::Attached;
    dpu.topology_generation = TopologyGeneration{1};
    dpu.capability_generation = CapabilityGeneration{1};
    dpu.device_evidence = synthetic_evidence(1);
    dpu.health_evidence = synthetic_evidence(1);
    dpu.health = HealthState::Healthy;
    dpu.health_observed_at = LogicalInstant{1};
    dpu.health_fresh = true;
    topology.snapshot.dpus.push_back(std::move(dpu));
  }
  topology.snapshot.evidence = synthetic_evidence(1);
  events.push_back(make(topology));

  PolicyAdvanced policy_event;
  policy_event.policy.generation = PolicyGeneration{1};
  policy_event.policy.evidence = synthetic_evidence(1);
  policy_event.policy.max_instances_per_group = 4096;
  policy_event.policy.max_stage_batch = 512;
  events.push_back(make(policy_event));

  ServiceDefinition service;
  service.id = ServiceId::literal("bench-svc");
  service.version = ServiceVersion{1, 0, 0};
  service.display_name = "benchmark service";
  service.resources.cpu_millicores = 10;
  service.resources.memory_bytes = 100;
  service.resources.network_bps = 10;
  service.resources.crypto_ops_per_sec = 10;
  service.resources.storage_bytes = 100;
  service.replicas.min = replicas;
  service.replicas.preferred = replicas;
  service.replicas.max = replicas;
  service.compatibility.architecture = DpuArchitecture::Aarch64;
  service.compatibility.min_firmware = FirmwareApiLevel{2, 0};
  service.compatibility.required_isolation = {IsolationKind::Process};
  events.push_back(make(ServiceDeclared{service}));

  ServiceGroup group;
  group.id = ServiceGroupId::literal("bench-group");
  group.members = {service.id};
  group.anti_affinity.max_replicas_per_dpu = 1;
  events.push_back(make(GroupDeclared{group}));

  PlacementIntent intent;
  intent.id = OperationId::literal("bench-intent");
  intent.group = group.id;
  intent.kind = IntentKind::Deploy;
  intent.generation = DeploymentGeneration{1};
  ServiceReplicaIntent entry;
  entry.service = service.id;
  entry.desired_replicas = replicas;
  intent.services.push_back(entry);
  intent.staging.batch_size = 64;
  intent.origin = OriginId::literal("benchmark-origin");
  intent.origin_epoch = OriginEpoch{1};
  intent.origin_seq = Sequence{100};
  intent.submitted_at = LogicalInstant{1};
  events.push_back(make(IntentSubmitted{intent}));
  return events;
}

}  // namespace

namespace {

Measurement measure_event_ingest(std::size_t dpus, std::uint32_t replicas) {
  Measurement measurement;
  measurement.name = "events.ingested_and_applied";
  const RuntimeConfig config = config_for(false);
  Result<std::unique_ptr<FabricRuntime>> runtime = FabricRuntime::open(config);
  if (!runtime) {
    measurement.completion = "open refused";
    return measurement;
  }
  const std::vector<FabricEvent> events = world_events(dpus, replicas);
  const Clock::time_point start = Clock::now();
  Result<BatchReport> batch = runtime.value()->submit(events);
  const Clock::time_point end = Clock::now();
  const std::uint64_t expected_instances = static_cast<std::uint64_t>(replicas);
  const bool applied = batch.has_value() && batch.value().applied == events.size() &&
                       runtime.value()->instance_count() == expected_instances;
  measurement.operations = static_cast<std::uint64_t>(events.size());
  measurement.seconds = std::chrono::duration<double>{end - start}.count();
  measurement.complete = applied;
  measurement.completion = applied ? "all events applied and instance set complete"
                                   : "INCOMPLETE";
  (void)runtime.value()->close();
  return measurement;
}

Measurement measure_placement(std::size_t dpus, std::uint32_t replicas) {
  Measurement measurement;
  measurement.name = "plans.sealed_with_every_replica";
  const RuntimeConfig config = config_for(false);
  Result<std::unique_ptr<FabricRuntime>> runtime = FabricRuntime::open(config);
  if (!runtime) {
    measurement.completion = "open refused";
    return measurement;
  }
  const std::vector<FabricEvent> events = world_events(dpus, replicas);
  if (!runtime.value()->submit(events)) {
    measurement.completion = "seed refused";
    return measurement;
  }
  const RuntimeState state = runtime.value()->snapshot();
  if (state.groups.empty() || state.services.empty()) {
    measurement.completion = "world did not apply";
    return measurement;
  }
  DependencyGraph graph;
  (void)graph.build(state.dependencies, 64);
  const AuthorityRegistry authority;
  const PlacementIntent intent = std::get<IntentSubmitted>(events.back().body).intent;
  PlanRequest request;
  request.topology = &state.topology;
  request.policy = &state.policy;
  request.services = &state.services;
  request.dependencies = &graph;
  request.group = &state.groups[0];
  request.intent = &intent;
  request.authority = &authority;
  request.now = LogicalInstant{2};
  request.epoch = CoordinatorEpoch{1};
  request.boot = BootIncarnation{1};
  request.generation = DeploymentGeneration{2};
  RuntimeBounds bounds;
  bounds.max_replicas_per_service = 64;
  bounds.max_instances = 4096;
  request.bounds = &bounds;

  const int iterations = 20;
  const Clock::time_point start = Clock::now();
  std::uint64_t placed = 0;
  Digest last{};
  for (int i = 0; i < iterations; ++i) {
    const PlanOutcome outcome = build_plan(request);
    if (outcome.feasible) {
      placed += outcome.plan.services.empty() ? 0 : outcome.plan.services[0].placed_replicas;
      last = outcome.plan.digest;
    }
    // Re-seal with a different generation so each iteration does the full job.
    request.generation = DeploymentGeneration{static_cast<std::uint64_t>(i) + 3};
  }
  const Clock::time_point end = Clock::now();
  const std::uint64_t expected = static_cast<std::uint64_t>(iterations) * replicas;
  measurement.operations = static_cast<std::uint64_t>(iterations);
  measurement.seconds = std::chrono::duration<double>{end - start}.count();
  measurement.complete = placed == expected && !last.is_zero();
  measurement.completion = measurement.complete ? "every plan sealed with all replicas placed"
                                                : "INCOMPLETE";
  (void)runtime.value()->close();
  return measurement;
}

Measurement measure_durable_ingest(std::size_t dpus, std::uint32_t replicas) {
  Measurement measurement;
  measurement.name = "durable.events_journaled";
  RuntimeConfig config = config_for(true);
  config.fsync_journal = true;
  Result<std::unique_ptr<FabricRuntime>> runtime = FabricRuntime::open(config);
  if (!runtime) {
    measurement.completion = "open refused";
    return measurement;
  }
  const std::vector<FabricEvent> events = world_events(dpus, replicas);
  const Clock::time_point start = Clock::now();
  Result<BatchReport> batch = runtime.value()->submit(events);
  const Status checkpoint = runtime.value()->checkpoint();
  const Clock::time_point end = Clock::now();
  const bool durable = batch.has_value() && batch.value().applied == events.size() && checkpoint.ok();
  measurement.operations = static_cast<std::uint64_t>(events.size());
  measurement.seconds = std::chrono::duration<double>{end - start}.count();
  measurement.complete = durable;
  measurement.completion = durable ? "every event journaled, applied and checkpointed"
                                   : "INCOMPLETE";
  (void)runtime.value()->close();
  return measurement;
}

}  // namespace

int main() {
  std::printf("DPU Service Fabric benchmarks (completion-measured)\n");
  std::printf("fixtures: SYNTHETIC (no hardware was exercised)\n\n");
  std::fflush(stdout);
  const std::vector<Measurement> measurements = {
      measure_event_ingest(64, 32),
      measure_placement(64, 32),
      measure_durable_ingest(64, 32),
  };
  bool complete = true;
  for (const Measurement& measurement : measurements) {
    measurement.report();
    if (!measurement.complete) complete = false;
  }
  std::printf("\n%s\n", complete ? "all benchmarks completed their asserted work"
                                  : "a benchmark did not complete its asserted work");
  std::fflush(stdout);
  return complete ? 0 : 1;
}
