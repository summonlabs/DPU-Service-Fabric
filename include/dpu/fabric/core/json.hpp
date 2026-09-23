#pragma once

// A small strict JSON DOM, parser and canonical writer.
//
// The runtime deliberately carries no third-party JSON dependency. The parser is
// strict by design: duplicate member names, floating point numbers, comments,
// trailing content, control characters, excessive nesting and oversized inputs
// are all refused with stable reason codes. Refusing is preferred over guessing,
// because every value that reaches the model layer can influence placement,
// authority or lifecycle decisions.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "dpu/fabric/core/result.hpp"

namespace dpu::fabric {

class JsonValue;
using JsonMemberList = std::vector<std::pair<std::string, JsonValue>>;

class JsonValue {
 public:
  enum class Kind : std::uint8_t { Null, Bool, Int, String, Array, Object };

  static constexpr std::size_t kMaxDepth = 64;

  JsonValue() = default;
  explicit JsonValue(bool value) : kind_(Kind::Bool), bool_(value) {}
  explicit JsonValue(std::int64_t value) : kind_(Kind::Int), int_(value) {}
  explicit JsonValue(std::string value) : kind_(Kind::String), text_(std::move(value)) {}
  JsonValue(Kind kind, JsonMemberList members, std::vector<JsonValue> elements);

  [[nodiscard]] static JsonValue make_array();
  [[nodiscard]] static JsonValue make_object();

  [[nodiscard]] Kind kind() const noexcept { return kind_; }
  [[nodiscard]] bool is_null() const noexcept { return kind_ == Kind::Null; }
  [[nodiscard]] bool is_object() const noexcept { return kind_ == Kind::Object; }
  [[nodiscard]] bool is_array() const noexcept { return kind_ == Kind::Array; }

  [[nodiscard]] bool as_bool() const noexcept { return bool_; }
  [[nodiscard]] std::int64_t as_int() const noexcept { return int_; }
  [[nodiscard]] const std::string& as_string() const noexcept { return text_; }
  [[nodiscard]] const std::vector<JsonValue>& elements() const noexcept { return elements_; }
  [[nodiscard]] const JsonMemberList& members() const noexcept { return members_; }

  void push_back(JsonValue value) { elements_.push_back(std::move(value)); }
  void set(std::string name, JsonValue value);

  /// Member lookup; null when absent.
  [[nodiscard]] const JsonValue* find(std::string_view name) const noexcept;

  /// Canonical text. Members are emitted in insertion order, which every model
  /// visit() fixes at compile time, so the output is deterministic.
  [[nodiscard]] std::string to_text(bool pretty = true) const;

 private:
  void write_to(std::string& out, bool pretty, std::size_t indent) const;

  Kind kind_{Kind::Null};
  bool bool_{false};
  std::int64_t int_{0};
  std::string text_{};
  JsonMemberList members_{};
  std::vector<JsonValue> elements_{};
};

/// Parses strict JSON. Refuses oversized, malformed, duplicated and floating
/// point input rather than coercing it.
[[nodiscard]] Result<JsonValue> parse_json(std::string_view text, std::size_t max_bytes = 1u << 20);

/// Renders a byte string as a JSON string literal (used for canonical exports of
/// fields whose text may contain arbitrary bytes).
[[nodiscard]] std::string json_escape(std::string_view text);

}  // namespace dpu::fabric
