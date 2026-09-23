# Architecture

## Shape

```
              adjacent runtimes                        this runtime
  +---------------------------------+     +--------------------------------------+
  | topology / capability / health  | --> |  evidence + provenance + freshness    |
  | policy and authority            | --> |  policy state with a generation       |
  | execution and effect reports    | --> |  effect reports with fences           |
  +---------------------------------+     +------------------+-------------------+
                                                             |
                          +----------------------------------v----------------------------------+
                          |                          FabricRuntime                              |
                          |  staged events -> deterministic total order -> reducer -> state       |
                          +----+----------------+----------------+----------------+---------------+
                               |                |                |                |
                     eligibility      dependency graph     authority/fences    plan + lifecycle
                               |                |                |                |
                          +----v----------------v----------------v----------------v----+
                          |  DeploymentPlan, InstanceState, Decision, Explanation    |
                          +------------------------------+---------------------------+
                                                         |
                                        +----------------v-----------------+
                                        | versioned integrity-checked store |
                                        | framed bounded protocol (TCP)     |
                                        +-----------------------------------+
```

## Layers

| Layer | Headers | Responsibility |
| --- | --- | --- |
| Core | `core/*.hpp` | Strongly typed identities, generations, checked arithmetic, stable reason codes, SHA-256, canonical binary archive, strict JSON, bounded containers and truncation accounting. |
| Model | `model/*.hpp` | Devices, capabilities, compatibility, services, groups, dependencies, intents, instances, attempts, fences, effects, plans, events. One `visit()` per type serves binary and JSON, encode and decode. |
| Engines | `engine/*.hpp` | Eligibility, dependency ordering, authority and fences, the planner, the runtime facade. |
| Durability | `persist/store.hpp` | Versioned framed records, snapshots, rotation, recovery classification, exclusive store ownership. |
| Transport | `transport/*.hpp` | Bounded framed protocol, server with a worker pool, blocking client. |

Everything is in namespace `dpu::fabric`; the umbrella header is
`dpu/fabric/dpu_fabric.hpp`.

## Determinism

Accepted state is a function of the accepted event set.

* Every event carries an event key: `(logical instant, origin, origin epoch,
  origin sequence, event id)`. That order is a total order derived only from the
  event itself.
* `stage()` only buffers. `flush(upto)` releases every staged event with
  `at <= upto` in key order. Staging order and thread interleaving therefore
  cannot change the result.
* An event at or below the released watermark is refused with
  `LateEventRejected`: the reorder window for that instant has closed. A
  duplicate of an already-seen delivery is answered with `DuplicateSuppressed`
  before the watermark rule is applied, so redelivery stays idempotent for as
  long as the bounded delivery window remembers it.
* No wall clock, no random source, no iteration-order dependence and no floating
  point participates in a decision. Freshness is evaluated at a logical instant.
* Canonical digests are SHA-256 over a length-prefixed binary encoding in which
  enumerations are written by name and every object carries its type name.

## Concurrency

* One mutex serializes every mutation of accepted state. Reads take the same
  mutex, so a query never observes a partially applied event.
* The transport server owns a fixed worker pool and a bounded queue of accepted
  connections. The accept loop and the workers are joined before the server
  reports itself stopped. Shutdown closes the listener (which wakes a blocked
  accept) and shuts every active connection down in both directions (which makes
  a blocked receive return), so a connected but silent peer cannot delay or
  deadlock shutdown. Queued connections that were never served are closed without
  executing anything.
* The server takes two locks in exactly one order: the connection mutex may be
  held while the statistics mutex is taken, never the reverse, and no lock is held
  across a worker join, a blocking socket operation or a callback into the
  runtime.
* A connection that disappears mid-request is cancelled: no reply is invented and
  no success is claimed for work whose acknowledgement could not be delivered.
* No lock is held while a callback, an event emission or a worker join could
  re-enter the runtime. The mutable path is: journal append, apply, record
  decision, all under one lock, with no nested acquisition.

## Authority

* A DPU has at most one exclusive owner. `AuthorityRegistry::claim` refuses a
  second exclusive scope with `ExclusiveScopeConflict`.
* A fence token is valid only for the instance it was issued to, in the current
  coordinator epoch and boot incarnation, at the instance's current serial.
  Advancing the epoch or restarting the process therefore invalidates every
  token issued before, and a superseded attempt can never publish an effect.
* Instances carry an effect fence instant. Evidence observed before that instant
  cannot justify a current claim, whatever its own timestamps say.

## Planning

The planner is a pure function of accepted state. It refuses rather than
approximating: an infeasible intent produces no usable plan identity, a plan that
would need more stages than the bound allows is refused with an accounted
truncation, and a device whose capability state is unknown is never selected.

Candidate ordering is by deterministic integer score (capacity headroom in
permille, isolation match, domain spread, pressure penalty) with the canonical
device identity as the tie-break, so a plan is reproducible byte for byte.

## Explanation surface

Every accepted or refused event produces a `Decision` carrying its reason code,
the generations in force (topology, capability, policy, deployment), the epoch and
boot incarnation, the evidence digest, and a bounded set of `Explanation`
records stamped with those generations. `FabricRuntime::explain_last_decision`,
`explain_instance` and `explain_eligibility` expose the same material, and the
canonical export carries the decision history.

## Bounded resources

Every externally influenced allocation is bounded before it is made: devices,
services, instances, batch size, reorder buffer, delivery window, plan history,
decision history, explanations, stages, journal bytes, record size, frame size,
pending connections, capabilities per device, dependencies and text lengths. Each
truncation, eviction or refusal-by-bound is recorded in the truncation ledger with
`requested == accepted + dropped`, and the ledger itself is bounded with an
explicit overflow counter.
