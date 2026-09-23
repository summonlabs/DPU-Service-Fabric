// Hard-kill crash-boundary proof.
//
// Each case starts a real child process that reaches a labelled durable boundary
// and is then terminated with TerminateProcess: no destructors, no flush, no
// unwinding. The parent then reopens the store and checks what the boundary
// actually committed. Nothing here fabricates success across an ambiguous crash.

#include <string>
#include <vector>

#include "dpu/fabric/dpu_fabric.hpp"
#include "support/harness.hpp"
#include "support/process.hpp"
#include "support/scenario.hpp"

namespace dpu::fabric {
namespace {

/// The crash tool's runtime identity. The store records it, and a reopen must
/// present the same identity or it is refused.
RuntimeConfig crash_store_config(const test::ScratchDirectory& store) {
  RuntimeConfig config;
  config.store_id = StoreId::literal("dpufabric-store");
  config.coordinator = OriginId::literal("dpufabric-coordinator");
  config.epoch = CoordinatorEpoch{1};
  config.boot = BootIncarnation{1};
  config.store_directory = store.path();
  return config;
}

struct CrashRun {
  int exit_code{0};
  std::string standard_output{};
};

/// Runs the crash tool and joins it. The exit code is the kill code, which proves
/// the process died at the boundary rather than returning normally.
Result<CrashRun> run_crash(const test::ScratchDirectory& store, const std::string& boundary,
                           const std::string& events_file) {
  std::vector<std::string> arguments = {"crash", "--store", store.path(), "--boundary", boundary};
  if (!events_file.empty()) {
    arguments.push_back("--events");
    arguments.push_back(events_file);
  }
  Result<test::ChildProcess> child =
      test::spawn_process(test::sibling_binary("dpufabric.exe"), arguments);
  if (!child) return child.status();
  Result<int> code = child.value().join();
  if (!code) return code.status();
  CrashRun run;
  run.exit_code = code.value();
  return run;
}

/// Writes the standard synthetic world plus a two-replica intent as JSON.
std::string write_world_events(const test::ScratchDirectory& store) {
  std::vector<FabricEvent> events = test::standard_world_events(1, 100);
  events.push_back(test::make_event("crash-intent", "world-origin", 50, 1,
                                    IntentSubmitted{test::standard_intent(1, 2)},
                                    EvidenceClass::Synthetic));
  const std::string path = store.path() + "/events.json";
  const Status written = test::write_text_file(path, encode_json(events).to_text(false));
  if (!written.ok()) test::report_failure(__FILE__, __LINE__, written.message());
  return path;
}

DPUF_TEST(crash, before_commit_leaves_no_trace) {
  test::ScratchDirectory store{"before-commit"};
  const std::string events = write_world_events(store);
  Result<CrashRun> run = run_crash(store, "before-commit", events);
  DPUF_REQUIRE(run);
  DPUF_CHECK_EQ(run.value().exit_code, 3);

  RuntimeConfig config = crash_store_config(store);
  Result<std::unique_ptr<FabricRuntime>> reopened = FabricRuntime::open(config);
  DPUF_REQUIRE(reopened);
  const RuntimeState state = reopened.value()->snapshot();
  // Nothing was staged, journaled or applied.
  DPUF_CHECK(!state.policy.generation.valid());
  DPUF_CHECK(!state.topology_generation.valid());
  DPUF_CHECK(state.instances.empty());
  DPUF_CHECK_EQ(state.journaled_events, std::uint64_t{0});
  DPUF_CHECK_OK(reopened.value()->close());
}

DPUF_TEST(crash, after_commit_before_ack_is_durable_exactly_once) {
  test::ScratchDirectory store{"after-commit"};
  const std::string events = write_world_events(store);
  Result<CrashRun> run = run_crash(store, "after-commit-before-ack", events);
  DPUF_REQUIRE(run);
  DPUF_CHECK_EQ(run.value().exit_code, 3);

  RuntimeConfig config = crash_store_config(store);
  Result<std::unique_ptr<FabricRuntime>> first = FabricRuntime::open(config);
  DPUF_REQUIRE(first);
  const RuntimeState state = first.value()->snapshot();
  // The event was durable before the process died, so recovery replays it once.
  DPUF_CHECK_EQ(state.policy.generation.value(), std::uint64_t{1});
  DPUF_CHECK_EQ(state.topology.dpus.size(), std::size_t{3});
  DPUF_CHECK_EQ(state.instances.size(), std::size_t{2});
  const Digest durable = first.value()->durable_digest();
  DPUF_CHECK_OK(first.value()->close());

  // Reopening again must not apply the event a second time.
  RuntimeConfig again = crash_store_config(store);
  Result<std::unique_ptr<FabricRuntime>> second = FabricRuntime::open(again);
  DPUF_REQUIRE(second);
  DPUF_CHECK_EQ(second.value()->durable_digest(), durable);
  DPUF_CHECK_EQ(second.value()->snapshot().instances.size(), std::size_t{2});
  DPUF_CHECK_OK(second.value()->close());
}

DPUF_TEST(crash, acknowledgement_boundary_claims_no_verified_effect) {
  test::ScratchDirectory store{"after-ack"};
  const std::string events = write_world_events(store);
  Result<CrashRun> run = run_crash(store, "after-ack-before-verified-effect", events);
  DPUF_REQUIRE(run);
  DPUF_CHECK_EQ(run.value().exit_code, 3);

  RuntimeConfig config = crash_store_config(store);
  Result<std::unique_ptr<FabricRuntime>> reopened = FabricRuntime::open(config);
  DPUF_REQUIRE(reopened);
  const RuntimeState state = reopened.value()->snapshot();
  DPUF_CHECK_EQ(state.instances.size(), std::size_t{2});
  for (const InstanceState& instance : state.instances) {
    // No effect was ever verified before the crash, and recovery does not invent
    // one. The instance is not serving and holds no open attempt.
    DPUF_CHECK(!instance.serving());
    DPUF_CHECK(instance.verified_digest.is_zero());
    DPUF_CHECK(!(instance.state == LifecycleState::Verified));
    if (instance.current_attempt() != nullptr) {
      DPUF_CHECK(!instance.current_attempt()->open());
    }
  }
  DPUF_CHECK(!reopened.value()->recovery().restart_fenced.empty());
  DPUF_CHECK_OK(reopened.value()->close());
}

DPUF_TEST(crash, interrupted_rotation_keeps_the_previous_snapshot) {
  test::ScratchDirectory store{"rotation"};
  {
    // Establish a committed snapshot first.
    RuntimeConfig config = crash_store_config(store);
    Result<std::unique_ptr<FabricRuntime>> seed = FabricRuntime::open(config);
    DPUF_REQUIRE(seed);
    const std::vector<FabricEvent> events = test::standard_world_events(1, 100);
    DPUF_CHECK(seed.value()->submit(events).has_value());
    DPUF_CHECK_OK(seed.value()->checkpoint());
    DPUF_CHECK_OK(seed.value()->close());
  }
  const std::string events = store.path() + "/events.json";
  DPUF_CHECK_OK(test::write_text_file(events, "[]"));
  Result<CrashRun> run = run_crash(store, "during-rotation", events);
  DPUF_REQUIRE(run);
  DPUF_CHECK_EQ(run.value().exit_code, 3);

  RuntimeConfig config = crash_store_config(store);
  Result<std::unique_ptr<FabricRuntime>> reopened = FabricRuntime::open(config);
  DPUF_REQUIRE(reopened);
  // The interrupted temporary snapshot is discarded and the committed snapshot
  // is used.
  DPUF_CHECK(reopened.value()->recovery().recovered_from_store);
  DPUF_CHECK_EQ(reopened.value()->snapshot().topology.dpus.size(), std::size_t{3});
  DPUF_CHECK_OK(reopened.value()->close());
  DPUF_CHECK_OK(reopened.value()->close());
}

DPUF_TEST(crash, shutdown_boundary_keeps_the_committed_state) {
  test::ScratchDirectory store{"shutdown"};
  const std::string events = write_world_events(store);
  Result<CrashRun> run = run_crash(store, "during-shutdown", events);
  DPUF_REQUIRE(run);
  DPUF_CHECK_EQ(run.value().exit_code, 3);

  RuntimeConfig config = crash_store_config(store);
  Result<std::unique_ptr<FabricRuntime>> reopened = FabricRuntime::open(config);
  DPUF_REQUIRE(reopened);
  DPUF_CHECK_EQ(reopened.value()->snapshot().instances.size(), std::size_t{2});
  DPUF_CHECK_EQ(reopened.value()->snapshot().policy.generation.value(), std::uint64_t{1});
  DPUF_CHECK_OK(reopened.value()->close());
}

}  // namespace
}  // namespace dpu::fabric
