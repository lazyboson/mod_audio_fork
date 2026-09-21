#include <gtest/gtest.h>

#include <numeric>
#include <string>
#include <vector>

#include "audiofork_net/ws_client.hpp"
#include "test_ws_server.hpp"
#include "ws_test_harness.hpp"

namespace audiofork::net {
namespace {

TestWsTls ServerTls(const std::string& name) {
  return TestWsTls{TestTlsPath(name + ".pem"), TestTlsPath(name + ".key"), ""};
}

TlsOptions ClientTls(const std::string& ca) {
  TlsOptions tls;
  tls.ca_file = TestTlsPath(ca);
  return tls;
}

struct EchoingHandler : RecordingHandler {
  WsConnection* self = nullptr;
  std::string text_to_send;
  void OnConnected() override {
    RecordingHandler::OnConnected();
    EXPECT_TRUE(self->SendText(text_to_send));
  }
};

TEST(WsTls, TrustedServerHandshakesAndEchoes) {
  auto server = TestWsServer::Start(ServerTls("server"));
  ASSERT_NE(server, nullptr);

  EchoingHandler handler;
  handler.text_to_send = R"({"type":"hello"})";
  LoopRunner runner(ClientTls("ca.pem"));
  runner.loop->Post([&] {
    handler.self =
        runner.loop->Connect({"127.0.0.1", server->port(), "/", true}, handler, kDefaultQueueCap);
    ASSERT_NE(handler.self, nullptr);
  });

  ASSERT_TRUE(handler.WaitFor([&] { return handler.texts.size() == 1; }));
  EXPECT_EQ(handler.texts[0], R"({"type":"hello"})");
  EXPECT_TRUE(handler.connected);
}

TEST(WsTls, BinaryAudioSurvivesTheTlsRecordLayer) {
  auto server = TestWsServer::Start(ServerTls("server"));
  ASSERT_NE(server, nullptr);

  std::vector<std::uint8_t> pcm(std::size_t{64} * 1024);
  for (std::size_t i = 0; i < pcm.size(); ++i) {
    pcm[i] = static_cast<std::uint8_t>(i % 251);
  }

  struct SendingHandler : RecordingHandler {
    WsConnection* self = nullptr;
    std::vector<std::uint8_t> payload;
    void OnConnected() override {
      RecordingHandler::OnConnected();
      EXPECT_TRUE(self->SendBinary(ConstByteSpan(payload)));
    }
  };
  SendingHandler handler;
  handler.payload = pcm;
  LoopRunner runner(ClientTls("ca.pem"));
  runner.loop->Post([&] {
    handler.self =
        runner.loop->Connect({"127.0.0.1", server->port(), "/", true}, handler, kDefaultQueueCap);
    ASSERT_NE(handler.self, nullptr);
  });

  ASSERT_TRUE(handler.WaitFor([&] { return handler.binaries.size() == 1; }));
  EXPECT_EQ(handler.binaries[0], pcm);
}

TEST(WsTls, UntrustedIssuerFailsAsAConnectFailure) {
  auto server = TestWsServer::Start(ServerTls("server"));
  ASSERT_NE(server, nullptr);

  RecordingHandler handler;
  LoopRunner runner(ClientTls("other-ca.pem"));
  runner.loop->Post([&] {
    (void)runner.loop->Connect({"127.0.0.1", server->port(), "/", true}, handler, kDefaultQueueCap);
  });

  ASSERT_TRUE(handler.WaitFor([&] { return handler.closed; }));
  EXPECT_TRUE(handler.connect_failed);
  EXPECT_FALSE(handler.connected);
}

TEST(WsTls, VerifyOffAcceptsAnUntrustedIssuer) {
  auto server = TestWsServer::Start(ServerTls("server"));
  ASSERT_NE(server, nullptr);

  TlsOptions tls = ClientTls("other-ca.pem");
  tls.verify = false;
  RecordingHandler handler;
  LoopRunner runner(tls);
  runner.loop->Post([&] {
    (void)runner.loop->Connect({"127.0.0.1", server->port(), "/", true}, handler, kDefaultQueueCap);
  });

  ASSERT_TRUE(handler.WaitFor([&] { return handler.connected; }));
  EXPECT_FALSE(handler.connect_failed);
}

TEST(WsTls, HostnameMismatchFailsWithVerifyOn) {
  auto server = TestWsServer::Start(ServerTls("server-othername"));
  ASSERT_NE(server, nullptr);

  RecordingHandler handler;
  LoopRunner runner(ClientTls("ca.pem"));
  runner.loop->Post([&] {
    (void)runner.loop->Connect({"127.0.0.1", server->port(), "/", true}, handler, kDefaultQueueCap);
  });

  ASSERT_TRUE(handler.WaitFor([&] { return handler.closed; }));
  EXPECT_TRUE(handler.connect_failed);
  EXPECT_FALSE(handler.connected);
}

TEST(WsTls, HostnameMismatchIsAcceptedWithVerifyOff) {
  auto server = TestWsServer::Start(ServerTls("server-othername"));
  ASSERT_NE(server, nullptr);

  TlsOptions tls = ClientTls("ca.pem");
  tls.verify = false;
  RecordingHandler handler;
  LoopRunner runner(tls);
  runner.loop->Post([&] {
    (void)runner.loop->Connect({"127.0.0.1", server->port(), "/", true}, handler, kDefaultQueueCap);
  });

  ASSERT_TRUE(handler.WaitFor([&] { return handler.connected; }));
}

TEST(WsTls, MutualTlsConnectsWhenTheClientCertificateIsPresented) {
  TestWsTls server_tls = ServerTls("server");
  server_tls.client_ca_file = TestTlsPath("ca.pem");
  auto server = TestWsServer::Start(server_tls);
  ASSERT_NE(server, nullptr);

  TlsOptions tls = ClientTls("ca.pem");
  tls.cert_file = TestTlsPath("client.pem");
  tls.key_file = TestTlsPath("client.key");

  EchoingHandler handler;
  handler.text_to_send = R"({"type":"hello"})";
  LoopRunner runner(tls);
  runner.loop->Post([&] {
    handler.self =
        runner.loop->Connect({"127.0.0.1", server->port(), "/", true}, handler, kDefaultQueueCap);
    ASSERT_NE(handler.self, nullptr);
  });

  ASSERT_TRUE(handler.WaitFor([&] { return handler.texts.size() == 1; }));
  EXPECT_EQ(handler.texts[0], R"({"type":"hello"})");
}

TEST(WsTls, MutualTlsFailsWithoutAClientCertificate) {
  TestWsTls server_tls = ServerTls("server");
  server_tls.client_ca_file = TestTlsPath("ca.pem");
  auto server = TestWsServer::Start(server_tls);
  ASSERT_NE(server, nullptr);

  RecordingHandler handler;
  LoopRunner runner(ClientTls("ca.pem"));
  runner.loop->Post([&] {
    (void)runner.loop->Connect({"127.0.0.1", server->port(), "/", true}, handler, kDefaultQueueCap);
  });

  ASSERT_TRUE(handler.WaitFor([&] { return handler.closed; }));
  EXPECT_TRUE(handler.connect_failed);
  EXPECT_FALSE(handler.connected);
}

TEST(WsTls, PlaintextClientAgainstATlsServerFailsToConnect) {
  auto server = TestWsServer::Start(ServerTls("server"));
  ASSERT_NE(server, nullptr);

  RecordingHandler handler;
  LoopRunner runner(ClientTls("ca.pem"));
  runner.loop->Post([&] {
    (void)runner.loop->Connect({"127.0.0.1", server->port(), "/", false}, handler,
                               kDefaultQueueCap);
  });

  ASSERT_TRUE(handler.WaitFor([&] { return handler.closed; }));
  EXPECT_TRUE(handler.connect_failed);
  EXPECT_FALSE(handler.connected);
}

}  // namespace
}  // namespace audiofork::net
