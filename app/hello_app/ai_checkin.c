#include "ai_checkin.h"
#include <errno.h>
#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static checkin_snapshot_t current;

int ai_checkin_begin(uint64_t now_ms, uint32_t timeout_ms, uint64_t *id)
{
  if (!id || !timeout_ms || now_ms > UINT64_MAX - timeout_ms)
    return -EINVAL;

  pthread_mutex_lock(&lock);
  int ret = 0;

  if (current.state == CHECKIN_WAITING ||
      current.state == CHECKIN_READY ||
      current.state == CHECKIN_SENDING ||
      current.state == CHECKIN_FAILED)
    ret = -EBUSY;
  else if (current.id == UINT64_MAX)
    ret = -EOVERFLOW;
  else {
    uint64_t next = current.id + 1;
    current = (checkin_snapshot_t){
      .id = next,
      .deadline_ms = now_ms + timeout_ms,
      .state = CHECKIN_WAITING
    };
    *id = next;
  }

  pthread_mutex_unlock(&lock);
  return ret;
}

int ai_checkin_respond(uint64_t id, bool needs_help, uint64_t now_ms)
{
  pthread_mutex_lock(&lock);
  int ret = 0;

  if (!id || id != current.id)
    ret = -ENOENT;
  else if (current.state != CHECKIN_WAITING)
    ret = -EALREADY;
  else if (now_ms >= current.deadline_ms) {
    current.state = CHECKIN_READY;
    ret = -ETIMEDOUT;
  } else {
    current.user_requested_help = needs_help;
    current.state = needs_help ? CHECKIN_READY : CHECKIN_CONFIRMED;
  }

  pthread_mutex_unlock(&lock);
  return ret;
}

void ai_checkin_tick(uint64_t now_ms)
{
  pthread_mutex_lock(&lock);

  if (current.state == CHECKIN_WAITING &&
      now_ms >= current.deadline_ms)
    current.state = CHECKIN_READY;

  pthread_mutex_unlock(&lock);
}

int ai_checkin_claim(uint64_t id, unsigned *attempt)
{
  if (!attempt)
    return -EINVAL;

  pthread_mutex_lock(&lock);
  int ret = 0;

  if (!id || id != current.id)
    ret = -ENOENT;
  else if (current.state != CHECKIN_READY)
    ret = -EALREADY;
  else {
    current.state = CHECKIN_SENDING;
    *attempt = ++current.attempts;
  }

  pthread_mutex_unlock(&lock);
  return ret;
}

int ai_checkin_finish(uint64_t id, unsigned attempt, int error)
{
  if (error > 0)
    return -EINVAL;

  pthread_mutex_lock(&lock);
  int ret = 0;

  if (!id || id != current.id)
    ret = -ENOENT;
  else if (current.state != CHECKIN_SENDING ||
           attempt != current.attempts)
    ret = -EALREADY;
  else {
    current.last_error = error;
    current.state = error == 0 ? CHECKIN_SENT : CHECKIN_FAILED;
  }

  pthread_mutex_unlock(&lock);
  return ret;
}

int ai_checkin_retry(uint64_t id)
{
  pthread_mutex_lock(&lock);
  int ret = 0;

  if (!id || id != current.id)
    ret = -ENOENT;
  else if (current.state != CHECKIN_FAILED)
    ret = -EALREADY;
  else if (current.attempts >= 3)
    ret = -EOVERFLOW;
  else
    current.state = CHECKIN_READY;

  pthread_mutex_unlock(&lock);
  return ret;
}

/* Explicit acknowledgement; never changes failure into success. */
int ai_checkin_close_failed(uint64_t id)
{
  pthread_mutex_lock(&lock);
  int ret = 0;

  if (!id || id != current.id)
    ret = -ENOENT;
  else if (current.state != CHECKIN_FAILED)
    ret = -EALREADY;
  else
    current.state = CHECKIN_CLOSED;

  pthread_mutex_unlock(&lock);
  return ret;
}

checkin_snapshot_t ai_checkin_snapshot(void)
{
  pthread_mutex_lock(&lock);
  checkin_snapshot_t result = current;
  pthread_mutex_unlock(&lock);
  return result;
}
