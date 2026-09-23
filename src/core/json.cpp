#include "dpu/fabric/core/json.hpp"

#include <cctype>
#include <cstdio>
#include <limits>

namespace dpu::fabric {
namespace {

class Parser {
 public:
  Parser(std::string_view text, std::size_t max_bytes) : text_(text), max_bytes_(max_bytes) {}

  Result<JsonValue> run() {
    if (text_.size() > max_bytes_) {
      return refuse(ReasonCode::OversizedInput, "json document exceeds limit");
    }
    skip_ws();
    JsonValue root;
    Status status = parse_value(root, 0);
    if (!status.ok()) return status;
    skip_ws();
    if (position_ != text_.size()) {
      return refuse(ReasonCode::MalformedInput, "trailing content after json value");
    }
    return root;
  }

 private:
  void skip_ws() {
    while (position_ < text_.size()) {
      const char c = text_[position_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++position_;
      } else {
        break;
      }
    }
  }

  Status parse_value(JsonValue& out, std::size_t depth) {
    if (depth > JsonValue::kMaxDepth) {
      return refuse(ReasonCode::DepthLimitExceeded, "json nesting too deep");
    }
    if (position_ >= text_.size()) {
      return refuse(ReasonCode::TruncatedInput, "unexpected end of json");
    }
    const char c = text_[position_];
    switch (c) {
      case '{':
        return parse_object(out, depth);
      case '[':
        return parse_array(out, depth);
      case '"': {
        std::string text;
        Status status = parse_string(text);
        if (!status.ok()) return status;
        out = JsonValue{std::move(text)};
        return Status::success();
      }
      case 't':
        return parse_literal("true", JsonValue{true}, out);
      case 'f':
        return parse_literal("false", JsonValue{false}, out);
      case 'n':
        return parse_literal("null", JsonValue{}, out);
      default:
        break;
    }
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(out);
    return refuse(ReasonCode::MalformedInput, "unexpected character in json");
  }

  Status parse_literal(std::string_view literal, JsonValue value, JsonValue& out) {
    if (text_.substr(position_, literal.size()) != literal) {
      return refuse(ReasonCode::MalformedInput, "invalid literal");
    }
    position_ += literal.size();
    out = std::move(value);
    return Status::success();
  }

  Status parse_number(JsonValue& out) {
    const std::size_t start = position_;
    if (position_ < text_.size() && text_[position_] == '-') ++position_;
    const std::size_t digits_start = position_;
    while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_]))) {
      ++position_;
    }
    if (position_ == digits_start) {
      return refuse(ReasonCode::MalformedInput, "json number requires digits");
    }
    if (position_ < text_.size() && (text_[position_] == '.' || text_[position_] == 'e' ||
                                     text_[position_] == 'E')) {
      return refuse(ReasonCode::UnsupportedValue, "floating point json numbers are not accepted");
    }
    const std::string_view digits = text_.substr(start, position_ - start);
    if (digits.size() > 19) {
      return refuse(ReasonCode::ValueOutOfRange, "json integer too large");
    }
    bool negative = false;
    std::size_t index = 0;
    if (digits[0] == '-') {
      negative = true;
      index = 1;
    }
    std::int64_t value = 0;
    for (; index < digits.size(); ++index) {
      const std::int64_t digit = static_cast<std::int64_t>(digits[index] - '0');
      if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10) {
        return refuse(ReasonCode::ValueOutOfRange, "json integer overflow");
      }
      value = value * 10 + digit;
    }
    out = JsonValue{negative ? -value : value};
    return Status::success();
  }

  static void append_utf8(std::string& out, std::uint32_t code_point) {
    if (code_point <= 0x7Fu) {
      out.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FFu) {
      out.push_back(static_cast<char>(0xC0u | (code_point >> 6u)));
      out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    } else if (code_point <= 0xFFFFu) {
      out.push_back(static_cast<char>(0xE0u | (code_point >> 12u)));
      out.push_back(static_cast<char>(0x80u | ((code_point >> 6u) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    } else {
      out.push_back(static_cast<char>(0xF0u | (code_point >> 18u)));
      out.push_back(static_cast<char>(0x80u | ((code_point >> 12u) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | ((code_point >> 6u) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    }
  }

  Status parse_hex4(std::uint32_t& out) {
    if (position_ + 4 > text_.size()) {
      return refuse(ReasonCode::TruncatedInput, "truncated unicode escape");
    }
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[position_++];
      std::uint32_t digit = 0;
      if (c >= '0' && c <= '9') {
        digit = static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = static_cast<std::uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        digit = static_cast<std::uint32_t>(c - 'A' + 10);
      } else {
        return refuse(ReasonCode::InvalidEncoding, "invalid unicode escape");
      }
      value = (value << 4u) | digit;
    }
    out = value;
    return Status::success();
  }

  Status parse_string(std::string& out) {
    if (text_[position_] != '"') return refuse(ReasonCode::MalformedInput, "expected string");
    ++position_;
    while (true) {
      if (position_ >= text_.size()) {
        return refuse(ReasonCode::TruncatedInput, "unterminated json string");
      }
      const unsigned char c = static_cast<unsigned char>(text_[position_]);
      if (c == '"') {
        ++position_;
        return Status::success();
      }
      if (c < 0x20u) return refuse(ReasonCode::InvalidEncoding, "control character in json string");
      if (c != '\\') {
        out.push_back(static_cast<char>(c));
        ++position_;
        continue;
      }
      ++position_;
      if (position_ >= text_.size()) {
        return refuse(ReasonCode::TruncatedInput, "truncated json escape");
      }
      const char esc = text_[position_++];
      switch (esc) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          std::uint32_t code_point = 0;
          Status status = parse_hex4(code_point);
          if (!status.ok()) return status;
          if (code_point >= 0xD800u && code_point <= 0xDBFFu) {
            if (position_ + 1 >= text_.size() || text_[position_] != '\\' ||
                text_[position_ + 1] != 'u') {
              return refuse(ReasonCode::InvalidEncoding, "unpaired surrogate");
            }
            position_ += 2;
            std::uint32_t low = 0;
            status = parse_hex4(low);
            if (!status.ok()) return status;
            if (low < 0xDC00u || low > 0xDFFFu) {
              return refuse(ReasonCode::InvalidEncoding, "invalid low surrogate");
            }
            code_point = 0x10000u + ((code_point - 0xD800u) << 10u) + (low - 0xDC00u);
          } else if (code_point >= 0xDC00u && code_point <= 0xDFFFu) {
            return refuse(ReasonCode::InvalidEncoding, "unpaired low surrogate");
          }
          append_utf8(out, code_point);
          break;
        }
        default:
          return refuse(ReasonCode::InvalidEncoding, "unknown json escape");
      }
    }
  }

  Status parse_object(JsonValue& out, std::size_t depth) {
    ++position_;  // '{'
    JsonValue object = JsonValue::make_object();
    skip_ws();
    if (position_ < text_.size() && text_[position_] == '}') {
      ++position_;
      out = std::move(object);
      return Status::success();
    }
    while (true) {
      skip_ws();
      if (position_ >= text_.size() || text_[position_] != '"') {
        return refuse(ReasonCode::MalformedInput, "json object requires a string member name");
      }
      std::string name;
      Status status = parse_string(name);
      if (!status.ok()) return status;
      skip_ws();
      if (position_ >= text_.size() || text_[position_] != ':') {
        return refuse(ReasonCode::MalformedInput, "json member requires ':'");
      }
      ++position_;
      skip_ws();
      JsonValue value;
      status = parse_value(value, depth + 1);
      if (!status.ok()) return status;
      if (object.find(name) != nullptr) {
        return refuse(ReasonCode::DuplicateIdentity, "duplicate json member '" + name + "'");
      }
      object.set(std::move(name), std::move(value));
      skip_ws();
      if (position_ < text_.size() && text_[position_] == ',') {
        ++position_;
        continue;
      }
      if (position_ < text_.size() && text_[position_] == '}') {
        ++position_;
        out = std::move(object);
        return Status::success();
      }
      return refuse(ReasonCode::MalformedInput, "json object not terminated");
    }
  }

  Status parse_array(JsonValue& out, std::size_t depth) {
    ++position_;  // '['
    JsonValue array = JsonValue::make_array();
    skip_ws();
    if (position_ < text_.size() && text_[position_] == ']') {
      ++position_;
      out = std::move(array);
      return Status::success();
    }
    while (true) {
      skip_ws();
      JsonValue value;
      Status status = parse_value(value, depth + 1);
      if (!status.ok()) return status;
      array.push_back(std::move(value));
      skip_ws();
      if (position_ < text_.size() && text_[position_] == ',') {
        ++position_;
        continue;
      }
      if (position_ < text_.size() && text_[position_] == ']') {
        ++position_;
        out = std::move(array);
        return Status::success();
      }
      return refuse(ReasonCode::MalformedInput, "json array not terminated");
    }
  }

  std::string_view text_;
  std::size_t max_bytes_;
  std::size_t position_{0};
};

}  // namespace

JsonValue::JsonValue(Kind kind, JsonMemberList members, std::vector<JsonValue> elements)
    : kind_(kind), members_(std::move(members)), elements_(std::move(elements)) {}

JsonValue JsonValue::make_array() { return JsonValue{Kind::Array, {}, {}}; }
JsonValue JsonValue::make_object() { return JsonValue{Kind::Object, {}, {}}; }

void JsonValue::set(std::string name, JsonValue value) {
  for (auto& member : members_) {
    if (member.first == name) {
      member.second = std::move(value);
      return;
    }
  }
  members_.emplace_back(std::move(name), std::move(value));
}

const JsonValue* JsonValue::find(std::string_view name) const noexcept {
  for (const auto& member : members_) {
    if (member.first == name) return &member.second;
  }
  return nullptr;
}

std::string json_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  out.push_back('"');
  for (char raw : text) {
    const unsigned char c = static_cast<unsigned char>(raw);
    switch (c) {
      case '"': out.append("\\\""); break;
      case '\\': out.append("\\\\"); break;
      case '\b': out.append("\\b"); break;
      case '\f': out.append("\\f"); break;
      case '\n': out.append("\\n"); break;
      case '\r': out.append("\\r"); break;
      case '\t': out.append("\\t"); break;
      default:
        if (c < 0x20u) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
          out.append(buffer);
        } else {
          out.push_back(raw);
        }
        break;
    }
  }
  out.push_back('"');
  return out;
}

void JsonValue::write_to(std::string& out, bool pretty, std::size_t indent) const {
  const auto newline = [&](std::size_t depth) {
    if (!pretty) return;
    out.push_back('\n');
    out.append(depth * 2, ' ');
  };
  switch (kind_) {
    case Kind::Null:
      out.append("null");
      return;
    case Kind::Bool:
      out.append(bool_ ? "true" : "false");
      return;
    case Kind::Int:
      out.append(std::to_string(int_));
      return;
    case Kind::String:
      out.append(json_escape(text_));
      return;
    case Kind::Array:
      if (elements_.empty()) {
        out.append("[]");
        return;
      }
      out.push_back('[');
      for (std::size_t i = 0; i < elements_.size(); ++i) {
        if (i != 0) out.push_back(',');
        newline(indent + 1);
        elements_[i].write_to(out, pretty, indent + 1);
      }
      newline(indent);
      out.push_back(']');
      return;
    case Kind::Object:
      if (members_.empty()) {
        out.append("{}");
        return;
      }
      out.push_back('{');
      for (std::size_t i = 0; i < members_.size(); ++i) {
        if (i != 0) out.push_back(',');
        newline(indent + 1);
        out.append(json_escape(members_[i].first));
        out.push_back(':');
        if (pretty) out.push_back(' ');
        members_[i].second.write_to(out, pretty, indent + 1);
      }
      newline(indent);
      out.push_back('}');
      return;
  }
}

std::string JsonValue::to_text(bool pretty) const {
  std::string out;
  write_to(out, pretty, 0);
  return out;
}

Result<JsonValue> parse_json(std::string_view text, std::size_t max_bytes) {
  Parser parser{text, max_bytes};
  return parser.run();
}

}  // namespace dpu::fabric
