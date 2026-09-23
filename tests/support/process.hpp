#pragma once

// Independent-process test support.
//
// The transport and crash tests are only meaningful across a real OS process
// boundary. This header spawns real child processes and joins them; it never
// sleeps, polls or uses a timeout. Readiness is established by an inherited
// listening socket that is already accepting before the child starts, so there
// is nothing to wait for.

#include <cstdint>
#include <string>
#include <vector>

#include "dpu/fabric/core/result.hpp"

namespace dpu::fabric::test {

/// A child process handle. Destruction joins the child.
class ChildProcess {
 public:
  ChildProcess() = default;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;
  ~ChildProcess();

  /// Blocks until the child exits and returns its exit code.
  [[nodiscard]] Result<int> join();
  [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }
  [[nodiscard]] std::uint32_t pid() const noexcept { return pid_; }

 private:
  friend Result<ChildProcess> spawn_process(const std::string& executable,
                                            const std::vector<std::string>& arguments,
                                            std::uintptr_t inherited_handle);
  void* handle_{nullptr};
  std::uint32_t pid_{0};
  bool joined_{false};
  int exit_code_{0};
};

/// Spawns \p executable with \p arguments. When \p inherited_handle is non-zero
/// it is marked inheritable and passed to the child, which adopts it as its
/// listening socket (socket activation).
[[nodiscard]] Result<ChildProcess> spawn_process(const std::string& executable,
                                                 const std::vector<std::string>& arguments,
                                                 std::uintptr_t inherited_handle = 0);

/// Creates a listening socket on 127.0.0.1 bound to an ephemeral port, marked
/// inheritable. The returned handle is not closed by this helper.
[[nodiscard]] Result<std::uintptr_t> create_inheritable_listener(std::uint16_t& port);
[[nodiscard]] Status close_raw_socket(std::uintptr_t socket);

/// A raw client socket for adversarial framing tests: it can send bytes the
/// framed client API would never produce.
class RawSocket {
 public:
  RawSocket() = default;
  RawSocket(const RawSocket&) = delete;
  RawSocket& operator=(const RawSocket&) = delete;
  RawSocket(RawSocket&& other) noexcept;
  RawSocket& operator=(RawSocket&& other) noexcept;
  ~RawSocket();

  [[nodiscard]] static Result<RawSocket> connect(std::uint16_t port);
  [[nodiscard]] Status send_bytes(const std::vector<std::uint8_t>& bytes);
  [[nodiscard]] Status send_partial(const std::vector<std::uint8_t>& bytes);
  [[nodiscard]] Result<std::size_t> receive(std::vector<std::uint8_t>& buffer);
  void close();

 private:
  std::uintptr_t socket_{0};
};

/// Absolute path of a binary built next to the running test executable.
[[nodiscard]] std::string sibling_binary(const std::string& name);

/// A scratch directory that removes itself.
class ScratchDirectory {
 public:
  explicit ScratchDirectory(const std::string& label);
  ScratchDirectory(const ScratchDirectory&) = delete;
  ScratchDirectory& operator=(const ScratchDirectory&) = delete;
  ~ScratchDirectory();
  [[nodiscard]] const std::string& path() const { return path_; }

 private:
  std::string path_{};
};

/// Writes a file, refusing on failure.
[[nodiscard]] Status write_text_file(const std::string& path, const std::string& text);

}  // namespace dpu::fabric::test
