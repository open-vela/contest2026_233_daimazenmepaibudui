#ifndef AI_CHECKIN_H
#define AI_CHECKIN_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  CHECKIN_IDLE,
  CHECKIN_WAITING,
  CHECKIN_CONFIRMED,
  CHECKIN_READY,
  CHECKIN_SENDING,
  CHECKIN_SENT,
  CHECKIN_FAILED,
  CHECKIN_CLOSED
} checkin_state_t;

typedef struct {
  uint64_t id;
  uint64_t deadline_ms;
  checkin_state_t state;
  unsigned attempts;
  bool user_requested_help;
  int last_error;
} checkin_snapshot_t;

/*
 * One active check-in per process.
 * IDs are unique only within this process lifetime.
 * now_ms must come from a monotonic clock.
 * No network, UI or persistent storage operations are performed here.
 */
int ai_checkin_begin(uint64_t now_ms, uint32_t timeout_ms, uint64_t *id);
int ai_checkin_respond(uint64_t id, bool needs_help, uint64_t now_ms);
void ai_checkin_tick(uint64_t now_ms);
int ai_checkin_claim(uint64_t id, unsigned *attempt);
int ai_checkin_finish(uint64_t id, unsigned attempt, int error);
int ai_checkin_retry(uint64_t id);
int ai_checkin_close_failed(uint64_t id);
checkin_snapshot_t ai_checkin_snapshot(void);

#endif
