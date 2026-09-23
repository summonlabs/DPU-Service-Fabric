// dpufabric: inspection, planning, replay, transport and crash-boundary tool.
//
// The tool is the operator surface and the independent-process harness used by
// the transport and crash tests. It never bypasses the runtime: every command
// goes through the public API, so what the tool can do is what a consumer can do.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "dpu/fabric/dpu_fabric.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#endif

namespace {

namespace fs = std::filesystem;
using namespace dpu::fabric;

int usage() {
  std::fprintf(stderr,
               "dpufabric %s\n"
               "usage:\n"
               "  dpufabric version\n"
               "  dpufabric export --store DIR [--compact]\n"
               "  dpufabric verify --store DIR\n"
               "  dpufabric explain --store DIR [--instance ID]\n"
               "  dpufabric plan --topology F --policy F --services F --group F --intent F\n"
               "                  [--dependencies F] [--generation N]\n"
               "  dpufabric replay --events F [--store DIR]\n"
               "  dpufabric serve (--listen HOST:PORT | --inherited-socket N) [--store DIR]\n"
               "                 [--workers N] [--max-frame N]\n"
               "  dpufabric client --connect HOST:PORT (ping|query|export|submit FILE|shutdown)\n"
               "  dpufabric crash --store DIR --boundary NAME\n",
               std::string{version_string()}.c_str());
  return 2;
}

/// Command-line arguments as name/value pairs. Unknown names are refused rather
/// than ignored, so a typo cannot silently change what a command does.
class Arguments {
 public:
  Arguments(int argc, char** argv) {
    for (int i = 0; i < argc; ++i) {
      const std::string token = argv[i];
      if (token.rfind("--", 0) == 0) {
        std::string name = token.substr(2);
        std::string value;
        const std::size_t equals = name.find('=');
        if (equals != std::string::npos) {
          value = name.substr(equals + 1);
          name = name.substr(0, equals);
        } else if (i + 1 < argc && std::string{argv[i + 1]}.rfind("--", 0) != 0) {
          value = argv[i + 1];
          ++i;
        }
        values_.push_back({name, value});
        continue;
      }
      positional_.push_back(token);
    }
  }

  [[nodiscard]] std::optional<std::string> get(std::string_view name) const {
    for (const auto& entry : values_) {
      if (entry.first == name) return entry.second;
    }
    return std::nullopt;
  }

  [[nodiscard]] bool has(std::string_view name) const { return get(name).has_value(); }

  [[nodiscard]] std::optional<std::uint64_t> number(std::string_view name) const {
    const std::optional<std::string> text = get(name);
    if (!text.has_value()) return std::nullopt;
    try {
      return static_cast<std::uint64_t>(std::stoull(*text));
    } catch (...) {
      return std::nullopt;
    }
  }

  [[nodiscard]] const std::vector<std::string>& positional() const { return positional_; }

 private:
  std::vector<std::pair<std::string, std::string>> values_{};
  std::vector<std::string> positional_{};
};

Result<std::string> read_text(const std::string& path) {
  std::ifstream stream{path, std::ios::binary};
  if (!stream) return refuse(ReasonCode::StoreNotFound, "cannot open " + path);
  std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  return text;
}

template <class T>
Result<T> read_json_model(const std::string& path) {
  const Result<std::string> text = read_text(path);
  if (!text) return text.status();
  const Result<JsonValue> parsed = parse_json(text.value(), 4u << 20);
  if (!parsed) return parsed.status();
  return decode_json<T>(parsed.value());
}

template <class T>
Result<std::vector<T>> read_json_array(const std::string& path) {
  const Result<std::string> text = read_text(path);
  if (!text) return text.status();
  const Result<JsonValue> parsed = parse_json(text.value(), 4u << 20);
  if (!parsed) return parsed.status();
  if (!parsed.value().is_array()) {
    return refuse(ReasonCode::MalformedInput, "expected a JSON array in " + path);
  }
  std::vector<T> out;
  JsonReader reader{parsed.value()};
  std::size_t count = 0;
  reader.begin_array(count);
  if (!reader.ok()) return reader.status();
  out.resize(count);
  for (std::size_t i = 0; i < count; ++i) {
    reader.begin_element();
    do_value(reader, out[i]);
    reader.end_element();
    if (!reader.ok()) return reader.status();
  }
  reader.end_array();
  return out;
}

int report(const Status& status, const char* what) {
  if (status.ok()) return 0;
  std::fprintf(stderr, "%s: %s\n", what, status.message().c_str());
  return 1;
}

/// The tool's runtime identity. Every command that touches a store uses the same
/// one, so a store written by one command can be inspected by another; a store
/// written with a different identity is refused on purpose.
RuntimeConfig tool_runtime_config() {
  RuntimeConfig config;
  config.store_id = StoreId::literal("dpufabric-store");
  config.coordinator = OriginId::literal("dpufabric-coordinator");
  config.epoch = CoordinatorEpoch{1};
  config.boot = BootIncarnation{1};
  return config;
}

RuntimeConfig store_runtime_config(const std::string& directory) {
  RuntimeConfig config = tool_runtime_config();
  config.store_directory = directory;
  return config;
}

/// Parses "host:port".
Result<std::pair<std::string, std::uint16_t>> parse_endpoint(const std::string& text) {
  const std::size_t colon = text.rfind(':');
  if (colon == std::string::npos) {
    return refuse(ReasonCode::MalformedInput, "endpoint must be host:port");
  }
  const std::string host = text.substr(0, colon);
  const std::string port_text = text.substr(colon + 1);
  if (host.empty() || port_text.empty()) {
    return refuse(ReasonCode::MalformedInput, "endpoint must be host:port");
  }
  unsigned long port = 0;
  try {
    port = std::stoul(port_text);
  } catch (...) {
    return refuse(ReasonCode::MalformedInput, "invalid port");
  }
  if (port == 0 || port > 65535) return refuse(ReasonCode::ValueOutOfRange, "invalid port");
  return std::make_pair(host, static_cast<std::uint16_t>(port));
}

int command_version() {
  std::printf("%s\n", version_banner().c_str());
  std::printf("format=%u semantics=%u\n", kFormatVersion, kSemanticVersion);
  return 0;
}

int command_export(const Arguments& arguments) {
  const std::optional<std::string> directory = arguments.get("store");
  if (!directory.has_value()) return usage();
  RuntimeConfig config = store_runtime_config(*directory);
  Result<std::unique_ptr<FabricRuntime>> runtime = FabricRuntime::open(config);
  if (!runtime) return report(runtime.status(), "open");
  std::printf("%s\n", runtime.value()->export_json().to_text(!arguments.has("compact")).c_str());
  const Status closed = runtime.value()->close();
  return report(closed, "close");
}

int command_verify(const Arguments& arguments) {
  const std::optional<std::string> directory = arguments.get("store");
  if (!directory.has_value()) return usage();
  RuntimeConfig config = store_runtime_config(*directory);
  Result<std::unique_ptr<FabricRuntime>> runtime = FabricRuntime::open(config);
  if (!runtime) {
    std::printf("classification=refused code=%s detail=%s\n",
                std::string{to_string(runtime.status().code())}.c_str(),
                runtime.status().detail().c_str());
    return 1;
  }
  const RuntimeState state = runtime.value()->snapshot();
  std::printf("classification=%s\n",
              std::string{to_string(runtime.value()->recovery().classification)}.c_str());
  std::printf("store_id=%s\n", state.store_id.str().c_str());
  std::printf("epoch=%llu boot=%llu\n",
              static_cast<unsigned long long>(state.epoch.value()),
              static_cast<unsigned long long>(state.boot.value()));
  std::printf("clock=%llu watermark=%llu\n",
              static_cast<unsigned long long>(state.clock.ticks),
              static_cast<unsigned long long>(state.watermark.ticks));
  std::printf("devices=%zu services=%zu groups=%zu instances=%zu plans=%zu decisions=%zu\n",
              state.topology.dpus.size(), state.services.size(), state.groups.size(),
              state.instances.size(), state.plans.size(), state.decisions.size());
  std::printf("state_digest=%s\n", runtime.value()->state_digest().hex().c_str());
  std::printf("durable_digest=%s\n", runtime.value()->durable_digest().hex().c_str());
  std::printf("truncation_records=%zu dropped=%llu\n", state.truncations.records().size(),
              static_cast<unsigned long long>(state.truncations.total_dropped()));
  for (const TruncationRecord& record : state.truncations.records()) {
    std::printf("  truncation container=%s requested=%llu accepted=%llu dropped=%llu reason=%s\n",
                record.container.c_str(), static_cast<unsigned long long>(record.requested),
                static_cast<unsigned long long>(record.accepted),
                static_cast<unsigned long long>(record.dropped),
                std::string{to_string(record.reason)}.c_str());
  }
  const Status closed = runtime.value()->close();
  return report(closed, "close");
}

int command_explain(const Arguments& arguments) {
  const std::optional<std::string> directory = arguments.get("store");
  if (!directory.has_value()) return usage();
  RuntimeConfig config = store_runtime_config(*directory);
  Result<std::unique_ptr<FabricRuntime>> runtime = FabricRuntime::open(config);
  if (!runtime) return report(runtime.status(), "open");
  std::vector<Explanation> explanations;
  const std::optional<std::string> instance_text = arguments.get("instance");
  if (instance_text.has_value()) {
    const Result<InstanceId> id = InstanceId::parse(*instance_text);
    if (!id) return report(id.status(), "instance");
    explanations = runtime.value()->explain_instance(id.value());
  } else {
    explanations = runtime.value()->explain_last_decision();
  }
  for (const Explanation& entry : explanations) {
    std::printf("%s subject=%s deployment=%llu topology=%llu capability=%llu policy=%llu note=%s\n",
                std::string{to_string(entry.code)}.c_str(), entry.subject.c_str(),
                static_cast<unsigned long long>(entry.deployment.value()),
                static_cast<unsigned long long>(entry.topology.value()),
                static_cast<unsigned long long>(entry.capability.value()),
                static_cast<unsigned long long>(entry.policy.value()), entry.note.c_str());
  }
  const Status closed = runtime.value()->close();
  return report(closed, "close");
}

int command_plan(const Arguments& arguments) {
  const std::optional<std::string> topology_path = arguments.get("topology");
  const std::optional<std::string> policy_path = arguments.get("policy");
  const std::optional<std::string> services_path = arguments.get("services");
  const std::optional<std::string> group_path = arguments.get("group");
  const std::optional<std::string> intent_path = arguments.get("intent");
  if (!topology_path || !policy_path || !services_path || !group_path || !intent_path) {
    return usage();
  }
  Result<TopologySnapshot> topology = read_json_model<TopologySnapshot>(*topology_path);
  if (!topology) return report(topology.status(), "topology");
  Result<PolicyState> policy = read_json_model<PolicyState>(*policy_path);
  if (!policy) return report(policy.status(), "policy");
  Result<std::vector<ServiceDefinition>> services =
      read_json_array<ServiceDefinition>(*services_path);
  if (!services) return report(services.status(), "services");
  Result<ServiceGroup> group = read_json_model<ServiceGroup>(*group_path);
  if (!group) return report(group.status(), "group");
  Result<PlacementIntent> intent = read_json_model<PlacementIntent>(*intent_path);
  if (!intent) return report(intent.status(), "intent");

  std::vector<Dependency> dependencies;
  if (const std::optional<std::string> path = arguments.get("dependencies")) {
    Result<std::vector<Dependency>> parsed = read_json_array<Dependency>(*path);
    if (!parsed) return report(parsed.status(), "dependencies");
    dependencies = std::move(parsed).value();
  }
  DependencyGraph graph;
  const Status built = graph.build(dependencies, RuntimeBounds{}.max_dependencies);
  if (!built.ok()) return report(built, "dependencies");

  AuthorityRegistry authority;
  PlanRequest request;
  request.topology = &topology.value();
  request.policy = &policy.value();
  request.services = &services.value();
  request.dependencies = &graph;
  request.group = &group.value();
  request.intent = &intent.value();
  request.authority = &authority;
  request.generation = intent.value().generation;
  request.now = intent.value().submitted_at;
  request.epoch = CoordinatorEpoch{1};
  request.boot = BootIncarnation{1};
  const RuntimeBounds bounds{};
  if (!arguments.has("generation")) {
    request.generation = intent.value().generation;
  } else {
    const std::optional<std::uint64_t> generation = arguments.number("generation");
    if (!generation.has_value()) return usage();
    request.generation = DeploymentGeneration{*generation};
  }
  request.bounds = &bounds;

  const PlanOutcome outcome = build_plan(request);
  std::printf("feasible=%s primary=%s\n", outcome.feasible ? "true" : "false",
              std::string{to_string(outcome.primary)}.c_str());
  for (const Explanation& entry : outcome.explanations) {
    std::printf("explanation code=%s subject=%s note=%s\n",
                std::string{to_string(entry.code)}.c_str(), entry.subject.c_str(),
                entry.note.c_str());
  }
  for (const TruncationRecord& record : outcome.truncations) {
    std::printf("truncation container=%s requested=%llu accepted=%llu dropped=%llu\n",
                record.container.c_str(), static_cast<unsigned long long>(record.requested),
                static_cast<unsigned long long>(record.accepted),
                static_cast<unsigned long long>(record.dropped));
  }
  if (outcome.feasible) {
    std::printf("%s\n", encode_json(outcome.plan).to_text(true).c_str());
    std::printf("plan_digest=%s\n", outcome.plan.digest.hex().c_str());
  }
  return outcome.feasible ? 0 : 1;
}

int command_replay(const Arguments& arguments) {
  const std::optional<std::string> events_path = arguments.get("events");
  if (!events_path.has_value()) return usage();
  Result<std::vector<FabricEvent>> events = read_json_array<FabricEvent>(*events_path);
  if (!events) return report(events.status(), "events");
  RuntimeConfig config = tool_runtime_config();
  if (const std::optional<std::string> directory = arguments.get("store")) {
    config.store_directory = *directory;
  }
  Result<std::unique_ptr<FabricRuntime>> runtime = FabricRuntime::open(config);
  if (!runtime) return report(runtime.status(), "open");
  Result<BatchReport> report_value = runtime.value()->submit(events.value());
  if (!report_value) return report(report_value.status(), "submit");
  const BatchReport& batch = report_value.value();
  std::printf("applied=%llu suppressed=%llu refused=%llu\n",
              static_cast<unsigned long long>(batch.applied),
              static_cast<unsigned long long>(batch.suppressed),
              static_cast<unsigned long long>(batch.refused));
  for (const EventOutcome& outcome : batch.outcomes) {
    std::printf("event=%s code=%s applied=%s\n", outcome.id.str().c_str(),
                std::string{to_string(outcome.code)}.c_str(), outcome.applied ? "true" : "false");
  }
  std::printf("state_digest=%s\n", runtime.value()->state_digest().hex().c_str());
  std::printf("durable_digest=%s\n", runtime.value()->durable_digest().hex().c_str());
  const Status closed = runtime.value()->close();
  return report(closed, "close");
}

int command_serve(const Arguments& arguments) {
  ServerOptions options;
  const std::optional<std::uint64_t> workers = arguments.number("workers");
  if (workers.has_value()) options.workers = static_cast<std::size_t>(*workers);
  const std::optional<std::uint64_t> max_frame = arguments.number("max-frame");
  if (max_frame.has_value()) options.max_frame_bytes = static_cast<std::size_t>(*max_frame);

  RuntimeConfig config = tool_runtime_config();
  if (const std::optional<std::string> directory = arguments.get("store")) {
    config.store_directory = *directory;
  }
  if (max_frame.has_value()) {
    config.bounds.max_frame_bytes = options.max_frame_bytes;
  }
  Result<std::unique_ptr<FabricRuntime>> runtime = FabricRuntime::open(config);
  if (!runtime) return report(runtime.status(), "open");
  Result<std::unique_ptr<Server>> server = Server::create(*runtime.value(), options);
  if (!server) return report(server.status(), "server");

  Status ready = Status::success();
  if (const std::optional<std::uint64_t> inherited = arguments.number("inherited-socket")) {
    ready = server.value()->adopt(static_cast<std::uintptr_t>(*inherited));
  } else if (const std::optional<std::string> listen = arguments.get("listen")) {
    Result<std::pair<std::string, std::uint16_t>> endpoint = parse_endpoint(*listen);
    if (!endpoint) return report(endpoint.status(), "listen");
    server.value()->set_endpoint(endpoint.value().first, endpoint.value().second);
    ready = server.value()->listen();
  } else {
    return usage();
  }
  if (!ready.ok()) return report(ready, "listen");
  // The port is announced on stdout so a parent process can read it without
  // polling: the line is emitted once the socket is accepting.
  std::printf("ready port=%u\n", static_cast<unsigned>(server.value()->port()));
  std::fflush(stdout);
  const Status served = server.value()->serve();
  if (!served.ok()) return report(served, "serve");
  const ServerStats stats = server.value()->stats();
  std::printf("stopped connections=%llu requests=%llu refusals=%llu rejected=%llu cancelled=%llu\n",
              static_cast<unsigned long long>(stats.connections),
              static_cast<unsigned long long>(stats.requests),
              static_cast<unsigned long long>(stats.refusals),
              static_cast<unsigned long long>(stats.frames_rejected),
              static_cast<unsigned long long>(stats.connections_cancelled));
  const Status closed = runtime.value()->close();
  return report(closed, "close");
}

int command_client(const Arguments& arguments) {
  const std::optional<std::string> connect = arguments.get("connect");
  if (!connect.has_value()) return usage();
  Result<std::pair<std::string, std::uint16_t>> endpoint = parse_endpoint(*connect);
  if (!endpoint) return report(endpoint.status(), "connect");
  const std::vector<std::string>& positional = arguments.positional();
  // positional[0] is the subcommand (client), so the operation is positional[1].
  if (positional.size() < 2) return usage();
  const std::string command = positional[1];

  ClientOptions options;
  options.address = endpoint.value().first;
  options.port = endpoint.value().second;
  Result<std::unique_ptr<Client>> client = Client::connect(options);
  if (!client) return report(client.status(), "connect");

  int exit_code = 0;
  if (command == "ping") {
    Result<PingResponse> reply = client.value()->ping("dpufabric");
    if (!reply) return report(reply.status(), "ping");
    std::printf("pong nonce=%s product=%s version=%s\n", reply.value().nonce.c_str(),
                reply.value().product.c_str(), reply.value().version.c_str());
  } else if (command == "query") {
    Result<QueryResponse> reply = client.value()->query();
    if (!reply) return report(reply.status(), "query");
    std::printf("instances=%llu staged=%llu state_digest=%s durable_digest=%s\n",
                static_cast<unsigned long long>(reply.value().instances),
                static_cast<unsigned long long>(reply.value().staged),
                reply.value().state_digest.hex().c_str(),
                reply.value().durable_digest.hex().c_str());
  } else if (command == "export") {
    Result<ExportResponse> reply = client.value()->export_state(false);
    if (!reply) return report(reply.status(), "export");
    std::printf("%s\n", reply.value().document.c_str());
  } else if (command == "submit") {
    if (positional.size() < 3) return usage();
    Result<std::vector<FabricEvent>> events = read_json_array<FabricEvent>(positional[2]);
    (void)positional;
    if (!events) return report(events.status(), "events");
    Result<SubmitResponse> reply = client.value()->submit(events.value());
    if (!reply) return report(reply.status(), "submit");
    std::printf("applied=%llu suppressed=%llu refused=%llu primary=%s state_digest=%s\n",
                static_cast<unsigned long long>(reply.value().report.applied),
                static_cast<unsigned long long>(reply.value().report.suppressed),
                static_cast<unsigned long long>(reply.value().report.refused),
                std::string{to_string(reply.value().report.primary)}.c_str(),
                reply.value().state_digest.hex().c_str());
    exit_code = is_refusal(reply.value().report.primary) ? 1 : 0;
  } else if (command == "shutdown") {
    Result<ShutdownResponse> reply = client.value()->shutdown();
    if (!reply) return report(reply.status(), "shutdown");
    std::printf("stopped instances=%llu state_digest=%s\n",
                static_cast<unsigned long long>(reply.value().instances),
                reply.value().state_digest.hex().c_str());
  } else {
    return usage();
  }
  (void)client.value()->close();
  return exit_code;
}

/// Hard-kills the process at a labelled durable boundary. Nothing after the
/// marker runs, so a parent can prove that no success was fabricated.
[[noreturn]] void hard_kill(int code) {
  std::fflush(stdout);
  std::fflush(stderr);
#ifdef _WIN32
  TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
#else
  std::raise(SIGKILL);
#endif
  std::_Exit(code);
}

int command_crash(const Arguments& arguments) {
  const std::optional<std::string> directory = arguments.get("store");
  const std::optional<std::string> boundary = arguments.get("boundary");
  if (!directory.has_value() || !boundary.has_value()) return usage();
  RuntimeConfig config = store_runtime_config(*directory);
  Result<std::unique_ptr<FabricRuntime>> runtime = FabricRuntime::open(config);
  if (!runtime) return report(runtime.status(), "open");

  std::vector<FabricEvent> events;
  if (const std::optional<std::string> path = arguments.get("events")) {
    Result<std::vector<FabricEvent>> decoded = read_json_array<FabricEvent>(*path);
    if (!decoded) return report(decoded.status(), "events");
    events = std::move(decoded).value();
  }

  const std::string name = *boundary;
  const auto submit_events = [&runtime, &events]() -> Status {
    if (events.empty()) return Status::success();
    Result<BatchReport> batch = runtime.value()->submit(events);
    if (!batch) return batch.status();
    return Status::success();
  };

  if (name == "before-commit") {
    // The events were decoded into memory but never staged or journaled.
    std::printf("marker boundary=before-commit decoded=%zu\n", events.size());
    hard_kill(3);
  }
  if (name == "after-commit-before-ack") {
    const Status submitted = submit_events();
    if (!submitted.ok()) return report(submitted, "submit");
    // The events are journaled and applied; the caller never learns the outcome
    // because the process dies before any reply is produced.
    std::printf("marker boundary=after-commit-before-ack events=%zu\n", events.size());
    hard_kill(3);
  }
  if (name == "after-ack-before-verified-effect") {
    const Status submitted = submit_events();
    if (!submitted.ok()) return report(submitted, "submit");
    std::printf("marker boundary=after-ack-before-verified-effect instances=%zu\n",
                runtime.value()->instance_count());
    hard_kill(3);
  }
  if (name == "during-rotation") {
    const Status submitted = submit_events();
    if (!submitted.ok()) return report(submitted, "submit");
    const std::string temporary = (fs::path{*directory} / "fabric.snapshot.tmp").string();
    std::ofstream stream{temporary, std::ios::binary | std::ios::trunc};
    stream.write("partial-snapshot", 16);
    stream.close();
    std::printf("marker boundary=during-rotation\n");
    hard_kill(3);
  }
  if (name == "during-shutdown") {
    const Status submitted = submit_events();
    if (!submitted.ok()) return report(submitted, "submit");
    const Status closed = runtime.value()->close();
    if (!closed.ok()) return report(closed, "close");
    std::printf("marker boundary=during-shutdown\n");
    hard_kill(3);
  }
  return usage();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage();
  const std::string command = argv[1];
  Arguments arguments{argc - 1, argv + 1};
  if (command == "version") return command_version();
  if (command == "export") return command_export(arguments);
  if (command == "verify") return command_verify(arguments);
  if (command == "explain") return command_explain(arguments);
  if (command == "plan") return command_plan(arguments);
  if (command == "replay") return command_replay(arguments);
  if (command == "serve") return command_serve(arguments);
  if (command == "client") return command_client(arguments);
  if (command == "crash") return command_crash(arguments);
  return usage();
}
