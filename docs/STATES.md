# State model

## Typed identities

Identities are distinct types, not strings: `DpuId`, `ServiceId`,
`ServiceGroupId`, `InstanceId`, `DependencyId`, `HostAttachmentId`,
`IsolationDomainId`, `EvidenceId`, `PlanId`, `OriginId`, `StoreId`,
`CapabilityKey`, `ExclusiveScopeId`, `OperationId`, `EventId`. Textual
identities are validated at the boundary: 1..64 characters from
`[A-Za-z0-9._:-]` starting with an alphanumeric character. A `DpuId` can never be
passed where a `ServiceId` is expected, and an absent identity is distinct from
an invalid one.

## Generations, epochs and fences

| Quantity | Meaning | Regression |
| --- | --- | --- |
| `TopologyGeneration` | accepted device inventory revision | refused (`StaleGeneration`) |
| `CapabilityGeneration` | per-device capability evidence revision | refused (`StaleGeneration`) |
| `PolicyGeneration` | policy revision from the adjacent authority | refused (`StaleGeneration`) |
| `DeploymentGeneration` | accepted placement intent revision | refused (`PlanGenerationRegressed`) |
| `CoordinatorEpoch` | coordinator incarnation | refused (`EpochRegressed`) |
| `BootIncarnation` | process incarnation of this runtime | advanced on every reopen |
| `AttemptNumber` | attempt counter within an instance | strictly increasing |
| `InstanceIncarnation` | replacement counter for an instance | strictly increasing |
| `FenceToken` | `(epoch, boot, serial)` authority token | a stale token is refused (`FenceMismatch`) |

## Instance lifecycle

```
                            intent accepted
                                  |
                                  v
   Absent --> Planned --> Authorized --> (acknowledge) --> Acknowledged
                                  |                              |
                                  |                              v
                                  |                         Unverified
                                  v                              |
                              Deploying ---(effect verified)---> Verified
                                  |                              |
                                  |  (effect failed)             |  (device lost)
                                  v                              v
                                Failed                          Lost
                                  ^                              |
                                  |         restart fencing      |
                                  +------------------------------+
                                  |
                           Quiescing --> (withdrawal verified) --> Withdrawn
                                  |
                              Superseded / Cancelled
```

Rules that hold unconditionally:

* **Submission is not completion.** Accepting an intent creates attempts in the
  `Authorized` state. Nothing is running until an effect is verified.
* **Acknowledgement is not a verified effect.** An acknowledgement moves the
  attempt to `Acknowledged` and records the `AcknowledgementOnly` reason code.
  The instance is not serving.
* **Serving requires both proof and freshness.** `InstanceState::serving()` is
  true only for a `Verified` instance with fresh `Healthy` evidence. Either half
  alone is not enough.
* **Loss is never healthy continuity.** `DpuLost` marks the device lost, sets
  health to `Unknown`, marks it not fresh, fences every instance on it and
  cancels open attempts. Health evidence observed before the loss is refused as
  stale; reattachment requires fresh evidence.
* **Cancellation is real.** Quiescing cancels open attempts, and a cancelled
  attempt can never publish an effect (`AttemptNotOpen`). A withdrawal plan opens
  a dedicated attempt so the withdrawal itself can still be proven.
* **Replacement reissues authority.** A replacement bumps the incarnation, fences
  the previous effect and issues a new attempt with a new fence, so the previous
  attempt's report is refused (`AttemptSuperseded`).

## Effect verification

An effect report is accepted only when all of the following hold:

1. the instance exists;
2. the attempt number matches the instance's current attempt
   (otherwise `AttemptSuperseded`);
3. the fence token is the instance's current token in the current epoch and boot
   incarnation (otherwise `FenceMismatch`);
4. the attempt is still open (otherwise `AttemptNotOpen`);
5. evidence is present (otherwise `EvidenceMissing`);
6. the evidence was observed at or after the instance's effect fence instant
   (otherwise `EvidenceStale`);
7. the evidence is fresh at the report instant (otherwise `EvidenceStale`);
8. the report is not partial (otherwise `EffectPartial`).

Only then does the instance become `Verified`, carrying the effect digest, the
verification instant and the verifying evidence reference.

## Reason codes

Reason codes live in `include/dpu/fabric/core/reason.hpp` and are grouped into
stable bands. Numeric values are part of the wire and persistence contract and
never change once released.

| Band | Meaning |
| --- | --- |
| 0 | success |
| 1..99 | input shape, encoding and versioning |
| 100..199 | identity, generations, epochs, fences and authority |
| 200..299 | device capability, compatibility and evidence quality |
| 300..399 | dependency and declaration topology |
| 400..499 | placement, replica accounting and rollout planning |
| 500..599 | instance lifecycle, attempts and verified effects |
| 700..799 | durability, recovery and store integrity |
| 800..899 | transport framing and protocol |
| 900..999 | bounded resources, truncation and accounting |

`is_informational(code)` distinguishes accepted outcomes and pure observations
(for example `DuplicateSuppressed`, `TruncationDropped`, `StoreTornTailRecovered`)
from refusals. `is_nominal(code)` is true only for `Ok`.

## Unknown is not false

* A capability that cannot be evaluated against fresh evidence is `Unknown`, and
  unknown blocks any claim that depends on it. Eligibility is a *positive*
  determination: a device is eligible only when every requirement was
  affirmatively satisfied.
* Missing evidence never becomes zero, false or success. A device with no fresh
  capacity evidence reports `CapacityUnknown`; a device with no fresh capability
  evidence reports `EvidenceMissing`.
* Planning refuses when no topology or no policy generation has been accepted,
  rather than planning against an empty world.
