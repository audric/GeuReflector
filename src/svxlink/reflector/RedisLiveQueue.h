#ifndef REDIS_LIVE_QUEUE_INCLUDED
#define REDIS_LIVE_QUEUE_INCLUDED

#include <string>
#include <vector>
#include <deque>
#include <cstdint>
#include <cstddef>

class RedisLiveQueue
{
  public:
    enum class OpType { HSET, DEL, EXPIRE };

    struct Op {
      OpType                                             op;
      std::string                                        key;
      std::vector<std::pair<std::string, std::string>>   fields;   // HSET
      int                                                ttl_s = 0;  // EXPIRE
    };

    explicit RedisLiveQueue(size_t max_pending = 4096);

    // Push. On overflow, drops the oldest entry and increments the
    // dropped counter. Always returns true (push never fails).
    bool push(Op op);

    // Drain everything currently queued into `out`. Returns count moved.
    size_t drain(std::vector<Op>& out);

    uint64_t droppedCount(void) const { return m_dropped; }
    size_t   size(void) const { return m_q.size(); }

  private:
    std::deque<Op> m_q;
    size_t         m_max;
    uint64_t       m_dropped = 0;
};

#endif /* REDIS_LIVE_QUEUE_INCLUDED */
