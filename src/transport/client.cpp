#include "dpu/fabric/transport/client.hpp"

#include <utility>

#include "socket.hpp"

namespace dpu::fabric {

struct Client::Impl {
  ClientOptions options{};
  net::dpu_socket_t socket{net::kDpuInvalidSocket};
  std::uint64_t next_request_id{1};
  std::vector<std::uint8_t> buffer{};
  std::vector<std::uint8_t> incoming{};

  Result<Frame> round_trip(OpCode op, const std::vector<std::uint8_t>& payload) {
    if (socket == net::kDpuInvalidSocket) {
      return refuse(ReasonCode::ConnectionClosed, "client is not connected");
    }
    Frame request;
    request.op = op;
    request.request_id = next_request_id++;
    request.set_status(ResponseStatus::Ok);
    request.payload = payload;
    const Result<std::vector<std::uint8_t>> encoded =
        encode_frame(request, options.max_frame_bytes);
    if (!encoded) return encoded.status();
    const Status sent = net::send_all(socket, encoded.value());
    if (!sent.ok()) {
      (void)close();
      return sent;
    }
    while (true) {
      const Result<DecodedFrame> decoded = decode_frame(buffer, options.max_frame_bytes);
      if (!decoded) {
        (void)close();
        return decoded.status();
      }
      if (decoded.value().complete) {
        const Frame frame = decoded.value().frame;
        buffer.erase(buffer.begin(),
                     buffer.begin() + static_cast<std::ptrdiff_t>(decoded.value().consumed));
        if (!(frame.request_id == request.request_id)) {
          (void)close();
          return refuse(ReasonCode::ProtocolViolation, "response does not match the request");
        }
        if (frame.status() != ResponseStatus::Ok) {
          const Result<RefusalPayload> refusal = decode_binary<RefusalPayload>(frame.payload);
          if (refusal) {
            return refuse(refusal.value().code,
                          refusal.value().detail.empty() ? "server refused the request"
                                                         : refusal.value().detail);
          }
          return refuse(ReasonCode::TransportFailure, "unreadable refusal payload");
        }
        return frame;
      }
      incoming.resize(16384);
      const Result<std::size_t> received = net::recv_some(socket, incoming);
      if (!received) {
        (void)close();
        return received.status();
      }
      if (received.value() == 0) {
        (void)close();
        return refuse(ReasonCode::ConnectionClosed, "server closed the connection");
      }
      buffer.insert(buffer.end(), incoming.begin(),
                    incoming.begin() + static_cast<std::ptrdiff_t>(received.value()));
    }
  }

  Status close() {
    if (socket == net::kDpuInvalidSocket) return Status::success();
    const net::dpu_socket_t handle = socket;
    socket = net::kDpuInvalidSocket;
    return net::close_socket(handle);
  }
};

Client::Client(ClientOptions options) : impl_(std::make_unique<Impl>()) {
  impl_->options = std::move(options);
}

Client::~Client() { (void)impl_->close(); }

Result<std::unique_ptr<Client>> Client::connect(const ClientOptions& options) {
  if (options.port == 0) return refuse(ReasonCode::ValueOutOfRange, "port is required");
  const Status started = net::startup();
  if (!started.ok()) return started;
  auto client = std::unique_ptr<Client>(new Client(options));
  Result<net::dpu_socket_t> socket = net::connect_to(options.address, options.port);
  if (!socket) return socket.status();
  client->impl_->socket = socket.value();
  return client;
}

Result<PingResponse> Client::ping(std::string nonce) {
  PingRequest request;
  request.nonce = nonce;
  const Result<Frame> frame = impl_->round_trip(OpCode::Ping, encode_binary(request));
  if (!frame) return frame.status();
  return decode_binary<PingResponse>(frame.value().payload);
}

Result<SubmitResponse> Client::submit(const std::vector<FabricEvent>& events) {
  SubmitRequest request;
  request.events = events;
  const Result<Frame> frame = impl_->round_trip(OpCode::Submit, encode_binary(request, 4096));
  if (!frame) return frame.status();
  return decode_binary<SubmitResponse>(frame.value().payload);
}

Result<QueryResponse> Client::query() {
  QueryRequest request;
  const Result<Frame> frame = impl_->round_trip(OpCode::Query, encode_binary(request));
  if (!frame) return frame.status();
  return decode_binary<QueryResponse>(frame.value().payload);
}

Result<ExplainResponse> Client::explain(const InstanceId* instance) {
  ExplainRequest request;
  if (instance != nullptr && instance->valid()) {
    request.has_instance = true;
    request.instance = *instance;
  }
  const Result<Frame> frame = impl_->round_trip(OpCode::Explain, encode_binary(request));
  if (!frame) return frame.status();
  return decode_binary<ExplainResponse>(frame.value().payload);
}

Result<ExportResponse> Client::export_state(bool pretty) {
  ExportRequest request;
  request.pretty = pretty;
  const Result<Frame> frame = impl_->round_trip(OpCode::Export, encode_binary(request));
  if (!frame) return frame.status();
  return decode_binary<ExportResponse>(frame.value().payload);
}

Result<ShutdownResponse> Client::shutdown() {
  ShutdownRequest request;
  const Result<Frame> frame = impl_->round_trip(OpCode::Shutdown, encode_binary(request));
  if (!frame) return frame.status();
  return decode_binary<ShutdownResponse>(frame.value().payload);
}

Status Client::close() { return impl_->close(); }

bool Client::connected() const noexcept { return impl_->socket != net::kDpuInvalidSocket; }

}  // namespace dpu::fabric
