#include "RateLimiter.h"

void RateLimiter::begin(mesh::RNG* rng) {
  _rng = rng;
  memset(_sessions, 0, sizeof(_sessions));
  _next_slot = 0;
  _filled = 0;
  _node_bucket.reset(STOOP_NODE_CAPACITY, STOOP_NODE_REFILL_MS, millis());
}

StoopSession* RateLimiter::findByToken(const uint8_t* token) {
  for (int i = 0; i < _filled; i++) {
    if (memcmp(_sessions[i].token, token, STOOP_SESSION_TOKEN_LEN) == 0) {
      return &_sessions[i];
    }
  }
  return NULL;
}

StoopSession* RateLimiter::allocateSession() {
  // allocate the next slot in the buffer, overwriting the oldest session if full
  StoopSession* s = &_sessions[_next_slot];
  _next_slot = (_next_slot + 1) % STOOP_MAX_SESSIONS;
  if (_filled < STOOP_MAX_SESSIONS) _filled++;
  return s;
}

StoopSession* RateLimiter::resolveSession(const uint8_t* presented_token, bool token_valid, uint32_t now) {
  StoopSession* s = token_valid ? findByToken(presented_token) : NULL;

  if (!s) {
    s = allocateSession();
    _rng->random(s->token, STOOP_SESSION_TOKEN_LEN);
    s->bucket.reset(STOOP_USER_CAPACITY, STOOP_USER_REFILL_MS, now);
  }

  return s;
}

bool RateLimiter::tryConsume(const uint8_t* presented_token, bool token_valid, uint32_t now,
                              uint8_t out_token[STOOP_SESSION_TOKEN_LEN],
                              uint32_t& user_tokens_remaining, uint32_t& user_next_ms,
                              uint32_t& node_tokens_remaining, uint32_t& node_next_ms) {
  StoopSession* s = resolveSession(presented_token, token_valid, now);
  memcpy(out_token, s->token, STOOP_SESSION_TOKEN_LEN);

  s->bucket.refill(now);
  _node_bucket.refill(now);

  user_tokens_remaining = s->bucket.tokens;
  user_next_ms = s->bucket.msUntilNextToken(now);
  node_tokens_remaining = _node_bucket.tokens;
  node_next_ms = _node_bucket.msUntilNextToken(now);
  
  bool user_ok = s->bucket.tokens > 0;
  bool node_ok = _node_bucket.tokens > 0;

  if (user_ok && node_ok) {
    s->bucket.tokens--;
    _node_bucket.tokens--;
  }

  return user_ok && node_ok;
}

void RateLimiter::getStatus(const uint8_t* presented_token, bool token_valid, uint32_t now,
                             uint8_t out_token[STOOP_SESSION_TOKEN_LEN],
                             uint32_t& user_tokens_remaining, uint32_t& user_next_ms,
                             uint32_t& node_tokens_remaining, uint32_t& node_next_ms) {
  StoopSession* s = resolveSession(presented_token, token_valid, now);
  memcpy(out_token, s->token, STOOP_SESSION_TOKEN_LEN);

  s->bucket.refill(now);
  _node_bucket.refill(now);

  user_tokens_remaining = s->bucket.tokens;
  user_next_ms = s->bucket.msUntilNextToken(now);
  node_tokens_remaining = _node_bucket.tokens;
  node_next_ms = _node_bucket.msUntilNextToken(now);
}
