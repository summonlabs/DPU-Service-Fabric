#include "dpu/fabric/engine/dependency.hpp"

#include <algorithm>
#include <optional>
#include <set>

namespace dpu::fabric {

Status DependencyGraph::build(std::vector<Dependency> edges, std::size_t max_edges) {
  if (edges.size() > max_edges) {
    return refuse(ReasonCode::BoundExceeded, "dependency edge count");
  }
  for (const Dependency& edge : edges) {
    if (!edge.id.valid() || !edge.from.valid() || !edge.to.valid()) {
      return refuse(ReasonCode::InvalidIdentity, "dependency edge identity");
    }
    if (edge.from == edge.to) {
      return refuse(ReasonCode::SelfDependency, edge.from.str());
    }
    if (static_cast<std::uint8_t>(edge.kind) >= kDependencyKindCount) {
      return refuse(ReasonCode::DependencyKindUnsupported, edge.from.str());
    }
  }
  std::sort(edges.begin(), edges.end(), [](const Dependency& lhs, const Dependency& rhs) {
    if (lhs.from != rhs.from) return lhs.from < rhs.from;
    if (lhs.to != rhs.to) return lhs.to < rhs.to;
    if (lhs.kind != rhs.kind) return lhs.kind < rhs.kind;
    return lhs.id < rhs.id;
  });
  for (std::size_t i = 1; i < edges.size(); ++i) {
    const Dependency& previous = edges[i - 1];
    const Dependency& current = edges[i];
    if (previous.from == current.from && previous.to == current.to &&
        previous.kind == current.kind) {
      return refuse(ReasonCode::DependencyDuplicate, previous.id.str());
    }
  }
  edges_ = std::move(edges);
  return Status::success();
}

std::vector<Dependency> DependencyGraph::outgoing(const ServiceId& from) const {
  std::vector<Dependency> out;
  const auto first = std::lower_bound(
      edges_.begin(), edges_.end(), from,
      [](const Dependency& edge, const ServiceId& probe) { return edge.from < probe; });
  for (auto it = first; it != edges_.end() && it->from == from; ++it) {
    out.push_back(*it);
  }
  return out;
}

std::vector<ServiceId> DependencyGraph::requirements(const ServiceId& from) const {
  std::vector<ServiceId> out;
  for (const Dependency& edge : outgoing(from)) {
    if (edge.kind == DependencyKind::Requires || edge.kind == DependencyKind::RequiresCapability) {
      if (std::find(out.begin(), out.end(), edge.to) == out.end()) out.push_back(edge.to);
    }
  }
  return out;
}

bool DependencyGraph::has_edge(const ServiceId& id) const {
  for (const Dependency& edge : edges_) {
    if (edge.from == id || edge.to == id) return true;
  }
  return false;
}

Result<std::vector<ServiceId>> DependencyGraph::topological_order(
    const std::vector<ServiceId>& services, std::vector<ServiceId>& cycle_path) const {
  cycle_path.clear();
  std::vector<ServiceId> nodes = services;
  std::sort(nodes.begin(), nodes.end());
  nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());

  // Deterministic Kahn: ready nodes are kept in a sorted set so the emitted
  // order depends only on the node and edge sets.
  std::vector<std::pair<ServiceId, ServiceId>> edges;
  std::vector<std::size_t> indegree(nodes.size(), 0);
  const auto index_of = [&nodes](const ServiceId& id) -> std::optional<std::size_t> {
    const auto it = std::lower_bound(nodes.begin(), nodes.end(), id);
    if (it == nodes.end() || !(*it == id)) return std::nullopt;
    return static_cast<std::size_t>(std::distance(nodes.begin(), it));
  };
  // A Requires edge means "from needs to", so the ordering constraint is that
  // `to` is emitted before `from`: the predecessor of `from` is `to`.
  for (const Dependency& edge : edges_) {
    if (edge.kind == DependencyKind::RequiresCapability) continue;
    const auto from = index_of(edge.from);
    const auto to = index_of(edge.to);
    if (!from || !to) continue;
    edges.emplace_back(edge.from, edge.to);
    indegree[*from] += 1;
  }
  std::sort(edges.begin(), edges.end());

  std::set<ServiceId> ready;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    if (indegree[i] == 0) ready.insert(nodes[i]);
  }

  std::vector<ServiceId> order;
  order.reserve(nodes.size());
  while (!ready.empty()) {
    const ServiceId current = *ready.begin();
    ready.erase(ready.begin());
    order.push_back(current);
    const auto current_index = index_of(current);
    if (!current_index) continue;
    for (const auto& edge : edges) {
      // Emitting a dependency releases the services that require it.
      if (!(edge.second == current)) continue;
      const auto dependent = index_of(edge.first);
      if (!dependent) continue;
      if (indegree[*dependent] == 0) continue;
      indegree[*dependent] -= 1;
      if (indegree[*dependent] == 0) ready.insert(edge.first);
    }
  }

  if (order.size() != nodes.size()) {
    // Report the canonical cycle: start at the smallest unresolved node and
    // follow the smallest outgoing edge that stays inside the residual graph.
    std::set<ServiceId> unresolved;
    for (const ServiceId& node : nodes) {
      if (std::find(order.begin(), order.end(), node) == order.end()) unresolved.insert(node);
    }
    ServiceId cursor = *unresolved.begin();
    cycle_path.push_back(cursor);
    for (std::size_t guard = 0; guard <= unresolved.size(); ++guard) {
      ServiceId next;
      bool found = false;
      for (const auto& edge : edges) {
        if (!(edge.first == cursor)) continue;
        if (unresolved.count(edge.second) == 0) continue;
        next = edge.second;
        found = true;
        break;
      }
      if (!found) break;
      if (next == cycle_path.front()) {
        cycle_path.push_back(next);
        break;
      }
      if (std::find(cycle_path.begin(), cycle_path.end(), next) != cycle_path.end()) {
        cycle_path.push_back(next);
        break;
      }
      cycle_path.push_back(next);
      cursor = next;
    }
    return refuse(ReasonCode::DependencyCycle, "dependency cycle detected");
  }
  return order;
}

}  // namespace dpu::fabric
