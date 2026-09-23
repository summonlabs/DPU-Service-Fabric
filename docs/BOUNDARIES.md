# Systems boundary

## Owned by this runtime

* **Placement intent**: accepting, validating, sequencing and refusing operator or
  controller intent to deploy, roll out, scale, replace, withdraw or roll back
  services on DPUs.
* **Lifecycle authority**: which instance may act, under which fence, in which
  coordinator epoch and boot incarnation, and when that authority ends.
* **Dependency and capability validation**: whether a service may run on a device
  given declared capabilities, compatibility, isolation and dependencies.
* **Rollout, withdrawal and replacement intent**: staged rollout shape, quiesce,
  withdrawal and replacement as first-class operations with verified completion.
* **Verified state**: what has actually been proven about each instance, with the
  evidence and generation that proved it, and what remains unproven.
* **Persistence and recovery** of that accepted state, including conservative
  restart behaviour and restart fencing.

## Not owned by this runtime

The runtime deliberately does not implement, and makes no claim about:

* the services themselves, their binaries, configuration or datapaths;
* DPU firmware, boot flows, flashing or firmware update;
* vendor deployment SDKs or vendor-specific device management protocols;
* packet forwarding, storage datapaths, security appliances;
* hardware telemetry collection, counters, or device health probing;
* policy authorship, topology discovery, or authority decisions.

## Adjacent runtimes

Adjacent systems interact with this runtime only through evidence, policy and
reports. Nothing else is required of them, and none of their internals are
assumed.

| Adjacent runtime | Provides | Enters as | Trust |
| --- | --- | --- | --- |
| Topology / inventory | device inventory, attachments, isolation domains | `TopologyObserved`, `DpuAttached` | Evidence, with generation and provenance; a device that disappears is fenced, not assumed healthy. |
| Capability evidence | device profile and capability values | `CapabilityObserved` | Evidence; carried with a capability generation. Unknown capabilities block claims that need them. |
| Health observation | health state of a device | `HealthObserved` | Evidence; carries an observation instant and a validity horizon. Health never reattaches a lost device. |
| Policy and authority | policy values, coordinator epoch | `PolicyAdvanced`, `CoordinatorAdvanced` | Authoritative for its own generation; an epoch advance revokes all authority issued before it. |
| Execution / effect reporting | acknowledgement and effect reports | `ExecutionAcknowledged`, `EffectReported` | Reports; only a matching, fresh, successful report verifies an effect. |

## Evidence classes

Every evidence reference carries an `EvidenceClass`:

* **Real** - produced by an adjacent runtime observing hardware.
* **Synthetic** - produced by a fixture, simulator or test.
* **Unsupported** - a claim this runtime cannot evaluate; it is treated as unknown
  and blocks decisions that depend on it.

Policy can refuse synthetic evidence outright
(`PolicyState::allow_synthetic_evidence`), and the eligibility evaluator refuses
evidence marked `Unsupported` rather than guessing.

## What this means in practice

A consumer of this runtime gets placement decisions, authority, lifecycle state,
verified effects and explanations. It does not get service implementations,
device drivers, or a data plane, and it should not expect this runtime to
substitute for the systems that provide evidence to it. Conversely, this runtime
never fabricates device facts: when evidence is missing, stale, conflicting or
unsupported, the answer is an explicit refusal or an explicit unknown.
