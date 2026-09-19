/****************************************************************************
 * apps/hello_app/sound_event.h
 *
 * 本地声音事件识别（摔倒 / 敲击 / 尖叫 / 其他），整条链路零外部依赖：
 *   mel 前端（自写 512 点 FFT）+ DSCNN-lite 前向 + 3/4 窗口投票后处理。
 * 权重来自 sound_event_model.h（由训练侧脚本导出），本模块不碰网络、不碰 LVGL。
 *
 * 输入契约（必须和训练侧一致，见 sound_event.c 顶部注释）：
 *   16 kHz 单声道 s16le；帧长 400(25ms)、跳 160(10ms)、Hann 窗；
 *   512 点 FFT → 257 点功率谱 → 40 个三角 mel（f_min=0 f_max=8000）；
 *   log(max(x,1e-10))；一个窗口 = 连续 64 帧(0.64s)，滑窗步长 20 帧(200ms)；
 *   整窗 (x-mean)/(std+1e-5) 归一化；mel[m*64+t]。
 *
 * 典型用法（在麦克风线程里）：
 *   sound_event_init();
 *   sound_event_set_callback(on_event, NULL);
 *   ... 每拿到一帧麦克风数据就 sound_event_feed(pcm, nsamples);
 * 回调在**调用 sound_event_feed() 的那个线程**里同步执行，所以回调里别做
 * 阻塞操作，要弹 UI 就丢给别的任务（例如 ui_post_ask_alarm()）。
 ****************************************************************************/

#ifndef __APPS_HELLO_APP_SOUND_EVENT_H
#define __APPS_HELLO_APP_SOUND_EVENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 类别序号固定：0=other 1=fall 2=knock 3=scream */
#define SOUND_EVENT_OTHER  0
#define SOUND_EVENT_FALL   1
#define SOUND_EVENT_KNOCK  2
#define SOUND_EVENT_SCREAM 3

/* 初始化：生成 Hann 窗 / FFT 旋转因子 / mel 滤波器组，清空流式状态。
 * 必须在 feed 之前调用一次。返回 0 成功，-1 失败。可重复调用（会重置状态）。 */
int sound_event_init(void);

/* 喂 PCM：16 kHz 单声道 int16，nsamples 是**样本数**（不是字节数）。
 * 内部维护环形缓冲，每攒够 20 帧（200ms）新数据就出一次窗口推理 + 后处理。
 * 返回本次实际完成的窗口推理次数（>=0），-1 表示未初始化 / 参数非法。
 * 允许任意长度、任意切分地连续调用。 */
int sound_event_feed(const int16_t *pcm, size_t nsamples);

/* 命中（投票 + 阈值 + 不应期都通过）时的回调。
 * cls 见 SOUND_EVENT_xxx，conf 是这几票的平均置信度（softmax 概率）。 */
typedef void (*sound_event_cb_t)(int cls, float conf, void *arg);

void sound_event_set_callback(sound_event_cb_t cb, void *arg);

/* 类别名，越界返回 "unknown"。 */
const char *sound_event_class_name(int cls);

/* 最近一个窗口 top-1 的置信度（softmax 概率）；一次都没推理过时返回 0。 */
float sound_event_last_confidence(void);

/* 清空流式状态：PCM 残余、mel 窗口、投票历史、每类不应期时间戳。
 * 滤波器组等只读表保留（init 过就可以直接再 feed）。 */
void sound_event_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* __APPS_HELLO_APP_SOUND_EVENT_H */
