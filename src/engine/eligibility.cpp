#include "dpu/fabric/engine/eligibility.hpp"

#include <algorithm>

namespace dpu::fabric {
namespace {

RequirementResult result(RequirementOutcome outcome, ReasonCode code, std::string subject,
                         std::string note) {
  RequirementResult out;
  out.outcome = outcome;
  out.code = code;
  out.subject = std::move(subject);
  out.note = std::move(note);
  return out;
}

/// Ordering between two capability values of the same kind. Returns false when
/// the kinds cannot be ordered, which is a requirement defect rather than a
/// device defect.
bool ordered_compare(const CapabilityValue& lhs, const CapabilityValue& rhs, int& out) {
  if (lhs.kind != rhs.kind) return false;
  switch (lhs.kind) {
    case CapabilityValueKind::Integer:
      out = lhs.integer < rhs.integer ? -1 : (lhs.integer > rhs.integer ? 1 : 0);
      return true;
    case CapabilityValueKind::Version: {
      const std::strong_ordering cmp = lhs.version <=> rhs.version;
      out = cmp < 0 ? -1 : (cmp > 0 ? 1 : 0);
      return true;
    }
    case CapabilityValueKind::Flag:
      out = lhs.flag == rhs.flag ? 0 : (lhs.flag ? 1 : -1);
      return true;
    case CapabilityValueKind::Text:
      out = lhs.text < rhs.text ? -1 : (lhs.text > rhs.text ? 1 : 0);
      return true;
    case CapabilityValueKind::Unknown:
      return false;
  }
  return false;
}

}  // namespace

RequirementResult EligibilityEvaluator::evaluate_capability(
    const CapabilitySet& capabilities, bool capabilities_known,
    const CapabilityRequirement& requirement, const CapabilityKey& key) {
  const std::string subject = key.str();
  const Capability* observed = capabilities.find(key);

  if (!capabilities_known) {
    // No fresh capability evidence: the state of this key is unknown. It is not
    // "absent" and it is not "false".
    return result(RequirementOutcome::Unknown, ReasonCode::CapabilityUnknown, subject,
                  "capability state unknown: no fresh capability evidence");
  }

  if (requirement.op == CapabilityOp::Absent) {
    if (observed == nullptr) {
      return result(RequirementOutcome::Satisfied, ReasonCode::Ok, subject, "known absent");
    }
    return result(RequirementOutcome::Violated, ReasonCode::CapabilityMismatch, subject,
                  "capability present but must be absent");
  }

  if (observed == nullptr) {
    return result(RequirementOutcome::Violated, ReasonCode::CapabilityMissing, subject,
                  "capability absent from fresh evidence");
  }

  switch (requirement.op) {
    case CapabilityOp::Present:
      if (observed->value.kind == CapabilityValueKind::Flag && !observed->value.flag) {
        return result(RequirementOutcome::Violated, ReasonCode::CapabilityMismatch, subject,
                      "capability flag is false");
      }
      return result(RequirementOutcome::Satisfied, ReasonCode::Ok, subject, "present");
    case CapabilityOp::Absent:
      break;
    case CapabilityOp::Equals:
      if (observed->value == requirement.value) {
        return result(RequirementOutcome::Satisfied, ReasonCode::Ok, subject, "equal");
      }
      return result(RequirementOutcome::Violated, ReasonCode::CapabilityMismatch, subject,
                    "value differs: observed " + observed->value.to_string() + ", required " +
                        requirement.value.to_string());
    case CapabilityOp::AtLeast:
    case CapabilityOp::AtMost: {
      int cmp = 0;
      if (!ordered_compare(observed->value, requirement.value, cmp)) {
        return result(RequirementOutcome::Violated, ReasonCode::UnsupportedValue, subject,
                      "capability value kinds are not orderable");
      }
      const bool ok = requirement.op == CapabilityOp::AtLeast ? cmp >= 0 : cmp <= 0;
      if (ok) return result(RequirementOutcome::Satisfied, ReasonCode::Ok, subject, "ordered");
      return result(RequirementOutcome::Violated, ReasonCode::CapabilityMismatch, subject,
                    "ordering requirement not met");
    }
    case CapabilityOp::OneOf: {
      for (const CapabilityValue& alternative : requirement.alternatives) {
        if (observed->value == alternative) {
          return result(RequirementOutcome::Satisfied, ReasonCode::Ok, subject, "alternative");
        }
      }
      return result(RequirementOutcome::Violated, ReasonCode::CapabilityMismatch, subject,
                    "no alternative matched");
    }
  }
  return result(RequirementOutcome::Violated, ReasonCode::UnsupportedValue, subject,
                "unsupported capability operator");
}

Eligibility EligibilityEvaluator::evaluate(const DpuRecord& dpu, const ServiceDefinition& service,
                                           const PolicyState& policy, LogicalInstant now) {
  Eligibility verdict;
  verdict.topology = dpu.topology_generation;
  verdict.capability = dpu.capability_generation;
  verdict.evidence_digest = dpu.device_evidence.digest;

  const auto push = [&verdict](RequirementResult entry) {
    verdict.results.push_back(std::move(entry));
  };

  if (!dpu.id.valid()) {
    push(result(RequirementOutcome::Violated, ReasonCode::InvalidIdentity, "<dpu>",
                "device record has no identity"));
  }

  if (dpu.state != DpuState::Attached) {
    push(result(RequirementOutcome::Violated, ReasonCode::DpuUnavailable, dpu.id.str(),
                std::string{"device state is "} + std::string{to_string(dpu.state)}));
  }

  // Fresh capability and device evidence is the precondition for every other
  // statement about the device.
  bool capabilities_known = false;
  if (!dpu.device_evidence.present()) {
    push(result(RequirementOutcome::Unknown, ReasonCode::EvidenceMissing, dpu.id.str(),
                "no device evidence reference"));
  } else if (!dpu.device_evidence.fresh_at(now)) {
    push(result(RequirementOutcome::Unknown, ReasonCode::EvidenceStale, dpu.id.str(),
                "device evidence is stale at the decision instant"));
  } else if (dpu.device_evidence.provenance.evidence_class == EvidenceClass::Unsupported) {
    push(result(RequirementOutcome::Unknown, ReasonCode::CapabilityUnknown, dpu.id.str(),
                "device evidence is marked unsupported"));
  } else if (dpu.device_evidence.provenance.evidence_class == EvidenceClass::Synthetic &&
             !policy.allow_synthetic_evidence) {
    push(result(RequirementOutcome::Violated, ReasonCode::CapabilityUnknown, dpu.id.str(),
                "policy rejects synthetic device evidence"));
  } else if (!dpu.capability_generation.valid()) {
    push(result(RequirementOutcome::Unknown, ReasonCode::CapabilityUnknown, dpu.id.str(),
                "no capability generation"));
  } else {
    capabilities_known = true;
  }

  const CompatibilityRequirement& requirement = service.compatibility;

  if (requirement.architecture.has_value()) {
    if (dpu.profile.architecture == DpuArchitecture::Unknown) {
      push(result(RequirementOutcome::Unknown, ReasonCode::CapabilityUnknown, dpu.id.str(),
                  "architecture unknown"));
    } else if (dpu.profile.architecture != *requirement.architecture) {
      push(result(RequirementOutcome::Violated, ReasonCode::ArchitectureMismatch, dpu.id.str(),
                  std::string{"architecture is "} +
                      std::string{to_string(dpu.profile.architecture)}));
    } else {
      push(result(RequirementOutcome::Satisfied, ReasonCode::Ok, dpu.id.str(), "architecture"));
    }
  }

  if (requirement.min_firmware.has_value()) {
    if (!capabilities_known) {
      push(result(RequirementOutcome::Unknown, ReasonCode::CapabilityUnknown, dpu.id.str(),
                  "firmware level unknown"));
    } else if (dpu.profile.firmware < *requirement.min_firmware) {
      push(result(RequirementOutcome::Violated, ReasonCode::FirmwareLevelUnsupported, dpu.id.str(),
                  "firmware " + dpu.profile.firmware.to_string() + " below required " +
                      requirement.min_firmware->to_string()));
    } else {
      push(result(RequirementOutcome::Satisfied, ReasonCode::Ok, dpu.id.str(), "firmware"));
    }
  }

  if (requirement.datapath.has_value() && *requirement.datapath != DatapathClass::Unspecified) {
    if (!capabilities_known) {
      push(result(RequirementOutcome::Unknown, ReasonCode::CapabilityUnknown, dpu.id.str(),
                  "datapath class unknown"));
    } else if (dpu.profile.datapath != *requirement.datapath) {
      push(result(RequirementOutcome::Violated, ReasonCode::DatapathUnsupported, dpu.id.str(),
                  std::string{"datapath is "} + std::string{to_string(dpu.profile.datapath)}));
    } else {
      push(result(RequirementOutcome::Satisfied, ReasonCode::Ok, dpu.id.str(), "datapath"));
    }
  }

  for (const IsolationKind kind : requirement.required_isolation) {
    if (!capabilities_known) {
      push(result(RequirementOutcome::Unknown, ReasonCode::CapabilityUnknown, dpu.id.str(),
                  std::string{"isolation "} + std::string{to_string(kind)} + " unknown"));
    } else if (!dpu.profile.supports(kind)) {
      push(result(RequirementOutcome::Violated, ReasonCode::IsolationUnsupported, dpu.id.str(),
                  std::string{"isolation "} + std::string{to_string(kind)} + " unsupported"));
    } else {
      push(result(RequirementOutcome::Satisfied, ReasonCode::Ok, dpu.id.str(), "isolation"));
    }
  }

  if (service.isolation.kind == IsolationKind::DedicatedDevice &&
      policy.require_exclusive_scope_for_dedicated_device && !service.isolation.exclusive_scope) {
    push(result(RequirementOutcome::Violated, ReasonCode::ExclusiveScopeRequired, service.id.str(),
                "dedicated device isolation requires an exclusive scope"));
  }

  if (service.isolation.kind != IsolationKind::None || service.isolation.exclusive_scope.has_value()) {
    if (!capabilities_known) {
      push(result(RequirementOutcome::Unknown, ReasonCode::CapabilityUnknown, dpu.id.str(),
                  "isolation capability unknown"));
    } else if (service.isolation.kind != IsolationKind::None &&
               !dpu.profile.supports(service.isolation.kind)) {
      push(result(RequirementOutcome::Violated, ReasonCode::IsolationUnsupported, dpu.id.str(),
                  "service isolation kind unsupported"));
    } else {
      push(result(RequirementOutcome::Satisfied, ReasonCode::Ok, dpu.id.str(), "isolation kind"));
    }
  }

  // Capacity: an all-zero capacity with no fresh evidence is unknown, never
  // "zero capacity available".
  const bool capacity_known = capabilities_known && !dpu.capacity.is_zero();
  if (!capacity_known) {
    push(result(RequirementOutcome::Unknown, ReasonCode::CapacityUnknown, dpu.id.str(),
                "device capacity unknown"));
  } else {
    const Result<ResourceVector> free = dpu.capacity.headroom(dpu.allocated);
    if (!free) {
      push(result(RequirementOutcome::Violated, ReasonCode::AllocationExhausted, dpu.id.str(),
                  "accounted allocation exceeds capacity"));
    } else if (!free.value().covers(service.resources)) {
      push(result(RequirementOutcome::Violated, ReasonCode::AllocationExhausted, dpu.id.str(),
                  "insufficient free capacity"));
    } else {
      push(result(RequirementOutcome::Satisfied, ReasonCode::Ok, dpu.id.str(), "capacity"));
    }
  }

  for (const CapabilityRequirement& capability : requirement.capabilities) {
    push(evaluate_capability(dpu.capabilities, capabilities_known, capability, capability.key));
  }

  // Aggregate: any violation refuses; otherwise any unknown blocks.
  bool violated = false;
  bool unknown = false;
  ReasonCode primary = ReasonCode::Ok;
  for (const RequirementResult& entry : verdict.results) {
    if (entry.outcome == RequirementOutcome::Violated) {
      if (!violated) primary = entry.code;
      violated = true;
    } else if (entry.outcome == RequirementOutcome::Unknown) {
      unknown = true;
      if (!violated && primary == ReasonCode::Ok) primary = entry.code;
    }
  }
  // Eligibility is a positive determination: a device is eligible only when
  // every requirement was affirmatively satisfied by fresh evidence. Unknown
  // state is reported through both flags so a caller that inspects only
  // `eligible` still refuses the device.
  verdict.unknown = unknown;
  verdict.eligible = !violated && !unknown;
  verdict.primary = primary;
  return verdict;
}

}  // namespace dpu::fabric
