#pragma once

#include <Arduino.h>
#include <Utils.h>

// fixed-size session table -- no heap allocation, no std::map/vector
#ifndef STOOP_MAX_SESSIONS
#define STOOP_MAX_SESSIONS 16
#endif

#define STOOP_SESSION_TOKEN_LEN 16

// static firmware-configured limits (no runtime/admin adjustment)
#ifndef STOOP_USER_CAPACITY
#define STOOP_USER_CAPACITY 5
#endif
#ifndef STOOP_USER_REFILL_MS
#define STOOP_USER_REFILL_MS 15000
#endif
#ifndef STOOP_NODE_CAPACITY
#define STOOP_NODE_CAPACITY 30
#endif
#ifndef STOOP_NODE_REFILL_MS
#define STOOP_NODE_REFILL_MS 60000
#endif

/*
The token bucket is used to rate-limit the number of messages a user can send in a given
time window. Each user has their own token bucket, and there is also a global token bucket 
for the node. When a user sends a message, it consumes a token from both their personal bucket 
and the global bucket. If either bucket is empty, the message is rejected. The buckets 
refill over time at a fixed rate, allowing users to send messages again after waiting for 
tokens to become available.
*/
struct TokenBucket {
  uint32_t capacity;
  uint32_t refill_ms;
  uint32_t tokens;
  uint32_t last_refill;

  void reset(uint32_t cap, uint32_t interval_ms, uint32_t now) {
    capacity = cap;
    refill_ms = interval_ms > 0 ? interval_ms : 1;
    tokens = cap;
    last_refill = now;
  }

  // must be called before reading/consuming tokens
  void refill(uint32_t now) {
    uint32_t elapsed = now - last_refill;
    uint32_t earned = elapsed / refill_ms;
    tokens = tokens + earned;
    if (tokens > capacity) tokens = capacity;
    last_refill = now - (elapsed % refill_ms);
  }

  bool tryConsume(uint32_t now) {
    refill(now);
    if (tokens == 0) return false;
    tokens--;
    return true;
  }

  // returns 0 if a token is available now or the number of ms until the next token is available
  uint32_t msUntilNextToken(uint32_t now) {
    refill(now);
    if (tokens > 0) return 0;
    return refill_ms - (now - last_refill); 
  }
};

struct StoopSession {
  uint8_t token[STOOP_SESSION_TOKEN_LEN];
  TokenBucket bucket;
};

class RateLimiter {
public:
  void begin(mesh::RNG* rng);

  // returns true if the request is allowed, false if rate-limited
  // checks both the per-user and per-node buckets, and returns the remaining tokens and next refill times
  // consumes a token if the request is allowed
  bool tryConsume(const uint8_t* presented_token, bool token_valid, uint32_t now,
                   uint8_t out_token[STOOP_SESSION_TOKEN_LEN],
                   uint32_t& user_tokens_remaining, uint32_t& user_next_ms,
                   uint32_t& node_tokens_remaining, uint32_t& node_next_ms);
   
  // gets the status of the rate limiter without consuming a token
  void getStatus(const uint8_t* presented_token, bool token_valid, uint32_t now,
                  uint8_t out_token[STOOP_SESSION_TOKEN_LEN],
                  uint32_t& user_tokens_remaining, uint32_t& user_next_ms,
                  uint32_t& node_tokens_remaining, uint32_t& node_next_ms);

private:
  mesh::RNG* _rng;
  TokenBucket _node_bucket;
  StoopSession _sessions[STOOP_MAX_SESSIONS];
  int _next_slot; 
  int _filled;    

  StoopSession* resolveSession(const uint8_t* presented_token, bool token_valid, uint32_t now);
  StoopSession* findByToken(const uint8_t* token);
  StoopSession* allocateSession();
};
