#pragma once

// Deterministic placement eligibility.
//
// Eligibility is the only place where device facts meet service requirements.
// Its rule is absolute: a requirement that cannot be evaluated against fresh
// evidence is Unknown, and Unknown blocks any claim that depends on it. Unknown
// is never collapsed into "false" or "zero", and absence of evidence is never
// treated as evidence of absence unless the evidence itself is fresh and
// complete.

#include <cstdint>
#include <string>
#include <vector>

#include "dpu/fabric/core/archive.hpp"
#include "dpu/fabric/model/capability.hpp"
#include "dpu/fabric/model/lifecycle.hpp"
#include "dpu/fabric/model/service.hpp"
#include "dpu/fabric/model/topology.hpp"

namespace dpu::fabric {

#define DPUF_REQUIREMENT_OUTCOME_LIST(X) X(Satisfied) X(Violated) X(Unknown)
DPUF_DECLARE_ENUM(RequirementOutcome, DPUF_REQUIREMENT_OUTCOME_LIST)

/// Result of evaluating one requirement against one device.
struct RequirementResult {
  RequirementOutcome outcome{RequirementOutcome::Unknown};
  ReasonCode code{ReasonCode::Ok};
  std::string subject{};
  std::string note{};

  [[nodiscard]] bool satisfied() const noexcept {
    return outcome == RequirementOutcome::Satisfied;
  }
};

/// Full eligibility verdict for one (DPU, service) pair. The digest covers the
/// evidence that produced the verdict, so a plan can cite exactly what made a
/// placement legal.
struct Eligibility {
  bool eligible{false};
  bool unknown{false};
  ReasonCode primary{ReasonCode::Ok};
  std::vector<RequirementResult> results{};
  Digest evidence_digest{};
  TopologyGeneration topology{};
  CapabilityGeneration capability{};

  [[nodiscard]] bool decisive() const noexcept { return eligible && !unknown; }
};

/// Evaluates capability, compatibility, capacity and isolation requirements.
///
/// The evaluator holds no state: the same inputs always produce the same
/// verdict, which is what makes placement reproducible.
class EligibilityEvaluator {
 public:
  /// \p now is the logical instant freshness is measured against.
  [[nodiscard]] static Eligibility evaluate(const DpuRecord& dpu,
                                            const ServiceDefinition& service,
                                            const PolicyState& policy, LogicalInstant now);

  /// Evaluates one capability requirement. Exposed for explanation and tests.
  [[nodiscard]] static RequirementResult evaluate_capability(const CapabilitySet& capabilities,
                                                             bool capabilities_known,
                                                             const CapabilityRequirement& requirement,
                                                             const CapabilityKey& key);
};

}  // namespace dpu::fabric
