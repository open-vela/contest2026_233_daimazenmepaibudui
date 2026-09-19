/*
 * sound_tts_cache.c - TTS 语音预缓存实现
 */

#include "sound_tts_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 缓存的语音 */
static tts_cache_item_t s_cache[TTS_ID_COUNT];

/* 语音文本（启动时用于请求） */
static const char *s_tts_texts[TTS_ID_COUNT] = {
    "您还好吗？请回答",
    "好的，注意安全",
    "检测到跌倒，正在呼叫紧急联系人",
    "需要帮助吗？"
};

/* ═══════════════════════════════════════════════════════════════
 * 外部接口（需要平台实现）
 * ═══════════════════════════════════════════════════════════════ */

/* 平台需要实现这个函数：异步请求 TTS */
extern void platform_tts_request_async(const char *text,
                                       void (*on_done)(int id, int16_t *data, int len),
                                       int id);

/* 平台需要实现这个函数：播放音频 */
extern void platform_play_audio(const int16_t *data, int len);

/* ═══════════════════════════════════════════════════════════════
 * 内部回调
 * ═══════════════════════════════════════════════════════════════ */

static void on_tts_done(int id, int16_t *data, int len) {
    if (id < 0 || id >= TTS_ID_COUNT) return;

    s_cache[id].data = data;
    s_cache[id].len = len;
    s_cache[id].ready = (data != NULL && len > 0);
    s_cache[id].requesting = false;

    printf("[TTS] 缓存完成: id=%d, len=%d\n", id, len);
}

/* ═══════════════════════════════════════════════════════════════
 * 公共 API
 * ═══════════════════════════════════════════════════════════════ */

void tts_cache_init(void) {
    memset(s_cache, 0, sizeof(s_cache));
    printf("[TTS] 缓存初始化\n");
}

void tts_cache_preload_all(void) {
    printf("[TTS] 预加载所有语音...\n");
    for (int i = 0; i < TTS_ID_COUNT; i++) {
        tts_cache_request(i, s_tts_texts[i]);
    }
}

void tts_cache_request(int id, const char *text) {
    if (id < 0 || id >= TTS_ID_COUNT) return;
    if (s_cache[id].ready || s_cache[id].requesting) return;

    s_cache[id].requesting = true;
    platform_tts_request_async(text, on_tts_done, id);
    printf("[TTS] 发起请求: id=%d, text=%s\n", id, text);
}

bool tts_cache_play(int id) {
    if (id < 0 || id >= TTS_ID_COUNT) return false;
    if (!s_cache[id].ready) return false;

    platform_play_audio(s_cache[id].data, s_cache[id].len);
    printf("[TTS] 播放: id=%d\n", id);
    return true;
}

bool tts_cache_is_ready(int id) {
    if (id < 0 || id >= TTS_ID_COUNT) return false;
    return s_cache[id].ready;
}

void tts_cache_deinit(void) {
    for (int i = 0; i < TTS_ID_COUNT; i++) {
        if (s_cache[i].data) {
            free(s_cache[i].data);
            s_cache[i].data = NULL;
        }
    }
    printf("[TTS] 缓存已清理\n");
}
