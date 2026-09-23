#pragma once

// Strongly typed identities, generations, time and checked arithmetic.
//
// Correctness-critical quantities are distinct types: a topology generation can
// never be passed where a policy generation is expected, and a DPU identity can
// never be passed where a service identity is expected. Textual identities are
// validated at the parse boundary and bounded in length so that externally
// derived names cannot grow memory without limit.

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>

#include "dpu/fabric/core/result.hpp"

namespace dpu::fabric {

// ---------------------------------------------------------------------------
// Enum declaration helper
// ---------------------------------------------------------------------------

#define DPUF_ENUM_ENTRY(name) name,
#define DPUF_ENUM_NAME(name) #name,

/// Declares a scoped enumeration whose textual form is stable and whose values
/// serialize by name (never by number) so that renumbering cannot silently
/// change persisted semantics.
#define DPUF_DECLARE_ENUM(Type, LIST)                                                    \
  enum class Type : std::uint8_t { LIST(DPUF_ENUM_ENTRY) };                              \
  inline constexpr std::string_view k##Type##Names[] = {LIST(DPUF_ENUM_NAME)};            \
  inline constexpr std::size_t k##Type##Count = std::size(k##Type##Names);                \
  [[nodiscard]] inline std::string_view to_string(Type value) noexcept {                  \
    const auto index = static_cast<std::size_t>(value);                                   \
    return index < k##Type##Count ? k##Type##Names[index] : std::string_view{"Unknown"};  \
  }                                                                                       \
  [[nodiscard]] inline bool from_string(std::string_view text, Type& out) noexcept { \
    for (std::size_t i = 0; i < k##Type##Count; ++i) {                                    \
      if (k##Type##Names[i] == text) {                                                    \
        out = static_cast<Type>(i);                                                       \
        return true;                                                                      \
      }                                                                                   \
    }                                                                                     \
    return false;                                                                         \
  }

#define DPUF_ARCHITECTURE_LIST(X) X(Unknown) X(Aarch64) X(X86_64) X(RiscV64)
DPUF_DECLARE_ENUM(DpuArchitecture, DPUF_ARCHITECTURE_LIST)

#define DPUF_DATAPATH_LIST(X) \
  X(Unspecified) X(None) X(PacketProcessor) X(CryptoOffload) X(StorageOffload) X(MemoryFabric)
DPUF_DECLARE_ENUM(DatapathClass, DPUF_DATAPATH_LIST)

#define DPUF_ISOLATION_LIST(X) \
  X(None) X(Process) X(Virtualization) X(ConfidentialDomain) X(DedicatedDevice)
DPUF_DECLARE_ENUM(IsolationKind, DPUF_ISOLATION_LIST)

#define DPUF_DPU_STATE_LIST(X) X(Unknown) X(Attached) X(Detached) X(Lost) X(Quarantined) X(Retired)
DPUF_DECLARE_ENUM(DpuState, DPUF_DPU_STATE_LIST)

#define DPUF_HEALTH_LIST(X) X(Unknown) X(Healthy) X(Degraded) X(Unhealthy)
DPUF_DECLARE_ENUM(HealthState, DPUF_HEALTH_LIST)

/// Provenance class of evidence. Real evidence was produced by an adjacent
/// runtime observing hardware; Synthetic evidence was produced by a fixture or
/// simulator; Unsupported marks a claim this runtime cannot make.
#define DPUF_EVIDENCE_CLASS_LIST(X) X(Unknown) X(Real) X(Synthetic) X(Unsupported)
DPUF_DECLARE_ENUM(EvidenceClass, DPUF_EVIDENCE_CLASS_LIST)

#define DPUF_EVIDENCE_KIND_LIST(X) \
  X(Topology) X(Capability) X(Health) X(Policy) X(Execution) X(Authority)
DPUF_DECLARE_ENUM(EvidenceKind, DPUF_EVIDENCE_KIND_LIST)

#define DPUF_LIFECYCLE_LIST(X)                                                     \
  X(Absent) X(Planned) X(Authorized) X(Deploying) X(Acknowledged) X(Unverified)     \
  X(Verified) X(Quiescing) X(Withdrawn) X(Failed) X(Lost) X(Superseded) X(Cancelled)
DPUF_DECLARE_ENUM(LifecycleState, DPUF_LIFECYCLE_LIST)

#define DPUF_DEPENDENCY_LIST(X) X(Requires) X(OrderingAfter) X(RequiresCapability)
DPUF_DECLARE_ENUM(DependencyKind, DPUF_DEPENDENCY_LIST)

#define DPUF_CAPABILITY_OP_LIST(X) X(Present) X(Absent) X(Equals) X(AtLeast) X(AtMost) X(OneOf)
DPUF_DECLARE_ENUM(CapabilityOp, DPUF_CAPABILITY_OP_LIST)

#define DPUF_CAPABILITY_VALUE_LIST(X) X(Unknown) X(Flag) X(Integer) X(Text) X(Version)
DPUF_DECLARE_ENUM(CapabilityValueKind, DPUF_CAPABILITY_VALUE_LIST)

#define DPUF_INTENT_LIST(X) X(Deploy) X(Rollout) X(Scale) X(Replace) X(Withdraw) X(Rollback)
DPUF_DECLARE_ENUM(IntentKind, DPUF_INTENT_LIST)

#define DPUF_EFFECT_KIND_LIST(X) X(Deploy) X(Withdraw) X(Replace) X(Restart) X(Configure)
DPUF_DECLARE_ENUM(EffectKind, DPUF_EFFECT_KIND_LIST)

#define DPUF_EFFECT_STATUS_LIST(X) X(Succeeded) X(Partial) X(Failed) X(Refused)
DPUF_DECLARE_ENUM(EffectStatus, DPUF_EFFECT_STATUS_LIST)

#define DPUF_ATTEMPT_OUTCOME_LIST(X) \
  X(Open) X(Verified) X(Failed) X(Cancelled) X(Fenced) X(Superseded) X(Quiesced) X(Withdrawn)
DPUF_DECLARE_ENUM(AttemptOutcome, DPUF_ATTEMPT_OUTCOME_LIST)

#define DPUF_DECISION_KIND_LIST(X) \
  X(Placement) X(Authority) X(Eligibility) X(Dependency) X(Lifecycle) X(Rollback)  \
  X(Withdrawal) X(Ingest) X(Recovery)
DPUF_DECLARE_ENUM(DecisionKind, DPUF_DECISION_KIND_LIST)

#define DPUF_RECOVERY_LIST(X)                                                        \
  X(Fresh) X(CleanReopen) X(TornTailTruncated) X(EmptyStore) X(RefusedCorrupt)        \
  X(RefusedVersion) X(RefusedTruncated) X(RefusedIntegrity)
DPUF_DECLARE_ENUM(RecoveryClass, DPUF_RECOVERY_LIST)

// ---------------------------------------------------------------------------
// Checked arithmetic
// ---------------------------------------------------------------------------

template <class T>
[[nodiscard]] constexpr bool checked_add(T a, T b, T& out) noexcept {
  static_assert(std::is_integral_v<T>, "checked_add requires an integral type");
  if constexpr (std::is_unsigned_v<T>) {
    if (a > static_cast<T>(std::numeric_limits<T>::max() - b)) return false;
    out = static_cast<T>(a + b);
    return true;
  } else {
    if (b > 0 && a > static_cast<T>(std::numeric_limits<T>::max() - b)) return false;
    if (b < 0 && a < static_cast<T>(std::numeric_limits<T>::min() - b)) return false;
    out = static_cast<T>(a + b);
    return true;
  }
}

template <class T>
[[nodiscard]] constexpr bool checked_sub(T a, T b, T& out) noexcept {
  static_assert(std::is_integral_v<T>, "checked_sub requires an integral type");
  if constexpr (std::is_unsigned_v<T>) {
    if (a < b) return false;
    out = static_cast<T>(a - b);
    return true;
  } else {
    if (b < 0 && a > static_cast<T>(std::numeric_limits<T>::max() + b)) return false;
    if (b > 0 && a < static_cast<T>(std::numeric_limits<T>::min() + b)) return false;
    out = static_cast<T>(a - b);
    return true;
  }
}

template <class T>
[[nodiscard]] constexpr bool checked_mul(T a, T b, T& out) noexcept {
  static_assert(std::is_integral_v<T>, "checked_mul requires an integral type");
  if (a == 0 || b == 0) {
    out = 0;
    return true;
  }
  if constexpr (std::is_unsigned_v<T>) {
    if (a > static_cast<T>(std::numeric_limits<T>::max() / b)) return false;
    out = static_cast<T>(a * b);
    return true;
  } else {
    const T hi = std::numeric_limits<T>::max();
    const T lo = std::numeric_limits<T>::min();
    if (a > 0) {
      if (b > 0) {
        if (a > hi / b) return false;
      } else {
        if (b < lo / a) return false;
      }
    } else {
      if (b > 0) {
        if (a < lo / b) return false;
      } else {
        if (a < hi / b) return false;
      }
    }
    out = static_cast<T>(a * b);
    return true;
  }
}

/// Narrowing conversion that fails instead of truncating.
template <class T>
[[nodiscard]] constexpr bool checked_narrow(std::uint64_t value, T& out) noexcept {
  static_assert(std::is_integral_v<T>, "checked_narrow requires an integral type");
  if (value > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) return false;
  out = static_cast<T>(value);
  return true;
}

template <class T>
[[nodiscard]] constexpr T saturating_add(T a, T b) noexcept {
  T out{};
  return checked_add(a, b, out) ? out : std::numeric_limits<T>::max();
}

template <class T>
[[nodiscard]] constexpr T saturating_mul(T a, T b) noexcept {
  T out{};
  return checked_mul(a, b, out) ? out : std::numeric_limits<T>::max();
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

/// Logical instant in runtime ticks. Every freshness, staleness and ordering
/// decision is taken on this value, never on the wall clock, so that accepted
/// state is a pure function of the accepted event sequence.
struct LogicalInstant {
  std::uint64_t ticks{0};
  friend constexpr bool operator==(const LogicalInstant&, const LogicalInstant&) = default;
  friend constexpr auto operator<=>(const LogicalInstant&, const LogicalInstant&) = default;

  // visit() is a template member so that this header stays independent of the
  // archive layer; the archive is supplied at instantiation.
  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("LogicalInstant");
    field(ar, "ticks", ticks);
    ar.end_object();
  }
};

/// Wall-clock provenance. Recorded for operators, never used to decide anything.
struct WallClockMs {
  std::int64_t ms{0};
  friend constexpr bool operator==(const WallClockMs&, const WallClockMs&) = default;
  friend constexpr auto operator<=>(const WallClockMs&, const WallClockMs&) = default;

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("WallClockMs");
    field(ar, "ms", ms);
    ar.end_object();
  }
};

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

/// Validation for textual identities: 1..64 characters from [A-Za-z0-9._:-],
/// starting with an alphanumeric character. The restricted alphabet keeps
/// identities safe to use as file names, JSON keys and log fields without
/// further escaping.
[[nodiscard]] bool validate_identity_text(std::string_view text) noexcept;

template <class Tag>
class StrongId {
 public:
  static constexpr std::size_t kMaxLength = 64;

  StrongId() = default;

  [[nodiscard]] static Result<StrongId> parse(std::string_view text) {
    if (text.size() > kMaxLength) {
      return refuse(ReasonCode::OversizedInput, "identity text exceeds 64 characters");
    }
    if (!validate_identity_text(text)) {
      return refuse(ReasonCode::InvalidIdentity, std::string{text});
    }
    StrongId id;
    id.text_.assign(text);
    return id;
  }

  /// Test and tool helper for compile-time-known identities. Data-derived input
  /// must use parse(); this helper terminates on invalid text.
  [[nodiscard]] static StrongId literal(std::string_view text) {
    Result<StrongId> parsed = parse(text);
    if (!parsed) {
      std::terminate();
    }
    return std::move(parsed).value();
  }

  [[nodiscard]] bool valid() const noexcept { return !text_.empty(); }
  [[nodiscard]] const std::string& str() const noexcept { return text_; }
  [[nodiscard]] std::string_view view() const noexcept { return text_; }

  friend bool operator==(const StrongId&, const StrongId&) = default;
  friend std::strong_ordering operator<=>(const StrongId& lhs, const StrongId& rhs) {
    return lhs.text_ <=> rhs.text_;
  }

  [[nodiscard]] std::size_t hash() const noexcept { return std::hash<std::string>{}(text_); }

 private:
  std::string text_{};
};

// ---------------------------------------------------------------------------
// Monotonic counters (generations, epochs, incarnations, indices)
// ---------------------------------------------------------------------------

/// A monotonic counter. Regressions are rejected at every boundary that accepts
/// externally derived generations.
template <class Tag>
class Counter {
 public:
  using rep = std::uint64_t;

  constexpr Counter() = default;
  constexpr explicit Counter(rep value) noexcept : value_(value) {}

  [[nodiscard]] constexpr rep value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }

  [[nodiscard]] constexpr Result<Counter> next() const {
    rep next_value = 0;
    if (!checked_add(value_, rep{1}, next_value)) {
      return refuse(ReasonCode::ArithmeticOverflow, "counter exhausted");
    }
    return Counter{next_value};
  }

  [[nodiscard]] constexpr Counter saturating_next() const noexcept {
    return Counter{saturating_add(value_, rep{1})};
  }

  friend constexpr bool operator==(Counter, Counter) = default;
  friend constexpr auto operator<=>(Counter, Counter) = default;

  [[nodiscard]] constexpr std::size_t hash() const noexcept {
    return static_cast<std::size_t>(value_ * 1099511628211ull);
  }

 private:
  rep value_{0};
};

// ---------------------------------------------------------------------------
// Identity tags
// ---------------------------------------------------------------------------

using DpuId = StrongId<struct DpuIdTag>;
using ServiceId = StrongId<struct ServiceIdTag>;
using ServiceGroupId = StrongId<struct ServiceGroupIdTag>;
using InstanceId = StrongId<struct InstanceIdTag>;
using DependencyId = StrongId<struct DependencyIdTag>;
using HostAttachmentId = StrongId<struct HostAttachmentIdTag>;
using IsolationDomainId = StrongId<struct IsolationDomainIdTag>;
using EvidenceId = StrongId<struct EvidenceIdTag>;
using PlanId = StrongId<struct PlanIdTag>;
using OriginId = StrongId<struct OriginIdTag>;
using StoreId = StrongId<struct StoreIdTag>;
using CapabilityKey = StrongId<struct CapabilityKeyTag>;
using ExclusiveScopeId = StrongId<struct ExclusiveScopeIdTag>;
using OperationId = StrongId<struct OperationIdTag>;
using EventId = StrongId<struct EventIdTag>;

using TopologyGeneration = Counter<struct TopologyGenerationTag>;
using CapabilityGeneration = Counter<struct CapabilityGenerationTag>;
using PolicyGeneration = Counter<struct PolicyGenerationTag>;
using DeploymentGeneration = Counter<struct DeploymentGenerationTag>;
using CoordinatorEpoch = Counter<struct CoordinatorEpochTag>;
using BootIncarnation = Counter<struct BootIncarnationTag>;
using AttemptNumber = Counter<struct AttemptNumberTag>;
using ReplicaIndex = Counter<struct ReplicaIndexTag>;
using OriginEpoch = Counter<struct OriginEpochTag>;
using Sequence = Counter<struct SequenceTag>;
using Incarnation = Counter<struct InstanceIncarnationTag>;

// ---------------------------------------------------------------------------
// Versions
// ---------------------------------------------------------------------------

/// Semantic version with an optional pre-release component. Release versions
/// order after pre-release versions of the same triple.
class ServiceVersion {
 public:
  static constexpr std::size_t kMaxPrerelease = 32;

  ServiceVersion() = default;
  constexpr ServiceVersion(std::uint32_t major, std::uint32_t minor, std::uint32_t patch,
                           std::string prerelease = {})
      : major_(major), minor_(minor), patch_(patch), prerelease_(std::move(prerelease)) {}

  [[nodiscard]] static Result<ServiceVersion> parse(std::string_view text);
  [[nodiscard]] std::string to_string() const;
  [[nodiscard]] bool valid() const noexcept { return major_ != 0 || minor_ != 0 || patch_ != 0; }

  [[nodiscard]] constexpr std::uint32_t major() const noexcept { return major_; }
  [[nodiscard]] constexpr std::uint32_t minor() const noexcept { return minor_; }
  [[nodiscard]] constexpr std::uint32_t patch() const noexcept { return patch_; }
  [[nodiscard]] const std::string& prerelease() const noexcept { return prerelease_; }

  friend bool operator==(const ServiceVersion&, const ServiceVersion&) = default;
  friend std::strong_ordering operator<=>(const ServiceVersion& lhs, const ServiceVersion& rhs);

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("ServiceVersion");
    field(ar, "major", major_);
    field(ar, "minor", minor_);
    field(ar, "patch", patch_);
    field(ar, "prerelease", prerelease_);
    ar.end_object();
  }

 private:
  std::uint32_t major_{0};
  std::uint32_t minor_{0};
  std::uint32_t patch_{0};
  std::string prerelease_{};
};

/// Minimum firmware API level a DPU must expose for a service to be compatible.
struct FirmwareApiLevel {
  std::uint32_t major{0};
  std::uint32_t minor{0};
  friend constexpr bool operator==(const FirmwareApiLevel&, const FirmwareApiLevel&) = default;
  friend constexpr auto operator<=>(const FirmwareApiLevel&, const FirmwareApiLevel&) = default;
  [[nodiscard]] std::string to_string() const;

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("FirmwareApiLevel");
    field(ar, "major", major);
    field(ar, "minor", minor);
    ar.end_object();
  }
};

}  // namespace dpu::fabric
