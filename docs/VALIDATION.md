# Validation

## Builds

| Configuration | Toolchain | Warnings | Result |
| --- | --- | --- | --- |
| Debug | MSVC 19.44 (Visual Studio 2022 Build Tools), CMake 4.3, Ninja | `/W4 /WX /permissive- /utf-8 /Zc:__cplusplus` | build clean, 105 tests green |
| Release | same | same | build clean, 105 tests green |
| Debug + AddressSanitizer | same, `DPUF_ENABLE_ASAN=ON` (`/fsanitize=address /Zi`) | same | build clean, 105 tests green, no sanitizer report |

Third-party code is not part of this repository, so the strict warning policy
applies to first-party code only. Warning flags are `PRIVATE` to each target and
are proven not to leak into consumers: the downstream project fails its configure
step if the exported target carries compile options or definitions.

Not claimed: any compiler other than MSVC was run. Not claimed: LeakSanitizer or
any other leak checker - the MSVC sanitizer runtime on Windows does not provide
one, and `detect_leaks` is not available there.

## Test inventory

10 suites, 105 tests.

| Suite | Tests |
| --- | --- |
| `test_core` | 14 |
| `test_model` | 13 |
| `test_planning` | 20 |
| `test_lifecycle` | 14 |
| `test_persistence` | 13 |
| `test_transport` | 6 |
| `test_crash` | 5 |
| `test_concurrency` | 7 |
| `test_property` | 4 |
| `test_adversarial` | 9 |

| Suite | File | Covers |
| --- | --- | --- |
| `test_core` | `tests/unit/test_core.cpp` | reason codes, checked arithmetic, SHA-256 vectors, strict JSON, canonical archives, bounded accounting |
| `test_model` | `tests/unit/test_model.cpp` | identities, versions, capability sets, plan sealing, event identity, fences, authority |
| `test_planning` | `tests/unit/test_planning.cpp` | eligibility, dependency graph, authority registry, planner determinism, bounds |
| `test_lifecycle` | `tests/integration/test_lifecycle.cpp` | the lifecycle proof obligations end to end |
| `test_persistence` | `tests/integration/test_persistence.cpp` | round-trip, recovery classification, corruption, rotation, locking |
| `test_transport` | `tests/integration/test_transport.cpp` | the framed protocol across independent OS processes |
| `test_crash` | `tests/integration/test_crash.cpp` | hard process kills at durable boundaries |
| `test_concurrency` | `tests/integration/test_concurrency.cpp` | concurrent ingest and query, repeated start/stop, cancellation, bounds |
| `test_property` | `tests/integration/test_property.cpp` | seeded randomized worlds and invariants, order independence, digest stability |
| `test_adversarial` | `tests/integration/test_adversarial.cpp` | malformed, oversized, stale, duplicated and impossible input |

There are no timeouts anywhere in the suite: no `sleep`, no polling, no
`timeout` parameter. Synchronisation is by thread join, process join, socket
completion and protocol completion. A test that hangs would be treated as a defect.

## Techniques used

* **Unit** - core primitives, model validation, engines.
* **Integration** - whole-runtime event streams over the public API.
* **End-to-end** - the CLI tool and the framed protocol in child processes.
* **Property / invariant** - 40 seeded randomized worlds plus invariant checks;
  31 seeded shuffled-order differential runs; 41 seeded hostile-event runs.
* **Differential** - identical event sets applied in shuffled staging order and
  compared by state digest.
* **Adversarial** - every truncation prefix, single-byte flips, hostile JSON,
  hostile frames, damaged journals, oversized declarations, impossible requests.
* **Concurrency** - concurrent staging, concurrent submission of the same batch,
  concurrent queries during mutation, close with work in flight.
* **Restart / recovery** - clean reopen, torn tail, mid-file corruption,
  incompatible format and semantics, truncation, rotation interruption, repeated
  open/close cycles, exclusive ownership.
* **Independent-process transport** - socket activation, real TCP, hostile
  connections from a raw socket, graceful shutdown and join.
* **Crash** - `TerminateProcess` at four durable boundaries.

## Benchmarks

`dpuf_benchmarks` measures **completed** work and asserts completion before
reporting: every event counted was journaled and applied, every plan counted was
sealed with all replicas placed, and the durable run asserts that the checkpoint
completed. Enqueue latency is not measured because it would measure nothing about
this runtime. The fixtures are SYNTHETIC.

Run on the validation machine (MSVC Release, x64, 64 synthetic devices, 32
replicas, 5 seeded events and 20 plan iterations):

```
events.ingested_and_applied        ops=5   all events applied and instance set complete
plans.sealed_with_every_replica    ops=20  every plan sealed with all replicas placed
durable.events_journaled           ops=5   every event journaled, applied and checkpointed
```

Absolute figures depend on the machine; the assertion is the point.

## Package and downstream proof

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix <prefix>
cmake -S examples/downstream -B consumer -DCMAKE_PREFIX_PATH=<prefix>
cmake --build consumer && ./consumer/dpuf_consumer
```

The downstream project includes only installed headers, links only the exported
`dpu::fabric` target, and fails its configure step if the package leaks compile
options, definitions or private include directories.

## REAL / SYNTHETIC / UNSUPPORTED

**REAL.** The process, thread, socket, file-system and crash behaviour exercised
by the suite is real: real OS processes, real TCP sockets, real files, real
`TerminateProcess` kills, real thread scheduling.

**SYNTHETIC.** Every device, capability, evidence record, service declaration,
policy value, effect report and topology in the tests and benchmarks is
synthetic. No DPU, SmartNIC, NIC, switch, ASIC, RDMA or InfiniBand device was
present or exercised. The fixtures label themselves: evidence produced by the
test fixtures carries `EvidenceClass::Synthetic` and source
`synthetic.fixture`/ `synthetic.benchmark`.

**UNSUPPORTED.** This runtime makes no claim about, and contains no code for:

* real DPU firmware behaviour, flashing or firmware update;
* vendor deployment SDKs or vendor device protocols;
* packet forwarding, storage datapaths or security appliance behaviour;
* hardware telemetry collection or device health probing;
* RDMA, InfiniBand, NVLink, multi-host fabrics or CUDA;
* switch or ASIC behaviour of any kind.

Real-hardware validation of the runtime's *own* logic is not required and is not
claimed: the runtime's contract is about accepting evidence, planning, granting
authority and recording verified state, all of which are proven with synthetic
evidence through the real public interface.

## Limitations

* Single-process mutation is serialized by one mutex; the runtime is not a
  distributed consensus system and does not claim to be. Distributed agreement
  about which coordinator holds which epoch is the adjacent authority's job.
* The reorder buffer requires a release horizon: an event older than the released
  watermark is refused (`LateEventRejected`) rather than reordered.
* The transport is a bounded request/response protocol over TCP with a fixed
  worker pool; there is no back-pressure negotiation beyond refusing connections
  when the queue is full.
* The store keeps one snapshot plus a bounded journal; there is no incremental
  compaction of a snapshot larger than the record bound, which is refused instead.
* Leak detection is not claimed (see above).
