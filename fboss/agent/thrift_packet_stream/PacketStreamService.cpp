// Copyright 2004-present Facebook. All Rights Reserved.

#include "fboss/agent/thrift_packet_stream/PacketStreamService.h"

namespace facebook {
namespace fboss {

PacketStreamService::~PacketStreamService() {
  try {
    clientMap_.withWLock([](auto& lockedMap) {
      for (auto& iter : lockedMap) {
        auto& clientInfo = iter.second;
        auto publisher = std::move(clientInfo.publisher_);
        std::move(*publisher.get()).complete();
      }
      lockedMap.clear();
    });
  } catch (const std::exception& ex) {
    LOG(ERROR) << "Failed to close the thrift stream;";
  }
}

apache::thrift::ServerStream<Packet> PacketStreamService::connect(
    std::unique_ptr<std::string> clientIdPtr) {
  try {
    if (!clientIdPtr || clientIdPtr->empty()) {
      throw PacketException(
          apache::thrift::FragileConstructor(),
          ErrorCode::INVALID_CLIENT,
          "Invalid client");
    }
    const auto& clientId = *clientIdPtr;
    auto streamAndPublisher =
        apache::thrift::ServerStream<Packet>::createPublisher(
            [client = clientId, this] {
              // when the client is disconnected run this section.
              LOG(INFO) << "Client disconnected: " << client;
              clientMap_.withWLock([client = client](auto& lockedMap) {
                lockedMap.erase(client);
              });
              clientDisconnected(client);
            });

    clientMap_.withWLock(
        [client = clientId,
         &publisher = streamAndPublisher.second](auto& lockedMap) {
          lockedMap.emplace(
              std::make_pair(client, ClientInfo(std::move(publisher))));
        });
    clientConnected(clientId);
    LOG(INFO) << clientId << " connected successfully to PacketStreamService";
    return std::move(streamAndPublisher.first);
  } catch (const std::exception& except) {
    throw PacketException(
        apache::thrift::FragileConstructor(),
        ErrorCode::INTERNAL_ERROR,
        except.what());
  }
}

void PacketStreamService::send(const std::string& clientId, Packet&& packet) {
  clientMap_.withRLock([&](auto& lockedMap) {
    auto iter = lockedMap.find(clientId);
    if (iter == lockedMap.end()) {
      LOG(ERROR) << "Client Not Connected";
      throw PacketException(
          apache::thrift::FragileConstructor(),
          ErrorCode::CLIENT_NOT_CONNECTED,
          "client not connected");
    }
    const auto& clientInfo = iter->second;
    auto portIter = clientInfo.portList_.find(*packet.l2Port_ref());
    if (portIter == clientInfo.portList_.end()) {
      LOG(ERROR) << "Port Not Registered";
      throw PacketException(
          apache::thrift::FragileConstructor(),
          ErrorCode::PORT_NOT_REGISTERED,
          "PORT not registered");
    }
    clientInfo.publisher_->next(packet);
  });
}

bool PacketStreamService::isClientConnected(const std::string& clientId) {
  return clientMap_.withRLock([&](auto& lockedMap) {
    auto iter = lockedMap.find(clientId);
    if (iter == lockedMap.end()) {
      return false;
    }
    return true;
  });
}

bool PacketStreamService::isPortRegistered(
    const std::string& clientId,
    const std::string& port) {
  return clientMap_.withRLock([&](auto& lockedMap) {
    auto iter = lockedMap.find(clientId);
    if (iter == lockedMap.end()) {
      return false;
    }
    const auto& clientInfo = iter->second;
    auto portIter = clientInfo.portList_.find(port);
    if (portIter == clientInfo.portList_.end()) {
      return false;
    }
    return true;
  });
}
void PacketStreamService::registerPort(
    std::unique_ptr<std::string> clientIdPtr,
    std::unique_ptr<std::string> l2PortPtr) {
  if (!clientIdPtr || clientIdPtr->empty()) {
    LOG(ERROR) << "Invalid Client";
    throw PacketException(
        apache::thrift::FragileConstructor(),
        ErrorCode::INVALID_CLIENT,
        "Invalid client");
  }

  if (!l2PortPtr || l2PortPtr->empty()) {
    throw PacketException(
        apache::thrift::FragileConstructor(),
        ErrorCode::INVALID_L2PORT,
        "Invalid Port");
  }
  const auto& clientId = *clientIdPtr;
  const auto& l2Port = *l2PortPtr;

  clientMap_.withWLock([&](auto& lockedMap) {
    auto iter = lockedMap.find(clientId);
    if (iter == lockedMap.end()) {
      LOG(ERROR) << "Client " << clientId << " not found";
      throw PacketException(
          apache::thrift::FragileConstructor(),
          ErrorCode::CLIENT_NOT_CONNECTED,
          "client not connected");
    }
    auto& clientInfo = iter->second;
    clientInfo.portList_.insert(l2Port);
    addPort(clientId, l2Port);
  });
}
void PacketStreamService::clearPort(
    std::unique_ptr<std::string> clientIdPtr,
    std::unique_ptr<std::string> l2PortPtr) {
  if (!clientIdPtr || clientIdPtr->empty()) {
    throw PacketException(
        apache::thrift::FragileConstructor(),
        ErrorCode::INVALID_CLIENT,
        "Invalid client");
  }

  if (!l2PortPtr || l2PortPtr->empty()) {
    throw PacketException(
        apache::thrift::FragileConstructor(),
        ErrorCode::INVALID_L2PORT,
        "Invalid Port");
  }
  const auto& clientId = *clientIdPtr;
  const auto& l2Port = *l2PortPtr;

  clientMap_.withWLock([&](auto& lockedMap) {
    auto iter = lockedMap.find(clientId);
    if (iter == lockedMap.end()) {
      throw PacketException(
          apache::thrift::FragileConstructor(),
          ErrorCode::CLIENT_NOT_CONNECTED,
          "client not connected");
    }
    auto& clientInfo = iter->second;
    auto portIter = clientInfo.portList_.find(l2Port);
    if (portIter == clientInfo.portList_.end()) {
      throw PacketException(
          apache::thrift::FragileConstructor(),
          ErrorCode::PORT_NOT_REGISTERED,
          "PORT not registered");
    }
    clientInfo.portList_.erase(l2Port);
    removePort(clientId, l2Port);
  });
}
void PacketStreamService::disconnect(std::unique_ptr<std::string> clientIdPtr) {
  if (!clientIdPtr || clientIdPtr->empty()) {
    throw PacketException(
        apache::thrift::FragileConstructor(),
        ErrorCode::INVALID_CLIENT,
        "Invalid client");
  }

  const auto& clientId = *clientIdPtr;

  clientMap_.withWLock([&](auto& lockedMap) {
    auto iter = lockedMap.find(clientId);
    if (iter == lockedMap.end()) {
      throw PacketException(
          apache::thrift::FragileConstructor(),
          ErrorCode::CLIENT_NOT_CONNECTED,
          "client not connected");
    }
    auto& clientInfo = iter->second;
    auto publisher = std::move(clientInfo.publisher_);
    std::move(*publisher.get()).complete();
    lockedMap.erase(iter);
    clientDisconnected(clientId);
  });
}
} // namespace fboss
} // namespace facebook
