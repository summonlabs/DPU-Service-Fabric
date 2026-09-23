#pragma once

// Thin blocking-socket shim. Private to the transport implementation: the public
// headers never expose a socket type.

#include <cstdint>
#include <span>
#include <string>

#include "dpu/fabric/core/result.hpp"

namespace dpu::fabric::net {

#ifdef _WIN32
using dpu_socket_t = std::uintptr_t;
inline constexpr dpu_socket_t kDpuInvalidSocket = ~static_cast<std::uintptr_t>(0);
#else
using dpu_socket_t = int;
inline constexpr dpu_socket_t kDpuInvalidSocket = -1;
#endif

[[nodiscard]] Status startup();
void cleanup();

[[nodiscard]] Result<dpu_socket_t> listen_on(const std::string& address, std::uint16_t port);
[[nodiscard]] Result<std::uint16_t> local_port(dpu_socket_t socket);
[[nodiscard]] Result<dpu_socket_t> accept_one(dpu_socket_t listener);
[[nodiscard]] Result<dpu_socket_t> connect_to(const std::string& address, std::uint16_t port);
[[nodiscard]] Status close_socket(dpu_socket_t socket);

/// Marks a socket inheritable so a child process can adopt it. This is the
/// socket-activation path used to prove multiprocess transport without races.
[[nodiscard]] Status set_inheritable(dpu_socket_t socket);

/// Sends the whole buffer. Returns a refusal on any short write.
[[nodiscard]] Status send_all(dpu_socket_t socket, std::span<const std::uint8_t> data);

/// Receives at least one byte. Returns the byte count, 0 for an orderly close,
/// or a refusal with the code ConnectionClosed for a peer reset.
[[nodiscard]] Result<std::size_t> recv_some(dpu_socket_t socket, std::span<std::uint8_t> buffer);

/// Asks the peer to stop sending.
[[nodiscard]] Status shutdown_send(dpu_socket_t socket);

/// Shuts the connection down in both directions. A blocked receive on this
/// socket returns immediately afterwards, which is how shutdown wakes a worker
/// without waiting on a timeout.
[[nodiscard]] Status shutdown_both(dpu_socket_t socket);

[[nodiscard]] std::string last_error();

}  // namespace dpu::fabric::net
