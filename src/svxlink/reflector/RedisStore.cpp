#include "RedisStore.h"
#include <hiredis/hiredis.h>
#include <iostream>

RedisStore::RedisStore(const Config& cfg) : m_cfg(cfg) {}
RedisStore::~RedisStore() {
  if (m_sync) { redisFree(m_sync); m_sync = nullptr; }
}

bool RedisStore::connect(void) {
  std::cerr << "RedisStore::connect not implemented yet" << std::endl;
  return false;
}

bool RedisStore::isReady(void) const { return m_sync != nullptr; }

std::string RedisStore::lookupUserKey(const std::string&) { return ""; }
std::map<std::string, std::string> RedisStore::loadAllUsers(void) { return {}; }
bool RedisStore::isUserEnabled(const std::string&) { return true; }
std::set<uint32_t> RedisStore::loadClusterTgs(void) { return {}; }
std::string RedisStore::loadTrunkFilter(const std::string&, const std::string&) { return ""; }
std::map<uint32_t, uint32_t> RedisStore::loadTrunkTgMap(const std::string&) { return {}; }
std::set<std::string> RedisStore::loadTrunkMutes(const std::string&) { return {}; }
void RedisStore::addTrunkMute(const std::string&, const std::string&) {}
void RedisStore::removeTrunkMute(const std::string&, const std::string&) {}
void RedisStore::publishConfigChanged(const std::string&) {}
void RedisStore::pushLiveTalker(uint32_t, const std::string&, const std::string&) {}
void RedisStore::clearLiveTalker(uint32_t) {}
void RedisStore::pushLiveClient(const std::string&, const std::string&,
                                const std::string&, uint32_t) {}
void RedisStore::clearLiveClient(const std::string&) {}
void RedisStore::pushLiveTrunk(const std::string&, const std::string&,
                               const std::string&) {}

size_t RedisStore::liveQueueSize(void) const { return 0; }

bool RedisStore::connectSync(void) { return false; }
bool RedisStore::connectAsync(void) { return false; }
void RedisStore::subscribe(void) {}
void RedisStore::scheduleReconnect(void) {}
void RedisStore::onAsyncDisconnect(int) {}
void RedisStore::onPubSubMessage(const std::string&, const std::string&) {}
void RedisStore::drainLiveQueue(Async::Timer*) {}
void RedisStore::refreshLiveExpire(Async::Timer*) {}

std::string RedisStore::keyFor(const std::string& suffix) const {
  if (m_cfg.key_prefix.empty()) return suffix;
  return m_cfg.key_prefix + ":" + suffix;
}

std::string RedisStore::channelName(void) const {
  return keyFor("config.changed");
}
