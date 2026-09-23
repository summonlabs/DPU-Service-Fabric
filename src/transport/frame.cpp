#include "dpu/fabric/transport/frame.hpp"

#include <cstring>

namespace dpu::fabric {
namespace {

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

void put_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

std::uint16_t read_u16(const std::uint8_t* data) {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[0]) |
                                    (static_cast<std::uint16_t>(data[1]) << 8u));
}

std::uint32_t read_u32(const std::uint8_t* data) {
  std::uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(data[i]) << (8u * i);
  return value;
}

std::uint64_t read_u64(const std::uint8_t* data) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(data[i]) << (8u * i);
  return value;
}

}  // namespace

Result<std::vector<std::uint8_t>> encode_frame(const Frame& frame, std::size_t max_frame_bytes) {
  const std::size_t total = kProtocolHeaderBytes + frame.payload.size() + kProtocolTrailerBytes;
  if (total > max_frame_bytes) {
    return refuse(ReasonCode::FrameOversized, "frame exceeds the configured bound");
  }
  std::vector<std::uint8_t> out;
  out.reserve(total);
  put_u32(out, kProtocolMagic);
  put_u16(out, kProtocolVersion);
  put_u16(out, static_cast<std::uint16_t>(frame.op));
  put_u32(out, frame.flags);
  put_u64(out, frame.request_id);
  put_u32(out, static_cast<std::uint32_t>(frame.payload.size()));
  out.insert(out.end(), frame.payload.begin(), frame.payload.end());
  const Digest digest = sha256(std::span<const std::uint8_t>(out.data(), out.size()));
  out.insert(out.end(), digest.bytes().begin(),
             digest.bytes().begin() + static_cast<std::ptrdiff_t>(kProtocolTrailerBytes));
  return out;
}

Result<DecodedFrame> decode_frame(std::span<const std::uint8_t> buffer, std::size_t max_frame_bytes) {
  DecodedFrame decoded;
  if (buffer.size() < kProtocolHeaderBytes) {
    decoded.expected = kProtocolHeaderBytes;
    return decoded;
  }
  const std::uint8_t* header = buffer.data();
  if (read_u32(header) != kProtocolMagic) {
    return refuse(ReasonCode::ProtocolViolation, "frame magic mismatch");
  }
  const std::uint16_t version = read_u16(header + 4);
  if (version != kProtocolVersion) {
    return refuse(ReasonCode::UnsupportedVersion, "unsupported protocol version");
  }
  const std::uint32_t raw_op = read_u16(header + 6);
  if (raw_op >= kOpCodeCount) {
    return refuse(ReasonCode::UnknownOperation, "unknown operation code");
  }
  decoded.frame.op = static_cast<OpCode>(raw_op);
  decoded.frame.flags = read_u32(header + 8);
  decoded.frame.request_id = read_u64(header + 12);
  const std::uint32_t length = read_u32(header + 20);
  if (length > max_frame_bytes) {
    return refuse(ReasonCode::FrameOversized, "declared payload exceeds the bound");
  }
  decoded.header_ready = true;
  const std::size_t total = kProtocolHeaderBytes + static_cast<std::size_t>(length) +
                            kProtocolTrailerBytes;
  decoded.expected = total;
  if (total > max_frame_bytes) {
    return refuse(ReasonCode::FrameOversized, "frame exceeds the configured bound");
  }
  if (buffer.size() < total) {
    return decoded;
  }
  const Digest digest = sha256(buffer.subspan(0, kProtocolHeaderBytes + length));
  if (std::memcmp(digest.bytes().data(), buffer.data() + kProtocolHeaderBytes + length,
                  kProtocolTrailerBytes) != 0) {
    return refuse(ReasonCode::FrameTruncated, "frame digest mismatch");
  }
  decoded.frame.payload.assign(buffer.begin() + static_cast<std::ptrdiff_t>(kProtocolHeaderBytes),
                               buffer.begin() + static_cast<std::ptrdiff_t>(kProtocolHeaderBytes +
                                                                            length));
  decoded.complete = true;
  decoded.consumed = total;
  return decoded;
}

}  // namespace dpu::fabric
