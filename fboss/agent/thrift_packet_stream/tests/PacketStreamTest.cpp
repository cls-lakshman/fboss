// Copyright 2004-present Facebook.  All rights reserved.

#include <folly/Memory.h>
#include <folly/Random.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <thrift/lib/cpp2/util/ScopedServerInterfaceThread.h>
#include "fboss/agent/thrift_packet_stream/PacketStreamClient.h"
#include "fboss/agent/thrift_packet_stream/PacketStreamService.h"
#include "servicerouter/client/cpp2/ServiceRouter.h"
#include "servicerouter/tests/TestServer.h"
#include "servicerouter/tests/gen-cpp2/TestServerService.h"

using namespace testing;
using namespace facebook::fboss;

static const std::string g_client = "testClient";
static const std::unordered_set<std::string> g_ports = {"eth0", "eth1", "lo"};
static const std::string g_pktCnt = "Content";
class DerivedPacketStreamService : public PacketStreamService {
 public:
  explicit DerivedPacketStreamService(
      const std::string& serviceName,
      std::shared_ptr<folly::Baton<>> baton)
      : PacketStreamService(serviceName), baton_(baton) {}
  virtual ~DerivedPacketStreamService() override {}
  virtual void clientConnected(const std::string& clientId) override {
    EXPECT_EQ(clientId, g_client);
  }
  virtual void clientDisconnected(const std::string& clientId) override {
    EXPECT_EQ(clientId, g_client);
    if (baton_) {
      baton_->post();
    }
  }
  virtual void addPort(const std::string& clientId, const std::string& l2Port)
      override {
    EXPECT_EQ(clientId, g_client);
    EXPECT_FALSE(g_ports.find(l2Port) == g_ports.end());
  }
  virtual void removePort(
      const std::string& clientId,
      const std::string& l2Port) override {
    EXPECT_EQ(clientId, g_client);
    EXPECT_FALSE(g_ports.find(l2Port) == g_ports.end());
  }

  std::shared_ptr<folly::Baton<>> baton_;
};

class DerivedPacketStreamClient : public PacketStreamClient {
 public:
  DerivedPacketStreamClient(
      const std::string& clientId,
      folly::EventBase* evb,
      std::shared_ptr<folly::Baton<>> baton)
      : PacketStreamClient(clientId, evb), baton_(baton) {}
  virtual void recvPacket(Packet&& packet) override {
    EXPECT_FALSE(g_ports.find(*packet.l2Port_ref()) == g_ports.end());
    EXPECT_EQ(g_pktCnt, *packet.buf_ref());
    pktCnt_[*packet.l2Port_ref()]++;
    if (baton_) {
      baton_->post();
    }
  }
  size_t getPckCnt(const std::string& port) {
    return pktCnt_[port].load();
  }

  std::unordered_map<std::string, std::atomic<size_t>> pktCnt_;
  std::shared_ptr<folly::Baton<>> baton_;
};

class PacketStreamTest : public Test {
 public:
  PacketStreamTest() {}

  std::unique_ptr<facebook::fboss::PacketStreamAsyncClient> createClient() {
    auto serverAddr = server_->getAddress();
    // create an SR thrift client
    facebook::servicerouter::ServiceOptions opts;
    opts["single_host"] = {"::1", folly::to<std::string>(serverAddr.getPort())};
    opts["svc_select_count"] = {"1"};
    facebook::servicerouter::ConnConfigs cfg;
    cfg["thrift_transport"] = "rocket";
    cfg["thrift_security"] = "disabled";
    return std::make_unique<facebook::fboss::PacketStreamAsyncClient>(
        facebook::servicerouter::cpp2::getClientFactory().getChannel(
            "", nullptr, opts, cfg));
  }
  void tryConnect(
      std::shared_ptr<folly::Baton<>> baton,
      DerivedPacketStreamClient& streamClient) {
    streamClient.connectToServer(createClient());
    auto retry = 15;
    EXPECT_FALSE(baton->try_wait_for(std::chrono::milliseconds(50)));
    while (!streamClient.isConnectedToServer() && retry > 0) {
      baton->reset();
      EXPECT_FALSE(baton->try_wait_for(std::chrono::milliseconds(50)));
      retry--;
    }
    baton->reset();
    EXPECT_TRUE(streamClient.isConnectedToServer());
  }

  void clientReset(std::unique_ptr<DerivedPacketStreamClient> streamClient) {
    streamClient.reset();
    baton_->reset();
    baton_->try_wait_for(std::chrono::milliseconds(500));
  }

  void SetUp() override {
    baton_ = std::make_shared<folly::Baton<>>();
    handler_ = std::make_shared<DerivedPacketStreamService>(
        "PacketStreamServiceTest", baton_);
    server_ =
        std::make_unique<apache::thrift::ScopedServerInterfaceThread>(handler_);
  }

  void TearDown() override {
    handler_.reset();
    baton_->reset();
    baton_->try_wait_for(std::chrono::milliseconds(100));
  }

  void sendPkt(const std::string& port) {
    EXPECT_TRUE(handler_->isClientConnected(g_client));
    EXPECT_TRUE(handler_->isPortRegistered(g_client, port));
    Packet pkt;
    *pkt.l2Port_ref() = port;
    *pkt.buf_ref() = g_pktCnt;
    EXPECT_NO_THROW(handler_->send(g_client, std::move(pkt)));
  }
  std::shared_ptr<folly::Baton<>> baton_;
  std::shared_ptr<DerivedPacketStreamService> handler_;
  std::unique_ptr<apache::thrift::ScopedServerInterfaceThread> server_;
  folly::ScopedEventBaseThread clientThread_;
};

TEST_F(PacketStreamTest, Connect) {
  std::string port(*g_ports.begin());
  auto baton = std::make_shared<folly::Baton<>>();
  auto streamClient = std::make_unique<DerivedPacketStreamClient>(
      g_client, clientThread_.getEventBase(), baton);
  tryConnect(baton, *streamClient.get());
  baton.reset();
  EXPECT_NO_THROW(streamClient->registerPortToServer(port));
  clientReset(std::move(streamClient));
}

TEST_F(PacketStreamTest, PacketSend) {
  std::string port(*g_ports.begin());
  auto baton = std::make_shared<folly::Baton<>>();
  auto streamClient = std::make_unique<DerivedPacketStreamClient>(
      g_client, clientThread_.getEventBase(), baton);
  tryConnect(baton, *streamClient);
  EXPECT_NO_THROW(streamClient->registerPortToServer(port));
  auto packetCnt = streamClient->getPckCnt(port);
  baton->reset();
  sendPkt(port);
  EXPECT_TRUE(baton->try_wait_for(std::chrono::milliseconds(50)));
  EXPECT_EQ(streamClient->getPckCnt(port), packetCnt + 1);
  clientReset(std::move(streamClient));
}

TEST_F(PacketStreamTest, PacketSendMultiple) {
  std::string port(*g_ports.begin());
  auto baton = std::make_shared<folly::Baton<>>();
  auto streamClient = std::make_unique<DerivedPacketStreamClient>(
      g_client, clientThread_.getEventBase(), baton);
  tryConnect(baton, *streamClient);
  EXPECT_NO_THROW(streamClient->registerPortToServer(port));
  for (auto i = 0; i < 1000; i++) {
    auto packetCnt = streamClient->getPckCnt(port);
    baton->reset();
    sendPkt(port);
    EXPECT_TRUE(baton->try_wait_for(std::chrono::milliseconds(100)));
    EXPECT_EQ(streamClient->getPckCnt(port), packetCnt + 1);
  }
  clientReset(std::move(streamClient));
}

TEST_F(PacketStreamTest, UnregisterPortToServerFail) {
  std::string port(*g_ports.begin());
  auto baton = std::make_shared<folly::Baton<>>();
  auto streamClient = std::make_unique<DerivedPacketStreamClient>(
      g_client, clientThread_.getEventBase(), baton);
  tryConnect(baton, *streamClient);
  EXPECT_TRUE(handler_->isClientConnected(g_client));
  EXPECT_FALSE(handler_->isPortRegistered(g_client, port));
  Packet pkt;
  *pkt.l2Port_ref() = port;
  EXPECT_THROW(handler_->send(g_client, std::move(pkt)), PacketException);
  clientReset(std::move(streamClient));
}

TEST_F(PacketStreamTest, clearPortFromServerFail) {
  std::string port(*g_ports.begin());
  auto baton = std::make_shared<folly::Baton<>>();
  auto streamClient = std::make_unique<DerivedPacketStreamClient>(
      g_client, clientThread_.getEventBase(), baton);
  tryConnect(baton, *streamClient);

  EXPECT_TRUE(handler_->isClientConnected(g_client));
  EXPECT_FALSE(handler_->isPortRegistered(g_client, port));
  EXPECT_THROW(streamClient->clearPortFromServer(port), std::exception);
  clientReset(std::move(streamClient));
}

TEST_F(PacketStreamTest, clearPortFromServerSendFail) {
  std::string port(*g_ports.begin());
  auto baton = std::make_shared<folly::Baton<>>();
  auto streamClient = std::make_unique<DerivedPacketStreamClient>(
      g_client, clientThread_.getEventBase(), baton);
  tryConnect(baton, *streamClient);
  EXPECT_TRUE(handler_->isClientConnected(g_client));
  EXPECT_FALSE(handler_->isPortRegistered(g_client, port));

  EXPECT_NO_THROW(streamClient->registerPortToServer(port));
  baton->reset();
  sendPkt(port);
  EXPECT_TRUE(baton->try_wait_for(std::chrono::milliseconds(50)));
  EXPECT_TRUE(handler_->isPortRegistered(g_client, port));

  EXPECT_NO_THROW(streamClient->clearPortFromServer(port));
  Packet pkt;
  *pkt.l2Port_ref() = port;
  EXPECT_THROW(handler_->send(g_client, std::move(pkt)), std::exception);
  EXPECT_FALSE(handler_->isPortRegistered(g_client, port));
  clientReset(std::move(streamClient));
}

TEST_F(PacketStreamTest, disconnectFail) {
  auto client = createClient();
  std::string port(*g_ports.begin());
  auto baton = std::make_shared<folly::Baton<>>();
  EXPECT_THROW(
      folly::coro::blockingWait(client->co_disconnect(g_client)),
      PacketException);
}

TEST_F(PacketStreamTest, PacketSendMultiPort) {
  auto baton = std::make_shared<folly::Baton<>>();
  auto streamClient = std::make_unique<DerivedPacketStreamClient>(
      g_client, clientThread_.getEventBase(), baton);

  tryConnect(baton, *streamClient);
  for (auto& port : g_ports) {
    EXPECT_NO_THROW(streamClient->registerPortToServer(port));
  }
  streamClient->baton_ = nullptr;
  for (auto& port : g_ports) {
    sendPkt(port);
  }
  baton->reset();
  EXPECT_FALSE(baton->try_wait_for(std::chrono::milliseconds(50)));
  size_t count = 0;
  for (auto& val : streamClient->pktCnt_) {
    EXPECT_EQ(val.second.load(std::memory_order_relaxed), 1);
    count++;
  }
  EXPECT_EQ(count, g_ports.size());
  clientReset(std::move(streamClient));
}

TEST_F(PacketStreamTest, PacketSendMultiPortPauseSend) {
  auto baton = std::make_shared<folly::Baton<>>();
  auto streamClient = std::make_unique<DerivedPacketStreamClient>(
      g_client, clientThread_.getEventBase(), baton);
  tryConnect(baton, *streamClient);

  for (auto& port : g_ports) {
    EXPECT_NO_THROW(streamClient->registerPortToServer(port));
  }
  streamClient->baton_ = nullptr;
  for (auto& port : g_ports) {
    for (auto i = 0; i < 100; i++) {
      baton->reset();
      EXPECT_FALSE(baton->try_wait_for(std::chrono::milliseconds(5)));
      sendPkt(port);
    }
  }
  baton->reset();
  EXPECT_FALSE(baton->try_wait_for(std::chrono::milliseconds(50)));
  size_t count = 0;
  for (auto& val : streamClient->pktCnt_) {
    EXPECT_EQ(val.second.load(std::memory_order_relaxed), 100);
    count++;
  }
  EXPECT_EQ(count, g_ports.size());
  clientReset(std::move(streamClient));
}

TEST_F(PacketStreamTest, PacketSendMultiPortclearPortFromServer) {
  auto baton = std::make_shared<folly::Baton<>>();
  auto streamClient = std::make_unique<DerivedPacketStreamClient>(
      g_client, clientThread_.getEventBase(), baton);
  tryConnect(baton, *streamClient);

  for (auto& port : g_ports) {
    EXPECT_NO_THROW(streamClient->registerPortToServer(port));
  }
  streamClient->baton_ = nullptr;
  for (auto& port : g_ports) {
    for (auto i = 0; i < 100; i++) {
      baton->reset();
      EXPECT_FALSE(baton->try_wait_for(std::chrono::milliseconds(5)));
      sendPkt(port);
    }
    EXPECT_NO_THROW(streamClient->clearPortFromServer(port));
  }
  baton->reset();
  EXPECT_FALSE(baton->try_wait_for(std::chrono::milliseconds(50)));
  size_t count = 0;
  for (auto& port : g_ports) {
    Packet pkt;
    *pkt.l2Port_ref() = port;
    *pkt.buf_ref() = g_pktCnt;
    EXPECT_THROW(handler_->send(g_client, std::move(pkt)), PacketException);
  }
  for (auto& val : streamClient->pktCnt_) {
    EXPECT_EQ(val.second.load(std::memory_order_relaxed), 100);
    count++;
  }
  EXPECT_EQ(count, g_ports.size());
  clientReset(std::move(streamClient));
}

TEST_F(PacketStreamTest, registerPortToServerTest) {
  std::string port(*g_ports.begin());
  auto baton = std::make_shared<folly::Baton<>>();
  auto streamClient = std::make_unique<DerivedPacketStreamClient>(
      g_client, clientThread_.getEventBase(), baton);
  tryConnect(baton, *streamClient);
  EXPECT_NO_THROW(streamClient->registerPortToServer(port));
  EXPECT_NO_THROW(streamClient->registerPortToServer(port));
  auto rPort = std::make_unique<std::string>("test");
  EXPECT_THROW(handler_->registerPort({}, std::move(rPort)), PacketException);
  EXPECT_THROW(handler_->registerPort({}, {}), PacketException);
  EXPECT_THROW(
      handler_->registerPort(
          std::make_unique<std::string>("test123"),
          std::make_unique<std::string>()),
      PacketException);
  EXPECT_FALSE(handler_->isPortRegistered("test123", port));
  EXPECT_FALSE(handler_->isPortRegistered(g_client, "newport"));
  EXPECT_THROW(
      handler_->disconnect(std::make_unique<std::string>()), PacketException);
  clientReset(std::move(streamClient));
}

TEST_F(PacketStreamTest, clearPortFromServerTest) {
  std::string port(*g_ports.begin());
  auto baton = std::make_shared<folly::Baton<>>();
  std::unique_ptr<DerivedPacketStreamClient> streamClient =
      std::make_unique<DerivedPacketStreamClient>(
          g_client, clientThread_.getEventBase(), baton);
  tryConnect(baton, *streamClient);
  EXPECT_NO_THROW(streamClient->registerPortToServer(port));
  EXPECT_NO_THROW(streamClient->clearPortFromServer(port));
  EXPECT_THROW(streamClient->clearPortFromServer(port), PacketException);
  auto rPort = std::make_unique<std::string>("test");
  EXPECT_THROW(handler_->clearPort({}, std::move(rPort)), PacketException);
  EXPECT_THROW(
      handler_->clearPort(
          std::make_unique<std::string>("test"), std::move(rPort)),
      PacketException);
  EXPECT_FALSE(handler_->isClientConnected("test"));
  EXPECT_THROW(handler_->clearPort({}, {}), PacketException);
  EXPECT_THROW(
      handler_->clearPort(
          std::make_unique<std::string>("test123"),
          std::make_unique<std::string>("test123")),
      PacketException);
  clientReset(std::move(streamClient));
}

TEST_F(PacketStreamTest, SendHandlerFailTest) {
  Packet pkt;
  *pkt.l2Port_ref() = "testRandom";
  EXPECT_THROW(handler_->send("test123", std::move(pkt)), PacketException);
  Packet newpkt;
  *newpkt.l2Port_ref() = "testRandom";
  std::string port(*g_ports.begin());
  auto baton = std::make_shared<folly::Baton<>>();
  auto streamClient = std::make_unique<DerivedPacketStreamClient>(
      g_client, clientThread_.getEventBase(), baton);
  EXPECT_THROW(streamClient->registerPortToServer(port), std::exception);
  tryConnect(baton, *streamClient);
  EXPECT_THROW(handler_->send(g_client, std::move(newpkt)), PacketException);
  clientReset(std::move(streamClient));
}

TEST_F(PacketStreamTest, ServerDisconnected) {
  std::string port(*g_ports.begin());
  auto baton = std::make_shared<folly::Baton<>>();
  auto streamClient = std::make_unique<DerivedPacketStreamClient>(
      g_client, clientThread_.getEventBase(), baton);
  tryConnect(baton, *streamClient);
  EXPECT_NO_THROW(streamClient->registerPortToServer(port));
  auto packetCnt = streamClient->getPckCnt(port);
  baton->reset();
  sendPkt(port);
  EXPECT_TRUE(baton->try_wait_for(std::chrono::milliseconds(50)));
  EXPECT_EQ(streamClient->getPckCnt(port), packetCnt + 1);
  server_.reset();
  handler_.reset();
  clientReset(std::move(streamClient));
}

TEST_F(PacketStreamTest, ServerNotStarted) {
  std::string port(*g_ports.begin());
  auto baton = std::make_shared<folly::Baton<>>();
  auto client = createClient();
  server_.reset();
  handler_.reset();

  std::unique_ptr<DerivedPacketStreamClient> streamClient =
      std::make_unique<DerivedPacketStreamClient>(
          g_client, clientThread_.getEventBase(), baton);
  streamClient->connectToServer(std::move(client));

  auto retry = 3;
  EXPECT_FALSE(baton->try_wait_for(std::chrono::milliseconds(50)));
  while (!streamClient->isConnectedToServer() && retry > 0) {
    baton->reset();
    EXPECT_FALSE(baton->try_wait_for(std::chrono::milliseconds(50)));
    retry--;
  }
  baton->reset();
  EXPECT_FALSE(streamClient->isConnectedToServer());
  clientReset(std::move(streamClient));
}

TEST_F(PacketStreamTest, ClientDisconnect) {
  std::string port(*g_ports.begin());
  auto baton = std::make_shared<folly::Baton<>>();
  std::unique_ptr<DerivedPacketStreamClient> streamClient =
      std::make_unique<DerivedPacketStreamClient>(
          g_client, clientThread_.getEventBase(), baton);
  tryConnect(baton, *streamClient);
  EXPECT_NO_THROW(streamClient->registerPortToServer(port));
  auto packetCnt = streamClient->getPckCnt(port);
  baton->reset();
  sendPkt(port);
  EXPECT_TRUE(baton->try_wait_for(std::chrono::milliseconds(50)));
  EXPECT_EQ(streamClient->getPckCnt(port), packetCnt + 1);
  clientReset(std::move(streamClient));
  EXPECT_FALSE(handler_->isClientConnected(g_client));
}

TEST_F(PacketStreamTest, ServerSendClientDisconnected) {
  std::string port(*g_ports.begin());
  auto baton = std::make_shared<folly::Baton<>>();
  std::unique_ptr<DerivedPacketStreamClient> streamClient =
      std::make_unique<DerivedPacketStreamClient>(
          g_client, clientThread_.getEventBase(), baton);
  tryConnect(baton, *streamClient);
  EXPECT_NO_THROW(streamClient->registerPortToServer(port));
  auto packetCnt = streamClient->getPckCnt(port);
  baton->reset();
  sendPkt(port);
  EXPECT_TRUE(baton->try_wait_for(std::chrono::milliseconds(50)));
  EXPECT_EQ(streamClient->getPckCnt(port), packetCnt + 1);
  EXPECT_TRUE(handler_->isClientConnected(g_client));
  EXPECT_TRUE(handler_->isPortRegistered(g_client, port));
  streamClient.reset();
  Packet pkt;
  *pkt.l2Port_ref() = port;
  *pkt.buf_ref() = g_pktCnt;
  EXPECT_THROW(handler_->send(g_client, std::move(pkt)), PacketException);
  clientReset(std::move(streamClient));
}
