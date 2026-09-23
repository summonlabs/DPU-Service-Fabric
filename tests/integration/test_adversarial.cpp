// Adversarial input tests.
//
// Every input in this file is hostile, impossible, stale, duplicated, truncated,
// oversized or simply wrong. The requirement is uniform: the runtime refuses with
// a stable reason code and never produces a valid-looking success.

#include <string>
#include <vector>

#include "dpu/fabric/dpu_fabric.hpp"
#include "support/harness.hpp"
#include "support/scenario.hpp"

namespace dpu::fabric {
namespace {

using test::Scenario;

DPUF_TEST(adversarial, event_shape_is_validated_before_anything_else) {
  Scenario scenario;
  DPUF_CHECK_RESULT_OK(test::apply_standard_world(scenario));

  FabricEvent anonymous = test::make_event("ok-id", "ok-origin", 99, scenario.released() + 1,
                                           LogicalTimeAdvanced{LogicalInstant{scenario.released() + 1}},
                                           EvidenceClass::Synthetic);
  anonymous.id = EventId{};
  DPUF_CHECK_CODE(scenario.rt().stage(anonymous), ReasonCode::InvalidIdentity);

  FabricEvent nameless = anonymous;
  nameless.id = EventId::literal("ok-id");
  nameless.origin = OriginId{};
  DPUF_CHECK_CODE(scenario.rt().stage(nameless), ReasonCode::InvalidIdentity);

  FabricEvent no_epoch = nameless;
  no_epoch.origin = OriginId::literal("ok-origin");
  no_epoch.origin_epoch = OriginEpoch{};
  DPUF_CHECK_CODE(scenario.rt().stage(no_epoch), ReasonCode::InvalidIdentity);

  // A payload digest that disagrees with the body is a corruption signal.
  FabricEvent tampered = no_epoch;
  tampered.origin_epoch = OriginEpoch{1};
  tampered.payload_digest = sha256("not-the-body");
  DPUF_CHECK_CODE(scenario.rt().stage(tampered), ReasonCode::InvalidDigest);

  // Nothing was staged by any of the refused events.
  DPUF_CHECK_EQ(scenario.rt().staged_count(), std::size_t{0});
}

DPUF_TEST(adversarial, decode_refuses_every_truncation_prefix) {
  FabricEvent event = test::make_event("prefix", "prefix-origin", 1, 1,
                                       IntentSubmitted{test::standard_intent(1, 1)},
                                       EvidenceClass::Synthetic);
  const std::vector<std::uint8_t> encoded = encode_binary(event);
  DPUF_CHECK(encoded.size() > 8);
  for (std::size_t length = 0; length < encoded.size(); ++length) {
    const std::span<const std::uint8_t> prefix{encoded.data(), length};
    const Result<FabricEvent> decoded = decode_binary<FabricEvent>(prefix);
    if (decoded.has_value()) {
      test::report_failure(__FILE__, __LINE__,
                           "a truncated payload decoded successfully at length " +
                               std::to_string(length));
      break;
    }
  }
  // Every single-byte flip in the identity region is refused too.
  for (std::size_t i = 0; i < std::min<std::size_t>(encoded.size(), 64); ++i) {
    std::vector<std::uint8_t> damaged = encoded;
    damaged[i] = static_cast<std::uint8_t>(damaged[i] ^ 0xFFu);
    const Result<FabricEvent> decoded = decode_binary<FabricEvent>(damaged);
    if (decoded.has_value()) {
      const Digest original = event_digest(event);
      const Digest recovered = event_digest(decoded.value());
      if (original == recovered) {
        test::report_failure(__FILE__, __LINE__,
                             "a flipped byte decoded to the same event at offset " + std::to_string(i));
        break;
      }
    }
  }
}

DPUF_TEST(adversarial, frames_are_bounded_and_integrity_checked) {
  Frame frame;
  frame.op = OpCode::Submit;
  frame.request_id = 7;
  frame.payload.assign(64, 0x41);
  Result<std::vector<std::uint8_t>> encoded = encode_frame(frame, 1024);
  DPUF_REQUIRE(encoded);

  // The bound is enforced at encode time.
  DPUF_CHECK(!encode_frame(frame, 32).has_value());
  DPUF_CHECK_CODE(encode_frame(frame, 32).status(), ReasonCode::FrameOversized);

  // A declaration larger than the bound is refused before allocating.
  std::vector<std::uint8_t> oversized = encoded.value();
  oversized[20] = 0xFF;
  oversized[21] = 0xFF;
  oversized[22] = 0xFF;
  oversized[23] = 0x7F;
  const Result<DecodedFrame> rejected = decode_frame(oversized, 1024);
  DPUF_CHECK(!rejected.has_value());
  DPUF_CHECK_CODE(rejected.status(), ReasonCode::FrameOversized);

  // Bad magic and unsupported versions are distinct refusals.
  std::vector<std::uint8_t> wrong_magic = encoded.value();
  wrong_magic[0] = 0;
  DPUF_CHECK_CODE(decode_frame(wrong_magic, 1024).status(), ReasonCode::ProtocolViolation);
  std::vector<std::uint8_t> wrong_version = encoded.value();
  wrong_version[4] = 9;
  DPUF_CHECK_CODE(decode_frame(wrong_version, 1024).status(), ReasonCode::UnsupportedVersion);
  std::vector<std::uint8_t> wrong_op = encoded.value();
  wrong_op[6] = 0xFF;
  wrong_op[7] = 0x7F;
  DPUF_CHECK_CODE(decode_frame(wrong_op, 1024).status(), ReasonCode::UnknownOperation);

  // A flipped payload byte breaks the trailer.
  std::vector<std::uint8_t> corrupted = encoded.value();
  corrupted[kProtocolHeaderBytes + 1] ^= 0xFF;
  DPUF_CHECK_CODE(decode_frame(corrupted, 1024).status(), ReasonCode::FrameTruncated);

  // Partial buffers report what is still needed instead of failing.
  for (std::size_t length = 0; length < encoded.value().size(); ++length) {
    const Result<DecodedFrame> partial =
        decode_frame(std::span<const std::uint8_t>(encoded.value().data(), length), 1024);
    DPUF_REQUIRE(partial);
    DPUF_CHECK(!partial.value().complete);
    DPUF_CHECK(partial.value().expected >= length);
  }
}

DPUF_TEST(adversarial, stale_generations_and_authority_are_refused) {
  Scenario scenario;
  DPUF_CHECK_RESULT_OK(test::apply_standard_world(scenario));

  // Topology generation regression.
  TopologyObserved older;
  older.snapshot.generation = TopologyGeneration{0};
  older.snapshot.dpus = test::standard_dpus(scenario.now(), 100, CapabilityGeneration{1},
                                            TopologyGeneration{1});
  Result<BatchReport> report = scenario.send(older);
  DPUF_REQUIRE(report);
  DPUF_CHECK_EQ(report.value().outcomes[0].code, ReasonCode::InvalidIdentity);

  TopologyObserved regression;
  regression.snapshot.generation = TopologyGeneration{1};
  regression.snapshot.dpus = test::standard_dpus(scenario.now(), 100, CapabilityGeneration{1},
                                                 TopologyGeneration{1});
  DPUF_CHECK_RESULT_OK(scenario.send(regression));
  TopologyObserved older_valid;
  older_valid.snapshot.generation = TopologyGeneration{1};
  older_valid.snapshot.observed_at = LogicalInstant{scenario.now()};
  older_valid.snapshot.dpus = test::standard_dpus(scenario.now(), 100, CapabilityGeneration{1},
                                                  TopologyGeneration{1});
  older_valid.snapshot.evidence = test::make_evidence("ev-old-topology", EvidenceKind::Topology,
                                                      LogicalInstant{scenario.now()}, 100,
                                                      EvidenceClass::Synthetic);
  DPUF_CHECK_RESULT_OK(scenario.send(older_valid));

  // Capability generation regression on one device.
  CapabilityObserved capability;
  capability.dpu = test::dpu_id("dpu-1");
  capability.generation = CapabilityGeneration{0};
  capability.evidence = test::make_evidence("ev-cap", EvidenceKind::Capability,
                                            LogicalInstant{scenario.now()}, 100,
                                            EvidenceClass::Synthetic, CapabilityGeneration{0},
                                            TopologyGeneration{1});
  report = scenario.send(capability);
  DPUF_REQUIRE(report);
  DPUF_CHECK_EQ(report.value().outcomes[0].code, ReasonCode::StaleGeneration);

  // Policy generation regression.
  PolicyAdvanced policy;
  policy.policy = test::make_policy(PolicyGeneration{0}, LogicalInstant{scenario.now()}, 100);
  report = scenario.send(policy);
  DPUF_REQUIRE(report);
  DPUF_CHECK_EQ(report.value().outcomes[0].code, ReasonCode::InvalidIdentity);

  // Coordinator epoch regression, and epoch advance fencing all authority.
  CoordinatorAdvanced advance;
  advance.epoch = CoordinatorEpoch{0};
  advance.coordinator = test::origin_id("coordinator");
  report = scenario.send(advance);
  DPUF_REQUIRE(report);
  DPUF_CHECK_EQ(report.value().outcomes[0].code, ReasonCode::EpochRegressed);

  // A deployment generation that regresses is refused.
  PlacementIntent intent = test::standard_intent(scenario.now(), 1);
  intent.generation = DeploymentGeneration{5};
  intent.id = OperationId::literal("op-ahead");
  intent.origin_seq = Sequence{900};
  DPUF_CHECK_RESULT_OK(scenario.send(IntentSubmitted{intent}));
  PlacementIntent older_intent = test::standard_intent(scenario.now(), 1);
  older_intent.generation = DeploymentGeneration{2};
  older_intent.id = OperationId::literal("op-behind");
  older_intent.origin_seq = Sequence{901};
  report = scenario.send(IntentSubmitted{older_intent});
  DPUF_REQUIRE(report);
  DPUF_CHECK_EQ(report.value().outcomes[0].code, ReasonCode::PlanGenerationRegressed);
}

DPUF_TEST(adversarial, duplicate_identities_are_refused) {
  Scenario scenario;
  DPUF_CHECK_RESULT_OK(test::apply_standard_world(scenario));

  TopologyObserved duplicated;
  duplicated.snapshot.generation = TopologyGeneration{2};
  duplicated.snapshot.observed_at = LogicalInstant{scenario.now()};
  duplicated.snapshot.dpus = test::standard_dpus(scenario.now(), 100, CapabilityGeneration{1},
                                                 TopologyGeneration{2});
  duplicated.snapshot.dpus.push_back(duplicated.snapshot.dpus.front());
  duplicated.snapshot.evidence = test::make_evidence("ev-dup", EvidenceKind::Topology,
                                                     LogicalInstant{scenario.now()}, 100,
                                                     EvidenceClass::Synthetic);
  Result<BatchReport> report = scenario.send(duplicated);
  DPUF_REQUIRE(report);
  DPUF_CHECK_EQ(report.value().outcomes[0].code, ReasonCode::DuplicateIdentity);

  // A capability set with the same key twice.
  CapabilityObserved duplicated_capability;
  duplicated_capability.dpu = test::dpu_id("dpu-1");
  duplicated_capability.generation = CapabilityGeneration{2};
  duplicated_capability.topology = TopologyGeneration{1};
  duplicated_capability.capabilities.entries.push_back(
      test::make_capability("dup.key", test::flag_value(true), CapabilityGeneration{2}));
  duplicated_capability.capabilities.entries.push_back(
      test::make_capability("dup.key", test::flag_value(false), CapabilityGeneration{2}));
  duplicated_capability.evidence = test::make_evidence(
      "ev-dup-cap", EvidenceKind::Capability, LogicalInstant{scenario.now()}, 100,
      EvidenceClass::Synthetic, CapabilityGeneration{2}, TopologyGeneration{1});
  report = scenario.send(duplicated_capability);
  DPUF_REQUIRE(report);
  DPUF_CHECK_EQ(report.value().outcomes[0].code, ReasonCode::DuplicateIdentity);

  // A repeated dependency edge with the same endpoints and kind.
  Dependency edge;
  edge.id = DependencyId::literal("dup-dep");
  edge.from = test::service_id("svc-b");
  edge.to = test::service_id("svc-a");
  edge.kind = DependencyKind::Requires;
  DPUF_CHECK_RESULT_OK(scenario.send(DependencyDeclared{edge}));
  Dependency again = edge;
  again.id = DependencyId::literal("dup-dep-2");
  report = scenario.send(DependencyDeclared{again});
  DPUF_REQUIRE(report);
  DPUF_CHECK_EQ(report.value().outcomes[0].code, ReasonCode::DependencyDuplicate);

  // Self dependency.
  Dependency self;
  self.id = DependencyId::literal("self-dep");
  self.from = test::service_id("svc-a");
  self.to = test::service_id("svc-a");
  report = scenario.send(DependencyDeclared{self});
  DPUF_REQUIRE(report);
  DPUF_CHECK_EQ(report.value().outcomes[0].code, ReasonCode::SelfDependency);
}

DPUF_TEST(adversarial, impossible_requests_are_refused) {
  Scenario scenario;
  DPUF_CHECK_RESULT_OK(test::apply_standard_world(scenario));

  // An intent for a group that does not exist.
  PlacementIntent unknown_group = test::standard_intent(scenario.now(), 1);
  unknown_group.group = test::group_id("grp-absent");
  unknown_group.id = OperationId::literal("op-absent");
  unknown_group.origin_seq = Sequence{910};
  Result<BatchReport> report = scenario.send(IntentSubmitted{unknown_group});
  DPUF_REQUIRE(report);
  DPUF_CHECK_EQ(report.value().outcomes[0].code, ReasonCode::GroupNotDeclared);

  // An intent that asks for a service that was never declared.
  PlacementIntent unknown_service = test::standard_intent(scenario.now(), 1);
  unknown_service.services[0].service = test::service_id("svc-absent");
  unknown_service.id = OperationId::literal("op-absent-svc");
  unknown_service.origin_seq = Sequence{911};
  report = scenario.send(IntentSubmitted{unknown_service});
  DPUF_REQUIRE(report);
  DPUF_CHECK_EQ(report.value().outcomes[0].code, ReasonCode::IntentNotAccepted);

  // Zero replicas for a service whose policy requires at least one.
  PlacementIntent zero = test::standard_intent(scenario.now(), 1);
  zero.services[0].desired_replicas = 0;
  zero.id = OperationId::literal("op-zero");
  zero.origin_seq = Sequence{912};
  report = scenario.send(IntentSubmitted{zero});
  DPUF_REQUIRE(report);
  DPUF_CHECK_EQ(report.value().outcomes[0].code, ReasonCode::IntentNotAccepted);

  // More replicas than devices can host.
  PlacementIntent too_many = test::standard_intent(scenario.now(), 1);
  too_many.services[0].desired_replicas = 4;
  too_many.id = OperationId::literal("op-too-many");
  too_many.origin_seq = Sequence{913};
  report = scenario.send(IntentSubmitted{too_many});
  DPUF_REQUIRE(report);
  DPUF_CHECK_EQ(report.value().outcomes[0].code, ReasonCode::InsufficientEligibleDpus);
  DPUF_CHECK_EQ(scenario.rt().instance_count(), std::size_t{0});

  // Bounds: a topology larger than the device bound.
  RuntimeConfig small = test::make_config();
  small.bounds.max_dpus = 2;
  Scenario bounded{small};
  TopologyObserved oversized;
  oversized.snapshot.generation = TopologyGeneration{1};
  oversized.snapshot.observed_at = LogicalInstant{1};
  oversized.snapshot.dpus = test::standard_dpus(1, 100, CapabilityGeneration{1},
                                                TopologyGeneration{1});
  report = bounded.send(oversized);
  DPUF_REQUIRE(report);
  DPUF_CHECK_EQ(report.value().outcomes[0].code, ReasonCode::BoundExceeded);
}

DPUF_TEST(adversarial, oversized_payloads_are_refused_and_accounted) {
  // A frame far beyond the bound is refused at the codec.
  Frame huge;
  huge.payload.assign(4096, 0x7F);
  DPUF_CHECK(!encode_frame(huge, 1024).has_value());

  // A batch larger than the configured batch bound.
  RuntimeConfig config = test::make_config();
  config.bounds.max_batch_events = 4;
  Result<std::unique_ptr<FabricRuntime>> runtime = FabricRuntime::open(config);
  DPUF_REQUIRE(runtime);
  std::vector<FabricEvent> events;
  for (std::uint64_t i = 0; i < 8; ++i) {
    events.push_back(test::make_event("big-" + std::to_string(i), "big-origin", i + 1, 1,
                                      LogicalTimeAdvanced{LogicalInstant{i + 1}},
                                      EvidenceClass::Synthetic));
  }
  const Result<BatchReport> refused = runtime.value()->submit(events);
  DPUF_CHECK(!refused.has_value());
  DPUF_CHECK_CODE(refused.status(), ReasonCode::BatchTooLarge);

  // A sequence that declares more elements than the decode bound allows is
  // refused before anything is allocated.
  ServiceDefinition service = test::make_service("svc-a", 1, 2, test::capacity(10, 10));
  service.compatibility.capabilities.push_back(
      test::requirement("crypto.aes", CapabilityOp::Present));
  service.compatibility.capabilities.push_back(
      test::requirement("crypto.throughput", CapabilityOp::AtLeast, test::integer_value(1)));
  const std::vector<std::uint8_t> encoded = encode_binary(service);
  const Result<ServiceDefinition> decoded = decode_binary<ServiceDefinition>(encoded, 1);
  DPUF_CHECK(!decoded.has_value());
  DPUF_CHECK_CODE(decoded.status(), ReasonCode::OversizedInput);
  const Result<ServiceDefinition> allowed = decode_binary<ServiceDefinition>(encoded, 8);
  DPUF_CHECK(allowed.has_value());
  DPUF_CHECK_OK(runtime.value()->close());
}

DPUF_TEST(adversarial, authority_cannot_be_replayed_across_a_coordinator_change) {
  Scenario scenario;
  DPUF_CHECK_RESULT_OK(test::apply_standard_world(scenario));
  const Result<BatchReport> deployed =
      scenario.send(IntentSubmitted{test::standard_intent(scenario.now(), 1)});
  DPUF_REQUIRE(deployed);
  DPUF_CHECK_EQ(deployed.value().applied, std::uint64_t{1});
  const InstanceId id = scenario.rt().snapshot().instances.front().id;
  const std::optional<InstanceState> before_advance = scenario.rt().instance(id);
  DPUF_CHECK(before_advance.has_value());
  if (!before_advance.has_value() || before_advance->current_attempt() == nullptr) return;
  // The attempt is copied: instance() returns a value, so a pointer into it
  // would dangle as soon as the statement ended.
  const AttemptRecord attempt = *before_advance->current_attempt();
  const FenceToken before = attempt.fence;

  CoordinatorAdvanced advance;
  advance.epoch = CoordinatorEpoch{2};
  advance.coordinator = test::origin_id("coordinator-2");
  DPUF_CHECK_RESULT_OK(scenario.send(advance));
  // The epoch advance fences every instance and revokes every grant.
  DPUF_CHECK(!scenario.rt().instance(id)->serving());
  DPUF_CHECK_EQ(scenario.rt().instance(id)->state, LifecycleState::Unverified);
  DPUF_CHECK(scenario.rt().snapshot().authority.grants().empty());

  EffectReported reported;
  reported.report.instance = id;
  reported.report.attempt = attempt.number;
  reported.report.fence = before;
  reported.report.kind = EffectKind::Deploy;
  reported.report.status = EffectStatus::Succeeded;
  const std::uint64_t at = scenario.released() + 1;
  reported.report.reported_at = LogicalInstant{at};
  reported.report.evidence = test::make_evidence("ev-stale", EvidenceKind::Execution,
                                                  LogicalInstant{at}, 50, EvidenceClass::Real);
  reported.report.effect_digest = sha256("stale");
  Result<BatchReport> stale = scenario.send_all({scenario.make(reported, at)});
  DPUF_REQUIRE(stale);
  DPUF_CHECK_EQ(stale.value().outcomes[0].code, ReasonCode::FenceMismatch);
  DPUF_CHECK(!scenario.rt().instance(id)->serving());
}

DPUF_TEST(adversarial, json_input_refuses_hostile_documents) {
  ServiceDefinition service = test::make_service("svc-a", 1, 2, test::capacity(10, 10));
  const JsonValue document = encode_json(service);
  // Wrong type in a known field.
  JsonValue wrong = document;
  wrong.set("replicas", JsonValue{std::int64_t{5}});
  DPUF_CHECK(!decode_json<ServiceDefinition>(wrong).has_value());
  // Missing nested member.
  JsonValue incomplete = JsonValue::make_object();
  for (const auto& member : document.members()) {
    if (member.first != "compatibility") incomplete.set(member.first, member.second);
  }
  DPUF_CHECK_CODE(decode_json<ServiceDefinition>(incomplete).status(), ReasonCode::MissingField);
  // An enumeration name that does not exist.
  JsonValue bad_enum = document;
  JsonValue compatibility = *document.find("compatibility");
  JsonValue architecture = JsonValue::make_object();
  for (const auto& member : compatibility.members()) {
    architecture.set(member.first, member.second);
  }
  architecture.set("architecture", JsonValue{std::string{"NotAnArchitecture"}});
  bad_enum.set("compatibility", architecture);
  const Result<ServiceDefinition> refused = decode_json<ServiceDefinition>(bad_enum);
  DPUF_CHECK(!refused.has_value());
  DPUF_CHECK_CODE(refused.status(), ReasonCode::InvalidEnumeration);
  // A negative value in an unsigned field.
  JsonValue negative = document;
  JsonValue replicas = JsonValue::make_object();
  for (const auto& member : document.find("replicas")->members()) {
    replicas.set(member.first, member.second);
  }
  replicas.set("max", JsonValue{std::int64_t{-1}});
  negative.set("replicas", replicas);
  DPUF_CHECK(!decode_json<ServiceDefinition>(negative).has_value());
}

}  // namespace
}  // namespace dpu::fabric
