/****************************************************************************
 * board/contest_board/src/sf32lb52_audio_in.h
 *
 * SF32LB52 录音通路封装（麦克风输入）
 *
 * 把「打开 / 配置 / START / read / STOP / close」这一套标准 NuttX audio
 * 接口收进三个函数，给不熟悉驱动的上层（成员二的 ai_audio.c 等）直接用。
 *
 * 底层设备是 /dev/audio/audio0（注意带 audio/ 子目录），接口本身是
 * ioctl(AUDIOIOC_CONFIGURE, AUDIO_TYPE_INPUT) + ioctl(AUDIOIOC_START)
 * + read() + ioctl(AUDIOIOC_STOP)，已在真机 `hw_test audio 2` 验证
 * （read 返回 64000/64000 字节）。完整说明见
 * docs/audio_driver_usage.md 第 9 节。
 *
 * 与设备驱动本身的坑（低频次说明，细节见文档第 3.2、8 节）：
 *   - read() 会一直阻塞到读满，失败/被打断时返回 0（不是"这次没数据"）；
 *   - 下层一次 read 内部有 5 秒超时，所以单次 read 不要超过 1 秒的数据量；
 *   - AUDIOIOC_STOP 会同时停掉播放和录音两条通路，不能全双工；
 *   - close() 在"最后一个 fd"时会走驱动的 shutdown 路径（关中断上下文），
 *     该路径已修（sf32lb52_audio.c 的最小化 hw_shutdown），但本模块
 *     绝不在里面做额外的事，只 ioctl(STOP) + close()。
 *
 ****************************************************************************/

#ifndef __BOARDS_CONTEST_BOARD_SRC_SF32LB52_AUDIO_IN_H
#define __BOARDS_CONTEST_BOARD_SRC_SF32LB52_AUDIO_IN_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/compiler.h>

#include <sys/types.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 设备路径：驱动里是 audio_register("audio0")，上层会自动拼 /dev/audio/ 前缀 */

#define AUDIO_IN_DEV          "/dev/audio/audio0"

/* 本板录音固定用 16 kHz / 单声道 / 16bit（s16le）。
 * 16k 单声道 16bit = 每秒 32000 字节。 */

#define AUDIO_IN_DEFAULT_RATE 16000
#define AUDIO_IN_DEFAULT_BITS 16

/* 单次 read 的建议上限：1 秒的数据量。
 * 驱动下层一次 read 内部只等 5 秒（sf32lb52_audio.c:1130），
 * 拆成 1 秒一块是最稳的（也是 audio_test 真机验证过的大小）。 */

#define AUDIO_IN_CHUNK_BYTES  (AUDIO_IN_DEFAULT_RATE * 2)

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: audio_in_start
 *
 * Description:
 *   打开 + 配置 + 启动录音通路。成功返回 0。
 *
 *   **重复调用**：已经处于录音状态时返回 -EBUSY（不会先停再开，
 *   免得把别人正在录的这次会话打断）。要重录请先 audio_in_stop()。
 *
 * Input Parameters:
 *   sample_rate - 采样率，只支持驱动支持的档位：
 *                 8000 / 16000 / 44100 / 48000（本板用 16000）；
 *   channels    - 声道数，目前只支持 1；
 *   bits        - 位深，目前只支持 16（s16le）。
 *
 * Returned Value:
 *   成功返回 0；参数非法返回 -EINVAL；open/ioctl 失败返回负 errno；
 *   已在录音中返回 -EBUSY。
 *
 ****************************************************************************/

int audio_in_start(int sample_rate, int channels, int bits);

/****************************************************************************
 * Name: audio_in_read
 *
 * Description:
 *   读一段 PCM 进 buf（最多 len 字节），阻塞到读满或被打断。
 *
 *   **单次调用不要超过 AUDIO_IN_CHUNK_BYTES（1 秒 = 32000 字节）**：
 *   要录更长得自己循环调这个函数，并且**返回 <= 0 必须跳出循环**
 *   （0 = 被 AUDIOIOC_STOP 打断或下层 5 秒超时，不是"再读一次就有数据"）。
 *
 *   注意：本函数**不持锁**读（否则另一个任务就没法用 audio_in_stop()
 *   发 STOP 来把阻塞的 read 唤醒），只会在开头短暂取锁拿 fd 快照。
 *
 * Returned Value:
 *   实际读到的字节数；<0 为负 errno（未 start 时返回 -EINVAL）。
 *
 ****************************************************************************/

ssize_t audio_in_read(FAR void *buf, size_t len);

/****************************************************************************
 * Name: audio_in_stop
 *
 * Description:
 *   停止并关闭录音通路（ioctl(AUDIOIOC_STOP) + close(fd)）。
 *   没在录音时是安全的空操作，返回 OK。
 *
 *   可以从别的任务调用来"救"一个正阻塞在 audio_in_read() 里的任务
 *   （驱动已修：STOP 能唤醒阻塞中的 read，read 会返回 0）。
 *
 *   提醒：如果本模块是设备上**唯一**的使用者，close() 会走驱动的
 *   shutdown 路径；该路径曾在关中断上下文里卡死整机，现已修成最小化
 *   的 hw_shutdown()（见 docs/audio_driver_usage.md 第 8 节）。
 *
 *   **不要在中断上下文里调用**（close 路径可能取锁）。
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int audio_in_stop(void);

#endif /* __BOARDS_CONTEST_BOARD_SRC_SF32LB52_AUDIO_IN_H */
