#include "dpu/fabric/transport/server.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <string>
#include <utility>

#include "socket.hpp"

namespace dpu::fabric {
/// Lock order, documented because it is the only place two locks are held at
/// once: \c mutex may be taken while \c stats_mutex is held is never true; the
/// only nesting is \c mutex -> \c stats_mutex inside enqueue(), and nothing takes
/// them in the other order. No lock is held across a worker join, a blocking
/// socket operation or a callback into the runtime.
struct Server::Impl {
  FabricRuntime* runtime{nullptr};
  ServerOptions options{};
  net::dpu_socket_t listener{net::kDpuInvalidSocket};
  std::uint16_t bound_port{0};
  std::mutex mutex{};
  std::condition_variable wake{};
  std::deque<net::dpu_socket_t> queue{};
  /// Connections currently being served. Shutdown shuts each of them down so a
  /// worker blocked in recv returns immediately instead of waiting for a peer
  /// that may never speak again.
  std::vector<net::dpu_socket_t> active{};
  std::vector<std::thread> workers{};
  bool stopping{false};
  bool started{false};
  mutable std::mutex stats_mutex{};
  ServerStats stats{};

  void record(void (*fn)(ServerStats&)) {
    std::lock_guard<std::mutex> guard{stats_mutex};
    fn(stats);
  }

  Status dispatch(const Frame& request, Frame& response, bool& shutdown_after);
  void handle_connection(net::dpu_socket_t client);
  void worker_loop();
  Status enqueue(net::dpu_socket_t client);
  /// Wakes the accept loop and every worker, then releases the listener.
  void stop();
};

void Server::Impl::stop() {
  std::vector<net::dpu_socket_t> to_wake;
  {
    std::lock_guard<std::mutex> guard{mutex};
    stopping = true;
    if (listener != net::kDpuInvalidSocket) {
      // Closing the listener is what wakes a blocked accept().
      (void)net::close_socket(listener);
      listener = net::kDpuInvalidSocket;
    }
    to_wake = active;
    // Queued connections are never served once shutdown has begun.
    for (const net::dpu_socket_t queued : queue) {
      (void)net::shutdown_both(queued);
      (void)net::close_socket(queued);
    }
    queue.clear();
  }
  // Shutting a connection down in both directions makes a blocked receive
  // return, so a worker finishes its current request instead of hanging on a
  // peer that will never send another byte. This is a wakeup, not a timeout.
  for (const net::dpu_socket_t socket : to_wake) {
    (void)net::shutdown_both(socket);
  }
  wake.notify_all();
}

Status Server::Impl::dispatch(const Frame& request, Frame& response, bool& shutdown_after) {
  shutdown_after = false;
  response.op = request.op;
  response.request_id = request.request_id;
  response.set_status(ResponseStatus::Ok);
  switch (request.op) {
    case OpCode::Ping: {
      const Result<PingRequest> body = decode_binary<PingRequest>(request.payload);
      if (!body) {
        response.set_status(ResponseStatus::Refused);
        response.payload = encode_binary(RefusalPayload{body.status().code(),
                                                        body.status().detail()});
        return Status::success();
      }
      PingResponse reply;
      reply.nonce = body.value().nonce;
      reply.product.assign(kProductName);
      reply.version.assign(version_string());
      response.payload = encode_binary(reply);
      return Status::success();
    }
    case OpCode::Submit: {
      Result<SubmitRequest> body = decode_binary<SubmitRequest>(request.payload);
      if (!body) {
        response.set_status(ResponseStatus::Refused);
        response.payload = encode_binary(RefusalPayload{body.status().code(),
                                                        body.status().detail()});
        return Status::success();
      }
      Result<BatchReport> report = runtime->submit(body.value().events);
      if (!report) {
        response.set_status(ResponseStatus::Refused);
        response.payload = encode_binary(RefusalPayload{report.status().code(),
                                                        report.status().detail()});
        return Status::success();
      }
      SubmitResponse reply;
      reply.report = std::move(report).value();
      reply.state_digest = runtime->state_digest();
      reply.durable_digest = runtime->durable_digest();
      // Per-event refusals are part of a successfully executed submit: the
      // report says what was refused and why. The frame status stays Ok so the
      // client decodes the report instead of a transport-level refusal.
      response.payload = encode_binary(reply);
      return Status::success();
    }
    case OpCode::Query: {
      QueryResponse reply;
      reply.state_digest = runtime->state_digest();
      reply.durable_digest = runtime->durable_digest();
      reply.instances = runtime->instance_count();
      reply.staged = runtime->staged_count();
      reply.recovery = runtime->recovery();
      response.payload = encode_binary(reply);
      return Status::success();
    }
    case OpCode::Explain: {
      Result<ExplainRequest> body = decode_binary<ExplainRequest>(request.payload);
      if (!body) {
        response.set_status(ResponseStatus::Refused);
        response.payload = encode_binary(RefusalPayload{body.status().code(),
                                                        body.status().detail()});
        return Status::success();
      }
      ExplainResponse reply;
      if (body.value().has_instance) {
        reply.explanations = runtime->explain_instance(body.value().instance);
      } else {
        reply.explanations = runtime->explain_last_decision();
      }
      response.payload = encode_binary(reply);
      return Status::success();
    }
    case OpCode::Export: {
      Result<ExportRequest> body = decode_binary<ExportRequest>(request.payload);
      if (!body || !options.export_allowed) {
        response.set_status(ResponseStatus::Refused);
        const ReasonCode code = !body ? body.status().code() : ReasonCode::UnsupportedOperation;
        response.payload =
            encode_binary(RefusalPayload{code, "export is refused by server policy"});
        return Status::success();
      }
      ExportResponse reply;
      reply.document = runtime->export_json().to_text(body.value().pretty);
      if (reply.document.size() > options.max_frame_bytes / 2) {
        response.set_status(ResponseStatus::Refused);
        response.payload =
            encode_binary(RefusalPayload{ReasonCode::FrameOversized,
                                         "export exceeds the frame bound"});
        return Status::success();
      }
      response.payload = encode_binary(reply);
      return Status::success();
    }
    case OpCode::Shutdown: {
      ShutdownResponse reply;
      reply.instances = runtime->instance_count();
      reply.state_digest = runtime->state_digest();
      response.payload = encode_binary(reply);
      shutdown_after = true;
      return Status::success();
    }
  }
  response.set_status(ResponseStatus::Refused);
  response.payload = encode_binary(RefusalPayload{ReasonCode::UnknownOperation, "operation"});
  return Status::success();
}

void Server::Impl::handle_connection(net::dpu_socket_t client) {
  {
    std::lock_guard<std::mutex> guard{mutex};
    if (stopping) {
      // A shutdown that began while this connection was queued: it is never
      // served, so nothing is executed and nothing is acknowledged.
      (void)net::shutdown_both(client);
      (void)net::close_socket(client);
      return;
    }
    active.push_back(client);
  }
  std::vector<std::uint8_t> buffer;
  buffer.reserve(options.max_frame_bytes / 4 + kProtocolHeaderBytes + kProtocolTrailerBytes);
  std::vector<std::uint8_t> incoming(16384);
  bool shutdown_after = false;
  while (!shutdown_after) {
    Result<DecodedFrame> decoded = decode_frame(buffer, options.max_frame_bytes);
    if (!decoded) {
      // A framing violation ends the connection. An observable refusal frame is
      // sent first when the peer can still receive it.
      Frame refusal;
      refusal.op = OpCode::Ping;
      refusal.set_status(ResponseStatus::Error);
      refusal.payload =
          encode_binary(RefusalPayload{decoded.status().code(), decoded.status().detail()});
      const Result<std::vector<std::uint8_t>> encoded = encode_frame(refusal, options.max_frame_bytes);
      if (encoded) (void)net::send_all(client, encoded.value());
      record([](ServerStats& stats) {
        stats.frames_rejected += 1;
        stats.connections_cancelled += 1;
      });
      break;
    }
    if (!decoded.value().complete) {
      const Result<std::size_t> received = net::recv_some(client, incoming);
      if (!received) {
        record([](ServerStats& stats) { stats.connections_cancelled += 1; });
        break;
      }
      if (received.value() == 0) {
        // Orderly close. Whatever was buffered is incomplete, so nothing is
        // executed and nothing is acknowledged.
        if (!buffer.empty()) {
          record([](ServerStats& stats) { stats.connections_cancelled += 1; });
        }
        break;
      }
      buffer.insert(buffer.end(), incoming.begin(),
                    incoming.begin() + static_cast<std::ptrdiff_t>(received.value()));
      continue;
    }

    Frame response;
    bool request_shutdown = false;
    (void)dispatch(decoded.value().frame, response, request_shutdown);
    buffer.erase(buffer.begin(),
                 buffer.begin() + static_cast<std::ptrdiff_t>(decoded.value().consumed));
    record([](ServerStats& stats) { stats.requests += 1; });
    if (response.status() != ResponseStatus::Ok) {
      record([](ServerStats& stats) { stats.refusals += 1; });
    }
    const Result<std::vector<std::uint8_t>> encoded = encode_frame(response, options.max_frame_bytes);
    if (!encoded) {
      record([](ServerStats& stats) { stats.frames_rejected += 1; });
      break;
    }
    const Status sent = net::send_all(client, encoded.value());
    if (!sent.ok()) {
      // The acknowledgement could not be delivered: no success is claimed for
      // this request and the connection is dropped.
      record([](ServerStats& stats) { stats.connections_cancelled += 1; });
      break;
    }
    if (request_shutdown) {
      {
        std::lock_guard<std::mutex> guard{stats_mutex};
        stats.shutdown_requested = true;
      }
      stop();
      break;
    }
  }
  {
    std::lock_guard<std::mutex> guard{mutex};
    active.erase(std::remove(active.begin(), active.end(), client), active.end());
  }
  (void)net::close_socket(client);
}

void Server::Impl::worker_loop() {
  while (true) {
    net::dpu_socket_t client = net::kDpuInvalidSocket;
    {
      std::unique_lock<std::mutex> lock{mutex};
      wake.wait(lock, [this] { return stopping || !queue.empty(); });
      if (queue.empty()) {
        if (stopping) return;
        continue;
      }
      client = queue.front();
      queue.pop_front();
    }
    handle_connection(client);
  }
}

Status Server::Impl::enqueue(net::dpu_socket_t client) {
  {
    std::lock_guard<std::mutex> guard{mutex};
    if (queue.size() >= options.max_pending_connections) {
      record([](ServerStats& stats) { stats.queue_rejections += 1; });
      Frame refusal;
      refusal.op = OpCode::Ping;
      refusal.set_status(ResponseStatus::Refused);
      refusal.payload =
          encode_binary(RefusalPayload{ReasonCode::ServerBusy, "connection queue is full"});
      const Result<std::vector<std::uint8_t>> encoded = encode_frame(refusal, options.max_frame_bytes);
      if (encoded) (void)net::send_all(client, encoded.value());
      (void)net::close_socket(client);
      return refuse(ReasonCode::ServerBusy, "connection queue is full");
    }
    queue.push_back(client);
  }
  wake.notify_one();
  return Status::success();
}

Server::Server(FabricRuntime& runtime, const ServerOptions& options)
    : impl_(std::make_unique<Impl>()) {
  impl_->runtime = &runtime;
  impl_->options = options;
}

Server::~Server() {
  request_shutdown();
  if (impl_ && impl_->started) {
    for (std::thread& worker : impl_->workers) {
      if (worker.joinable()) worker.join();
    }
    impl_->workers.clear();
  }
  if (impl_ && impl_->listener != net::kDpuInvalidSocket) {
    (void)net::close_socket(impl_->listener);
    impl_->listener = net::kDpuInvalidSocket;
  }
}

Result<std::unique_ptr<Server>> Server::create(FabricRuntime& runtime, const ServerOptions& options) {
  if (options.workers == 0 || options.workers > RuntimeBounds::kMaxWorkersCeiling) {
    return refuse(ReasonCode::ValueOutOfRange, "worker count out of range");
  }
  if (options.max_pending_connections == 0 ||
      options.max_pending_connections > RuntimeBounds::kMaxPendingRequestsCeiling) {
    return refuse(ReasonCode::ValueOutOfRange, "connection queue bound out of range");
  }
  const Status started = net::startup();
  if (!started.ok()) return started;
  return std::unique_ptr<Server>(new Server(runtime, options));
}

void Server::set_endpoint(std::string address, std::uint16_t port) {
  impl_->options.bind_address = std::move(address);
  impl_->options.port = port;
}

Status Server::listen() {
  Result<net::dpu_socket_t> listener = net::listen_on(impl_->options.bind_address, impl_->options.port);
  if (!listener) return listener.status();
  impl_->listener = listener.value();
  Result<std::uint16_t> port = net::local_port(impl_->listener);
  if (!port) return port.status();
  impl_->bound_port = port.value();
  return Status::success();
}

Status Server::adopt(std::uintptr_t socket_handle) {
  if (socket_handle == 0) return refuse(ReasonCode::ValueOutOfRange, "invalid socket handle");
  impl_->listener = static_cast<net::dpu_socket_t>(socket_handle);
  Result<std::uint16_t> port = net::local_port(impl_->listener);
  if (!port) return port.status();
  impl_->bound_port = port.value();
  return Status::success();
}

std::uint16_t Server::port() const noexcept { return impl_->bound_port; }

Status Server::serve() {
  if (impl_->listener == net::kDpuInvalidSocket) {
    return refuse(ReasonCode::InvalidStateTransition, "server is not listening");
  }
  {
    std::lock_guard<std::mutex> guard{impl_->mutex};
    if (impl_->started) return refuse(ReasonCode::InvalidStateTransition, "server already serving");
    impl_->started = true;
    impl_->stopping = false;
  }
  impl_->workers.reserve(impl_->options.workers);
  for (std::size_t i = 0; i < impl_->options.workers; ++i) {
    impl_->workers.emplace_back([this] { impl_->worker_loop(); });
  }

  while (true) {
    {
      std::lock_guard<std::mutex> guard{impl_->mutex};
      if (impl_->stopping) break;
    }
    Result<net::dpu_socket_t> client = net::accept_one(impl_->listener);
    if (!client) {
      std::lock_guard<std::mutex> guard{impl_->mutex};
      if (impl_->stopping) break;
      continue;
    }
    impl_->record([](ServerStats& stats) { stats.connections += 1; });
    (void)impl_->enqueue(client.value());
  }

  {
    std::lock_guard<std::mutex> guard{impl_->mutex};
    impl_->stopping = true;
  }
  impl_->wake.notify_all();
  for (std::thread& worker : impl_->workers) {
    if (worker.joinable()) worker.join();
  }
  impl_->workers.clear();
  if (impl_->listener != net::kDpuInvalidSocket) {
    (void)net::close_socket(impl_->listener);
    impl_->listener = net::kDpuInvalidSocket;
  }
  impl_->started = false;
  return Status::success();
}

void Server::request_shutdown() { impl_->stop(); }

ServerStats Server::stats() const {
  std::lock_guard<std::mutex> guard{impl_->stats_mutex};
  return impl_->stats;
}

}  // namespace dpu::fabric
