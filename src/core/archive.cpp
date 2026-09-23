#include "dpu/fabric/core/archive.hpp"

#include "dpu/fabric/core/types.hpp"

#include <cstring>
#include <limits>

namespace dpu::fabric {

// ---------------------------------------------------------------------------
// BinaryWriter
// ---------------------------------------------------------------------------

BinaryWriter::BinaryWriter(std::size_t reserve) { buffer_.reserve(reserve); }

void BinaryWriter::reserve(std::size_t extra) { buffer_.reserve(buffer_.size() + extra); }

void BinaryWriter::put_u8(std::uint8_t value) { buffer_.push_back(value); }

void BinaryWriter::put_u32(std::uint32_t value) {
  buffer_.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  buffer_.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
  buffer_.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xFFu));
  buffer_.push_back(static_cast<std::uint8_t>((value >> 24u) & 0xFFu));
}

void BinaryWriter::put_u64(std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    buffer_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

void BinaryWriter::put_raw(std::span<const std::uint8_t> data) {
  buffer_.insert(buffer_.end(), data.begin(), data.end());
}

void BinaryWriter::put_tag(WireTag tag) { put_u8(static_cast<std::uint8_t>(tag)); }

void BinaryWriter::begin_object(std::string_view type_name) {
  put_tag(WireTag::Object);
  std::string name{type_name};
  primitive(name);
}

void BinaryWriter::end_object() {}

void BinaryWriter::begin_array(std::size_t& count) {
  put_tag(WireTag::Array);
  std::uint32_t narrowed = 0;
  if (count > std::numeric_limits<std::uint32_t>::max()) {
    narrowed = std::numeric_limits<std::uint32_t>::max();
  } else {
    narrowed = static_cast<std::uint32_t>(count);
  }
  put_u32(narrowed);
}

void BinaryWriter::end_array() {}
void BinaryWriter::begin_element() {}
void BinaryWriter::end_element() {}
void BinaryWriter::begin_field(std::string_view) {}
void BinaryWriter::end_field() {}

void BinaryWriter::optional_begin(bool& present) { put_u8(present ? 1u : 0u); }
void BinaryWriter::optional_end() {}

void BinaryWriter::primitive(bool& value) {
  put_tag(WireTag::Bool);
  put_u8(value ? 1u : 0u);
}

void BinaryWriter::primitive(std::uint8_t& value) {
  put_tag(WireTag::U64);
  put_u64(value);
}
void BinaryWriter::primitive(std::uint16_t& value) {
  put_tag(WireTag::U64);
  put_u64(value);
}
void BinaryWriter::primitive(std::uint32_t& value) {
  put_tag(WireTag::U64);
  put_u64(value);
}
void BinaryWriter::primitive(std::uint64_t& value) {
  put_tag(WireTag::U64);
  put_u64(value);
}
void BinaryWriter::primitive(std::int8_t& value) {
  put_tag(WireTag::I64);
  put_u64(static_cast<std::uint64_t>(static_cast<std::int64_t>(value)));
}
void BinaryWriter::primitive(std::int16_t& value) {
  put_tag(WireTag::I64);
  put_u64(static_cast<std::uint64_t>(static_cast<std::int64_t>(value)));
}
void BinaryWriter::primitive(std::int32_t& value) {
  put_tag(WireTag::I64);
  put_u64(static_cast<std::uint64_t>(static_cast<std::int64_t>(value)));
}
void BinaryWriter::primitive(std::int64_t& value) {
  put_tag(WireTag::I64);
  put_u64(static_cast<std::uint64_t>(value));
}

void BinaryWriter::primitive(std::string& value) {
  put_tag(WireTag::Str);
  put_u32(static_cast<std::uint32_t>(value.size()));
  put_raw(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(value.data()),
                                        value.size()));
}

void BinaryWriter::primitive_digest(Digest& value) {
  put_tag(WireTag::Digest);
  put_raw(value.bytes());
}

void BinaryWriter::primitive_bytes(std::vector<std::uint8_t>& value) {
  put_tag(WireTag::Bytes);
  put_u32(static_cast<std::uint32_t>(value.size()));
  put_raw(value);
}

// ---------------------------------------------------------------------------
// BinaryReader
// ---------------------------------------------------------------------------

BinaryReader::BinaryReader(std::span<const std::uint8_t> data, std::size_t max_sequence)
    : data_(data), max_sequence_(max_sequence) {}

void BinaryReader::fail(ReasonCode code, std::string detail) {
  if (!status_.ok()) return;
  status_ = Status{code, std::move(detail)};
}

bool BinaryReader::expect_tag(WireTag tag) {
  if (!ok()) return false;
  const std::uint8_t raw = get_u8();
  if (!ok()) return false;
  if (raw != static_cast<std::uint8_t>(tag)) {
    fail(ReasonCode::InvalidTag, "unexpected wire tag");
    return false;
  }
  return true;
}

std::uint8_t BinaryReader::get_u8() {
  if (!ok()) return 0;
  if (position_ >= data_.size()) {
    fail(ReasonCode::TruncatedInput, "binary stream ended");
    return 0;
  }
  return data_[position_++];
}

std::uint32_t BinaryReader::get_u32() {
  std::uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(get_u8()) << (8u * i);
  }
  return value;
}

std::uint64_t BinaryReader::get_u64() {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(get_u8()) << (8u * i);
  }
  return value;
}

bool BinaryReader::take_raw(std::size_t count, std::span<const std::uint8_t>& out) {
  if (!ok()) return false;
  if (count > remaining()) {
    fail(ReasonCode::TruncatedInput, "binary blob exceeds remaining input");
    return false;
  }
  out = data_.subspan(position_, count);
  position_ += count;
  return true;
}

void BinaryReader::begin_object(std::string_view type_name) {
  if (!expect_tag(WireTag::Object)) return;
  std::string name;
  primitive(name);
  if (!ok()) return;
  if (name != type_name) {
    fail(ReasonCode::InvalidTag, "type mismatch: expected " + std::string{type_name} + ", found " +
                                    name);
  }
}

void BinaryReader::end_object() {}
void BinaryReader::begin_element() {}
void BinaryReader::end_element() {}
void BinaryReader::begin_field(std::string_view) {}
void BinaryReader::end_field() {}
void BinaryReader::optional_end() {}

void BinaryReader::begin_array(std::size_t& count) {
  if (!expect_tag(WireTag::Array)) {
    count = 0;
    return;
  }
  const std::uint32_t declared = get_u32();
  if (!ok()) {
    count = 0;
    return;
  }
  if (declared > max_sequence_) {
    fail(ReasonCode::OversizedInput, "declared sequence exceeds bound");
    count = 0;
    return;
  }
  count = declared;
}

void BinaryReader::end_array() {}

void BinaryReader::optional_begin(bool& present) {
  const std::uint8_t flag = get_u8();
  if (!ok()) {
    present = false;
    return;
  }
  if (flag > 1u) {
    fail(ReasonCode::MalformedInput, "invalid optional flag");
    present = false;
    return;
  }
  present = flag == 1u;
}

void BinaryReader::primitive(bool& value) {
  if (!expect_tag(WireTag::Bool)) return;
  const std::uint8_t raw = get_u8();
  if (!ok()) return;
  if (raw > 1u) {
    fail(ReasonCode::MalformedInput, "invalid boolean");
    return;
  }
  value = raw == 1u;
}

void BinaryReader::primitive(std::uint8_t& value) { read_integral(value, WireTag::U64); }
void BinaryReader::primitive(std::uint16_t& value) { read_integral(value, WireTag::U64); }
void BinaryReader::primitive(std::uint32_t& value) { read_integral(value, WireTag::U64); }
void BinaryReader::primitive(std::uint64_t& value) { read_integral(value, WireTag::U64); }
void BinaryReader::primitive(std::int8_t& value) { read_integral(value, WireTag::I64); }
void BinaryReader::primitive(std::int16_t& value) { read_integral(value, WireTag::I64); }
void BinaryReader::primitive(std::int32_t& value) { read_integral(value, WireTag::I64); }
void BinaryReader::primitive(std::int64_t& value) { read_integral(value, WireTag::I64); }

void BinaryReader::primitive(std::string& value) {
  if (!expect_tag(WireTag::Str)) return;
  const std::uint32_t length = get_u32();
  if (!ok()) return;
  if (length > kMaxDecodedBlob) {
    fail(ReasonCode::OversizedInput, "declared string exceeds bound");
    return;
  }
  std::span<const std::uint8_t> raw;
  if (!take_raw(length, raw)) return;
  value.assign(reinterpret_cast<const char*>(raw.data()), raw.size());
}

void BinaryReader::primitive_digest(Digest& value) {
  if (!expect_tag(WireTag::Digest)) return;
  std::span<const std::uint8_t> raw;
  if (!take_raw(Digest::kBytes, raw)) return;
  value = Digest::from_bytes(raw.data());
}

void BinaryReader::primitive_bytes(std::vector<std::uint8_t>& value) {
  if (!expect_tag(WireTag::Bytes)) return;
  const std::uint32_t length = get_u32();
  if (!ok()) return;
  if (length > kMaxDecodedBlob) {
    fail(ReasonCode::OversizedInput, "declared blob exceeds bound");
    return;
  }
  std::span<const std::uint8_t> raw;
  if (!take_raw(length, raw)) return;
  value.assign(raw.begin(), raw.end());
}

// ---------------------------------------------------------------------------
// JsonWriter
// ---------------------------------------------------------------------------

void JsonWriter::attach(JsonValue value) {
  if (stack_.empty()) {
    root_ = std::move(value);
    root_set_ = true;
    return;
  }
  Frame& frame = stack_.back();
  if (frame.value.is_array()) {
    frame.value.push_back(std::move(value));
    return;
  }
  if (has_pending_name_) {
    frame.value.set(pending_name_, std::move(value));
    has_pending_name_ = false;
    return;
  }
  frame.value.set(std::string{}, std::move(value));
}

void JsonWriter::begin_object(std::string_view) {
  Frame frame;
  frame.value = JsonValue::make_object();
  if (has_pending_name_) {
    frame.name = pending_name_;
    frame.named = true;
    has_pending_name_ = false;
  }
  stack_.push_back(std::move(frame));
}

void JsonWriter::end_object() {
  if (stack_.empty()) return;
  Frame frame = std::move(stack_.back());
  stack_.pop_back();
  if (frame.named) {
    pending_name_ = frame.name;
    has_pending_name_ = true;
  }
  attach(std::move(frame.value));
}

void JsonWriter::begin_array(std::size_t&) {
  Frame frame;
  frame.value = JsonValue::make_array();
  if (has_pending_name_) {
    frame.name = pending_name_;
    frame.named = true;
    has_pending_name_ = false;
  }
  stack_.push_back(std::move(frame));
}

void JsonWriter::end_array() {
  if (stack_.empty()) return;
  Frame frame = std::move(stack_.back());
  stack_.pop_back();
  if (frame.named) {
    pending_name_ = frame.name;
    has_pending_name_ = true;
  }
  attach(std::move(frame.value));
}

void JsonWriter::begin_element() {}
void JsonWriter::end_element() {}
void JsonWriter::end_field() {}
void JsonWriter::optional_end() {}

void JsonWriter::begin_field(std::string_view name) {
  pending_name_.assign(name);
  has_pending_name_ = true;
}

void JsonWriter::optional_begin(bool& present) {
  if (!present) attach(JsonValue{});
}

void JsonWriter::primitive(bool& value) { attach(JsonValue{value}); }
void JsonWriter::primitive(std::uint8_t& value) { attach(JsonValue{static_cast<std::int64_t>(value)}); }
void JsonWriter::primitive(std::uint16_t& value) { attach(JsonValue{static_cast<std::int64_t>(value)}); }
void JsonWriter::primitive(std::uint32_t& value) { attach(JsonValue{static_cast<std::int64_t>(value)}); }

void JsonWriter::primitive(std::uint64_t& value) {
  // Unsigned 64-bit values above INT64_MAX are written as strings so that no
  // JSON consumer can silently lose precision, and so that decode is exact.
  if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    attach(JsonValue{std::to_string(value)});
    return;
  }
  attach(JsonValue{static_cast<std::int64_t>(value)});
}

void JsonWriter::primitive(std::int8_t& value) { attach(JsonValue{static_cast<std::int64_t>(value)}); }
void JsonWriter::primitive(std::int16_t& value) { attach(JsonValue{static_cast<std::int64_t>(value)}); }
void JsonWriter::primitive(std::int32_t& value) { attach(JsonValue{static_cast<std::int64_t>(value)}); }
void JsonWriter::primitive(std::int64_t& value) { attach(JsonValue{value}); }
void JsonWriter::primitive(std::string& value) { attach(JsonValue{value}); }

void JsonWriter::primitive_digest(Digest& value) { attach(JsonValue{value.hex()}); }

void JsonWriter::primitive_bytes(std::vector<std::uint8_t>& value) {
  static const char* kHex = "0123456789abcdef";
  std::string hex;
  hex.resize(value.size() * 2);
  for (std::size_t i = 0; i < value.size(); ++i) {
    hex[i * 2] = kHex[value[i] >> 4u];
    hex[i * 2 + 1] = kHex[value[i] & 0x0Fu];
  }
  attach(JsonValue{std::move(hex)});
}

JsonValue JsonWriter::take() noexcept { return std::move(root_); }

// ---------------------------------------------------------------------------
// JsonReader
// ---------------------------------------------------------------------------

JsonReader::JsonReader(const JsonValue& root, std::size_t max_sequence)
    : root_(&root), max_sequence_(max_sequence) {}

void JsonReader::fail(ReasonCode code, std::string detail) {
  if (!status_.ok()) return;
  status_ = Status{code, std::move(detail)};
}

const JsonValue* JsonReader::current() const {
  if (!ok()) return nullptr;
  if (pending_ != nullptr) return pending_;
  if (!stack_.empty()) return stack_.back().node;
  return root_;
}

void JsonReader::begin_object(std::string_view type_name) {
  const JsonValue* node = current();
  if (node == nullptr) return;
  if (!node->is_object()) {
    fail(ReasonCode::MalformedInput, "expected json object for " + std::string{type_name});
    return;
  }
  Frame frame;
  frame.node = node;
  frame.consumed.assign(node->members().size(), false);
  stack_.push_back(std::move(frame));
  pending_ = nullptr;
}

void JsonReader::end_object() {
  if (!ok()) return;
  if (stack_.empty()) {
    fail(ReasonCode::MalformedInput, "unbalanced json object");
    return;
  }
  Frame& frame = stack_.back();
  for (std::size_t i = 0; i < frame.consumed.size(); ++i) {
    if (!frame.consumed[i]) {
      fail(ReasonCode::UnknownField, "unconsumed json member '" + frame.node->members()[i].first +
                                         "'");
      return;
    }
  }
  stack_.pop_back();
  pending_ = nullptr;
}

void JsonReader::begin_array(std::size_t& count) {
  const JsonValue* node = current();
  if (node == nullptr) {
    count = 0;
    return;
  }
  if (!node->is_array()) {
    fail(ReasonCode::MalformedInput, "expected json array");
    count = 0;
    return;
  }
  if (node->elements().size() > max_sequence_) {
    fail(ReasonCode::OversizedInput, "json array exceeds bound");
    count = 0;
    return;
  }
  count = node->elements().size();
  Frame frame;
  frame.node = node;
  frame.index = 0;
  stack_.push_back(std::move(frame));
  pending_ = nullptr;
}

void JsonReader::end_array() {
  if (!ok()) return;
  if (stack_.empty()) {
    fail(ReasonCode::MalformedInput, "unbalanced json array");
    return;
  }
  stack_.pop_back();
  pending_ = nullptr;
}

void JsonReader::begin_element() {
  if (!ok()) return;
  if (stack_.empty() || !stack_.back().node->is_array()) {
    fail(ReasonCode::MalformedInput, "json element outside array");
    return;
  }
  Frame& frame = stack_.back();
  if (frame.index >= frame.node->elements().size()) {
    fail(ReasonCode::TruncatedInput, "json array shorter than declared");
    return;
  }
  pending_ = &frame.node->elements()[frame.index];
  ++frame.index;
}

void JsonReader::end_element() { pending_ = nullptr; }

void JsonReader::begin_field(std::string_view name) {
  if (!ok()) return;
  if (stack_.empty() || !stack_.back().node->is_object()) {
    fail(ReasonCode::MalformedInput, "json field outside object");
    return;
  }
  Frame& frame = stack_.back();
  const JsonMemberList& members = frame.node->members();
  for (std::size_t i = 0; i < members.size(); ++i) {
    if (members[i].first == name) {
      if (frame.consumed[i]) {
        fail(ReasonCode::DuplicateIdentity, "json member consumed twice");
        return;
      }
      frame.consumed[i] = true;
      pending_ = &members[i].second;
      return;
    }
  }
  fail(ReasonCode::MissingField, "missing json member '" + std::string{name} + "'");
}

void JsonReader::end_field() { pending_ = nullptr; }
void JsonReader::optional_end() {}

void JsonReader::optional_begin(bool& present) {
  const JsonValue* node = current();
  if (node == nullptr) {
    present = false;
    return;
  }
  present = !node->is_null();
  if (!present) pending_ = nullptr;
}

void JsonReader::read_integral_signed(std::int64_t& out) {
  const JsonValue* node = current();
  if (node == nullptr) return;
  if (node->kind() == JsonValue::Kind::Int) {
    out = node->as_int();
    pending_ = nullptr;
    return;
  }
  fail(ReasonCode::MalformedInput, "expected json integer");
}

void JsonReader::read_integral_unsigned(std::uint64_t& out) {
  const JsonValue* node = current();
  if (node == nullptr) return;
  if (node->kind() == JsonValue::Kind::Int) {
    if (node->as_int() < 0) {
      fail(ReasonCode::ValueOutOfRange, "negative value for unsigned field");
      return;
    }
    out = static_cast<std::uint64_t>(node->as_int());
    pending_ = nullptr;
    return;
  }
  if (node->kind() == JsonValue::Kind::String) {
    const std::string& text = node->as_string();
    if (text.empty() || text.size() > 20) {
      fail(ReasonCode::ValueOutOfRange, "integer text out of range");
      return;
    }
    std::uint64_t value = 0;
    for (char c : text) {
      if (c < '0' || c > '9') {
        fail(ReasonCode::MalformedInput, "expected json integer text");
        return;
      }
      const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
      std::uint64_t scaled = 0;
      if (!checked_mul(value, std::uint64_t{10}, scaled) || !checked_add(scaled, digit, value)) {
        fail(ReasonCode::ValueOutOfRange, "integer text overflow");
        return;
      }
    }
    out = value;
    pending_ = nullptr;
    return;
  }
  fail(ReasonCode::MalformedInput, "expected json integer");
}

template <class T>
static void assign_integral(JsonReader& reader, T& value, std::uint64_t raw) {
  if (raw > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
    reader.fail(ReasonCode::ValueOutOfRange, "json integer out of range");
    return;
  }
  value = static_cast<T>(raw);
}

void JsonReader::primitive(bool& value) {
  const JsonValue* node = current();
  if (node == nullptr) return;
  if (node->kind() != JsonValue::Kind::Bool) {
    fail(ReasonCode::MalformedInput, "expected json boolean");
    return;
  }
  value = node->as_bool();
  pending_ = nullptr;
}

void JsonReader::primitive(std::uint8_t& value) {
  std::uint64_t raw = 0;
  read_integral_unsigned(raw);
  if (!ok()) return;
  assign_integral(*this, value, raw);
}
void JsonReader::primitive(std::uint16_t& value) {
  std::uint64_t raw = 0;
  read_integral_unsigned(raw);
  if (!ok()) return;
  assign_integral(*this, value, raw);
}
void JsonReader::primitive(std::uint32_t& value) {
  std::uint64_t raw = 0;
  read_integral_unsigned(raw);
  if (!ok()) return;
  assign_integral(*this, value, raw);
}
void JsonReader::primitive(std::uint64_t& value) {
  std::uint64_t raw = 0;
  read_integral_unsigned(raw);
  if (!ok()) return;
  value = raw;
}
void JsonReader::primitive(std::int8_t& value) {
  std::int64_t raw = 0;
  read_integral_signed(raw);
  if (!ok()) return;
  if (raw < std::numeric_limits<std::int8_t>::min() ||
      raw > std::numeric_limits<std::int8_t>::max()) {
    fail(ReasonCode::ValueOutOfRange, "json integer out of range");
    return;
  }
  value = static_cast<std::int8_t>(raw);
}
void JsonReader::primitive(std::int16_t& value) {
  std::int64_t raw = 0;
  read_integral_signed(raw);
  if (!ok()) return;
  if (raw < std::numeric_limits<std::int16_t>::min() ||
      raw > std::numeric_limits<std::int16_t>::max()) {
    fail(ReasonCode::ValueOutOfRange, "json integer out of range");
    return;
  }
  value = static_cast<std::int16_t>(raw);
}
void JsonReader::primitive(std::int32_t& value) {
  std::int64_t raw = 0;
  read_integral_signed(raw);
  if (!ok()) return;
  if (raw < std::numeric_limits<std::int32_t>::min() ||
      raw > std::numeric_limits<std::int32_t>::max()) {
    fail(ReasonCode::ValueOutOfRange, "json integer out of range");
    return;
  }
  value = static_cast<std::int32_t>(raw);
}
void JsonReader::primitive(std::int64_t& value) {
  std::int64_t raw = 0;
  read_integral_signed(raw);
  if (!ok()) return;
  value = raw;
}

void JsonReader::primitive(std::string& value) {
  const JsonValue* node = current();
  if (node == nullptr) return;
  if (node->kind() != JsonValue::Kind::String) {
    fail(ReasonCode::MalformedInput, "expected json string");
    return;
  }
  value = node->as_string();
  pending_ = nullptr;
}

void JsonReader::primitive_digest(Digest& value) {
  const JsonValue* node = current();
  if (node == nullptr) return;
  if (node->kind() != JsonValue::Kind::String) {
    fail(ReasonCode::MalformedInput, "expected json digest string");
    return;
  }
  Result<Digest> parsed = Digest::parse_hex(node->as_string());
  if (!parsed) {
    fail(parsed.status().code(), parsed.status().detail());
    return;
  }
  value = parsed.value();
  pending_ = nullptr;
}

void JsonReader::primitive_bytes(std::vector<std::uint8_t>& value) {
  const JsonValue* node = current();
  if (node == nullptr) return;
  if (node->kind() != JsonValue::Kind::String) {
    fail(ReasonCode::MalformedInput, "expected json hex string");
    return;
  }
  const std::string& text = node->as_string();
  if (text.size() % 2 != 0 || text.size() / 2 > kMaxDecodedBlob) {
    fail(ReasonCode::OversizedInput, "invalid hex blob length");
    return;
  }
  value.clear();
  value.reserve(text.size() / 2);
  const auto nibble = [](char c, bool& ok) {
    if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<std::uint8_t>(c - 'A' + 10);
    ok = false;
    return static_cast<std::uint8_t>(0);
  };
  for (std::size_t i = 0; i < text.size(); i += 2) {
    bool ok = true;
    const std::uint8_t high = nibble(text[i], ok);
    const std::uint8_t low = nibble(text[i + 1], ok);
    if (!ok) {
      fail(ReasonCode::InvalidEncoding, "invalid hex digit");
      return;
    }
    value.push_back(static_cast<std::uint8_t>((high << 4u) | low));
  }
  pending_ = nullptr;
}

}  // namespace dpu::fabric
