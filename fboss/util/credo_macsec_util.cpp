// (c) Facebook, Inc. and its affiliates. Confidential and proprietary.

#include <folly/ThreadLocal.h>
#include "fboss/qsfp_service/if/gen-cpp2/QsfpService.h"
#include <folly/init/Init.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/io/async/AsyncSocket.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>
#include <folly/json.h>
#include <folly/FileUtil.h>
#include <folly/Random.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include "fboss/qsfp_service/lib/QsfpClient.h"

using namespace facebook::fboss;
using namespace facebook::fboss::mka;
using namespace apache::thrift;

DEFINE_bool(phy_port_map, false, "Print Phy Port map for this platform");

/*
 * getMacsecSaFromJson()
 * Reads MacSec SA information from JSON file to MKASak structure.
 * sa-json-file:
 *  {
 *       "sci": {
 *               "macAddress": "<mac-address>",
 *               "port": <port-id>
 *       },
 *       "l2Port": "<l2-port-string>",
 *       "assocNum": <association-number>,
 *       "keyHex": "<key-hex-value>",
 *       "keyIdHex": "<Key-id-hex-value>",
 *       "primary": <true/false>
 *  }
 */
bool getMacsecSaFromJson(std::string saJsonFile, MKASak& sak) {
    std::string saJsonStr;
    bool ret = folly::readFile(saJsonFile.c_str(), saJsonStr);
    if (!ret) {
        printf("SA config could not be read\n");
        return false;
    }
    folly::dynamic saJsonObj = folly::parseJson(saJsonStr);

    if (saJsonObj.find("assocNum") == saJsonObj.items().end()) {
        sak.assocNum_ref() = 0;
    } else {
        sak.assocNum_ref() = saJsonObj["assocNum"].asInt();
    }

    if (saJsonObj.find("l2Port") == saJsonObj.items().end()) {
        printf("l2Port not present in SA config\n");
        return false;
    } else {
        sak.l2Port_ref() = saJsonObj["l2Port"].asString();
    }

    if (saJsonObj.find("keyHex") == saJsonObj.items().end()) {
        printf("keyHex not present in SA config\n");
        return false;
    } else {
        sak.keyHex_ref() = saJsonObj["keyHex"].asString();
    }

    if (saJsonObj.find("keyIdHex") == saJsonObj.items().end()) {
        printf("keyIdHex not present in SA config\n");
        return false;
    } else {
        sak.keyIdHex_ref() = saJsonObj["keyIdHex"].asString();
    }

    if (saJsonObj.find("primary") == saJsonObj.items().end()) {
        printf("primary not present in SA config\n");
        return false;
    } else {
        sak.primary_ref() = saJsonObj["primary"].asBool();
    }

    if (saJsonObj.find("sci") == saJsonObj.items().end()) {
        printf("sci not present in SA config\n");
        return false;
    }
    folly::dynamic scJsonObj = saJsonObj["sci"];

    if (scJsonObj.find("macAddress") == scJsonObj.items().end()) {
        printf("macAddress not present in SA config\n");
        return false;
    } else {
        sak.sci_ref()->macAddress_ref() = scJsonObj["macAddress"].asString();
    }

    if (scJsonObj.find("port") == scJsonObj.items().end()) {
        printf("port not present in SA config\n");
        return false;
    } else {
        sak.sci_ref()->port_ref() = scJsonObj["port"].asInt();
    }
    return true;
}

/*
 * getMacsecScFromJson()
 * Reads MacSec SC information from JSON file to MKASci structure.
 * sc-json-file:
 * {
 *       "macAddress": "<mac-address>",
 *       "port": <port-id>
 * }
 */
bool getMacsecScFromJson(std::string scJsonFile, MKASci& sci) {
    std::string scJsonStr;
    bool ret = folly::readFile(scJsonFile.c_str(), scJsonStr);
    if (!ret) {
        printf("SC config could not be read\n");
        return false;
    }
    folly::dynamic scJsonObj = folly::parseJson(scJsonStr);

    if (scJsonObj.find("port") == scJsonObj.items().end()) {
        printf("port not present in SC config\n");
        return false;
    } else {
        sci.port_ref() = scJsonObj["port"].asInt();
    }

    if (scJsonObj.find("macAddress") == scJsonObj.items().end()) {
        printf("macAddress not present in SC config\n");
        return false;
    } else {
        sci.macAddress_ref() = scJsonObj["macAddress"].asString();
    }
    return true;
}

/*
 * printPhyPortMap()
 * Print the Phy port mapping information from the macsec handling process.
 * This function will make the thrift call to the qsfp_service (or phy_service)
 * process to get this phy port mapping information. ie:
 * PORT    SLOT    MDIO    PHYID   NAME
 *  1       2       1       0      eth4/2/1
 *  3       0       0       0      eth2/1/1
 *  5       3       4       0      eth5/5/1
 */
void printPhyPortMap(QsfpServiceAsyncClient* fbMacsecHandler) {
  MacsecPortPhyMap macsecPortPhyMap{};
  fbMacsecHandler->sync_macsecGetPhyPortInfo(macsecPortPhyMap);

  printf("Printing the Phy port info map:\n");
  printf("PORT    SLOT    MDIO    PHYID   NAME\n");
  for (auto it : *macsecPortPhyMap.macsecPortPhyMap_ref()) {
      int port = it.first;
      int slot = *it.second.slotId_ref();
      int mdio = *it.second.mdioId_ref();
      int phy = *it.second.phyId_ref();

      printf("%4d    %4d    %4d    %4d    eth%d/%d/1\n",  port, slot, mdio, phy, slot, mdio+1);
  }
}

int main(int argc, char* argv[]) {
  folly::init(&argc, &argv, true);
  gflags::SetCommandLineOptionWithMode(
      "minloglevel", "0", gflags::SET_FLAGS_DEFAULT);
  folly::EventBase evb;

  std::unique_ptr<QsfpServiceAsyncClient> client;
  client = QsfpClient::createClient(&evb).getVia(&evb);

  if (FLAGS_phy_port_map) {
    printPhyPortMap(client.get());
    return 0;
  }

  return 0;
}
