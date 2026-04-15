#include "RedisStore.h"
#include <hiredis/hiredis.h>
#include <iostream>

RedisStore::RedisStore(const Config& cfg) : m_cfg(cfg) {}
RedisStore::~RedisStore() {
  if (m_sync) { redisFree(m_sync); m_sync = nullptr; }
}

bool RedisStore::connectSync(void) {
  struct timeval tv = { 5, 0 };
  if (!m_cfg.unix_socket.empty()) {
    m_sync = redisConnectUnixWithTimeout(m_cfg.unix_socket.c_str(), tv);
  } else {
    m_sync = redisConnectWithTimeout(m_cfg.host.c_str(), m_cfg.port, tv);
  }
  if (!m_sync || m_sync->err) {
    std::cerr << "*** ERROR: Redis sync connect failed: "
              << (m_sync ? m_sync->errstr : "alloc failed") << std::endl;
    if (m_sync) { redisFree(m_sync); m_sync = nullptr; }
    return false;
  }
  if (!m_cfg.password.empty()) {
    redisReply* r = (redisReply*)redisCommand(m_sync, "AUTH %s", m_cfg.password.c_str());
    if (!r || r->type == REDIS_REPLY_ERROR) {
      std::cerr << "*** ERROR: Redis AUTH failed" << std::endl;
      if (r) freeReplyObject(r);
      redisFree(m_sync); m_sync = nullptr;
      return false;
    }
    freeReplyObject(r);
  }
  if (m_cfg.db != 0) {
    redisReply* r = (redisReply*)redisCommand(m_sync, "SELECT %d", m_cfg.db);
    if (!r || r->type == REDIS_REPLY_ERROR) {
      std::cerr << "*** ERROR: Redis SELECT failed" << std::endl;
      if (r) freeReplyObject(r);
      redisFree(m_sync); m_sync = nullptr;
      return false;
    }
    freeReplyObject(r);
  }
  return true;
}

bool RedisStore::connect(void) {
  if (!connectSync()) return false;
  std::cout << "RedisStore: connected (sync) to "
            << (m_cfg.unix_socket.empty()
                ? (m_cfg.host + ":" + std::to_string(m_cfg.port))
                : m_cfg.unix_socket)
            << std::endl;
  return true;
}

bool RedisStore::isReady(void) const { return m_sync != nullptr; }

std::string RedisStore::lookupUserKey(const std::string& callsign) {
  if (!m_sync) return "";
  std::string user_key = keyFor("user:" + callsign);
  redisReply* r = (redisReply*)redisCommand(m_sync,
      "HMGET %s group enabled", user_key.c_str());
  if (!r || r->type != REDIS_REPLY_ARRAY || r->elements != 2) {
    if (r) freeReplyObject(r);
    return "";
  }
  if (r->element[0]->type != REDIS_REPLY_STRING) {
    freeReplyObject(r);
    return "";
  }
  std::string group(r->element[0]->str, r->element[0]->len);
  bool enabled = true;
  if (r->element[1]->type == REDIS_REPLY_STRING) {
    enabled = std::string(r->element[1]->str, r->element[1]->len) != "0";
  }
  freeReplyObject(r);
  if (!enabled) {
    std::cout << "RedisStore: user " << callsign << " disabled" << std::endl;
    return "";
  }

  std::string group_key = keyFor("group:" + group);
  r = (redisReply*)redisCommand(m_sync, "HGET %s password", group_key.c_str());
  if (!r || r->type != REDIS_REPLY_STRING) {
    if (r) freeReplyObject(r);
    std::cout << "*** WARNING: group " << group << " missing password" << std::endl;
    return "";
  }
  std::string pw(r->str, r->len);
  freeReplyObject(r);
  return pw;
}

std::map<std::string, std::string> RedisStore::loadAllUsers(void) { return {}; }
bool RedisStore::isUserEnabled(const std::string&) { return true; }

std::set<uint32_t> RedisStore::loadClusterTgs(void) {
  std::set<uint32_t> out;
  if (!m_sync) return out;
  std::string k = keyFor("cluster:tgs");
  redisReply* r = (redisReply*)redisCommand(m_sync, "SMEMBERS %s", k.c_str());
  if (r && r->type == REDIS_REPLY_ARRAY) {
    for (size_t i = 0; i < r->elements; ++i) {
      if (r->element[i]->type == REDIS_REPLY_STRING) {
        try { out.insert(std::stoul(std::string(r->element[i]->str, r->element[i]->len))); }
        catch (...) {}
      }
    }
  }
  if (r) freeReplyObject(r);
  return out;
}

std::string RedisStore::loadTrunkFilter(const std::string& section,
                                       const std::string& field) {
  if (!m_sync) return "";
  std::string k = keyFor("trunk:" + section + ":" + field);
  redisReply* r = (redisReply*)redisCommand(m_sync, "SMEMBERS %s", k.c_str());
  std::string out;
  if (r && r->type == REDIS_REPLY_ARRAY) {
    for (size_t i = 0; i < r->elements; ++i) {
      if (r->element[i]->type == REDIS_REPLY_STRING) {
        if (!out.empty()) out += ",";
        out.append(r->element[i]->str, r->element[i]->len);
      }
    }
  }
  if (r) freeReplyObject(r);
  return out;
}

std::map<uint32_t, uint32_t> RedisStore::loadTrunkTgMap(const std::string& section) {
  std::map<uint32_t, uint32_t> out;
  if (!m_sync) return out;
  std::string k = keyFor("trunk:" + section + ":tgmap");
  redisReply* r = (redisReply*)redisCommand(m_sync, "HGETALL %s", k.c_str());
  if (r && r->type == REDIS_REPLY_ARRAY) {
    for (size_t i = 0; i + 1 < r->elements; i += 2) {
      try {
        uint32_t peer  = std::stoul(std::string(r->element[i]->str,   r->element[i]->len));
        uint32_t local = std::stoul(std::string(r->element[i+1]->str, r->element[i+1]->len));
        out[peer] = local;
      } catch (...) {}
    }
  }
  if (r) freeReplyObject(r);
  return out;
}

std::set<std::string> RedisStore::loadTrunkMutes(const std::string& section) {
  std::set<std::string> out;
  if (!m_sync) return out;
  std::string k = keyFor("trunk:" + section + ":mutes");
  redisReply* r = (redisReply*)redisCommand(m_sync, "SMEMBERS %s", k.c_str());
  if (r && r->type == REDIS_REPLY_ARRAY) {
    for (size_t i = 0; i < r->elements; ++i) {
      if (r->element[i]->type == REDIS_REPLY_STRING) {
        out.emplace(r->element[i]->str, r->element[i]->len);
      }
    }
  }
  if (r) freeReplyObject(r);
  return out;
}

void RedisStore::addTrunkMute(const std::string& section, const std::string& callsign) {
  if (!m_sync) return;
  std::string k = keyFor("trunk:" + section + ":mutes");
  redisReply* r = (redisReply*)redisCommand(m_sync, "SADD %s %s",
                                            k.c_str(), callsign.c_str());
  if (r) freeReplyObject(r);
}

void RedisStore::removeTrunkMute(const std::string& section, const std::string& callsign) {
  if (!m_sync) return;
  std::string k = keyFor("trunk:" + section + ":mutes");
  redisReply* r = (redisReply*)redisCommand(m_sync, "SREM %s %s",
                                            k.c_str(), callsign.c_str());
  if (r) freeReplyObject(r);
}

void RedisStore::publishConfigChanged(const std::string& scope) {
  if (!m_sync) return;
  std::string ch = channelName();
  redisReply* r = (redisReply*)redisCommand(m_sync, "PUBLISH %s %s",
                                            ch.c_str(), scope.c_str());
  if (r) freeReplyObject(r);
}

void RedisStore::pushLiveTalker(uint32_t, const std::string&, const std::string&) {}
void RedisStore::clearLiveTalker(uint32_t) {}
void RedisStore::pushLiveClient(const std::string&, const std::string&,
                                const std::string&, uint32_t) {}
void RedisStore::clearLiveClient(const std::string&) {}
void RedisStore::pushLiveTrunk(const std::string&, const std::string&,
                               const std::string&) {}

size_t RedisStore::liveQueueSize(void) const { return 0; }

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
