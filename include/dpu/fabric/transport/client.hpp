#pragma once

// Framed-protocol client.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dpu/fabric/core/result.hpp"
#include "dpu/fabric/engine/runtime.hpp"
#include "dpu/fabric/transport/frame.hpp"

namespace dpu::fabric {

struct ClientOptions {
  std::string address{"127.0.0.1"};
  std::uint16_t port{0};
  std::size_t max_frame_bytes{1024u * 1024u};
};

class Client {
 public:
  [[nodiscard]] static Result<std::unique_ptr<Client>> connect(const ClientOptions& options);

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  ~Client();

  [[nodiscard]] Result<PingResponse> ping(std::string nonce);
  [[nodiscard]] Result<SubmitResponse> submit(const std::vector<FabricEvent>& events);
  [[nodiscard]] Result<QueryResponse> query();
  [[nodiscard]] Result<ExplainResponse> explain(const InstanceId* instance);
  [[nodiscard]] Result<ExportResponse> export_state(bool pretty);
  [[nodiscard]] Result<ShutdownResponse> shutdown();

  /// Closes the connection. Closing is always safe and never reports success for
  /// work that was not answered.
  [[nodiscard]] Status close();
  [[nodiscard]] bool connected() const noexcept;

 private:
  struct Impl;
  explicit Client(ClientOptions options);
  std::unique_ptr<Impl> impl_{};
};

}  // namespace dpu::fabric
