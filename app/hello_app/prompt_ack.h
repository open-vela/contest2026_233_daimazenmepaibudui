/****************************************************************************
 * app/hello_app/prompt_ack.h
 *
 * 「说完之后立刻响一声」—— 让用户听见"它听见我了"。
 *
 * 为什么要有它（2026-10-25，优先级②「回复快」里收益最大的一项）：
 *   真实答复的最短物理延迟砍不进 1 秒（ASR 整段上传 ~1s → 大模型阻塞 POST
 *   1.5~5s → 才开始 TTS 合成）。但用户从"我说完了"到"它有点动静"之间那
 *   两三秒的静默，被感知成"没反应 / 是不是坏了"，对老人尤其明显。
 *   这一层**不缩短**真实等待，是把"没反应"换成"有回应感" —— 说清楚这个
 *   取舍，别当成提速。
 *
 * 出声两种，优先语音：
 *   ① 预缓存命中（tts_cache 查 PROMPT_ACK_TEXT）→ 直接说人话，不出网、
 *      不阻塞、几十 KB 的 ROMFS 读；
 *   ② 没命中 → 回落一段**程序合成的短提示音**（两声降调，约 0.43 秒）。
 *      提示音是现成的，不依赖 PC 侧合成素材，所以现在就能用。
 *
 * ★ 想升级成说人话只要一步：在 PC 上跑 _flash/gen_tts_cache.py，把
 *   PROMPT_ACK_TEXT 那句话合成进 ROMFS（口径 16 kHz / 单声道 / s16le 裸
 *   PCM，文件名规则见 tts_cache.h 头上），重新编 ROMFS。本模块查表命中
 *   就自动换成语音，代码一个字都不用改。没跑之前一直用提示音。
 *
 * ⚠ 半双工 + 起播互斥（这条是"别又不能对话"的保险，别删）：
 *   audio_play_start() / audio_play_start_ref() 对"已经在播"一律返回
 *   -EBUSY，**不会顶掉在播的那一段**（ai_audio.c:2128/2195）。所以真答复
 *   首块要出声时，必须先 prompt_ack_stop() 把提示音收掉，否则那一句
 *   "起播失败"会把整轮回复就地收尾 —— 用户听到的是提示音响完就没下文了。
 *   调用点见 ai_companion_main.c / robot_ui/main.c 里
 *   "提示音可能还在响"那两处注释。
 *
 * 本文件自给自足（同 tts_cache.h / ai_companion_req.h 的约束）：
 * hello_app 和 robot_ui 都用它，所以只引 ai_audio.h 和 C 标准库，不引
 * LVGL、不引任何一个 app 的内部类型。
 ****************************************************************************/

#ifndef __APP_HELLO_APP_PROMPT_ACK_H
#define __APP_HELLO_APP_PROMPT_ACK_H

#include <stdbool.h>

#include "ai_audio.h"

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* 两种语气，两句话、两段提示音。
 *
 * ACK  「嗯，我在」—— 说完之后那一声，答"我听见了，正在想"。
 * MISS 「我刚才没听清楚，您再说一遍好吗？」—— ASR 空 / 纯标点 / 识别失败
 *       时说的一声。2026-10-25 之前这条路只把文字推上屏、喇叭一声不吭；
 *       加了 ACK 提示音之后，用户听到的会变成"叮一声然后死寂"——比原来
 *       更像坏了，所以必须补上这一声。
 *
 * 两句话都写死在这里当**查表 key**：PC 上跑一次 gen_tts_cache.py 把它们
 * 合成进 ROMFS，就自动从提示音升级成说人话，代码一个字不用改。 */

typedef enum
{
  PROMPT_ACK_KIND_ACK  = 0,
  PROMPT_ACK_KIND_MISS = 1,
} prompt_ack_kind_t;

/* 可以直接拿去上屏 / 丢给 gen_tts_cache.py 的那两句话 */
#define PROMPT_ACK_TEXT        "嗯，我在"
#define PROMPT_MISS_TEXT       "我刚才没听清楚，您再说一遍好吗？"

/****************************************************************************
 * Name: prompt_ack_play_kind
 *
 * Description:
 *   起播一声"我听见了"或"没听清"。非阻塞：起播成功就立刻返回 0，真正
 *   出声在播放线程上（audio_play_start 只做 memcpy + 起线程）。
 *
 *   只能在**非录音线程**上调（ai_audio 的半双工切割 audio_prepare_output
 *   会停录音，从录音线程里调它是自己停自己）。hello_app 走 dialogue_worker
 *   / main_loop_task，robot_ui 走 voice_worker。录音线程上想说就置
 *   g_say_miss_pending，让 main_loop_task 去说。
 *
 *   已经在响时是幂等的（返回 0，不叠第二声）。
 *
 * Input Parameters:
 *   ctx  - 音频上下文（hello_app / robot_ui 各自己的那个）
 *   kind - PROMPT_ACK_KIND_ACK 或 PROMPT_ACK_KIND_MISS
 *
 * Returned Value:
 *   0 = 已起播（或本来就在响）；负 errno，其中 -EBUSY 说明设备正被别的
 *   播放占着 —— 这是**正常**情况（真答复已经先出声了），调用方不用处理，
 *   只是没听到这一声而已。
 *
 ****************************************************************************/

int prompt_ack_play_kind(audio_context_t *ctx, prompt_ack_kind_t kind);

/****************************************************************************
 * Name: prompt_ack_play
 *
 * Description:
 *   prompt_ack_play_kind(ctx, PROMPT_ACK_KIND_ACK) 的简写。
 *
 ****************************************************************************/

int prompt_ack_play(audio_context_t *ctx);

/****************************************************************************
 * Name: prompt_ack_stop
 *
 * Description:
 *   强收提示音（真答复首块要出声时调）。没在响时是空操作。
 *
 *   内部走 audio_play_stop()，那条路已经验过：置 play_stop → 有界 join
 *   播放线程 → playing=false。被打断的播放**不会**触发完成回调。
 *
 * Input Parameters:
 *   ctx - 音频上下文
 *
 ****************************************************************************/

void prompt_ack_stop(audio_context_t *ctx);

/****************************************************************************
 * Name: prompt_ack_busy
 *
 * Description:
 *   提示音此刻是不是在响（真答复起播前可以先问一句，决定要不要 stop）。
 *
 * Returned Value:
 *   true = 在响。
 *
 ****************************************************************************/

bool prompt_ack_busy(void);

#endif /* __APP_HELLO_APP_PROMPT_ACK_H */
