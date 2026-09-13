/****************************************************************************
 * app/hello_app/mimo_voice.h
 *
 * 小米 MiMo 云端语音后端（ASR + TTS）的对外接口。
 *
 * 实现见 mimo_voice.c：走普通 HTTPS POST（不是火山那套 WebSocket），
 * 凭据从 ai_agent 的配置库读（llm_host / api_key），本模块不含任何密钥。
 *
 ****************************************************************************/

#ifndef __APP_HELLO_APP_MIMO_VOICE_H
#define __APP_HELLO_APP_MIMO_VOICE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stddef.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: mimo_asr_register / mimo_tts_register
 *
 * Description:
 *   把 MiMo 的 ASR / TTS 后端挂进 voice_asr.c / voice_tts.c 的分发层，
 *   后端名字都是 "mimo"（用 voice_asr_set_backend("mimo") 选中）。
 *
 *   注意：register 只是登记；真正的凭据检查在 init / 每次调用时做，
 *   没配 llm_host + api_key 时注册会成功但调用返回 -ENOENT。
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int mimo_asr_register(void);
int mimo_tts_register(void);

/****************************************************************************
 * Name: mimo_voice_available
 *
 * Description:
 *   配置里同时有非空的 api_key 和 llm_host 就返回 1，否则返回 0。
 *   ai_companion 用它决定语音后端选 "mimo" 还是 "volcengine"。
 *
 ****************************************************************************/

int mimo_voice_available(void);

/****************************************************************************
 * Name: mimo_wav_load_16k
 *
 * Description:
 *   读一个 WAV 文件，转成整条语音链路用的 16 kHz / 单声道 / s16le。
 *
 *   支持：16bit PCM（fmt=1）；多声道只取第 0 声道；采样率不是 16k 时
 *   用线性插值重采样到 16k（MiMo TTS 回的 24k 就是这么处理的）。
 *   不支持 8bit / 浮点 / ADPCM（返回 -ENOSYS）。
 *
 *   给 `hw_test asr <文件>` 用：板子上的录音链路是 16k 单声道，
 *   随手放的测试 WAV 可能是 24k/44.1k，这里统一转换。
 *
 * Input Parameters:
 *   path    - WAV 文件路径（任意路径，不要求 ROMFS）；
 *   pcm_out - 输出：堆上分配的 PCM 缓冲（调用方 free）；
 *   pcm_len - 输出：PCM 字节数。
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int mimo_wav_load_16k(const char *path, unsigned char **pcm_out,
                      size_t *pcm_len);

#endif /* __APP_HELLO_APP_MIMO_VOICE_H */
