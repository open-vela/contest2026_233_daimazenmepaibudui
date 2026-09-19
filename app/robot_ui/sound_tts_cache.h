/*
 * sound_tts_cache.h - TTS 语音预缓存
 *
 * 启动时下载常用语音，检测到跌倒时立即播放
 */

#ifndef SOUND_TTS_CACHE_H
#define SOUND_TTS_CACHE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 预缓存的语音 ID */
#define TTS_ID_FALL_ASK       0   /* "您还好吗？请回答" */
#define TTS_ID_FALL_OK        1   /* "好的，注意安全" */
#define TTS_ID_FALL_ALARM     2   /* "检测到跌倒，正在呼叫紧急联系人" */
#define TTS_ID_NEED_HELP      3   /* "需要帮助吗？" */
#define TTS_ID_COUNT          4

/* 缓存的音频数据 */
typedef struct {
    int16_t *data;
    int len;
    bool ready;
    bool requesting;    /* 正在请求中 */
} tts_cache_item_t;

/* 初始化（启动时调用一次） */
void tts_cache_init(void);

/* 预加载所有常用语音（后台线程调用） */
void tts_cache_preload_all(void);

/* 请求单条语音（异步，立即返回） */
void tts_cache_request(int id, const char *text);

/* 播放缓存的语音（立即播放，0延迟） */
bool tts_cache_play(int id);

/* 检查是否就绪 */
bool tts_cache_is_ready(int id);

/* 清理 */
void tts_cache_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
