// Downstream consumer.
//
// This program uses only the installed headers and the exported CMake target: it
// is the proof that the package is usable from an independent project. It
// exercises a real path through the runtime rather than merely linking.

#include <cstdio>
#include <string>
#include <vector>

#include "dpu/fabric/dpu_fabric.hpp"

namespace {

using namespace dpu::fabric;

CapabilityGeneration kCapability{1};
TopologyGeneration kTopology{1};

CapabilitySet capabilities() {
  CapabilitySet set;
  CapabilityValue flag;
  flag.kind = CapabilityValueKind::Flag;
  flag.flag = true;
  Capability entry;
  entry.key = CapabilityKey::literal("crypto.aes");
  entry.value = flag;
  entry.generation = kCapability;
  set.entries.push_back(entry);
  return set;
}

EvidenceRef evidence(std::string_view id, EvidenceKind kind, std::uint64_t at) {
  EvidenceRef ref;
  ref.id = EvidenceId::literal(id);
  ref.kind = kind;
  ref.capability_generation = kCapability;
  ref.topology_generation = kTopology;
  ref.observed_at = LogicalInstant{at};
  ref.valid_until = LogicalInstant{at + 1000};
  ref.provenance.evidence_class = EvidenceClass::Synthetic;
  ref.provenance.source = "downstream.consumer";
  ref.digest = sha256(id);
  return ref;
}

FabricEvent make(std::uint64_t sequence, FabricEventBody body) {
  FabricEvent event;
  event.id = EventId::literal("consumer-" + std::to_string(sequence));
  event.origin = OriginId::literal("consumer-origin");
  event.origin_epoch = OriginEpoch{1};
  event.origin_seq = Sequence{sequence};
  event.at = LogicalInstant{1};
  event.evidence_class = EvidenceClass::Synthetic;
  event.body = std::move(body);
  event.payload_digest = event.compute_payload_digest();
  return event;
}

}  // namespace

int main() {
  std::printf("consumer: %s\n", version_banner().c_str());

  RuntimeConfig config;
  config.store_id = StoreId::literal("consumer-store");
  config.coordinator = OriginId::literal("consumer-coordinator");
  config.epoch = CoordinatorEpoch{1};
  config.boot = BootIncarnation{1};

  Result<std::unique_ptr<FabricRuntime>> runtime = FabricRuntime::open(config);
  if (!runtime) {
    std::fprintf(stderr, "open failed: %s\n", runtime.status().message().c_str());
    return 1;
  }

  std::vector<FabricEvent> events;
  TopologyObserved topology;
  topology.snapshot.generation = kTopology;
  topology.snapshot.observed_at = LogicalInstant{1};
  for (int i = 0; i < 2; ++i) {
    DpuRecord dpu;
    dpu.id = DpuId::literal("consumer-dpu-" + std::to_string(i));
    dpu.attachment = HostAttachmentId::literal("att-" + std::to_string(i));
    dpu.domain = IsolationDomainId::literal("dom-" + std::to_string(i));
    dpu.profile.architecture = DpuArchitecture::Aarch64;
    dpu.profile.firmware = FirmwareApiLevel{2, 0};
    dpu.profile.isolation = {IsolationKind::Process};
    dpu.profile.datapath = DatapathClass::PacketProcessor;
    dpu.capabilities = capabilities();
    dpu.capacity.cpu_millicores = 8000;
    dpu.capacity.memory_bytes = 16000;
    dpu.capacity.network_bps = 8000;
    dpu.capacity.crypto_ops_per_sec = 800;
    dpu.capacity.storage_bytes = 16000;
    dpu.state = DpuState::Attached;
    dpu.topology_generation = kTopology;
    dpu.capability_generation = kCapability;
    dpu.device_evidence = evidence("consumer-dev-" + std::to_string(i), EvidenceKind::Capability, 1);
    dpu.health_evidence = evidence("consumer-health-" + std::to_string(i), EvidenceKind::Health, 1);
    dpu.health = HealthState::Healthy;
    dpu.health_observed_at = LogicalInstant{1};
    dpu.health_fresh = true;
    topology.snapshot.dpus.push_back(std::move(dpu));
  }
  topology.snapshot.evidence = evidence("consumer-topology", EvidenceKind::Topology, 1);
  events.push_back(make(1, topology));

  PolicyAdvanced policy;
  policy.policy.generation = PolicyGeneration{1};
  policy.policy.evidence = evidence("consumer-policy", EvidenceKind::Policy, 1);
  policy.policy.policy_digest = sha256("consumer-policy");
  events.push_back(make(2, policy));

  ServiceDefinition service;
  service.id = ServiceId::literal("consumer-svc");
  service.version = ServiceVersion{1, 0, 0};
  service.display_name = "consumer service";
  service.resources.cpu_millicores = 100;
  service.resources.memory_bytes = 200;
  service.resources.network_bps = 100;
  service.resources.crypto_ops_per_sec = 10;
  service.resources.storage_bytes = 200;
  service.replicas.min = 2;
  service.replicas.preferred = 2;
  service.replicas.max = 2;
  service.compatibility.architecture = DpuArchitecture::Aarch64;
  service.compatibility.min_firmware = FirmwareApiLevel{2, 0};
  service.compatibility.required_isolation = {IsolationKind::Process};
  service.compatibility.capabilities.push_back(
      CapabilityRequirement{CapabilityKey::literal("crypto.aes"), CapabilityOp::Present, {}, {}});
  events.push_back(make(3, ServiceDeclared{service}));

  ServiceGroup group;
  group.id = ServiceGroupId::literal("consumer-group");
  group.members = {service.id};
  group.anti_affinity.max_replicas_per_dpu = 1;
  events.push_back(make(4, GroupDeclared{group}));

  PlacementIntent intent;
  intent.id = OperationId::literal("consumer-intent");
  intent.group = group.id;
  intent.kind = IntentKind::Deploy;
  intent.generation = DeploymentGeneration{1};
  ServiceReplicaIntent entry;
  entry.service = service.id;
  entry.desired_replicas = 2;
  intent.services.push_back(entry);
  intent.staging.batch_size = 1;
  intent.origin = OriginId::literal("consumer-origin");
  intent.origin_epoch = OriginEpoch{1};
  intent.origin_seq = Sequence{100};
  intent.submitted_at = LogicalInstant{1};
  events.push_back(make(5, IntentSubmitted{intent}));

  Result<BatchReport> report = runtime.value()->submit(events);
  if (!report || report.value().applied != events.size()) {
    std::fprintf(stderr, "submit did not apply every event\n");
    return 1;
  }
  if (runtime.value()->instance_count() != 2) {
    std::fprintf(stderr, "expected two instances\n");
    return 1;
  }
  std::printf("instances=%zu\n", runtime.value()->instance_count());
  std::printf("state_digest=%s\n", runtime.value()->state_digest().hex().c_str());
  std::printf("durable_digest=%s\n", runtime.value()->durable_digest().hex().c_str());
  const std::string document = runtime.value()->export_json().to_text(false);
  const Result<JsonValue> parsed = parse_json(document, 1u << 20);
  if (!parsed) {
    std::fprintf(stderr, "export is not valid JSON\n");
    return 1;
  }
  const Status closed = runtime.value()->close();
  if (!closed.ok()) {
    std::fprintf(stderr, "close failed: %s\n", closed.message().c_str());
    return 1;
  }
  std::printf("consumer ok\n");
  return 0;
}
