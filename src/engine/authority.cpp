#include "dpu/fabric/engine/authority.hpp"

#include <algorithm>

namespace dpu::fabric {

// ---------------------------------------------------------------------------
// AuthorityRegistry
// ---------------------------------------------------------------------------

const AuthorityGrant* AuthorityRegistry::owner_of(const DpuId& dpu) const noexcept {
  const auto it = std::lower_bound(
      grants_.begin(), grants_.end(), dpu,
      [](const AuthorityGrant& grant, const DpuId& probe) { return grant.dpu < probe; });
  if (it == grants_.end() || !(it->dpu == dpu)) return nullptr;
  return &*it;
}

bool AuthorityRegistry::owned_by_other(const DpuId& dpu, const ExclusiveScopeId& scope) const noexcept {
  const AuthorityGrant* grant = owner_of(dpu);
  return grant != nullptr && !(grant->scope == scope);
}

Status AuthorityRegistry::claim(const ExclusiveScopeId& scope, const ServiceGroupId& group,
                                const DpuId& dpu, CoordinatorEpoch epoch, BootIncarnation boot,
                                LogicalInstant at, std::size_t max_grants) {
  if (!scope.valid() || !dpu.valid() || !group.valid()) {
    return refuse(ReasonCode::InvalidIdentity, "authority grant identity");
  }
  const auto it = std::lower_bound(
      grants_.begin(), grants_.end(), dpu,
      [](const AuthorityGrant& grant, const DpuId& probe) { return grant.dpu < probe; });
  if (it != grants_.end() && it->dpu == dpu) {
    if (it->scope == scope) {
      // Idempotent re-claim by the same scope; refresh the generation.
      it->epoch = epoch;
      it->boot = boot;
      it->granted_at = at;
      return Status::success();
    }
    // A DPU has exactly one exclusive owner. This is the boundary that stops two
    // exclusive scopes from double-owning the same device.
    return refuse(ReasonCode::ExclusiveScopeConflict,
                  it->dpu.str() + " owned by scope " + it->scope.str() + " of group " +
                      it->group.str());
  }
  if (grants_.size() >= max_grants) {
    return refuse(ReasonCode::BoundExceeded, "authority grant table full");
  }
  AuthorityGrant grant;
  grant.dpu = dpu;
  grant.scope = scope;
  grant.group = group;
  grant.epoch = epoch;
  grant.boot = boot;
  grant.granted_at = at;
  grants_.insert(it, std::move(grant));
  return Status::success();
}

bool AuthorityRegistry::release(const ExclusiveScopeId& scope, const DpuId& dpu) {
  const auto it = std::lower_bound(
      grants_.begin(), grants_.end(), dpu,
      [](const AuthorityGrant& grant, const DpuId& probe) { return grant.dpu < probe; });
  if (it == grants_.end() || !(it->dpu == dpu) || !(it->scope == scope)) return false;
  grants_.erase(it);
  return true;
}

std::size_t AuthorityRegistry::release_group(const ServiceGroupId& group) {
  const std::size_t before = grants_.size();
  grants_.erase(std::remove_if(grants_.begin(), grants_.end(),
                               [&group](const AuthorityGrant& grant) {
                                 return grant.group == group;
                               }),
                grants_.end());
  return before - grants_.size();
}

std::size_t AuthorityRegistry::fence_all() {
  const std::size_t removed = grants_.size();
  grants_.clear();
  return removed;
}

// ---------------------------------------------------------------------------
// FenceIssuer
// ---------------------------------------------------------------------------

void FenceIssuer::reset(CoordinatorEpoch epoch, BootIncarnation boot) {
  epoch_ = epoch;
  boot_ = boot;
  serials_.clear();
}

Status FenceIssuer::advance_epoch(CoordinatorEpoch epoch, BootIncarnation boot) {
  if (epoch < epoch_) {
    return refuse(ReasonCode::EpochRegressed, "coordinator epoch cannot regress");
  }
  epoch_ = epoch;
  boot_ = boot;
  // A coordinator change fences everything that came before it.
  serials_.clear();
  return Status::success();
}

Status FenceIssuer::advance_boot(BootIncarnation boot) {
  if (boot < boot_) {
    return refuse(ReasonCode::GenerationRegressed, "boot incarnation cannot regress");
  }
  boot_ = boot;
  serials_.clear();
  return Status::success();
}

FenceIssuer::Entry* FenceIssuer::find(const InstanceId& instance) noexcept {
  for (Entry& entry : serials_) {
    if (entry.instance == instance) return &entry;
  }
  return nullptr;
}

const FenceIssuer::Entry* FenceIssuer::find(const InstanceId& instance) const noexcept {
  for (const Entry& entry : serials_) {
    if (entry.instance == instance) return &entry;
  }
  return nullptr;
}

Result<FenceToken> FenceIssuer::issue(const InstanceId& instance, LogicalInstant) {
  if (!instance.valid()) return refuse(ReasonCode::InvalidIdentity, "fence target");
  if (!epoch_.valid() || !boot_.valid()) {
    return refuse(ReasonCode::AuthorityNotGranted, "fence issuer has no epoch or boot");
  }
  Entry* entry = find(instance);
  if (entry == nullptr) {
    Entry created;
    created.instance = instance;
    created.serial = 1;
    serials_.push_back(std::move(created));
    entry = &serials_.back();
  } else {
    std::uint64_t next_serial = 0;
    if (!checked_add(entry->serial, std::uint64_t{1}, next_serial)) {
      return refuse(ReasonCode::ArithmeticOverflow, "fence serial exhausted");
    }
    entry->serial = next_serial;
  }
  FenceToken token;
  token.epoch = epoch_;
  token.boot = boot_;
  token.serial = entry->serial;
  return token;
}

void FenceIssuer::invalidate(const InstanceId& instance) {
  Entry* entry = find(instance);
  if (entry == nullptr) return;
  // Bumping the serial makes every previously issued token for this instance
  // stale without issuing a replacement.
  entry->serial = saturating_add(entry->serial, std::uint64_t{1});
}

bool FenceIssuer::validate(const InstanceId& instance, const FenceToken& token) const noexcept {
  if (!token.valid()) return false;
  if (!token.from(epoch_, boot_)) return false;
  const Entry* entry = find(instance);
  if (entry == nullptr) return false;
  return entry->serial == token.serial;
}

FenceToken FenceIssuer::current(const InstanceId& instance) const noexcept {
  const Entry* entry = find(instance);
  if (entry == nullptr) return FenceToken{};
  FenceToken token;
  token.epoch = epoch_;
  token.boot = boot_;
  token.serial = entry->serial;
  return token;
}

}  // namespace dpu::fabric
