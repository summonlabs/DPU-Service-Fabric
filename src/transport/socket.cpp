#include "socket.hpp"

#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace dpu::fabric::net {
namespace {

#ifdef _WIN32
using raw_socket = SOCKET;
constexpr raw_socket kInvalid = INVALID_SOCKET;
raw_socket to_raw(dpu_socket_t socket) { return static_cast<raw_socket>(socket); }
dpu_socket_t from_raw(raw_socket socket) { return static_cast<dpu_socket_t>(socket); }
#else
using raw_socket = int;
constexpr raw_socket kInvalid = -1;
raw_socket to_raw(dpu_socket_t socket) { return socket; }
dpu_socket_t from_raw(raw_socket socket) { return socket; }
#endif

}  // namespace

Status startup() {
#ifdef _WIN32
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    return refuse(ReasonCode::TransportFailure, "winsock initialisation failed");
  }
#endif
  return Status::success();
}

void cleanup() {
#ifdef _WIN32
  WSACleanup();
#endif
}

std::string last_error() {
#ifdef _WIN32
  const int code = WSAGetLastError();
  return "winsock error " + std::to_string(code);
#else
  return std::string{std::strerror(errno)};
#endif
}

Result<dpu_socket_t> listen_on(const std::string& address, std::uint16_t port) {
  const raw_socket listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == kInvalid) {
    return refuse(ReasonCode::TransportFailure, "socket() failed: " + last_error());
  }
  int reuse = 1;
  (void)::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                     sizeof(reuse));
  sockaddr_in endpoint{};
  endpoint.sin_family = AF_INET;
  endpoint.sin_port = htons(port);
  if (::inet_pton(AF_INET, address.c_str(), &endpoint.sin_addr) != 1) {
    (void)close_socket(from_raw(listener));
    return refuse(ReasonCode::ValueOutOfRange, "invalid bind address");
  }
  if (::bind(listener, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
    (void)close_socket(from_raw(listener));
    return refuse(ReasonCode::TransportFailure, "bind() failed: " + last_error());
  }
  if (::listen(listener, 16) != 0) {
    (void)close_socket(from_raw(listener));
    return refuse(ReasonCode::TransportFailure, "listen() failed: " + last_error());
  }
  return from_raw(listener);
}

Result<std::uint16_t> local_port(dpu_socket_t socket) {
  sockaddr_in endpoint{};
#ifdef _WIN32
  int length = sizeof(endpoint);
#else
  socklen_t length = sizeof(endpoint);
#endif
  if (::getsockname(to_raw(socket), reinterpret_cast<sockaddr*>(&endpoint), &length) != 0) {
    return refuse(ReasonCode::TransportFailure, "getsockname() failed");
  }
  return static_cast<std::uint16_t>(ntohs(endpoint.sin_port));
}

Result<dpu_socket_t> accept_one(dpu_socket_t listener) {
  const raw_socket client = ::accept(to_raw(listener), nullptr, nullptr);
  if (client == kInvalid) {
    return refuse(ReasonCode::TransportFailure, "accept() failed: " + last_error());
  }
  return from_raw(client);
}

Result<dpu_socket_t> connect_to(const std::string& address, std::uint16_t port) {
  const raw_socket client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (client == kInvalid) {
    return refuse(ReasonCode::TransportFailure, "socket() failed");
  }
  sockaddr_in endpoint{};
  endpoint.sin_family = AF_INET;
  endpoint.sin_port = htons(port);
  if (::inet_pton(AF_INET, address.c_str(), &endpoint.sin_addr) != 1) {
    (void)close_socket(from_raw(client));
    return refuse(ReasonCode::ValueOutOfRange, "invalid connect address");
  }
  if (::connect(client, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
    (void)close_socket(from_raw(client));
    return refuse(ReasonCode::TransportFailure, "connect() failed: " + last_error());
  }
  return from_raw(client);
}

Status close_socket(dpu_socket_t socket) {
  if (socket == kDpuInvalidSocket) return Status::success();
#ifdef _WIN32
  if (::closesocket(to_raw(socket)) != 0) {
    return refuse(ReasonCode::TransportFailure, "closesocket() failed");
  }
#else
  if (::close(to_raw(socket)) != 0) {
    return refuse(ReasonCode::TransportFailure, "close() failed");
  }
#endif
  return Status::success();
}

Status set_inheritable(dpu_socket_t socket) {
#ifdef _WIN32
  if (::SetHandleInformation(reinterpret_cast<HANDLE>(to_raw(socket)), HANDLE_FLAG_INHERIT,
                             HANDLE_FLAG_INHERIT) == 0) {
    return refuse(ReasonCode::TransportFailure, "SetHandleInformation() failed");
  }
  return Status::success();
#else
  (void)socket;
  return Status::success();
#endif
}

Status send_all(dpu_socket_t socket, std::span<const std::uint8_t> data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
#ifdef _WIN32
    const int chunk = ::send(to_raw(socket), reinterpret_cast<const char*>(data.data() + sent),
                             static_cast<int>(data.size() - sent), 0);
#else
    const int chunk = static_cast<int>(
        ::send(to_raw(socket), data.data() + sent, data.size() - sent, MSG_NOSIGNAL));
#endif
    if (chunk <= 0) {
      return refuse(ReasonCode::ConnectionClosed, "peer closed during send: " + last_error());
    }
    sent += static_cast<std::size_t>(chunk);
  }
  return Status::success();
}

Result<std::size_t> recv_some(dpu_socket_t socket, std::span<std::uint8_t> buffer) {
  if (buffer.empty()) return std::size_t{0};
#ifdef _WIN32
  const int received =
      ::recv(to_raw(socket), reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
#else
  const int received = static_cast<int>(::recv(to_raw(socket), buffer.data(), buffer.size(), 0));
#endif
  if (received == 0) return std::size_t{0};
  if (received < 0) {
    return refuse(ReasonCode::ConnectionClosed, "peer reset: " + last_error());
  }
  return static_cast<std::size_t>(received);
}

Status shutdown_both(dpu_socket_t socket) {
#ifdef _WIN32
  if (::shutdown(to_raw(socket), SD_BOTH) != 0) {
    return refuse(ReasonCode::TransportFailure, "shutdown() failed");
  }
#else
  if (::shutdown(to_raw(socket), SHUT_RDWR) != 0) {
    return refuse(ReasonCode::TransportFailure, "shutdown() failed");
  }
#endif
  return Status::success();
}

Status shutdown_send(dpu_socket_t socket) {
#ifdef _WIN32
  if (::shutdown(to_raw(socket), SD_SEND) != 0) {
    return refuse(ReasonCode::TransportFailure, "shutdown() failed");
  }
#else
  if (::shutdown(to_raw(socket), SHUT_WR) != 0) {
    return refuse(ReasonCode::TransportFailure, "shutdown() failed");
  }
#endif
  return Status::success();
}

}  // namespace dpu::fabric::net
