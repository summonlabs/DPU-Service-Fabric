#include "dpu/fabric/persist/store.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace dpu::fabric {
namespace {

namespace fs = std::filesystem;

constexpr const char* kSnapshotName = "fabric.snapshot";
constexpr const char* kSnapshotTempName = "fabric.snapshot.tmp";
constexpr const char* kJournalName = "fabric.journal";
constexpr const char* kLockName = "fabric.lock";

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

std::uint16_t read_u16(const std::uint8_t* data) {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[0]) |
                                    (static_cast<std::uint16_t>(data[1]) << 8u));
}

std::uint32_t read_u32(const std::uint8_t* data) {
  std::uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(data[i]) << (8u * i);
  }
  return value;
}

Status ensure_directory(const std::string& directory) {
  std::error_code error;
  const bool exists = fs::exists(fs::path{directory}, error);
  if (error) return refuse(ReasonCode::StoreIoFailure, "cannot stat store directory");
  if (!exists) {
    fs::create_directories(fs::path{directory}, error);
    if (error) return refuse(ReasonCode::StoreIoFailure, "cannot create store directory");
  }
  return Status::success();
}

std::string join(const std::string& directory, const char* name) {
  return (fs::path{directory} / name).string();
}

Result<std::FILE*> open_file(const std::string& path, const char* mode) {
  std::FILE* handle = nullptr;
#ifdef _WIN32
  if (fopen_s(&handle, path.c_str(), mode) != 0) handle = nullptr;
#else
  handle = std::fopen(path.c_str(), mode);
#endif
  if (handle == nullptr) {
    return refuse(ReasonCode::StoreIoFailure,
                  "cannot open " + path + ": " + std::strerror(errno));
  }
  return handle;
}

Status sync_file(std::FILE* handle) {
  if (handle == nullptr) return refuse(ReasonCode::StoreIoFailure, "null file handle");
#ifdef _WIN32
  if (_commit(_fileno(handle)) != 0) {
    return refuse(ReasonCode::StoreIoFailure, "flush to device failed");
  }
#else
  if (fsync(fileno(handle)) != 0) {
    return refuse(ReasonCode::StoreIoFailure, "flush to device failed");
  }
#endif
  return Status::success();
}

Status close_file(std::FILE* handle) {
  if (handle == nullptr) return Status::success();
  if (std::fclose(handle) != 0) {
    return refuse(ReasonCode::StoreIoFailure, "close failed");
  }
  return Status::success();
}

Status atomic_replace(const std::string& from, const std::string& to) {
#ifdef _WIN32
  if (MoveFileExW(fs::path{from}.wstring().c_str(), fs::path{to}.wstring().c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    return refuse(ReasonCode::StoreIoFailure, "atomic replace failed");
  }
#else
  std::error_code error;
  fs::rename(fs::path{from}, fs::path{to}, error);
  if (error) return refuse(ReasonCode::StoreIoFailure, "atomic replace failed");
#endif
  return Status::success();
}

Status remove_file(const std::string& path) {
  std::error_code error;
  const bool removed = fs::remove(fs::path{path}, error);
  if (error) return refuse(ReasonCode::StoreIoFailure, "remove failed");
  (void)removed;
  return Status::success();
}

Result<std::vector<std::uint8_t>> read_whole_file(const std::string& path, std::size_t max_bytes) {
  std::error_code error;
  const auto size = fs::file_size(fs::path{path}, error);
  if (error) return refuse(ReasonCode::StoreIoFailure, "cannot size " + path);
  if (size > max_bytes) {
    return refuse(ReasonCode::StoreRecordOversized, "file exceeds the configured bound");
  }
  Result<std::FILE*> handle = open_file(path, "rb");
  if (!handle) return handle.status();
  std::vector<std::uint8_t> buffer(static_cast<std::size_t>(size));
  std::size_t read = 0;
  if (!buffer.empty()) {
    read = std::fread(buffer.data(), 1, buffer.size(), handle.value());
  }
  const Status closed = close_file(handle.value());
  if (read != buffer.size()) {
    return refuse(ReasonCode::StoreTruncated, "short read");
  }
  if (!closed.ok()) return closed;
  return buffer;
}

Result<std::uint64_t> file_size_of(const std::string& path) {
  std::error_code error;
  const auto size = fs::file_size(fs::path{path}, error);
  if (error) return refuse(ReasonCode::StoreNotFound, path);
  return static_cast<std::uint64_t>(size);
}

bool file_exists(const std::string& path) {
  std::error_code error;
  return fs::exists(fs::path{path}, error);
}

/// Takes an exclusive lock on the store directory. Two runtimes must never
/// journal into the same store: the second one would interleave records that
/// neither could replay correctly.
Status acquire_lock(const std::string& path, void*& handle) {
  handle = nullptr;
#ifdef _WIN32
  const HANDLE file = CreateFileW(fs::path{path}.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                                  0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return refuse(ReasonCode::StoreAlreadyOpen, "store is already open elsewhere");
  }
  handle = file;
  return Status::success();
#else
  const int fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0644);
  if (fd < 0) return refuse(ReasonCode::StoreIoFailure, "cannot open lock file");
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    return refuse(ReasonCode::StoreAlreadyOpen, "store is already open elsewhere");
  }
  handle = reinterpret_cast<void*>(static_cast<std::intptr_t>(fd));
  return Status::success();
#endif
}

void release_lock(void* handle) {
  if (handle == nullptr) return;
#ifdef _WIN32
  CloseHandle(static_cast<HANDLE>(handle));
#else
  const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(handle));
  (void)::flock(fd, LOCK_UN);
  ::close(fd);
#endif
}

Status truncate_file(const std::string& path, std::uint64_t size) {
  std::error_code error;
  fs::resize_file(fs::path{path}, size, error);
  if (error) return refuse(ReasonCode::StoreIoFailure, "truncate failed");
  return Status::success();
}

}  // namespace

std::vector<std::uint8_t> Store::frame(RecordKind kind, std::uint32_t format_version,
                                       std::uint32_t semantic_version,
                                       std::span<const std::uint8_t> payload) {
  std::vector<std::uint8_t> out;
  out.reserve(kFrameHeaderBytes + payload.size() + kFrameDigestBytes);
  put_u32(out, kFrameMagic);
  put_u16(out, static_cast<std::uint16_t>(format_version & 0xFFFFu));
  put_u16(out, static_cast<std::uint16_t>(semantic_version & 0xFFFFu));
  out.push_back(static_cast<std::uint8_t>(kind));
  out.push_back(0);
  out.push_back(0);
  out.push_back(0);
  put_u32(out, static_cast<std::uint32_t>(payload.size()));
  out.insert(out.end(), payload.begin(), payload.end());
  const Digest digest = sha256(payload);
  out.insert(out.end(), digest.bytes().begin(), digest.bytes().end());
  return out;
}

FrameParseResult parse_frame(std::span<const std::uint8_t> data, std::size_t offset,
                             std::uint32_t expected_format, std::uint32_t expected_semantic,
                             std::size_t max_record_bytes) {
  FrameParseResult result;
  result.status = Status::success();
  if (offset > data.size()) {
    result.status = refuse(ReasonCode::StoreTruncated, "offset beyond end of record stream");
    return result;
  }
  if (offset == data.size()) {
    return result;  // clean end of stream
  }
  if (data.size() - offset < kFrameHeaderBytes) {
    result.torn_tail = true;
    result.status = refuse(ReasonCode::StoreTruncated, "torn frame header");
    return result;
  }
  const std::uint8_t* header = data.data() + offset;
  if (read_u32(header) != kFrameMagic) {
    result.status = refuse(ReasonCode::StoreCorrupt, "frame magic mismatch");
    return result;
  }
  const std::uint16_t format = read_u16(header + 4);
  const std::uint16_t semantic = read_u16(header + 6);
  if (format != static_cast<std::uint16_t>(expected_format & 0xFFFFu)) {
    result.status = refuse(ReasonCode::StoreVersionIncompatible, "frame format version");
    return result;
  }
  if (semantic != static_cast<std::uint16_t>(expected_semantic & 0xFFFFu)) {
    result.status = refuse(ReasonCode::StoreSemanticsIncompatible, "frame semantic version");
    return result;
  }
  const std::uint8_t raw_kind = header[8];
  if (raw_kind > static_cast<std::uint8_t>(RecordKind::Marker)) {
    result.status = refuse(ReasonCode::StoreCorrupt, "unknown record kind");
    return result;
  }
  result.kind = static_cast<RecordKind>(raw_kind);
  const std::uint32_t length = read_u32(header + 12);
  if (length > max_record_bytes) {
    result.status = refuse(ReasonCode::StoreRecordOversized, "frame payload exceeds bound");
    return result;
  }
  const std::size_t needed =
      kFrameHeaderBytes + static_cast<std::size_t>(length) + kFrameDigestBytes;
  if (data.size() - offset < needed) {
    result.torn_tail = true;
    result.status = refuse(ReasonCode::StoreTruncated, "torn frame payload");
    return result;
  }
  const std::span<const std::uint8_t> payload = data.subspan(offset + kFrameHeaderBytes, length);
  const std::span<const std::uint8_t> stored =
      data.subspan(offset + kFrameHeaderBytes + length, kFrameDigestBytes);
  const Digest computed = sha256(payload);
  if (std::memcmp(computed.bytes().data(), stored.data(), kFrameDigestBytes) != 0) {
    result.status = refuse(ReasonCode::StoreIntegrityFailure, "frame digest mismatch");
    return result;
  }
  result.complete = true;
  result.payload_offset = offset + kFrameHeaderBytes;
  result.payload_length = length;
  result.consumed = needed;
  return result;
}

Store::Store(StoreOptions options) : options_(std::move(options)) {}

Store::~Store() { (void)close(); }

Result<std::unique_ptr<Store>> Store::open(const StoreOptions& options) {
  if (options.directory.empty()) {
    return refuse(ReasonCode::MalformedInput, "store directory is required");
  }
  if (options.max_record_bytes == 0 || options.max_journal_bytes == 0) {
    return refuse(ReasonCode::ValueOutOfRange, "store bounds must be positive");
  }
  const Status prepared = ensure_directory(options.directory);
  if (!prepared.ok()) return prepared;

  auto store = std::unique_ptr<Store>(new Store(options));
  store->recovery_.found_snapshot = file_exists(join(options.directory, kSnapshotName));
  store->recovery_.found_journal = file_exists(join(options.directory, kJournalName));

  // A temporary snapshot means a rotation was interrupted. The previous
  // snapshot is still authoritative; the temporary file is never trusted.
  const std::string temp = join(options.directory, kSnapshotTempName);
  if (file_exists(temp)) {
    const Status removed = remove_file(temp);
    if (!removed.ok()) return removed;
  }

  const Status locked = acquire_lock(join(options.directory, kLockName), store->lock_);
  if (!locked.ok()) return locked;

  store->closed_ = false;
  if (store->recovery_.found_journal) {
    Result<std::uint64_t> size = file_size_of(join(options.directory, kJournalName));
    if (size) store->journal_bytes_ = size.value();
  }
  return store;
}

Result<StoreRecovery> Store::load(std::vector<std::uint8_t>& snapshot_payload,
                                  std::vector<std::vector<std::uint8_t>>& journal_records) {
  snapshot_payload.clear();
  journal_records.clear();
  recovery_.classification = RecoveryClass::EmptyStore;

  const std::string snapshot_path = join(options_.directory, kSnapshotName);
  if (file_exists(snapshot_path)) {
    Result<std::vector<std::uint8_t>> raw =
        read_whole_file(snapshot_path, options_.max_record_bytes + kFrameHeaderBytes + kFrameDigestBytes);
    if (!raw) {
      recovery_.classification = raw.status().code() == ReasonCode::StoreRecordOversized
                                     ? RecoveryClass::RefusedIntegrity
                                     : RecoveryClass::RefusedCorrupt;
      return raw.status();
    }
    if (!raw.value().empty()) {
      const FrameParseResult parsed =
          parse_frame(raw.value(), 0, options_.format_version, options_.semantic_version,
                      options_.max_record_bytes);
      if (!parsed.complete) {
        RecoveryClass classification = RecoveryClass::RefusedCorrupt;
        if (parsed.status.code() == ReasonCode::StoreVersionIncompatible ||
            parsed.status.code() == ReasonCode::StoreSemanticsIncompatible) {
          classification = RecoveryClass::RefusedVersion;
        } else if (parsed.status.code() == ReasonCode::StoreTruncated) {
          classification = RecoveryClass::RefusedTruncated;
        } else if (parsed.status.code() == ReasonCode::StoreIntegrityFailure ||
                   parsed.status.code() == ReasonCode::StoreRecordOversized) {
          classification = RecoveryClass::RefusedIntegrity;
        }
        recovery_.classification = classification;
        return parsed.status;
      }
      if (parsed.kind != RecordKind::Snapshot) {
        recovery_.classification = RecoveryClass::RefusedCorrupt;
        return refuse(ReasonCode::StoreCorrupt, "snapshot file does not hold a snapshot record");
      }
      if (parsed.consumed != raw.value().size()) {
        // Trailing bytes after a complete snapshot mean the file was damaged
        // rather than torn: refuse instead of guessing which part is real.
        recovery_.classification = RecoveryClass::RefusedCorrupt;
        return refuse(ReasonCode::StoreCorrupt, "trailing bytes after snapshot record");
      }
      snapshot_payload.assign(raw.value().begin() + static_cast<std::ptrdiff_t>(parsed.payload_offset),
                              raw.value().begin() + static_cast<std::ptrdiff_t>(parsed.payload_offset +
                                                                               parsed.payload_length));
      recovery_.snapshot_digest = sha256(snapshot_payload);
      recovery_.snapshot_bytes = raw.value().size();
      recovery_.classification = RecoveryClass::CleanReopen;
    }
  }

  const std::string journal_path = join(options_.directory, kJournalName);
  if (file_exists(journal_path)) {
    // The journal is bounded, so it is read in full and parsed frame by frame.
    Result<std::vector<std::uint8_t>> raw = read_whole_file(journal_path, options_.max_journal_bytes);
    if (!raw) {
      if (raw.status().code() == ReasonCode::StoreRecordOversized) {
        recovery_.classification = RecoveryClass::RefusedIntegrity;
      }
      return raw.status();
    }
    recovery_.journal_bytes = raw.value().size();
    std::size_t offset = 0;
    while (offset < raw.value().size()) {
      const FrameParseResult parsed =
          parse_frame(raw.value(), offset, options_.format_version, options_.semantic_version,
                      options_.max_record_bytes);
      if (parsed.complete) {
        if (parsed.kind != RecordKind::Event) {
          recovery_.classification = RecoveryClass::RefusedCorrupt;
          return refuse(ReasonCode::StoreCorrupt, "journal holds a non-event record");
        }
        journal_records.emplace_back(
            raw.value().begin() + static_cast<std::ptrdiff_t>(parsed.payload_offset),
            raw.value().begin() + static_cast<std::ptrdiff_t>(parsed.payload_offset +
                                                              parsed.payload_length));
        offset += parsed.consumed;
        continue;
      }
      const bool at_tail = parsed.torn_tail || offset + kFrameHeaderBytes >= raw.value().size();
      if (!at_tail) {
        // Damage in the middle of live data: refuse. Truncating here would
        // silently discard acknowledged events.
        RecoveryClass classification = RecoveryClass::RefusedCorrupt;
        if (parsed.status.code() == ReasonCode::StoreIntegrityFailure) {
          classification = RecoveryClass::RefusedIntegrity;
        } else if (parsed.status.code() == ReasonCode::StoreVersionIncompatible ||
                   parsed.status.code() == ReasonCode::StoreSemanticsIncompatible) {
          classification = RecoveryClass::RefusedVersion;
        }
        recovery_.classification = classification;
        return parsed.status;
      }
      // Torn tail: the last record never reached the device intact. Drop it, and
      // say so.
      recovery_.torn_bytes_dropped = raw.value().size() - offset;
      recovery_.records_dropped = 1;
      TruncationRecord record;
      record.container = "store.journal.tail";
      record.requested = 1;
      record.accepted = 0;
      record.dropped = 1;
      record.reason = ReasonCode::StoreTornTailRecovered;
      recovery_.truncations.push_back(std::move(record));
      recovery_.classification = RecoveryClass::TornTailTruncated;
      const Status truncated = truncate_file(journal_path, offset);
      if (!truncated.ok()) return truncated;
      break;
    }
    recovery_.journal_records = journal_records.size();
    if (recovery_.classification == RecoveryClass::EmptyStore && !journal_records.empty()) {
      recovery_.classification = RecoveryClass::CleanReopen;
    }
  }
  return recovery_;
}

Status Store::ensure_appender() {
  if (handle_ != nullptr) return Status::success();
  Result<std::FILE*> handle = open_file(join(options_.directory, kJournalName), "ab");
  if (!handle) return handle.status();
  handle_ = handle.value();
  return Status::success();
}

Status Store::append_event(const FabricEvent& event) {
  if (closed_) return refuse(ReasonCode::StoreClosed, "store is closed");
  const Status opened = ensure_appender();
  if (!opened.ok()) return opened;
  const std::vector<std::uint8_t> payload = encode_binary(event, 512);
  const std::vector<std::uint8_t> framed =
      frame(RecordKind::Event, options_.format_version, options_.semantic_version, payload);
  if (framed.size() > options_.max_record_bytes) {
    return refuse(ReasonCode::StoreRecordOversized, "event record exceeds bound");
  }
  if (journal_bytes_ + framed.size() > options_.max_journal_bytes) {
    return refuse(ReasonCode::BoundExceeded, "journal bound reached; rotation required");
  }
  auto* handle = static_cast<std::FILE*>(handle_);
  if (std::fwrite(framed.data(), 1, framed.size(), handle) != framed.size()) {
    return refuse(ReasonCode::StoreIoFailure, "journal write failed");
  }
  if (std::fflush(handle) != 0) {
    return refuse(ReasonCode::StoreIoFailure, "journal flush failed");
  }
  if (options_.fsync) {
    const Status synced = sync_file(handle);
    if (!synced.ok()) return synced;
  }
  journal_bytes_ += framed.size();
  journal_records_ += 1;
  return Status::success();
}

Status Store::rotate(const std::vector<std::uint8_t>& snapshot_payload) {
  if (closed_) return refuse(ReasonCode::StoreClosed, "store is closed");
  const std::vector<std::uint8_t> framed =
      frame(RecordKind::Snapshot, options_.format_version, options_.semantic_version,
            snapshot_payload);
  if (framed.size() > options_.max_record_bytes) {
    return refuse(ReasonCode::StoreRecordOversized, "snapshot exceeds bound");
  }
  const std::string temp = join(options_.directory, kSnapshotTempName);
  const std::string target = join(options_.directory, kSnapshotName);
  const Status removed = remove_file(temp);
  if (!removed.ok()) return removed;
  Result<std::FILE*> writer = open_file(temp, "wb");
  if (!writer) return writer.status();
  if (std::fwrite(framed.data(), 1, framed.size(), writer.value()) != framed.size()) {
    (void)close_file(writer.value());
    return refuse(ReasonCode::StoreIoFailure, "snapshot write failed");
  }
  if (std::fflush(writer.value()) != 0) {
    (void)close_file(writer.value());
    return refuse(ReasonCode::StoreIoFailure, "snapshot flush failed");
  }
  const Status synced = sync_file(writer.value());
  const Status closed_ok = close_file(writer.value());
  if (!synced.ok()) return synced;
  if (!closed_ok.ok()) return closed_ok;
  const Status replaced = atomic_replace(temp, target);
  if (!replaced.ok()) return replaced;

  // The journal is emptied only after the snapshot is durable. If the process
  // dies between these two steps, recovery replays the journal on top of the new
  // snapshot and skips records the snapshot already covers, because the snapshot
  // carries the runtime watermark.
  if (handle_ != nullptr) {
    const Status closed_journal = close_file(static_cast<std::FILE*>(handle_));
    if (!closed_journal.ok()) return closed_journal;
    handle_ = nullptr;
  }
  Result<std::FILE*> fresh = open_file(join(options_.directory, kJournalName), "wb");
  if (!fresh) return fresh.status();
  if (std::fflush(fresh.value()) != 0) {
    (void)close_file(fresh.value());
    return refuse(ReasonCode::StoreIoFailure, "journal truncate failed");
  }
  if (options_.fsync) {
    const Status sync_new = sync_file(fresh.value());
    if (!sync_new.ok()) {
      (void)close_file(fresh.value());
      return sync_new;
    }
  }
  const Status truncate_closed = close_file(fresh.value());
  if (!truncate_closed.ok()) return truncate_closed;
  journal_bytes_ = 0;
  journal_records_ = 0;
  recovery_.snapshot_rewrites += 1;
  recovery_.snapshot_digest = sha256(snapshot_payload);
  return Status::success();
}

Status Store::close() {
  if (closed_) return Status::success();
  auto* handle = static_cast<std::FILE*>(handle_);
  handle_ = nullptr;
  closed_ = true;
  release_lock(lock_);
  lock_ = nullptr;
  if (handle == nullptr) return Status::success();
  if (std::fflush(handle) != 0) {
    (void)close_file(handle);
    return refuse(ReasonCode::StoreIoFailure, "journal flush on close failed");
  }
  if (options_.fsync) {
    const Status synced = sync_file(handle);
    if (!synced.ok()) {
      (void)close_file(handle);
      return synced;
    }
  }
  return close_file(handle);
}

bool Store::closed() const noexcept { return closed_; }

}  // namespace dpu::fabric
