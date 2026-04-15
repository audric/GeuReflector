#ifndef REDIS_STORE_INCLUDED
#define REDIS_STORE_INCLUDED

#include <sigc++/sigc++.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <cstdint>

struct redisContext;
struct redisAsyncContext;
namespace Async { class Config; class Timer; }

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
    std::set<std::string> loadTrunkMutes(const std::string& section);

    // Mute set ops (sync, used by PTY persistence).
    void addTrunkMute(const std::string& section, const std::string& callsign);
    void removeTrunkMute(const std::string& section, const std::string& callsign);

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
    void pushLiveTrunk(const std::string& section,
                       const std::string& state,
                       const std::string& peer_id);

    uint64_t droppedLiveWrites(void) const { return m_dropped_live_writes; }
    size_t   liveQueueSize(void) const;

    void onAsyncDisconnect(int status);

  private:
    Config              m_cfg;
    redisContext*       m_sync = nullptr;
    redisAsyncContext*  m_async = nullptr;
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
    void subscribe(void);
    void scheduleReconnect(void);
    void onPubSubMessage(const std::string& channel, const std::string& payload);
    void drainLiveQueue(Async::Timer*);
    void refreshLiveExpire(Async::Timer*);

    std::string keyFor(const std::string& suffix) const;
    std::string channelName(void) const;
};

#endif /* REDIS_STORE_INCLUDED */
