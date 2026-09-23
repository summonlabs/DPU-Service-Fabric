// Unit tests for the foundation: reason codes, checked arithmetic, digests,
// canonical archives, strict JSON and bounded accounting.

#include <limits>
#include <string>
#include <vector>

#include "dpu/fabric/dpu_fabric.hpp"
#include "support/harness.hpp"
#include "support/scenario.hpp"

namespace dpu::fabric {
namespace {

using test::describe;

DPUF_TEST(reason, codes_are_stable_and_named) {
  DPUF_CHECK_EQ(std::string{to_string(ReasonCode::Ok)}, std::string{"Ok"});
  DPUF_CHECK_EQ(static_cast<int>(ReasonCode::DpuLost), 512);
  DPUF_CHECK_EQ(static_cast<int>(ReasonCode::ExclusiveScopeConflict), 110);
  DPUF_CHECK_EQ(static_cast<int>(ReasonCode::EvidenceStale), 210);
  DPUF_CHECK_EQ(static_cast<int>(ReasonCode::RollbackUnavailable), 407);
  ReasonCode parsed = ReasonCode::MalformedInput;
  DPUF_CHECK(from_string("FenceMismatch", parsed));
  DPUF_CHECK_EQ(parsed, ReasonCode::FenceMismatch);
  DPUF_CHECK(!from_string("NotAReason", parsed));
  DPUF_CHECK_EQ(std::string{to_string(static_cast<ReasonCode>(65000))}, std::string{"Unknown"});
  DPUF_CHECK_EQ(std::string{reason_band(ReasonCode::FenceMismatch)}, std::string{"authority"});
  DPUF_CHECK(is_refusal(ReasonCode::EvidenceStale));
  DPUF_CHECK(!is_refusal(ReasonCode::DuplicateSuppressed));
  DPUF_CHECK(!is_refusal(ReasonCode::DpuLost));
}

DPUF_TEST(reason, every_code_round_trips_through_its_name) {
  for (std::uint32_t value = 0; value < 1000; ++value) {
    const auto code = static_cast<ReasonCode>(value);
    const std::string name{to_string(code)};
    if (name == "Unknown") continue;
    ReasonCode parsed = ReasonCode::Ok;
    DPUF_CHECK(from_string(name, parsed));
    DPUF_CHECK_EQ(parsed, code);
  }
}

DPUF_TEST(checked, arithmetic_detects_overflow_and_underflow) {
  std::uint64_t out = 0;
  DPUF_CHECK(checked_add(std::uint64_t{2}, std::uint64_t{3}, out));
  DPUF_CHECK_EQ(out, std::uint64_t{5});
  DPUF_CHECK(!checked_add(std::numeric_limits<std::uint64_t>::max(), std::uint64_t{1}, out));
  DPUF_CHECK(!checked_sub(std::uint64_t{1}, std::uint64_t{2}, out));
  DPUF_CHECK(checked_mul(std::uint64_t{1000}, std::uint64_t{1000}, out));
  DPUF_CHECK_EQ(out, std::uint64_t{1000000});
  DPUF_CHECK(!checked_mul(std::numeric_limits<std::uint64_t>::max(), std::uint64_t{2}, out));
  std::int64_t signed_out = 0;
  DPUF_CHECK(checked_add(std::int64_t{-5}, std::int64_t{3}, signed_out));
  DPUF_CHECK_EQ(signed_out, std::int64_t{-2});
  DPUF_CHECK(!checked_add(std::numeric_limits<std::int64_t>::max(), std::int64_t{1}, signed_out));
  DPUF_CHECK(!checked_mul(std::numeric_limits<std::int64_t>::min(), std::int64_t{-1}, signed_out));
  std::uint8_t narrowed = 0;
  DPUF_CHECK(checked_narrow(std::uint64_t{255}, narrowed));
  DPUF_CHECK(!checked_narrow(std::uint64_t{256}, narrowed));
  DPUF_CHECK_EQ(saturating_add(std::uint64_t{5}, std::uint64_t{7}), std::uint64_t{12});
  DPUF_CHECK_EQ(saturating_add(std::numeric_limits<std::uint64_t>::max(), std::uint64_t{1}),
                std::numeric_limits<std::uint64_t>::max());
}

DPUF_TEST(digest, matches_published_sha256_vectors) {
  DPUF_CHECK_EQ(sha256(std::string{""}).hex(),
                std::string{"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"});
  DPUF_CHECK_EQ(sha256(std::string{"abc"}).hex(),
                std::string{"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"});
  DPUF_CHECK_EQ(
      sha256(std::string{"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"}).hex(),
      std::string{"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"});
  // Streaming in uneven chunks must produce the same digest as one shot.
  Sha256 hasher;
  hasher.update(std::string{"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"}.substr(0, 7));
  hasher.update(std::string{"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"}.substr(7));
  DPUF_CHECK_EQ(hasher.finish().hex(),
                std::string{"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"});
  DPUF_CHECK_EQ(sha256(std::string{"abc"}).short_hex(), std::string{"ba7816bf8f01cfea"});
  const Result<Digest> parsed = Digest::parse_hex(sha256(std::string{"abc"}).hex());
  DPUF_CHECK(parsed.has_value());
  DPUF_CHECK_EQ(parsed.value(), sha256(std::string{"abc"}));
  DPUF_CHECK(!Digest::parse_hex("nothex").has_value());
  DPUF_CHECK(!Digest::parse_hex(std::string(64, 'z')).has_value());
  DPUF_CHECK(Digest{}.is_zero());
}

DPUF_TEST(json, refuses_malformed_and_unsupported_documents) {
  DPUF_CHECK(parse_json("{}").has_value());
  DPUF_CHECK(parse_json("{\"a\":1}").has_value());
  DPUF_CHECK(parse_json("[]").has_value());
  DPUF_CHECK(parse_json("\"text\"").has_value());
  DPUF_CHECK(parse_json("null").has_value());
  DPUF_CHECK(!parse_json("{").has_value());
  DPUF_CHECK(!parse_json("{\"a\":1,}").has_value());
  DPUF_CHECK(!parse_json("{\"a\":1}{").has_value());
  DPUF_CHECK(!parse_json("{\"a\":1.5}").has_value());
  DPUF_CHECK(!parse_json("{\"a\":1e3}").has_value());
  DPUF_CHECK(!parse_json("{\"a\":1,\"a\":2}").has_value());
  DPUF_CHECK(!parse_json("{\"a\":}").has_value());
  DPUF_CHECK(!parse_json("").has_value());
  DPUF_CHECK(!parse_json("[1,2").has_value());
  DPUF_CHECK(!parse_json("\"unterminated").has_value());
  DPUF_CHECK(!parse_json(std::string{"[1,2,3]\n\0", 9}).has_value());
  const Result<JsonValue> trailing = parse_json("[1] x");
  DPUF_CHECK(!trailing.has_value());
  DPUF_CHECK_CODE(trailing.status(), ReasonCode::MalformedInput);
  const Result<JsonValue> huge = parse_json(std::string(4096, '1'), 128);
  DPUF_CHECK(!huge.has_value());
  DPUF_CHECK_CODE(huge.status(), ReasonCode::OversizedInput);
  // Deep nesting is refused before it can exhaust the stack.
  std::string deep;
  for (int i = 0; i < 200; ++i) deep.push_back('[');
  for (int i = 0; i < 200; ++i) deep.push_back(']');
  const Result<JsonValue> nested = parse_json(deep);
  DPUF_CHECK(!nested.has_value());
  DPUF_CHECK_CODE(nested.status(), ReasonCode::DepthLimitExceeded);
}

DPUF_TEST(json, round_trips_strings_and_numbers) {
  const std::string text = "{\"a\":1,\"b\":\"x\\ny\",\"c\":[true,false,null],\"d\":-7}";
  const Result<JsonValue> parsed = parse_json(text);
  DPUF_CHECK(parsed.has_value());
  const JsonValue* a = parsed.value().find("a");
  DPUF_CHECK(a != nullptr);
  DPUF_CHECK_EQ(a->as_int(), std::int64_t{1});
  DPUF_CHECK_EQ(parsed.value().find("b")->as_string(), std::string{"x\ny"});
  DPUF_CHECK_EQ(parsed.value().find("c")->elements().size(), std::size_t{3});
  DPUF_CHECK_EQ(parsed.value().find("d")->as_int(), std::int64_t{-7});
  const Result<JsonValue> again = parse_json(parsed.value().to_text(false));
  DPUF_CHECK(again.has_value());
  DPUF_CHECK_EQ(again.value().to_text(false), parsed.value().to_text(false));
  const Result<JsonValue> unicode = parse_json("\"\\u00e9\\ud83d\\ude00\"");
  DPUF_CHECK(unicode.has_value());
  DPUF_CHECK_EQ(unicode.value().as_string().size(), std::size_t{6});
  DPUF_CHECK(!parse_json("\"\\ud83d\"").has_value());
  DPUF_CHECK_EQ(json_escape("a\"b"), std::string{"\"a\\\"b\""});
  // Integers wider than 64 bits are refused rather than truncated.
  DPUF_CHECK(!parse_json("99999999999999999999999").has_value());
}

DPUF_TEST(archive, binary_round_trip_preserves_every_field) {
  PlacementIntent intent = test::standard_intent(10, 3);
  intent.rollback_target = DeploymentGeneration{4};
  const std::vector<std::uint8_t> encoded = encode_binary(intent);
  const Result<PlacementIntent> decoded = decode_binary<PlacementIntent>(encoded);
  DPUF_CHECK(decoded.has_value());
  DPUF_CHECK_EQ(decoded.value().id.str(), intent.id.str());
  DPUF_CHECK_EQ(decoded.value().services.size(), std::size_t{1});
  DPUF_CHECK_EQ(decoded.value().services[0].desired_replicas, std::uint32_t{3});
  DPUF_CHECK(decoded.value().rollback_target.has_value());
  DPUF_CHECK_EQ(decoded.value().rollback_target->value(), std::uint64_t{4});
  DPUF_CHECK_EQ(canonical_digest(decoded.value()), canonical_digest(intent));
  // Encoding is stable across runs.
  DPUF_CHECK_EQ(encode_binary(intent), encoded);
}

DPUF_TEST(archive, json_round_trip_is_name_based_and_strict) {
  ServiceDefinition service = test::make_service("svc-a", 1, 3, test::capacity(500, 900));
  service.compatibility.capabilities.push_back(
      test::requirement("crypto.aes", CapabilityOp::Present));
  const JsonValue json = encode_json(service);
  const Result<ServiceDefinition> decoded = decode_json<ServiceDefinition>(json);
  DPUF_CHECK(decoded.has_value());
  DPUF_CHECK_EQ(canonical_digest(decoded.value()), canonical_digest(service));

  JsonValue mutated = json;
  JsonValue object = JsonValue::make_object();
  for (const auto& member : mutated.members()) object.set(member.first, member.second);
  object.set("unexpected", JsonValue{std::int64_t{1}});
  const Result<ServiceDefinition> unknown = decode_json<ServiceDefinition>(object);
  DPUF_CHECK(!unknown.has_value());
  DPUF_CHECK_CODE(unknown.status(), ReasonCode::UnknownField);

  JsonValue missing = JsonValue::make_object();
  for (const auto& member : json.members()) {
    if (member.first != "resources") missing.set(member.first, member.second);
  }
  const Result<ServiceDefinition> incomplete = decode_json<ServiceDefinition>(missing);
  DPUF_CHECK(!incomplete.has_value());
  DPUF_CHECK_CODE(incomplete.status(), ReasonCode::MissingField);
}

DPUF_TEST(archive, corrupt_binary_streams_are_refused) {
  ServiceDefinition service = test::make_service("svc-a", 1, 3, test::capacity(500, 900));
  const std::vector<std::uint8_t> encoded = encode_binary(service);
  DPUF_CHECK(!decode_binary<ServiceDefinition>(
                  std::span<const std::uint8_t>(encoded.data(), encoded.size() / 2))
                  .has_value());
  DPUF_CHECK(!decode_binary<ServiceDefinition>(std::span<const std::uint8_t>{}).has_value());
  std::vector<std::uint8_t> trailing = encoded;
  trailing.push_back(0xAB);
  const Result<ServiceDefinition> extra = decode_binary<ServiceDefinition>(trailing);
  DPUF_CHECK(!extra.has_value());
  DPUF_CHECK_CODE(extra.status(), ReasonCode::MalformedInput);

  // Flip a byte inside the enum name to prove enumeration values are validated
  // by name rather than by ordinal.
  std::vector<std::uint8_t> flipped = encoded;
  bool flipped_something = false;
  for (std::size_t i = 0; i + 3 < flipped.size(); ++i) {
    if (flipped[i] == 'A' && flipped[i + 1] == 'a' && flipped[i + 2] == 'r') {
      flipped[i] = 'X';
      flipped_something = true;
      break;
    }
  }
  if (flipped_something) {
    const Result<ServiceDefinition> broken = decode_binary<ServiceDefinition>(flipped);
    DPUF_CHECK(!broken.has_value());
  }
  // A declared length far beyond the buffer is refused before allocating.
  std::vector<std::uint8_t> oversized = encoded;
  for (std::size_t i = 0; i < oversized.size(); ++i) {
    if (oversized[i] == static_cast<std::uint8_t>(WireTag::Str)) {
      oversized[i + 1] = 0xFF;
      oversized[i + 2] = 0xFF;
      oversized[i + 3] = 0xFF;
      oversized[i + 4] = 0x7F;
      break;
    }
  }
  DPUF_CHECK(!decode_binary<ServiceDefinition>(oversized).has_value());
}

DPUF_TEST(archive, variant_and_optional_edges) {
  FabricEvent event =
      test::make_event("ev-1", "origin-1", 1, 5, IntentSubmitted{test::standard_intent(5)});
  std::vector<std::uint8_t> encoded = encode_binary(event);
  const Result<FabricEvent> decoded = decode_binary<FabricEvent>(encoded);
  DPUF_CHECK(decoded.has_value());
  DPUF_CHECK_EQ(canonical_digest(decoded.value()), canonical_digest(event));
  DPUF_CHECK(std::holds_alternative<IntentSubmitted>(decoded.value().body));

  // An out-of-range variant index is refused, never clamped.
  std::vector<std::uint8_t> corrupted = encoded;
  const std::string needle = "index";
  bool patched = false;
  for (std::size_t i = 0; i + needle.size() < corrupted.size(); ++i) {
    if (std::string(corrupted.begin() + static_cast<std::ptrdiff_t>(i),
                    corrupted.begin() + static_cast<std::ptrdiff_t>(i + needle.size())) == needle) {
      corrupted[i + needle.size() + 2] = 0x7F;
      patched = true;
      break;
    }
  }
  if (patched) {
    DPUF_CHECK(!decode_binary<FabricEvent>(corrupted).has_value());
  }
}

DPUF_TEST(bounded, ledger_accounts_for_every_drop) {
  TruncationLedger ledger;
  DPUF_CHECK(ledger.empty());
  DPUF_CHECK_OK(ledger.record("history", 10, 6, ReasonCode::HistoryTruncated));
  DPUF_CHECK_OK(ledger.record("history", 4, 2, ReasonCode::HistoryTruncated));
  DPUF_CHECK_EQ(ledger.records().size(), std::size_t{1});
  DPUF_CHECK_EQ(ledger.records()[0].requested, std::uint64_t{14});
  DPUF_CHECK_EQ(ledger.records()[0].accepted, std::uint64_t{8});
  DPUF_CHECK_EQ(ledger.records()[0].dropped, std::uint64_t{6});
  DPUF_CHECK_EQ(ledger.total_requested(), ledger.total_accepted() + ledger.total_dropped());
  DPUF_CHECK_CODE(ledger.record("bad", 1, 2, ReasonCode::HistoryTruncated),
                  ReasonCode::InvalidAccounting);
  for (std::size_t i = 0; i < 200; ++i) {
    (void)ledger.record("container-" + std::to_string(i), 1, 0, ReasonCode::EvictionOccurred);
  }
  DPUF_CHECK(ledger.records().size() <= TruncationLedger::kMaxRecords);
  DPUF_CHECK(ledger.ledger_overflow() > 0);
  const std::vector<std::uint8_t> encoded = encode_binary(ledger);
  const Result<TruncationLedger> restored = decode_binary<TruncationLedger>(encoded);
  DPUF_CHECK(restored.has_value());
  DPUF_CHECK_EQ(restored.value().total_dropped(), ledger.total_dropped());
}

DPUF_TEST(bounded, history_evicts_oldest_and_counts_it) {
  BoundedHistory<int> history{3};
  DPUF_CHECK_EQ(history.push(1), std::size_t{0});
  DPUF_CHECK_EQ(history.push(2), std::size_t{0});
  DPUF_CHECK_EQ(history.push(3), std::size_t{0});
  DPUF_CHECK_EQ(history.push(4), std::size_t{1});
  DPUF_CHECK_EQ(history.size(), std::size_t{3});
  DPUF_CHECK_EQ(history.entries().front(), 1 + 1);
  DPUF_CHECK_EQ(history.evicted(), std::uint64_t{1});
  BoundedHistory<int> zero{0};
  DPUF_CHECK_EQ(zero.push(1), std::size_t{0});
  DPUF_CHECK_EQ(zero.rejected(), std::uint64_t{1});
  DPUF_CHECK(zero.empty());
}

DPUF_TEST(resources, vector_arithmetic_is_checked) {
  const ResourceVector a = test::capacity(100, 200);
  const ResourceVector b = test::capacity(40, 80);
  DPUF_CHECK(a.covers(b));
  DPUF_CHECK(!b.covers(a));
  const Result<ResourceVector> sum = ResourceVector::add(a, b);
  DPUF_CHECK(sum.has_value());
  DPUF_CHECK_EQ(sum.value().cpu_millicores, std::uint64_t{140});
  const Result<ResourceVector> difference = ResourceVector::subtract(a, b);
  DPUF_CHECK(difference.has_value());
  DPUF_CHECK_EQ(difference.value().cpu_millicores, std::uint64_t{60});
  DPUF_CHECK(!ResourceVector::subtract(b, a).has_value());
  const Result<std::uint32_t> permille = a.headroom_permille(b);
  DPUF_CHECK(permille.has_value());
  DPUF_CHECK_EQ(permille.value(), std::uint32_t{600});
  const Result<std::uint32_t> none = a.headroom_permille(ResourceVector{});
  DPUF_CHECK(!none.has_value());
  ResourceVector huge;
  huge.cpu_millicores = std::numeric_limits<std::uint64_t>::max();
  DPUF_CHECK(!ResourceVector::add(huge, huge).has_value());
  DPUF_CHECK(!ResourceVector::scale(huge, 2).has_value());
}

DPUF_TEST(bounds, validation_rejects_impossible_configuration) {
  RuntimeBounds bounds;
  DPUF_CHECK_OK(bounds.validate());
  bounds.max_workers = 0;
  DPUF_CHECK_CODE(bounds.validate(), ReasonCode::ValueOutOfRange);
  bounds = RuntimeBounds{};
  bounds.max_dpus = RuntimeBounds::kMaxDpusCeiling + 1;
  DPUF_CHECK_CODE(bounds.validate(), ReasonCode::BoundExceeded);
  bounds = RuntimeBounds{};
  bounds.evidence_validity_ticks = 0;
  DPUF_CHECK_CODE(bounds.validate(), ReasonCode::ValueOutOfRange);
  bounds = RuntimeBounds{};
  bounds.max_frame_bytes = RuntimeBounds::kMaxFrameBytesCeiling + 1;
  DPUF_CHECK_CODE(bounds.validate(), ReasonCode::BoundExceeded);
}

}  // namespace
}  // namespace dpu::fabric
