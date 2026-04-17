#ifndef REDIS_STORE_INCLUDED
#define REDIS_STORE_INCLUDED

#include <sigc++/sigc++.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <cstdint>

namespace Async { class Config; class Timer; }

#ifdef WITH_REDIS

struct redisContext;
struct redisAsyncContext;

class RedisAsyncAdapter;
class RedisLiveQueue;

class RedisStore : public sigc::trackable
{
  public:
    struct Config
    {
      std::string host;
      uint16_t    port = 6379;
      std::string password;
      int         db = 0;
      std::string key_prefix;
      std::string unix_socket;
      bool        tls_enabled = false;
      std::string tls_ca_cert;
      std::string tls_client_cert;
      std::string tls_client_key;
    };

    struct TrunkPeerConfig
    {
      std::string host;
      std::string port;
      std::string secret;
      std::string remote_prefix;
      std::string peer_id;
    };

    explicit RedisStore(const Config& cfg);
    ~RedisStore();

    // Synchronous startup connect. Returns false on failure (caller exits).
    bool connect(void);

    bool isReady(void) const;

    // Emitted when a config.changed message arrives from Redis. The
    // string payload identifies the scope: "users", "cluster",
    // "trunk:<section>", or "all".
    sigc::signal<void, std::string> configChanged;

    // Schema accessors (sync). Empty results on Redis errors are logged
    // and treated as "no entries".
    std::string lookupUserKey(const std::string& callsign);
    std::map<std::string, std::string> loadAllUsers(void);
    bool isUserEnabled(const std::string& callsign);

    std::set<uint32_t> loadClusterTgs(void);

    std::string loadTrunkFilter(const std::string& section,
                                const std::string& field);  // "blacklist"|"allow"
    std::map<uint32_t, uint32_t> loadTrunkTgMap(const std::string& section);
    std::map<std::string, TrunkPeerConfig> loadTrunkPeers(void);
    // Publish a config.changed scope token (sync).
    void publishConfigChanged(const std::string& scope);

    // Live-state push (non-blocking; enqueues onto live queue).
    void pushLiveTalker(uint32_t tg, const std::string& callsign,
                        const std::string& source);
    void clearLiveTalker(uint32_t tg);
    void pushLiveClient(const std::string& callsign,
                        const std::string& ip,
                        const std::string& codecs,
                        uint32_t current_tg);
    void clearLiveClient(const std::string& callsign);
    // Publish the rich per-client status blob (rx levels, monitoredTGs,
    // qth/location etc.) as a serialized JSON string in the `status`
    // field of the existing live:client:<callsign> hash.
    void pushClientStatus(const std::string& callsign,
                          const std::string& status_json);
    // Reflector-wide meta: mode, version, prefixes, listen ports, cluster
    // TGs, satellite-server stats. Stored under live:meta as a hash;
    // written event-driven (startup + on cfg/satellite-count changes).
    void pushMeta(const std::vector<std::pair<std::string,std::string>>& fields);
    // Per-satellite status snapshot. Stored under live:satellite:<sat_id>
    // as a hash with a `status` JSON field. Written when the satellite
    // hello is received and on per-satellite events; cleared on disconnect.
    void pushSatelliteStatus(const std::string& sat_id,
                             const std::string& status_json);
    void clearSatelliteStatus(const std::string& sat_id);
    // Extended per-trunk status (full TrunkLink::statusJson snapshot).
    // Stored as the `status` field on the existing live:trunk:<section>
    // hash. Written on trunk state-change events.
    void pushTrunkStatus(const std::string& section,
                         const std::string& status_json);
    void pushLiveTrunk(const std::string& section,
                       const std::string& state,
                       const std::string& peer_id);
    void pushPeerNode(const std::string& peer_id,
                      const std::string& callsign,
                      uint32_t tg,
                      float lat, float lon,
                      const std::string& qth_name);
    void clearPeerNode(const std::string& peer_id,
                       const std::string& callsign);

    uint64_t droppedLiveWrites(void) const { return m_dropped_live_writes; }
    size_t   liveQueueSize(void) const;

    void onAsyncDisconnect(int status);
    void onAsyncWriteDisconnect(int status);

  private:
    Config              m_cfg;
    redisContext*       m_sync = nullptr;
    redisAsyncContext*  m_async = nullptr;       // pub/sub subscriber only
    redisAsyncContext*  m_async_write = nullptr; // live-state writes (HSET/DEL)
    RedisAsyncAdapter*  m_adapter = nullptr;
    RedisLiveQueue*     m_live_queue = nullptr;
    Async::Timer*       m_drain_timer = nullptr;
    Async::Timer*       m_reconnect_timer = nullptr;
    Async::Timer*       m_heartbeat_timer = nullptr;
    int                 m_reconnect_backoff_s = 1;
    bool                m_shutting_down = false;
    uint64_t            m_dropped_live_writes = 0;
    std::set<std::string> m_live_keys;

    bool connectSync(void);
    bool connectAsync(void);
    bool connectAsyncWrite(void);
    void subscribe(void);
    void scheduleReconnect(void);
    void onPubSubMessage(const std::string& channel, const std::string& payload);
    void drainLiveQueue(Async::Timer*);
    void refreshLiveExpire(Async::Timer*);

    std::string keyFor(const std::string& suffix) const;
    std::string channelName(void) const;
};

#else /* !WITH_REDIS */

// Build-time no-op stub: lets the rest of the codebase compile and link
// when libhiredis is unavailable. connect() returns false so
// Reflector::initialize aborts startup if [REDIS] is configured —
// matching the existing "Redis is unreachable" path. The load*
// accessors return empty containers; the live-state pushes are no-ops.
class RedisStore : public sigc::trackable
{
  public:
    struct Config
    {
      std::string host;
      uint16_t    port = 6379;
      std::string password;
      int         db = 0;
      std::string key_prefix;
      std::string unix_socket;
      bool        tls_enabled = false;
      std::string tls_ca_cert;
      std::string tls_client_cert;
      std::string tls_client_key;
    };

    struct TrunkPeerConfig
    {
      std::string host;
      std::string port;
      std::string secret;
      std::string remote_prefix;
      std::string peer_id;
    };

    explicit RedisStore(const Config&) {}

    bool connect(void) { return false; }
    bool isReady(void) const { return false; }

    sigc::signal<void, std::string> configChanged;

    std::string lookupUserKey(const std::string&) { return {}; }
    std::map<std::string, std::string> loadAllUsers(void) { return {}; }
    bool isUserEnabled(const std::string&) { return false; }

    std::set<uint32_t> loadClusterTgs(void) { return {}; }

    std::string loadTrunkFilter(const std::string&,
                                const std::string&) { return {}; }
    std::map<uint32_t, uint32_t> loadTrunkTgMap(const std::string&) {
      return {};
    }
    std::map<std::string, TrunkPeerConfig> loadTrunkPeers(void) {
      return {};
    }
    void publishConfigChanged(const std::string&) {}

    void pushLiveTalker(uint32_t, const std::string&,
                        const std::string&) {}
    void clearLiveTalker(uint32_t) {}
    void pushLiveClient(const std::string&, const std::string&,
                        const std::string&, uint32_t) {}
    void clearLiveClient(const std::string&) {}
    void pushClientStatus(const std::string&, const std::string&) {}
    void pushMeta(const std::vector<std::pair<std::string,std::string>>&) {}
    void pushSatelliteStatus(const std::string&, const std::string&) {}
    void clearSatelliteStatus(const std::string&) {}
    void pushTrunkStatus(const std::string&, const std::string&) {}
    void pushLiveTrunk(const std::string&, const std::string&,
                       const std::string&) {}
    void pushPeerNode(const std::string&, const std::string&, uint32_t,
                      float, float, const std::string&) {}
    void clearPeerNode(const std::string&, const std::string&) {}

    uint64_t droppedLiveWrites(void) const { return 0; }
    size_t   liveQueueSize(void) const { return 0; }
};

#endif /* WITH_REDIS */

#endif /* REDIS_STORE_INCLUDED */
