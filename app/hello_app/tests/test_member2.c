#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ai_care.h"
#include "ai_llm.h"
#include "ai_sound_detect.h"
#include "ai_state_machine.h"

static int g_sound_callbacks;

static void sound_callback(sound_type_t type, float confidence,
                           void *user_data)
{
  (void)user_data;
  assert(type == SOUND_TYPE_FALL);
  assert(confidence >= SOUND_DETECT_THRESHOLD_DEFAULT);
  g_sound_callbacks++;
}

static void test_state_machine(void)
{
  sm_context_t ctx;
  assert(sm_init(&ctx) == OK);
  assert(sm_get_state(&ctx) == SM_STATE_IDLE);
  assert(sm_handle_event(&ctx, SM_EVENT_WAKEUP) == OK);
  assert(sm_get_state(&ctx) == SM_STATE_LISTENING);
  assert(strcmp(sm_get_state_name((sm_state_t)-1), "UNKNOWN") == 0);
  assert(strcmp(sm_get_event_name((sm_event_t)-1), "UNKNOWN") == 0);
  sm_deinit(&ctx);
}

static void test_llm_json(void)
{
  llm_context_t ctx;
  char json[4096];
  char tiny[8];

  assert(llm_init(&ctx, NULL) == OK);
  assert(llm_build_request_json(&ctx, "引号\"和换行\n测试",
                                json, sizeof(json)) == OK);
  assert(strstr(json, "\\\"") != NULL);
  assert(strstr(json, "\\n") != NULL);
  assert(llm_build_request_json(&ctx, "too large", tiny,
                                sizeof(tiny)) == -ENOSPC);
  assert(strcmp(llm_get_state_name((llm_state_t)-1), "UNKNOWN") == 0);
  llm_deinit(&ctx);
}

static void test_sound_detector(void)
{
  sound_detect_context_t ctx;
  sound_detect_config_t config =
  {
    .mode = DETECT_MODE_BATCH,
    .threshold = SOUND_DETECT_THRESHOLD_DEFAULT,
    .sample_rate = SOUND_DETECT_SAMPLE_RATE,
    .frame_ms = SOUND_DETECT_FRAME_MS,
    .callback = sound_callback
  };
  int16_t samples[SOUND_DETECT_FRAMES_PER_WINDOW] = {0};
  sound_type_t type;
  float confidence;

  for (size_t i = 0; i < SOUND_DETECT_FRAMES_PER_WINDOW; i += 80)
    {
      samples[i] = (i / 80) % 2 == 0 ? INT16_MAX : INT16_MIN;
    }

  assert(sound_detect_init(&ctx, &config) == OK);
  assert(sound_detect_once(&ctx, samples,
                           SOUND_DETECT_FRAMES_PER_WINDOW,
                           &type, &confidence) == OK);
  assert(type == SOUND_TYPE_FALL);
  assert(g_sound_callbacks == 1);
  assert(strcmp(sound_detect_get_type_name((sound_type_t)-1),
                "UNKNOWN") == 0);
  sound_detect_deinit(&ctx);
}

static void test_care_validation(void)
{
  care_context_t ctx;
  care_task_t invalid = {0};
  care_task_t valid = {0};

  assert(care_init(&ctx, NULL) == OK);
  invalid.type = CARE_TYPE_HEALTH;
  invalid.state = CARE_TASK_ENABLED;
  invalid.trigger = CARE_TRIGGER_INTERVAL;
  assert(care_add_task(&ctx, &invalid) == -EINVAL);

  valid.type = CARE_TYPE_HEALTH;
  valid.state = CARE_TASK_ENABLED;
  valid.trigger = CARE_TRIGGER_INTERVAL;
  valid.interval_minutes = 1;
  strcpy(valid.name, "测试提醒");
  strcpy(valid.message, "测试消息");
  assert(care_add_task(&ctx, &valid) >= 0);
  assert(care_get_task_list(&ctx, &valid, 0) == 0);
  assert(strcmp(care_get_type_name((care_type_t)-1), "未知类型") == 0);
  care_deinit(&ctx);
}

int main(void)
{
  test_state_machine();
  test_llm_json();
  test_sound_detector();
  test_care_validation();
  puts("member2 host tests: PASS");
  return 0;
}
