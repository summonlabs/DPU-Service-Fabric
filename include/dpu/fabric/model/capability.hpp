#pragma once

// Device capability, profile and compatibility model.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dpu/fabric/core/archive.hpp"
#include "dpu/fabric/core/resources.hpp"
#include "dpu/fabric/core/types.hpp"

namespace dpu::fabric {

/// A typed capability value. The kind discriminates which field carries the
/// value; a consumer that requires, say, an integer never reads the text field.
struct CapabilityValue {
  CapabilityValueKind kind{CapabilityValueKind::Unknown};
  bool flag{false};
  std::int64_t integer{0};
  std::string text{};
  ServiceVersion version{};

  friend bool operator==(const CapabilityValue&, const CapabilityValue&) = default;
  friend bool operator<(const CapabilityValue& lhs, const CapabilityValue& rhs) {
    if (lhs.kind != rhs.kind) return lhs.kind < rhs.kind;
    if (lhs.flag != rhs.flag) return lhs.flag < rhs.flag;
    if (lhs.integer != rhs.integer) return lhs.integer < rhs.integer;
    if (lhs.text != rhs.text) return lhs.text < rhs.text;
    return lhs.version.to_string() < rhs.version.to_string();
  }

  [[nodiscard]] std::string to_string() const;

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("CapabilityValue");
    field(ar, "kind", kind);
    field(ar, "flag", flag);
    field(ar, "integer", integer);
    field(ar, "text", text);
    field(ar, "version", version);
    ar.end_object();
  }
};

/// A single capability observation, carrying the generation that produced it.
struct Capability {
  CapabilityKey key{};
  CapabilityValue value{};
  CapabilityGeneration generation{};

  friend bool operator==(const Capability&, const Capability&) = default;

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("Capability");
    field(ar, "key", key);
    field(ar, "value", value);
    field(ar, "generation", generation);
    ar.end_object();
  }
};

/// A capability set in canonical order (ascending key). Duplicate keys are
/// refused at the boundary: a DPU cannot report two answers for one key.
struct CapabilitySet {
  std::vector<Capability> entries{};

  /// Sorts entries by key and refuses duplicates or an over-large set.
  [[nodiscard]] Status normalize(std::size_t max_entries);

  [[nodiscard]] const Capability* find(const CapabilityKey& key) const noexcept;
  [[nodiscard]] bool contains(const CapabilityKey& key) const noexcept { return find(key) != nullptr; }
  [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("CapabilitySet");
    field(ar, "entries", entries);
    ar.end_object();
  }
};

/// Hardware-neutral device profile. Vendor and model are descriptive labels and
/// never participate in a compatibility decision.
struct DpuProfile {
  DpuArchitecture architecture{DpuArchitecture::Unknown};
  FirmwareApiLevel firmware{};
  std::vector<IsolationKind> isolation{};
  DatapathClass datapath{DatapathClass::Unspecified};
  std::string vendor{};
  std::string model{};

  [[nodiscard]] bool supports(IsolationKind kind) const noexcept;

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("DpuProfile");
    field(ar, "architecture", architecture);
    field(ar, "firmware", firmware);
    field(ar, "isolation", isolation);
    field(ar, "datapath", datapath);
    field(ar, "vendor", vendor);
    field(ar, "model", model);
    ar.end_object();
  }
};

/// A requirement on one capability key.
struct CapabilityRequirement {
  CapabilityKey key{};
  CapabilityOp op{CapabilityOp::Present};
  CapabilityValue value{};
  std::vector<CapabilityValue> alternatives{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("CapabilityRequirement");
    field(ar, "key", key);
    field(ar, "op", op);
    field(ar, "value", value);
    field(ar, "alternatives", alternatives);
    ar.end_object();
  }
};

/// Everything a service needs from a DPU in order to be placed on it. Absent
/// optionals mean "no constraint", never "unknown value".
struct CompatibilityRequirement {
  std::optional<DpuArchitecture> architecture{};
  std::optional<FirmwareApiLevel> min_firmware{};
  std::vector<IsolationKind> required_isolation{};
  std::optional<DatapathClass> datapath{};
  std::vector<CapabilityRequirement> capabilities{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("CompatibilityRequirement");
    field(ar, "architecture", architecture);
    field(ar, "min_firmware", min_firmware);
    field(ar, "required_isolation", required_isolation);
    field(ar, "datapath", datapath);
    field(ar, "capabilities", capabilities);
    ar.end_object();
  }
};

/// Isolation a service instance needs, optionally under an exclusive scope.
struct IsolationRequirement {
  IsolationKind kind{IsolationKind::None};
  std::optional<ExclusiveScopeId> exclusive_scope{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("IsolationRequirement");
    field(ar, "kind", kind);
    field(ar, "exclusive_scope", exclusive_scope);
    ar.end_object();
  }
};

/// Replica bounds. min <= preferred <= max is enforced at declaration time.
struct ReplicaPolicy {
  std::uint32_t min{0};
  std::uint32_t preferred{1};
  std::uint32_t max{1};

  [[nodiscard]] Status validate(std::size_t ceiling) const;

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("ReplicaPolicy");
    field(ar, "min", min);
    field(ar, "preferred", preferred);
    field(ar, "max", max);
    ar.end_object();
  }
};

/// Anti-affinity and isolation spread constraints for a service group.
struct AntiAffinityPolicy {
  bool spread_across_dpus{true};
  std::uint32_t max_replicas_per_dpu{1};
  std::optional<std::uint32_t> max_replicas_per_domain{};
  bool require_distinct_domains{false};

  [[nodiscard]] Status validate() const;

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("AntiAffinityPolicy");
    field(ar, "spread_across_dpus", spread_across_dpus);
    field(ar, "max_replicas_per_dpu", max_replicas_per_dpu);
    field(ar, "max_replicas_per_domain", max_replicas_per_domain);
    field(ar, "require_distinct_domains", require_distinct_domains);
    ar.end_object();
  }
};

}  // namespace dpu::fabric
