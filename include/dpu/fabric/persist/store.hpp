#pragma once

// Versioned, integrity-checked durable state.
//
// Layout of a store directory:
//
//   fabric.snapshot       last committed snapshot (header + payload + digest)
//   fabric.snapshot.tmp   in-progress snapshot; never trusted on recovery
//   fabric.journal        append-only event records committed since the snapshot
//
// Commit rules:
//
//  * A journal record is one framed, length-prefixed, digest-protected unit.
//    A record is durable only after its bytes have reached the device.
//  * A snapshot is written to a temporary file, forced to the device, then
//    renamed over the previous snapshot, so a crash mid-rotation leaves the
//    previous snapshot intact and a temporary file that recovery deletes.
//  * Recovery classifies what it found instead of guessing: a clean reopen, a
//    torn tail that was truncated, an empty store, or a refusal with a stable
//    reason code. A store whose semantics this build cannot interpret is
//    refused rather than reinterpreted.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dpu/fabric/core/archive.hpp"
#include "dpu/fabric/core/bounded.hpp"
#include "dpu/fabric/core/result.hpp"
#include "dpu/fabric/core/types.hpp"
#include "dpu/fabric/model/event.hpp"
#include "dpu/fabric/version.hpp"

namespace dpu::fabric {

/// One durable record kind. Values are part of the format contract.
#define DPUF_RECORD_KIND_LIST(X) X(Event) X(Snapshot) X(Marker)
DPUF_DECLARE_ENUM(RecordKind, DPUF_RECORD_KIND_LIST)

struct StoreOptions {
  std::string directory{};
  StoreId store_id{};
  std::size_t max_journal_bytes{4u * 1024u * 1024u};
  std::uint32_t format_version{kFormatVersion};
  std::uint32_t semantic_version{kSemanticVersion};
  bool fsync{true};
  /// Maximum size of a single journal record, including framing.
  std::size_t max_record_bytes{1024u * 1024u};
  /// When true, a corrupt record in the middle of the journal is refused. When
  /// false it is still refused: corruption never silently truncates live data.
  bool allow_mid_file_truncation{false};
};

struct StoreRecovery {
  RecoveryClass classification{RecoveryClass::EmptyStore};
  bool found_snapshot{false};
  bool found_journal{false};
  std::uint64_t snapshot_bytes{0};
  std::uint64_t journal_bytes{0};
  std::uint64_t journal_records{0};
  std::uint64_t records_dropped{0};
  std::uint64_t torn_bytes_dropped{0};
  std::uint64_t snapshot_rewrites{0};
  Digest snapshot_digest{};
  std::vector<TruncationRecord> truncations{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("StoreRecovery");
    field(ar, "classification", classification);
    field(ar, "found_snapshot", found_snapshot);
    field(ar, "found_journal", found_journal);
    field(ar, "snapshot_bytes", snapshot_bytes);
    field(ar, "journal_bytes", journal_bytes);
    field(ar, "journal_records", journal_records);
    field(ar, "records_dropped", records_dropped);
    field(ar, "torn_bytes_dropped", torn_bytes_dropped);
    field(ar, "snapshot_rewrites", snapshot_rewrites);
    field(ar, "snapshot_digest", snapshot_digest);
    field(ar, "truncations", truncations);
    ar.end_object();
  }
};

class Store {
 public:
  [[nodiscard]] static Result<std::unique_ptr<Store>> open(const StoreOptions& options);

  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;
  ~Store();

  /// Loads the snapshot and the journal records that follow it. The journal is
  /// returned in file order; the caller replays it through the same reducer that
  /// produced it.
  [[nodiscard]] Result<StoreRecovery> load(std::vector<std::uint8_t>& snapshot_payload,
                                           std::vector<std::vector<std::uint8_t>>& journal_records);

  /// Appends one event record and (when configured) forces it to the device.
  [[nodiscard]] Status append_event(const FabricEvent& event);

  /// Replaces the snapshot atomically and truncates the journal.
  [[nodiscard]] Status rotate(const std::vector<std::uint8_t>& snapshot_payload);

  [[nodiscard]] Status close();
  [[nodiscard]] bool closed() const noexcept;
  [[nodiscard]] const StoreOptions& options() const noexcept { return options_; }
  [[nodiscard]] std::uint64_t journal_bytes() const noexcept { return journal_bytes_; }
  [[nodiscard]] std::uint64_t journal_records() const noexcept { return journal_records_; }
  [[nodiscard]] const StoreRecovery& recovery() const noexcept { return recovery_; }

  /// Frame encoding used by the journal: exposed for the integrity tests.
  [[nodiscard]] static std::vector<std::uint8_t> frame(RecordKind kind, std::uint32_t format_version,
                                                        std::uint32_t semantic_version,
                                                        std::span<const std::uint8_t> payload);

 private:
  explicit Store(StoreOptions options);

  /// Opens the journal for appending on first use. The handle is deliberately
  /// not held across recovery: a reading handle and an appending handle cannot
  /// coexist on this platform.
  [[nodiscard]] Status ensure_appender();

  StoreOptions options_{};
  StoreRecovery recovery_{};
  void* handle_{nullptr};
  void* lock_{nullptr};
  std::uint64_t journal_bytes_{0};
  std::uint64_t journal_records_{0};
  bool closed_{true};
};

/// Reads and validates a framed record stream.
struct FrameParseResult {
  Status status{};
  bool complete{false};
  bool torn_tail{false};
  RecordKind kind{RecordKind::Event};
  std::size_t payload_offset{0};
  std::size_t payload_length{0};
  std::size_t consumed{0};
};

/// Parses one frame at \p offset. Structural damage, length violations and
/// digest mismatches are reported distinctly so recovery can classify them.
[[nodiscard]] FrameParseResult parse_frame(std::span<const std::uint8_t> data, std::size_t offset,
                                           std::uint32_t expected_format,
                                           std::uint32_t expected_semantic,
                                           std::size_t max_record_bytes);

/// Framing constants, shared with the tests that attack the format.
inline constexpr std::size_t kFrameHeaderBytes = 16;
inline constexpr std::size_t kFrameDigestBytes = 32;
inline constexpr std::uint32_t kFrameMagic = 0x46555044u;  // "DPUF" little endian

}  // namespace dpu::fabric
