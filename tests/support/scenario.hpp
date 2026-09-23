#pragma once

// Scenario helpers: a synthetic world plus a runtime driver.

#include <memory>
#include <string>
#include <vector>

#include "dpu/fabric/dpu_fabric.hpp"
#include "fixtures.hpp"

namespace dpu::fabric::test {

/// Drives a runtime with automatically identified events.
class Scenario {
 public:
  explicit Scenario(RuntimeConfig config = make_config());

  [[nodiscard]] FabricRuntime& rt() { return *runtime_; }
  [[nodiscard]] const FabricRuntime& rt() const { return *runtime_; }
  [[nodiscard]] const RuntimeBounds& bounds() const { return config_.bounds; }
  [[nodiscard]] std::uint64_t now() const { return now_; }
  void set_now(std::uint64_t value) { now_ = value; }
  /// Highest logical instant that has already been released. The runtime
  /// refuses events at or below it, so new work must use a later instant.
  [[nodiscard]] std::uint64_t released() const { return released_; }

  /// Advances logical time by emitting a LogicalTimeAdvanced event.
  Result<BatchReport> tick(std::uint64_t delta = 1);

  [[nodiscard]] Result<BatchReport> send(FabricEventBody body);
  [[nodiscard]] Result<BatchReport> send_at(FabricEventBody body, std::uint64_t at);
  [[nodiscard]] Result<BatchReport> send_all(std::vector<FabricEvent> events);

  /// Builds an event without submitting it.
  [[nodiscard]] FabricEvent make(FabricEventBody body, std::uint64_t at);

 private:
  RuntimeConfig config_{};
  std::unique_ptr<FabricRuntime> runtime_{};
  std::uint64_t now_{1};
  std::uint64_t released_{0};
  std::uint64_t sequence_{0};
};

/// The standard synthetic topology: three aarch64 devices in two domains, all
/// with fresh capability and health evidence.
[[nodiscard]] std::vector<DpuRecord> standard_dpus(std::uint64_t observed, std::uint64_t validity,
                                                   CapabilityGeneration capability,
                                                   TopologyGeneration topology);

/// Declares the standard service/group/dependency set and the standard topology.
/// Returns the events in submission order.
[[nodiscard]] std::vector<FabricEvent> standard_world_events(std::uint64_t observed,
                                                             std::uint64_t validity);

/// Applies the standard world to a scenario and returns the batch report.
[[nodiscard]] Result<BatchReport> apply_standard_world(Scenario& scenario,
                                                       std::uint64_t validity = 100);

/// The intent used by most lifecycle tests: two replicas of "svc-a" in group
/// "grp-1" at generation 1.
[[nodiscard]] PlacementIntent standard_intent(std::uint64_t at, std::uint32_t replicas = 2);

}  // namespace dpu::fabric::test
