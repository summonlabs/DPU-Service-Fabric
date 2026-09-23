// Independent-process transport proof.
//
// A parent test process creates a listening socket and hands it to a child
// process that adopts it (socket activation). Readiness therefore needs no
// polling and no timeout: the socket is already accepting before the child
// exists, and the parent connects to a port it bound itself. Every request in
// this file crosses a real OS process boundary over a real TCP socket.

#include <algorithm>
#include <string>
#include <vector>

#include "dpu/fabric/dpu_fabric.hpp"
#include "support/harness.hpp"
#include "support/process.hpp"
#include "support/scenario.hpp"

namespace dpu::fabric {
namespace {

/// A serving child process plus a connected client.
class ServingChild {
 public:
  ServingChild() = default;
  ServingChild(const ServingChild&) = delete;
  ServingChild& operator=(const ServingChild&) = delete;

  [[nodiscard]] static Result<std::unique_ptr<ServingChild>> start(
      const test::ScratchDirectory& store) {
    std::uint16_t port = 0;
    Result<std::uintptr_t> listener = test::create_inheritable_listener(port);
    if (!listener) return listener.status();
    std::vector<std::string> arguments = {"serve",
                                          "--inherited-socket",
                                          std::to_string(listener.value()),
                                          "--store",
                                          store.path(),
                                          "--workers",
                                          "2"};
    Result<test::ChildProcess> child =
        test::spawn_process(test::sibling_binary("dpufabric.exe"), arguments, listener.value());
    if (!child) {
      (void)test::close_raw_socket(listener.value());
      return child.status();
    }
    auto serving = std::unique_ptr<ServingChild>(new ServingChild());
    serving->child_ = std::move(child).value();
    serving->port_ = port;
    // The parent releases its copy of the listening socket: holding it open
    // would let new connections queue on a listener nobody accepts.
    (void)test::close_raw_socket(listener.value());
    ClientOptions options;
    options.address = "127.0.0.1";
    options.port = port;
    Result<std::unique_ptr<Client>> client = Client::connect(options);
    if (!client) return client.status();
    serving->client_ = std::move(client).value();
    return serving;
  }

  [[nodiscard]] Client& client() { return *client_; }
  [[nodiscard]] std::uint16_t port() const { return port_; }

  ~ServingChild() {
    // Ask the child to stop, then close and join. Joining is the only wait: the
    // child exits after its workers have been joined and its store closed.
    if (client_ != nullptr) {
      if (client_->connected()) (void)client_->shutdown();
      (void)client_->close();
    }
    if (child_.valid()) (void)child_.join();
  }

 private:
  test::ChildProcess child_{};
  std::unique_ptr<Client> client_{};
  std::uint16_t port_{0};
};

DPUF_TEST(transport, child_process_serves_over_a_real_socket) {
  test::ScratchDirectory store{"serve"};
  Result<std::unique_ptr<ServingChild>> serving = ServingChild::start(store);
  DPUF_REQUIRE(serving);
  Client& client = serving.value()->client();

  const Result<PingResponse> ping = client.ping("nonce-1");
  DPUF_REQUIRE(ping);
  DPUF_CHECK_EQ(ping.value().nonce, std::string{"nonce-1"});
  DPUF_CHECK_EQ(ping.value().product, std::string{kProductName});
  DPUF_CHECK_EQ(ping.value().version, std::string{version_string()});

  // Odd payloads survive the round trip unchanged.
  const Result<PingResponse> odd = client.ping(std::string(512, 'x'));
  DPUF_REQUIRE(odd);
  DPUF_CHECK_EQ(odd.value().nonce.size(), std::size_t{512});
}

DPUF_TEST(transport, events_submitted_across_processes_are_applied) {
  test::ScratchDirectory store{"submit"};
  Result<std::unique_ptr<ServingChild>> serving = ServingChild::start(store);
  DPUF_REQUIRE(serving);
  Client& client = serving.value()->client();

  const std::vector<FabricEvent> events = test::standard_world_events(1, 100);
  const Result<SubmitResponse> submitted = client.submit(events);
  DPUF_REQUIRE(submitted);
  DPUF_CHECK_EQ(submitted.value().report.applied, static_cast<std::uint64_t>(events.size()));
  DPUF_CHECK_EQ(submitted.value().report.refused, std::uint64_t{0});

  const Result<QueryResponse> query = client.query();
  DPUF_REQUIRE(query);
  DPUF_CHECK_EQ(query.value().state_digest, submitted.value().state_digest);
  DPUF_CHECK_EQ(query.value().durable_digest, submitted.value().durable_digest);

  // A redelivery of the identical batch is suppressed, and the digest is
  // unchanged: duplicate delivery is idempotent across the process boundary too.
  const Result<SubmitResponse> again = client.submit(events);
  DPUF_REQUIRE(again);
  DPUF_CHECK_EQ(again.value().report.suppressed, static_cast<std::uint64_t>(events.size()));
  DPUF_CHECK_EQ(again.value().report.applied, std::uint64_t{0});
  DPUF_CHECK_EQ(again.value().state_digest, submitted.value().state_digest);

  const Result<ExportResponse> exported = client.export_state(false);
  DPUF_REQUIRE(exported);
  const Result<JsonValue> parsed = parse_json(exported.value().document, 4u << 20);
  DPUF_CHECK(parsed.has_value());
  DPUF_CHECK(parsed.value().find("state") != nullptr);

  const Result<ShutdownResponse> stopped = client.shutdown();
  DPUF_REQUIRE(stopped);
  DPUF_CHECK_EQ(stopped.value().instances, std::uint64_t{0});
}

DPUF_TEST(transport, refusal_is_reported_not_faked) {
  test::ScratchDirectory store{"refusal"};
  Result<std::unique_ptr<ServingChild>> serving = ServingChild::start(store);
  DPUF_REQUIRE(serving);
  Client& client = serving.value()->client();

  // An intent for a group that was never declared is refused by the server and
  // reported as a refusal rather than as an accepted submit.
  std::vector<FabricEvent> events = test::standard_world_events(1, 100);
  events.push_back(test::make_event("bad-intent", "world-origin", 99, 1,
                                    IntentSubmitted{test::standard_intent(1, 4)},
                                    EvidenceClass::Synthetic));
  const Result<SubmitResponse> submitted = client.submit(events);
  DPUF_REQUIRE(submitted);
  DPUF_CHECK_EQ(submitted.value().report.refused, std::uint64_t{1});
  DPUF_CHECK(is_refusal(submitted.value().report.primary));
  const Result<QueryResponse> query = client.query();
  DPUF_REQUIRE(query);
  DPUF_CHECK_EQ(query.value().instances, std::uint64_t{0});
}

DPUF_TEST(transport, malformed_frames_are_rejected_and_the_server_survives) {
  test::ScratchDirectory store{"frames"};
  Result<std::unique_ptr<ServingChild>> serving = ServingChild::start(store);
  DPUF_REQUIRE(serving);
  Client& client = serving.value()->client();
  DPUF_CHECK(client.ping("alive").has_value());

  {
    // Bad magic.
    Result<test::RawSocket> raw = test::RawSocket::connect(serving.value()->port());
    DPUF_REQUIRE(raw);
    std::vector<std::uint8_t> garbage(64, 0x5A);
    DPUF_CHECK_OK(raw.value().send_bytes(garbage));
    std::vector<std::uint8_t> reply(256);
    const Result<std::size_t> received = raw.value().receive(reply);
    DPUF_CHECK(received.has_value());
    DPUF_CHECK(received.value() > 0);
    raw.value().close();
  }
  {
    // A frame header that declares far more payload than the bound allows.
    Result<test::RawSocket> raw = test::RawSocket::connect(serving.value()->port());
    DPUF_REQUIRE(raw);
    std::vector<std::uint8_t> header(24, 0);
    header[0] = 0x44;
    header[1] = 0x50;
    header[2] = 0x55;
    header[3] = 0x46;
    header[4] = 1;  // version
    header[6] = 1;  // op = Ping
    header[20] = 0xFF;
    header[21] = 0xFF;
    header[22] = 0xFF;
    header[23] = 0x7F;  // ~2 GiB declared
    DPUF_CHECK_OK(raw.value().send_bytes(header));
    std::vector<std::uint8_t> reply(256);
    const Result<std::size_t> received = raw.value().receive(reply);
    DPUF_CHECK(received.has_value());
    raw.value().close();
  }
  {
    // A connection abandoned mid-frame is cancelled, and nothing is applied.
    Result<test::RawSocket> raw = test::RawSocket::connect(serving.value()->port());
    DPUF_REQUIRE(raw);
    std::vector<std::uint8_t> partial(12, 0);
    partial[0] = 0x44;
    partial[1] = 0x50;
    partial[2] = 0x55;
    partial[3] = 0x46;
    partial[4] = 1;
    partial[6] = 2;  // op = Submit
    DPUF_CHECK_OK(raw.value().send_partial(partial));
    raw.value().close();
  }
  // The server is still serving after every hostile connection.
  const Result<PingResponse> still = client.ping("still-alive");
  DPUF_REQUIRE(still);
  DPUF_CHECK_EQ(still.value().nonce, std::string{"still-alive"});
  const Result<QueryResponse> query = client.query();
  DPUF_REQUIRE(query);
  DPUF_CHECK_EQ(query.value().instances, std::uint64_t{0});
}

DPUF_TEST(transport, shutdown_wakes_a_silent_connection) {
  test::ScratchDirectory store{"silent"};
  std::uint16_t port = 0;
  Result<std::uintptr_t> listener = test::create_inheritable_listener(port);
  DPUF_REQUIRE(listener);
  std::vector<std::string> arguments = {"serve",
                                        "--inherited-socket",
                                        std::to_string(listener.value()),
                                        "--store",
                                        store.path(),
                                        "--workers",
                                        "2"};
  Result<test::ChildProcess> child =
      test::spawn_process(test::sibling_binary("dpufabric.exe"), arguments, listener.value());
  DPUF_REQUIRE(child);
  (void)test::close_raw_socket(listener.value());

  ClientOptions options;
  options.address = "127.0.0.1";
  options.port = port;
  Result<std::unique_ptr<Client>> client = Client::connect(options);
  DPUF_REQUIRE(client);
  DPUF_CHECK(client.value()->ping("before-silence").has_value());

  // A connection that connects and then never sends anything holds a worker. It
  // is kept open across the shutdown on purpose: shutdown must wake it rather
  // than wait for a peer that will never speak.
  Result<test::RawSocket> silent = test::RawSocket::connect(port);
  DPUF_REQUIRE(silent);

  const Result<ShutdownResponse> stopped = client.value()->shutdown();
  DPUF_REQUIRE(stopped);
  (void)client.value()->close();
  const Result<int> code = child.value().join();
  DPUF_REQUIRE(code);
  DPUF_CHECK_EQ(code.value(), 0);
  silent.value().close();
}

DPUF_TEST(transport, shutdown_is_graceful_and_joinable) {
  test::ScratchDirectory store{"shutdown"};
  std::uint16_t port = 0;
  Result<std::uintptr_t> listener = test::create_inheritable_listener(port);
  DPUF_REQUIRE(listener);
  std::vector<std::string> arguments = {"serve",
                                        "--inherited-socket",
                                        std::to_string(listener.value()),
                                        "--store",
                                        store.path(),
                                        "--workers",
                                        "2"};
  Result<test::ChildProcess> child =
      test::spawn_process(test::sibling_binary("dpufabric.exe"), arguments, listener.value());
  DPUF_REQUIRE(child);
  (void)test::close_raw_socket(listener.value());
  ClientOptions options;
  options.address = "127.0.0.1";
  options.port = port;
  Result<std::unique_ptr<Client>> client = Client::connect(options);
  DPUF_REQUIRE(client);
  DPUF_CHECK(client.value()->ping("before-shutdown").has_value());
  const Result<ShutdownResponse> stopped = client.value()->shutdown();
  DPUF_REQUIRE(stopped);
  (void)client.value()->close();
  // Joining is the synchronisation point: the child exits only after its workers
  // have been joined and its store closed.
  const Result<int> code = child.value().join();
  DPUF_REQUIRE(code);
  DPUF_CHECK_EQ(code.value(), 0);
  // The store it wrote is a valid store.
  // The child ran with the tool's identity, and a reopen must present the same
  // one or it is refused.
  RuntimeConfig config;
  config.store_id = StoreId::literal("dpufabric-store");
  config.coordinator = OriginId::literal("inspection");
  config.epoch = CoordinatorEpoch{1};
  config.boot = BootIncarnation{1};
  config.store_directory = store.path();
  Result<std::unique_ptr<FabricRuntime>> reopened = FabricRuntime::open(config);
  DPUF_REQUIRE(reopened);
  DPUF_CHECK(reopened.value()->recovery().recovered_from_store);
  DPUF_CHECK_OK(reopened.value()->close());
}

}  // namespace
}  // namespace dpu::fabric
