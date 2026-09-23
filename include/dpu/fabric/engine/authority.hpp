#pragma once

// Authority: exclusive scopes and fence tokens.
//
// Two rules are enforced here and nowhere else:
//
//  * A DPU has at most one exclusive owner. Two exclusive scopes can never both
//    hold authority over the same DPU, so a service can never double-own the
//    resource it claims to own alone.
//  * A fence token is valid only for the coordinator epoch and boot incarnation
//    it was issued under. Advancing the epoch or restarting the process
//    invalidates every token issued before, which is what stops a pre-restart
//    attempt from publishing an effect after recovery.

#include <cstdint>
#include <vector>

#include "dpu/fabric/core/archive.hpp"
#include "dpu/fabric/core/result.hpp"
#include "dpu/fabric/core/types.hpp"
#include "dpu/fabric/model/lifecycle.hpp"

namespace dpu::fabric {

/// One exclusive grant over one DPU.
struct AuthorityGrant {
  DpuId dpu{};
  ExclusiveScopeId scope{};
  ServiceGroupId group{};
  CoordinatorEpoch epoch{};
  BootIncarnation boot{};
  LogicalInstant granted_at{};

  friend bool operator==(const AuthorityGrant&, const AuthorityGrant&) = default;

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("AuthorityGrant");
    field(ar, "dpu", dpu);
    field(ar, "scope", scope);
    field(ar, "group", group);
    field(ar, "epoch", epoch);
    field(ar, "boot", boot);
    field(ar, "granted_at", granted_at);
    ar.end_object();
  }
};

/// Registry of exclusive scope grants. Grants are kept sorted by DPU identity
/// and bounded by the configured DPU bound.
class AuthorityRegistry {
 public:
  [[nodiscard]] Status claim(const ExclusiveScopeId& scope, const ServiceGroupId& group,
                             const DpuId& dpu, CoordinatorEpoch epoch, BootIncarnation boot,
                             LogicalInstant at, std::size_t max_grants);

  /// Releases one grant. Returns whether a grant was actually released.
  bool release(const ExclusiveScopeId& scope, const DpuId& dpu);
  /// Releases every grant held by a group; returns the number released.
  std::size_t release_group(const ServiceGroupId& group);
  /// Drops every grant: used when the coordinator epoch advances, because
  /// authority does not survive a coordinator change.
  std::size_t fence_all();

  [[nodiscard]] const AuthorityGrant* owner_of(const DpuId& dpu) const noexcept;
  [[nodiscard]] bool owned_by_other(const DpuId& dpu, const ExclusiveScopeId& scope) const noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return grants_.size(); }
  [[nodiscard]] const std::vector<AuthorityGrant>& grants() const noexcept { return grants_; }

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("AuthorityRegistry");
    field(ar, "grants", grants_);
    ar.end_object();
  }

 private:
  std::vector<AuthorityGrant> grants_{};
};

/// Issues and validates fence tokens.
class FenceIssuer {
 public:
  void reset(CoordinatorEpoch epoch, BootIncarnation boot);

  /// Advances the coordinator epoch and drops all previously issued tokens.
  Status advance_epoch(CoordinatorEpoch epoch, BootIncarnation boot);
  /// Advances the boot incarnation; every outstanding token becomes invalid.
  Status advance_boot(BootIncarnation boot);

  [[nodiscard]] Result<FenceToken> issue(const InstanceId& instance, LogicalInstant at);
  /// Invalidates the current token for an instance without issuing a new one.
  void invalidate(const InstanceId& instance);

  /// A token is valid only for the instance it was issued to, in the current
  /// epoch and boot incarnation, at the instance's current serial.
  [[nodiscard]] bool validate(const InstanceId& instance, const FenceToken& token) const noexcept;
  /// Current token for an instance; an invalid token when none was issued.
  [[nodiscard]] FenceToken current(const InstanceId& instance) const noexcept;

  [[nodiscard]] CoordinatorEpoch epoch() const noexcept { return epoch_; }
  [[nodiscard]] BootIncarnation boot() const noexcept { return boot_; }

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("FenceIssuer");
    field(ar, "epoch", epoch_);
    field(ar, "boot", boot_);
    field(ar, "serials", serials_);
    ar.end_object();
  }

 private:
  struct Entry {
    InstanceId instance{};
    std::uint64_t serial{0};

    template <class Ar>
    void visit(Ar& ar) {
      ar.begin_object("FenceEntry");
      field(ar, "instance", instance);
      field(ar, "serial", serial);
      ar.end_object();
    }
  };

  Entry* find(const InstanceId& instance) noexcept;
  const Entry* find(const InstanceId& instance) const noexcept;

  std::vector<Entry> serials_{};
  CoordinatorEpoch epoch_{};
  BootIncarnation boot_{};
};

}  // namespace dpu::fabric
