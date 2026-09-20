#include "audiofork/config.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace audiofork {
namespace {

using std::chrono::milliseconds;

TEST(AudioFormat, ByteMath) {
  const AudioFormat stereo_16k{16000, 2};
  EXPECT_EQ(stereo_16k.BytesPerSecond(), 64000U);
  EXPECT_EQ(stereo_16k.BytesForDuration(milliseconds{20}), 1280U);
  EXPECT_EQ(stereo_16k.BytesForDuration(milliseconds{10000}), 640000U);

  const AudioFormat mono_8k{8000, 1};
  EXPECT_EQ(mono_8k.BytesPerSecond(), 16000U);
  EXPECT_EQ(mono_8k.BytesForDuration(milliseconds{20}), 320U);
}

TEST(SanitizeConfig, DefaultsPassThroughUnchanged) {
  const ModuleConfig sanitized = SanitizeConfig(ModuleConfig{});
  EXPECT_EQ(sanitized.send_buffer, milliseconds{10000});
  EXPECT_EQ(sanitized.handoff_buffer, milliseconds{1000});
  EXPECT_EQ(sanitized.coalesce_max, milliseconds{100});
  EXPECT_EQ(sanitized.reconnect_min, milliseconds{250});
  EXPECT_EQ(sanitized.reconnect_max, milliseconds{5000});
  EXPECT_EQ(sanitized.max_forks_per_call, 4U);
}

TEST(SanitizeConfig, ClampsNonsenseInsteadOfFailing) {
  ModuleConfig config;
  config.shard_count = 999;
  config.send_buffer = milliseconds{0};
  config.handoff_buffer = milliseconds{999999};
  config.coalesce_max = milliseconds{0};
  config.reconnect_min = milliseconds{0};
  config.reconnect_max = milliseconds{1};
  config.slab_size_bytes = 7;
  config.global_memory_cap_bytes = 1;
  config.max_forks_per_call = 0;

  const ModuleConfig sanitized = SanitizeConfig(config);
  EXPECT_LE(sanitized.shard_count, 16U);
  EXPECT_GE(sanitized.send_buffer, milliseconds{100});
  EXPECT_LE(sanitized.handoff_buffer, sanitized.send_buffer);
  EXPECT_LE(sanitized.coalesce_max, sanitized.handoff_buffer);
  EXPECT_GE(sanitized.reconnect_min, milliseconds{1});
  EXPECT_GE(sanitized.reconnect_max, sanitized.reconnect_min);
  EXPECT_GE(sanitized.slab_size_bytes, 4096U);
  EXPECT_GE(sanitized.global_memory_cap_bytes, sanitized.slab_size_bytes * 16);
  EXPECT_GE(sanitized.max_forks_per_call, 1U);
}

TEST(SanitizeConfig, HugeBufferRequestIsBounded) {
  ModuleConfig config;
  config.send_buffer = milliseconds{600000};
  const ModuleConfig sanitized = SanitizeConfig(config);
  EXPECT_LE(sanitized.send_buffer, milliseconds{60000});
}

TEST(SanitizeConfig, IsIdempotent) {
  ModuleConfig config;
  config.send_buffer = milliseconds{0};
  const ModuleConfig once = SanitizeConfig(config);
  const ModuleConfig twice = SanitizeConfig(once);
  EXPECT_EQ(once.send_buffer, twice.send_buffer);
  EXPECT_EQ(once.handoff_buffer, twice.handoff_buffer);
  EXPECT_EQ(once.coalesce_max, twice.coalesce_max);
  EXPECT_EQ(once.slab_size_bytes, twice.slab_size_bytes);
}

TEST(SanitizeConfig, TlsDefaultsToSystemStoreWithVerifyOn) {
  const ModuleConfig sanitized = SanitizeConfig(ModuleConfig{});
  EXPECT_TRUE(sanitized.tls.ca_file.empty());
  EXPECT_TRUE(sanitized.tls.cert_file.empty());
  EXPECT_TRUE(sanitized.tls.key_file.empty());
  EXPECT_TRUE(sanitized.tls.verify);
}

TEST(SanitizeConfig, ClientCertificateSurvivesWhenBothHalvesAreSet) {
  ModuleConfig config;
  config.tls.ca_file = "/etc/ssl/ca.pem";
  config.tls.cert_file = "/etc/ssl/client.pem";
  config.tls.key_file = "/etc/ssl/client.key";

  const ModuleConfig sanitized = SanitizeConfig(config);
  EXPECT_EQ(sanitized.tls.ca_file, "/etc/ssl/ca.pem");
  EXPECT_EQ(sanitized.tls.cert_file, "/etc/ssl/client.pem");
  EXPECT_EQ(sanitized.tls.key_file, "/etc/ssl/client.key");
}

TEST(SanitizeConfig, HalfAClientCertificateIsDroppedEitherWayRound) {
  ModuleConfig cert_only;
  cert_only.tls.cert_file = "/etc/ssl/client.pem";
  const ModuleConfig without_key = SanitizeConfig(cert_only);
  EXPECT_TRUE(without_key.tls.cert_file.empty());
  EXPECT_TRUE(without_key.tls.key_file.empty());

  ModuleConfig key_only;
  key_only.tls.key_file = "/etc/ssl/client.key";
  const ModuleConfig without_cert = SanitizeConfig(key_only);
  EXPECT_TRUE(without_cert.tls.cert_file.empty());
  EXPECT_TRUE(without_cert.tls.key_file.empty());
}

TEST(SanitizeConfig, DroppingHalfAClientCertificateLeavesTheCaAndVerifyAlone) {
  ModuleConfig config;
  config.tls.ca_file = "/etc/ssl/ca.pem";
  config.tls.cert_file = "/etc/ssl/client.pem";
  config.tls.verify = false;

  const ModuleConfig sanitized = SanitizeConfig(config);
  EXPECT_EQ(sanitized.tls.ca_file, "/etc/ssl/ca.pem");
  EXPECT_FALSE(sanitized.tls.verify);
}

Endpoint MustParse(std::string_view url) {
  std::string error;
  const std::optional<Endpoint> endpoint = ParseWsUrl(url, error);
  EXPECT_TRUE(endpoint.has_value()) << url << ": " << error;
  EXPECT_TRUE(error.empty()) << url;
  return endpoint.value_or(Endpoint{});
}

void ExpectRejected(std::string_view url) {
  std::string error;
  const std::optional<Endpoint> endpoint = ParseWsUrl(url, error);
  EXPECT_FALSE(endpoint.has_value()) << url;
  EXPECT_FALSE(error.empty()) << url;
}

TEST(ParseWsUrl, PlainSchemeDefaultsToPortEighty) {
  const Endpoint endpoint = MustParse("ws://example.com");
  EXPECT_EQ(endpoint.host, "example.com");
  EXPECT_EQ(endpoint.port, 80);
  EXPECT_EQ(endpoint.path, "/");
  EXPECT_FALSE(endpoint.tls);
}

TEST(ParseWsUrl, TlsSchemeDefaultsToPortFourFourThree) {
  const Endpoint endpoint = MustParse("wss://example.com/");
  EXPECT_EQ(endpoint.host, "example.com");
  EXPECT_EQ(endpoint.port, 443);
  EXPECT_EQ(endpoint.path, "/");
  EXPECT_TRUE(endpoint.tls);
}

TEST(ParseWsUrl, ExplicitPortWins) {
  const Endpoint plain = MustParse("ws://example.com:9099/stream");
  EXPECT_EQ(plain.port, 9099);
  EXPECT_EQ(plain.path, "/stream");

  const Endpoint secure = MustParse("wss://example.com:8443/stream");
  EXPECT_EQ(secure.port, 8443);
  EXPECT_TRUE(secure.tls);
}

TEST(ParseWsUrl, PortBoundariesAreAccepted) {
  EXPECT_EQ(MustParse("ws://h:1/").port, 1);
  EXPECT_EQ(MustParse("ws://h:65535/").port, 65535);
}

TEST(ParseWsUrl, PathKeepsItsQueryString) {
  const Endpoint endpoint = MustParse("wss://host/media?token=abc&v=1");
  EXPECT_EQ(endpoint.host, "host");
  EXPECT_EQ(endpoint.path, "/media?token=abc&v=1");
}

TEST(ParseWsUrl, Ipv6LiteralLosesItsBrackets) {
  const Endpoint with_port = MustParse("ws://[::1]:8080/x");
  EXPECT_EQ(with_port.host, "::1");
  EXPECT_EQ(with_port.port, 8080);
  EXPECT_EQ(with_port.path, "/x");

  const Endpoint without_port = MustParse("wss://[2001:db8::1]/");
  EXPECT_EQ(without_port.host, "2001:db8::1");
  EXPECT_EQ(without_port.port, 443);
}

TEST(ParseWsUrl, RejectsMissingHost) {
  ExpectRejected("ws://");
  ExpectRejected("wss:///path");
  ExpectRejected("ws://:9099/");
}

TEST(ParseWsUrl, RejectsUnknownScheme) {
  ExpectRejected("http://example.com/");
  ExpectRejected("https://example.com/");
  ExpectRejected("//example.com/");
  ExpectRejected("WS://example.com/");
}

TEST(ParseWsUrl, RejectsGarbage) {
  ExpectRejected("");
  ExpectRejected("example.com");
  ExpectRejected("ws:/example.com");
  ExpectRejected("\x01\x02\x03");
}

TEST(ParseWsUrl, RejectsUnusablePorts) {
  ExpectRejected("ws://host:0/");
  ExpectRejected("ws://host:65536/");
  ExpectRejected("ws://host:99999999/");
  ExpectRejected("ws://host:abc/");
  ExpectRejected("ws://host:-1/");
  ExpectRejected("ws://host:/");
  ExpectRejected("ws://host: 80/");
}

TEST(ParseWsUrl, RejectsMalformedIpv6Literals) {
  ExpectRejected("ws://[::1/x");
  ExpectRejected("ws://[]/x");
  ExpectRejected("ws://[::1]x/");
}

}  // namespace
}  // namespace audiofork
