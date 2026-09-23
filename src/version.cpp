#include "dpu/fabric/version.hpp"

#include <string>

namespace dpu::fabric {

std::string_view version_string() noexcept { return "1.0.0"; }

std::string version_banner() {
  std::string out;
  out.reserve(96);
  out.append(kProductName);
  out.append(" ");
  out.append(version_string());
  out.append(" (format ");
  out.append(std::to_string(kFormatVersion));
  out.append(", semantics ");
  out.append(std::to_string(kSemanticVersion));
  out.append(")");
  return out;
}

}  // namespace dpu::fabric
