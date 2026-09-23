#pragma once

// Events: the only path by which runtime state changes.
//
// Every mutation enters through a staged event, is ordered by a deterministic
// total order over its event key, and is applied by the reducer. Duplicate
// delivery is suppressed by (origin, origin epoch, sequence); a duplicate that
// carries different content is refused as conflicting rather than merged.

#include <cstdint>
#include <vector>

#include "dpu/fabric/core/archive.hpp"
#include "dpu/fabric/core/types.hpp"
#include "dpu/fabric/model/lifecycle.hpp"
#include "dpu/fabric/model/plan.hpp"
#include "dpu/fabric/model/service.hpp"
#include "dpu/fabric/model/topology.hpp"

namespace dpu::fabric {

struct TopologyObserved {
  TopologySnapshot snapshot{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("TopologyObserved");
    field(ar, "snapshot", snapshot);
    ar.end_object();
  }
};

struct CapabilityObserved {
  DpuId dpu{};
  CapabilitySet capabilities{};
  CapabilityGeneration generation{};
  TopologyGeneration topology{};
  EvidenceRef evidence{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("CapabilityObserved");
    field(ar, "dpu", dpu);
    field(ar, "capabilities", capabilities);
    field(ar, "generation", generation);
    field(ar, "topology", topology);
    field(ar, "evidence", evidence);
    ar.end_object();
  }
};

struct HealthObserved {
  DpuId dpu{};
  HealthState health{HealthState::Unknown};
  EvidenceRef evidence{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("HealthObserved");
    field(ar, "dpu", dpu);
    field(ar, "health", health);
    field(ar, "evidence", evidence);
    ar.end_object();
  }
};

struct PolicyAdvanced {
  PolicyState policy{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("PolicyAdvanced");
    field(ar, "policy", policy);
    ar.end_object();
  }
};

struct DpuAttached {
  DpuId dpu{};
  HostAttachmentId attachment{};
  IsolationDomainId domain{};
  TopologyGeneration topology{};
  EvidenceRef evidence{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("DpuAttached");
    field(ar, "dpu", dpu);
    field(ar, "attachment", attachment);
    field(ar, "domain", domain);
    field(ar, "topology", topology);
    field(ar, "evidence", evidence);
    ar.end_object();
  }
};

struct DpuLost {
  DpuId dpu{};
  TopologyGeneration topology{};
  EvidenceRef evidence{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("DpuLost");
    field(ar, "dpu", dpu);
    field(ar, "topology", topology);
    field(ar, "evidence", evidence);
    ar.end_object();
  }
};

struct ServiceDeclared {
  ServiceDefinition service{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("ServiceDeclared");
    field(ar, "service", service);
    ar.end_object();
  }
};

struct GroupDeclared {
  ServiceGroup group{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("GroupDeclared");
    field(ar, "group", group);
    ar.end_object();
  }
};

struct DependencyDeclared {
  Dependency dependency{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("DependencyDeclared");
    field(ar, "dependency", dependency);
    ar.end_object();
  }
};

struct IntentSubmitted {
  PlacementIntent intent{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("IntentSubmitted");
    field(ar, "intent", intent);
    ar.end_object();
  }
};

/// An executor reports that it acknowledged an attempt. Acknowledgement is not
/// a verified effect and never advances an instance to Verified.
struct ExecutionAcknowledged {
  InstanceId instance{};
  AttemptNumber attempt{};
  FenceToken fence{};
  LogicalInstant acknowledged_at{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("ExecutionAcknowledged");
    field(ar, "instance", instance);
    field(ar, "attempt", attempt);
    field(ar, "fence", fence);
    field(ar, "acknowledged_at", acknowledged_at);
    ar.end_object();
  }
};

struct EffectReported {
  EffectReport report{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("EffectReported");
    field(ar, "report", report);
    ar.end_object();
  }
};

/// The coordinator changed. Advancing the epoch fences all authority issued
/// under the previous epoch.
struct CoordinatorAdvanced {
  CoordinatorEpoch epoch{};
  OriginId coordinator{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("CoordinatorAdvanced");
    field(ar, "epoch", epoch);
    field(ar, "coordinator", coordinator);
    ar.end_object();
  }
};

/// Advances logical time. Freshness is evaluated against this value, so tests
/// and replays are deterministic without any dependency on the wall clock.
struct LogicalTimeAdvanced {
  LogicalInstant now{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("LogicalTimeAdvanced");
    field(ar, "now", now);
    ar.end_object();
  }
};

/// Requests quiesce of one service: no new attempts, pending attempts cancelled.
struct QuiesceRequested {
  ServiceId service{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("QuiesceRequested");
    field(ar, "service", service);
    ar.end_object();
  }
};

using FabricEventBody =
    std::variant<TopologyObserved, CapabilityObserved, HealthObserved, PolicyAdvanced, DpuAttached,
                 DpuLost, ServiceDeclared, GroupDeclared, DependencyDeclared, IntentSubmitted,
                 ExecutionAcknowledged, EffectReported, CoordinatorAdvanced, LogicalTimeAdvanced,
                 QuiesceRequested>;

struct FabricEvent {
  EventId id{};
  OriginId origin{};
  OriginEpoch origin_epoch{};
  Sequence origin_seq{};
  LogicalInstant at{};
  WallClockMs wall_clock{};
  EvidenceClass evidence_class{EvidenceClass::Unknown};
  Digest payload_digest{};
  FabricEventBody body{};

  /// Recomputes payload_digest and returns the digest actually carried.
  [[nodiscard]] Digest compute_payload_digest() const;

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("FabricEvent");
    field(ar, "id", id);
    field(ar, "origin", origin);
    field(ar, "origin_epoch", origin_epoch);
    field(ar, "origin_seq", origin_seq);
    field(ar, "at", at);
    field(ar, "wall_clock", wall_clock);
    field(ar, "evidence_class", evidence_class);
    field(ar, "body", body);
    ar.end_object();
  }
};

/// Total order over events. Independent of arrival order and of wall time.
///
/// Ordered by (logical instant, origin identity, origin epoch, origin sequence,
/// event identity). Every component is part of the accepted event, so the order
/// is reproducible from the event set alone.
struct EventKey {
  LogicalInstant at{};
  OriginId origin{};
  OriginEpoch origin_epoch{};
  Sequence origin_seq{};
  EventId id{};
};

[[nodiscard]] EventKey event_key(const FabricEvent& event);
[[nodiscard]] bool event_key_less(const EventKey& lhs, const EventKey& rhs) noexcept;

/// Digest over the event identity and body, used for duplicate detection.
[[nodiscard]] Digest event_digest(const FabricEvent& event);

/// Human-facing event kind name, derived from the active alternative.
[[nodiscard]] std::string_view event_kind_name(const FabricEvent& event);

}  // namespace dpu::fabric
