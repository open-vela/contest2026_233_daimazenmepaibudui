/**
 * tts_cache.h - 固定文案的 TTS 预缓存（PC 侧预合成 → ROMFS → 板端直接播）
 *
 * 为什么要有它：
 *   关怀 / 追问 / 提醒这些话都是**写死的文案**，可现在的链路是"到点了才现调
 *   云端 TTS"——一次阻塞 HTTPS，网络慢或断的时候这句就哑了，而且那次调用是
 *   在**别人的线程**上跑的（主循环 / 关怀线程），把人一起拖住。
 *   这些话一辈子都不会变，所以在 PC 上合成一次、随固件打进 ROMFS，板子上
 *   只做"算文件名 → open → read → 播"，一个字节的网络都不走。
 *
 * 交付物两端：
 *   - PC 侧：D:/apply/claw/_flash/gen_tts_cache.py（不进仓库）按同一套规则算文件名，
 *     调 MiMo TTS 合成 **16 kHz / 单声道 / s16le 裸 PCM**，落到
 *     board/contest_board/src/etc/assets/tts/<文件名>.pcm；
 *   - 板端：本模块按同一套规则算文件名去 /etc/assets/tts/ 里找，
 *     找到就直接播，找不到才回落现有的 live TTS（回落路径必须保留 ——
 *     用户现场说的提醒标题、大模型的回复这些没法预生成）。
 *
 * ★★ 文件名规则（PC 与板端**必须逐字节相同**，改一边就必须改另一边）★★
 *
 *   第 1 步 归一化：把文案看成 UTF-8 字节流，按下面三条压成"归一化字节串"
 *     a) "空白"只有两种：单个 ASCII 空白字节（0x20 0x09 0x0A 0x0B 0x0C 0x0D）、
 *        以及全角空格 U+3000（字节 E3 80 80）；
 *     b) 首尾的空白全部丢掉；
 *     c) 正文里每一段**连续**空白折成一个 0x20；其余字节原样复制（不改大小写、
 *        不做 Unicode 规范化 —— 这一层只治"抄文案时多敲了一个空格"这种手误）。
 *     例：" 该吃饭了 \n" 和 "该吃饭了" 归一化后相同；
 *        "该吃   饭了" 和 "该吃 饭了" 相同；"该吃\u3000饭了" 和 "该吃 饭了" 相同。
 *
 *   第 2 步 哈希：对归一化后的字节串做 **FNV-1a 32 位**
 *     hash = 2166136261; 每个字节：hash ^= byte; hash *= 16777619 (mod 2^32)
 *
 *   第 3 步 文件名：sprintf("%08x.pcm", hash) —— 纯 ASCII、小写十六进制、
 *     固定 8 位（**前导零不能省**，否则两端会算出不同的名字）。
 *
 *   板端完整路径："%s/%08x.pcm"，TTS_CACHE_ROMFS_DIR = "/etc/assets/tts"
 *   （ROMFS 的挂载点在 /etc，素材在仓库的 board/contest_board/src/etc/assets/tts/）
 *
 * 一致性怎么保证：C 与 Python 各实现一遍，靠 _tts_cache_host_test/ 的 host 测试
 * 对拍（那个测试直接拿板端这份 C 代码去算名字，再断言盘上真有那个文件）。
 *
 * ⚠️ 本头文件必须**自给自足**（同 ai_companion_req.h 的约束）：hello_app 和
 *    robot_ui 都要 include 它，所以不引 LVGL、不引任何一个 app 的内部类型，
 *    只依赖 C 标准库。
 */

#ifndef __TTS_CACHE_H
#define __TTS_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 预缓存 PCM 放哪（ROMFS 挂载点是 /etc，素材在仓库的
 * board/contest_board/src/etc/assets/tts/）。
 * 允许外部覆盖：host 测试要在 PC 上跑同一条查表路径（见 _tts_cache_host_test），
 * 板子上永远用默认值。 */
#ifndef TTS_CACHE_ROMFS_DIR
#  define TTS_CACHE_ROMFS_DIR   "/etc/assets/tts"
#endif

/* 文件名缓冲要多大："<8 位哈希>.pcm" + '\0' */
#define TTS_CACHE_NAME_MAX    16

/**
 * @brief  文案 -> FNV-1a 32 位哈希（先按上面第 1 步归一化）
 * @param  text  UTF-8 文案（NULL 按空串算，返回空串的哈希）
 * @return 32 位哈希
 */
uint32_t tts_cache_hash(const char *text);

/**
 * @brief  按同一套规则算归一化字节串（主要用于对拍与自检）
 * @param  text    UTF-8 文案
 * @param  out     输出缓冲；out_len 是它的字节数；**不保证**收尾 '\0'
 * @param  out_len 输出缓冲字节数
 * @return >0 = 写进的字节数；0 = 归一化后是空串；<0 = 参数非法
 * @note   截断是"截在哪儿算哪儿"，调用方按 >0 的返回值处理；
 *         文案最长也就几十字节，给 256 字节的缓冲绰绰有余。
 */
int tts_cache_normalize(const char *text, char *out, size_t out_len);

/**
 * @brief  文案 -> 预缓存文件名（"<8 位哈希>.pcm"，纯 ASCII）
 * @param  text     UTF-8 文案
 * @param  name    输出缓冲（建议 TTS_CACHE_NAME_MAX 字节）
 * @param  name_len 输出缓冲字节数（< 9 会返回 -ENOSPC）
 * @return 0 成功；负 errno
 */
int tts_cache_name(const char *text, char *name, size_t name_len);

/**
 * @brief  文案 -> 预缓存的完整绝对路径（"/etc/assets/tts/<8 位哈希>.pcm"）
 * @param  text     UTF-8 文案
 * @param  path     输出缓冲（建议 64 字节以上）
 * @param  path_len 输出缓冲字节数
 * @return 0 成功；负 errno
 */
int tts_cache_path(const char *text, char *path, size_t path_len);

/**
 * @brief  查预缓存：命中就把整段 PCM 读进堆缓冲
 *
 * @param  text     UTF-8 文案（**原样**传，和 PC 脚本喂的是同一句话）
 * @param  pcm_out  输出：堆上那一份 PCM（16k / 单声道 / s16le 裸数据）；
 *                  未命中时置 NULL，**调用方负责 free**
 * @param  len_out  输出：PCM 字节数（偶数）；未命中置 0
 * @return 0 = 命中且读全了；-ENOENT = ROMFS 里没有（没预生成 / 忘了重编 ROMFS）；
 *         其它负 errno = 文件在但读不出来（长度非偶数 / 内存不够 / read 失败）
 *
 * 只读文件、不碰音频设备、不加锁，任何线程都能调；最坏耗时就是几十 KB 的
 * XIP 读（亚毫秒级），所以它不会把调用方挂住 —— 这正是相对 live TTS 的意义。
 */
int tts_cache_load(const char *text, unsigned char **pcm_out, size_t *len_out);

#endif /* __TTS_CACHE_H */
