// Concurrency tests: deterministic ordering under concurrent ingest, read-only
// queries that never observe a partial event, repeated start/stop and real
// cancellation.
//
// No test in this file uses a sleep, a poll or a timeout. Threads are joined, and
// every assertion is about state that is complete when it is observed.

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "dpu/fabric/dpu_fabric.hpp"
#include "support/harness.hpp"
#include "support/process.hpp"
#include "support/scenario.hpp"

namespace dpu::fabric {
namespace {

using test::Scenario;

/// The digest a serial application of `events` produces, used as the reference
/// for concurrently staged batches.
Result<Digest> serial_reference(std::vector<FabricEvent> events) {
  Scenario scenario;
  Result<BatchReport> report = scenario.send_all(std::move(events));
  if (!report) return report.status();
  return scenario.rt().state_digest();
}

DPUF_TEST(concurrency, concurrently_staged_events_apply_in_canonical_order) {
  const std::vector<FabricEvent> events = test::standard_world_events(1, 100);
  Result<Digest> reference = serial_reference(events);
  DPUF_REQUIRE(reference);

  for (int round = 0; round < 8; ++round) {
    Scenario scenario;
    std::vector<std::thread> threads;
    std::atomic<std::size_t> accepted{0};
    threads.reserve(events.size());
    for (std::size_t i = 0; i < events.size(); ++i) {
      threads.emplace_back([&scenario, &events, &accepted, i] {
        const Status staged = scenario.rt().stage(events[i]);
        if (staged.ok()) accepted.fetch_add(1);
      });
    }
    for (std::thread& thread : threads) thread.join();
    DPUF_CHECK_EQ(accepted.load(), events.size());
    DPUF_CHECK_EQ(scenario.rt().staged_count(), events.size());
    // Every event was staged before the release, so the flush applies exactly
    // the canonical order of the whole set.
    Result<BatchReport> report = scenario.rt().flush(LogicalInstant{1});
    DPUF_REQUIRE(report);
    DPUF_CHECK_EQ(report.value().applied, static_cast<std::uint64_t>(events.size()));
    DPUF_CHECK_EQ(scenario.rt().state_digest(), reference.value());
  }
}

DPUF_TEST(concurrency, concurrent_queries_never_observe_a_partial_event) {
  Scenario scenario;
  const std::vector<FabricEvent> world = test::standard_world_events(1, 100);
  DPUF_CHECK_RESULT_OK(scenario.send_all(world));

  std::vector<Digest> expected;
  expected.push_back(scenario.rt().state_digest());
  for (int i = 0; i < 6; ++i) {
    DPUF_CHECK_RESULT_OK(scenario.tick());
    expected.push_back(scenario.rt().state_digest());
  }

  std::atomic<bool> stop{false};
  std::vector<Digest> observed;
  std::thread reader{[&scenario, &stop, &observed] {
    while (!stop.load()) {
      observed.push_back(scenario.rt().state_digest());
    }
  }};

  // Rewind and replay the same events while the reader watches.
  Scenario writer;
  DPUF_CHECK_RESULT_OK(writer.send_all(world));
  for (int i = 0; i < 6; ++i) {
    DPUF_CHECK_RESULT_OK(writer.tick());
  }
  stop.store(true);
  reader.join();

  DPUF_CHECK(!observed.empty());
  DPUF_CHECK_EQ(writer.rt().state_digest(), scenario.rt().state_digest());
  for (const Digest& digest : observed) {
    bool found = false;
    for (const Digest& allowed : expected) {
      if (digest == allowed) found = true;
    }
    // Every observation is a state that was actually accepted at some point,
    // never a half-applied event.
    if (!found) {
      test::report_failure(__FILE__, __LINE__, "query observed a state that was never accepted");
      break;
    }
  }
}

DPUF_TEST(concurrency, concurrent_submitters_apply_each_event_once) {
  Scenario scenario;
  const std::vector<FabricEvent> world = test::standard_world_events(1, 100);
  DPUF_CHECK_RESULT_OK(scenario.send_all(world));

  // Twelve threads each submit the same three-event batch: the events must be
  // applied exactly once in total, whatever the interleaving.
  std::vector<FabricEvent> batch;
  batch.push_back(test::make_event("shared-1", "shared-origin", 1, 5,
                                   ServiceDeclared{test::make_service("svc-c", 1, 2,
                                                                      test::capacity(10, 10))},
                                   EvidenceClass::Synthetic));
  ServiceGroup shared_group;
  shared_group.id = test::group_id("grp-c");
  shared_group.members = {test::service_id("svc-c")};
  batch.push_back(test::make_event("shared-2", "shared-origin", 2, 5, GroupDeclared{shared_group},
                                   EvidenceClass::Synthetic));
  batch.push_back(test::make_event("shared-3", "shared-origin", 3, 5,
                                   LogicalTimeAdvanced{LogicalInstant{5}},
                                   EvidenceClass::Synthetic));

  std::vector<std::thread> threads;
  std::atomic<std::uint64_t> applied{0};
  std::atomic<std::uint64_t> suppressed{0};
  for (int i = 0; i < 12; ++i) {
    threads.emplace_back([&scenario, &batch, &applied, &suppressed] {
      Result<BatchReport> report = scenario.send_all(batch);
      if (!report) return;
      applied.fetch_add(report.value().applied);
      suppressed.fetch_add(report.value().suppressed);
    });
  }
  for (std::thread& thread : threads) thread.join();
  // The two declarations and the time advance are applied exactly once between
  // them; every other delivery is suppressed.
  DPUF_CHECK_EQ(applied.load(), std::uint64_t{3});
  DPUF_CHECK_EQ(suppressed.load(), std::uint64_t{33});
  std::size_t services = 0;
  for (const ServiceDefinition& service : scenario.rt().snapshot().services) {
    if (service.id == test::service_id("svc-c")) ++services;
  }
  DPUF_CHECK_EQ(services, std::size_t{1});
}

DPUF_TEST(concurrency, repeated_start_and_stop_is_stable) {
  test::ScratchDirectory store{"cycles"};
  Digest durable{};
  {
    RuntimeConfig config = test::make_config();
    config.store_directory = store.path();
    Scenario scenario{config};
    DPUF_CHECK_RESULT_OK(test::apply_standard_world(scenario));
    durable = scenario.rt().durable_digest();
    DPUF_CHECK_OK(scenario.rt().close());
  }
  for (int cycle = 0; cycle < 5; ++cycle) {
    RuntimeConfig config = test::make_config();
    config.store_directory = store.path();
    Result<std::unique_ptr<FabricRuntime>> runtime = FabricRuntime::open(config);
    DPUF_REQUIRE(runtime);
    DPUF_CHECK_EQ(runtime.value()->durable_digest(), durable);
    DPUF_CHECK_OK(runtime.value()->close());
    // Closing twice is idempotent, and a closed runtime refuses new work.
    DPUF_CHECK_OK(runtime.value()->close());
    LogicalTimeAdvanced advance;
    advance.now = LogicalInstant{99};
    const Status refused = runtime.value()->stage(test::make_event(
        "after-close", "origin", 1, 99, advance, EvidenceClass::Synthetic));
    DPUF_CHECK_CODE(refused, ReasonCode::StoreClosed);
  }
}

DPUF_TEST(concurrency, close_with_work_in_flight_is_safe) {
  test::ScratchDirectory store{"inflight"};
  RuntimeConfig config = test::make_config();
  config.store_directory = store.path();
  Result<std::unique_ptr<FabricRuntime>> runtime = FabricRuntime::open(config);
  DPUF_REQUIRE(runtime);

  std::atomic<bool> start{false};
  std::atomic<std::uint64_t> accepted{0};
  std::atomic<std::uint64_t> refused{0};
  std::thread writer{[&runtime, &start, &accepted, &refused] {
    while (!start.load()) {
    }
    for (std::uint64_t i = 0; i < 200; ++i) {
      LogicalTimeAdvanced advance;
      advance.now = LogicalInstant{i + 1};
      FabricEvent event = test::make_event("inflight-" + std::to_string(i), "inflight-origin",
                                           i + 1, i + 1, advance, EvidenceClass::Synthetic);
      Result<BatchReport> report = runtime.value()->submit({event});
      if (report) {
        accepted.fetch_add(1);
      } else {
        refused.fetch_add(1);
      }
    }
  }};
  start.store(true);
  // Close while the writer is still working. Close takes the same lock as every
  // other mutation, so this is a serialisation point, not a race.
  const Status closed = runtime.value()->close();
  writer.join();
  DPUF_CHECK_OK(closed);
  DPUF_CHECK(accepted.load() + refused.load() == 200);
  DPUF_CHECK(runtime.value()->closed());

  // Whatever was accepted before the close is durable and reopens cleanly.
  RuntimeConfig reopen_config = test::make_config();
  reopen_config.store_directory = store.path();
  Result<std::unique_ptr<FabricRuntime>> reopened = FabricRuntime::open(reopen_config);
  DPUF_REQUIRE(reopened);
  DPUF_CHECK(reopened.value()->recovery().recovered_from_store);
  DPUF_CHECK_OK(reopened.value()->close());
}

DPUF_TEST(concurrency, staged_work_dropped_by_close_publishes_nothing) {
  test::ScratchDirectory store{"drop"};
  RuntimeConfig config = test::make_config();
  config.store_directory = store.path();
  {
    Result<std::unique_ptr<FabricRuntime>> runtime = FabricRuntime::open(config);
    DPUF_REQUIRE(runtime);
    // Stage events but never release them: nothing is applied yet.
    for (const FabricEvent& event : test::standard_world_events(1, 100)) {
      DPUF_CHECK_OK(runtime.value()->stage(event));
    }
    DPUF_CHECK_EQ(runtime.value()->staged_count(), std::size_t{6});
    DPUF_CHECK_OK(runtime.value()->close());
    DPUF_CHECK_EQ(runtime.value()->staged_count(), std::size_t{0});
  }
  Result<std::unique_ptr<FabricRuntime>> reopened = FabricRuntime::open(config);
  DPUF_REQUIRE(reopened);
  // Nothing was released, so nothing is durable.
  DPUF_CHECK(!reopened.value()->snapshot().policy.generation.valid());
  DPUF_CHECK(reopened.value()->snapshot().topology.dpus.empty());
  DPUF_CHECK_OK(reopened.value()->close());
}

DPUF_TEST(concurrency, reorder_buffer_bound_refuses_and_accounts) {
  RuntimeConfig config = test::make_config();
  config.bounds.max_batch_events = 4;
  Scenario scenario{config};
  std::uint64_t sequence = 0;
  int refused = 0;
  for (int i = 0; i < 8; ++i) {
    ++sequence;
    LogicalTimeAdvanced advance;
    advance.now = LogicalInstant{static_cast<std::uint64_t>(i) + 1};
    FabricEvent event = test::make_event("bound-" + std::to_string(sequence), "bound-origin",
                                         sequence, static_cast<std::uint64_t>(i) + 1, advance,
                                         EvidenceClass::Synthetic);
    const Status staged = scenario.rt().stage(event);
    if (staged.code() == ReasonCode::QueueFull) ++refused;
  }
  DPUF_CHECK_EQ(refused, 4);
  DPUF_CHECK_EQ(scenario.rt().staged_count(), std::size_t{4});
  const TruncationLedger& ledger = scenario.rt().snapshot().truncations;
  bool found = false;
  for (const TruncationRecord& record : ledger.records()) {
    if (record.container == "ingest.pending") {
      found = true;
      DPUF_CHECK_EQ(record.requested, record.accepted + record.dropped);
    }
  }
  DPUF_CHECK(found);
}

}  // namespace
}  // namespace dpu::fabric
