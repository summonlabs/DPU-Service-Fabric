# DPU Service Fabric

A standalone, vendor-neutral C++20 runtime that deploys and governs
infrastructure services on DPUs. It owns service placement intent, lifecycle
authority, dependency and capability validation, rollout, withdrawal and verified
state. It does not implement the services, DPU firmware, vendor deployment SDKs,
packet forwarding, storage datapaths, security appliances or hardware telemetry
collectors.

Copyright 2026 Summon Software Labs. Apache License 2.0.

## What it does

Given evidence about DPUs (topology, capabilities, health), declarations of
services and their dependencies, policy from an adjacent authority, and an
operator's placement intent, the runtime:

* evaluates **placement eligibility** deterministically against fresh evidence;
* builds a **dependency-aware deployment plan** with bounded replicas,
  anti-affinity, isolation and staged rollout;
* **authorizes** attempts with fencing tokens and keeps exclusive ownership of
  devices single-owner;
* tracks the **lifecycle** of every instance from authorized to verified, with
  acknowledgement, loss, replacement, quiesce and withdrawal as explicit states;
* verifies **effects** against matching fences, attempts and fresh evidence;
* persists accepted state in a versioned, integrity-checked store and recovers
  conservatively after a crash or restart;
* explains every decision and exports canonical machine-readable state.

## Build

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix <prefix>
```

Requirements: CMake 3.25 or newer, a C++20 compiler. On Windows the build is
validated with MSVC 19.44 (`/W4 /WX /permissive-`). There are no third-party
dependencies: SHA-256, the canonical binary archive, the JSON reader/writer, the
test harness and the socket layer are all part of this repository.

Options: `DPUF_BUILD_TESTS`, `DPUF_BUILD_TOOLS`, `DPUF_BUILD_BENCHMARKS`,
`DPUF_BUILD_SHARED`, `DPUF_WARNINGS_AS_ERRORS`, `DPUF_ENABLE_ASAN`.

## Use

```cpp
#include "dpu/fabric/dpu_fabric.hpp"

dpu::fabric::RuntimeConfig config;
config.store_id = dpu::fabric::StoreId::literal("my-store");
config.coordinator = dpu::fabric::OriginId::literal("my-coordinator");
config.epoch = dpu::fabric::CoordinatorEpoch{1};
config.boot = dpu::fabric::BootIncarnation{1};
config.store_directory = "var/fabric";   // optional: enables durability

auto runtime = dpu::fabric::FabricRuntime::open(config);
runtime.value()->submit(events);          // topology, policy, declarations, intent
auto plan = runtime.value()->latest_plan();
auto digest = runtime.value()->state_digest();
runtime.value()->close();
```

An independent downstream project that consumes the installed package with
`find_package(DPUServiceFabric)` lives in `examples/downstream`.

## Tool

`dpufabric` is the operator and inspection surface, and the harness used by the
independent-process tests:

```
dpufabric version
dpufabric verify   --store DIR
dpufabric export   --store DIR [--compact]
dpufabric explain  --store DIR [--instance ID]
dpufabric plan     --topology F --policy F --services F --group F --intent F [--dependencies F]
dpufabric replay   --events F [--store DIR]
dpufabric serve    (--listen HOST:PORT | --inherited-socket N) [--store DIR] [--workers N]
dpufabric client   --connect HOST:PORT (ping|query|export|submit FILE|shutdown)
dpufabric crash    --store DIR --boundary NAME [--events FILE]
```

## Documentation

* `docs/ARCHITECTURE.md` - components, data flow, determinism and concurrency.
* `docs/BOUNDARIES.md` - the exact systems boundary and adjacent runtimes.
* `docs/STATES.md` - state model, generations, fences and reason codes.
* `docs/PERSISTENCE.md` - on-disk format, commit rules and recovery classes.
* `docs/PROOF.md` - every proof obligation and the test that discharges it.
* `docs/VALIDATION.md` - builds, sanitizers, benchmarks and REAL/SYNTHETIC/UNSUPPORTED.

## Validation summary

* Debug and Release builds with MSVC `/W4 /WX /permissive-`; 105 tests across 10
  suites, all green in both configurations.
* The whole suite is green under AddressSanitizer. LeakSanitizer is not available
  in the MSVC sanitizer runtime on Windows, so leak checking is not claimed.
* Independent OS processes exchange the framed protocol over real TCP sockets;
  hard process kills at four durable boundaries are recovered and classified.
* Benchmarks measure completed work and assert completion.

## Status

Everything described here is implemented and tested. Evidence produced by the
test suite and benchmarks is SYNTHETIC: no DPU, SmartNIC, switch, RDMA fabric or
vendor protocol was exercised, and no such validation is claimed. See
`docs/VALIDATION.md` for the REAL / SYNTHETIC / UNSUPPORTED breakdown.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
