#include "ai_checkin.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>

static uint64_t race_id;
static int claims[2];
static unsigned tickets[2];

static void *claim_worker(void *arg)
{
  int i = *(int *)arg;
  claims[i] = ai_checkin_claim(race_id, &tickets[i]);
  return NULL;
}

int main(void)
{
  uint64_t id, old;
  unsigned attempt;

  assert(ai_checkin_begin(0, 0, &id) == -EINVAL);
  assert(ai_checkin_begin(UINT64_MAX, 1, &id) == -EINVAL);

  /* Confirmation and duplicate clicks. */
  assert(ai_checkin_begin(1000, 100, &id) == 0);
  old = id;
  assert(ai_checkin_begin(1001, 100, &id) == -EBUSY);
  assert(ai_checkin_respond(id, false, 1099) == 0);
  assert(ai_checkin_respond(id, true, 1099) == -EALREADY);
  ai_checkin_tick(2000);
  assert(ai_checkin_snapshot().state == CHECKIN_CONFIRMED);
  assert(ai_checkin_claim(id, &attempt) == -EALREADY);

  /* Old UI event and exact timeout boundary. */
  assert(ai_checkin_begin(2000, 100, &id) == 0);
  assert(id != old);
  assert(ai_checkin_respond(old, false, 2001) == -ENOENT);
  assert(ai_checkin_respond(id, false, 2100) == -ETIMEDOUT);
  assert(ai_checkin_snapshot().state == CHECKIN_READY);
  assert(!ai_checkin_snapshot().user_requested_help);

  /* Only one concurrent sender may claim this attempt. */
  race_id = id;
  pthread_t threads[2];
  int indexes[2] = {0, 1};

  for (int i = 0; i < 2; ++i)
    assert(pthread_create(
      &threads[i], NULL, claim_worker, &indexes[i]) == 0);

  for (int i = 0; i < 2; ++i)
    assert(pthread_join(threads[i], NULL) == 0);

  assert((claims[0] == 0) + (claims[1] == 0) == 1);
  attempt = claims[0] == 0 ? tickets[0] : tickets[1];

  /* Failed send, explicit retry, reject late old result. */
  assert(ai_checkin_finish(id, attempt, -EIO) == 0);
  assert(ai_checkin_snapshot().state == CHECKIN_FAILED);
  assert(ai_checkin_retry(id) == 0);
  assert(ai_checkin_claim(id, &attempt) == 0 && attempt == 2);
  assert(ai_checkin_finish(id, 1, 0) == -EALREADY);
  assert(ai_checkin_finish(id, attempt, 0) == 0);
  assert(ai_checkin_finish(id, attempt, 0) == -EALREADY);

  /* Explicit help request and retry limit. */
  assert(ai_checkin_begin(3000, 100, &id) == 0);
  assert(ai_checkin_respond(id, true, 3001) == 0);
  assert(ai_checkin_snapshot().user_requested_help);

  for (unsigned i = 1; i <= 3; ++i) {
    assert(ai_checkin_claim(id, &attempt) == 0 && attempt == i);
    assert(ai_checkin_finish(id, attempt, -EIO) == 0);
    assert(ai_checkin_retry(id) == (i < 3 ? 0 : -EOVERFLOW));
  }

  assert(ai_checkin_snapshot().state == CHECKIN_FAILED);
  assert(ai_checkin_close_failed(id) == 0);
  assert(ai_checkin_snapshot().last_error == -EIO);

  /* Repeated polling must not create additional attempts. */
  assert(ai_checkin_begin(4000, 100, &id) == 0);
  ai_checkin_tick(4099);
  assert(ai_checkin_snapshot().state == CHECKIN_WAITING);
  ai_checkin_tick(4100);
  ai_checkin_tick(4200);
  assert(ai_checkin_snapshot().state == CHECKIN_READY);
  assert(ai_checkin_snapshot().attempts == 0);

  puts("checkin host tests: PASS");
  return 0;
}
