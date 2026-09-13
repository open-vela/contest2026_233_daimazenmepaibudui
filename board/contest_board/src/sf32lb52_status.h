/****************************************************************************
 * board/contest_board/src/sf32lb52_status.h
 *
 * SF32LB52 统一外设状态查询：一次拿全网络 / 存储 / 音频 / 显示 / 触摸 /
 * 按键 / 时间，供 UI 状态栏、自检、上报心跳等复用。
 *
 * 背景：以前各个模块各查各的（UI 自己 socket + ioctl 查网卡、别处又
 * open("/dev/lcd0") 试一下），同一件事有好几份实现，状态还可能不一致。
 * 这里给一份统一的、只读的快照接口。
 *
 * ---------------------------------------------------------------------------
 * 设计约定（很重要）
 * ---------------------------------------------------------------------------
 * 1. **只做轻量探测，不初始化任何设备**：open() 一下立刻 close()、
 *    stat() 一下、读一次 RTC。这些设备（LCD / 触摸 / 音频）平时
 *    可能已经被别的模块开着（LVGL 开着 /dev/lcd0、录音开着音频节点），
 *    本模块绝对不做 configure / START / 挂载之类的动作，只是"敲一下门"。
 *    （/data 的"能不能写"是唯一例外：为了不轻信 access()，会写一个
 *    0 字节探针文件再立刻删掉，/data 是 tmpfs，不写 flash。）
 *
 * 2. **不需要先 init**，board_status_get() 随调随用，也不会阻塞：
 *    探测失败就是字段为 false，不是错误（返回值只在参数非法时为负）。
 *
 * 3. **mqtt_connected 只能由使用方喂**：板级模块不 include 应用头文件，
 *    所以 MQTT 的连接状态由网络模块调 board_status_set_mqtt() 写进来。
 *    没喂过时这个字段是 false。
 *
 * 4. 别在**中断**里调（里面有 open/ioctl/socket）。任务里调没问题。
 *    也别高频狂调：每次 get 会有几次 open/close 和一次 socket，
 *    UI 状态栏 1~2 秒拉一次足够；真要 200ms 刷，就只取便宜的几个字段
 *    （或自己缓存），别每次都全量探测。
 *
 * 用法见 docs/board_status_usage.md。
 *
 ****************************************************************************/

#ifndef __BOARDS_CONTEST_BOARD_SRC_SF32LB52_STATUS_H
#define __BOARDS_CONTEST_BOARD_SRC_SF32LB52_STATUS_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/compiler.h>

#include <stdint.h>
#include <stdbool.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BOARD_STATUS_ROM_ASSETS   "/etc/assets"      /* ROMFS 里的素材目录 */
#define BOARD_STATUS_DATA_DIR     "/data"            /* tmpfs 运行期目录 */
#define BOARD_STATUS_AUDIO_DEV    "/dev/audio/audio0"
#define BOARD_STATUS_LCD_DEV      "/dev/lcd0"
#define BOARD_STATUS_TOUCH_DEV    "/dev/input0"
#define BOARD_STATUS_RTC_DEV      "/dev/rtc0"

#define BOARD_STATUS_IP_MAX       16                 /* net_ip[] 的长度 */

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct board_status_s
{
  /* 网络（网卡本身是 netdev，不是 /dev 节点） */

  bool     net_link_up;        /* 有没有非回环的 IPv4 地址 */
  char     net_ip[BOARD_STATUS_IP_MAX];   /* 例如 "192.168.137.2"；没有时空串 */
  bool     mqtt_connected;     /* 由使用方通过 board_status_set_mqtt() 喂进来 */

  /* 存储 */

  bool     rom_assets_ready;   /* /etc/assets 能不能 stat 到（ROMFS 挂上没） */
  bool     data_ready;         /* /data 能不能写（tmpfs 挂上没） */

  /* 音频 */

  bool     audio_play_ready;   /* /dev/audio/audio0 能不能按播放方向打开 */
  bool     audio_rec_ready;    /* /dev/audio/audio0 能不能按录音方向打开 */

  /* 显示 / 触摸 / 按键 */

  bool     lcd_ready;          /* /dev/lcd0 能打开 */
  bool     touch_ready;        /* /dev/input0 能打开 */
  bool     btn_key_pressed;    /* PA11(KEY) 此刻是不是按下的 */

  /* 时间 */

  bool     rtc_valid;          /* RTC 年份 >= 2000（板子没有备份电池） */
  uint32_t uptime_s;           /* 开机到现在的秒数 */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: board_status_get
 *
 * Description:
 *   探一遍所有外设，把结果填进 `out`（调用前 out 里的内容会被清掉）。
 *
 *   每个字段都是"当场探一次"的值，不是缓存。**不需要先 init**，
 *   也不会初始化任何设备（见文件头的约定）。
 *
 *   由于板子上播放和录音用的是同一个节点（/dev/audio/audio0），
 *   audio_play_ready / audio_rec_ready 其实是"以播放方向 / 录音方向
 *   能不能打开这个节点"，方向本身驱动在 open 时不校验，所以两者
 *   通常同时为真；见 docs/board_status_usage.md 的说明。
 *
 * Returned Value:
 *   OK 表示快照已填好（字段全 false 也算 OK）；
 *   out == NULL 返回 -EINVAL。
 *
 ****************************************************************************/

int board_status_get(FAR struct board_status_s *out);

/****************************************************************************
 * Name: board_status_dump
 *
 * Description:
 *   往**调用者的 stdout** 打一份人看得懂的状态清单（`hw_test status`
 *   用的就是它）。内部就是 board_status_get() 再逐行 printf，
 *   所以调用约定跟 board_status_get() 完全一样。
 *
 * Returned Value:
 *   OK；参数/探测层面的失败只体现在内容里，只有内部 get 失败才返回负值。
 *
 ****************************************************************************/

int board_status_dump(void);

/****************************************************************************
 * Name: board_status_set_mqtt
 *
 * Description:
 *   由使用方（网络/MQTT 模块）喂 MQTT 连接状态，供 board_status_get()
 *   的 `mqtt_connected` 字段回读。板级模块不认识应用层的 MQTT 客户端，
 *   所以只能用这种"反向喂"的方式。
 *
 *   建议在 MQTT 连上/断开的地方各调一次（例如 network_comm 里 socket
 *   状态变化处），喂进来的值一直保留到下次被改写。
 *
 ****************************************************************************/

void board_status_set_mqtt(bool connected);

#endif /* __BOARDS_CONTEST_BOARD_SRC_SF32LB52_STATUS_H */
