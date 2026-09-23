// Persistence and restart tests: round-trip fidelity, recovery classification,
// corruption handling and the guarantee that a restart never revives liveness.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "dpu/fabric/dpu_fabric.hpp"
#include "support/harness.hpp"
#include "support/scenario.hpp"

namespace dpu::fabric {
namespace {

using test::Scenario;

namespace fs = std::filesystem;

/// A scratch store directory that removes itself.
class TempStore {
 public:
  explicit TempStore(std::string_view label) {
    static std::uint64_t counter = 0;
    ++counter;
    directory_ = (fs::temp_directory_path() /
                  ("dpuf-" + std::string{label} + "-" + std::to_string(counter)))
                     .string();
    std::error_code error;
    fs::remove_all(fs::path{directory_}, error);
    fs::create_directories(fs::path{directory_}, error);
  }
  ~TempStore() {
    std::error_code error;
    fs::remove_all(fs::path{directory_}, error);
  }
  TempStore(const TempStore&) = delete;
  TempStore& operator=(const TempStore&) = delete;

  [[nodiscard]] const std::string& path() const { return directory_; }
  [[nodiscard]] std::string file(std::string_view name) const {
    return (fs::path{directory_} / name).string();
  }

 private:
  std::string directory_{};
};

std::vector<std::uint8_t> read_file(const std::string& path) {
  std::ifstream stream{path, std::ios::binary};
  return std::vector<std::uint8_t>{std::istreambuf_iterator<char>(stream),
                                   std::istreambuf_iterator<char>()};
}

void write_file(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream stream{path, std::ios::binary | std::ios::trunc};
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

/// Writes a journal made of independently encoded event frames. Synthesizing the
/// file is how damage is introduced deterministically, without depending on the
/// runtime's own rotation timing.
void write_journal(const TempStore& store, const std::vector<FabricEvent>& events) {
  std::vector<std::uint8_t> bytes;
  for (const FabricEvent& event : events) {
    const std::vector<std::uint8_t> payload = encode_binary(event);
    const std::vector<std::uint8_t> framed =
        Store::frame(RecordKind::Event, kFormatVersion, kSemanticVersion, payload);
    bytes.insert(bytes.end(), framed.begin(), framed.end());
  }
  write_file(store.file("fabric.journal"), bytes);
}

RuntimeConfig store_config(const TempStore& store) {
  RuntimeConfig config = test::make_config();
  config.store_directory = store.path();
  return config;
}

/// Builds a world, deploys two replicas, verifies both and returns the ids.
std::vector<InstanceId> populate(Scenario& scenario) {
  if (!test::apply_standard_world(scenario)) return {};
  const Result<BatchReport> intent =
      scenario.send(IntentSubmitted{test::standard_intent(scenario.now(), 2)});
  if (!intent || intent.value().applied != 1) return {};
  std::vector<FabricEvent> reports;
  std::vector<InstanceId> ids;
  for (const InstanceState& instance : scenario.rt().snapshot().instances) {
    ids.push_back(instance.id);
    if (instance.current_attempt() == nullptr) continue;
    EffectReported reported;
    reported.report.instance = instance.id;
    reported.report.attempt = instance.current_attempt()->number;
    reported.report.fence = instance.current_attempt()->fence;
    reported.report.kind = EffectKind::Deploy;
    reported.report.status = EffectStatus::Succeeded;
    const std::uint64_t at = scenario.now() + 1 + reports.size();
    reported.report.reported_at = LogicalInstant{at};
    reported.report.evidence = test::make_evidence("ev-effect-" + instance.id.str(),
                                                    EvidenceKind::Execution, LogicalInstant{at}, 40,
                                                    EvidenceClass::Real);
    reported.report.effect_digest = sha256(instance.id.str());
    reports.push_back(scenario.make(reported, at));
  }
  (void)scenario.send_all(reports);
  return ids;
}

DPUF_TEST(persistence, clean_reopen_preserves_the_durable_projection) {
  TempStore store{"clean"};
  Digest durable_before{};
  Digest state_before{};
  std::vector<InstanceId> ids;
  {
    Scenario scenario{store_config(store)};
    DPUF_CHECK_RESULT_OK(test::apply_standard_world(scenario));
    ids = populate(scenario);
    DPUF_CHECK_EQ(ids.size(), std::size_t{2});
    for (const InstanceId& id : ids) {
      DPUF_CHECK_EQ(scenario.rt().instance(id)->state, LifecycleState::Verified);
    }
    durable_before = scenario.rt().durable_digest();
    state_before = scenario.rt().state_digest();
    DPUF_CHECK_OK(scenario.rt().close());
  }
  {
    RuntimeConfig config = store_config(store);
    Result<std::unique_ptr<FabricRuntime>> reopened = FabricRuntime::open(config);
    DPUF_REQUIRE(reopened);
    FabricRuntime& runtime = *reopened.value();
    // The placement-intent projection round-trips exactly.
    DPUF_CHECK_EQ(runtime.durable_digest(), durable_before);
    DPUF_CHECK_NE(runtime.state_digest(), state_before);
    DPUF_CHECK(runtime.boot().value() >= 2);
    DPUF_CHECK(runtime.recovery().recovered_from_store);
    DPUF_CHECK_EQ(runtime.instance_count(), std::size_t{2});
    DPUF_CHECK(!runtime.recovery().restart_fenced.empty());
    for (const InstanceId& id : ids) {
      const std::optional<InstanceState> instance = runtime.instance(id);
      DPUF_CHECK(instance.has_value());
      // Proven liveness does not survive: the instance is no longer verified,
      // holds no open attempt and is not serving.
      DPUF_CHECK_EQ(instance->state, LifecycleState::Unverified);
      DPUF_CHECK(!instance->serving());
      DPUF_CHECK_EQ(instance->fence_reason, ReasonCode::RestartFenced);
      for (const AttemptRecord& attempt : instance->attempts) {
        DPUF_CHECK(attempt.outcome != AttemptOutcome::Open);
      }
      DPUF_CHECK(instance->current_attempt() != nullptr);
      DPUF_CHECK(!instance->current_attempt()->open());
    }
    DPUF_CHECK_OK(runtime.close());
  }
}

DPUF_TEST(persistence, restart_does_not_revive_liveness_or_authority) {
  TempStore store{"restart"};
  FenceToken old_fence{};
  InstanceId first{};
  {
    Scenario scenario{store_config(store)};
    DPUF_CHECK_RESULT_OK(test::apply_standard_world(scenario));
    const std::vector<InstanceId> ids = populate(scenario);
    DPUF_CHECK_EQ(ids.size(), std::size_t{2});
    first = ids[0];
    old_fence = scenario.rt().instance(first)->current_attempt()->fence;
    DPUF_CHECK_OK(scenario.rt().close());
  }
  Scenario scenario{store_config(store)};
  const std::optional<InstanceState> instance = scenario.rt().instance(first);
  DPUF_CHECK(instance.has_value());
  DPUF_CHECK(!instance->serving());
  // A report carrying the pre-restart fence cannot verify anything now.
  scenario.set_now(scenario.rt().clock().ticks + 1);
  EffectReported reported;
  reported.report.instance = first;
  reported.report.attempt = instance->current_attempt()->number;
  reported.report.fence = old_fence;
  reported.report.kind = EffectKind::Deploy;
  reported.report.status = EffectStatus::Succeeded;
  const std::uint64_t at = scenario.now();
  reported.report.reported_at = LogicalInstant{at};
  reported.report.evidence = test::make_evidence("ev-late", EvidenceKind::Execution,
                                                  LogicalInstant{at}, 40, EvidenceClass::Real);
  reported.report.effect_digest = sha256("late");
  const Result<BatchReport> refused = scenario.send_all({scenario.make(reported, at)});
  DPUF_CHECK(refused.has_value());
  DPUF_CHECK_EQ(refused.value().refused, std::uint64_t{1});
  DPUF_CHECK_EQ(refused.value().outcomes[0].code, ReasonCode::FenceMismatch);
  DPUF_CHECK_EQ(scenario.rt().instance(first)->state, LifecycleState::Unverified);
  DPUF_CHECK(!scenario.rt().instance(first)->serving());
  DPUF_CHECK_OK(scenario.rt().close());
}

DPUF_TEST(persistence, torn_tail_is_truncated_and_reported) {
  TempStore store{"torn"};
  const std::vector<FabricEvent> events = test::standard_world_events(1, 100);
  write_journal(store, events);
  const std::size_t good_bytes = read_file(store.file("fabric.journal")).size();
  DPUF_CHECK(good_bytes > 0);

  // A frame header that promises payload bytes which never arrived.
  std::vector<std::uint8_t> bytes = read_file(store.file("fabric.journal"));
  std::vector<std::uint8_t> torn(kFrameHeaderBytes, 0);
  torn[0] = 0x44;
  torn[1] = 0x50;
  torn[2] = 0x55;
  torn[3] = 0x46;
  torn[4] = 1;
  torn[6] = 1;
  torn[12] = 0xFF;  // 255 declared payload bytes
  bytes.insert(bytes.end(), torn.begin(), torn.end());
  write_file(store.file("fabric.journal"), bytes);

  // Reading the store directly shows the classification and the truncation.
  StoreOptions options;
  options.directory = store.path();
  options.store_id = test::store_id("store-test");
  Result<std::unique_ptr<Store>> raw = Store::open(options);
  DPUF_REQUIRE(raw);
  std::vector<std::uint8_t> snapshot;
  std::vector<std::vector<std::uint8_t>> records;
  Result<StoreRecovery> recovery = raw.value()->load(snapshot, records);
  DPUF_REQUIRE(recovery);
  DPUF_CHECK_EQ(recovery.value().classification, RecoveryClass::TornTailTruncated);
  DPUF_CHECK_EQ(recovery.value().records_dropped, std::uint64_t{1});
  DPUF_CHECK_EQ(records.size(), events.size());
  DPUF_CHECK_EQ(recovery.value().truncations.size(), std::size_t{1});
  DPUF_CHECK_OK(raw.value()->close());
  DPUF_CHECK_EQ(read_file(store.file("fabric.journal")).size(), good_bytes);

  // The runtime reports the same classification on its own first open of an
  // equally damaged store, and the surviving events are replayed in full.
  TempStore runtime_store{"torn-runtime"};
  {
    std::vector<std::uint8_t> runtime_bytes;
    for (const FabricEvent& event : events) {
      const std::vector<std::uint8_t> payload = encode_binary(event);
      const std::vector<std::uint8_t> framed =
          Store::frame(RecordKind::Event, kFormatVersion, kSemanticVersion, payload);
      runtime_bytes.insert(runtime_bytes.end(), framed.begin(), framed.end());
    }
    runtime_bytes.insert(runtime_bytes.end(), torn.begin(), torn.end());
    write_file(runtime_store.file("fabric.journal"), runtime_bytes);
  }
  RuntimeConfig config = store_config(runtime_store);
  Result<std::unique_ptr<FabricRuntime>> runtime = FabricRuntime::open(config);
  DPUF_REQUIRE(runtime);
  DPUF_CHECK_EQ(runtime.value()->recovery().classification, RecoveryClass::TornTailTruncated);
  DPUF_CHECK(runtime.value()->recovery().torn_tail);
  DPUF_CHECK_EQ(runtime.value()->recovery().records_dropped, std::uint64_t{1});
  DPUF_CHECK_EQ(runtime.value()->recovery().journal_records_replayed,
                static_cast<std::uint64_t>(events.size()));
  DPUF_CHECK_EQ(runtime.value()->snapshot().topology.dpus.size(), std::size_t{3});
  DPUF_CHECK_OK(runtime.value()->close());
}

DPUF_TEST(persistence, mid_file_corruption_is_refused) {
  TempStore store{"corrupt"};
  write_journal(store, test::standard_world_events(1, 100));
  std::vector<std::uint8_t> bytes = read_file(store.file("fabric.journal"));
  DPUF_CHECK(bytes.size() > 2 * (kFrameHeaderBytes + kFrameDigestBytes));
  // Flip a payload byte of the first record while later records remain.
  bytes[kFrameHeaderBytes + 2] ^= 0xFF;
  write_file(store.file("fabric.journal"), bytes);
  RuntimeConfig config = store_config(store);
  const Result<std::unique_ptr<FabricRuntime>> opened = FabricRuntime::open(config);
  DPUF_CHECK(!opened.has_value());
  DPUF_CHECK_CODE(opened.status(), ReasonCode::StoreIntegrityFailure);
}

DPUF_TEST(persistence, a_synthesized_journal_replays_into_accepted_state) {
  TempStore store{"replay"};
  const std::vector<FabricEvent> events = test::standard_world_events(1, 100);
  write_journal(store, events);
  RuntimeConfig config = store_config(store);
  Result<std::unique_ptr<FabricRuntime>> opened = FabricRuntime::open(config);
  DPUF_REQUIRE(opened);
  DPUF_CHECK_EQ(opened.value()->recovery().journal_records_replayed,
                static_cast<std::uint64_t>(events.size()));
  DPUF_CHECK_EQ(opened.value()->snapshot().topology.dpus.size(), std::size_t{3});
  DPUF_CHECK(opened.value()->snapshot().policy.generation.valid());
  DPUF_CHECK_OK(opened.value()->close());
}

DPUF_TEST(persistence, truncated_snapshot_and_wrong_versions_are_refused) {
  {
    TempStore store{"truncated"};
    {
      Scenario scenario{store_config(store)};
      DPUF_CHECK_RESULT_OK(test::apply_standard_world(scenario));
      DPUF_CHECK_OK(scenario.rt().checkpoint());
      DPUF_CHECK_OK(scenario.rt().close());
    }
    const std::string snapshot = store.file("fabric.snapshot");
    std::vector<std::uint8_t> bytes = read_file(snapshot);
    bytes.resize(bytes.size() / 2);
    write_file(snapshot, bytes);
    RuntimeConfig config = store_config(store);
    const Result<std::unique_ptr<FabricRuntime>> opened = FabricRuntime::open(config);
    DPUF_CHECK(!opened.has_value());
  }
  {
    TempStore store{"version"};
    {
      Scenario scenario{store_config(store)};
      DPUF_CHECK_RESULT_OK(test::apply_standard_world(scenario));
      DPUF_CHECK_OK(scenario.rt().checkpoint());
      DPUF_CHECK_OK(scenario.rt().close());
    }
    const std::string snapshot = store.file("fabric.snapshot");
    std::vector<std::uint8_t> bytes = read_file(snapshot);
    bytes[4] = 9;  // format version
    write_file(snapshot, bytes);
    RuntimeConfig config = store_config(store);
    const Result<std::unique_ptr<FabricRuntime>> opened = FabricRuntime::open(config);
    DPUF_CHECK(!opened.has_value());
    DPUF_CHECK_CODE(opened.status(), ReasonCode::StoreVersionIncompatible);
  }
  {
    TempStore store{"semantics"};
    {
      Scenario scenario{store_config(store)};
      DPUF_CHECK_RESULT_OK(test::apply_standard_world(scenario));
      DPUF_CHECK_OK(scenario.rt().checkpoint());
      DPUF_CHECK_OK(scenario.rt().close());
    }
    const std::string snapshot = store.file("fabric.snapshot");
    std::vector<std::uint8_t> bytes = read_file(snapshot);
    bytes[6] = 9;  // semantic version
    write_file(snapshot, bytes);
    RuntimeConfig config = store_config(store);
    const Result<std::unique_ptr<FabricRuntime>> opened = FabricRuntime::open(config);
    DPUF_CHECK(!opened.has_value());
    DPUF_CHECK_CODE(opened.status(), ReasonCode::StoreSemanticsIncompatible);
  }
}

DPUF_TEST(persistence, oversized_declared_record_is_refused_before_allocation) {
  TempStore store{"oversized"};
  write_journal(store, test::standard_world_events(1, 100));
  const std::string journal = store.file("fabric.journal");
  std::vector<std::uint8_t> bytes = read_file(journal);
  // Declare a payload larger than the whole journal bound.
  bytes[12] = 0xFF;
  bytes[13] = 0xFF;
  bytes[14] = 0xFF;
  bytes[15] = 0x7F;
  write_file(journal, bytes);
  RuntimeConfig config = store_config(store);
  const Result<std::unique_ptr<FabricRuntime>> opened = FabricRuntime::open(config);
  DPUF_CHECK(!opened.has_value());
}

DPUF_TEST(persistence, checkpoint_compacts_the_journal) {
  TempStore store{"compact"};
  {
    Scenario scenario{store_config(store)};
    DPUF_CHECK_RESULT_OK(test::apply_standard_world(scenario));
    const std::uint64_t before = scenario.rt().snapshot().journaled_events;
    DPUF_CHECK(before > 0);
    DPUF_CHECK_OK(scenario.rt().checkpoint());
    DPUF_CHECK_EQ(scenario.rt().snapshot().journaled_events, std::uint64_t{0});
    // The snapshot is durable and the journal is empty.
    DPUF_CHECK(fs::file_size(fs::path{store.file("fabric.snapshot")}) > 0);
    DPUF_CHECK_EQ(fs::file_size(fs::path{store.file("fabric.journal")}), std::uint64_t{0});
    const Digest durable = scenario.rt().durable_digest();
    DPUF_CHECK_OK(scenario.rt().close());
    RuntimeConfig config = store_config(store);
    Result<std::unique_ptr<FabricRuntime>> reopened = FabricRuntime::open(config);
    DPUF_REQUIRE(reopened);
    DPUF_CHECK_EQ(reopened.value()->durable_digest(), durable);
    DPUF_CHECK_OK(reopened.value()->close());
  }
}

DPUF_TEST(persistence, journal_bound_forces_rotation_rather_than_loss) {
  TempStore store{"rotation"};
  RuntimeConfig config = test::make_config(1200);
  config.store_directory = store.path();
  Scenario scenario{config};
  DPUF_CHECK_RESULT_OK(test::apply_standard_world(scenario));
  // Enough events to exceed the small journal bound several times over.
  for (int i = 0; i < 12; ++i) {
    DPUF_CHECK_RESULT_OK(scenario.tick());
  }
  DPUF_CHECK(scenario.rt().snapshot().journaled_events < 12);
  const Digest durable = scenario.rt().durable_digest();
  DPUF_CHECK_OK(scenario.rt().close());
  config = test::make_config(1200);
  config.store_directory = store.path();
  Result<std::unique_ptr<FabricRuntime>> reopened = FabricRuntime::open(config);
  DPUF_REQUIRE(reopened);
  DPUF_CHECK_EQ(reopened.value()->durable_digest(), durable);
  DPUF_CHECK_OK(reopened.value()->close());
}

DPUF_TEST(persistence, a_store_cannot_be_opened_twice) {
  TempStore store{"lock"};
  Scenario first{store_config(store)};
  DPUF_CHECK_RESULT_OK(test::apply_standard_world(first));
  RuntimeConfig config = store_config(store);
  const Result<std::unique_ptr<FabricRuntime>> second = FabricRuntime::open(config);
  DPUF_CHECK(!second.has_value());
  DPUF_CHECK_CODE(second.status(), ReasonCode::StoreAlreadyOpen);
  DPUF_CHECK_OK(first.rt().close());
  // After the first runtime closes, the store can be reopened.
  Result<std::unique_ptr<FabricRuntime>> third = FabricRuntime::open(config);
  DPUF_REQUIRE(third);
  DPUF_CHECK_OK(third.value()->close());
}

DPUF_TEST(persistence, snapshot_survives_a_rotation_crash_boundary) {
  TempStore store{"rotate-crash"};
  {
    Scenario scenario{store_config(store)};
    DPUF_CHECK_RESULT_OK(test::apply_standard_world(scenario));
    DPUF_CHECK_OK(scenario.rt().checkpoint());
    DPUF_CHECK_OK(scenario.rt().close());
  }
  // Emulate a crash between writing the temporary snapshot and replacing the
  // committed one: the temporary file must be ignored and removed.
  const std::string temporary = store.file("fabric.snapshot.tmp");
  write_file(temporary, std::vector<std::uint8_t>(64, 0xAB));
  const Digest durable = [&store]() {
    Scenario brief{store_config(store)};
    return brief.rt().durable_digest();
  }();
  const std::string temporary_again = store.file("fabric.snapshot.tmp");
  write_file(temporary_again, std::vector<std::uint8_t>(64, 0xCD));
  Scenario scenario{store_config(store)};
  DPUF_CHECK(!fs::exists(fs::path{temporary_again}));
  DPUF_CHECK(scenario.rt().recovery().recovered_from_store);
  DPUF_CHECK_EQ(scenario.rt().durable_digest(), durable);
  DPUF_CHECK_OK(scenario.rt().close());
}

DPUF_TEST(persistence, recovery_classification_is_explicit_for_every_case) {
  TempStore fresh{"fresh"};
  {
    RuntimeConfig config = store_config(fresh);
    Result<std::unique_ptr<FabricRuntime>> runtime = FabricRuntime::open(config);
    DPUF_REQUIRE(runtime);
    DPUF_CHECK_EQ(runtime.value()->recovery().classification, RecoveryClass::EmptyStore);
    DPUF_CHECK(!runtime.value()->recovery().recovered_from_store);
    DPUF_CHECK_OK(runtime.value()->close());
  }
  TempStore reload{"reload"};
  {
    Scenario scenario{store_config(reload)};
    DPUF_CHECK_RESULT_OK(test::apply_standard_world(scenario));
    DPUF_CHECK_OK(scenario.rt().close());
  }
  {
    RuntimeConfig config = store_config(reload);
    Result<std::unique_ptr<FabricRuntime>> runtime = FabricRuntime::open(config);
    DPUF_REQUIRE(runtime);
    DPUF_CHECK_EQ(runtime.value()->recovery().classification, RecoveryClass::CleanReopen);
    DPUF_CHECK(runtime.value()->recovery().recovered_from_store);
    DPUF_CHECK_OK(runtime.value()->close());
  }
}

DPUF_TEST(persistence, repeated_open_close_cycles_stay_stable) {
  TempStore store{"cycles"};
  Digest durable{};
  {
    Scenario scenario{store_config(store)};
    DPUF_CHECK_RESULT_OK(test::apply_standard_world(scenario));
    durable = scenario.rt().durable_digest();
    DPUF_CHECK_OK(scenario.rt().close());
  }
  for (int cycle = 0; cycle < 4; ++cycle) {
    RuntimeConfig config = store_config(store);
    Result<std::unique_ptr<FabricRuntime>> runtime = FabricRuntime::open(config);
    DPUF_REQUIRE(runtime);
    DPUF_CHECK_EQ(runtime.value()->durable_digest(), durable);
    DPUF_CHECK_OK(runtime.value()->close());
  }
  // A second close is idempotent rather than an error.
  RuntimeConfig config = store_config(store);
  Result<std::unique_ptr<FabricRuntime>> runtime = FabricRuntime::open(config);
  DPUF_REQUIRE(runtime);
  DPUF_CHECK_OK(runtime.value()->close());
  DPUF_CHECK_OK(runtime.value()->close());
}

}  // namespace
}  // namespace dpu::fabric
