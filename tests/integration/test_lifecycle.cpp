// Integration tests for the lifecycle proof obligations: submission is not
// completion, acknowledgement is not a verified effect, stale attempts are
// fenced, loss is never healthy continuity, and replica accounting is exact.

#include <algorithm>
#include <string>
#include <vector>

#include "dpu/fabric/dpu_fabric.hpp"
#include "support/harness.hpp"
#include "support/scenario.hpp"

namespace dpu::fabric {
namespace {

using test::Scenario;

/// The current attempt of an instance, or a failure if there is none.
struct AttemptView {
  InstanceId instance{};
  AttemptNumber number{};
  FenceToken fence{};
  LifecycleState state{LifecycleState::Absent};
};

AttemptView attempt_of(Scenario& scenario, const InstanceId& id) {
  AttemptView view;
  const std::optional<InstanceState> state = scenario.rt().instance(id);
  if (!state.has_value()) {
    test::report_failure(__FILE__, __LINE__, "instance not found: " + id.str());
    return view;
  }
  view.instance = id;
  view.state = state->state;
  if (state->current_attempt() != nullptr) {
    view.number = state->current_attempt()->number;
    view.fence = state->current_attempt()->fence;
  }
  return view;
}

/// Submits an intent and returns the instances the plan created.
std::vector<InstanceId> deploy(Scenario& scenario, std::uint32_t replicas = 2) {
  const Result<BatchReport> report =
      scenario.send(IntentSubmitted{test::standard_intent(scenario.now(), replicas)});
  if (!report) {
    test::report_failure(__FILE__, __LINE__, "intent refused: " + report.status().message());
    return {};
  }
  if (report.value().applied != 1) {
    test::report_failure(__FILE__, __LINE__,
                         "intent was not applied: " +
                             std::string{to_string(report.value().outcomes.empty()
                                                      ? ReasonCode::MalformedInput
                                                      : report.value().outcomes[0].code)});
  }
  std::vector<InstanceId> ids;
  for (const InstanceState& instance : scenario.rt().snapshot().instances) {
    if (instance.service == test::service_id("svc-a")) ids.push_back(instance.id);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

FabricEvent acknowledge_event(Scenario& scenario, const AttemptView& view) {
  ExecutionAcknowledged acknowledged;
  acknowledged.instance = view.instance;
  acknowledged.attempt = view.number;
  acknowledged.fence = view.fence;
  acknowledged.acknowledged_at = LogicalInstant{scenario.now() + 1};
  return scenario.make(acknowledged, scenario.now() + 1);
}

FabricEvent effect_event(Scenario& scenario, const AttemptView& view, EffectStatus status,
                         std::uint64_t observed, std::uint64_t valid_until,
                         EffectKind kind = EffectKind::Deploy) {
  EffectReported reported;
  reported.report.operation = OperationId::literal("op-effect");
  reported.report.instance = view.instance;
  reported.report.attempt = view.number;
  reported.report.fence = view.fence;
  reported.report.kind = kind;
  reported.report.status = status;
  reported.report.reported_at = LogicalInstant{observed};
  reported.report.evidence =
      test::make_evidence("ev-effect-" + view.instance.str() + "-" + std::to_string(observed),
                          EvidenceKind::Execution, LogicalInstant{observed}, valid_until - observed,
                          EvidenceClass::Real);
  reported.report.effect_digest = sha256(view.instance.str() + std::to_string(observed));
  return scenario.make(reported, observed);
}

DPUF_TEST(lifecycle, submission_is_not_completion) {
  Scenario scenario;
  DPUF_CHECK_RESULT_OK(test::apply_standard_world(scenario));
  const std::vector<InstanceId> ids = deploy(scenario, 2);
  DPUF_CHECK_EQ(ids.size(), std::size_t{2});
  // Accepting a plan authorizes attempts; it does not run anything.
  for (const InstanceId& id : ids) {
    const std::optional<InstanceState> state = scenario.rt().instance(id);
    DPUF_CHECK(state.has_value());
    DPUF_CHECK_EQ(state->state, LifecycleState::Authorized);
    DPUF_CHECK(state->current_attempt() != nullptr);
    DPUF_CHECK(state->current_attempt()->open());
    DPUF_CHECK(state->current_attempt()->fence.valid());
    DPUF_CHECK(!state->serving());
    DPUF_CHECK(state->verified_digest.is_zero());
  }
  // Replica accounting is exact: two replicas were asked for, two instances
  // exist, each on its own device.
  DPUF_CHECK_EQ(scenario.rt().instance_count(), std::size_t{2});
  std::vector<std::string> devices;
  for (const InstanceId& id : ids) {
    devices.push_back(scenario.rt().instance(id)->dpu.str());
  }
  std::sort(devices.begin(), devices.end());
  DPUF_CHECK_EQ(std::unique(devices.begin(), devices.end()) - devices.begin(),
                static_cast<std::ptrdiff_t>(devices.size()));
}

DPUF_TEST(lifecycle, acknowledgement_is_not_a_verified_effect) {
  Scenario scenario;
  DPUF_CHECK_RESULT_OK(test::apply_standard_world(scenario));
  const std::vector<InstanceId> ids = deploy(scenario, 1);
  DPUF_CHECK_EQ(ids.size(), std::size_t{1});
  const AttemptView view = attempt_of(scenario, ids[0]);
  const Result<BatchReport> report =
      scenario.send_all({acknowledge_event(scenario, view)});
  DPUF_CHECK(report.has_value());
  DPUF_CHECK_EQ(report.value().applied, std::uint64_t{1});
  const std::optional<InstanceState> state = scenario.rt().instance(ids[0]);
  DPUF_CHECK(state.has_value());
  DPUF_CHECK_EQ(state->state, LifecycleState::Acknowledged);
  DPUF_CHECK(!state->serving());
  DPUF_CHECK(state->verified_digest.is_zero());
  DPUF_CHECK(state->verified_evidence.id.str().empty());
  // The decision that recorded the acknowledgement says so explicitly.
  const std::optional<Decision> decision = scenario.rt().last_decision();
  DPUF_CHECK(decision.has_value());
  DPUF_CHECK_EQ(decision->primary, ReasonCode::AcknowledgementOnly);
}

DPUF_TEST(lifecycle, verified_effect_requires_matching_fence_and_fresh_evidence) {
  Scenario scenario;
  DPUF_CHECK_RESULT_OK(test::apply_standard_world(scenario));
  const std::vector<InstanceId> ids = deploy(scenario, 1);
  const AttemptView view = attempt_of(scenario, ids[0]);

  // A report with a stale fence from a previous incarnation is refused.
  AttemptView stale = view;
  stale.fence.serial += 7;
  const Result<BatchReport> fenced = scenario.send_all({effect_event(
      scenario, stale, EffectStatus::Succeeded, scenario.now() + 1, scenario.now() + 50)});
  DPUF_CHECK(fenced.has_value());
  DPUF_CHECK_EQ(fenced.value().refused, std::uint64_t{1});
  DPUF_CHECK_EQ(fenced.value().outcomes[0].code, ReasonCode::FenceMismatch);
  DPUF_CHECK_EQ(scenario.rt().instance(ids[0])->state, LifecycleState::Authorized);

  // A report with a wrong attempt number is refused as superseded.
  AttemptView wrong_attempt = view;
  wrong_attempt.number = AttemptNumber{view.number.value() + 3};
  const Result<BatchReport> superseded = scenario.send_all({effect_event(
      scenario, wrong_attempt, EffectStatus::Succeeded, scenario.now() + 2, scenario.now() + 50)});
  DPUF_CHECK(superseded.has_value());
  DPUF_CHECK_EQ(superseded.value().outcomes[0].code, ReasonCode::AttemptSuperseded);

  // A report with no evidence is refused.
  EffectReported bare;
  bare.report.instance = view.instance;
  bare.report.attempt = view.number;
  bare.report.fence = view.fence;
  bare.report.kind = EffectKind::Deploy;
  bare.report.status = EffectStatus::Succeeded;
  bare.report.reported_at = LogicalInstant{scenario.now() + 3};
  const Result<BatchReport> no_evidence = scenario.send_all({scenario.make(bare, scenario.now() + 3)});
  DPUF_CHECK(no_evidence.has_value());
  DPUF_CHECK_EQ(no_evidence.value().outcomes[0].code, ReasonCode::EvidenceMissing);
  DPUF_CHECK(!scenario.rt().instance(ids[0])->serving());

  // A matching, fresh report verifies the effect.
  const std::uint64_t at = scenario.now() + 4;
  const Result<BatchReport> verified =
      scenario.send_all({effect_event(scenario, view, EffectStatus::Succeeded, at, at + 40)});
  DPUF_CHECK(verified.has_value());
  DPUF_CHECK_EQ(verified.value().applied, std::uint64_t{1});
  const std::optional<InstanceState> state = scenario.rt().instance(ids[0]);
  DPUF_CHECK(state.has_value());
  DPUF_CHECK_EQ(state->state, LifecycleState::Verified);
  DPUF_CHECK(!state->verified_digest.is_zero());
  // Verified effect alone is not "serving": health evidence must still be fresh.
  DPUF_CHECK(!state->serving() || state->health_fresh);
  const std::optional<Decision> decision = scenario.rt().last_decision();
  DPUF_CHECK(decision.has_value());
  DPUF_CHECK(decision->accepted);
  DPUF_CHECK_EQ(decision->deployment.value(), 1);
}

DPUF_TEST(lifecycle, partial_and_failed_effects_are_never_success) {
  Scenario scenario;
  DPUF_CHECK_RESULT_OK(test::apply_standard_world(scenario));
  const std::vector<InstanceId> ids = deploy(scenario, 1);
  const AttemptView view = attempt_of(scenario, ids[0]);
  const std::uint64_t at = scenario.now() + 1;
  const Result<BatchReport> partial =
      scenario.send_all({effect_event(scenario, view, EffectStatus::Partial, at, at + 20)});
  DPUF_CHECK(partial.has_value());
  DPUF_CHECK_EQ(partial.value().outcomes[0].code, ReasonCode::EffectPartial);
  DPUF_CHECK_EQ(scenario.rt().instance(ids[0])->state, LifecycleState::Authorized);
  DPUF_CHECK(scenario.rt().instance(ids[0])->verified_digest.is_zero());

  const std::uint64_t at2 = scenario.now() + 2;
  const Result<BatchReport> failed =
      scenario.send_all({effect_event(scenario, view, EffectStatus::Failed, at2, at2 + 20)});
  DPUF_CHECK(failed.has_value());
  DPUF_CHECK_EQ(failed.value().outcomes[0].code, ReasonCode::EffectFailed);
  DPUF_CHECK_EQ(scenario.rt().instance(ids[0])->state, LifecycleState::Failed);
  DPUF_CHECK(!scenario.rt().instance(ids[0])->serving());
}

DPUF_TEST(lifecycle, duplicate_delivery_is_idempotent_and_conflicts_are_refused) {
  Scenario scenario;
  DPUF_CHECK_RESULT_OK(test::apply_standard_world(scenario));
  scenario.set_now(scenario.released() + 1);
  FabricEvent intent =
      scenario.make(IntentSubmitted{test::standard_intent(scenario.now(), 1)}, scenario.now());
  const Result<BatchReport> first = scenario.send_all({intent});
  DPUF_CHECK(first.has_value());
  DPUF_CHECK_EQ(first.value().applied, std::uint64_t{1});
  const Digest after_first = scenario.rt().state_digest();
  const std::size_t instances = scenario.rt().instance_count();

  // Redelivering the identical event is suppressed and changes nothing.
  const Result<BatchReport> again = scenario.send_all({intent});
  DPUF_CHECK(again.has_value());
  DPUF_CHECK_EQ(again.value().suppressed, std::uint64_t{1});
  DPUF_CHECK_EQ(again.value().applied, std::uint64_t{0});
  DPUF_CHECK_EQ(scenario.rt().state_digest(), after_first);
  DPUF_CHECK_EQ(scenario.rt().instance_count(), instances);

  // The same origin sequence carrying different content is a conflict, not a
  // silent overwrite.
  FabricEvent conflicting = intent;
  conflicting.body = IntentSubmitted{test::standard_intent(scenario.now() + 1, 3)};
  conflicting.at = LogicalInstant{scenario.now() + 1};
  conflicting.payload_digest = conflicting.compute_payload_digest();
  const Result<BatchReport> conflict = scenario.send_all({conflicting});
  DPUF_CHECK(conflict.has_value());
  DPUF_CHECK_EQ(conflict.value().outcomes[0].code, ReasonCode::ConflictingDuplicate);
  DPUF_CHECK_EQ(scenario.rt().state_digest(), after_first);
}

DPUF_TEST(lifecycle, events_older_than_the_released_horizon_are_refused) {
  Scenario scenario;
  DPUF_CHECK_RESULT_OK(test::apply_standard_world(scenario));
  const std::uint64_t horizon = scenario.released();
  FabricEvent late = scenario.make(IntentSubmitted{test::standard_intent(horizon, 1)}, horizon);
  const Status staged = scenario.rt().stage(late);
  DPUF_CHECK_CODE(staged, ReasonCode::LateEventRejected);
  DPUF_CHECK_EQ(scenario.rt().staged_count(), std::size_t{0});
  DPUF_CHECK(scenario.rt().snapshot().truncations.total_dropped() > 0);
}

DPUF_TEST(lifecycle, device_loss_is_never_healthy_continuity) {
  Scenario scenario;
  DPUF_CHECK_RESULT_OK(test::apply_standard_world(scenario));
  const std::vector<InstanceId> ids = deploy(scenario, 2);
  DPUF_CHECK_EQ(ids.size(), std::size_t{2});
  // Verify both replicas so they would otherwise look healthy.
  std::vector<FabricEvent> reports;
  for (const InstanceId& id : ids) {
    const AttemptView view = attempt_of(scenario, id);
    const std::uint64_t at = scenario.now() + 1 + reports.size();
    reports.push_back(effect_event(scenario, view, EffectStatus::Succeeded, at, at + 40));
  }
  DPUF_CHECK_RESULT_OK(scenario.send_all(reports));
  for (const InstanceId& id : ids) {
    DPUF_CHECK_EQ(scenario.rt().instance(id)->state, LifecycleState::Verified);
    DPUF_CHECK(scenario.rt().instance(id)->serving());
  }

  const DpuId lost = scenario.rt().instance(ids[0])->dpu;
  DpuLost loss;
  loss.dpu = lost;
  loss.topology = TopologyGeneration{1};
  loss.evidence = test::make_evidence("ev-loss-" + lost.str(), EvidenceKind::Topology,
                                      LogicalInstant{scenario.now() + 5}, 100,
                                      EvidenceClass::Real);
  DPUF_CHECK_RESULT_OK(scenario.send(loss));

  // The instance on the lost device is not serving, not verified, and its health
  // is explicitly indeterminate.
  const std::optional<InstanceState> victim = scenario.rt().instance(ids[0]);
  DPUF_CHECK(victim.has_value());
  if (victim->dpu == lost) {
    DPUF_CHECK_EQ(victim->state, LifecycleState::Lost);
    DPUF_CHECK(!victim->serving());
    DPUF_CHECK_EQ(victim->health, HealthState::Unknown);
    DPUF_CHECK(!victim->health_fresh);
    DPUF_CHECK_EQ(victim->fence_reason, ReasonCode::DpuLost);
  }
  const std::optional<DpuRecord> record = scenario.rt().device(lost);
  DPUF_CHECK(record.has_value());
  DPUF_CHECK_EQ(record->state, DpuState::Lost);
  DPUF_CHECK(!record->health_fresh);

  // Health evidence observed before the loss cannot restore a health claim.
  HealthObserved stale;
  stale.dpu = lost;
  stale.health = HealthState::Healthy;
  stale.evidence = test::make_evidence("ev-old-health", EvidenceKind::Health, LogicalInstant{1}, 500,
                                       EvidenceClass::Real);
  const Result<BatchReport> refused = scenario.send(stale);
  DPUF_CHECK(refused.has_value());
  DPUF_CHECK_EQ(refused.value().outcomes[0].code, ReasonCode::EvidenceStale);
  DPUF_CHECK(!scenario.rt().device(lost)->health_fresh);

  // Fresh evidence observed after the loss reattaches the device and restores
  // health; only then can continuity be claimed again.
  const std::uint64_t observed = scenario.now() + 6;
  DpuAttached attached;
  attached.dpu = lost;
  attached.attachment = HostAttachmentId::literal("att-" + lost.str());
  attached.domain = IsolationDomainId::literal("dom-a");
  attached.topology = TopologyGeneration{1};
  attached.evidence = test::make_evidence("ev-attach-" + lost.str(), EvidenceKind::Capability,
                                          LogicalInstant{observed}, 100, EvidenceClass::Real);
  DPUF_CHECK_RESULT_OK(scenario.send_at(attached, observed));
  HealthObserved fresh;
  fresh.dpu = lost;
  fresh.health = HealthState::Healthy;
  fresh.evidence = test::make_evidence("ev-new-health-" + lost.str(), EvidenceKind::Health,
                                       LogicalInstant{observed + 1}, 100, EvidenceClass::Real);
  DPUF_CHECK_RESULT_OK(scenario.send_at(fresh, observed + 1));
  const std::optional<DpuRecord> restored = scenario.rt().device(lost);
  DPUF_CHECK(restored.has_value());
  DPUF_CHECK_EQ(restored->state, DpuState::Attached);
  DPUF_CHECK(restored->health_fresh);
  // The device is healthy again, but the instance's proven effect is still
  // fenced: recovery of liveness requires a new attempt and a new verified
  // effect, not merely a healthy device.
  DPUF_CHECK(!scenario.rt().instance(ids[0])->serving());
  DPUF_CHECK_EQ(scenario.rt().instance(ids[0])->state, LifecycleState::Lost);
}

DPUF_TEST(lifecycle, replacement_reissues_authority_and_fences_the_old_attempt) {
  Scenario scenario;
  DPUF_CHECK_RESULT_OK(test::apply_standard_world(scenario));
  const std::vector<InstanceId> ids = deploy(scenario, 1);
  const AttemptView before = attempt_of(scenario, ids[0]);
  const std::uint64_t first_incarnation = scenario.rt().instance(ids[0])->incarnation.value();
  const std::string first_device = scenario.rt().instance(ids[0])->dpu.str();

  // A rollout that moves the service to a new version replaces the instance: a
  // new incarnation, a new attempt and a new fence.
  ServiceDefinition upgraded = test::make_service("svc-a", 1, 4, test::capacity(1000, 2000));
  upgraded.version = ServiceVersion{2, 0, 0};
  DPUF_CHECK_RESULT_OK(scenario.send(ServiceDeclared{upgraded}));
  PlacementIntent intent = test::standard_intent(scenario.now(), 1);
  intent.generation = DeploymentGeneration{2};
  intent.kind = IntentKind::Rollout;
  intent.id = OperationId::literal("op-rollout-2");
  intent.origin_seq = Sequence{200};
  const Result<BatchReport> rollout = scenario.send(IntentSubmitted{intent});
  DPUF_CHECK(rollout.has_value());
  DPUF_CHECK_EQ(rollout.value().applied, std::uint64_t{1});
  const std::optional<InstanceState> after = scenario.rt().instance(ids[0]);
  DPUF_CHECK(after.has_value());
  DPUF_CHECK(after->incarnation.value() >= first_incarnation);
  DPUF_CHECK(after->current_attempt() != nullptr);
  DPUF_CHECK(after->current_attempt()->open());
  DPUF_CHECK(after->current_attempt()->fence.serial > before.fence.serial);
  DPUF_CHECK(after->fence_reason == ReasonCode::AttemptSuperseded ||
             after->dpu.str() != first_device);

  // The superseded attempt can no longer publish an effect.
  const std::uint64_t at = scenario.now() + 1;
  const Result<BatchReport> late_report =
      scenario.send_all({effect_event(scenario, before, EffectStatus::Succeeded, at, at + 20)});
  DPUF_CHECK(late_report.has_value());
  DPUF_CHECK(is_refusal(late_report.value().outcomes[0].code));
  DPUF_CHECK_NE(scenario.rt().instance(ids[0])->state, LifecycleState::Verified);
}

DPUF_TEST(lifecycle, exclusive_scope_authority_is_exclusive_in_the_runtime) {
  Scenario scenario;
  DPUF_CHECK_RESULT_OK(test::apply_standard_world(scenario));
  // Two groups each declare an exclusive scope over services with dedicated
  // device isolation.
  ServiceDefinition dedicated = test::make_service("svc-x", 1, 4, test::capacity(100, 100));
  dedicated.isolation.kind = IsolationKind::DedicatedDevice;
  dedicated.isolation.exclusive_scope = test::scope_id("scope-x");
  DPUF_CHECK_RESULT_OK(scenario.send(ServiceDeclared{dedicated}));
  ServiceDefinition other = test::make_service("svc-y", 1, 4, test::capacity(100, 100));
  other.isolation.kind = IsolationKind::DedicatedDevice;
  other.isolation.exclusive_scope = test::scope_id("scope-y");
  DPUF_CHECK_RESULT_OK(scenario.send(ServiceDeclared{other}));

  ServiceGroup group_x;
  group_x.id = test::group_id("grp-x");
  group_x.members = {test::service_id("svc-x")};
  DPUF_CHECK_RESULT_OK(scenario.send(GroupDeclared{group_x}));
  ServiceGroup group_y;
  group_y.id = test::group_id("grp-y");
  group_y.members = {test::service_id("svc-y")};
  DPUF_CHECK_RESULT_OK(scenario.send(GroupDeclared{group_y}));

  PlacementIntent intent_x;
  intent_x.id = OperationId::literal("op-x");
  intent_x.group = test::group_id("grp-x");
  intent_x.kind = IntentKind::Deploy;
  intent_x.generation = DeploymentGeneration{1};
  intent_x.origin_seq = Sequence{300};
  ServiceReplicaIntent entry_x;
  entry_x.service = test::service_id("svc-x");
  entry_x.desired_replicas = 3;
  intent_x.services.push_back(entry_x);
  intent_x.staging.batch_size = 1;
  DPUF_CHECK_RESULT_OK(scenario.send(IntentSubmitted{intent_x}));

  PlacementIntent intent_y;
  intent_y.id = OperationId::literal("op-y");
  intent_y.group = test::group_id("grp-y");
  intent_y.kind = IntentKind::Deploy;
  intent_y.generation = DeploymentGeneration{2};
  intent_y.origin_seq = Sequence{301};
  ServiceReplicaIntent entry_y;
  entry_y.service = test::service_id("svc-y");
  entry_y.desired_replicas = 1;
  intent_y.services.push_back(entry_y);
  intent_y.staging.batch_size = 1;
  const Result<BatchReport> second = scenario.send(IntentSubmitted{intent_y});
  DPUF_CHECK(second.has_value());
  // Every device is already exclusively owned, so the second group cannot place.
  DPUF_CHECK_EQ(second.value().refused, std::uint64_t{1});
  DPUF_CHECK_EQ(second.value().outcomes[0].code, ReasonCode::NoEligibleDpu);

  // The authority registry still has exactly one owner per device.
  const RuntimeState state = scenario.rt().snapshot();
  std::vector<std::string> owned;
  for (const AuthorityGrant& grant : state.authority.grants()) owned.push_back(grant.dpu.str());
  std::sort(owned.begin(), owned.end());
  DPUF_CHECK_EQ(std::unique(owned.begin(), owned.end()) - owned.begin(),
                static_cast<std::ptrdiff_t>(owned.size()));
  for (const AuthorityGrant& grant : state.authority.grants()) {
    DPUF_CHECK_EQ(grant.scope.str(), std::string{"scope-x"});
  }
}

DPUF_TEST(lifecycle, quiesce_and_withdrawal_require_verified_effects) {
  Scenario scenario;
  DPUF_CHECK_RESULT_OK(test::apply_standard_world(scenario));
  const std::vector<InstanceId> ids = deploy(scenario, 2);
  DPUF_CHECK_EQ(ids.size(), std::size_t{2});

  DPUF_CHECK_RESULT_OK(scenario.send(QuiesceRequested{test::service_id("svc-a")}));
  for (const InstanceId& id : ids) {
    // The instance is copied into a local: ranging over a container reached
    // through a temporary would dangle as soon as the range expression ended.
    const std::optional<InstanceState> state = scenario.rt().instance(id);
    DPUF_CHECK(state.has_value());
    if (!state.has_value()) continue;
    DPUF_CHECK_EQ(state->state, LifecycleState::Quiescing);
    DPUF_CHECK(!state->serving());
    for (const AttemptRecord& attempt : state->attempts) {
      DPUF_CHECK(attempt.outcome != AttemptOutcome::Open);
    }
  }

  // Withdrawal is an effect: it is complete only once an executor proves it.
  PlacementIntent withdraw = test::standard_intent(scenario.now(), 2);
  withdraw.kind = IntentKind::Withdraw;
  withdraw.generation = DeploymentGeneration{2};
  withdraw.id = OperationId::literal("op-withdraw");
  withdraw.origin_seq = Sequence{400};
  const Result<BatchReport> withdrawn = scenario.send(IntentSubmitted{withdraw});
  DPUF_CHECK(withdrawn.has_value());
  DPUF_CHECK_EQ(withdrawn.value().applied, std::uint64_t{1});
  for (const InstanceId& id : ids) {
    DPUF_CHECK_EQ(scenario.rt().instance(id)->state, LifecycleState::Quiescing);
  }
  // A verified withdrawal effect completes the transition.
  const std::vector<InstanceState> instances = scenario.rt().snapshot().instances;
  std::vector<FabricEvent> reports;
  for (const InstanceState& instance : instances) {
    if (instance.current_attempt() == nullptr) continue;
    AttemptView view;
    view.instance = instance.id;
    view.number = instance.current_attempt()->number;
    view.fence = instance.current_attempt()->fence;
    const std::uint64_t at = scenario.now() + 1 + reports.size();
    reports.push_back(effect_event(scenario, view, EffectStatus::Succeeded, at, at + 30,
                                   EffectKind::Withdraw));
  }
  DPUF_CHECK_RESULT_OK(scenario.send_all(reports));
  for (const InstanceId& id : ids) {
    DPUF_CHECK_EQ(scenario.rt().instance(id)->state, LifecycleState::Withdrawn);
    DPUF_CHECK(!scenario.rt().instance(id)->serving());
  }
  DPUF_CHECK(scenario.rt().snapshot().authority.grants().empty());
}

DPUF_TEST(lifecycle, rollback_reuses_a_retained_generation) {
  Scenario scenario;
  DPUF_CHECK_RESULT_OK(test::apply_standard_world(scenario));
  DPUF_CHECK_EQ(deploy(scenario, 2).size(), std::size_t{2});
  const Digest generation_one = scenario.rt().snapshot().plans.back().digest;

  PlacementIntent rollout = test::standard_intent(scenario.now(), 1);
  rollout.generation = DeploymentGeneration{2};
  rollout.kind = IntentKind::Rollout;
  rollout.id = OperationId::literal("op-rollout");
  rollout.origin_seq = Sequence{500};
  DPUF_CHECK_RESULT_OK(scenario.send(IntentSubmitted{rollout}));
  DPUF_CHECK_EQ(scenario.rt().snapshot().deployment_generation.value(), std::uint64_t{2});

  PlacementIntent rollback = test::standard_intent(scenario.now(), 2);
  rollback.generation = DeploymentGeneration{3};
  rollback.kind = IntentKind::Rollback;
  rollback.rollback_target = DeploymentGeneration{1};
  rollback.id = OperationId::literal("op-rollback");
  rollback.origin_seq = Sequence{501};
  const Result<BatchReport> rolled = scenario.send(IntentSubmitted{rollback});
  DPUF_CHECK(rolled.has_value());
  DPUF_CHECK_EQ(rolled.value().applied, std::uint64_t{1});
  const DeploymentPlan plan = scenario.rt().snapshot().plans.back();
  DPUF_CHECK_EQ(plan.kind, IntentKind::Rollback);
  DPUF_CHECK(plan.rollback_target.has_value());
  DPUF_CHECK_EQ(plan.rollback_target->value(), std::uint64_t{1});
  DPUF_CHECK_EQ(plan.services.size(), std::size_t{1});
  DPUF_CHECK_EQ(plan.services[0].placed_replicas, std::uint32_t{2});
  DPUF_CHECK_NE(plan.digest, generation_one);

  // A rollback to a generation that was never retained is refused.
  PlacementIntent missing = test::standard_intent(scenario.now(), 2);
  missing.generation = DeploymentGeneration{9};
  missing.kind = IntentKind::Rollback;
  missing.rollback_target = DeploymentGeneration{8};
  missing.id = OperationId::literal("op-rollback-missing");
  missing.origin_seq = Sequence{502};
  const Result<BatchReport> refused = scenario.send(IntentSubmitted{missing});
  DPUF_CHECK(refused.has_value());
  DPUF_CHECK_EQ(refused.value().outcomes[0].code, ReasonCode::RollbackUnavailable);
}

DPUF_TEST(lifecycle, unknown_capability_blocks_placement_claims) {
  Scenario scenario;
  DPUF_CHECK_RESULT_OK(test::apply_standard_world(scenario));
  ServiceDefinition picky = test::make_service("svc-picky", 1, 2, test::capacity(100, 100));
  picky.compatibility.capabilities.push_back(
      test::requirement("never.observed", CapabilityOp::Present));
  DPUF_CHECK_RESULT_OK(scenario.send(ServiceDeclared{picky}));
  ServiceGroup picky_group;
  picky_group.id = test::group_id("grp-picky");
  picky_group.members = {test::service_id("svc-picky")};
  DPUF_CHECK_RESULT_OK(scenario.send(GroupDeclared{picky_group}));

  PlacementIntent intent;
  intent.id = OperationId::literal("op-picky");
  intent.group = test::group_id("grp-picky");
  intent.kind = IntentKind::Deploy;
  intent.generation = DeploymentGeneration{1};
  intent.origin_seq = Sequence{600};
  ServiceReplicaIntent entry;
  entry.service = test::service_id("svc-picky");
  entry.desired_replicas = 1;
  intent.services.push_back(entry);
  const Result<BatchReport> refused = scenario.send(IntentSubmitted{intent});
  DPUF_CHECK(refused.has_value());
  DPUF_CHECK(is_refusal(refused.value().outcomes[0].code));
  DPUF_CHECK_EQ(scenario.rt().instance_count(), std::size_t{0});
}

DPUF_TEST(lifecycle, decision_and_explanation_surfaces_name_the_evidence) {
  Scenario scenario;
  DPUF_CHECK_RESULT_OK(test::apply_standard_world(scenario));
  DPUF_CHECK_EQ(deploy(scenario, 1).size(), std::size_t{1});
  const std::vector<Explanation> explanations = scenario.rt().explain_last_decision();
  DPUF_CHECK(!explanations.empty());
  bool mentions_generation = false;
  for (const Explanation& entry : explanations) {
    if (entry.deployment.value() == 1) mentions_generation = true;
  }
  DPUF_CHECK(mentions_generation);

  const RuntimeState state = scenario.rt().snapshot();
  const InstanceId id = state.instances[0].id;
  const std::vector<Explanation> instance_explanations = scenario.rt().explain_instance(id);
  DPUF_CHECK(!instance_explanations.empty());
  DPUF_CHECK_RESULT_OK(scenario.tick());
  const std::optional<Eligibility> verdict =
      scenario.rt().explain_eligibility(state.instances[0].dpu, test::service_id("svc-a"));
  DPUF_CHECK(verdict.has_value());
  DPUF_CHECK(verdict->eligible);
  DPUF_CHECK(!verdict->results.empty());
  for (const RequirementResult& entry : verdict->results) {
    DPUF_CHECK(entry.outcome != RequirementOutcome::Violated);
  }
  // The export is canonical JSON carrying both digests and the recovery record.
  const std::string document = scenario.rt().export_json().to_text(false);
  const Result<JsonValue> parsed = parse_json(document, 1u << 22);
  DPUF_CHECK(parsed.has_value());
  DPUF_CHECK(parsed.value().find("state_digest") != nullptr);
  DPUF_CHECK(parsed.value().find("durable_digest") != nullptr);
  DPUF_CHECK(parsed.value().find("state") != nullptr);
  DPUF_CHECK(parsed.value().find("recovery") != nullptr);
}

DPUF_TEST(lifecycle, state_is_a_function_of_the_accepted_event_sequence) {
  Scenario first;
  Scenario second;
  DPUF_CHECK_RESULT_OK(test::apply_standard_world(first));
  DPUF_CHECK_RESULT_OK(test::apply_standard_world(second));
  DPUF_CHECK_EQ(first.rt().state_digest(), second.rt().state_digest());
  DPUF_CHECK_EQ(first.rt().durable_digest(), second.rt().durable_digest());
  DPUF_CHECK_EQ(deploy(first, 2).size(), std::size_t{2});
  DPUF_CHECK_EQ(deploy(second, 2).size(), std::size_t{2});
  DPUF_CHECK_EQ(first.rt().state_digest(), second.rt().state_digest());
  DPUF_CHECK_EQ(first.rt().export_json().to_text(false), second.rt().export_json().to_text(false));
}

}  // namespace
}  // namespace dpu::fabric
