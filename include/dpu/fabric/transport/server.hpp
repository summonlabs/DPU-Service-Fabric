#pragma once

// Bounded framed-protocol server.
//
// The server owns a fixed pool of worker threads and a bounded accepted-socket
// queue. A connection is served to completion of its requests; the accept loop
// and the workers are joined before the server reports itself stopped, so
// shutdown never races with work in flight. A client that disconnects is
// cancelled: no reply is fabricated, and no success is claimed for work whose
// acknowledgement could not be delivered.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dpu/fabric/core/result.hpp"
#include "dpu/fabric/engine/runtime.hpp"
#include "dpu/fabric/transport/frame.hpp"

namespace dpu::fabric {

struct ServerOptions {
  std::string bind_address{"127.0.0.1"};
  std::uint16_t port{0};
  std::size_t workers{2};
  std::size_t max_pending_connections{16};
  std::size_t max_frame_bytes{1024u * 1024u};
  bool export_allowed{true};
};

struct ServerStats {
  std::uint64_t connections{0};
  std::uint64_t requests{0};
  std::uint64_t refusals{0};
  std::uint64_t frames_rejected{0};
  std::uint64_t connections_cancelled{0};
  std::uint64_t queue_rejections{0};
  bool shutdown_requested{false};
};

class Server {
 public:
  [[nodiscard]] static Result<std::unique_ptr<Server>> create(FabricRuntime& runtime,
                                                              const ServerOptions& options);

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;
  ~Server();

  /// Sets the bind endpoint before listen(). Port 0 selects an ephemeral port.
  void set_endpoint(std::string address, std::uint16_t port);
  /// Binds and listens, returning the port actually bound (useful with port 0).
  [[nodiscard]] Status listen();
  /// Adopts an already-listening socket handle inherited from a parent process.
  /// This is how the multiprocess transport test proves real socket behaviour
  /// without racing on a port or sleeping.
  [[nodiscard]] Status adopt(std::uintptr_t socket_handle);
  [[nodiscard]] std::uint16_t port() const noexcept;

  /// Serves until a shutdown is requested. Blocks; returns when every worker
  /// has been joined.
  [[nodiscard]] Status serve();
  void request_shutdown();
  [[nodiscard]] ServerStats stats() const;

 private:
  struct Impl;
  explicit Server(FabricRuntime& runtime, const ServerOptions& options);
  std::unique_ptr<Impl> impl_{};
};

}  // namespace dpu::fabric
