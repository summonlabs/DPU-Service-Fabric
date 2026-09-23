// Unit tests for the typed model: identities, versions, capabilities,
// canonical ordering, plan sealing and event identity.

#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#include "dpu/fabric/dpu_fabric.hpp"
#include "support/harness.hpp"
#include "support/scenario.hpp"

namespace dpu::fabric {
namespace {

DPUF_TEST(identity, parse_refuses_invalid_text) {
  DPUF_CHECK(DpuId::parse("dpu-1").has_value());
  DPUF_CHECK(DpuId::parse("D.1_2:3-4").has_value());
  DPUF_CHECK(!DpuId::parse("").has_value());
  DPUF_CHECK_CODE(DpuId::parse("").status(), ReasonCode::InvalidIdentity);
  DPUF_CHECK(!DpuId::parse("-leading").has_value());
  DPUF_CHECK(!DpuId::parse("has space").has_value());
  DPUF_CHECK(!DpuId::parse("slash/bad").has_value());
  DPUF_CHECK(!DpuId::parse(std::string(65, 'a')).has_value());
  DPUF_CHECK_CODE(DpuId::parse(std::string(65, 'a')).status(), ReasonCode::OversizedInput);
  const Result<DpuId> ok = DpuId::parse(std::string(64, 'a'));
  DPUF_CHECK(ok.has_value());
  DPUF_CHECK(DpuId{} == DpuId{});
  DPUF_CHECK(!DpuId{}.valid());
  DPUF_CHECK(DpuId::literal("a") < DpuId::literal("b"));
  const DpuId named = DpuId::literal("dpu-1");
  DPUF_CHECK_EQ(named.str(), std::string{"dpu-1"});
  // Distinct identity types are distinct types: this would not compile if they
  // shared a representation.
  static_assert(!std::is_same_v<DpuId, ServiceId>);
  static_assert(!std::is_same_v<TopologyGeneration, CapabilityGeneration>);
}

DPUF_TEST(counter, regressions_and_exhaustion_are_explicit) {
  TopologyGeneration generation{5};
  DPUF_CHECK_EQ(generation.value(), std::uint64_t{5});
  DPUF_CHECK(generation.next().has_value());
  DPUF_CHECK_EQ(generation.next().value().value(), std::uint64_t{6});
  TopologyGeneration exhausted{std::numeric_limits<std::uint64_t>::max()};
  DPUF_CHECK(!exhausted.next().has_value());
  DPUF_CHECK_CODE(exhausted.next().status(), ReasonCode::ArithmeticOverflow);
  DPUF_CHECK_EQ(exhausted.saturating_next().value(), std::numeric_limits<std::uint64_t>::max());
  DPUF_CHECK(TopologyGeneration{1} < TopologyGeneration{2});
}

DPUF_TEST(version, parse_and_order) {
  const Result<ServiceVersion> parsed = ServiceVersion::parse("1.2.3");
  DPUF_CHECK(parsed.has_value());
  DPUF_CHECK_EQ(parsed.value().major(), std::uint32_t{1});
  DPUF_CHECK_EQ(parsed.value().minor(), std::uint32_t{2});
  DPUF_CHECK_EQ(parsed.value().patch(), std::uint32_t{3});
  DPUF_CHECK_EQ(parsed.value().to_string(), std::string{"1.2.3"});
  const Result<ServiceVersion> pre = ServiceVersion::parse("1.2.3-rc.1");
  DPUF_CHECK(pre.has_value());
  DPUF_CHECK_EQ(pre.value().to_string(), std::string{"1.2.3-rc.1"});
  DPUF_CHECK(pre.value() < parsed.value());
  DPUF_CHECK((ServiceVersion{1, 2, 3} < ServiceVersion{1, 2, 4}));
  DPUF_CHECK((ServiceVersion{1, 2, 3} < ServiceVersion{2, 0, 0}));
  DPUF_CHECK(!ServiceVersion::parse("1.2").has_value());
  DPUF_CHECK(!ServiceVersion::parse("1.2.3.4").has_value());
  DPUF_CHECK(!ServiceVersion::parse("a.b.c").has_value());
  DPUF_CHECK(!ServiceVersion::parse("1.2.3-").has_value());
  DPUF_CHECK(!ServiceVersion::parse(std::string(80, '1')).has_value());
  DPUF_CHECK(!ServiceVersion::parse("99999999999.0.0").has_value());
  DPUF_CHECK(!(ServiceVersion{}.valid()));
}

DPUF_TEST(capability, sets_are_canonical_and_refuse_ambiguity) {
  CapabilitySet set;
  set.entries.push_back(test::make_capability("b.key", test::flag_value(true),
                                              CapabilityGeneration{1}));
  set.entries.push_back(test::make_capability("a.key", test::integer_value(7),
                                              CapabilityGeneration{1}));
  DPUF_CHECK_OK(set.normalize(16));
  DPUF_CHECK_EQ(set.entries[0].key.str(), std::string{"a.key"});
  DPUF_CHECK_EQ(set.entries[1].key.str(), std::string{"b.key"});
  DPUF_CHECK(set.find(test::capability_key("a.key")) != nullptr);
  DPUF_CHECK(set.find(test::capability_key("zz")) == nullptr);

  CapabilitySet duplicate;
  duplicate.entries.push_back(test::make_capability("a", test::flag_value(true),
                                                    CapabilityGeneration{1}));
  duplicate.entries.push_back(test::make_capability("a", test::flag_value(false),
                                                    CapabilityGeneration{2}));
  DPUF_CHECK_CODE(duplicate.normalize(16), ReasonCode::DuplicateIdentity);

  CapabilitySet oversized;
  for (std::size_t i = 0; i < 5; ++i) {
    oversized.entries.push_back(test::make_capability("k" + std::to_string(i),
                                                      test::flag_value(true),
                                                      CapabilityGeneration{1}));
  }
  DPUF_CHECK_CODE(oversized.normalize(4), ReasonCode::BoundExceeded);

  // A capability entry with no value is not an observation.
  CapabilitySet unknown;
  Capability entry;
  entry.key = test::capability_key("odd");
  entry.value.kind = CapabilityValueKind::Unknown;
  entry.generation = CapabilityGeneration{1};
  unknown.entries.push_back(entry);
  DPUF_CHECK_CODE(unknown.normalize(4), ReasonCode::CapabilityUnknown);
}

DPUF_TEST(capability, value_text_is_canonical) {
  DPUF_CHECK_EQ(test::flag_value(true).to_string(), std::string{"true"});
  DPUF_CHECK_EQ(test::flag_value(false).to_string(), std::string{"false"});
  DPUF_CHECK_EQ(test::integer_value(-3).to_string(), std::string{"-3"});
  DPUF_CHECK_EQ(test::version_value(1, 2, 3).to_string(), std::string{"1.2.3"});
  CapabilityValue text;
  text.kind = CapabilityValueKind::Text;
  text.text = "x";
  DPUF_CHECK_EQ(text.to_string(), std::string{"x"});
  CapabilityValue unknown;
  DPUF_CHECK_EQ(unknown.to_string(), std::string{"unknown"});
}

DPUF_TEST(plan, sealing_is_deterministic_and_content_addressed) {
  DeploymentPlan plan;
  plan.generation = DeploymentGeneration{3};
  plan.group = test::group_id("grp-1");
  plan.kind = IntentKind::Deploy;
  plan.created_at = LogicalInstant{7};
  ServicePlan service_plan;
  service_plan.service = test::service_id("svc-a");
  service_plan.version = ServiceVersion{1, 0, 0};
  service_plan.desired_replicas = 1;
  service_plan.placed_replicas = 1;
  PlannedReplica replica;
  replica.index = ReplicaIndex{0};
  replica.dpu = test::dpu_id("dpu-1");
  replica.attachment = HostAttachmentId::literal("att-dpu-1");
  replica.domain = IsolationDomainId::literal("dom-a");
  replica.score = 42;
  service_plan.replicas.push_back(replica);
  plan.services.push_back(service_plan);
  plan.seal();
  const Digest first = plan.digest;
  const std::string first_id = plan.id.str();
  DPUF_CHECK(!first.is_zero());
  DPUF_CHECK_EQ(first_id.substr(0, 5), std::string{"plan-"});
  plan.seal();
  DPUF_CHECK_EQ(plan.digest, first);
  DPUF_CHECK_EQ(plan.id.str(), first_id);
  // A different placement produces a different identity.
  plan.services[0].replicas[0].score = 43;
  plan.seal();
  DPUF_CHECK_NE(plan.digest, first);
  DPUF_CHECK_NE(plan.id.str(), first_id);
  // Round-trip preserves the sealed identity.
  const Result<DeploymentPlan> decoded = decode_binary<DeploymentPlan>(encode_binary(plan));
  DPUF_CHECK(decoded.has_value());
  DPUF_CHECK_EQ(canonical_digest(decoded.value()), canonical_digest(plan));
}

DPUF_TEST(event, total_order_and_digest_are_content_derived) {
  FabricEvent earlier =
      test::make_event("ev-a", "origin-1", 1, 5, IntentSubmitted{test::standard_intent(5)});
  FabricEvent later =
      test::make_event("ev-b", "origin-1", 2, 6, IntentSubmitted{test::standard_intent(6)});
  FabricEvent same_time_other_origin =
      test::make_event("ev-c", "origin-0", 1, 6, IntentSubmitted{test::standard_intent(6)});
  DPUF_CHECK(event_key_less(event_key(earlier), event_key(later)));
  DPUF_CHECK(!event_key_less(event_key(later), event_key(earlier)));
  DPUF_CHECK(event_key_less(event_key(same_time_other_origin), event_key(later)));
  DPUF_CHECK(!event_key_less(event_key(later), event_key(later)));
  DPUF_CHECK_EQ(event_digest(earlier), event_digest(earlier));
  DPUF_CHECK_NE(event_digest(earlier), event_digest(later));
  DPUF_CHECK_EQ(std::string{event_kind_name(earlier)}, std::string{"IntentSubmitted"});
  const Result<FabricEvent> decoded = decode_binary<FabricEvent>(encode_binary(earlier));
  DPUF_CHECK(decoded.has_value());
  DPUF_CHECK_EQ(event_digest(decoded.value()), event_digest(earlier));
}

DPUF_TEST(topology, snapshot_lookup_and_profile_support) {
  const std::vector<DpuRecord> dpus =
      test::standard_dpus(1, 100, CapabilityGeneration{1}, TopologyGeneration{1});
  TopologySnapshot snapshot;
  snapshot.generation = TopologyGeneration{1};
  snapshot.dpus = dpus;
  DPUF_CHECK(snapshot.find(test::dpu_id("dpu-2")) != nullptr);
  DPUF_CHECK(snapshot.find(test::dpu_id("nope")) == nullptr);
  DPUF_CHECK(snapshot.dpus[0].profile.supports(IsolationKind::Process));
  DPUF_CHECK(!snapshot.dpus[0].profile.supports(IsolationKind::Virtualization));
  DPUF_CHECK(snapshot.dpus[0].attached());
}

DPUF_TEST(evidence, freshness_is_a_logical_instant_question) {
  const EvidenceRef evidence = test::make_evidence("ev-1", EvidenceKind::Capability,
                                                   LogicalInstant{10}, 5);
  DPUF_CHECK(evidence.present());
  DPUF_CHECK(evidence.fresh_at(LogicalInstant{10}));
  DPUF_CHECK(evidence.fresh_at(LogicalInstant{15}));
  DPUF_CHECK(!evidence.fresh_at(LogicalInstant{16}));
  DPUF_CHECK(!evidence.fresh_at(LogicalInstant{9}));
  DPUF_CHECK(!EvidenceRef{}.present());
}

DPUF_TEST(instance, serving_requires_verified_effect_and_fresh_health) {
  InstanceState instance;
  DPUF_CHECK(!instance.serving());
  instance.state = LifecycleState::Verified;
  instance.health = HealthState::Healthy;
  instance.health_fresh = false;
  DPUF_CHECK(!instance.serving());
  instance.health_fresh = true;
  DPUF_CHECK(instance.serving());
  instance.health = HealthState::Degraded;
  DPUF_CHECK(!instance.serving());
  instance.health = HealthState::Healthy;
  instance.state = LifecycleState::Unverified;
  DPUF_CHECK(!instance.serving());
}

DPUF_TEST(fence, validity_requires_epoch_boot_and_serial) {
  FenceIssuer issuer;
  issuer.reset(CoordinatorEpoch{2}, BootIncarnation{3});
  const InstanceId instance = test::instance_id("i-1");
  const Result<FenceToken> token = issuer.issue(instance, LogicalInstant{1});
  DPUF_CHECK(token.has_value());
  DPUF_CHECK_EQ(token.value().epoch.value(), std::uint64_t{2});
  DPUF_CHECK_EQ(token.value().boot.value(), std::uint64_t{3});
  DPUF_CHECK(issuer.validate(instance, token.value()));
  DPUF_CHECK(!issuer.validate(test::instance_id("i-2"), token.value()));
  DPUF_CHECK(!issuer.validate(instance, FenceToken{}));
  FenceToken wrong_epoch = token.value();
  wrong_epoch.epoch = CoordinatorEpoch{1};
  DPUF_CHECK(!issuer.validate(instance, wrong_epoch));
  FenceToken wrong_boot = token.value();
  wrong_boot.boot = BootIncarnation{1};
  DPUF_CHECK(!issuer.validate(instance, wrong_boot));
  issuer.invalidate(instance);
  DPUF_CHECK(!issuer.validate(instance, token.value()));
  const Result<FenceToken> next = issuer.issue(instance, LogicalInstant{2});
  DPUF_CHECK(next.has_value());
  // Serial numbers are strictly monotonic across invalidation and reissue, so a
  // fenced token can never be reissued with its old value.
  DPUF_CHECK(next.value().serial > token.value().serial);
  DPUF_CHECK(issuer.validate(instance, next.value()));
}

DPUF_TEST(authority, one_exclusive_owner_per_device) {
  AuthorityRegistry registry;
  const ExclusiveScopeId scope_a = test::scope_id("scope-a");
  const ExclusiveScopeId scope_b = test::scope_id("scope-b");
  const DpuId dpu = test::dpu_id("dpu-1");
  DPUF_CHECK_OK(registry.claim(scope_a, test::group_id("grp-1"), dpu, CoordinatorEpoch{1},
                               BootIncarnation{1}, LogicalInstant{1}, 16));
  // The same scope re-claiming is idempotent.
  DPUF_CHECK_OK(registry.claim(scope_a, test::group_id("grp-1"), dpu, CoordinatorEpoch{1},
                               BootIncarnation{1}, LogicalInstant{2}, 16));
  DPUF_CHECK_EQ(registry.size(), std::size_t{1});
  // A second exclusive scope cannot own the same device.
  DPUF_CHECK_CODE(registry.claim(scope_b, test::group_id("grp-2"), dpu, CoordinatorEpoch{1},
                                 BootIncarnation{1}, LogicalInstant{2}, 16),
                  ReasonCode::ExclusiveScopeConflict);
  DPUF_CHECK_EQ(registry.size(), std::size_t{1});
  DPUF_CHECK(registry.owned_by_other(dpu, scope_b));
  DPUF_CHECK(!registry.owned_by_other(dpu, scope_a));
  DPUF_CHECK(registry.owner_of(dpu) != nullptr);
  DPUF_CHECK_EQ(registry.owner_of(dpu)->scope.str(), std::string{"scope-a"});
  DPUF_CHECK(!registry.release(scope_b, dpu));
  DPUF_CHECK(registry.release(scope_a, dpu));
  DPUF_CHECK(registry.owner_of(dpu) == nullptr);
  DPUF_CHECK_EQ(registry.release_group(test::group_id("grp-1")), std::size_t{0});
}

DPUF_TEST(authority, grant_table_is_bounded) {
  AuthorityRegistry registry;
  for (std::size_t i = 0; i < 4; ++i) {
    const DpuId dpu = DpuId::literal("dpu-" + std::to_string(i));
    DPUF_CHECK_OK(registry.claim(test::scope_id("scope"), test::group_id("grp"),
                                 dpu, CoordinatorEpoch{1}, BootIncarnation{1}, LogicalInstant{1}, 4));
  }
  DPUF_CHECK_CODE(registry.claim(test::scope_id("scope"), test::group_id("grp"),
                                 test::dpu_id("dpu-9"), CoordinatorEpoch{1}, BootIncarnation{1},
                                 LogicalInstant{1}, 4),
                  ReasonCode::BoundExceeded);
  DPUF_CHECK_EQ(registry.fence_all(), std::size_t{4});
  DPUF_CHECK_EQ(registry.size(), std::size_t{0});
}

}  // namespace
}  // namespace dpu::fabric
