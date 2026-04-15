#ifndef REDIS_ASYNC_ADAPTER_INCLUDED
#define REDIS_ASYNC_ADAPTER_INCLUDED

struct redisAsyncContext;

class RedisAsyncAdapter
{
  public:
    // Attach hiredis async callbacks to the Async event loop via FdWatch.
    // Returns false if the context is already attached or invalid.
    // The adapter cleans itself up when hiredis invokes ev.cleanup
    // (on disconnect/free).
    static bool attach(redisAsyncContext* ac);
};

#endif /* REDIS_ASYNC_ADAPTER_INCLUDED */
