#include "dpu/fabric/model/capability.hpp"

#include <algorithm>

#include "dpu/fabric/model/event.hpp"
#include "dpu/fabric/model/plan.hpp"
#include "dpu/fabric/model/topology.hpp"

namespace dpu::fabric {

// ---------------------------------------------------------------------------
// Capability
// ---------------------------------------------------------------------------

std::string CapabilityValue::to_string() const {
  switch (kind) {
    case CapabilityValueKind::Unknown:
      return "unknown";
    case CapabilityValueKind::Flag:
      return flag ? "true" : "false";
    case CapabilityValueKind::Integer:
      return std::to_string(integer);
    case CapabilityValueKind::Text:
      return text;
    case CapabilityValueKind::Version:
      return version.to_string();
  }
  return "unknown";
}

Status CapabilitySet::normalize(std::size_t max_entries) {
  if (entries.size() > max_entries) {
    return refuse(ReasonCode::BoundExceeded, "capability set exceeds bound");
  }
  std::sort(entries.begin(), entries.end(),
            [](const Capability& lhs, const Capability& rhs) { return lhs.key < rhs.key; });
  for (std::size_t i = 0; i < entries.size(); ++i) {
    Capability& entry = entries[i];
    if (!entry.key.valid()) {
      return refuse(ReasonCode::InvalidIdentity, "capability key missing");
    }
    if (entry.value.kind == CapabilityValueKind::Unknown) {
      // An entry that carries no value is not an observation. Accepting it would
      // let "unknown" masquerade as a present capability.
      return refuse(ReasonCode::CapabilityUnknown, entry.key.str());
    }
    if (entry.value.text.size() > 256) {
      return refuse(ReasonCode::OversizedInput, entry.key.str());
    }
    if (i > 0 && entries[i - 1].key == entry.key) {
      return refuse(ReasonCode::DuplicateIdentity, entry.key.str());
    }
  }
  return Status::success();
}

const Capability* CapabilitySet::find(const CapabilityKey& key) const noexcept {
  const auto it = std::lower_bound(
      entries.begin(), entries.end(), key,
      [](const Capability& entry, const CapabilityKey& probe) { return entry.key < probe; });
  if (it == entries.end() || !(it->key == key)) return nullptr;
  return &*it;
}

bool DpuProfile::supports(IsolationKind kind) const noexcept {
  return std::find(isolation.begin(), isolation.end(), kind) != isolation.end();
}

// ---------------------------------------------------------------------------
// Policy validation helpers
// ---------------------------------------------------------------------------

Status ReplicaPolicy::validate(std::size_t ceiling) const {
  if (max == 0) return refuse(ReasonCode::ValueOutOfRange, "max replicas must be positive");
  if (min > preferred || preferred > max) {
    return refuse(ReasonCode::ValueOutOfRange, "replica policy is not monotone");
  }
  if (static_cast<std::size_t>(max) > ceiling) {
    return refuse(ReasonCode::ReplicaBoundExceeded, "max replicas exceeds bound");
  }
  return Status::success();
}

Status AntiAffinityPolicy::validate() const {
  if (max_replicas_per_dpu == 0) {
    return refuse(ReasonCode::ValueOutOfRange, "max replicas per DPU must be positive");
  }
  if (max_replicas_per_domain.has_value() && *max_replicas_per_domain == 0) {
    return refuse(ReasonCode::ValueOutOfRange, "max replicas per domain must be positive");
  }
  return Status::success();
}

Status StagedRolloutPolicy::validate(std::size_t ceiling) const {
  if (batch_size == 0) return refuse(ReasonCode::ValueOutOfRange, "batch size must be positive");
  if (static_cast<std::size_t>(batch_size) > ceiling) {
    return refuse(ReasonCode::BoundExceeded, "batch size exceeds policy ceiling");
  }
  if (max_unavailable >= batch_size && batch_size > 1) {
    return refuse(ReasonCode::StagingInfeasible, "max_unavailable must be below batch size");
  }
  return Status::success();
}

// ---------------------------------------------------------------------------
// Topology
// ---------------------------------------------------------------------------

const DpuRecord* TopologySnapshot::find(const DpuId& id) const noexcept {
  for (const DpuRecord& dpu : dpus) {
    if (dpu.id == id) return &dpu;
  }
  return nullptr;
}

DpuRecord* TopologySnapshot::find(const DpuId& id) noexcept {
  for (DpuRecord& dpu : dpus) {
    if (dpu.id == id) return &dpu;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Plans
// ---------------------------------------------------------------------------

void DeploymentPlan::seal() {
  const Digest body = canonical_digest(*this);
  digest = body;
  id = PlanId::literal("plan-" + body.hex().substr(0, 32));
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

Digest FabricEvent::compute_payload_digest() const { return canonical_digest(body); }

namespace {

struct EventIdentity {
  EventId id{};
  OriginId origin{};
  OriginEpoch origin_epoch{};
  Sequence origin_seq{};
  LogicalInstant at{};
  Digest payload{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("EventIdentity");
    field(ar, "id", id);
    field(ar, "origin", origin);
    field(ar, "origin_epoch", origin_epoch);
    field(ar, "origin_seq", origin_seq);
    field(ar, "at", at);
    field(ar, "payload", payload);
    ar.end_object();
  }
};

}  // namespace

EventKey event_key(const FabricEvent& event) {
  EventKey key;
  key.at = event.at;
  key.origin = event.origin;
  key.origin_epoch = event.origin_epoch;
  key.origin_seq = event.origin_seq;
  key.id = event.id;
  return key;
}

bool event_key_less(const EventKey& lhs, const EventKey& rhs) noexcept {
  if (lhs.at != rhs.at) return lhs.at < rhs.at;
  if (lhs.origin != rhs.origin) return lhs.origin < rhs.origin;
  if (lhs.origin_epoch != rhs.origin_epoch) return lhs.origin_epoch < rhs.origin_epoch;
  if (lhs.origin_seq != rhs.origin_seq) return lhs.origin_seq < rhs.origin_seq;
  return lhs.id < rhs.id;
}

Digest event_digest(const FabricEvent& event) {
  EventIdentity identity;
  identity.id = event.id;
  identity.origin = event.origin;
  identity.origin_epoch = event.origin_epoch;
  identity.origin_seq = event.origin_seq;
  identity.at = event.at;
  identity.payload = event.compute_payload_digest();
  return canonical_digest(identity);
}

std::string_view event_kind_name(const FabricEvent& event) {
  return std::visit(
      [](const auto& body) -> std::string_view {
        using B = std::decay_t<decltype(body)>;
        if constexpr (std::is_same_v<B, TopologyObserved>) {
          return "TopologyObserved";
        } else if constexpr (std::is_same_v<B, CapabilityObserved>) {
          return "CapabilityObserved";
        } else if constexpr (std::is_same_v<B, HealthObserved>) {
          return "HealthObserved";
        } else if constexpr (std::is_same_v<B, PolicyAdvanced>) {
          return "PolicyAdvanced";
        } else if constexpr (std::is_same_v<B, DpuAttached>) {
          return "DpuAttached";
        } else if constexpr (std::is_same_v<B, DpuLost>) {
          return "DpuLost";
        } else if constexpr (std::is_same_v<B, ServiceDeclared>) {
          return "ServiceDeclared";
        } else if constexpr (std::is_same_v<B, GroupDeclared>) {
          return "GroupDeclared";
        } else if constexpr (std::is_same_v<B, DependencyDeclared>) {
          return "DependencyDeclared";
        } else if constexpr (std::is_same_v<B, IntentSubmitted>) {
          return "IntentSubmitted";
        } else if constexpr (std::is_same_v<B, ExecutionAcknowledged>) {
          return "ExecutionAcknowledged";
        } else if constexpr (std::is_same_v<B, EffectReported>) {
          return "EffectReported";
        } else if constexpr (std::is_same_v<B, CoordinatorAdvanced>) {
          return "CoordinatorAdvanced";
        } else if constexpr (std::is_same_v<B, LogicalTimeAdvanced>) {
          return "LogicalTimeAdvanced";
        } else if constexpr (std::is_same_v<B, QuiesceRequested>) {
          return "QuiesceRequested";
        } else {
          return "Unknown";
        }
      },
      event.body);
}

}  // namespace dpu::fabric
