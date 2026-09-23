#pragma once

// Provenance, evidence references and DPU topology.

#include <cstdint>
#include <string>
#include <vector>

#include "dpu/fabric/core/archive.hpp"
#include "dpu/fabric/core/resources.hpp"
#include "dpu/fabric/core/types.hpp"
#include "dpu/fabric/model/capability.hpp"

namespace dpu::fabric {

/// Where a fact came from. Provenance is part of the value: evidence without
/// provenance cannot be accepted, and evidence polluted by a superseded origin
/// epoch is refused rather than merged.
struct Provenance {
  OriginId origin{};
  OriginEpoch origin_epoch{};
  Sequence origin_seq{};
  LogicalInstant observed_at{};
  WallClockMs wall_clock{};
  EvidenceClass evidence_class{EvidenceClass::Unknown};
  std::string source{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("Provenance");
    field(ar, "origin", origin);
    field(ar, "origin_epoch", origin_epoch);
    field(ar, "origin_seq", origin_seq);
    field(ar, "observed_at", observed_at);
    field(ar, "wall_clock", wall_clock);
    field(ar, "evidence_class", evidence_class);
    field(ar, "source", source);
    ar.end_object();
  }
};

/// A reference to one item of evidence, with the generations it was produced
/// under and the logical instant after which it is stale.
struct EvidenceRef {
  EvidenceId id{};
  EvidenceKind kind{EvidenceKind::Topology};
  CapabilityGeneration capability_generation{};
  TopologyGeneration topology_generation{};
  PolicyGeneration policy_generation{};
  LogicalInstant observed_at{};
  LogicalInstant valid_until{};
  Digest digest{};
  Provenance provenance{};

  /// Freshness is evaluated at a logical instant, never on the wall clock.
  [[nodiscard]] bool fresh_at(LogicalInstant now) const noexcept {
    return now <= valid_until && observed_at <= now;
  }

  [[nodiscard]] bool present() const noexcept { return id.valid(); }

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("EvidenceRef");
    field(ar, "id", id);
    field(ar, "kind", kind);
    field(ar, "capability_generation", capability_generation);
    field(ar, "topology_generation", topology_generation);
    field(ar, "policy_generation", policy_generation);
    field(ar, "observed_at", observed_at);
    field(ar, "valid_until", valid_until);
    field(ar, "digest", digest);
    field(ar, "provenance", provenance);
    ar.end_object();
  }
};

/// One DPU as the fabric currently understands it.
struct DpuRecord {
  DpuId id{};
  HostAttachmentId attachment{};
  IsolationDomainId domain{};
  DpuProfile profile{};
  CapabilitySet capabilities{};
  ResourceVector capacity{};
  ResourceVector allocated{};
  DpuState state{DpuState::Unknown};
  TopologyGeneration topology_generation{};
  CapabilityGeneration capability_generation{};
  EvidenceRef device_evidence{};
  EvidenceRef health_evidence{};
  HealthState health{HealthState::Unknown};
  LogicalInstant health_observed_at{};
  bool health_fresh{false};

  [[nodiscard]] bool attached() const noexcept { return state == DpuState::Attached; }

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("DpuRecord");
    field(ar, "id", id);
    field(ar, "attachment", attachment);
    field(ar, "domain", domain);
    field(ar, "profile", profile);
    field(ar, "capabilities", capabilities);
    field(ar, "capacity", capacity);
    field(ar, "allocated", allocated);
    field(ar, "state", state);
    field(ar, "topology_generation", topology_generation);
    field(ar, "capability_generation", capability_generation);
    field(ar, "device_evidence", device_evidence);
    field(ar, "health_evidence", health_evidence);
    field(ar, "health", health);
    field(ar, "health_observed_at", health_observed_at);
    field(ar, "health_fresh", health_fresh);
    ar.end_object();
  }
};

/// The accepted topology: the DPU inventory at one topology generation.
struct TopologySnapshot {
  TopologyGeneration generation{};
  LogicalInstant observed_at{};
  std::vector<DpuRecord> dpus{};
  EvidenceRef evidence{};
  Provenance provenance{};

  [[nodiscard]] const DpuRecord* find(const DpuId& id) const noexcept;
  [[nodiscard]] DpuRecord* find(const DpuId& id) noexcept;

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("TopologySnapshot");
    field(ar, "generation", generation);
    field(ar, "observed_at", observed_at);
    field(ar, "dpus", dpus);
    field(ar, "evidence", evidence);
    field(ar, "provenance", provenance);
    ar.end_object();
  }
};

}  // namespace dpu::fabric
