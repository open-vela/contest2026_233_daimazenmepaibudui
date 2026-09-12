/****************************************************************************
 * AI Companion Main Entry
 * 智爱陪伴 - AI老人陪伴守护终端
 * 主程序入口 - 初始化并运行状态机
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>

#include "ai_state_machine.h"
#include "ai_audio.h"
#include "ai_llm.h"
#include "ai_sound_detect.h"
#include "ai_care.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 主循环间隔(毫秒) */
#define MAIN_LOOP_INTERVAL_MS    100

/* 程序退出标志 */
static volatile bool g_running = true;
static bool g_audio_started;
static bool g_sound_started;
static bool g_care_started;

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 全局状态机上下文 */
static sm_context_t g_sm_ctx;

/* 全局音频模块上下文 */
static audio_context_t g_audio_ctx;

/* 全局LLM模块上下文 */
static llm_context_t g_llm_ctx;

/* 全局声音检测上下文 */
static sound_detect_context_t g_sound_ctx;

/* 全局主动关怀上下文 */
static care_context_t g_care_ctx;

static void print_usage(const char *program)
{
  printf("用法: %s [选项]\n", program);
  printf("  --ask <文本>          启动后向 MiMo/ai_agent 发送文本\n");
  printf("  --sound-self-test     注入测试冲击声，验证检测链路\n");
  printf("  --help                显示帮助\n");
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/**
 * @brief  信号处理函数
 */

static void signal_handler(int signo)
{
  printf("收到信号 %d, 准备退出...\n", signo);
  g_running = false;
}

/**
 * @brief  VAD回调 - 语音活动检测
 */

static void vad_callback(bool speech_detected, void *user_data)
{
  sm_context_t *ctx = (sm_context_t *)user_data;

  if (speech_detected)
    {
      printf("[VAD] 检测到语音开始\n");
      /* 语音开始，触发唤醒事件 */
      sm_handle_event(ctx, SM_EVENT_WAKEUP);
    }
  else
    {
      printf("[VAD] 语音结束\n");
      /* 语音结束，触发语音完成事件 */
      sm_handle_event(ctx, SM_EVENT_VOICE_COMPLETE);
    }
}

static void audio_data_callback(const int16_t *data, size_t frames,
                                void *user_data)
{
  (void)user_data;

  if (g_sound_started)
    {
      int ret = sound_detect_feed(&g_sound_ctx, data, frames);
      if (ret < 0 && ret != -ENOSPC)
        {
          printf("[安全] 音频送入检测器失败: %d\n", ret);
        }
    }
}

/**
 * @brief  初始化音频并启动VAD监听
 */

static int start_audio_listening(sm_context_t *ctx)
{
  int ret;

  /* 启用VAD检测 */

  audio_vad_enable(&g_audio_ctx, vad_callback, ctx);

  /* 开始录音 */

  audio_record_config_t record_cfg =
  {
    .enable_vad = true,
    .silence_timeout_ms = AUDIO_VAD_SILENCE_TIMEOUT_MS,
    .min_speech_ms = AUDIO_VAD_MIN_SPEECH_MS,
    .data_callback = audio_data_callback,
    .user_data = ctx
  };

  ret = audio_record_start(&g_audio_ctx, &record_cfg);
  if (ret < 0)
    {
      printf("[错误] 启动录音失败: %d\n", ret);
      return ret;
    }

  printf("[音频] 开始语音监听\n");
  g_audio_started = true;
  return OK;
}

/**
 * @brief  停止音频监听
 */

static void stop_audio_listening(void)
{
  if (!g_audio_started)
    {
      return;
    }

  audio_record_stop(&g_audio_ctx);
  audio_vad_disable(&g_audio_ctx);
  g_audio_started = false;
  printf("[音频] 停止语音监听\n");
}

/**
 * @brief  异常声音检测回调
 */

static void sound_detect_callback(sound_type_t type, float confidence,
                                  void *user_data)
{
  sm_context_t *ctx = (sm_context_t *)user_data;

  printf("[安全] 检测到异常声音: %s (置信度: %.2f)\n",
         sound_detect_get_type_name(type), confidence);

  /* 触发异常报警事件 */

  sm_handle_event(ctx, SM_EVENT_ALARM_DETECTED);

  /* TODO: 根据异常类型采取不同措施 */
  /* - SOUND_TYPE_HELP: 立即呼叫紧急联系人 */
  /* - SOUND_TYPE_FALL: 记录并通知家人 */
  /* - SOUND_TYPE_SCREAM: 启动录音取证 */
}

/**
 * @brief  初始化并启动声音检测
 */

static int start_sound_detection(sm_context_t *ctx)
{
  int ret;

  /* 配置声音检测 */

  sound_detect_config_t detect_cfg =
  {
    .mode = DETECT_MODE_REALTIME,
    .threshold = SOUND_DETECT_THRESHOLD_DEFAULT,
    .sample_rate = SOUND_DETECT_SAMPLE_RATE,
    .frame_ms = SOUND_DETECT_FRAME_MS,
    .enable_vad = true,
    .enable_feedback = true,
    .callback = sound_detect_callback,
    .user_data = ctx
  };

  ret = sound_detect_init(&g_sound_ctx, &detect_cfg);
  if (ret < 0)
    {
      printf("[错误] 声音检测初始化失败: %d\n", ret);
      return ret;
    }

  /* TODO: 加载训练好的Edge Impulse模型 */
  /* ret = sound_detect_load_model_file(&g_sound_ctx, "/data/model.bin"); */

  /* 启动实时检测 */

  ret = sound_detect_start(&g_sound_ctx);
  if (ret < 0)
    {
      printf("[错误] 启动声音检测失败: %d\n", ret);
      sound_detect_deinit(&g_sound_ctx);
      return ret;
    }

  printf("[安全] 声音检测已启动\n");
  g_sound_started = true;
  return OK;
}

/**
 * @brief  停止声音检测
 */

static void stop_sound_detection(void)
{
  if (!g_sound_started)
    {
      return;
    }

  sound_detect_stop(&g_sound_ctx);
  sound_detect_deinit(&g_sound_ctx);
  g_sound_started = false;
  printf("[安全] 声音检测已停止\n");
}

/**
 * @brief  关怀回调函数
 */

static void care_callback(care_type_t type, const char *message,
                          void *user_data)
{
  sm_context_t *ctx = (sm_context_t *)user_data;

  printf("[关怀] 触发关怀: %s\n", care_get_type_name(type));
  printf("[关怀] 消息: %s\n", message);

  /* 触发关怀事件 */

  sm_handle_event(ctx, SM_EVENT_CARE_TIMER);

  /* TODO: 将消息转换为语音播放 */
  /* 1. 调用TTS服务 */
  /* 2. 播放语音 */

  printf("[关怀] 关怀完成\n");
}

/**
 * @brief  初始化并启动主动关怀
 */

static int start_care(sm_context_t *ctx)
{
  int ret;

  /* 配置主动关怀 */

  care_config_t care_cfg =
  {
    .enable_greeting = true,
    .enable_health = true,
    .enable_life = true,
    .enable_emotion = true,
    .enable_exercise = true,
    .enable_medicine = true,
    .enable_meal = true,
    .enable_sleep = true,
    .smart_mode = true,
    .callback = care_callback,
    .user_data = ctx
  };

  ret = care_init(&g_care_ctx, &care_cfg);
  if (ret < 0)
    {
      printf("[错误] 主动关怀初始化失败: %d\n", ret);
      return ret;
    }

  /* 启动关怀 */

  ret = care_start(&g_care_ctx);
  if (ret < 0)
    {
      printf("[错误] 启动主动关怀失败: %d\n", ret);
      care_deinit(&g_care_ctx);
      return ret;
    }

  printf("[关怀] 主动关怀已启动\n");
  g_care_started = true;

  /* 打印任务列表 */
  /* 注意: care_task_t 含 256B message 等, 20 个约 9KB, 放栈上会溢出
   * hello_app 任务栈(8KB), 因此用 static 一次性缓冲 */
  static care_task_t tasks[CARE_MAX_TASKS];
  int count = care_get_task_list(&g_care_ctx, tasks, CARE_MAX_TASKS);
  printf("[关怀] 当前关怀任务: %d 个\n", count);

  return OK;
}

/**
 * @brief  停止主动关怀
 */

static void stop_care(void)
{
  if (!g_care_started)
    {
      return;
    }

  care_stop(&g_care_ctx);
  care_deinit(&g_care_ctx);
  g_care_started = false;
  printf("[关怀] 主动关怀已停止\n");
}

/**
 * @brief  模拟语音唤醒检测
 * @note   实际实现由成员一提供音频驱动接口
 */

static void simulate_wakeup_detection(sm_context_t *ctx)
{
  /* TODO: 替换为真实的语音唤醒检测 */
  /* wakeup_listen_start(); */
  /* if (wakeup_word_detected()) { */
  /*     sm_handle_event(ctx, SM_EVENT_WAKEUP); */
  /* } */
}

/**
 * @brief  模拟异常声音检测
 * @note   实际实现由成员二集成声音分类模型
 */

static void simulate_alarm_detection(sm_context_t *ctx)
{
  /* TODO: 替换为真实的声音检测 */
  /* if (sound_detect_alarm()) { */
  /*     sm_handle_event(ctx, SM_EVENT_ALARM_DETECTED); */
  /* } */
}

/**
 * @brief  模拟主动关怀定时器
 * @note   实际实现由成员二设计关怀逻辑
 */

static void simulate_care_timer(sm_context_t *ctx)
{
  /* TODO: 实现定时关怀逻辑 */
  /* static uint32_t last_care_time = 0; */
  /* uint32_t now = get_tick_ms(); */
  /* if (now - last_care_time > CARE_INTERVAL_MS) { */
  /*     sm_handle_event(ctx, SM_EVENT_CARE_TIMER); */
  /*     last_care_time = now; */
  /* } */
}

/**
 * @brief  播放完成回调
 */

static void play_complete_callback(void *user_data)
{
  sm_context_t *ctx = (sm_context_t *)user_data;
  printf("[音频] 语音播放完成\n");

  /* 播放完成后触发AI响应完成事件 */

  sm_handle_event(ctx, SM_EVENT_AI_RESPONSE);
}

/**
 * @brief  AI回复完成回调
 */

static void llm_complete_callback(const char *response, int error,
                                  void *user_data)
{
  sm_context_t *ctx = (sm_context_t *)user_data;

  if (error != 0)
    {
      printf("[LLM] AI请求失败: %d\n", error);
      sm_handle_event(ctx, SM_EVENT_AI_ERROR);
      return;
    }

  printf("[LLM] AI回复: %s\n", response);

  /* TODO: 将文本转换为语音并播放 */
  /* 1. 调用TTS服务将文本转为音频 */
  /* 2. 播放音频 */

  /* 模拟播放 */

  /* audio_play_start(&g_audio_ctx, tts_audio, frames, */
  /*                  play_complete_callback, ctx); */

  /* 模拟播放完成 */

  sm_handle_event(ctx, SM_EVENT_AI_RESPONSE);
}

/**
 * @brief  处理AI对话（在AI_TALKING状态调用）
 */

static void process_ai_dialogue(sm_context_t *ctx)
{
  /* 获取最后录制的音频数据 */

  /* TODO: 从音频模块获取录制的音频数据 */

  /* 模拟音频数据 */

  printf("[AI] 开始AI对话处理\n");

  /* 发送到LLM */

  /* 模拟用户输入文本 */

  const char *user_text = "你好，请问今天天气怎么样？";

  int ret = llm_send_text(&g_llm_ctx, user_text,
                          NULL, llm_complete_callback, ctx);
  if (ret < 0)
    {
      printf("[AI] 发送请求失败: %d\n", ret);
      sm_handle_event(ctx, SM_EVENT_AI_ERROR);
    }
}

/**
 * @brief  主循环任务
 */

static void *main_loop_task(void *arg)
{
  sm_context_t *ctx = (sm_context_t *)arg;

  printf("主循环任务启动\n");

  while (g_running)
    {
      /* 1. 运行状态机（检查超时等） */

      sm_run(ctx);

      /* 2. 轮询检测各种事件 */

      simulate_wakeup_detection(ctx);
      simulate_alarm_detection(ctx);
      simulate_care_timer(ctx);

      /* 3. 休眠等待 */

      usleep(MAIN_LOOP_INTERVAL_MS * 1000);
    }

  printf("主循环任务退出\n");
  return NULL;
}

/**
 * @brief  状态机调试输出
 */

static void print_system_status(sm_context_t *ctx)
{
  printf("\n========================================\n");
  printf("  智爱陪伴 - AI老人陪伴守护终端\n");
  printf("========================================\n");
  printf("  当前状态: %s\n", sm_get_state_name(ctx->current_state));
  printf("  上一状态: %s\n", sm_get_state_name(ctx->prev_state));
  printf("  最后事件: %s\n", sm_get_event_name(ctx->last_event));
  printf("  状态机状态: %s\n",
         ctx->initialized ? "已初始化" : "未初始化");
  printf("========================================\n\n");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/**
 * @brief  主函数入口
 */

int main(int argc, char *argv[])
{
  int ret;
  pthread_t loop_thread;
  const char *startup_text = NULL;
  bool sound_self_test = false;

  for (int i = 1; i < argc; i++)
    {
      if (strcmp(argv[i], "--help") == 0)
        {
          print_usage(argv[0]);
          return 0;
        }
      else if (strcmp(argv[i], "--ask") == 0 && i + 1 < argc)
        {
          startup_text = argv[++i];
        }
      else if (strcmp(argv[i], "--sound-self-test") == 0)
        {
          sound_self_test = true;
        }
      else
        {
          printf("未知或不完整的参数: %s\n", argv[i]);
          print_usage(argv[0]);
          return -EINVAL;
        }
    }

  printf("\n");
  printf("╔══════════════════════════════════════════╗\n");
  printf("║  智爱陪伴 - AI老人陪伴守护终端 v1.0     ║\n");
  printf("║  Powered by openvela                    ║\n");
  printf("╚══════════════════════════════════════════╝\n");
  printf("\n");

  /* 注册信号处理 */

  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  /* 1. 初始化状态机 */

  printf("[初始化] 正在初始化状态机...\n");
  ret = sm_init(&g_sm_ctx);
  if (ret < 0)
    {
      printf("[错误] 状态机初始化失败: %d\n", ret);
      return ret;
    }

  /* 2. 初始化音频模块 */

  printf("[初始化] 正在初始化音频模块...\n");
  ret = audio_init(&g_audio_ctx, NULL);
  if (ret < 0)
    {
      printf("[错误] 音频模块初始化失败: %d\n", ret);
      sm_deinit(&g_sm_ctx);
      return ret;
    }

  /* 3. 初始化LLM模块 */

  printf("[初始化] 正在初始化AI对话模块...\n");
  ret = llm_init(&g_llm_ctx, NULL);
  if (ret < 0)
    {
      printf("[错误] AI对话模块初始化失败: %d\n", ret);
      audio_deinit(&g_audio_ctx);
      sm_deinit(&g_sm_ctx);
      return ret;
    }

  /* 4. 初始化声音检测模块 */

  printf("[初始化] 正在初始化声音检测模块...\n");
  ret = start_sound_detection(&g_sm_ctx);
  if (ret < 0)
    {
      printf("[警告] 声音检测初始化失败: %d\n", ret);
      /* 声音检测失败不退出，继续运行 */
    }

  /* 5. 初始化主动关怀模块 */

  printf("[初始化] 正在初始化主动关怀模块...\n");
  ret = start_care(&g_sm_ctx);
  if (ret < 0)
    {
      printf("[警告] 主动关怀初始化失败: %d\n", ret);
      /* 关怀失败不退出，继续运行 */
    }

  /* TODO: 初始化其他模块 */
  /* printf("[初始化] 正在初始化网络模块...\n"); */
  /* network_init(); */

  /* 打印初始状态 */

  print_system_status(&g_sm_ctx);

  /* 5. 启动语音监听 */

  printf("[启动] 正在启动语音监听...\n");
  ret = start_audio_listening(&g_sm_ctx);
  if (ret < 0)
    {
      printf("[警告] 启动语音监听失败: %d\n", ret);
    }

  /* 2. 启动主循环任务 */

  printf("[启动] 正在启动主循环任务...\n");
  ret = pthread_create(&loop_thread, NULL, main_loop_task, &g_sm_ctx);
  if (ret != 0)
    {
      printf("[错误] 主循环任务创建失败: %d\n", ret);
      sm_deinit(&g_sm_ctx);
      return ret;
    }

  printf("[运行] 系统已启动, 按 Ctrl+C 退出\n\n");

  if (startup_text != NULL)
    {
      ret = llm_send_text(&g_llm_ctx, startup_text, NULL,
                          llm_complete_callback, &g_sm_ctx);
      if (ret < 0)
        {
          printf("[LLM] 启动请求失败: %d\n", ret);
        }
    }

  if (sound_self_test && g_sound_started)
    {
      int16_t test_audio[SOUND_DETECT_FRAMES_PER_WINDOW];
      memset(test_audio, 0, sizeof(test_audio));
      for (size_t i = 0; i < SOUND_DETECT_FRAMES_PER_WINDOW; i += 80)
        {
          test_audio[i] = (i / 80) % 2 == 0 ? INT16_MAX : INT16_MIN;
        }

      ret = sound_detect_feed(&g_sound_ctx, test_audio,
                              SOUND_DETECT_FRAMES_PER_WINDOW);
      printf("[自检] 声音检测测试数据已注入: %s (%d)\n",
             ret == OK ? "成功" : "失败", ret);
    }

  /* 3. 主线程等待退出 */

  while (g_running)
    {
      sleep(1);
    }

  /* 4. 清理退出 */

  printf("\n[退出] 正在停止系统...\n");

  /* 停止主动关怀 */

  stop_care();

  /* 停止声音检测 */

  stop_sound_detection();

  /* 停止音频监听 */

  stop_audio_listening();

  /* 等待主循环任务退出 */

  pthread_join(loop_thread, NULL);

  /* 反初始化LLM模块 */

  llm_deinit(&g_llm_ctx);

  /* 反初始化音频模块 */

  audio_deinit(&g_audio_ctx);

  /* 反初始化状态机 */

  sm_deinit(&g_sm_ctx);

  /* TODO: 反初始化其他模块 */
  /* network_deinit(); */

  printf("[退出] 系统已安全退出\n\n");

  return 0;
}
