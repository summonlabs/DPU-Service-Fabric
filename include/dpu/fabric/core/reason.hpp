#pragma once

// Stable reason codes.
//
// Every acceptance, refusal, observation, and recovery classification produced by
// the runtime carries exactly one of these codes. The numeric value of a code is
// part of the wire and persistence contract and never changes once released; new
// codes take new numbers. Codes are grouped into stable bands so that a consumer
// can classify an outcome without knowing every individual code.

#include <cstdint>
#include <string_view>

namespace dpu::fabric {

// Band layout:
//   0        success
//   1..99    input shape, encoding and versioning
//   100..199 identity, generations, epochs, fences and authority
//   200..299 device capability, compatibility and evidence quality
//   300..399 dependency and declaration topology
//   400..499 placement, replica accounting and rollout planning
//   500..599 instance lifecycle, attempts and verified effects
//   700..799 durability, recovery and store integrity
//   800..899 transport framing and protocol
//   900..999 bounded resources, truncation and accounting
#define DPUF_REASON_CODES(X)                                    \
  /* ---- success ---- */                                       \
  X(Ok, 0)                                                      \
                                                                \
  /* ---- 1..99 input ---- */                                   \
  X(MalformedInput, 1)                                          \
  X(MissingField, 2)                                            \
  X(InvalidIdentity, 3)                                         \
  X(ValueOutOfRange, 4)                                         \
  X(OversizedInput, 5)                                          \
  X(TruncatedInput, 6)                                          \
  X(UnsupportedVersion, 7)                                      \
  X(InvalidEnumeration, 8)                                      \
  X(DuplicateIdentity, 9)                                       \
  X(ConflictingDuplicate, 10)                                   \
  X(ArithmeticOverflow, 11)                                     \
  X(InvalidStateTransition, 12)                                 \
  X(UnsupportedValue, 13)                                       \
  X(InvalidDigest, 14)                                          \
  X(InvalidTag, 15)                                             \
  X(UnknownField, 16)                                           \
  X(InvalidEncoding, 17)                                        \
  X(UnsupportedOperation, 18)                                   \
                                                                \
  /* ---- 100..199 identity/authority ---- */                   \
  X(StaleOriginEpoch, 100)                                      \
  X(StaleGeneration, 101)                                       \
  X(StaleAuthority, 102)                                        \
  X(FenceMismatch, 103)                                         \
  X(FenceSuperseded, 104)                                       \
  X(AttemptSuperseded, 105)                                     \
  X(AttemptCancelled, 106)                                      \
  X(AttemptNotOpen, 107)                                        \
  X(AuthorityNotGranted, 108)                                   \
  X(AuthorityRevoked, 109)                                      \
  X(ExclusiveScopeConflict, 110)                                \
  X(EpochRegressed, 111)                                        \
  X(BootFenced, 112)                                            \
  X(LateEventRejected, 113)                                     \
  X(DuplicateSuppressed, 114)                                   \
  X(GenerationRegressed, 115)                                   \
  X(AuthorityHeldElsewhere, 116)                                \
                                                                \
  /* ---- 200..299 capability/evidence ---- */                  \
  X(CapabilityUnknown, 200)                                     \
  X(CapabilityMissing, 201)                                     \
  X(CapabilityMismatch, 202)                                    \
  X(CompatibilityUnsatisfied, 203)                              \
  X(DpuIneligible, 204)                                         \
  X(DpuUnavailable, 205)                                        \
  X(CapacityUnknown, 206)                                       \
  X(HostAttachmentMismatch, 207)                                \
  X(FirmwareLevelUnsupported, 208)                              \
  X(IsolationUnsupported, 209)                                  \
  X(EvidenceStale, 210)                                         \
  X(EvidenceMissing, 211)                                       \
  X(EvidenceSuperseded, 212)                                    \
  X(EvidenceConflicting, 213)                                   \
  X(HealthIndeterminate, 214)                                   \
  X(ContinuityUnproven, 215)                                    \
  X(ArchitectureMismatch, 216)                                  \
  X(DatapathUnsupported, 217)                                   \
  X(AllocationExhausted, 218)                                   \
                                                                \
  /* ---- 300..399 dependency ---- */                           \
  X(DependencyCycle, 300)                                       \
  X(DependencyUnsatisfied, 301)                                  \
  X(DependencyUnknown, 302)                                     \
  X(DependencyKindUnsupported, 303)                             \
  X(ServiceNotDeclared, 304)                                    \
  X(GroupNotDeclared, 305)                                      \
  X(SelfDependency, 306)                                        \
  X(DependencyDuplicate, 307)                                   \
  X(ServiceVersionUnavailable, 308)                             \
                                                                \
  /* ---- 400..499 placement ---- */                            \
  X(NoEligibleDpu, 400)                                         \
  X(InsufficientEligibleDpus, 401)                              \
  X(ReplicaBoundExceeded, 402)                                  \
  X(AntiAffinityUnsatisfiable, 403)                             \
  X(IsolationDomainExhausted, 404)                              \
  X(PlanInfeasible, 405)                                        \
  X(PlanGenerationRegressed, 406)                               \
  X(RollbackUnavailable, 407)                                   \
  X(IntentNotAccepted, 408)                                     \
  X(NoPlacementChange, 409)                                     \
  X(ExclusiveScopeRequired, 410)                                \
  X(StagingInfeasible, 411)                                     \
                                                                \
  /* ---- 500..599 lifecycle ---- */                            \
  X(LifecycleConflict, 500)                                     \
  X(InstanceNotFound, 501)                                      \
  X(ServiceNotFound, 502)                                       \
  X(WithdrawalInProgress, 503)                                  \
  X(QuiesceIncomplete, 504)                                     \
  X(VerifiedEffectMissing, 505)                                 \
  X(AcknowledgementOnly, 506)                                   \
  X(CancelledAttemptReport, 507)                                \
  X(EffectReportUnmatched, 508)                                 \
  X(EffectPartial, 509)                                         \
  X(EffectFailed, 510)                                          \
  X(RestartFenced, 511)                                         \
  X(DpuLost, 512)                                               \
  X(InstanceNotReady, 513)                                      \
  X(QuiesceRequested, 514)                                      \
  X(Withdrawn, 515)                                             \
  X(Superseded, 516)                                            \
  X(Cancelled, 517)                                             \
                                                                \
  /* ---- 700..799 persistence ---- */                          \
  X(StoreNotFound, 700)                                         \
  X(StoreCorrupt, 701)                                          \
  X(StoreTruncated, 702)                                        \
  X(StoreVersionIncompatible, 703)                              \
  X(StoreSemanticsIncompatible, 704)                            \
  X(StoreIntegrityFailure, 705)                                 \
  X(StoreRecordOversized, 706)                                  \
  X(StoreIoFailure, 707)                                        \
  X(StoreRecoveryRefused, 708)                                  \
  X(StoreAlreadyOpen, 709)                                      \
  X(StoreTornTailRecovered, 710)                                \
  X(StoreCompacted, 711)                                        \
  X(StoreCreated, 712)                                          \
  X(StoreReopened, 713)                                         \
  X(StoreClosed, 714)                                           \
                                                                \
  /* ---- 800..899 transport ---- */                            \
  X(ProtocolViolation, 800)                                     \
  X(FrameOversized, 801)                                        \
  X(FrameTruncated, 802)                                        \
  X(UnknownOperation, 803)                                      \
  X(RequestQueueFull, 804)                                      \
  X(ResponseQueueFull, 805)                                     \
  X(ConnectionClosed, 806)                                      \
  X(TransportFailure, 807)                                      \
  X(RequestIdReused, 808)                                       \
  X(ServerBusy, 809)                                            \
  X(ShutdownInProgress, 810)                                    \
  X(UnauthorizedPeer, 811)                                      \
  X(HandshakeRejected, 812)                                     \
                                                                \
  /* ---- 900..999 bounds/accounting ---- */                    \
  X(BoundExceeded, 900)                                         \
  X(TruncationDropped, 901)                                     \
  X(EvictionOccurred, 902)                                      \
  X(HistoryTruncated, 903)                                      \
  X(RetryBudgetExhausted, 904)                                  \
  X(QueueFull, 905)                                             \
  X(BatchTooLarge, 906)                                         \
  X(MetadataLimitExceeded, 907)                                 \
  X(DuplicateWindowEvicted, 908)                                \
  X(DepthLimitExceeded, 909)                                    \
  X(InvalidAccounting, 910)

enum class ReasonCode : std::uint16_t {
#define DPUF_REASON_ENUM(name, value) name = value,
  DPUF_REASON_CODES(DPUF_REASON_ENUM)
#undef DPUF_REASON_ENUM
};

/// Stable canonical name of a reason code. Unknown values render as "Unknown".
[[nodiscard]] std::string_view to_string(ReasonCode code) noexcept;

/// Parses the canonical name produced by to_string(). Returns false when the
/// text is not a known reason code.
[[nodiscard]] bool reason_from_string(std::string_view text, ReasonCode& out) noexcept;

/// Overload that lets ReasonCode participate in the archive layer's generic
/// enumeration decoding alongside the DPUF_DECLARE_ENUM types.
[[nodiscard]] inline bool from_string(std::string_view text, ReasonCode& out) noexcept {
  return reason_from_string(text, out);
}

/// Numeric band of a reason code, e.g. "authority" for 100..199.
[[nodiscard]] std::string_view reason_band(ReasonCode code) noexcept;

/// True when the code reports a condition that does not refuse an operation:
/// accepted outcomes plus observability records (truncation, eviction, recovery
/// classification, lifecycle observations).
[[nodiscard]] constexpr bool is_informational(ReasonCode code) noexcept {
  return code == ReasonCode::Ok || code == ReasonCode::DuplicateSuppressed ||
         code == ReasonCode::StoreTornTailRecovered || code == ReasonCode::StoreCompacted ||
         code == ReasonCode::StoreCreated || code == ReasonCode::StoreReopened ||
         code == ReasonCode::StoreClosed || code == ReasonCode::TruncationDropped ||
         code == ReasonCode::EvictionOccurred || code == ReasonCode::HistoryTruncated ||
         code == ReasonCode::DuplicateWindowEvicted || code == ReasonCode::QuiesceRequested ||
         code == ReasonCode::Withdrawn || code == ReasonCode::Superseded ||
         code == ReasonCode::Cancelled || code == ReasonCode::DpuLost ||
         code == ReasonCode::AcknowledgementOnly;
}

/// True when the code refused the requested operation.
[[nodiscard]] constexpr bool is_refusal(ReasonCode code) noexcept { return !is_informational(code); }

/// True only for strictly nominal success. DuplicateSuppressed is not nominal:
/// the work was already done by an earlier delivery, so no new effect occurred.
[[nodiscard]] constexpr bool is_nominal(ReasonCode code) noexcept {
  return code == ReasonCode::Ok;
}

}  // namespace dpu::fabric
