#include "RedisStore.h"
#include <hiredis/hiredis.h>
#include <hiredis/async.h>
#include "RedisAsyncAdapter.h"
#include <AsyncTimer.h>
#include "RedisLiveQueue.h"
#include <iostream>
#include <ctime>

namespace {

void onPubSubMessageThunk(redisAsyncContext* /*ac*/, void* reply, void* priv) {
  auto* self = static_cast<RedisStore*>(priv);
  auto* r = static_cast<redisReply*>(reply);
  if (!r || r->type != REDIS_REPLY_ARRAY || r->elements < 3) return;
  if (r->element[0]->type == REDIS_REPLY_STRING &&
      std::string(r->element[0]->str, r->element[0]->len) == "message") {
    std::string payload(r->element[2]->str, r->element[2]->len);
    self->configChanged.emit(payload);
  }
}

void onAsyncDisconnectThunk(const redisAsyncContext* ac, int status) {
  auto* self = static_cast<RedisStore*>(ac->data);
  if (self) self->onAsyncDisconnect(status);
}

void onAsyncWriteDisconnectThunk(const redisAsyncContext* ac, int status) {
  auto* self = static_cast<RedisStore*>(ac->data);
  if (self) self->onAsyncWriteDisconnect(status);
}

} // namespace

RedisStore::RedisStore(const Config& cfg) : m_cfg(cfg) {}
RedisStore::~RedisStore() {
  m_shutting_down = true;
  delete m_reconnect_timer; m_reconnect_timer = nullptr;
  delete m_heartbeat_timer; m_heartbeat_timer = nullptr;
  delete m_drain_timer;    m_drain_timer = nullptr;
  delete m_live_queue;     m_live_queue  = nullptr;
  if (m_async_write) { redisAsyncFree(m_async_write); m_async_write = nullptr; }
  if (m_async) { redisAsyncFree(m_async); m_async = nullptr; }
  if (m_sync)  { redisFree(m_sync);       m_sync  = nullptr; }
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
  if (!connectAsync()) return false;
  subscribe();
  if (!connectAsyncWrite()) return false;

  m_live_queue = new RedisLiveQueue(4096);
  m_drain_timer = new Async::Timer(75, Async::Timer::TYPE_PERIODIC);
  m_drain_timer->expired.connect(
      sigc::mem_fun(*this, &RedisStore::drainLiveQueue));

  m_heartbeat_timer = new Async::Timer(30000, Async::Timer::TYPE_PERIODIC);
  m_heartbeat_timer->expired.connect(
      sigc::mem_fun(*this, &RedisStore::refreshLiveExpire));

  std::cout << "RedisStore: connected sync+async, subscribed to "
            << channelName() << std::endl;
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

std::map<std::string, RedisStore::TrunkPeerConfig>
RedisStore::loadTrunkPeers(void)
{
  std::map<std::string, TrunkPeerConfig> out;
  if (!m_sync) return out;
  std::string pattern = keyFor("trunk:*:peer");
  redisReply* r = (redisReply*)redisCommand(m_sync, "KEYS %s", pattern.c_str());
  if (!r || r->type != REDIS_REPLY_ARRAY) {
    if (r) freeReplyObject(r);
    return out;
  }
  std::vector<std::string> matched_keys;
  for (size_t i = 0; i < r->elements; ++i) {
    if (r->element[i]->type == REDIS_REPLY_STRING) {
      matched_keys.emplace_back(r->element[i]->str, r->element[i]->len);
    }
  }
  freeReplyObject(r);

  // Extract section name: strip "<keyprefix>:trunk:" prefix and ":peer" suffix.
  std::string left = keyFor("trunk:");
  const std::string right = ":peer";
  for (const std::string& key : matched_keys) {
    if (key.size() < left.size() + right.size()) continue;
    if (key.substr(0, left.size()) != left) continue;
    if (key.substr(key.size() - right.size()) != right) continue;
    std::string section = key.substr(left.size(),
                                     key.size() - left.size() - right.size());
    if (section.empty()) continue;

    redisReply* h = (redisReply*)redisCommand(m_sync, "HGETALL %s", key.c_str());
    if (!h || h->type != REDIS_REPLY_ARRAY) {
      if (h) freeReplyObject(h);
      continue;
    }
    TrunkPeerConfig cfg;
    for (size_t i = 0; i + 1 < h->elements; i += 2) {
      std::string field(h->element[i]->str, h->element[i]->len);
      std::string value(h->element[i+1]->str, h->element[i+1]->len);
      if      (field == "host")          cfg.host = value;
      else if (field == "port")          cfg.port = value;
      else if (field == "secret")        cfg.secret = value;
      else if (field == "remote_prefix") cfg.remote_prefix = value;
      else if (field == "peer_id")       cfg.peer_id = value;
    }
    freeReplyObject(h);

    // Require host + secret + remote_prefix
    if (cfg.host.empty() || cfg.secret.empty() || cfg.remote_prefix.empty()) {
      std::cerr << "*** WARN: RedisStore: trunk peer " << section
                << " missing required fields (host/secret/remote_prefix); "
                << "skipping" << std::endl;
      continue;
    }
    out[section] = std::move(cfg);
  }
  return out;
}

void RedisStore::publishConfigChanged(const std::string& scope) {
  if (!m_sync) return;
  std::string ch = channelName();
  redisReply* r = (redisReply*)redisCommand(m_sync, "PUBLISH %s %s",
                                            ch.c_str(), scope.c_str());
  if (r) freeReplyObject(r);
}

void RedisStore::pushLiveTalker(uint32_t tg, const std::string& callsign,
                                const std::string& source) {
  if (!m_live_queue) return;
  RedisLiveQueue::Op op;
  op.op  = RedisLiveQueue::OpType::HSET;
  op.key = keyFor("live:talker:" + std::to_string(tg));
  m_live_keys.insert(op.key);
  op.fields = {
    {"callsign",   callsign},
    {"started_at", std::to_string(std::time(nullptr))},
    {"source",     source}
  };
  op.ttl_s = 60;
  m_live_queue->push(std::move(op));
}

void RedisStore::clearLiveTalker(uint32_t tg) {
  if (!m_live_queue) return;
  RedisLiveQueue::Op op;
  op.op  = RedisLiveQueue::OpType::DEL;
  op.key = keyFor("live:talker:" + std::to_string(tg));
  m_live_keys.erase(op.key);
  m_live_queue->push(std::move(op));
}

void RedisStore::pushLiveClient(const std::string& callsign,
                                const std::string& ip,
                                const std::string& codecs,
                                uint32_t current_tg) {
  if (!m_live_queue) return;
  RedisLiveQueue::Op op;
  op.op  = RedisLiveQueue::OpType::HSET;
  op.key = keyFor("live:client:" + callsign);
  m_live_keys.insert(op.key);
  op.fields = {
    {"connected_at", std::to_string(std::time(nullptr))},
    {"ip",           ip},
    {"codecs",       codecs},
    {"tg",           std::to_string(current_tg)}
  };
  op.ttl_s = 60;
  m_live_queue->push(std::move(op));
}

void RedisStore::clearLiveClient(const std::string& callsign) {
  if (!m_live_queue) return;
  RedisLiveQueue::Op op;
  op.op  = RedisLiveQueue::OpType::DEL;
  op.key = keyFor("live:client:" + callsign);
  m_live_keys.erase(op.key);
  m_live_queue->push(std::move(op));
}

void RedisStore::pushLiveTrunk(const std::string& section,
                               const std::string& state,
                               const std::string& peer_id) {
  if (!m_live_queue) return;
  RedisLiveQueue::Op op;
  op.op  = RedisLiveQueue::OpType::HSET;
  op.key = keyFor("live:trunk:" + section);
  m_live_keys.insert(op.key);
  op.fields = {
    {"state",   state},
    {"peer_id", peer_id},
    {"last_hb", std::to_string(std::time(nullptr))}
  };
  op.ttl_s = 60;
  m_live_queue->push(std::move(op));
}

size_t RedisStore::liveQueueSize(void) const {
  return m_live_queue ? m_live_queue->size() : 0;
}

bool RedisStore::connectAsync(void) {
  if (!m_cfg.unix_socket.empty()) {
    m_async = redisAsyncConnectUnix(m_cfg.unix_socket.c_str());
  } else {
    m_async = redisAsyncConnect(m_cfg.host.c_str(), m_cfg.port);
  }
  if (!m_async || m_async->err) {
    std::cerr << "*** ERROR: Redis async connect failed: "
              << (m_async ? m_async->errstr : "alloc failed") << std::endl;
    if (m_async) { redisAsyncFree(m_async); m_async = nullptr; }
    return false;
  }
  m_async->data = this;
  if (!RedisAsyncAdapter::attach(m_async)) {
    redisAsyncFree(m_async); m_async = nullptr;
    return false;
  }
  redisAsyncSetDisconnectCallback(m_async, onAsyncDisconnectThunk);

  if (!m_cfg.password.empty()) {
    redisAsyncCommand(m_async, nullptr, nullptr, "AUTH %s", m_cfg.password.c_str());
  }
  if (m_cfg.db != 0) {
    redisAsyncCommand(m_async, nullptr, nullptr, "SELECT %d", m_cfg.db);
  }
  return true;
}

bool RedisStore::connectAsyncWrite(void) {
  if (!m_cfg.unix_socket.empty()) {
    m_async_write = redisAsyncConnectUnix(m_cfg.unix_socket.c_str());
  } else {
    m_async_write = redisAsyncConnect(m_cfg.host.c_str(), m_cfg.port);
  }
  if (!m_async_write || m_async_write->err) {
    std::cerr << "*** ERROR: Redis async-write connect failed: "
              << (m_async_write ? m_async_write->errstr : "alloc failed")
              << std::endl;
    if (m_async_write) { redisAsyncFree(m_async_write); m_async_write = nullptr; }
    return false;
  }
  m_async_write->data = this;
  if (!RedisAsyncAdapter::attach(m_async_write)) {
    redisAsyncFree(m_async_write); m_async_write = nullptr;
    return false;
  }
  redisAsyncSetDisconnectCallback(m_async_write, onAsyncWriteDisconnectThunk);

  if (!m_cfg.password.empty()) {
    redisAsyncCommand(m_async_write, nullptr, nullptr, "AUTH %s", m_cfg.password.c_str());
  }
  if (m_cfg.db != 0) {
    redisAsyncCommand(m_async_write, nullptr, nullptr, "SELECT %d", m_cfg.db);
  }
  return true;
}

void RedisStore::subscribe(void) {
  if (!m_async) return;
  std::string ch = channelName();
  redisAsyncCommand(m_async, onPubSubMessageThunk, this,
                    "SUBSCRIBE %s", ch.c_str());
}
void RedisStore::onAsyncDisconnect(int /*status*/) {
  if (m_shutting_down) return;
  std::cerr << "*** WARN: Redis async disconnect" << std::endl;
  // hiredis will free the context after this returns — just null out.
  m_async = nullptr;
  scheduleReconnect();
}

void RedisStore::onAsyncWriteDisconnect(int /*status*/) {
  if (m_shutting_down) return;
  std::cerr << "*** WARN: Redis async-write disconnect" << std::endl;
  m_async_write = nullptr;
  scheduleReconnect();
}

void RedisStore::scheduleReconnect(void) {
  if (m_shutting_down) return;
  delete m_reconnect_timer;
  m_reconnect_timer = new Async::Timer(m_reconnect_backoff_s * 1000,
                                       Async::Timer::TYPE_ONESHOT);
  m_reconnect_timer->expired.connect([this](Async::Timer*) {
    if (m_shutting_down) return;
    // Reconnect sub context if needed.
    if (!m_async && !connectAsync()) {
      m_reconnect_backoff_s = std::min(m_reconnect_backoff_s * 2, 30);
      scheduleReconnect();
      return;
    }
    if (!m_async_write && !connectAsyncWrite()) {
      m_reconnect_backoff_s = std::min(m_reconnect_backoff_s * 2, 30);
      scheduleReconnect();
      return;
    }
    // If sub was freshly reconnected, re-subscribe.
    subscribe();
    // Rebuild sync context too — its TCP socket is likely dead.
    if (m_sync) { redisFree(m_sync); m_sync = nullptr; }
    if (!connectSync()) {
      std::cerr << "*** WARN: Redis sync reconnect failed" << std::endl;
      // async is up but sync isn't — schedule another retry so we fix sync.
      m_reconnect_backoff_s = std::min(m_reconnect_backoff_s * 2, 30);
      scheduleReconnect();
      return;
    }
    std::cout << "RedisStore: reconnected, re-subscribing and requesting "
                 "full reload" << std::endl;
    m_reconnect_backoff_s = 1;
    configChanged.emit("all");
  });
}
void RedisStore::onPubSubMessage(const std::string&, const std::string&) {}
void RedisStore::drainLiveQueue(Async::Timer*) {
  if (!m_async_write || !m_live_queue) return;
  std::vector<RedisLiveQueue::Op> ops;
  m_live_queue->drain(ops);
  if (ops.empty()) return;

  for (auto& op : ops) {
    switch (op.op) {
      case RedisLiveQueue::OpType::HSET: {
        std::vector<const char*> argv;
        std::vector<size_t>      argl;
        argv.push_back("HSET"); argl.push_back(4);
        argv.push_back(op.key.c_str()); argl.push_back(op.key.size());
        for (auto& kv : op.fields) {
          argv.push_back(kv.first.c_str());  argl.push_back(kv.first.size());
          argv.push_back(kv.second.c_str()); argl.push_back(kv.second.size());
        }
        redisAsyncCommandArgv(m_async_write, nullptr, nullptr,
                              static_cast<int>(argv.size()),
                              argv.data(), argl.data());
        if (op.ttl_s > 0) {
          redisAsyncCommand(m_async_write, nullptr, nullptr,
                            "EXPIRE %s %d", op.key.c_str(), op.ttl_s);
        }
        break;
      }
      case RedisLiveQueue::OpType::DEL:
        redisAsyncCommand(m_async_write, nullptr, nullptr,
                          "DEL %s", op.key.c_str());
        break;
      case RedisLiveQueue::OpType::EXPIRE:
        redisAsyncCommand(m_async_write, nullptr, nullptr,
                          "EXPIRE %s %d", op.key.c_str(), op.ttl_s);
        break;
    }
  }

  // Notify dashboards once per drain cycle.
  std::string ch = keyFor("live.changed");
  redisAsyncCommand(m_async_write, nullptr, nullptr,
                    "PUBLISH %s tick", ch.c_str());
}
void RedisStore::refreshLiveExpire(Async::Timer*) {
  if (!m_live_queue) return;
  for (const std::string& k : m_live_keys) {
    RedisLiveQueue::Op op;
    op.op    = RedisLiveQueue::OpType::EXPIRE;
    op.key   = k;
    op.ttl_s = 60;
    m_live_queue->push(std::move(op));
  }
}

std::string RedisStore::keyFor(const std::string& suffix) const {
  if (m_cfg.key_prefix.empty()) return suffix;
  return m_cfg.key_prefix + ":" + suffix;
}

std::string RedisStore::channelName(void) const {
  return keyFor("config.changed");
}
