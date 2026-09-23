#include "dpu/fabric/core/result.hpp"

namespace dpu::fabric {

Status::Status(ReasonCode code, std::string detail) : code_(code), detail_(std::move(detail)) {
  if (detail_.size() > kMaxStatusDetail) {
    detail_.resize(kMaxStatusDetail);
  }
}

std::string Status::message() const {
  std::string out{to_string(code_)};
  if (!detail_.empty()) {
    out.append(": ");
    out.append(detail_);
  }
  return out;
}

Status Status::with_detail(std::string detail) const { return Status{code_, std::move(detail)}; }

}  // namespace dpu::fabric
