#pragma once

// Canonical archives.
//
// Every model type implements a single \c visit member template:
//
//   template <class Ar> void visit(Ar& ar) {
//     ar.begin_object("ServiceDefinition");
//     field(ar, "id", id);
//     ...
//     ar.end_object();
//   }
//
// One visit() therefore serves four directions: canonical binary encode and
// decode (used for digests, persistence and the transport protocol) and JSON
// encode and decode (used for the machine-readable export and for CLI input).
// Because the same code drives both directions, a value that round-trips through
// persistence is structurally identical to the value that was accepted.
//
// The binary encoding is self-describing enough to detect drift: every object
// carries its type name, every value carries a tag, enumerations carry their
// canonical name (never their ordinal), and sequence counts are explicit. The
// binary reader is order-strict and refuses on the first mismatch, holding that
// first refusal as a sticky status. The JSON reader is name-based and refuses
// missing members as well as members that no visit() consumed.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "dpu/fabric/core/digest.hpp"
#include "dpu/fabric/core/json.hpp"
#include "dpu/fabric/core/result.hpp"
#include "dpu/fabric/core/types.hpp"

namespace dpu::fabric {

/// Maximum number of elements a decoded sequence may declare. Checked before
/// allocation so a hostile length prefix cannot request unbounded memory.
inline constexpr std::size_t kMaxDecodedSequence = 1u << 16;

/// Maximum number of bytes a decoded string or blob may declare.
inline constexpr std::size_t kMaxDecodedBlob = 1u << 20;

enum class WireTag : std::uint8_t {
  Null = 0,
  Bool = 1,
  U64 = 2,
  I64 = 3,
  Str = 4,
  Bytes = 5,
  Array = 6,
  Object = 7,
  Enum = 8,
  Digest = 9,
};

enum class WireType : std::uint8_t {
  Bool = 0,
  U8 = 1,
  U16 = 2,
  U32 = 3,
  U64 = 4,
  I8 = 5,
  I16 = 6,
  I32 = 7,
  I64 = 8,
};

// ---------------------------------------------------------------------------
// Binary writer
// ---------------------------------------------------------------------------

class BinaryWriter {
 public:
  static constexpr bool kIsReader = false;

  explicit BinaryWriter(std::size_t reserve = 4096);

  void begin_object(std::string_view type_name);
  void end_object();
  void begin_array(std::size_t& count);
  void end_array();
  void begin_element();
  void end_element();
  void begin_field(std::string_view name);
  void end_field();
  void optional_begin(bool& present);
  void optional_end();

  void primitive(bool& value);
  void primitive(std::uint8_t& value);
  void primitive(std::uint16_t& value);
  void primitive(std::uint32_t& value);
  void primitive(std::uint64_t& value);
  void primitive(std::int8_t& value);
  void primitive(std::int16_t& value);
  void primitive(std::int32_t& value);
  void primitive(std::int64_t& value);
  void primitive(std::string& value);
  void primitive_digest(Digest& value);
  template <class E>
  void primitive_enum(E& value) {
    put_tag(WireTag::Enum);
    std::string name{to_string(value)};
    primitive(name);
  }
  void primitive_bytes(std::vector<std::uint8_t>& value);

  /// Writers cannot fail; the accessor exists so generic code can be written
  /// once for both directions.
  [[nodiscard]] bool ok() const noexcept { return true; }
  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return buffer_; }
  [[nodiscard]] std::vector<std::uint8_t> take() noexcept { return std::move(buffer_); }
  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
  void reserve(std::size_t extra);

 private:
  void put_tag(WireTag tag);
  void put_u8(std::uint8_t value);
  void put_u32(std::uint32_t value);
  void put_u64(std::uint64_t value);
  void put_raw(std::span<const std::uint8_t> data);

  std::vector<std::uint8_t> buffer_;
};

// ---------------------------------------------------------------------------
// Binary reader
// ---------------------------------------------------------------------------

class BinaryReader {
 public:
  static constexpr bool kIsReader = true;

  explicit BinaryReader(std::span<const std::uint8_t> data,
                        std::size_t max_sequence = kMaxDecodedSequence);

  void begin_object(std::string_view type_name);
  void end_object();
  void begin_array(std::size_t& count);
  void end_array();
  void begin_element();
  void end_element();
  void begin_field(std::string_view name);
  void end_field();
  void optional_begin(bool& present);
  void optional_end();

  void primitive(bool& value);
  void primitive(std::uint8_t& value);
  void primitive(std::uint16_t& value);
  void primitive(std::uint32_t& value);
  void primitive(std::uint64_t& value);
  void primitive(std::int8_t& value);
  void primitive(std::int16_t& value);
  void primitive(std::int32_t& value);
  void primitive(std::int64_t& value);
  void primitive(std::string& value);
  void primitive_digest(Digest& value);
  template <class E>
  void primitive_enum(E& value) {
    if (!expect_tag(WireTag::Enum)) return;
    std::string name;
    primitive(name);
    if (!ok()) return;
    if (!from_string(name, value)) {
      fail(ReasonCode::InvalidEnumeration, name);
    }
  }
  void primitive_bytes(std::vector<std::uint8_t>& value);

  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }
  [[nodiscard]] bool at_end() const noexcept { return position_ == data_.size(); }
  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - position_; }
  void fail(ReasonCode code, std::string detail);

 private:
  bool expect_tag(WireTag tag);
  std::uint8_t get_u8();
  std::uint32_t get_u32();
  std::uint64_t get_u64();
  bool take_raw(std::size_t count, std::span<const std::uint8_t>& out);
  template <class T>
  void read_integral(T& value, WireTag tag) {
    if (!expect_tag(tag)) return;
    const std::uint64_t raw = get_u64();
    if (!ok()) return;
    if constexpr (std::is_signed_v<T>) {
      if (raw > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
        fail(ReasonCode::ValueOutOfRange, "signed integer field out of range");
        return;
      }
    } else {
      if (raw > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
        fail(ReasonCode::ValueOutOfRange, "unsigned integer field out of range");
        return;
      }
    }
    value = static_cast<T>(raw);
  }

  std::span<const std::uint8_t> data_;
  std::size_t position_{0};
  std::size_t max_sequence_;
  Status status_{};
};

// ---------------------------------------------------------------------------
// JSON writer
// ---------------------------------------------------------------------------

class JsonWriter {
 public:
  static constexpr bool kIsReader = false;

  JsonWriter() = default;

  void begin_object(std::string_view type_name);
  void end_object();
  void begin_array(std::size_t& count);
  void end_array();
  void begin_element();
  void end_element();
  void begin_field(std::string_view name);
  void end_field();
  void optional_begin(bool& present);
  void optional_end();

  void primitive(bool& value);
  void primitive(std::uint8_t& value);
  void primitive(std::uint16_t& value);
  void primitive(std::uint32_t& value);
  void primitive(std::uint64_t& value);
  void primitive(std::int8_t& value);
  void primitive(std::int16_t& value);
  void primitive(std::int32_t& value);
  void primitive(std::int64_t& value);
  void primitive(std::string& value);
  void primitive_digest(Digest& value);
  template <class E>
  void primitive_enum(E& value) {
    attach(JsonValue{std::string{to_string(value)}});
  }
  void primitive_bytes(std::vector<std::uint8_t>& value);

  [[nodiscard]] bool ok() const noexcept { return true; }
  [[nodiscard]] JsonValue take() noexcept;
  [[nodiscard]] const JsonValue& root() const noexcept { return root_; }

 private:
  struct Frame {
    JsonValue value;
    std::string name;
    bool named{false};
  };

  void attach(JsonValue value);

  std::vector<Frame> stack_{};
  JsonValue root_{};
  std::string pending_name_{};
  bool has_pending_name_{false};
  bool root_set_{false};
};

// ---------------------------------------------------------------------------
// JSON reader
// ---------------------------------------------------------------------------

class JsonReader {
 public:
  static constexpr bool kIsReader = true;

  explicit JsonReader(const JsonValue& root, std::size_t max_sequence = kMaxDecodedSequence);

  void begin_object(std::string_view type_name);
  void end_object();
  void begin_array(std::size_t& count);
  void end_array();
  void begin_element();
  void end_element();
  void begin_field(std::string_view name);
  void end_field();
  void optional_begin(bool& present);
  void optional_end();

  void primitive(bool& value);
  void primitive(std::uint8_t& value);
  void primitive(std::uint16_t& value);
  void primitive(std::uint32_t& value);
  void primitive(std::uint64_t& value);
  void primitive(std::int8_t& value);
  void primitive(std::int16_t& value);
  void primitive(std::int32_t& value);
  void primitive(std::int64_t& value);
  void primitive(std::string& value);
  void primitive_digest(Digest& value);
  template <class E>
  void primitive_enum(E& value) {
    const JsonValue* node = current();
    if (node == nullptr) return;
    if (node->kind() != JsonValue::Kind::String) {
      fail(ReasonCode::MalformedInput, "enumeration must be a string");
      return;
    }
    if (!from_string(node->as_string(), value)) {
      fail(ReasonCode::InvalidEnumeration, node->as_string());
    }
    pending_ = nullptr;
  }
  void primitive_bytes(std::vector<std::uint8_t>& value);

  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }
  void fail(ReasonCode code, std::string detail);

 private:
  struct Frame {
    const JsonValue* node{nullptr};
    std::size_t index{0};
    std::vector<bool> consumed{};
  };

  [[nodiscard]] const JsonValue* current() const;
  void read_integral_signed(std::int64_t& out);
  void read_integral_unsigned(std::uint64_t& out);

  const JsonValue* root_{nullptr};
  std::vector<Frame> stack_{};
  const JsonValue* pending_{nullptr};
  std::size_t max_sequence_;
  Status status_{};
};

// ---------------------------------------------------------------------------
// Generic field visitors
// ---------------------------------------------------------------------------

template <class T>
struct is_std_optional : std::false_type {};
template <class T>
struct is_std_optional<std::optional<T>> : std::true_type {};

template <class T>
struct is_std_vector : std::false_type {};
template <class T>
struct is_std_vector<std::vector<T>> : std::true_type {};

template <class T>
struct is_strong_id : std::false_type {};
template <class Tag>
struct is_strong_id<StrongId<Tag>> : std::true_type {};

template <class T>
struct is_counter : std::false_type {};
template <class Tag>
struct is_counter<Counter<Tag>> : std::true_type {};

template <class T>
struct is_std_variant : std::false_type {};
template <class... Ts>
struct is_std_variant<std::variant<Ts...>> : std::true_type {};

/// Reads a variant alternative selected by `index`. Alternatives must be
/// default constructible. An out-of-range index is refused rather than clamped.
template <class V, std::size_t I = 0, class Ar>
void variant_read(Ar& ar, V& value, std::uint32_t index) {
  if constexpr (I < std::variant_size_v<V>) {
    if (index == I) {
      value.template emplace<I>();
      do_value(ar, std::get<I>(value));
      return;
    }
    variant_read<V, I + 1>(ar, value, index);
  } else {
    ar.fail(ReasonCode::InvalidEnumeration, "variant index out of range");
  }
}

/// Writes a variant as an object holding its alternative index and value, so the
/// encoding never depends on alternative declaration order alone.
template <class Ar, class V>
void variant_write(Ar& ar, V& value) {
  ar.begin_object("Variant");
  std::uint32_t index = static_cast<std::uint32_t>(value.index());
  field(ar, "index", index);
  std::visit(
      [&ar](auto& alternative) {
        ar.begin_field("value");
        do_value(ar, alternative);
        ar.end_field();
      },
      value);
  ar.end_object();
}

template <class Ar, class V>
void variant_value(Ar& ar, V& value) {
  if constexpr (Ar::kIsReader) {
    ar.begin_object("Variant");
    std::uint32_t index = 0;
    field(ar, "index", index);
    if (ar.ok()) {
      if (index >= std::variant_size_v<V>) {
        ar.fail(ReasonCode::InvalidEnumeration, "variant index out of range");
      } else {
        ar.begin_field("value");
        variant_read(ar, value, index);
        ar.end_field();
      }
    }
    ar.end_object();
  } else {
    variant_write(ar, value);
  }
}
template <class Ar, class T>
void do_value(Ar& ar, T& value);

/// One model field. The name is used by the JSON archives; the binary archives
/// are order-strict and do not carry field names.
template <class Ar, class T>
void field(Ar& ar, std::string_view name, T& value) {
  ar.begin_field(name);
  do_value(ar, value);
  ar.end_field();
}

template <class Ar, class T>
void do_value(Ar& ar, T& value) {
  if constexpr (std::is_same_v<T, bool>) {
    ar.primitive(value);
  } else if constexpr (std::is_enum_v<T>) {
    ar.primitive_enum(value);
  } else if constexpr (std::is_integral_v<T>) {
    ar.primitive(value);
  } else if constexpr (std::is_same_v<T, std::string>) {
    ar.primitive(value);
  } else if constexpr (std::is_same_v<T, Digest>) {
    ar.primitive_digest(value);
  } else if constexpr (std::is_same_v<T, std::vector<std::uint8_t>>) {
    ar.primitive_bytes(value);
  } else if constexpr (is_strong_id<T>::value) {
    std::string text = value.str();
    ar.primitive(text);
    if constexpr (Ar::kIsReader) {
      if (text.empty()) {
        // An absent identity is encoded as an empty string and decoded back to
        // an absent identity. It is never turned into a fabricated value, and
        // non-empty text is still parsed and validated.
        value = T{};
      } else {
        Result<T> parsed = T::parse(text);
        if (!parsed) {
          ar.fail(parsed.status().code(), parsed.status().detail());
        } else {
          value = std::move(parsed).value();
        }
      }
    }
  } else if constexpr (is_counter<T>::value) {
    std::uint64_t raw = value.value();
    ar.primitive(raw);
    if constexpr (Ar::kIsReader) {
      value = T{raw};
    }
  } else if constexpr (is_std_variant<T>::value) {
    variant_value(ar, value);
  } else if constexpr (is_std_optional<T>::value) {
    bool present = value.has_value();
    ar.optional_begin(present);
    if (present) {
      if (!value.has_value()) value.emplace();
      do_value(ar, *value);
    }
    ar.optional_end();
  } else if constexpr (is_std_vector<T>::value) {
    std::size_t count = value.size();
    ar.begin_array(count);
    if constexpr (Ar::kIsReader) {
      if (ar.ok()) value.resize(count);
    }
    if (ar.ok()) {
      for (std::size_t i = 0; i < count; ++i) {
        ar.begin_element();
        do_value(ar, value[i]);
        ar.end_element();
        if constexpr (Ar::kIsReader) {
          if (!ar.ok()) break;
        }
      }
    }
    ar.end_array();
  } else {
    value.visit(ar);
  }
}

// ---------------------------------------------------------------------------
// Whole-value helpers
// ---------------------------------------------------------------------------

template <class T>
[[nodiscard]] std::vector<std::uint8_t> encode_binary(const T& value, std::size_t reserve = 4096) {
  BinaryWriter writer{reserve};
  // Writers never modify the visited value; the cast lets one visit() serve both
  // directions without duplicating every model type. Routing through do_value
  // keeps variants, optionals and sequences on the same path as structs.
  do_value(writer, const_cast<T&>(value));
  return writer.take();
}

template <class T>
[[nodiscard]] Result<T> decode_binary(std::span<const std::uint8_t> data,
                                      std::size_t max_sequence = kMaxDecodedSequence) {
  BinaryReader reader{data, max_sequence};
  T value{};
  do_value(reader, value);
  if (!reader.ok()) return reader.status();
  if (!reader.at_end()) {
    return refuse(ReasonCode::MalformedInput, "trailing bytes after decoded value");
  }
  return value;
}

template <class T>
[[nodiscard]] JsonValue encode_json(const T& value) {
  JsonWriter writer;
  do_value(writer, const_cast<T&>(value));
  return writer.take();
}

template <class T>
[[nodiscard]] Result<T> decode_json(const JsonValue& value,
                                    std::size_t max_sequence = kMaxDecodedSequence) {
  JsonReader reader{value, max_sequence};
  T out{};
  do_value(reader, out);
  if (!reader.ok()) return reader.status();
  return out;
}

/// SHA-256 over the canonical binary encoding of a value.
template <class T>
[[nodiscard]] Digest canonical_digest(const T& value) {
  const std::vector<std::uint8_t> bytes = encode_binary(value);
  return sha256(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
}

}  // namespace dpu::fabric
