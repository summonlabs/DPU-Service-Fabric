#pragma once

// Dependency graph: cycles, unsatisfied edges and a deterministic total order.

#include <cstddef>
#include <string>
#include <vector>

#include "dpu/fabric/core/result.hpp"
#include "dpu/fabric/core/types.hpp"
#include "dpu/fabric/model/service.hpp"

namespace dpu::fabric {

/// The declared dependency graph, held in canonical order.
///
/// Edges are sorted by (from, to, kind, id) so that every traversal, cycle
/// report and topological order is reproducible from the edge set alone.
class DependencyGraph {
 public:
  [[nodiscard]] Status build(std::vector<Dependency> edges, std::size_t max_edges);

  [[nodiscard]] const std::vector<Dependency>& edges() const noexcept { return edges_; }
  [[nodiscard]] std::size_t edge_count() const noexcept { return edges_.size(); }

  /// Outgoing edges of \p from, in canonical order.
  [[nodiscard]] std::vector<Dependency> outgoing(const ServiceId& from) const;

  /// Dependency targets of \p from that must be present before it can start.
  [[nodiscard]] std::vector<ServiceId> requirements(const ServiceId& from) const;

  /// Deterministic topological order over \p services. Refuses with
  /// DependencyCycle when the induced subgraph has a cycle, and reports the
  /// canonical cycle path.
  [[nodiscard]] Result<std::vector<ServiceId>> topological_order(
      const std::vector<ServiceId>& services, std::vector<ServiceId>& cycle_path) const;

  /// True when \p id has an edge into or out of the given set.
  [[nodiscard]] bool has_edge(const ServiceId& id) const;

 private:
  std::vector<Dependency> edges_{};
};

}  // namespace dpu::fabric
