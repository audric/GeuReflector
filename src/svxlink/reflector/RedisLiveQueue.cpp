#include "RedisLiveQueue.h"

RedisLiveQueue::RedisLiveQueue(size_t max_pending) : m_max(max_pending) {}

bool RedisLiveQueue::push(Op op) {
  if (m_q.size() >= m_max) {
    m_q.pop_front();
    ++m_dropped;
  }
  m_q.push_back(std::move(op));
  return true;
}

size_t RedisLiveQueue::drain(std::vector<Op>& out) {
  size_t n = m_q.size();
  out.reserve(out.size() + n);
  while (!m_q.empty()) {
    out.push_back(std::move(m_q.front()));
    m_q.pop_front();
  }
  return n;
}
