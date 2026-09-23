#include "process.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace dpu::fabric::test {
namespace {

namespace fs = std::filesystem;

#ifdef _WIN32
using raw_socket = SOCKET;
constexpr raw_socket kInvalidRaw = INVALID_SOCKET;
#else
using raw_socket = int;
constexpr raw_socket kInvalidRaw = -1;
#endif

void ensure_winsock() {
#ifdef _WIN32
  static bool started = false;
  if (!started) {
    WSADATA data;
    (void)WSAStartup(MAKEWORD(2, 2), &data);
    started = true;
  }
#endif
}

}  // namespace

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : handle_(other.handle_), pid_(other.pid_), joined_(other.joined_), exit_code_(other.exit_code_) {
  other.handle_ = nullptr;
  other.pid_ = 0;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this == &other) return *this;
  (void)join();
  handle_ = other.handle_;
  pid_ = other.pid_;
  joined_ = other.joined_;
  exit_code_ = other.exit_code_;
  other.handle_ = nullptr;
  other.pid_ = 0;
  return *this;
}

ChildProcess::~ChildProcess() { (void)join(); }

Result<int> ChildProcess::join() {
  if (handle_ == nullptr) {
    if (joined_) return exit_code_;
    return refuse(ReasonCode::InvalidStateTransition, "no child process");
  }
#ifdef _WIN32
  const HANDLE handle = static_cast<HANDLE>(handle_);
  // INFINITE: joining is a synchronisation point, not a timeout.
  const DWORD waited = WaitForSingleObject(handle, INFINITE);
  if (waited != WAIT_OBJECT_0) {
    return refuse(ReasonCode::TransportFailure, "wait for child failed");
  }
  DWORD code = 0;
  if (GetExitCodeProcess(handle, &code) == 0) {
    return refuse(ReasonCode::TransportFailure, "child exit code unavailable");
  }
  CloseHandle(handle);
  handle_ = nullptr;
  joined_ = true;
  exit_code_ = static_cast<int>(code);
  return exit_code_;
#else
  const pid_t pid = static_cast<pid_t>(pid_);
  int status = 0;
  if (waitpid(pid, &status, 0) != pid) {
    return refuse(ReasonCode::TransportFailure, "waitpid failed");
  }
  handle_ = nullptr;
  joined_ = true;
  exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
  return exit_code_;
#endif
}

Result<ChildProcess> spawn_process(const std::string& executable,
                                   const std::vector<std::string>& arguments,
                                   std::uintptr_t inherited_handle) {
  std::string command = "\"" + executable + "\"";
  for (const std::string& argument : arguments) {
    command.append(" \"");
    command.append(argument);
    command.append("\"");
  }
  ChildProcess child;
#ifdef _WIN32
  if (inherited_handle != 0) {
    if (SetHandleInformation(reinterpret_cast<HANDLE>(inherited_handle), HANDLE_FLAG_INHERIT,
                             HANDLE_FLAG_INHERIT) == 0) {
      return refuse(ReasonCode::TransportFailure, "cannot mark the listener inheritable");
    }
  }
  std::vector<char> mutable_command{command.begin(), command.end()};
  mutable_command.push_back('\0');
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info{};
  // The application name is given explicitly: the executable path contains
  // spaces, and relying on command-line parsing alone is fragile.
  if (CreateProcessA(executable.c_str(), mutable_command.data(), nullptr, nullptr, TRUE, 0, nullptr,
                     nullptr, &startup, &info) == 0) {
    return refuse(ReasonCode::TransportFailure,
                  "CreateProcess failed with error " +
                      std::to_string(static_cast<unsigned long>(GetLastError())));
  }
  CloseHandle(info.hThread);
  child.handle_ = info.hProcess;
  child.pid_ = info.dwProcessId;
  return child;
#else
  const pid_t pid = fork();
  if (pid < 0) return refuse(ReasonCode::TransportFailure, "fork failed");
  if (pid == 0) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const std::string& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    execv(executable.c_str(), argv.data());
    _exit(127);
  }
  child.pid_ = static_cast<std::uint32_t>(pid);
  child.handle_ = reinterpret_cast<void*>(static_cast<std::uintptr_t>(pid));
  return child;
#endif
}

Result<std::uintptr_t> create_inheritable_listener(std::uint16_t& port) {
  ensure_winsock();
  const raw_socket listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == kInvalidRaw) {
    return refuse(ReasonCode::TransportFailure, "socket() failed");
  }
  int reuse = 1;
  (void)::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                     sizeof(reuse));
  sockaddr_in endpoint{};
  endpoint.sin_family = AF_INET;
  endpoint.sin_port = 0;
  endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(listener, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
    return refuse(ReasonCode::TransportFailure, "bind() failed");
  }
  if (::listen(listener, 8) != 0) {
    return refuse(ReasonCode::TransportFailure, "listen() failed");
  }
#ifdef _WIN32
  int length = sizeof(endpoint);
#else
  socklen_t length = sizeof(endpoint);
#endif
  if (::getsockname(listener, reinterpret_cast<sockaddr*>(&endpoint), &length) != 0) {
    return refuse(ReasonCode::TransportFailure, "getsockname() failed");
  }
  port = ntohs(endpoint.sin_port);
#ifdef _WIN32
  if (SetHandleInformation(reinterpret_cast<HANDLE>(listener), HANDLE_FLAG_INHERIT,
                           HANDLE_FLAG_INHERIT) == 0) {
    return refuse(ReasonCode::TransportFailure, "cannot mark the listener inheritable");
  }
#endif
  return static_cast<std::uintptr_t>(listener);
}

Status close_raw_socket(std::uintptr_t socket) {
  if (socket == 0) return Status::success();
#ifdef _WIN32
  if (::closesocket(static_cast<raw_socket>(socket)) != 0) {
    return refuse(ReasonCode::TransportFailure, "closesocket failed");
  }
#else
  if (::close(static_cast<int>(socket)) != 0) {
    return refuse(ReasonCode::TransportFailure, "close failed");
  }
#endif
  return Status::success();
}

RawSocket::RawSocket(RawSocket&& other) noexcept : socket_(other.socket_) { other.socket_ = 0; }

RawSocket& RawSocket::operator=(RawSocket&& other) noexcept {
  if (this == &other) return *this;
  close();
  socket_ = other.socket_;
  other.socket_ = 0;
  return *this;
}

RawSocket::~RawSocket() { close(); }

Result<RawSocket> RawSocket::connect(std::uint16_t port) {
  ensure_winsock();
  const raw_socket client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (client == kInvalidRaw) return refuse(ReasonCode::TransportFailure, "socket() failed");
  sockaddr_in endpoint{};
  endpoint.sin_family = AF_INET;
  endpoint.sin_port = htons(port);
  endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(client, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
    (void)close_raw_socket(static_cast<std::uintptr_t>(client));
    return refuse(ReasonCode::TransportFailure, "connect() failed");
  }
  RawSocket out;
  out.socket_ = static_cast<std::uintptr_t>(client);
  return out;
}

Status RawSocket::send_bytes(const std::vector<std::uint8_t>& bytes) {
  std::size_t sent = 0;
  while (sent < bytes.size()) {
#ifdef _WIN32
    const int chunk = ::send(static_cast<raw_socket>(socket_),
                             reinterpret_cast<const char*>(bytes.data() + sent),
                             static_cast<int>(bytes.size() - sent), 0);
#else
    const int chunk = static_cast<int>(::send(static_cast<int>(socket_), bytes.data() + sent,
                                              bytes.size() - sent, MSG_NOSIGNAL));
#endif
    if (chunk <= 0) return refuse(ReasonCode::ConnectionClosed, "send failed");
    sent += static_cast<std::size_t>(chunk);
  }
  return Status::success();
}

Status RawSocket::send_partial(const std::vector<std::uint8_t>& bytes) { return send_bytes(bytes); }

Result<std::size_t> RawSocket::receive(std::vector<std::uint8_t>& buffer) {
#ifdef _WIN32
  const int received = ::recv(static_cast<raw_socket>(socket_),
                              reinterpret_cast<char*>(buffer.data()),
                              static_cast<int>(buffer.size()), 0);
#else
  const int received = static_cast<int>(
      ::recv(static_cast<int>(socket_), buffer.data(), buffer.size(), 0));
#endif
  if (received < 0) return refuse(ReasonCode::ConnectionClosed, "recv failed");
  return static_cast<std::size_t>(received);
}

void RawSocket::close() {
  if (socket_ == 0) return;
  (void)close_raw_socket(socket_);
  socket_ = 0;
}

std::string sibling_binary(const std::string& name) {
#ifdef _WIN32
  char buffer[MAX_PATH] = {0};
  const DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
  if (length == 0) return name;
  const fs::path directory = fs::path{std::string{buffer, length}}.parent_path();
#else
  const fs::path directory = fs::current_path();
#endif
  // The build places tools and tests in sibling directories, so a few candidate
  // locations are tried in a fixed order.
  const fs::path candidates[] = {
      directory / name,
      directory / ".." / "tools" / name,
      directory / ".." / ".." / "tools" / name,
      directory / ".." / name,
  };
  std::error_code error;
  for (const fs::path& candidate : candidates) {
    if (fs::exists(candidate, error)) return fs::weakly_canonical(candidate, error).string();
  }
  return (directory / name).string();
}

ScratchDirectory::ScratchDirectory(const std::string& label) {
  static std::uint64_t counter = 0;
  ++counter;
  path_ = (fs::temp_directory_path() /
           ("dpuf-child-" + label + "-" + std::to_string(counter)))
              .string();
  std::error_code error;
  fs::remove_all(fs::path{path_}, error);
  fs::create_directories(fs::path{path_}, error);
}

ScratchDirectory::~ScratchDirectory() {
  // Keeping the scratch directory is a debugging affordance for investigating a
  // failing crash-boundary case; it is off by default.
  if (std::getenv("DPUF_KEEP_SCRATCH") != nullptr) {
    std::fprintf(stderr, "kept scratch directory %s\n", path_.c_str());
    return;
  }
  std::error_code error;
  fs::remove_all(fs::path{path_}, error);
}

Status write_text_file(const std::string& path, const std::string& text) {
  std::ofstream stream{path, std::ios::binary | std::ios::trunc};
  if (!stream) return refuse(ReasonCode::StoreIoFailure, "cannot write " + path);
  stream.write(text.data(), static_cast<std::streamsize>(text.size()));
  stream.close();
  if (!stream) return refuse(ReasonCode::StoreIoFailure, "short write to " + path);
  return Status::success();
}

}  // namespace dpu::fabric::test
