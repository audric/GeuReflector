#include "RedisAsyncAdapter.h"
#include <hiredis/hiredis.h>
#include <hiredis/async.h>
#include <AsyncFdWatch.h>

namespace {

struct AdapterData
{
  redisAsyncContext* ac;
  Async::FdWatch*    rd = nullptr;
  Async::FdWatch*    wr = nullptr;
};

void adapterAddRead(void* priv) {
  auto* d = static_cast<AdapterData*>(priv);
  if (d->rd) d->rd->setEnabled(true);
}
void adapterDelRead(void* priv) {
  auto* d = static_cast<AdapterData*>(priv);
  if (d->rd) d->rd->setEnabled(false);
}
void adapterAddWrite(void* priv) {
  auto* d = static_cast<AdapterData*>(priv);
  if (d->wr) d->wr->setEnabled(true);
}
void adapterDelWrite(void* priv) {
  auto* d = static_cast<AdapterData*>(priv);
  if (d->wr) d->wr->setEnabled(false);
}
void adapterCleanup(void* priv) {
  auto* d = static_cast<AdapterData*>(priv);
  delete d->rd;
  delete d->wr;
  delete d;
}

} // namespace

bool RedisAsyncAdapter::attach(redisAsyncContext* ac) {
  if (!ac || ac->ev.data != nullptr) return false;
  auto* d = new AdapterData;
  d->ac = ac;
  int fd = ac->c.fd;
  d->rd = new Async::FdWatch(fd, Async::FdWatch::FD_WATCH_RD);
  d->wr = new Async::FdWatch(fd, Async::FdWatch::FD_WATCH_WR);
  d->rd->setEnabled(false);
  d->wr->setEnabled(false);
  d->rd->activity.connect(
      [ac](Async::FdWatch*){ redisAsyncHandleRead(ac); });
  d->wr->activity.connect(
      [ac](Async::FdWatch*){ redisAsyncHandleWrite(ac); });
  ac->ev.data = d;
  ac->ev.addRead    = adapterAddRead;
  ac->ev.delRead    = adapterDelRead;
  ac->ev.addWrite   = adapterAddWrite;
  ac->ev.delWrite   = adapterDelWrite;
  ac->ev.cleanup    = adapterCleanup;
  return true;
}
