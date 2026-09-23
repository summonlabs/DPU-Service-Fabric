#pragma once

// Result and Status: the runtime never signals failure through exceptions for
// data-derived conditions. Every externally derived input is validated and a
// refusal is returned as a Status carrying a stable ReasonCode.

#include <string>
#include <utility>
#include <variant>

#include "dpu/fabric/core/reason.hpp"

namespace dpu::fabric {

/// Maximum length of a human-readable status detail. Details are diagnostic
/// only; they never carry authority. The bound keeps logs and explanations
/// bounded when derived from external text.
inline constexpr std::size_t kMaxStatusDetail = 256;

class Status {
 public:
  constexpr Status() noexcept = default;
  Status(ReasonCode code, std::string detail = {});

  [[nodiscard]] static Status success() noexcept { return Status{}; }

  /// True when the code is informational (accepted, or a pure observation).
  [[nodiscard]] bool ok() const noexcept { return is_informational(code_); }
  /// True only for strictly nominal success.
  [[nodiscard]] bool nominal() const noexcept { return code_ == ReasonCode::Ok; }
  [[nodiscard]] ReasonCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& detail() const noexcept { return detail_; }
  /// "Code" or "Code: detail".
  [[nodiscard]] std::string message() const;
  [[nodiscard]] Status with_detail(std::string detail) const;

 private:
  ReasonCode code_{ReasonCode::Ok};
  std::string detail_{};
};

/// A value or a refusal. Constructing from a Status produces a failed result.
template <class T>
class Result {
 public:
  Result(T value) : storage_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
  Result(Status status) : storage_(std::move(status)) {}   // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool has_value() const noexcept { return std::holds_alternative<T>(storage_); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] T& value() & { return std::get<T>(storage_); }
  [[nodiscard]] const T& value() const& { return std::get<T>(storage_); }
  [[nodiscard]] T&& value() && { return std::get<T>(std::move(storage_)); }

  // The member function name `status` would otherwise shadow the Status type inside
  // the class scope, so the return type is fully qualified here.
  [[nodiscard]] const dpu::fabric::Status& status() const noexcept {
    return std::get<dpu::fabric::Status>(storage_);
  }
  [[nodiscard]] ReasonCode code() const noexcept { return status().code(); }

  [[nodiscard]] T value_or(T fallback) const {
    return has_value() ? std::get<T>(storage_) : std::move(fallback);
  }

 private:
  std::variant<T, Status> storage_;
};

/// Convenience: a Status that carries a reason code.
[[nodiscard]] inline Status refuse(ReasonCode code, std::string detail = {}) {
  return Status{code, std::move(detail)};
}

}  // namespace dpu::fabric
