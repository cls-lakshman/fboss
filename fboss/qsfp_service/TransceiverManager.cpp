// Copyright 2021-present Facebook. All Rights Reserved.
#include "fboss/qsfp_service/TransceiverManager.h"

#include "fboss/agent/AgentConfig.h"
#include "fboss/agent/FbossError.h"
#include "fboss/agent/Utils.h"
#include "fboss/agent/gen-cpp2/agent_config_types.h"
#include "fboss/agent/gen-cpp2/switch_config_types.h"
#include "fboss/lib/CommonFileUtils.h"
#include "fboss/lib/config/PlatformConfigUtils.h"
#include "fboss/lib/phy/gen-cpp2/phy_types.h"
#include "fboss/lib/phy/gen-cpp2/prbs_types.h"
#include "fboss/lib/thrift_service_client/ThriftServiceClient.h"
#include "fboss/qsfp_service/TransceiverStateMachineUpdate.h"
#include "fboss/qsfp_service/if/gen-cpp2/transceiver_types.h"

using namespace std::chrono;

// allow us to configure the qsfp_service dir so that the qsfp cold boot test
// can run concurrently with itself
DEFINE_string(
    qsfp_service_volatile_dir,
    "/dev/shm/fboss/qsfp_service",
    "Path to the directory in which we store the qsfp_service's cold boot flag");

DEFINE_bool(
    can_qsfp_service_warm_boot,
    true,
    "Enable/disable warm boot functionality for qsfp_service");

namespace {
constexpr auto kForceColdBootFileName = "cold_boot_once_qsfp_service";
constexpr auto kWarmBootFlag = "can_warm_boot";
constexpr auto kWarmbootStateFileName = "qsfp_service_state";
constexpr auto kPhyStateKey = "phy";
} // namespace

namespace facebook::fboss {

TransceiverManager::TransceiverManager(
    std::unique_ptr<TransceiverPlatformApi> api,
    std::unique_ptr<PlatformMapping> platformMapping)
    : qsfpPlatApi_(std::move(api)),
      platformMapping_(std::move(platformMapping)),
      stateMachines_(setupTransceiverToStateMachineHelper()),
      tcvrToPortInfo_(setupTransceiverToPortInfo()) {
  // Cache the static mapping based on platformMapping_
  const auto& platformPorts = platformMapping_->getPlatformPorts();
  const auto& chips = platformMapping_->getChips();
  for (const auto& [portIDInt, platformPort] : platformPorts) {
    PortID portID = PortID(portIDInt);
    const auto& portName = *platformPort.mapping()->name();
    portNameToPortID_.insert(PortNameIdMap::value_type(portName, portID));
    SwPortInfo portInfo;
    portInfo.name = portName;
    portInfo.tcvrID = utility::getTransceiverId(platformPort, chips);
    portToSwPortInfo_.emplace(portID, std::move(portInfo));
  }
}

TransceiverManager::~TransceiverManager() {
  // Make sure if gracefulExit() is not called, we will still stop the threads
  if (!isExiting_) {
    isExiting_ = true;
    stopThreads();
  }
}

void TransceiverManager::init() {
  // Check whether we can warm boot
  canWarmBoot_ = checkWarmBootFlags();
  if (!FLAGS_can_qsfp_service_warm_boot) {
    canWarmBoot_ = false;
  }
  XLOG(INFO) << "Will attempt " << (canWarmBoot_ ? "WARM" : "COLD") << " boot";
  if (!canWarmBoot_) {
    // Since this is going to be cold boot, we need to remove the can_warm_boot
    // file
    removeWarmBootFlag();
  }

  // Now we might need to start threads
  startThreads();

  // Initialize the PhyManager all ExternalPhy for the system
  initExternalPhyMap();
  // Initialize the I2c bus
  initTransceiverMap();
}

void TransceiverManager::gracefulExit() {
  steady_clock::time_point begin = steady_clock::now();
  XLOG(INFO) << "[Exit] Starting TransceiverManager graceful exit";
  // Stop all the threads before shutdown
  isExiting_ = true;
  stopThreads();
  steady_clock::time_point stopThreadsDone = steady_clock::now();
  XLOG(INFO) << "[Exit] Stopped all state machine threads. Stop time: "
             << duration_cast<duration<float>>(stopThreadsDone - begin).count();

  // Set all warm boot related files before gracefully shut down
  setWarmBootState();
  setCanWarmBoot();
  steady_clock::time_point setWBFilesDone = steady_clock::now();
  XLOG(INFO) << "[Exit] Done creating Warm Boot related files. Stop time: "
             << duration_cast<duration<float>>(setWBFilesDone - stopThreadsDone)
                    .count()
             << std::endl
             << "[Exit] Total TransceiverManager graceful Exit time: "
             << duration_cast<duration<float>>(setWBFilesDone - begin).count();
}

const TransceiverManager::PortNameMap&
TransceiverManager::getPortNameToModuleMap() const {
  if (portNameToModule_.empty()) {
    const auto& platformPorts = platformMapping_->getPlatformPorts();
    for (const auto& it : platformPorts) {
      auto port = it.second;
      auto transceiverId =
          utility::getTransceiverId(port, platformMapping_->getChips());
      if (!transceiverId) {
        continue;
      }

      auto& portName = *(port.mapping()->name());
      portNameToModule_[portName] = transceiverId.value();
    }
  }

  return portNameToModule_;
}

const std::set<std::string> TransceiverManager::getPortNames(
    TransceiverID tcvrId) const {
  std::set<std::string> ports;
  auto it = portGroupMap_.find(tcvrId);
  if (it != portGroupMap_.end() && !it->second.empty()) {
    for (const auto& port : it->second) {
      if (auto portName = port.name()) {
        ports.insert(*portName);
      }
    }
  }
  return ports;
}

const std::string TransceiverManager::getPortName(TransceiverID tcvrId) const {
  auto portNames = getPortNames(tcvrId);
  return portNames.empty() ? "" : *portNames.begin();
}

TransceiverManager::TransceiverToStateMachineHelper
TransceiverManager::setupTransceiverToStateMachineHelper() {
  // Set up NewModuleStateMachine map
  TransceiverToStateMachineHelper stateMachineMap;
  if (FLAGS_use_new_state_machine) {
    for (auto chip : platformMapping_->getChips()) {
      if (*chip.second.type() != phy::DataPlanePhyChipType::TRANSCEIVER) {
        continue;
      }
      auto tcvrID = TransceiverID(*chip.second.physicalID());
      stateMachineMap.emplace(
          tcvrID,
          std::make_unique<TransceiverStateMachineHelper>(this, tcvrID));
    }
  }
  return stateMachineMap;
}

TransceiverManager::TransceiverToPortInfo
TransceiverManager::setupTransceiverToPortInfo() {
  TransceiverToPortInfo tcvrToPortInfo;
  if (FLAGS_use_new_state_machine) {
    for (auto chip : platformMapping_->getChips()) {
      if (*chip.second.type() != phy::DataPlanePhyChipType::TRANSCEIVER) {
        continue;
      }
      auto tcvrID = TransceiverID(*chip.second.physicalID());
      auto portToPortInfo = std::make_unique<folly::Synchronized<
          std::unordered_map<PortID, TransceiverPortInfo>>>();
      tcvrToPortInfo.emplace(tcvrID, std::move(portToPortInfo));
    }
  }
  return tcvrToPortInfo;
}

void TransceiverManager::startThreads() {
  if (FLAGS_use_new_state_machine) {
    // Setup all TransceiverStateMachineHelper thread
    for (auto& stateMachineHelper : stateMachines_) {
      stateMachineHelper.second->startThread();
    }

    XLOG(DBG2) << "Started TransceiverStateMachineUpdateThread";
    updateEventBase_ = std::make_unique<folly::EventBase>();
    updateThread_.reset(new std::thread([=] {
      this->threadLoop(
          "TransceiverStateMachineUpdateThread", updateEventBase_.get());
    }));
  }
}

void TransceiverManager::stopThreads() {
  // We use runInEventBaseThread() to terminateLoopSoon() rather than calling it
  // directly here.  This ensures that any events already scheduled via
  // runInEventBaseThread() will have a chance to run.
  if (updateThread_) {
    updateEventBase_->runInEventBaseThread(
        [this] { updateEventBase_->terminateLoopSoon(); });
    updateThread_->join();
    XLOG(DBG2) << "Terminated TransceiverStateMachineUpdateThread";
  }
  // Drain any pending updates by calling handlePendingUpdates directly.
  bool updatesDrained = false;
  do {
    handlePendingUpdates();
    {
      std::unique_lock guard(pendingUpdatesLock_);
      updatesDrained = pendingUpdates_.empty();
    }
  } while (!updatesDrained);
  // And finally stop all TransceiverStateMachineHelper thread
  for (auto& stateMachineHelper : stateMachines_) {
    stateMachineHelper.second->stopThread();
  }
}

void TransceiverManager::threadLoop(
    folly::StringPiece name,
    folly::EventBase* eventBase) {
  initThread(name);
  eventBase->loopForever();
}

void TransceiverManager::updateStateBlocking(
    TransceiverID id,
    TransceiverStateMachineEvent event) {
  auto result = updateStateBlockingWithoutWait(id, event);
  if (result) {
    result->wait();
  }
}

std::shared_ptr<BlockingTransceiverStateMachineUpdateResult>
TransceiverManager::updateStateBlockingWithoutWait(
    TransceiverID id,
    TransceiverStateMachineEvent event) {
  auto result = std::make_shared<BlockingTransceiverStateMachineUpdateResult>();
  auto update = std::make_unique<BlockingTransceiverStateMachineUpdate>(
      id, event, result);
  if (updateState(std::move(update))) {
    // Only return blocking result if the update has been added in queue
    return result;
  }
  return nullptr;
}

bool TransceiverManager::updateState(
    std::unique_ptr<TransceiverStateMachineUpdate> update) {
  if (isExiting_) {
    XLOG(WARN) << "Skipped queueing update:" << update->getName()
               << ", since exit already started";
    return false;
  }
  if (!updateEventBase_) {
    XLOG(WARN) << "Skipped queueing update:" << update->getName()
               << ", since updateEventBase_ is not created yet";
    return false;
  }
  {
    std::unique_lock guard(pendingUpdatesLock_);
    pendingUpdates_.push_back(*update.release());
  }

  // Signal the update thread that updates are pending.
  // We call runInEventBaseThread() with a static function pointer since this
  // is more efficient than having to allocate a new bound function object.
  updateEventBase_->runInEventBaseThread(handlePendingUpdatesHelper, this);
  return true;
}

void TransceiverManager::handlePendingUpdatesHelper(TransceiverManager* mgr) {
  return mgr->handlePendingUpdates();
}
void TransceiverManager::handlePendingUpdates() {
  // Get the list of updates to run.
  // We might pull multiple updates off the list at once if several updates were
  // scheduled before we had a chance to process them.
  // In some case we might also end up finding 0 updates to process if a
  // previous handlePendingUpdates() call processed multiple updates.
  StateUpdateList updates;
  std::set<TransceiverID> toBeUpdateTransceivers;
  {
    std::unique_lock guard(pendingUpdatesLock_);
    // Each TransceiverStateMachineUpdate should be able to process at the same
    // time as we already have lock protection in ExternalPhy and QsfpModule.
    // Therefore, we should just put all the pending update into the updates
    // list as long as they are from totally different transceivers.
    auto iter = pendingUpdates_.begin();
    while (iter != pendingUpdates_.end()) {
      auto [_, isInserted] =
          toBeUpdateTransceivers.insert(iter->getTransceiverID());
      if (!isInserted) {
        // Stop when we find another update for the same transceiver
        break;
      }
      ++iter;
    }
    updates.splice(
        updates.begin(), pendingUpdates_, pendingUpdates_.begin(), iter);
  }

  // handlePendingUpdates() is invoked once for each update, but a previous
  // call might have already processed everything.  If we don't have anything
  // to do just return early.
  if (updates.empty()) {
    return;
  }

  XLOG(DBG2) << "About to update " << updates.size()
             << " TransceiverStateMachine";
  // To expedite all these different transceivers state update, use Future
  std::vector<folly::Future<folly::Unit>> stateUpdateTasks;
  auto iter = updates.begin();
  while (iter != updates.end()) {
    TransceiverStateMachineUpdate* update = &(*iter);
    ++iter;

    auto stateMachineItr = stateMachines_.find(update->getTransceiverID());
    if (stateMachineItr == stateMachines_.end()) {
      XLOG(WARN) << "Unrecognize Transceiver:" << update->getTransceiverID()
                 << ", can't find StateMachine for it. Skip updating.";
      delete update;
      continue;
    }

    stateUpdateTasks.push_back(
        folly::via(stateMachineItr->second->getEventBase())
            .thenValue([update, stateMachineItr](auto&&) {
              XLOG(INFO) << "Preparing TransceiverStateMachine update for "
                         << update->getName();
              // Hold the module state machine lock
              const auto& lockedStateMachine =
                  stateMachineItr->second->getStateMachine().wlock();
              update->applyUpdate(*lockedStateMachine);
            })
            .thenError(
                folly::tag_t<std::exception>{},
                [update](const std::exception& ex) {
                  update->onError(ex);
                  delete update;
                }));
  }
  folly::collectAll(stateUpdateTasks).wait();

  // Notify all of the updates of success and delete them.
  while (!updates.empty()) {
    std::unique_ptr<TransceiverStateMachineUpdate> update(&updates.front());
    updates.pop_front();
    update->onSuccess();
  }
}

TransceiverStateMachineState TransceiverManager::getCurrentState(
    TransceiverID id) const {
  auto stateMachineItr = stateMachines_.find(id);
  if (stateMachineItr == stateMachines_.end()) {
    throw FbossError("Transceiver:", id, " doesn't exist");
  }

  const auto& lockedStateMachine =
      stateMachineItr->second->getStateMachine().rlock();
  auto curStateOrder = *lockedStateMachine->current_state();
  auto curState = getStateByOrder(curStateOrder);
  XLOG(DBG4) << "Current transceiver:" << static_cast<int32_t>(id)
             << ", state order:" << curStateOrder
             << ", state:" << apache::thrift::util::enumNameSafe(curState);
  return curState;
}

const state_machine<TransceiverStateMachine>&
TransceiverManager::getStateMachineForTesting(TransceiverID id) const {
  auto stateMachineItr = stateMachines_.find(id);
  if (stateMachineItr == stateMachines_.end()) {
    throw FbossError("Transceiver:", id, " doesn't exist");
  }
  const auto& lockedStateMachine =
      stateMachineItr->second->getStateMachine().rlock();
  return *lockedStateMachine;
}

bool TransceiverManager::getNeedResetDataPath(TransceiverID id) const {
  auto stateMachineItr = stateMachines_.find(id);
  if (stateMachineItr == stateMachines_.end()) {
    throw FbossError("Transceiver:", id, " doesn't exist");
  }
  return stateMachineItr->second->getStateMachine().rlock()->get_attribute(
      needResetDataPath);
}

std::vector<TransceiverID> TransceiverManager::triggerProgrammingEvents() {
  std::vector<TransceiverID> programmedTcvrs;
  int32_t numProgramIphy{0}, numProgramXphy{0}, numProgramTcvr{0};
  BlockingStateUpdateResultList results;
  steady_clock::time_point begin = steady_clock::now();
  for (auto& stateMachine : stateMachines_) {
    bool needProgramIphy{false}, needProgramXphy{false}, needProgramTcvr{false};
    {
      const auto& lockedStateMachine =
          stateMachine.second->getStateMachine().rlock();
      needProgramIphy = !lockedStateMachine->get_attribute(isIphyProgrammed);
      needProgramXphy = !lockedStateMachine->get_attribute(isXphyProgrammed);
      needProgramTcvr =
          !lockedStateMachine->get_attribute(isTransceiverProgrammed);
    }
    auto tcvrID = stateMachine.first;
    if (needProgramIphy) {
      if (auto result = updateStateBlockingWithoutWait(
              tcvrID, TransceiverStateMachineEvent::PROGRAM_IPHY)) {
        programmedTcvrs.push_back(tcvrID);
        ++numProgramIphy;
        results.push_back(result);
      }
    } else if (needProgramXphy && phyManager_ != nullptr) {
      if (auto result = updateStateBlockingWithoutWait(
              tcvrID, TransceiverStateMachineEvent::PROGRAM_XPHY)) {
        programmedTcvrs.push_back(tcvrID);
        ++numProgramXphy;
        results.push_back(result);
      }
    } else if (needProgramTcvr) {
      if (auto result = updateStateBlockingWithoutWait(
              tcvrID, TransceiverStateMachineEvent::PROGRAM_TRANSCEIVER)) {
        programmedTcvrs.push_back(tcvrID);
        ++numProgramTcvr;
        results.push_back(result);
      }
    }
  }
  waitForAllBlockingStateUpdateDone(results);
  XLOG_IF(DBG2, !programmedTcvrs.empty())
      << "triggerProgrammingEvents has " << numProgramIphy
      << " IPHY programming, " << numProgramXphy << " XPHY programming, "
      << numProgramTcvr << " TCVR programming. Total execute time(ms):"
      << duration_cast<milliseconds>(steady_clock::now() - begin).count();
  return programmedTcvrs;
}

void TransceiverManager::programInternalPhyPorts(TransceiverID id) {
  std::map<int32_t, cfg::PortProfileID> programmedIphyPorts;
  if (auto overridePortAndProfileIt =
          overrideTcvrToPortAndProfileForTest_.find(id);
      overridePortAndProfileIt != overrideTcvrToPortAndProfileForTest_.end()) {
    // NOTE: This is only used for testing.
    for (const auto& [portID, profileID] : overridePortAndProfileIt->second) {
      programmedIphyPorts.emplace(portID, profileID);
    }
  } else {
    // Then call wedge_agent programInternalPhyPorts
    auto wedgeAgentClient = utils::createWedgeAgentClient();
    wedgeAgentClient->sync_programInternalPhyPorts(
        programmedIphyPorts, getTransceiverInfo(id), false);
  }

  std::string logStr = folly::to<std::string>(
      "programInternalPhyPorts() for Transceiver=", id, " return [");
  for (const auto& [portID, profileID] : programmedIphyPorts) {
    logStr = folly::to<std::string>(
        logStr,
        portID,
        " : ",
        apache::thrift::util::enumNameSafe(profileID),
        ", ");
  }
  XLOG(INFO) << logStr << "]";

  // Now update the programmed SW port to profile mapping
  if (auto portToPortInfoIt = tcvrToPortInfo_.find(id);
      portToPortInfoIt != tcvrToPortInfo_.end()) {
    auto portToPortInfoWithLock = portToPortInfoIt->second->wlock();
    portToPortInfoWithLock->clear();
    for (auto [portID, profileID] : programmedIphyPorts) {
      TransceiverPortInfo portInfo;
      portInfo.profile = profileID;
      portToPortInfoWithLock->emplace(PortID(portID), portInfo);
    }
  }
}

std::unordered_map<PortID, TransceiverManager::TransceiverPortInfo>
TransceiverManager::getProgrammedIphyPortToPortInfo(TransceiverID id) const {
  if (auto tcvrToPortInfo_It = tcvrToPortInfo_.find(id);
      tcvrToPortInfo_It != tcvrToPortInfo_.end()) {
    return *(tcvrToPortInfo_It->second->rlock());
  }
  return {};
}

void TransceiverManager::resetProgrammedIphyPortToPortInfo(TransceiverID id) {
  if (auto it = tcvrToPortInfo_.find(id); it != tcvrToPortInfo_.end()) {
    auto portToPortInfoWithLock = it->second->wlock();
    portToPortInfoWithLock->clear();
  }
}

std::unordered_map<PortID, cfg::PortProfileID>
TransceiverManager::getOverrideProgrammedIphyPortAndProfileForTest(
    TransceiverID id) const {
  if (auto portAndProfileIt = overrideTcvrToPortAndProfileForTest_.find(id);
      portAndProfileIt != overrideTcvrToPortAndProfileForTest_.end()) {
    return portAndProfileIt->second;
  }
  return {};
}

void TransceiverManager::programExternalPhyPorts(TransceiverID id) {
  auto phyManager = getPhyManager();
  if (!phyManager) {
    return;
  }
  // Get programmed iphy port profile
  const auto& programmedPortToPortInfo = getProgrammedIphyPortToPortInfo(id);
  if (programmedPortToPortInfo.empty()) {
    // This is due to the iphy ports are disabled. So no need to program xphy
    XLOG(DBG2) << "Skip programming xphy ports for Transceiver=" << id
               << ". Can't find programmed iphy port and port info";
    return;
  }
  const auto& supportedXphyPorts = phyManager->getXphyPorts();
  const auto& transceiver = getTransceiverInfo(id);
  for (const auto& [portID, portInfo] : programmedPortToPortInfo) {
    if (std::find(
            supportedXphyPorts.begin(), supportedXphyPorts.end(), portID) ==
        supportedXphyPorts.end()) {
      XLOG(DBG2) << "Skip programming xphy ports for Transceiver=" << id
                 << ", Port=" << portID << ". Can't find supported xphy";
      continue;
    }

    phyManager->programOnePort(portID, portInfo.profile, transceiver);
    XLOG(INFO) << "Programmed XPHY port for Transceiver=" << id
               << ", Port=" << portID << ", Profile="
               << apache::thrift::util::enumNameSafe(portInfo.profile);
  }
}

TransceiverInfo TransceiverManager::getTransceiverInfo(TransceiverID id) {
  auto lockedTransceivers = transceivers_.rlock();
  if (auto it = lockedTransceivers->find(id); it != lockedTransceivers->end()) {
    return it->second->getTransceiverInfo();
  } else {
    TransceiverInfo absentTcvr;
    absentTcvr.present() = false;
    absentTcvr.port() = id;
    return absentTcvr;
  }
}

void TransceiverManager::programTransceiver(
    TransceiverID id,
    bool needResetDataPath) {
  // Get programmed iphy port profile
  const auto& programmedPortToPortInfo = getProgrammedIphyPortToPortInfo(id);
  if (programmedPortToPortInfo.empty()) {
    // This is due to the iphy ports are disabled. So no need to program tcvr
    XLOG(DBG2) << "Skip programming Transceiver=" << id
               << ". Can't find programmed iphy port and port info";
    return;
  }

  // We don't support single transceiver for different speed software ports
  // And we use the unified speed of software ports to program transceiver
  std::optional<cfg::PortProfileID> unifiedProfile;
  for (const auto& portToPortInfo : programmedPortToPortInfo) {
    auto portProfile = portToPortInfo.second.profile;
    if (!unifiedProfile) {
      unifiedProfile = portProfile;
    } else if (*unifiedProfile != portProfile) {
      throw FbossError(
          "Multiple profiles found for member ports of Transceiver=",
          id,
          ", profiles=[",
          apache::thrift::util::enumNameSafe(*unifiedProfile),
          ", ",
          apache::thrift::util::enumNameSafe(portProfile),
          "]");
    }
  }
  // This should never happen
  if (!unifiedProfile) {
    throw FbossError(
        "Can't find unified profile for member ports of Transceiver=", id);
  }
  const auto profileID = *unifiedProfile;
  auto profileCfgOpt = platformMapping_->getPortProfileConfig(
      PlatformPortProfileConfigMatcher(profileID));
  if (!profileCfgOpt) {
    throw FbossError(
        "Can't find profile config for profileID=",
        apache::thrift::util::enumNameSafe(profileID));
  }
  const auto speed = *profileCfgOpt->speed();

  auto lockedTransceivers = transceivers_.rlock();
  auto tcvrIt = lockedTransceivers->find(id);
  if (tcvrIt == lockedTransceivers->end()) {
    XLOG(DBG2) << "Skip programming Transceiver=" << id
               << ". Transeciver is not present";
    return;
  }
  tcvrIt->second->programTransceiver(speed, needResetDataPath);
  XLOG(INFO) << "Programmed Transceiver for Transceiver=" << id
             << " with speed=" << apache::thrift::util::enumNameSafe(speed)
             << (needResetDataPath ? " with" : " without")
             << " resetting data path";
}

bool TransceiverManager::tryRemediateTransceiver(TransceiverID id) {
  auto lockedTransceivers = transceivers_.rlock();
  auto tcvrIt = lockedTransceivers->find(id);
  if (tcvrIt == lockedTransceivers->end()) {
    XLOG(DBG2) << "Skip remediating Transceiver=" << id
               << ". Transeciver is not present";
    return false;
  }
  bool didRemediate = tcvrIt->second->tryRemediate();
  XLOG_IF(INFO, didRemediate)
      << "Remediated Transceiver for Transceiver=" << id;
  return didRemediate;
}

bool TransceiverManager::supportRemediateTransceiver(TransceiverID id) {
  auto lockedTransceivers = transceivers_.rlock();
  auto tcvrIt = lockedTransceivers->find(id);
  if (tcvrIt == lockedTransceivers->end()) {
    XLOG(DBG2) << "Transceiver=" << id
               << " is not present and can't support remediate";
    return false;
  }
  return tcvrIt->second->supportRemediate();
}

void TransceiverManager::updateTransceiverPortStatus() noexcept {
  if (!FLAGS_use_new_state_machine) {
    return;
  }

  steady_clock::time_point begin = steady_clock::now();
  std::map<int32_t, PortStatus> newPortToPortStatus;
  try {
    // Then call wedge_agent getPortStatus() to get current port status
    auto wedgeAgentClient = utils::createWedgeAgentClient();
    wedgeAgentClient->sync_getPortStatus(newPortToPortStatus, {});
  } catch (const std::exception& ex) {
    // We have retry mechanism to handle failure. No crash here
    XLOG(WARN) << "Failed to call wedge_agent getPortStatus(). "
               << folly::exceptionStr(ex);
    if (overrideAgentPortStatusForTesting_.empty()) {
      return;
    } else {
      XLOG(WARN) << "[TEST ONLY] Use overrideAgentPortStatusForTesting_ "
                 << "for wedge_agent getPortStatus()";
      newPortToPortStatus = overrideAgentPortStatusForTesting_;
    }
  }

  int numResetToDiscovered{0}, numResetToNotPresent{0}, numPortStatusChanged{0};
  auto genStateMachineResetEvent =
      [&numResetToDiscovered, &numResetToNotPresent](
          std::optional<TransceiverStateMachineEvent>& event,
          bool isTcvrPresent) {
        // Update present transceiver state machine back to DISCOVERED
        // and absent transeiver state machine back to NOT_PRESENT
        if (event.has_value()) {
          // If event is already set, no-op
          return;
        }
        if (isTcvrPresent) {
          ++numResetToDiscovered;
          event.emplace(TransceiverStateMachineEvent::RESET_TO_DISCOVERED);
        } else {
          ++numResetToNotPresent;
          event.emplace(TransceiverStateMachineEvent::RESET_TO_NOT_PRESENT);
        }
      };

  const auto& presentTransceivers = getPresentTransceivers();
  BlockingStateUpdateResultList results;
  for (auto& [tcvrID, portToPortInfo] : tcvrToPortInfo_) {
    std::unordered_set<PortID> statusChangedPorts;
    bool anyPortUp = false;
    bool isTcvrPresent =
        (presentTransceivers.find(tcvrID) != presentTransceivers.end());
    bool isTcvrJustProgrammed =
        (getCurrentState(tcvrID) ==
         TransceiverStateMachineState::TRANSCEIVER_PROGRAMMED);
    std::optional<TransceiverStateMachineEvent> event;
    { // lock block for portToPortInfo
      auto portToPortInfoWithLock = portToPortInfo->wlock();
      // All possible platform ports for such transceiver
      const auto& portIDs = getAllPlatformPorts(tcvrID);
      for (auto portID : portIDs) {
        auto portStatusIt = newPortToPortStatus.find(portID);
        auto cachedPortInfoIt = portToPortInfoWithLock->find(portID);
        // If portStatus from agent doesn't have such port
        if (portStatusIt == newPortToPortStatus.end()) {
          if (cachedPortInfoIt == portToPortInfoWithLock->end()) {
            continue;
          } else {
            // Agent remove such port, we need to trigger a state machine reset
            // to trigger programming to get the new sw ports
            portToPortInfoWithLock->erase(cachedPortInfoIt);
            genStateMachineResetEvent(event, isTcvrPresent);
          }
        } else { // If portStatus exists
          // But if the port is disabled, we don't need disabled ports in the
          // cache, since we only store enabled ports as we do in the
          // programInternalPhyPorts()
          if (!(*portStatusIt->second.enabled())) {
            if (cachedPortInfoIt != portToPortInfoWithLock->end()) {
              portToPortInfoWithLock->erase(cachedPortInfoIt);
              genStateMachineResetEvent(event, isTcvrPresent);
            }
          } else {
            // Only care about enabled port status
            anyPortUp = anyPortUp || *portStatusIt->second.up();
            // Agent create such port, we need to trigger a state machine
            // reset to trigger programming to get the new sw ports
            if (cachedPortInfoIt == portToPortInfoWithLock->end()) {
              TransceiverPortInfo portInfo;
              portInfo.status.emplace(portStatusIt->second);
              portToPortInfoWithLock->insert({portID, std::move(portInfo)});
              genStateMachineResetEvent(event, isTcvrPresent);
            } else {
              // Both agent and cache here have such port, update the cached
              // status
              if (!cachedPortInfoIt->second.status ||
                  *cachedPortInfoIt->second.status->up() !=
                      *portStatusIt->second.up()) {
                statusChangedPorts.insert(portID);
              }
              cachedPortInfoIt->second.status.emplace(portStatusIt->second);
            }
          }
        }
      }
      // If event is not set, it means not reset event is needed, now check
      // whether we need port status event.
      // Make sure we update active state for a transceiver which just
      // finished programming
      if (!event && ((!statusChangedPorts.empty()) || isTcvrJustProgrammed)) {
        event.emplace(
            anyPortUp ? TransceiverStateMachineEvent::PORT_UP
                      : TransceiverStateMachineEvent::ALL_PORTS_DOWN);
        ++numPortStatusChanged;
      }

      // Make sure the port event will be added to the update queue under the
      // lock of portToPortInfo, so that it will make sure the cached status
      // and the state machine will be in sync
      if (event.has_value()) {
        if (auto result = updateStateBlockingWithoutWait(tcvrID, *event)) {
          results.push_back(result);
        }
      }
    } // lock block for portToPortInfo
    // After releasing portToPortInfo lock, publishLinkSnapshots() will use
    // transceivers_ lock later
    for (auto portID : statusChangedPorts) {
      try {
        publishLinkSnapshots(portID);
      } catch (const std::exception& ex) {
        XLOG(ERR) << "Port " << portID
                  << " failed publishLinkSnapshpts(): " << ex.what();
      }
    }
  }
  waitForAllBlockingStateUpdateDone(results);
  XLOG_IF(
      DBG2,
      numResetToDiscovered + numResetToNotPresent + numPortStatusChanged > 0)
      << "updateTransceiverPortStatus has " << numResetToDiscovered
      << " transceivers state machines set back to discovered, "
      << numResetToNotPresent << " set back to not_present, "
      << numPortStatusChanged
      << " transceivers need to update port status. Total execute time(ms):"
      << duration_cast<milliseconds>(steady_clock::now() - begin).count();
}

void TransceiverManager::updateTransceiverActiveState(
    const std::set<TransceiverID>& tcvrs,
    const std::map<int32_t, PortStatus>& portStatus) noexcept {
  int numPortStatusChanged{0};
  BlockingStateUpdateResultList results;
  std::unordered_set<TransceiverID> needRefreshTranscevers;
  for (auto tcvrID : tcvrs) {
    auto tcvrToPortInfoIt = tcvrToPortInfo_.find(tcvrID);
    if (tcvrToPortInfoIt == tcvrToPortInfo_.end()) {
      XLOG(WARN) << "Unrecoginized Transceiver:" << tcvrID
                 << ", skip updateTransceiverActiveState()";
      continue;
    }
    XLOG(INFO) << "Syncing ports of transceiver " << tcvrID;
    std::unordered_set<PortID> statusChangedPorts;
    bool anyPortUp = false;
    bool isTcvrJustProgrammed =
        (getCurrentState(tcvrID) ==
         TransceiverStateMachineState::TRANSCEIVER_PROGRAMMED);
    { // lock block for portToPortInfo
      auto portToPortInfoWithLock = tcvrToPortInfoIt->second->wlock();
      for (auto& [portID, tcvrPortInfo] : *portToPortInfoWithLock) {
        // Check whether there's a new port status for such port
        auto portStatusIt = portStatus.find(portID);
        // If port doesn't need to be updated, use the current cached status
        // to indicate whether we need a state update
        if (portStatusIt == portStatus.end()) {
          if (tcvrPortInfo.status) {
            anyPortUp = anyPortUp || *tcvrPortInfo.status->up();
          }
        } else {
          // Only care about enabled port status
          if (*portStatusIt->second.enabled()) {
            anyPortUp = anyPortUp || *portStatusIt->second.up();
            if (!tcvrPortInfo.status ||
                *tcvrPortInfo.status->up() != *portStatusIt->second.up()) {
              statusChangedPorts.insert(portID);
              // Following current QsfpModule::transceiverPortsChanged() logic
              // to make sure we'll always refresh the transceiver if we detect
              // a port status changed. So that we can detect whether the
              // transceiver is removed without waiting for the state machine
              // routinely refreshing
              needRefreshTranscevers.insert(tcvrID);
            }
            // And also update the cached port status
            tcvrPortInfo.status = portStatusIt->second;
          }
        }
      }

      // Make sure the port event will be added to the update queue under the
      // lock of portToPortInfo, so that it will make sure the cached status
      // and the state machine will be in sync
      // Make sure we update active state for a transceiver which just
      // finished programming
      if ((!statusChangedPorts.empty()) || isTcvrJustProgrammed) {
        auto event = anyPortUp ? TransceiverStateMachineEvent::PORT_UP
                               : TransceiverStateMachineEvent::ALL_PORTS_DOWN;
        ++numPortStatusChanged;
        if (auto result = updateStateBlockingWithoutWait(tcvrID, event)) {
          results.push_back(result);
        }
      }
    } // lock block for portToPortInfo
    // After releasing portToPortInfo lock, publishLinkSnapshots() will use
    // transceivers_ lock later
    for (auto portID : statusChangedPorts) {
      try {
        publishLinkSnapshots(portID);
      } catch (const std::exception& ex) {
        XLOG(ERR) << "Port " << portID
                  << " failed publishLinkSnapshpts(): " << ex.what();
      }
    }
  }
  waitForAllBlockingStateUpdateDone(results);
  XLOG_IF(DBG2, numPortStatusChanged > 0)
      << "updateTransceiverActiveState has " << numPortStatusChanged
      << " transceivers need to update port status.";

  if (!needRefreshTranscevers.empty()) {
    XLOG(INFO) << needRefreshTranscevers.size()
               << " transceivers have port status changed and need to refresh";
    refreshTransceivers(needRefreshTranscevers);
  }
}

void TransceiverManager::refreshStateMachines() {
  // Step1: Fetch current port status from wedge_agent.
  // Since the following steps, like refreshTransceivers() might need to use
  // port status to decide whether it's safe to reset a transceiver.
  // Therefore, always do port status update first.
  updateTransceiverPortStatus();

  // Step2: Refresh all transceivers so that we can get an update
  // TransceiverInfo
  const auto& presentXcvrIds = refreshTransceivers();

  // Step3: Check whether there's a wedge_agent config change
  triggerAgentConfigChangeEvent();

  if (FLAGS_use_new_state_machine) {
    // Step4: Once the transceivers are detected, trigger programming events
    const auto& programmedTcvrs = triggerProgrammingEvents();

    // Step5: Remediate inactive transceivers
    // Only need to remediate ports which are not recently finished
    // programming. Because if they only finished early stage programming like
    // iphy without programming xphy or tcvr, the ports of such transceiver
    // will still be not stable to be remediated.
    std::vector<TransceiverID> stableTcvrs;
    for (auto tcvrID : presentXcvrIds) {
      if (std::find(programmedTcvrs.begin(), programmedTcvrs.end(), tcvrID) ==
          programmedTcvrs.end()) {
        stableTcvrs.push_back(tcvrID);
      }
    }
    triggerRemediateEvents(stableTcvrs);
  }
}

void TransceiverManager::triggerAgentConfigChangeEvent() {
  if (!FLAGS_use_new_state_machine) {
    return;
  }

  auto wedgeAgentClient = utils::createWedgeAgentClient();
  ConfigAppliedInfo newConfigAppliedInfo;
  try {
    wedgeAgentClient->sync_getConfigAppliedInfo(newConfigAppliedInfo);
  } catch (const std::exception& ex) {
    // We have retry mechanism to handle failure. No crash here
    XLOG(WARN) << "Failed to call wedge_agent getConfigAppliedInfo(). "
               << folly::exceptionStr(ex);

    // For testing only, if overrideAgentConfigAppliedInfoForTesting_ is set,
    // use it directly; otherwise return without trigger any config changed
    // events
    if (overrideAgentConfigAppliedInfoForTesting_) {
      XLOG(INFO)
          << "triggerAgentConfigChangeEvent is using override ConfigAppliedInfo"
          << ", lastAppliedInMs="
          << *overrideAgentConfigAppliedInfoForTesting_->lastAppliedInMs()
          << ", lastColdbootAppliedInMs="
          << (overrideAgentConfigAppliedInfoForTesting_
                      ->lastColdbootAppliedInMs()
                  ? *overrideAgentConfigAppliedInfoForTesting_
                         ->lastColdbootAppliedInMs()
                  : 0);
      newConfigAppliedInfo = *overrideAgentConfigAppliedInfoForTesting_;
    } else {
      return;
    }
  }

  // Now check if the new timestamp is later than the cached one.
  if (*newConfigAppliedInfo.lastAppliedInMs() <=
      *configAppliedInfo_.lastAppliedInMs()) {
    return;
  }

  // Only need to reset data path if there's a new coldboot
  bool resetDataPath = false;
  std::optional<std::string> resetDataPathLog;
  if (auto lastColdbootAppliedInMs =
          newConfigAppliedInfo.lastColdbootAppliedInMs()) {
    if (auto oldLastColdbootAppliedInMs =
            configAppliedInfo_.lastColdbootAppliedInMs()) {
      resetDataPath = (*lastColdbootAppliedInMs > *oldLastColdbootAppliedInMs);
      resetDataPathLog = folly::to<std::string>(
          "Need reset data path. [Old Coldboot time:",
          *oldLastColdbootAppliedInMs,
          ", New Coldboot time:",
          *lastColdbootAppliedInMs,
          "]");
    } else {
      // Always reset data path the cached info doesn't have coldboot config
      // applied time
      resetDataPath = true;
      resetDataPathLog = folly::to<std::string>(
          "Need reset data path. [Old Coldboot time:0, New Coldboot time:",
          *lastColdbootAppliedInMs,
          "]");
    }
  }

  XLOG(INFO) << "New Agent config applied time:"
             << *newConfigAppliedInfo.lastAppliedInMs()
             << " and last cached time:"
             << *configAppliedInfo_.lastAppliedInMs()
             << ". Issue all ports reprogramming events. "
             << (resetDataPathLog ? *resetDataPathLog : "");

  // Update present transceiver state machine back to DISCOVERED
  // and absent transeiver state machine back to NOT_PRESENT
  int numResetToDiscovered{0}, numResetToNotPresent{0};
  const auto& presentTransceivers = getPresentTransceivers();
  BlockingStateUpdateResultList results;
  for (auto& stateMachine : stateMachines_) {
    // Only need to set true to `needResetDataPath` attribute here. And leave
    // the state machine to change it to false once it finishes
    // programTransceiver
    if (resetDataPath) {
      stateMachine.second->getStateMachine().wlock()->get_attribute(
          needResetDataPath) = true;
    }
    auto tcvrID = stateMachine.first;
    if (presentTransceivers.find(tcvrID) != presentTransceivers.end()) {
      if (auto result = updateStateBlockingWithoutWait(
              tcvrID, TransceiverStateMachineEvent::RESET_TO_DISCOVERED)) {
        ++numResetToDiscovered;
        results.push_back(result);
      }
    } else {
      if (auto result = updateStateBlockingWithoutWait(
              tcvrID, TransceiverStateMachineEvent::RESET_TO_NOT_PRESENT)) {
        ++numResetToNotPresent;
        results.push_back(result);
      }
    }
  }
  waitForAllBlockingStateUpdateDone(results);
  XLOG(INFO) << "triggerAgentConfigChangeEvent has " << numResetToDiscovered
             << " transceivers state machines set back to discovered, "
             << numResetToNotPresent << " set back to not_present";
  configAppliedInfo_ = newConfigAppliedInfo;
}

TransceiverManager::TransceiverStateMachineHelper::
    TransceiverStateMachineHelper(
        TransceiverManager* tcvrMgrPtr,
        TransceiverID tcvrID)
    : tcvrID_(tcvrID) {
  // Init state should be "TRANSCEIVER_STATE_NOT_PRESENT"
  auto lockedStateMachine = stateMachine_.wlock();
  lockedStateMachine->get_attribute(transceiverMgrPtr) = tcvrMgrPtr;
  lockedStateMachine->get_attribute(transceiverID) = tcvrID_;
  // Make sure this attr is false by default.
  lockedStateMachine->get_attribute(needResetDataPath) = false;
}

void TransceiverManager::TransceiverStateMachineHelper::startThread() {
  updateEventBase_ = std::make_unique<folly::EventBase>();
  updateThread_.reset(
      new std::thread([this] { updateEventBase_->loopForever(); }));
}

void TransceiverManager::TransceiverStateMachineHelper::stopThread() {
  if (updateThread_) {
    updateEventBase_->terminateLoopSoon();
    updateThread_->join();
  }
}

void TransceiverManager::waitForAllBlockingStateUpdateDone(
    const TransceiverManager::BlockingStateUpdateResultList& results) {
  for (const auto& result : results) {
    result->wait();
  }
}

/*
 * getPortIDByPortName
 *
 * This function takes the port name string (eth2/1/1) and returns the
 * software port id (or the agent port id) for that
 */
std::optional<PortID> TransceiverManager::getPortIDByPortName(
    const std::string& portName) {
  auto portMapIt = portNameToPortID_.left.find(portName);
  if (portMapIt != portNameToPortID_.left.end()) {
    return portMapIt->second;
  }
  return std::nullopt;
}

/*
 * getPortNameByPortId
 *
 * This function takes the software port id and returns corresponding port name
 * string (ie: eth2/1/1)
 */
std::optional<std::string> TransceiverManager::getPortNameByPortId(
    PortID portId) {
  auto portMapIt = portNameToPortID_.right.find(portId);
  if (portMapIt != portNameToPortID_.right.end()) {
    return portMapIt->second;
  }
  return std::nullopt;
}

std::vector<PortID> TransceiverManager::getAllPlatformPorts(
    TransceiverID tcvrID) const {
  std::vector<PortID> ports;
  for (const auto& [portID, portInfo] : portToSwPortInfo_) {
    if (portInfo.tcvrID && *portInfo.tcvrID == tcvrID) {
      ports.push_back(portID);
    }
  }
  return ports;
}

std::set<TransceiverID> TransceiverManager::getPresentTransceivers() const {
  std::set<TransceiverID> presentTcvrs;
  auto lockedTransceivers = transceivers_.rlock();
  for (const auto& tcvrIt : *lockedTransceivers) {
    if (tcvrIt.second->isPresent()) {
      presentTcvrs.insert(tcvrIt.first);
    }
  }
  return presentTcvrs;
}

void TransceiverManager::setOverrideAgentPortStatusForTesting(
    bool up,
    bool enabled,
    bool clearOnly) {
  // Use overrideTcvrToPortAndProfileForTest_ to prepare
  // overrideAgentPortStatusForTesting_
  overrideAgentPortStatusForTesting_.clear();
  if (clearOnly) {
    return;
  }
  for (const auto& it : overrideTcvrToPortAndProfileForTest_) {
    for (const auto& [portID, profileID] : it.second) {
      PortStatus status;
      status.enabled() = enabled;
      status.up() = up;
      status.profileID() = apache::thrift::util::enumNameSafe(profileID);
      overrideAgentPortStatusForTesting_.emplace(portID, std::move(status));
    }
  }
}

void TransceiverManager::setOverrideAgentConfigAppliedInfoForTesting(
    std::optional<ConfigAppliedInfo> configAppliedInfo) {
  overrideAgentConfigAppliedInfoForTesting_ = configAppliedInfo;
}

bool TransceiverManager::areAllPortsDown(TransceiverID id) const noexcept {
  auto portToPortInfoIt = tcvrToPortInfo_.find(id);
  if (portToPortInfoIt == tcvrToPortInfo_.end()) {
    XLOG(WARN) << "Can't find Transceiver:" << id
               << " in cached tcvrToPortInfo_";
    return false;
  }
  auto portToPortInfoWithLock = portToPortInfoIt->second->rlock();
  if (portToPortInfoWithLock->empty()) {
    XLOG(WARN) << "Can't find any programmed port for Transceiver:" << id
               << " in cached tcvrToPortInfo_";
    return false;
  }
  for (const auto& [portID, portInfo] : *portToPortInfoWithLock) {
    if (!portInfo.status.has_value()) {
      // If no status set, assume ports are up so we won't trigger any
      // disruptive event
      return false;
    }
    if (*portInfo.status->up()) {
      return false;
    }
  }
  return true;
}

void TransceiverManager::triggerRemediateEvents(
    const std::vector<TransceiverID>& stableTcvrs) {
  if (stableTcvrs.empty()) {
    return;
  }
  BlockingStateUpdateResultList results;
  for (auto tcvrID : stableTcvrs) {
    // For stabled transceivers, we check whether the current state machine
    // state is INACTIVE, which means all the ports are down for such
    // Transceiver, so that it's safe to call remediate
    auto curState = getCurrentState(tcvrID);
    if (curState != TransceiverStateMachineState::INACTIVE) {
      continue;
    }
    const auto& programmedPortToPortInfo =
        getProgrammedIphyPortToPortInfo(tcvrID);
    if (programmedPortToPortInfo.empty()) {
      // This is due to the iphy ports are disabled. So no need to remediate
      continue;
    }
    // Then check whether we should remediate so that we don't have to create
    // too many unnecessary state machine update
    auto lockedTransceivers = transceivers_.rlock();
    auto tcvrIt = lockedTransceivers->find(tcvrID);
    if (tcvrIt == lockedTransceivers->end()) {
      XLOG(DBG2) << "Skip remediating Transceiver=" << tcvrID
                 << ". Transeciver is not present";
      continue;
    }
    if (!tcvrIt->second->shouldRemediate()) {
      continue;
    }
    if (auto result = updateStateBlockingWithoutWait(
            tcvrID, TransceiverStateMachineEvent::REMEDIATE_TRANSCEIVER)) {
      results.push_back(result);
    }
  }
  waitForAllBlockingStateUpdateDone(results);
  XLOG_IF(INFO, !results.empty())
      << "triggerRemediateEvents has " << results.size()
      << " transceivers kicked off remediation";
}

void TransceiverManager::markLastDownTime(TransceiverID id) noexcept {
  auto lockedTransceivers = transceivers_.rlock();
  auto tcvrIt = lockedTransceivers->find(id);
  if (tcvrIt == lockedTransceivers->end()) {
    XLOG(DBG2) << "Skip markLastDownTime for Transceiver=" << id
               << ". Transeciver is not present";
    return;
  }
  tcvrIt->second->markLastDownTime();
}

time_t TransceiverManager::getLastDownTime(TransceiverID id) const {
  auto lockedTransceivers = transceivers_.rlock();
  auto tcvrIt = lockedTransceivers->find(id);
  if (tcvrIt == lockedTransceivers->end()) {
    throw FbossError(
        "Can't find Transceiver=", id, ". Transceiver is not present");
  }
  return tcvrIt->second->getLastDownTime();
}

void TransceiverManager::publishLinkSnapshots(std::string portName) {
  auto portIDOpt = getPortIDByPortName(portName);
  if (!portIDOpt) {
    throw FbossError(
        "Unrecoginized portName:", portName, ", can't find port id");
  }
  publishLinkSnapshots(*portIDOpt);
}

void TransceiverManager::publishLinkSnapshots(PortID portID) {
  // Publish xphy snapshots if there's a phyManager and xphy ports
  if (phyManager_) {
    phyManager_->publishXphyInfoSnapshots(portID);
  }
  // Publish transceiver snapshots if there's a transceiver
  if (auto tcvrIDOpt = getTransceiverID(portID)) {
    auto lockedTransceivers = transceivers_.rlock();
    if (auto tcvrIt = lockedTransceivers->find(*tcvrIDOpt);
        tcvrIt != lockedTransceivers->end()) {
      tcvrIt->second->publishSnapshots();
    }
  }
}

std::optional<TransceiverID> TransceiverManager::getTransceiverID(
    PortID portID) {
  auto swPortInfo = portToSwPortInfo_.find(portID);
  if (swPortInfo == portToSwPortInfo_.end()) {
    throw FbossError("Failed to find SwPortInfo for port ID ", portID);
  }
  return swPortInfo->second.tcvrID;
}

bool TransceiverManager::verifyEepromChecksums(TransceiverID id) {
  auto lockedTransceivers = transceivers_.rlock();
  auto tcvrIt = lockedTransceivers->find(id);
  if (tcvrIt == lockedTransceivers->end()) {
    XLOG(DBG2) << "Skip verifying eeprom checksum for Transceiver=" << id
               << ". Transceiver is not present";
    return true;
  }
  return tcvrIt->second->verifyEepromChecksums();
}

bool TransceiverManager::checkWarmBootFlags() {
  // TODO(joseph5wu) Due to new-port-programming is still rolling out, we only
  // need cold boot for new state machine, and old state machine should always
  // use warm boot. Will only check the warm boot flag once we finish rolling
  // out the new port programming feature.

  // Return true if coldBootOnceFile does not exist and
  // - If use_new_state_machine check canWarmBoot file exists
  // - Otherwise, always use warmboot
  const auto& forceColdBootFile = forceColdBootFileName();
  bool forceColdBoot = removeFile(forceColdBootFile);
  if (forceColdBoot) {
    XLOG(INFO) << "Force Cold Boot file: " << forceColdBootFile << " is set";
    return false;
  }

  // However, because all the previous qsfp_service didn't set this warm boot
  // flag at all, we need to have the roll out order:
  // 1) Always return true no matter whether there's warm boot flag file so that
  //    this qsfp_service version can start generating such flag during shut
  //    down;
  // 2) Only check warm boot flag file for new_port_programming case
  // 3) Always check warm boot flag file once new_port_programming is enabled
  //    everywhere.
  const auto& warmBootFile = warmBootFlagFileName();
  // Instead of removing the can_warm_boot file, we keep it unless it's a
  // coldboot, so that qsfp_service crash can still use warm boot.
  bool canWarmBoot = checkFileExists(warmBootFile);
  XLOG(INFO) << "Warm Boot flag: " << warmBootFile << " is "
             << (canWarmBoot ? "set" : "missing");
  // Step 2)
  return FLAGS_use_new_state_machine ? canWarmBoot : true;
}

void TransceiverManager::removeWarmBootFlag() {
  removeFile(warmBootFlagFileName());
}

std::string TransceiverManager::forceColdBootFileName() {
  return folly::to<std::string>(
      FLAGS_qsfp_service_volatile_dir, "/", kForceColdBootFileName);
}

std::string TransceiverManager::warmBootFlagFileName() {
  return folly::to<std::string>(
      FLAGS_qsfp_service_volatile_dir, "/", kWarmBootFlag);
}

std::string TransceiverManager::warmBootStateFileName() const {
  return folly::to<std::string>(
      FLAGS_qsfp_service_volatile_dir, "/", kWarmbootStateFileName);
}

void TransceiverManager::setWarmBootState() {
  // Store necessary information of qsfp_service state into the warmboot state
  // file. This can be the lane id vector of each port from PhyManager or
  // transciever info.
  // Right now, we only need to store phy related info.
  if (phyManager_) {
    folly::dynamic qsfpServiceState = folly::dynamic::object;
    steady_clock::time_point begin = steady_clock::now();
    qsfpServiceState[kPhyStateKey] = phyManager_->getWarmbootState();
    steady_clock::time_point getWarmbootState = steady_clock::now();
    XLOG(INFO)
        << "[Exit] Finish getting warm boot state. Time: "
        << duration_cast<duration<float>>(getWarmbootState - begin).count();
    folly::writeFile(
        folly::toPrettyJson(qsfpServiceState), warmBootStateFileName().c_str());
    steady_clock::time_point serializeState = steady_clock::now();
    XLOG(INFO) << "[Exit] Finish writing warm boot state to file. Time: "
               << duration_cast<duration<float>>(
                      serializeState - getWarmbootState)
                      .count();
  }
}

void TransceiverManager::setCanWarmBoot() {
  const auto& warmBootFile = warmBootFlagFileName();
  auto createFd = createFile(warmBootFile);
  close(createFd);
  XLOG(INFO) << "Wrote can warm boot flag: " << warmBootFile;
}

void TransceiverManager::restoreWarmBootPhyState() {
  // Only need to restore warm boot state if this is a warm boot
  if (!canWarmBoot_) {
    XLOG(INFO) << "[Cold Boot] No need to restore warm boot state";
    return;
  }

  std::string warmBootJson;
  const auto& warmBootStateFile = warmBootStateFileName();
  if (!folly::readFile(warmBootStateFile.c_str(), warmBootJson)) {
    XLOG(WARN) << "Warm Boot state file: " << warmBootStateFile
               << " doesn't exit, skip restoring warm boot state";
    return;
  }

  auto wbState = folly::parseJson(warmBootJson);
  if (const auto& phyStateIt = wbState.find(kPhyStateKey);
      phyManager_ && phyStateIt != wbState.items().end()) {
    phyManager_->restoreFromWarmbootState(phyStateIt->second);
  }
}

namespace {
phy::Side prbsComponentToPhySide(phy::PrbsComponent component) {
  switch (component) {
    case phy::PrbsComponent::ASIC:
      throw FbossError("qsfp_service doesn't support program ASIC prbs");
    case phy::PrbsComponent::GB_SYSTEM:
    case phy::PrbsComponent::TRANSCEIVER_SYSTEM:
      return phy::Side::SYSTEM;
    case phy::PrbsComponent::GB_LINE:
    case phy::PrbsComponent::TRANSCEIVER_LINE:
      return phy::Side::LINE;
  };
  throw FbossError(
      "Unsupported prbs component: ",
      apache::thrift::util::enumNameSafe(component));
}
} // namespace

void TransceiverManager::setInterfacePrbs(
    std::string portName,
    phy::PrbsComponent component,
    const phy::PortPrbsState& state) {
  // Get the port ID first
  auto portId = getPortIDByPortName(portName);
  if (!portId.has_value()) {
    throw FbossError("Can't find a portID for portName ", portName);
  }

  if (component == phy::PrbsComponent::TRANSCEIVER_SYSTEM ||
      component == phy::PrbsComponent::TRANSCEIVER_LINE) {
    if (auto tcvrID = getTransceiverID(portId.value())) {
      phy::Side side = prbsComponentToPhySide(component);
      auto lockedTransceivers = transceivers_.rlock();
      if (auto it = lockedTransceivers->find(*tcvrID);
          it != lockedTransceivers->end()) {
        if (!it->second->setPortPrbs(side, state)) {
          throw FbossError("Failed to set PRBS on transceiver ", *tcvrID);
        }
      } else {
        throw FbossError("Can't find transceiver ", *tcvrID);
      }
    } else {
      throw FbossError("Can't find transceiverID for portID ", portId.value());
    }
  } else {
    if (!phyManager_) {
      throw FbossError("Current platform doesn't support xphy");
    }
    phyManager_->setPortPrbs(
        portId.value(), prbsComponentToPhySide(component), state);
  }
}

phy::PrbsStats TransceiverManager::getPortPrbsStats(
    PortID portId,
    phy::PrbsComponent component) {
  phy::Side side = prbsComponentToPhySide(component);
  if (component == phy::PrbsComponent::TRANSCEIVER_SYSTEM ||
      component == phy::PrbsComponent::TRANSCEIVER_LINE) {
    auto lockedTransceivers = transceivers_.rlock();
    if (auto tcvrID = getTransceiverID(portId)) {
      if (auto it = lockedTransceivers->find(*tcvrID);
          it != lockedTransceivers->end()) {
        return it->second->getPortPrbsStats(side);
      } else {
        throw FbossError("Can't find transceiver ", *tcvrID);
      }
    } else {
      throw FbossError("Can't find transceiverID for portID ", portId);
    }
  } else {
    if (!phyManager_) {
      throw FbossError("Current platform doesn't support xphy");
    }
    phy::PrbsStats stats;
    stats.laneStats() = phyManager_->getPortPrbsStats(portId, side);
    stats.portId() = portId;
    stats.component() = component;
    return stats;
  }
}

void TransceiverManager::clearPortPrbsStats(
    PortID portId,
    phy::PrbsComponent component) {
  phy::Side side = prbsComponentToPhySide(component);
  if (component == phy::PrbsComponent::TRANSCEIVER_SYSTEM ||
      component == phy::PrbsComponent::TRANSCEIVER_LINE) {
    auto lockedTransceivers = transceivers_.rlock();
    if (auto tcvrID = getTransceiverID(portId)) {
      if (auto it = lockedTransceivers->find(*tcvrID);
          it != lockedTransceivers->end()) {
        it->second->clearTransceiverPrbsStats(side);
      } else {
        throw FbossError("Can't find transceiver ", *tcvrID);
      }
    } else {
      throw FbossError("Can't find transceiverID for portID ", portId);
    }
  } else if (!phyManager_) {
    throw FbossError("Current platform doesn't support xphy");
  } else {
    phyManager_->clearPortPrbsStats(portId, prbsComponentToPhySide(component));
  }
}

std::vector<prbs::PrbsPolynomial>
TransceiverManager::getTransceiverPrbsCapabilities(
    TransceiverID tcvrID,
    phy::Side side) {
  auto lockedTransceivers = transceivers_.rlock();
  if (auto it = lockedTransceivers->find(tcvrID);
      it != lockedTransceivers->end()) {
    return it->second->getPrbsCapabilities(side);
  }
  return std::vector<prbs::PrbsPolynomial>();
}

void TransceiverManager::getSupportedPrbsPolynomials(
    std::vector<prbs::PrbsPolynomial>& prbsCapabilities,
    std::string portName,
    phy::PrbsComponent component) {
  phy::Side side = prbsComponentToPhySide(component);
  if (component == phy::PrbsComponent::TRANSCEIVER_SYSTEM ||
      component == phy::PrbsComponent::TRANSCEIVER_LINE) {
    if (portNameToModule_.find(portName) == portNameToModule_.end()) {
      throw FbossError("Can't find transceiver module for port ", portName);
    }
    prbsCapabilities = getTransceiverPrbsCapabilities(
        TransceiverID(portNameToModule_[portName]), side);
  } else {
    throw FbossError(
        "PRBS on ",
        apache::thrift::util::enumNameSafe(component),
        " not supported by qsfp_service");
  }
}

void TransceiverManager::setPortPrbs(
    PortID portId,
    phy::PrbsComponent component,
    const phy::PortPrbsState& state) {
  auto portName = getPortNameByPortId(portId);
  if (!portName.has_value()) {
    throw FbossError("Can't find a portName for portId ", portId);
  }

  setInterfacePrbs(portName.value(), component, state);
}

void TransceiverManager::getInterfacePrbsState(
    prbs::InterfacePrbsState& prbsState,
    std::string portName,
    phy::PrbsComponent component) {
  if (auto portID = getPortIDByPortName(portName)) {
    if (component == phy::PrbsComponent::TRANSCEIVER_SYSTEM ||
        component == phy::PrbsComponent::TRANSCEIVER_LINE) {
      if (auto tcvrID = getTransceiverID(*portID)) {
        phy::Side side = prbsComponentToPhySide(component);
        auto lockedTransceivers = transceivers_.rlock();
        if (auto it = lockedTransceivers->find(*tcvrID);
            it != lockedTransceivers->end()) {
          prbsState = it->second->getPortPrbsState(side);
          return;
        } else {
          throw FbossError("Can't find transceiver ", *tcvrID);
        }
      } else {
        throw FbossError("Can't find transceiverID for portID ", *portID);
      }
    } else {
      throw FbossError(
          "getInterfacePrbsState not supported on component ",
          apache::thrift::util::enumNameSafe(component));
    }
  } else {
    throw FbossError("Can't find a portID for portName ", portName);
  }
}

phy::PrbsStats TransceiverManager::getInterfacePrbsStats(
    std::string portName,
    phy::PrbsComponent component) {
  if (auto portID = getPortIDByPortName(portName)) {
    return getPortPrbsStats(*portID, component);
  }
  throw FbossError("Can't find a portID for portName ", portName);
}

void TransceiverManager::clearInterfacePrbsStats(
    std::string portName,
    phy::PrbsComponent component) {
  if (auto portID = getPortIDByPortName(portName)) {
    clearPortPrbsStats(*portID, component);
  } else {
    throw FbossError("Can't find a portID for portName ", portName);
  }
}

std::optional<DiagsCapability> TransceiverManager::getDiagsCapability(
    TransceiverID id) const {
  auto lockedTransceivers = transceivers_.rlock();
  if (auto it = lockedTransceivers->find(id); it != lockedTransceivers->end()) {
    return it->second->getDiagsCapability();
  }
  XLOG(WARN) << "Return nullopt DiagsCapability for Transceiver=" << id
             << ". Transeciver is not present";
  return std::nullopt;
}

void TransceiverManager::setDiagsCapability(TransceiverID id) {
  auto lockedTransceivers = transceivers_.rlock();
  if (auto it = lockedTransceivers->find(id); it != lockedTransceivers->end()) {
    return it->second->setDiagsCapability();
  }
  XLOG(DBG2) << "Skip setting DiagsCapability for Transceiver=" << id
             << ". Transceiver is not present";
}

Transceiver* TransceiverManager::overrideTransceiverForTesting(
    TransceiverID id,
    std::unique_ptr<Transceiver> overrideTcvr) {
  auto lockedTransceivers = transceivers_.wlock();
  // Keep the same logic as updateTransceiverMap()
  if (auto it = lockedTransceivers->find(id); it != lockedTransceivers->end()) {
    lockedTransceivers->erase(it);
  }
  lockedTransceivers->emplace(id, std::move(overrideTcvr));
  return lockedTransceivers->at(id).get();
}

std::vector<TransceiverID> TransceiverManager::refreshTransceivers(
    const std::unordered_set<TransceiverID>& transceivers) {
  std::vector<TransceiverID> transceiverIds;
  std::vector<folly::Future<folly::Unit>> futs;

  auto lockedTransceivers = transceivers_.rlock();
  auto nTransceivers =
      transceivers.empty() ? lockedTransceivers->size() : transceivers.size();
  XLOG(INFO) << "Start refreshing " << nTransceivers << " transceivers...";

  for (const auto& transceiver : *lockedTransceivers) {
    TransceiverID id = TransceiverID(transceiver.second->getID());
    if (!transceivers.empty() && transceivers.find(id) == transceivers.end()) {
      continue;
    }
    XLOG(DBG3) << "Fired to refresh TransceiverID=" << id;
    transceiverIds.push_back(id);
    futs.push_back(transceiver.second->futureRefresh());
  }

  folly::collectAll(futs.begin(), futs.end()).wait();
  XLOG(INFO) << "Finished refreshing " << nTransceivers << " transceivers";
  return transceiverIds;
}
} // namespace facebook::fboss
