#pragma once

// Shared fixtures for the DPU Service Fabric test suite.
//
// Everything produced here is SYNTHETIC: the fixtures describe devices and
// services that were never observed on real hardware. Tests that need real
// hardware behaviour do not exist, because this runtime makes no real-hardware
// claim.

#include <string>
#include <vector>

#include "dpu/fabric/dpu_fabric.hpp"
#include "harness.hpp"

namespace dpu::fabric::test {

inline constexpr std::string_view kSyntheticSource = "synthetic.fixture";

[[nodiscard]] inline EvidenceId evidence_id(std::string_view text) {
  return EvidenceId::literal(text);
}

[[nodiscard]] inline InstanceId instance_id(std::string_view text) {
  return InstanceId::literal(text);
}

[[nodiscard]] inline DpuId dpu_id(std::string_view text) { return DpuId::literal(text); }
[[nodiscard]] inline ServiceId service_id(std::string_view text) {
  return ServiceId::literal(text);
}
[[nodiscard]] inline ServiceGroupId group_id(std::string_view text) {
  return ServiceGroupId::literal(text);
}
[[nodiscard]] inline OriginId origin_id(std::string_view text) { return OriginId::literal(text); }
[[nodiscard]] inline EventId event_id(std::string_view text) { return EventId::literal(text); }
[[nodiscard]] inline StoreId store_id(std::string_view text) { return StoreId::literal(text); }
[[nodiscard]] inline ExclusiveScopeId scope_id(std::string_view text) {
  return ExclusiveScopeId::literal(text);
}
[[nodiscard]] inline CapabilityKey capability_key(std::string_view text) {
  return CapabilityKey::literal(text);
}

[[nodiscard]] inline EvidenceRef make_evidence(std::string_view id, EvidenceKind kind,
                                               LogicalInstant observed, std::uint64_t validity,
                                               EvidenceClass evidence_class = EvidenceClass::Real,
                                               CapabilityGeneration capability = {},
                                               TopologyGeneration topology = {},
                                               PolicyGeneration policy = {}) {
  EvidenceRef evidence;
  evidence.id = evidence_id(id);
  evidence.kind = kind;
  evidence.capability_generation = capability;
  evidence.topology_generation = topology;
  evidence.policy_generation = policy;
  evidence.observed_at = observed;
  evidence.valid_until = LogicalInstant{observed.ticks + validity};
  evidence.digest = sha256(std::string{id});
  evidence.provenance.origin = origin_id("fixture-origin");
  evidence.provenance.origin_epoch = OriginEpoch{1};
  evidence.provenance.origin_seq = Sequence{1};
  evidence.provenance.observed_at = observed;
  evidence.provenance.wall_clock = WallClockMs{0};
  evidence.provenance.evidence_class = evidence_class;
  evidence.provenance.source.assign(kSyntheticSource);
  return evidence;
}

[[nodiscard]] inline CapabilityValue flag_value(bool value) {
  CapabilityValue out;
  out.kind = CapabilityValueKind::Flag;
  out.flag = value;
  return out;
}

[[nodiscard]] inline CapabilityValue integer_value(std::int64_t value) {
  CapabilityValue out;
  out.kind = CapabilityValueKind::Integer;
  out.integer = value;
  return out;
}

[[nodiscard]] inline CapabilityValue version_value(std::uint32_t major, std::uint32_t minor,
                                                   std::uint32_t patch) {
  CapabilityValue out;
  out.kind = CapabilityValueKind::Version;
  out.version = ServiceVersion{major, minor, patch};
  return out;
}

[[nodiscard]] inline Capability make_capability(std::string_view key, CapabilityValue value,
                                               CapabilityGeneration generation) {
  Capability capability;
  capability.key = capability_key(key);
  capability.value = std::move(value);
  capability.generation = generation;
  return capability;
}

/// A synthetic DPU with fresh capability and health evidence.
[[nodiscard]] inline DpuRecord make_dpu(std::string_view id, std::string_view domain,
                                        DpuArchitecture architecture, LogicalInstant observed,
                                        std::uint64_t validity, ResourceVector capacity,
                                        CapabilityGeneration capability_generation,
                                        TopologyGeneration topology_generation) {
  DpuRecord dpu;
  dpu.id = dpu_id(id);
  dpu.attachment = HostAttachmentId::literal(std::string{"att-"} + std::string{id});
  dpu.domain = IsolationDomainId::literal(domain);
  dpu.profile.architecture = architecture;
  dpu.profile.firmware = FirmwareApiLevel{2, 0};
  dpu.profile.isolation = {IsolationKind::Process, IsolationKind::DedicatedDevice};
  dpu.profile.datapath = DatapathClass::PacketProcessor;
  dpu.profile.vendor = "synthetic";
  dpu.profile.model = "fixture";
  dpu.capabilities.entries.push_back(make_capability("crypto.aes", flag_value(true),
                                                     capability_generation));
  dpu.capabilities.entries.push_back(
      make_capability("crypto.throughput", integer_value(40000), capability_generation));
  dpu.capability_generation = capability_generation;
  dpu.topology_generation = topology_generation;
  dpu.capacity = capacity;
  dpu.allocated = ResourceVector{};
  dpu.state = DpuState::Attached;
  dpu.device_evidence = make_evidence(std::string{"ev-dev-"} + std::string{id},
                                      EvidenceKind::Capability, observed, validity,
                                      EvidenceClass::Synthetic, capability_generation,
                                      topology_generation);
  dpu.health_evidence = make_evidence(std::string{"ev-health-"} + std::string{id},
                                      EvidenceKind::Health, observed, validity,
                                      EvidenceClass::Synthetic, capability_generation,
                                      topology_generation);
  dpu.health = HealthState::Healthy;
  dpu.health_observed_at = observed;
  dpu.health_fresh = true;
  return dpu;
}

[[nodiscard]] inline ResourceVector capacity(std::uint64_t cpu, std::uint64_t memory) {
  ResourceVector out;
  out.cpu_millicores = cpu;
  out.memory_bytes = memory;
  out.network_bps = cpu * 1000;
  out.crypto_ops_per_sec = cpu * 10;
  out.storage_bytes = memory;
  return out;
}

[[nodiscard]] inline ServiceDefinition make_service(std::string_view id,
                                                   std::uint32_t min_replicas,
                                                   std::uint32_t max_replicas,
                                                   ResourceVector resources) {
  ServiceDefinition service;
  service.id = service_id(id);
  service.version = ServiceVersion{1, 0, 0};
  service.display_name.assign(id);
  service.resources = resources;
  service.replicas.min = min_replicas;
  service.replicas.preferred = min_replicas;
  service.replicas.max = max_replicas;
  service.compatibility.architecture = DpuArchitecture::Aarch64;
  service.compatibility.min_firmware = FirmwareApiLevel{2, 0};
  service.compatibility.required_isolation = {IsolationKind::Process};
  return service;
}

[[nodiscard]] inline PolicyState make_policy(PolicyGeneration generation, LogicalInstant observed,
                                             std::uint64_t validity) {
  PolicyState policy;
  policy.generation = generation;
  policy.evidence = make_evidence("ev-policy", EvidenceKind::Policy, observed, validity,
                                 EvidenceClass::Real, {}, {}, generation);
  policy.policy_digest = sha256("policy");
  return policy;
}

[[nodiscard]] inline CapabilityRequirement requirement(std::string_view key, CapabilityOp op,
                                                    CapabilityValue value = {}) {
  CapabilityRequirement requirement;
  requirement.key = capability_key(key);
  requirement.op = op;
  requirement.value = std::move(value);
  return requirement;
}

/// Builds an event with a deterministic identity supplied by the caller.
[[nodiscard]] inline FabricEvent make_event(std::string_view id, std::string_view origin,
                                            std::uint64_t sequence, std::uint64_t at,
                                            FabricEventBody body,
                                            EvidenceClass evidence_class = EvidenceClass::Real) {
  FabricEvent event;
  event.id = event_id(id);
  event.origin = origin_id(origin);
  event.origin_epoch = OriginEpoch{1};
  event.origin_seq = Sequence{sequence};
  event.at = LogicalInstant{at};
  event.wall_clock = WallClockMs{0};
  event.evidence_class = evidence_class;
  event.body = std::move(body);
  event.payload_digest = event.compute_payload_digest();
  return event;
}

[[nodiscard]] inline FabricEvent make_event(std::string_view id, std::string_view origin,
                                            std::uint64_t sequence, std::uint64_t at,
                                            FabricEventBody body,
                                            EvidenceClass evidence_class,
                                            OriginEpoch origin_epoch) {
  FabricEvent event = make_event(id, origin, sequence, at, std::move(body), evidence_class);
  event.origin_epoch = origin_epoch;
  return event;
}

[[nodiscard]] inline RuntimeConfig make_config(std::size_t journal_bytes = 1u << 20) {
  RuntimeConfig config;
  config.bounds.max_journal_bytes = journal_bytes;
  config.store_id = store_id("store-test");
  config.coordinator = origin_id("coordinator-1");
  config.epoch = CoordinatorEpoch{1};
  config.boot = BootIncarnation{1};
  return config;
}

}  // namespace dpu::fabric::test
