#pragma once

// Bounded, framed, integrity-checked wire protocol.
//
// A frame is a fixed 24-byte header, a payload of at most the configured bound,
// and a 16-byte truncated SHA-256 trailer over header and payload. The framing
// is deliberately boring: length-prefixed, versioned, integrity-checked and
// bounded, so a peer cannot make the receiver allocate unbounded memory, and a
// truncated or corrupted frame is refused with a stable reason code instead of
// being interpreted.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "dpu/fabric/core/archive.hpp"
#include "dpu/fabric/core/result.hpp"
#include "dpu/fabric/engine/runtime.hpp"

namespace dpu::fabric {

#define DPUF_OPCODE_LIST(X) \
  X(Ping) X(Submit) X(Query) X(Explain) X(Export) X(Shutdown)
DPUF_DECLARE_ENUM(OpCode, DPUF_OPCODE_LIST)

#define DPUF_RESPONSE_STATUS_LIST(X) X(Ok) X(Refused) X(Error)
DPUF_DECLARE_ENUM(ResponseStatus, DPUF_RESPONSE_STATUS_LIST)

inline constexpr std::uint32_t kProtocolMagic = 0x46555044u;  // "DPUF"
inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr std::size_t kProtocolHeaderBytes = 24;
inline constexpr std::size_t kProtocolTrailerBytes = 16;

struct Frame {
  OpCode op{OpCode::Ping};
  std::uint32_t flags{0};
  std::uint64_t request_id{0};
  std::vector<std::uint8_t> payload{};

  /// The response status travels in the low byte of the flags word.
  [[nodiscard]] ResponseStatus status() const {
    return static_cast<ResponseStatus>(flags & 0xFFu);
  }
  void set_status(ResponseStatus value) {
    flags = (flags & ~0xFFu) | static_cast<std::uint32_t>(value);
  }
};

/// Payload carried by a refused response.
struct RefusalPayload {
  ReasonCode code{ReasonCode::Ok};
  std::string detail{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("RefusalPayload");
    field(ar, "code", code);
    field(ar, "detail", detail);
    ar.end_object();
  }
};

/// Encodes a frame. Refuses when the result would exceed `max_frame_bytes`.
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_frame(const Frame& frame,
                                                             std::size_t max_frame_bytes);

struct DecodedFrame {
  bool complete{false};
  bool header_ready{false};
  std::size_t consumed{0};
  std::size_t expected{0};
  Frame frame{};
};

/// Parses one frame from `buffer`. A partial buffer reports header_ready and the
/// number of bytes still needed, so a reader can drive itself from real
/// readability rather than from a heuristic.
[[nodiscard]] Result<DecodedFrame> decode_frame(std::span<const std::uint8_t> buffer,
                                                std::size_t max_frame_bytes);

// ---------------------------------------------------------------------------
// Request and response payloads
// ---------------------------------------------------------------------------

struct PingRequest {
  std::string nonce{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("PingRequest");
    field(ar, "nonce", nonce);
    ar.end_object();
  }
};

struct PingResponse {
  std::string nonce{};
  std::string product{};
  std::string version{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("PingResponse");
    field(ar, "nonce", nonce);
    field(ar, "product", product);
    field(ar, "version", version);
    ar.end_object();
  }
};

struct SubmitRequest {
  std::vector<FabricEvent> events{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("SubmitRequest");
    field(ar, "events", events);
    ar.end_object();
  }
};

struct SubmitResponse {
  BatchReport report{};
  Digest state_digest{};
  Digest durable_digest{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("SubmitResponse");
    field(ar, "report", report);
    field(ar, "state_digest", state_digest);
    field(ar, "durable_digest", durable_digest);
    ar.end_object();
  }
};

struct QueryRequest {
  std::uint32_t reserved{0};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("QueryRequest");
    field(ar, "reserved", reserved);
    ar.end_object();
  }
};

struct QueryResponse {
  Digest state_digest{};
  Digest durable_digest{};
  std::uint64_t instances{0};
  std::uint64_t staged{0};
  RecoverySummary recovery{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("QueryResponse");
    field(ar, "state_digest", state_digest);
    field(ar, "durable_digest", durable_digest);
    field(ar, "instances", instances);
    field(ar, "staged", staged);
    field(ar, "recovery", recovery);
    ar.end_object();
  }
};

struct ExplainRequest {
  bool has_instance{false};
  InstanceId instance{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("ExplainRequest");
    field(ar, "has_instance", has_instance);
    field(ar, "instance", instance);
    ar.end_object();
  }
};

struct ExplainResponse {
  std::vector<Explanation> explanations{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("ExplainResponse");
    field(ar, "explanations", explanations);
    ar.end_object();
  }
};

struct ExportRequest {
  bool pretty{true};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("ExportRequest");
    field(ar, "pretty", pretty);
    ar.end_object();
  }
};

struct ExportResponse {
  std::string document{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("ExportResponse");
    field(ar, "document", document);
    ar.end_object();
  }
};

struct ShutdownRequest {
  std::uint32_t reserved{0};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("ShutdownRequest");
    field(ar, "reserved", reserved);
    ar.end_object();
  }
};

struct ShutdownResponse {
  std::uint64_t instances{0};
  Digest state_digest{};

  template <class Ar>
  void visit(Ar& ar) {
    ar.begin_object("ShutdownResponse");
    field(ar, "instances", instances);
    field(ar, "state_digest", state_digest);
    ar.end_object();
  }
};

}  // namespace dpu::fabric
