/**
 * main.c - 智爱陪伴应用入口
 * 在 openvela 中运行
 */

#include <nuttx/config.h>
/* up_interrupt_context()：提醒出声前要挡一下中断上下文（在中断里等 hello_app
 * 把录音线程收摊是不可能的）。写法和 robot_ui_bridge.c 那道同类检查一致。 */
#include <nuttx/arch.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sched.h>
#include <lvgl/lvgl.h>

/* UI 模块头文件 */
#include "robot_ui.h"
#include "touch_ui.h"
#include "network_comm.h"
#include "time_sync.h"
/* 提醒软调度（app/robot_ui/reminder_sched.c）：提醒列表 + 到点回调。
 * 到点回调在 RTC 模块的工作线程里跑，这里只负责"弹窗 / 出声 / 推送"。 */
#include "reminder_sched.h"
/* 跨线程投递口（app/robot_ui/ui_async.c）：本文件里所有"投到 LVGL 线程"的
 * 动作都收口到它，不再直接调 lv_async_call()。原因见 ui_async.h 开头 ——
 * lv_async_call() 内部往 LVGL 的全局定时器链表插节点，而那次插入在 LVGL 9.1
 * 里完全没有锁，多个工作线程同时插会把链表插坏，一旦 LVGL 线程遍历到半初始
 * 化的节点（timer_cb 还是垃圾）就会跳到非法地址，整机硬故障、串口静默。 */
#include "ui_async.h"
/* 刷屏/渲染耗时仪表（app/robot_ui/ui_perf.c）：只在慢的时候打日志，用来回答
 * "黑屏那一下到底黑在哪一段"。总开关是头文件里的 UI_PERF_LOG，置 0 即完全摘掉。 */
#include "ui_perf.h"
/* 屏幕镜像的 LVGL 侧（app/robot_ui/lcd_mirror_glue.c）：把每次 flush 的脏区
 * 拷进板级影子帧缓冲，由 board/.../lcd_mirror.c 的镜像任务发给 PC。
 * 详细说明见那两个文件的文件头；nsh 命令是 `hw_test lcdmirror ...`。 */
#include "lcd_mirror_glue.h"
#include <netutils/cJSON.h>

/* AI 模块头文件 (成员二) */
#include "ai_state_machine.h"
#include "ai_audio.h"
#include "ai_sound_detect.h"
#include "ai_care.h"
/* [缺文件临时隔离] 上游 3ec9ab9 用了 ai_checkin_begin/respond/tick/snapshot，
 * 但 ai_checkin.{c,h} 没有提交（全仓库找不到），这份提交本身编不过。
 * 等队友补上文件后，把本文件里所有 [缺文件临时隔离] 的 #if 0 删掉即可。 */
/* #include "ai_checkin.h" */
#include <string.h>
#include <errno.h>      /* reminder_play_reason() 要按错误码说人话（-EBUSY / -EIO …） */
#include <stdint.h>     /* uintptr_t：询问页那条超时看门狗线程要把 gen 当参数传 */
#include <pthread.h>

/* 语音后端（小米 MiMo，实现在 app/hello_app/mimo_voice.c）。
 * voice_asr/voice_tts 是 ai_agent 包的分发层，头文件路径由 CMakeLists.txt
 * 的 INCLUDE_DIRECTORIES 提供（与 app/hello_app/CMakeLists.txt 同一写法）。 */
#include "mimo_voice.h"
#include "voice/voice_asr.h"
#include "voice/voice_tts.h"

/* 常态听音 + 突发大音量检测（工作流 C）：默认关（配置键 enable_ambient_listen），
 * 开了才会起线程占麦克风。它对半双工的仲裁靠 hold/pause/resume —— 下面的
 * ambient_busy() / ambient_on_event() 和几处 pause/resume 就是它的接线。 */
#include "ambient_listen.h"

/* 请 hello_app 干一件事的跨 app 入口（都**非阻塞**：只登记请求 / 读一次状态
 * 快照，一个设备都不碰）：
 *   - ai_companion_ask_abort()：报警真的走起来了，让它那条语音追问收摊；
 *   - ai_companion_voice_submit()：镜像面板的「提交」，请它立刻收尾当前这一段。
 * 实现在 app/hello_app 里，符号在最终链接时解析（单一大镜像）；头文件路径由
 * CMakeLists.txt 的 INCLUDE_DIRECTORIES `../hello_app` 提供，无需改构建脚本。
 * ★ 2026-09-21：原来的「让路 / 收回」（请 hello_app 交出它的常开麦）**整套
 *   删掉了**（用户拍板：「关闭让路…不需要做麦克风转让，因为我们的麦是常开的」；
 *   真机故障形状和取舍见 ai_companion_req.h 头上）。现在 robot_ui 要出声就直接
 *   audio_play_start()，半双工由它内部处理（见 reminder_play_exclusive 头上那段）。 */
#include "ai_companion_req.h"

/* 固定文案的 TTS 预缓存（查询接口，实现在 app/hello_app/tts_cache.c）。
 * 到点提醒那句「该<标题>了」和跌倒询问「您摔到了吗？」都是**写死的固定文案**：
 * 默认那几条的 PCM 已经随固件打进 ROMFS 了（PC 侧脚本 _flash/gen_tts_cache.py 生成，
 * 文件名规则见 tts_cache.h 头上），命中就一个字节的网络都不走 —— 网络慢/断的时候
 * 这句照样念得出来。用户现场用语音加的标题没预生成，照旧回落 live TTS。
 * 同样是自给自足的头文件 + 跨 app 的符号在最终链接时解析（和上面那个同一套）。 */
#include "tts_cache.h"

/* 语音链路推界面的同进程直调入口（robot_ui_bridge.c/.h）：
 * ai_companion 不再只靠公网 MQTT 回传才知道要刷屏，见该头文件头的说明。
 * 本文件只提供三个薄门面（sanitize / ui_post / 弹面板抑制），
 * 因为那三个东西是这里的 static，别处够不到。 */
#include "robot_ui_bridge.h"

/* 「疑似摔倒」事件链的唯一入口（fall_alarm.h，实现在本文件后半那一节）。
 * 头文件是自给自足的：等队友的摔倒检测器（本地模型 / hello_app 那一侧）要接
 * 进来时，include 它、在任务上下文里调一次 fall_alarm_trigger("...") 就行。 */
#include "fall_alarm.h"

/* 板级报警模块（alarm_trigger / alarm_clear）：报警声和"持续响到解除"由它负责，
 * 这是安全功能里唯一不依赖界面线程、也不依赖网络的一段。
 * 头文件路径由 CMakeLists.txt 的 ${NUTTX_BOARD_ABS_DIR}/src 提供
 * （robot_ui.c 的报警按钮走的是同一个模块）。 */
#include "sf32lb52_alarm.h"

/* clock_gettime(CLOCK_MONOTONIC)：摔倒链的"等回答"超时用它计时。
 * 不用 lv_tick_get()：那个 tick 由 LVGL 线程推进，而这里计时的是工作线程，
 * 不该受界面刷新节奏影响。 */
#include <time.h>

/* LVGL 定时器 */
static void lvgl_timer_handler(void)
{
    lv_timer_handler();
}

/* ==================== AI 模块全局上下文 ==================== */
static sm_context_t g_sm_ctx;           // 状态机
static audio_context_t g_audio_ctx;     // 音频
static sound_detect_context_t g_sound_ctx;  // 声音检测
static care_context_t g_care_ctx;       // 主动关怀

/* AI 模块是否初始化成功 */
static bool g_ai_initialized = false;

/* ==================== 语音对话（ASR → LLM → TTS） ==================== */
/*
 * 界面在 touch_ui.c 的"语音聊天弹窗"里，这里负责把音频链路串起来：
 *   录音回调攒 PCM（堆缓冲，10 秒上限）
 *     -> 点「提交」停录音，把这一轮 PCM 的所有权交给一个工作线程
 *     -> voice_asr_recognize() 识别（mimo_voice.c 的 MiMo ASR 后端）
 *     -> mimo_chat() 直接发 HTTPS 对话请求拿回复（同步，就在工作线程里）
 *     -> voice_tts_speak() 合成，audio_play_start() 出声
 * 整轮只用一个工作线程；网络调用一律不在 LVGL 线程里做。回界面只走
 * touch_ui_set_voice_status() / touch_ui_set_voice_reply() /
 * touch_ui_voice_chat_round_done()，它们内部用 lv_async_call 投到 LVGL 线程，
 * 并按世代号丢掉过期结果（用户已经关窗或又开了一轮）。所以关窗不需要 join
 * 工作线程，界面也不会被网络卡住。
 *
 * ⚠️ 不要改回 ai_agent 的 llm_send_text() / velaclaw_*：那条路把消息投进
 * ai_agent 自己初始化过的消息总线队列，而别的 app 直接调时队列锁还是 .bss 的
 * 全 0，pthread_mutex_lock 会撞 NXSEM_IS_MUTEX 断言把整个 app 打死
 * （2026-09-13 真机崩溃现场：voice_worker -> llm_send_text -> velaclaw_ask
 * -> msg_queue_push -> pthread_mutex_take）。跨 app 只用 mimo_voice.h 里这套
 * HTTPS 接口（自带凭据，不依赖 ai_agent 进程状态）。
 */

/* 10 秒 @16k/单声道/s16le = 320000 字节。
 * 必须走堆：320 KB 静态数组会把内核 SRAM 顶满（本项目踩过的坑，理由同
 * ai_companion_main.c 里 TTS 缓冲那段注释）。 */
#define VOICE_PCM_MAX_BYTES   (16000 * 2 * 10)
/* 少于 0.5 秒基本是误触或者根本没说话，不值得发一次网络请求 */
#define VOICE_PCM_MIN_BYTES   (16000 * 2 / 2)
/* TTS 输出缓冲：**必须和 ai_audio 的 AUDIO_PLAY_BUFFER_MS 对齐**（现在两边都是
 * 30 秒 = 960000 字节）—— audio_play_start() 是把整段 PCM 一次拷进自己的播放
 * 缓冲，这里比那边大就会在播放层 -ENOSPC 一声不响，比那边小则浪费可念的字数。
 *
 * 30 秒 ≈ 130 字：云端 TTS 现在是分块合成的（mimo_voice.c），更长的回复会在
 * 合成那层按"装得下多少念多少"截断并打日志（不会失败、也不会静默）。 */
#define VOICE_TTS_BUF_BYTES   (16000 * 2 * 30)
#define VOICE_ASR_TEXT_MAX    512
#define VOICE_REPLY_TEXT_MAX  2048

/* 录音累积缓冲：堆上按需分配，跨线程访问一律加 g_voice_lock */
static pthread_mutex_t g_voice_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char  *g_voice_pcm = NULL;
static size_t          g_voice_pcm_len = 0;
static volatile bool   g_voice_pcm_full = false;

/* 正在播放的那一段属于哪一轮（播放完成回调里用来判断结果是否还该写进界面） */
static volatile uint32_t g_voice_playing_gen = 0;

/* 一轮对话的任务：录音数据 + 复用缓冲 + 引用计数。
 * 整轮（ASR -> mimo_chat -> TTS -> 播放）现在都在同一个工作线程里跑，
 * 引用计数只在工作线程退出时把整份任务 free 掉；用户中途关窗时线程照跑、
 * 靠世代号丢弃结果，不用谁等谁。 */
typedef struct {
    pthread_mutex_t lock;
    int             refs;
    uint32_t        gen;                          /* 创建时的会话世代号 */
    unsigned char  *pcm;                          /* 本轮录音数据（任务持有） */
    size_t          pcm_len;
    unsigned char  *tts;                          /* TTS 输出缓冲（懒分配） */
    size_t          tts_cap;
    char            reply[VOICE_REPLY_TEXT_MAX];  /* 对话区显示的文字 */
} voice_task_t;

static void voice_task_unref(voice_task_t *task)
{
    bool dead = false;

    pthread_mutex_lock(&task->lock);
    if (--task->refs == 0) {
        dead = true;
    }
    pthread_mutex_unlock(&task->lock);

    if (dead) {
        free(task->pcm);
        free(task->tts);
        pthread_mutex_destroy(&task->lock);
        free(task);
    }
}

static voice_task_t *voice_task_new(unsigned char *pcm, size_t pcm_len)
{
    voice_task_t *task = calloc(1, sizeof(voice_task_t));

    if (task == NULL) {
        return NULL;
    }

    pthread_mutex_init(&task->lock, NULL);
    task->refs = 1;                                /* 这一份归工作线程 */
    task->gen = touch_ui_voice_chat_generation();
    task->pcm = pcm;
    task->pcm_len = pcm_len;

    return task;
}

/* 这一轮的世代号还对得上吗？对不上 = 用户关窗或又开了一轮，结果整份丢掉 */
static bool voice_task_alive(const voice_task_t *task)
{
    return task->gen == touch_ui_voice_chat_generation();
}

/* 把状态行 + 对话区一起投到弹窗上（只从工作线程调） */
static void voice_task_show(const voice_task_t *task, const char *status)
{
    if (!voice_task_alive(task)) {
        return;
    }

    if (status != NULL) {
        touch_ui_set_voice_status(status);
    }
    touch_ui_set_voice_reply(task->reply);
}

/* 录音数据回调：在音频录音线程里跑（每帧 20ms 一次） */
static void voice_record_callback(const int16_t *data, size_t frames,
                                  void *user_data)
{
    size_t bytes = frames * sizeof(int16_t);

    (void)user_data;

    if (data == NULL || bytes == 0) {
        return;
    }

    pthread_mutex_lock(&g_voice_lock);

    if (g_voice_pcm == NULL) {
        g_voice_pcm = malloc(VOICE_PCM_MAX_BYTES);
        if (g_voice_pcm == NULL) {
            pthread_mutex_unlock(&g_voice_lock);
            printf("[VoiceChat] PCM 缓冲分配失败\n");
            return;
        }
    }

    if (!g_voice_pcm_full) {
        if (g_voice_pcm_len + bytes > VOICE_PCM_MAX_BYTES) {
            /* 到 10 秒上限：多出来的丢掉并打标记。停录音交给主循环
             * （LVGL 线程）去做 —— 录音线程在自己的回调里拆设备不合适。 */
            g_voice_pcm_full = true;
            printf("[VoiceChat] 录音到 10 秒上限，后面的丢掉了\n");
        } else {
            memcpy(g_voice_pcm + g_voice_pcm_len, data, bytes);
            g_voice_pcm_len += bytes;
        }
    }

    pthread_mutex_unlock(&g_voice_lock);
}

/* ==================== 显示用文本清洗（只影响显示，不影响 TTS） ==================== */
/*
 * 字库（lv_font_ui_16/20/24.c）只覆盖 GB2312 6763 字 + ASCII + CJK 标点 + 全角。
 * AI 回复是 Markdown + emoji 的混合体（例："你好呀！😊\n\n- 📱 天气"），emoji 和
 * 大部分符号没有对应字形，直接塞给 lv_label 就是一个一个方块 —— 所以**送显示
 * 之前**先过一遍 sanitize_for_display()：
 *
 *   1. 丢掉字库渲染不了的码点：emoji（U+1F000 以上）、杂项符号/装饰符
 *      （U+2600-U+27BF）、几何图形与制表符（U+2500-U+25FF）、带圈数字与技术
 *      符号（U+2300-U+24FF）、箭头只留字库里有的 ←↑→↓、变体选择符/零宽字符
 *      （U+FE00-U+FE4F、U+2000-U+206F 里除常用标点之外的全部）、以及
 *      Latin-1 里那堆重音字母（é、ñ… 字库里只有 °±×÷）；
 *   2. Markdown 降级成纯文本：`*` 和反引号直接去掉，`__` 这种连续下划线去掉
 *      （单个 `_` 留着，免得把 ai_audio 这类标识符拆了），行首的 `#`（标题）和
 *      `>`（引用）去掉，行首的 `- `/`+ `/`* ` 列表符号换成 `· `；
 *   3. 换行保留（对话区是 "我说：…\n\n智爱：…" 的多行文本），但连续 3 个以上
 *      的换行压成 2 个；制表符换成空格。
 *
 * ⚠️ 只洗干净**显示**用的那一份。TTS 拿到的仍是原始文本（emoji 云端自己会读），
 *    两者在 voice_speak_reply() 里分别是局部变量 shown 和参数 response。
 *
 * ⚠️ 已知局限：GB2312 之外的生僻汉字（GBK 独有的字）仍会显示成方块 —— 在设备上
 *    精确判断"这个字在不在字库里"要带一张表，这里只处理可枚举的几个符号区。
 */

/* 取一个 UTF-8 码点，返回吃掉的字节数（非法序列返回 1 并给 U+FFFD） */
static size_t disp_utf8_get(const char *s, size_t len, unsigned int *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned int v;
    size_t n;
    size_t i;

    if (p[0] < 0x80) {
        *cp = p[0];
        return 1;
    }

    if ((p[0] & 0xe0) == 0xc0) {
        v = p[0] & 0x1f;
        n = 1;
    } else if ((p[0] & 0xf0) == 0xe0) {
        v = p[0] & 0x0f;
        n = 2;
    } else if ((p[0] & 0xf8) == 0xf0) {
        v = p[0] & 0x07;
        n = 3;
    } else {
        *cp = 0xfffd;
        return 1;
    }

    if (n >= len) {
        *cp = 0xfffd;
        return 1;
    }

    for (i = 1; i <= n; i++) {
        if ((p[i] & 0xc0) != 0x80) {
            *cp = 0xfffd;
            return 1;
        }

        v = (v << 6) | (p[i] & 0x3f);
    }

    *cp = v;
    return n + 1;
}

/* 这个码点字库里有字形吗？（只判"肯定没有"的几个区，其余一律放行） */
static bool disp_renderable(unsigned int cp)
{
    if (cp < 0x20 || cp == 0x7f) {
        return false;                          /* 控制字符（\n \t 在外面处理） */
    }

    if (cp >= 0xa0 && cp <= 0xff) {
        /* Latin-1 补充里字库只有 °±·×÷（· 就是下面列表项用的那个分隔点） */
        return cp == 0xb0 || cp == 0xb1 || cp == 0xb7
               || cp == 0xd7 || cp == 0xf7;
    }

    if (cp >= 0x2000 && cp <= 0x206f) {
        /* 常用标点里只留 – — ‘ ’ “ ” … （零宽字符、bullet、省略字符等没有字形） */
        return cp == 0x2013 || cp == 0x2014
               || (cp >= 0x2018 && cp <= 0x201d) || cp == 0x2026;
    }

    if (cp >= 0x2190 && cp <= 0x21ff) {
        return cp >= 0x2190 && cp <= 0x2193;   /* 只留 ← ↑ → ↓ */
    }

    if (cp >= 0x2300 && cp <= 0x2bff) {
        return false;                          /* 技术符号/带圈数字/几何图形/装饰符 */
    }

    if (cp >= 0x2e80 && cp <= 0x2eff) {
        return false;                          /* CJK 部首补充 */
    }

    if (cp >= 0xfe00 && cp <= 0xfe4f) {
        return false;                          /* 变体选择符 / CJK 兼容形式 */
    }

    if (cp >= 0x1f000) {
        return false;                          /* emoji / 平面 1 的各种符号 */
    }

    return true;
}

/* 把 in 清洗成能显示的纯文本写进 out（out_cap 含结尾 '\0'）。
 * 输出只会比输入短或等长（`- ` 也是 2 字节换成 2 字节的 `·`），
 * 尺寸：每个列表项最多比输入多 1 字节（"- " 2 字节 -> "· " 3 字节），
 * 其余情况只会变短。out_cap 到顶就停，不会越界。
 *
 * 落盘只有一处（下面的 emit: 标签）：要写出去的字节用 (src, src_len) 描述 ——
 * 默认是 in[i] 那几个字节，替换过的字符（列表符号 "· " 3 字节、制表符换的
 * 空格 1 字节）指向别的常量。**别改成"先把 cp 改掉再拷 in[i]"**：拷出来的还是
 * 源串里的原字符（这个坑真踩过：制表符照样输出成制表符）。
 * 返回写入的字节数。 */
static size_t sanitize_for_display(const char *in, char *out, size_t out_cap)
{
    size_t i = 0;
    size_t o = 0;
    size_t len;
    int nl = 0;                                /* 攒下的换行数（最多 2） */
    bool line_start = true;
    const char *src;                           /* 本次要写出去的字节 */
    size_t src_len;
    size_t adv;

    if (in == NULL || out == NULL || out_cap == 0) {
        return 0;
    }

    len = strlen(in);

    while (i < len) {
        unsigned int cp;
        size_t used = disp_utf8_get(in + i, len - i, &cp);

        src = in + i;                          /* 默认原样写这几个字节 */
        src_len = used;
        adv = used;                            /* 写完 i 往前走多少 */

        /* 行首的 Markdown 结构标记 */
        if (line_start && used == 1) {
            if (cp == '#' || cp == '>') {
                /* 标题号 / 引用号：连它后面的空白一起去掉 */
                i += used;

                while (i < len && (in[i] == ' ' || in[i] == '\t')) {
                    i++;
                }

                continue;
            }

            if ((cp == '-' || cp == '+' || cp == '*')
                && (i + used >= len || in[i + used] == ' '
                    || in[i + used] == '\t')) {
                /* 列表项 "- xxx" -> "· xxx"：标记连同随后的空白一起吃掉，
                 * 再补一个 "· "（U+00B7 的 UTF-8 是 C2 B7）。
                 * 走下面统一的 emit，别在这里自己拼 —— 换行落盘、越界检查
                 * 都只有那一份。 */
                i += used;

                while (i < len && (in[i] == ' ' || in[i] == '\t')) {
                    i++;
                }

                src = "\xc2\xb7 ";
                src_len = 3;
                adv = 0;                       /* i 上面已经走过了 */
                goto emit;
            }
        }

        if (cp == '\r') {
            i += used;
            continue;
        }

        if (cp == '\t') {
            /* 制表符宽度不可控，换成空格；同样要走 src/src_len，
             * 只把 cp 改掉的话下面 memcpy 出来的还是制表符 */
            cp = ' ';
            src = " ";
            src_len = 1;
        }

        if (cp == '\n') {
            i += used;
            line_start = true;

            if (nl < 2) {
                nl++;
            }

            continue;
        }

        /* Markdown 强调标记：` 和 * 一律去掉 */
        if (used == 1 && (cp == '*' || cp == '`')) {
            i += used;
            line_start = false;
            continue;
        }

        /* 连续下划线（__强调__）去掉；单个 _ 留着当普通字符 */
        if (used == 1 && cp == '_' && i + 1 < len && in[i + 1] == '_') {
            while (i < len && in[i] == '_') {
                i++;
            }

            continue;
        }

        if (!disp_renderable(cp)) {
            i += used;
            continue;
        }

emit:
        /* 丢掉一个码点之后很容易留下双空格（"- 📱 天气" -> "·  天气"，
         * "3 * 4" -> "3  4"），同一行里的连续空格在这里压成一个。
         * 只在本行内压（nl 不为 0 时说明还没落盘换行，不能拿 out[o-1] 判断）。 */
        if (nl == 0 && src_len == 1 && src[0] == ' '
            && o > 0 && out[o - 1] == ' ') {
            i += adv;
            continue;
        }

        /* 有内容要写了才把攒下的换行落盘，免得结尾挂一串空行 */
        while (nl > 0 && o + 1 < out_cap) {
            out[o++] = '\n';
            nl--;
        }

        if (o + src_len + 1 > out_cap) {
            break;
        }

        memcpy(out + o, src, src_len);
        o += src_len;
        i += adv;
        line_start = false;
    }

    out[o] = '\0';
    return o;
}

/* ==================== 跨线程改界面：一律投递到 LVGL 线程 ==================== */
/*
 * LVGL **不是线程安全的**：别的线程改控件，只要正好撞上它在刷屏
 * （disp->rendering_in_progress），就会踩断言
 *   _lv_inv_area: Invalidate area is not allowed during rendering
 * 整个 app 直接死 —— 屏幕停在最后一帧，看着就是"卡死"
 * （2026-09-14 实测栈：robot_ui_set_status → lv_label_set_text → lv_obj_invalidate
 *   → _lv_inv_area → assert，TTS 播完那一瞬间崩，正好冻结在"处理中…"）。
 *
 * 谁是非 LVGL 线程：语音工作线程、音频播放线程、MQTT/网络线程、声音检测线程、
 * 主动关怀线程。它们要改界面必须走下面这些 ui_post_*（内部 lv_async_call）；
 * LVGL 线程自己（触摸回调、主循环、lv_async_call 回调里）直接调 robot_ui_* 没问题。
 *
 * 投递失败（内存紧）就把这次界面更新丢掉：少刷一次无所谓，跨线程碰 LVGL 会死。
 */

typedef struct {
    int   status;   /* 状态栏，-1 = 这次不改 */
    int   face;     /* 表情，-1 = 这次不改 */
    char *reply;    /* 对话区文字，NULL = 不改（回调里 free） */
} ui_msg_t;

static void ui_apply_msg(void *arg)
{
    ui_msg_t *m = (ui_msg_t *)arg;

    if (m->status >= 0) {
        robot_ui_set_status((robot_status_t)m->status);
    }
    if (m->face >= 0) {
        robot_ui_set_face((robot_face_t)m->face);
    }
    if (m->reply != NULL) {
        robot_ui_set_ai_reply(m->reply);
    }

    free(m->reply);
    free(m);
}

/* 小工具：只为投递用的字符串副本（没有就直接返回 NULL，让回调跳过这一项） */

static char *ui_strdup(const char *s)
{
    char *p;

    if (s == NULL) {
        return NULL;
    }

    p = malloc(strlen(s) + 1);
    if (p != NULL) {
        strcpy(p, s);
    }

    return p;
}

/* 非 LVGL 线程用：投递一次"状态 + 表情 + 对话区文字"更新 */
static void ui_post(int status, int face, const char *reply)
{
    ui_msg_t *m = malloc(sizeof(ui_msg_t));

    if (m == NULL) {
        return;
    }

    m->status = status;
    m->face   = face;
    m->reply  = ui_strdup(reply);

    if (ui_async_call(ui_apply_msg, m) != LV_RESULT_OK) {
        printf("[UI] 界面更新投递失败，丢弃一次\n");
        free(m->reply);
        free(m);
    }
}

/* 提醒弹窗 / 报警页 / 关报警 / 报错页 / 询问页：都是"建对象"，同样只能在 LVGL 线程做 */

typedef struct {
    char *a;        /* 标题 / 正文 */
    char *b;        /* 正文（提醒/报错才有第二个参数） */
    int   kind;     /* 0=提醒 1=报警 2=关报警 3=报错 4=弹询问页 5=撤询问页 */
} ui_panel_msg_t;

static void ui_apply_panel(void *arg)
{
    ui_panel_msg_t *m = (ui_panel_msg_t *)arg;

    switch (m->kind) {
        case 0:  robot_ui_show_reminder(m->a, m->b); break;
        case 1:  robot_ui_show_alarm(m->a);          break;
        case 2:  robot_ui_close_alarm();             break;
        /* 4/5：「检测到异常 → 先问一句」那一页（异常声响二次确认） */
        case 4:  robot_ui_show_ask_alarm(m->a);      break;
        case 5:  robot_ui_close_ask_alarm();         break;
        default: robot_ui_show_error(m->a, m->b);    break;
    }

    free(m->a);
    free(m->b);
    free(m);
}

static void ui_post_panel(int kind, const char *a, const char *b)
{
    ui_panel_msg_t *m = malloc(sizeof(ui_panel_msg_t));

    if (m == NULL) {
        return;
    }

    m->kind = kind;
    m->a    = ui_strdup(a);
    m->b    = ui_strdup(b);

    if (ui_async_call(ui_apply_panel, m) != LV_RESULT_OK) {
        printf("[UI] 弹窗投递失败，丢弃一次\n");
        free(m->a);
        free(m->b);
        free(m);
    }
}

/* 语义化入口，调用点读起来跟直接调 robot_ui_* 一样 */

static void ui_post_reminder(const char *title, const char *content)
{
    ui_post_panel(0, title, content);
}

static void ui_post_alarm(const char *content)
{
    ui_post_panel(1, content, NULL);
}

static void ui_post_close_alarm(void)
{
    ui_post_panel(2, NULL, NULL);
}

static void ui_post_error(const char *title, const char *content)
{
    ui_post_panel(3, title, content);
}

/* 询问页（异常声响二次确认）：弹 / 撤，都只是"建控件"，同样投到 LVGL 线程 */
static void ui_post_ask_show(const char *reason)
{
    ui_post_panel(4, reason, NULL);
}

static void ui_post_ask_close(void)
{
    ui_post_panel(5, NULL, NULL);
}

/* ==================== 「检测到异常 → 先问一句，确认了才报警」 ==================== */
/*
 * 用户拍板：异常声音**不直接报警** —— 先弹一页问一句，用户二次确认了才报警。
 * 因为板子的屏幕现在可能是坏的/被拆下来的，"有人点屏幕"绝不能是报警的前提，
 * 所以确认有三条路，谁先给出回答都算数：
 *   ① 屏幕：询问页的两个按钮（robot_ui.c 建页面，回答回到 ask_btn_result_cb）
 *   ② 网络：MQTT 下行 {"action":"confirm_alarm","confirm":true|false}
 *          -> on_ai_command_received() 的分支 -> ui_post_ask_answer()
 *   ③ 兜底：20 秒无人应答自动按「不用了」处理（ask_timeout_thread）
 * 三条最后都收口到 ask_finish()：一处记账、一处决定报不报警，不会各走各的。
 *
 * 线程纪律：本节的入口可能在任意线程（声音检测线程 / network_task / hello_app），
 * 所以只做"记账 + 投递"；界面动作一律 ui_post_*，真正的动作只在 LVGL 线程里的
 * ask_finish() 里发生（三条入口都先投到 LVGL 线程）。
 *
 * 去重：同一时间只允许一条 pending 询问（新的来就丢弃并打日志，**不覆盖**）；
 * 用户否认后同一原因在 ROBOT_ASK_ALARM_DENY_HOLD_MS（60 秒）内不再询问 ——
 * "同一原因"按 ask_reason_key() 归一化后的**类别**算（置信度数字不参与比较，
 * 否则同一个类每回都像新原因，静默期等于没有）。
 *
 * 报警去重：两条确认入口（屏幕/MQTT 与语音追问）都可能对同一次异常给出"确认"。
 * 一次异常事件只允许报警一次，闸门是 robot_ui_alarm_claim()（见下面）。
 */

#define ASK_ALARM_POLL_MS   100     /* 超时看门狗线程的轮询粒度 */
#define ASK_ALARM_REASON_MAX 96     /* 和 robot_ui.c 那边留的缓冲同量级就够 */
#define ASK_ALARM_SRC_MAX    24

/* 单调毫秒时钟。和摔倒链的 fall_now_ms() 同一个算法、同一个理由：不用
 * lv_tick_get()，因为这里计时的是工作线程，不该受界面刷新节奏影响。 */
static uint32_t ask_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint32_t)ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000));
}

/* 把 reason 归一化成"比较用的类别键"：把尾部的置信度数字摘掉。
 *
 * 为什么必须归一化、而不能直接拿整条 reason 当 key（这就是那个"静默期永远失效"
 * 的 bug 的根因）：屏幕上要显示置信度（"疑似跌倒撞击 87%"，演示时有用），可
 * 置信度每次都不一样 —— 拿整条 reason 比，同一个类永远比不上，60 秒静默期就
 * 形同虚设（用户点了"不用了"，同类异常照样反复弹框）。
 * 所以：**显示照旧带数字，比较只看类别**。
 *
 * 认两种上游写法，都是 ai_companion_main.c 里原样传给 ui_post_ask_alarm() 的：
 *   "疑似跌倒撞击 87%"          <- sound_event_cb（本地小模型，尾部 " NN%"）
 *   "异常声音: 摔倒了 (0.87)"   <- sound_detect_callback（老检测器，尾部 " (0.NN)"）
 * 两种都把尾部数字摘掉；别的形式（例如 MQTT 自测传进来的"玻璃碎声"）原样保留。 */
static void ask_reason_key(const char *reason, char *out, size_t out_size)
{
    size_t n;
    size_t end;
    size_t i;

    if (out == NULL || out_size == 0) {
        return;
    }
    if (reason == NULL) {
        out[0] = '\0';
        return;
    }

    n = strlen(reason);
    end = n;

    if (n >= 3 && reason[n - 1] == ')') {
        /* "…(0.87)"：从右往左走，括号里只允许数字和小数点 */
        i = n - 1;
        while (i > 0 && reason[i - 1] != '(') {
            i--;
            if (reason[i] != '.' && (reason[i] < '0' || reason[i] > '9')) {
                break;
            }
        }
        if (i > 0 && reason[i - 1] == '(' && i < n - 1) {
            end = i - 1;
        }
    } else if (n >= 3 && reason[n - 1] == '%') {
        /* "… 87%"：'%' 前面得是数字 */
        i = n - 1;
        while (i > 0 && reason[i - 1] >= '0' && reason[i - 1] <= '9') {
            i--;
        }
        if (i < n - 1) {
            end = i;
        }
    }

    /* 数字前面那个空格也不算类别的一部分 */
    while (end > 0 && reason[end - 1] == ' ') {
        end--;
    }

    /* 整条都是数字（理论上不会有这种 reason）：那就别摘了，原样当 key 更安全 */
    if (end == 0) {
        end = n;
    }

    if (end > out_size - 1) {
        end = out_size - 1;
    }

    memcpy(out, reason, end);
    out[end] = '\0';
}

static pthread_mutex_t g_ask_lock = PTHREAD_MUTEX_INITIALIZER;
static bool            g_ask_pending = false;   /* 有一条询问在等回答（同一时间只允许一条）*/
static char            g_ask_reason[ASK_ALARM_REASON_MAX];  /* 待回答那条问的是什么 */
static unsigned        g_ask_gen = 0;           /* 每开始/结束一条 +1（超时线程据此作废）*/

/* 用户否认的记忆：一条就够。两分钟内出现两种不同 reason 的异常声音极少见，
 * 记多条要多一套淘汰逻辑，对一天工期的比赛没意义。
 * 存的是 ask_reason_key() 归一化之后的**类别键**（没有置信度数字），
 * 否则同一个类的置信度每次都不一样、这条静默期永远比不上。 */
static char            g_ask_deny_reason[ASK_ALARM_REASON_MAX];
static uint32_t        g_ask_deny_ms = 0;       /* 0 = 还没记过 */

/* 回答的名字（日志里要能看出"确认 / 否认 / 作废"是三种不同的东西） */
static const char *ask_answer_name(int confirmed)
{
    if (confirmed == 1) {
        return "是的，报警";
    }
    if (confirmed == 0) {
        return "不用了";
    }
    return "作废（页面让位）";
}

/* ==================== 报警去重闸：一次异常只报一次警 ==================== */
/*
 * 同一次异常现在有两条确认入口，用户拍板**两条都保留**：
 *   ① 屏幕按钮 / MQTT confirm_alarm -> 本文件 ask_finish()（报 sound_abnormal）
 *   ② 语音追问（hello_app 的 ask_flow_escalate：回答里出现"救命/疼"这类词）
 *      -> 报 sound_emergency
 * 两条都能确认，也都会各自报警：用户先在屏幕上点了「是的，报警」，再喊一句
 * "救命"，同一次异常就被上报两遍（家人的手机上就是两条报警）。
 *
 * 做法（谁先执行谁置位，另一方放弃并打日志）：
 *   - g_alarm_seq：事件序号，+1 就是"一次新事件的起点"。两处 +1：
 *     ui_post_ask_alarm()（"有新异常要问一句"）和 alarm_claim(new_event=true)
 *     （主菜单「紧急联系家人」—— 按下按钮本身就是一次新事件）。
 *     三条确认路都在各自的序号之内。
 *   - g_alarm_claimed_seq：这个序号已经被哪一路领走报警了。
 *   领得到就执行，领不到就整块放弃（页面/铃声/上报都不做：赢家那一路已经全做过）。
 * 序号一变闸门自动重开，所以这**不会**变成"一辈子只报一次警"。
 *
 * 线程纪律：几条路在不同线程（LVGL 线程 / hello_app 的 main_loop 任务），
 * 判-记必须在同一把锁里做完，所以复用 g_ask_lock。
 */

static unsigned g_alarm_seq = 0;            /* 事件序号：每"一次新事件" +1 */
static unsigned g_alarm_claimed_seq = 0;    /* 已被领走报警的序号（0 = 还没有）*/

/* 领票（闸门的唯一实现）。new_event = true 表示"这是一次全新事件"：领票前先
 * 把事件序号 +1 —— 序号一变，闸门自动重开（见上面那句"不会变成一辈子只报一次警"）。
 *
 * 为什么紧急呼叫必须走 new_event：g_alarm_claimed_seq 记的是**上一个**序号已经
 * 报过警了；序号不换的话，几分钟前那次异常声响的记号会把老人刚按下的紧急呼叫
 * 判成"已经报过了"而挡掉 —— 漏报比重复上报严重得多。这和 ui_post_ask_alarm()
 * 里"新异常就是新事件"是同一条规矩。
 * 反过来的情形也是对的：按下紧急呼叫时如果正挂着一条询问，序号一换，那条询问
 * 随后的"确认"就领不到票了 —— 屏幕上报警页已经弹起来，同一次呼叫不该报两遍。 */
static bool alarm_claim(const char *src, bool new_event)
{
    bool mine;

    pthread_mutex_lock(&g_ask_lock);

    if (new_event) {
        g_alarm_seq++;
    }

    /* "还没有过任何事件"（g_alarm_seq == 0）时不设闸：没有事件序号就没法判断
     * 谁和谁同一次，宁可多报一次也不能漏报（漏报是安全功能的失败）。 */
    if (g_alarm_seq != 0 && g_alarm_claimed_seq == g_alarm_seq) {
        mine = false;
    } else {
        g_alarm_claimed_seq = g_alarm_seq;
        mine = true;
    }

    pthread_mutex_unlock(&g_ask_lock);

    printf("[Alarm] 报警去重闸：%s（来源=%s，事件序号=%u）\n",
           mine ? "本路执行报警" : "这次异常已经报过警了，本路放弃",
           (src != NULL) ? src : "-", g_alarm_seq);

    return mine;
}

/* ★ 跨模块入口（声明在 robot_ui.h，hello_app 那边弱声明）：
 * 返回 true = 这次报警由本路执行；false = 另一条确认入口已经报过了，本路放弃。 */
bool robot_ui_alarm_claim(const char *src)
{
    return alarm_claim(src, false);
}

/* ★ 报警动作的**唯一收口**：屏幕/MQTT 确认（ask_finish）、语音追问带来的确认、
 *   主菜单「紧急联系家人」（emergency_call_handler）三条路都到这里 —— "报警页 +
 *   警音 + MQTT + 手机推送"永远只有这一份实现，谁也不另写一条。
 *
 * 线程纪律：只在 **LVGL 线程**里跑（三个调用方都在这一条线程上）。里面没有任何
 * 阻塞动作：ui_post_alarm 只是投递到下一拍、sm_handle_event 是纯内存、
 * report_alarm_queued 只把消息拷进队列；真正出声、发 MQTT 都在各自的工作线程里
 * （见 robot_ui.c 的 alarm_sound_worker）。
 *
 * 返回 true = 这次报警由本路执行；false = 别路已经报过了，本路只记日志。 */
static bool alarm_escalate(const char *src, bool new_event,
                           const char *alarm_type, const char *text)
{
    /* ① 去重闸：一次异常只报一次警。没领到票就整块放弃 —— 报警页、铃声、上报
     *    三件事都不做，因为赢家那一路已经全做过了（要么是本函数下面那三步，
     *    要么是 MQTT 的 start_alarm 那条路），再来一遍就是家人手机上两条报警。 */
    if (!alarm_claim(src, new_event)) {
        printf("[Alarm] %s：这次已经报过警了，本路不重复报警（只记这一行）\n", src);
        return false;
    }

    /* ② 报警页 + 响铃 + MQTT(ui)：复用现成入口（robot_ui_show_alarm 内部就是
     *    "先切页面 -> alarm_trigger 出声 + 上报"）。 */
    ui_post_alarm(text);

    /* ③ 状态机：和"声音检测到异常"、"机主摔倒"同一条事件（不为此新增状态） */
    if (g_ai_initialized) {
        sm_handle_event(&g_sm_ctx, SM_EVENT_ALARM_DETECTED);
    }

    /* ④ MQTT + 手机 Bark：走既有通道（report_alarm_queued 内部已经带了一次
     *    push_send_alarm，所以这里**不再**单独推一条）。上面 ② 那条上报的
     *    type 是写死的 "ui"，这条才带真实原因。 */
    if (report_alarm_queued(alarm_type, text) < 0) {
        printf("[Alarm] MQTT 报警没入队（报警页和铃声不受影响）\n");
    }

    return true;
}

/* 结束一条询问：**唯一收口**，三条确认路最后都到这里。
 * 只在 LVGL 线程里跑（入口都先投递），所以里面可以直接投界面动作。 */
static void ask_finish(int confirmed, const char *src)
{
    char reason[ASK_ALARM_REASON_MAX];
    char text[160];

    pthread_mutex_lock(&g_ask_lock);

    if (!g_ask_pending) {
        pthread_mutex_unlock(&g_ask_lock);
        printf("[Ask] 没有待回答的询问（来源=%s，回答=%s），忽略\n",
               src, ask_answer_name(confirmed));
        return;
    }

    snprintf(reason, sizeof(reason), "%s", g_ask_reason);
    g_ask_pending = false;
    g_ask_gen++;

    /* 只有"用户明确否认/超时"才记静默期：-1（作废，页面被报警页顶掉）不记 ——
     * 那不是用户的回答，记下去会让同一原因 60 秒内问不出来。
     * 这里存的是**归一化之后的类别键**（置信度数字不进去），比较时同样归一化，
     * 否则同一个类的置信度每次都不一样、这条静默期永远比不上（原 bug）。 */
    if (confirmed == 0) {
        ask_reason_key(reason, g_ask_deny_reason, sizeof(g_ask_deny_reason));
        g_ask_deny_ms = ask_now_ms();
    }

    pthread_mutex_unlock(&g_ask_lock);

    /* 不管哪种回答，询问页都不该再留在屏幕上 */
    ui_post_ask_close();

    if (confirmed == 1) {
        printf("[Ask] ★ 用户确认报警（原因=%s，来源=%s）→ 走既有报警入口\n",
               reason, src);

        snprintf(text, sizeof(text), "检测到异常声响：%s", reason);

        /* 去重闸 + 报警页 + 状态机 + 上报，四件事全在收口 alarm_escalate 里
         * （和紧急呼叫走的是同一份实现，这里一个字都没有重写）。
         * new_event = false：事件序号是 ui_post_ask_alarm() 那一步 +1 的，
         * 本函数只是这一次事件里的一条确认路，不是新事件。
         *
         * 没领到票（语音追问那条路已经报过警了）就在收口里记一行日志收工 ——
         * 报警页、铃声、上报三件事都不做，赢家那一路已经全做过了。 */
        alarm_escalate(src, false, "sound_abnormal", text);
    } else if (confirmed == 0) {
        printf("[Ask] 确认未通过（原因=%s，来源=%s）：不报警；"
               "同一原因 %u 秒内不再询问\n",
               reason, src,
               (unsigned)(ROBOT_ASK_ALARM_DENY_HOLD_MS / 1000));
    } else {
        /* -1：这一页被作废了（报警页要盖上来，询问页让位）——
         * 既不算确认也不记静默期，只把待答状态清掉、页面撤下。 */
        printf("[Ask] 询问作废（原因=%s，来源=%s）：页面让位，不记"
               "「用户否认」\n", reason, src);
    }
}

/* 询问页按钮的回答（robot_ui.c 在 **LVGL 线程**里回调过来）。
 * 和 MQTT / 超时那两条路走同一个收口 ask_finish()，所以"点按钮"和"手机上确认"
 * 的行为逐字一致。reason 参数不用：main.c 自己那份 g_ask_reason 才是权威
 * （页面上的字符串是清洗过的显示副本）。 */
static void ask_btn_result_cb(int confirmed, const char *reason, void *arg)
{
    (void)reason;
    (void)arg;

    if (confirmed == 1 || confirmed == 0) {
        ask_finish(confirmed, "屏幕按钮");
    } else {
        /* -1：报警页上来时 robot_ui.c 让位（用户没点过任何按钮） */
        ask_finish(-1, "报警页让位");
    }
}

/* 待回答的回答消息（投递用）：回答 + 是谁给的（日志里要能看出哪条路生效了） */
typedef struct {
    int  confirmed;
    char src[ASK_ALARM_SRC_MAX];
} ui_ask_answer_msg_t;

static void ui_apply_ask_answer(void *arg)
{
    ui_ask_answer_msg_t *m = (ui_ask_answer_msg_t *)arg;

    ask_finish(m->confirmed, m->src);
    free(m);
}

/* 非 LVGL 线程用：投一次"回答"（MQTT 下行 / 超时看门狗走这里）。
 * 投到 LVGL 线程再处理的原因：ask_finish() 要撤页面（robot_ui_close_ask_alarm()
 * 是建/删控件），跨线程碰 LVGL 会踩 rendering_in_progress 断言把 app 打死。 */
static void ui_post_ask_answer(int confirmed, const char *src)
{
    ui_ask_answer_msg_t *m = malloc(sizeof(ui_ask_answer_msg_t));

    if (m == NULL) {
        printf("[Ask] 内存不够，这次的回答（%s，来源=%s）投不出去\n",
               confirmed ? "是的，报警" : "不用了", src);
        return;
    }

    m->confirmed = confirmed;
    snprintf(m->src, sizeof(m->src), "%s", (src != NULL) ? src : "-");

    if (ui_async_call(ui_apply_ask_answer, m) != LV_RESULT_OK) {
        printf("[Ask] 回答投递失败（内存紧），丢弃一次\n");
        free(m);
    }
}

/**
 * @brief  跨模块入口（声明在 hello_app 的 ai_sound_detect.h）：另一条确认路
 *         （hello_app 的语音追问）对这次异常已经有结论了，询问页让位。
 *
 * 走的语义是「作废」（ask_finish(-1)，和「报警页要盖上来」同一条路）：撤页面，
 * **既不报警、也不记「用户否认」的 60 秒静默期** —— 它不代表用户回答过任何东西。
 * 所以这一路无论怎么调都变不成一次报警，也不会把同一原因的询问堵死。
 *
 * 与另一端（robot_ui_show_alarm() 登记 ai_companion_ask_abort()）合起来，就是
 * 「先询问、二次确认后才报警」那两条入口的互斥：先有结论的那一路收尾时，
 * 把另一路也撤掉，屏幕上永远只有一套在问、也只报一次警。
 *
 * 没有 pending 询问时是**空操作**（连日志都不打）：语音追问每一轮收尾都会调它。
 * 任何线程可调（内部照旧投到 LVGL 线程）。
 */
void ui_post_ask_standdown(const char *src)
{
    bool pending;
    const char *who = (src != NULL) ? src : "另一条确认路";

    pthread_mutex_lock(&g_ask_lock);
    pending = g_ask_pending;
    pthread_mutex_unlock(&g_ask_lock);

    if (!pending) {
        return;
    }

    printf("[Ask] %s 已经有结论，询问页让位（不报警、不记否认）\n", who);

    /* -1 就是「作废」：投到 LVGL 线程之后走 ask_finish(-1) 那一条路 */
    ui_post_ask_answer(-1, who);
}

/* 超时看门狗：等到自己那条询问超时就按「不用了」处理。
 * 为什么不用 lv_timer：那样计时精度取决于界面刷新节奏，而且**界面卡住时连
 * "自动放弃"也一起卡住** —— 那正是要避免的"卡在询问页把演示拖死"。
 * 本线程每 100ms 醒一次、只看两个变量；gen 变了（已有人回答 / 又开了新一条）
 * 就自己收摊，不占用资源。 */
static void *ask_timeout_thread(void *arg)
{
    unsigned gen = (unsigned)(uintptr_t)arg;
    long waited = 0;

    while (waited < (long)ROBOT_ASK_ALARM_TIMEOUT_MS) {
        bool still;

        usleep(ASK_ALARM_POLL_MS * 1000);
        waited += ASK_ALARM_POLL_MS;

        pthread_mutex_lock(&g_ask_lock);
        still = g_ask_pending && (g_ask_gen == gen);
        pthread_mutex_unlock(&g_ask_lock);

        if (!still) {
            return NULL;    /* 已经有人回答了（屏幕 / MQTT），本线程没用了 */
        }
    }

    printf("[Ask] 询问超时（%u 秒无人应答），自动按「不用了」处理\n",
           (unsigned)(ROBOT_ASK_ALARM_TIMEOUT_MS / 1000));

    ui_post_ask_answer(0, "超时");
    return NULL;
}

/* ★ 公开入口（声明在 robot_ui.h）：任何线程都能调 —— "检测到异常，先问用户一句"。
 * 检测线程（app/hello_app 那边的声音事件识别）和云端工具都调它，
 * 不需要关心界面到底弹没弹出来：屏幕坏了的时候本来就不该关心，
 * MQTT 确认和 20 秒超时那两条路照样有效。 */
void ui_post_ask_alarm(const char *reason)
{
    char reason_copy[ASK_ALARM_REASON_MAX];
    char key[ASK_ALARM_REASON_MAX];
    pthread_attr_t attr;
    pthread_t tid;
    unsigned gen;
    uint32_t now;

    if (reason == NULL || reason[0] == '\0') {
        reason = "异常声响";
    }

    /* 中断上下文里不能做（要起线程、要取锁）—— 和 fall_alarm_trigger
     * 那道 up_interrupt_context() 检查同一类原因 */
    if (up_interrupt_context()) {
        printf("[Ask] 在中断上下文里，不发起询问（要起线程）\n");
        return;
    }

    /* 归一化成"类别键"：静默期比的是类别，不是带置信度的整条 reason */
    ask_reason_key(reason, key, sizeof(key));

    pthread_mutex_lock(&g_ask_lock);

    /* ⓪ 事件序号 +1：**每一次"有新异常、要问一句"都是一次新事件**，包括这一次
     *    因为"已经在问"或"刚被否认过"而丢掉的那种 —— 新异常就是新事件。
     *    报警去重闸（robot_ui_alarm_claim）按这个序号开合：漏掉这一步，上一次
     *    事件的"已报警"记号会把这一次真正的新异常一起挡住，报警就永远出不去了
     *    （漏报比重复上报严重得多）。 */
    g_alarm_seq++;

    /* ① 已经在问一条了：**丢掉新的，不覆盖旧的**。覆盖会让"屏幕上问的问题"
     *    和"被回答的那条"对不上：用户点的明明是 A，报出去的却是 B。 */
    if (g_ask_pending) {
        pthread_mutex_unlock(&g_ask_lock);
        printf("[Ask] 已有一条询问在等回答（原因=%s），本次丢弃（原因=%s）\n",
               g_ask_reason, reason);
        return;
    }

    /* ② 同一原因刚被否认过：60 秒静默期里不再问第二遍。
     *    比的是归一化之后的类别键（key）、不是带置信度的整条 reason ——
     *    理由见 ask_reason_key() 上面那一段。 */
    now = ask_now_ms();
    if (g_ask_deny_ms != 0 &&
        (uint32_t)(now - g_ask_deny_ms) < (uint32_t)ROBOT_ASK_ALARM_DENY_HOLD_MS &&
        strcmp(g_ask_deny_reason, key) == 0) {
        unsigned left = (unsigned)((ROBOT_ASK_ALARM_DENY_HOLD_MS -
                                    (uint32_t)(now - g_ask_deny_ms)) / 1000);
        pthread_mutex_unlock(&g_ask_lock);
        printf("[Ask] 原因 %s 刚被用户否认过（类别键=%s），%u 秒内不再询问"
               "（本次丢弃）\n", reason, key, left);
        return;
    }

    snprintf(g_ask_reason, sizeof(g_ask_reason), "%s", reason);
    g_ask_pending = true;
    gen = ++g_ask_gen;
    snprintf(reason_copy, sizeof(reason_copy), "%s", g_ask_reason);

    pthread_mutex_unlock(&g_ask_lock);

    printf("[Ask] 检测到异常，先问一句再决定报警：原因=%s"
           "（%u 秒无人应答按「不用了」处理；MQTT confirm_alarm 也能回答）\n",
           reason_copy, (unsigned)(ROBOT_ASK_ALARM_TIMEOUT_MS / 1000));

    /* 弹页面 + 起超时看门狗。
     *
     * 注意这里**不看** robot_ui_bridge_is_ready() 那道闸门：屏幕这条路可能本来
     * 就不通（屏坏了/被拆了），把 MQTT 和超时两条确认路一起掐掉才是错的。
     * 界面还没起来时投进去只是排在队列里，等等级起来再画；真没界面，20 秒后
     * 这条询问会按「不用了」自己收尾，不会永远挂着不让人再问。 */
    ui_post_ask_show(reason_copy);

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 4096);

    if (pthread_create(&tid, &attr, ask_timeout_thread, (void *)(uintptr_t)gen) != 0) {
        printf("[Ask] 超时看门狗起不来：这条询问只能靠屏幕按钮 / MQTT 回答\n");
    }

    pthread_attr_destroy(&attr);
}

/* 一次语音请求失败 → 给老人看的一页。
 * 只在真的失败时弹（不做网络轮询），所以开机网络还没起来时不会刷屏。 */

static void voice_show_net_error(const char *what, int err)
{
    char content[192];

    if (err == -ENOENT) {
        snprintf(content, sizeof(content),
                 "%s没成功：这台设备里没有配好密钥，\n请让维护的人用串口检查配置。", what);
        ui_post_error("AI 未配置", content);
        return;
    }

    if (err == -EIO) {
        snprintf(content, sizeof(content),
                 "%s没成功：连不上服务器。\n"
                 "请检查两件事：\n"
                 "1. 板子的 USB 线插好没有；\n"
                 "2. 电脑上的网络共享还开着没有。", what);
        ui_post_error("网络不通", content);
        return;
    }

    snprintf(content, sizeof(content),
             "%s没成功（错误码 %d）。\n等一下再试一次。", what, err);
    ui_post_error("出错了", content);
}

/* 一轮语音聊天结束（提交后 / 播放完 / 取消 / 失败）统一回到待机。
 *
 * 为什么要专门收这一下：voice_chat_handler() 一开录音就把状态栏设成「聆听中」，
 * 但**结束路径原先一条都没有复位**，于是状态栏会永远停在「聆听中」—— 用户实测
 * 反馈过（识别失败之后也是这样）。现在只在真正开着麦克风时才是「聆听中」，
 * 播放时是「说话中」，其余时间一律回待机。 */
static void voice_ui_idle(void)
{
    robot_ui_set_status(ROBOT_STATUS_IDLE);
    robot_ui_set_face(ROBOT_FACE_HAPPY);
}

/* ==================== 常态听音（工作流 C）的接线 ==================== */
/*
 * 模块本体在 ambient_listen.c。这里只做三件事：
 *   1) 登记回调（结果用 ui_post* 投递到 LVGL 线程）；
 *   2) 告诉它"喇叭在响/麦克风被占"时不要开麦（busy 回调）；
 *   3) 在"要用麦克风 / 一整轮结束"的地方 pause / resume。
 * 默认关（配置键 enable_ambient_listen），关着时这些调用都是空操作。
 */

/* 任何线程都能读这两个状态；返回 true = 现在别开麦 */
static bool ambient_busy(void *arg)
{
    (void)arg;

    return audio_is_playing(&g_audio_ctx) || audio_is_recording(&g_audio_ctx);
}

/* ⚠️ 这个回调在**听音线程**里跑：只能投递，不能碰控件；
 * msg->text / msg->reply 是它栈上的缓冲，返回后即失效（ui_post* 会拷一份）。 */
static void ambient_on_event(const ambient_listen_msg_t *msg, void *arg)
{
    (void)arg;

    if (msg == NULL) {
        return;
    }

    if (msg->type == AMBIENT_EVENT_TRIGGER) {
        printf("[Ambient] 检到突发大音量（%d 倍基准，peak=%d），开始识别\n",
               msg->level, msg->peak);
        ui_post(ROBOT_STATUS_LISTENING, ROBOT_FACE_SURPRISED, NULL);
    } else if (msg->type == AMBIENT_EVENT_RESULT) {
        if (msg->reply != NULL) {
            ui_post_reminder("听到异响", msg->reply);
        } else if (msg->text != NULL) {
            ui_post_reminder("听到异响", msg->text);
        }
        ui_post(ROBOT_STATUS_IDLE, ROBOT_FACE_HAPPY, NULL);
    } else {
        printf("[Ambient] 处理失败: %d\n", msg->err);
        ui_post(ROBOT_STATUS_IDLE, ROBOT_FACE_HAPPY, NULL);
    }
}

/* ======== 语音聊天这一轮怎么用音频设备（2026-09-21：让路协议已删） ======== */
/*
 * 本板音频**半双工**、驱动状态**整机一份**：同一时刻只能有一个方向在跑。
 * 2026-09-14 到 2026-09-21 之间，robot_ui 要出声/录音前会先请 hello_app 的
 * ai_companion 交出它的常开麦（yield → 轮询 → reclaim 那套握手）。那套协议
 * **整套删掉了**（用户拍板：「关闭让路…不需要做麦克风转让，因为我们的麦是常开的」；
 * 真机故障形状和取舍见 app/hello_app/ai_companion_req.h 头上）。
 *
 * 现在这一轮的分工：
 *   - 要出声：直接 audio_play_start()。它内部会做半双工切割（ai_audio.c 的
 *     audio_prepare_output / audio_resume_record：先把正在录的停掉，播完按原配置
 *     恢复），所以播提醒/播回复都不再需要跟谁申请；
 *   - 要录音（语音聊天 / 摔倒链）：先把"别的路径可能占着的录音"停掉
 *     （audio_record_stop + ambient_listen_pause），再 audio_record_start()；
 *     开不起来就是 -EBUSY，如实告诉用户，**不重试、不自旋**（用户要重试就点
 *     「再说一次」）。
 *
 * ⚠️ 开麦这一整套只能在工作线程里做：audio_record_stop() 会 join 录音线程
 * （上限 300ms），在 LVGL 线程里等就是把界面冻住 —— 这个项目已经因为同类问题冻过
 * （见上面 ui_post 那一段）。所以"停别人的 + 开自己这一路"整体交给
 * voice_open_thread，voice_chat_handler()（LVGL 线程）只负责起线程和即时反馈。
 */

/* 开麦失败时给老人看的那句话（弹窗状态行 + 状态栏对话区共用一份）。
 * 说"麦克风正忙"而不是"录音启动失败"：原因确实就是麦克风被另一路占着
 * （hello_app 的常开麦没让出来，或板级直接 -EBUSY 拒了这次 START），
 * 说人话他才知道等一会儿再试；技术细节留在串口日志里。 */
#define VOICE_MIC_BUSY_TEXT  "麦克风正忙\n请过一会儿再说"

/* 语音聊天这边的开麦记账（受 g_voice_audio_lock 保护）：
 *   g_voice_open_opening —— 有一条开麦线程正在停别人的录音 / 开自己这一路
 * 为什么要锁：置位/清位发生在两个线程里（开麦线程、LVGL 线程的错误路径），
 * 而"同一时刻只允许一条开麦线程"必须是原子的查-改 —— 两条线程一起
 * audio_record_start()，第二条一定拿到 -EBUSY，而它那份失败收尾会把第一条刚
 * 占住的麦还掉。 */
static pthread_mutex_t g_voice_audio_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_voice_open_opening = false;

/* "这一轮还想开麦吗"的令牌：LVGL 线程发号，开麦线程在动设备前比一次。
 * 用户关窗（×）/点「提交」/走主菜单（touch_ui_show_menu 会先按取消回调走一遍）
 * 都会推进它，于是还在开麦路上的那条线程会自己作废 —— 否则它可能在用户已经走了
 * 之后把麦克风开起来，而那一刻已经没有人会去停它（面板没了、状态栏也没人管），
 * 麦克风就永远被占着了。
 * 只由 LVGL 线程写、开麦线程读，32 位对齐的读写在这颗 Cortex-M 上是原子的，
 * 所以不需要锁（和 g_voice_pcm_full 一个路子）。 */
static volatile uint32_t g_voice_open_token = 0;

/* 试着占下"开麦"这件事：已经有一条开麦线程在跑就返回 false。
 * 为什么必须挡：两条线程一起 audio_record_start()，第二条一定拿到
 * -EBUSY，而它那份失败收尾会把第一条刚占住的麦还回去 —— 那是最不该有的自伤。 */
static bool voice_open_claim(void)
{
    bool claimed = false;

    pthread_mutex_lock(&g_voice_audio_lock);
    if (!g_voice_open_opening) {
        g_voice_open_opening = true;
        claimed = true;
    }
    pthread_mutex_unlock(&g_voice_audio_lock);

    return claimed;
}

static void voice_open_finish(void)
{
    pthread_mutex_lock(&g_voice_audio_lock);
    g_voice_open_opening = false;
    pthread_mutex_unlock(&g_voice_audio_lock);
}

/**
 * @brief  开麦工作线程：停掉别人的录音 -> 真的开麦 -> 回报界面
 *
 * @param  arg: 堆上那一格"这一轮的令牌"（voice_chat_handler 发的，本函数负责 free）
 *
 * 为什么必须单开一条短命线程，而不是在 voice_chat_handler() 里顺手做完：
 *   ① audio_record_start() 要碰设备（open / CONFIGURE / START，慢的时候几百
 *      毫秒），停别的路径还可能 audio_record_stop()（join 录音线程，上限 300ms），
 *      而 voice_chat_handler() 是 touch_ui 的开启回调、跑在 **LVGL 线程**里 ——
 *      在那里等就是冻界面；
 *   ② 录音本身由 ai_audio 的录音线程跑（audio_record_start 内部起的那条），
 *      所以这条线程只活到"麦开起来了"为止，detach 掉、不占常驻资源。
 *
 * 顺序：
 *   1) 令牌还对得上吗？对不上说明用户已经关窗/提交，这一刻**一个设备都不碰**；
 *   2) 停掉别的路径可能占着的录音、停掉常态听音；
 *   3) 这两步最坏几百毫秒，之后再比一次令牌 + 看面板还在不在，走了就不开麦
 *      —— 开了也没人再会去停它；
 *   4) audio_record_start()；
 *   5) 成没成都回报界面：成功 = 状态行"正在录音…"；失败 = 明说"麦克风正忙"
 *      （原来这里失败是静默的，用户只看到"说话没反应"）。
 *
 * ⚠️ 开麦失败（板级 -EBUSY）时**不重试、不自旋**：语音聊天是用户主动发起、
 *   失败了面板上还留着「再说一次」，让他自己再点一下就好。硬重试正好会撞上
 *   板级那套残留自愈最敏感的窗口。用户看到的是"麦克风正忙，请过一会儿再说"。
 */
static void *voice_open_thread(void *arg)
{
    uint32_t token = *(uint32_t *)arg;
    audio_record_config_t rec_cfg;
    const char *fail = NULL;
    bool opened = false;
    int  ret = 0;

    free(arg);

    if (g_voice_open_token != token) {
        printf("[VoiceChat] 开麦请求已作废（用户关窗/提交/又开了一轮），不碰设备\n");
        voice_open_finish();
        return NULL;
    }

    /* 先把"别的路径可能占着的录音"停掉（用户是主动点开语音聊天的，这一轮该用它），
     * 再停常态听音。audio_record_stop() 会 join 录音线程（上限 300ms）—— 放在这条
     * 工作线程里做，不压 LVGL 线程。 */
    if (audio_is_recording(&g_audio_ctx)) {
        printf("[VoiceChat] 麦克风被别的路径占着，先停掉\n");
        audio_record_stop(&g_audio_ctx);
    }

    /* 常态听音常驻占着麦克风（默认关，开了才有）：先按停，再开我们自己的录音。
     * pause() 在锁内同步 audio_in_stop()，返回即保证麦克风已放。 */
    ambient_listen_pause();

    /* 上面两步最坏加起来是几百毫秒，这段时间里用户完全可能已经关窗/提交：
     * 再比一次令牌 + 看面板还在不在，走了就不开麦 —— 开了也没人会去停它。 */
    if (g_voice_open_token != token || !touch_ui_voice_chat_active()) {
        printf("[VoiceChat] 这一轮已被用户放弃，不开麦\n");
    } else {
        /* 录音数据全交给 voice_record_callback 攒起来。VAD 不在这里开：
         * 一问一答由用户按「提交」决定说完没有，不等静音超时。 */
        memset(&rec_cfg, 0, sizeof(rec_cfg));
        rec_cfg.enable_vad = false;
        rec_cfg.data_callback = voice_record_callback;
        rec_cfg.user_data = NULL;

        ret = audio_record_start(&g_audio_ctx, &rec_cfg);
        if (ret == 0) {
            opened = true;
        } else {
            printf("[VoiceChat] audio_record_start 失败: %d\n", ret);
            fail = "录音设备打不开（板级拒了这次 START）";
        }
    }

    if (opened) {
        sm_handle_event(&g_sm_ctx, SM_EVENT_WAKEUP);
        touch_ui_set_voice_status("正在录音…\n说完点「提交」");
        ui_post(ROBOT_STATUS_LISTENING, ROBOT_FACE_THINKING, "聆听中...\n请说话。");
        printf("[VoiceChat] 麦克风已开（这一轮独占，直到提交/取消/放完）\n");
    } else {
        /* 失败：要**明确告诉用户**为什么说话没反应 —— 不能像以前那样只在串口
         * 留一行。报界面之前还得再确认一次"这一轮还是用户要的那一轮"。 */
        bool wanted = (g_voice_open_token == token) && touch_ui_voice_chat_active();

        if (fail != NULL && wanted) {
            printf("[VoiceChat] 开麦失败：%s，已告诉用户「%s」\n",
                   fail, VOICE_MIC_BUSY_TEXT);
            ui_post(ROBOT_STATUS_IDLE, ROBOT_FACE_HAPPY, VOICE_MIC_BUSY_TEXT);
            touch_ui_set_voice_status(VOICE_MIC_BUSY_TEXT);
            touch_ui_voice_chat_round_done();   /* 按钮变回「再说一次」，能直接重试 */
        } else if (fail != NULL) {
            printf("[VoiceChat] 开麦失败：%s（用户已经离开这一轮，界面不动）\n", fail);
        } else if (!wanted) {
            /* 用户已经走了（关窗/进菜单）：状态栏别停在"正在准备麦克风"那一行。
             * 面板还在的情况（点了「提交」）由那条路自己 voice_ui_idle()，不抢。 */
            ui_post(ROBOT_STATUS_IDLE, ROBOT_FACE_HAPPY, NULL);
        }
    }

    voice_open_finish();
    return NULL;
}

/* 播放完成回调（在 audio 的播放线程里跑 —— **不是 LVGL 线程**）。
 * 播放要持续好几秒，期间用户可能关窗甚至又开了一轮；用放音时记下的世代号
 * 一比就知道这条"播放完成"该不该写进界面（同一个时刻只可能有一段在放，
 * audio_play_start() 对第二段会返回 -EBUSY，所以这个静态量不会串台）。 */
static void voice_play_complete_callback(void *user_data)
{
    (void)user_data;

    /* 本回调在播放线程里：改界面必须投递（直接调 robot_ui_set_* 会踩 LVGL
     * 的 rendering_in_progress 断言，把 app 打死 —— 2026-09-14 的崩溃就是这个） */
    ui_post(ROBOT_STATUS_IDLE, ROBOT_FACE_HAPPY, NULL);

    if (g_voice_playing_gen != touch_ui_voice_chat_generation()) {
        return;
    }

    touch_ui_set_voice_status("回复完成\n点「再说一次」继续，或按 × 关闭");
    touch_ui_voice_chat_round_done();

    /* 一轮彻底结束（含最后一段 TTS 放完）：放常态听音回来 */
    ambient_listen_resume();
}

/* 把一段回复文字合成语音并播放（从 voice_llm_reply_callback 抽出来复用，只从
 * voice_worker 这个工作线程调；播放完成由 voice_play_complete_callback 收尾。
 * 任务的生命周期由调用方负责，最后统一 voice_task_unref 一次）。
 *
 * 状态行必须如实反映这一步到底走到哪了（用户看到过"正在播放"但其实一句都没
 * 合成出来）：合成期间显示"正在合成语音…"，只有 audio_play_start() 真的返回 0
 * 才显示"正在播放…"，失败就把 errno 一起显示出来。 */
static void voice_speak_reply(voice_task_t *task, const char *response)
{
    char   shown[VOICE_REPLY_TEXT_MAX + 256];  /* 清洗后给界面看的那份
                                                * （每个列表项最多涨 1 字节，
                                                *  留点余量免得尾字被切） */
    char   status[64];
    size_t tts_len = 0;
    size_t used;
    int ret;

    if (!voice_task_alive(task)) {
        return;
    }

    /* 界面和 TTS 用的是两份不同的文本：
     *   shown    -> 对话区（去掉 emoji / Markdown，字库渲染不了的不显示成方块）
     *   response -> voice_tts_speak()（原样送云端，emoji 它自己会读） */
    sanitize_for_display(response, shown, sizeof(shown));

    used = strlen(task->reply);
    if (used < sizeof(task->reply) - 1) {
        snprintf(task->reply + used, sizeof(task->reply) - used,
                 "\n\n智爱：%s", shown);
    }
    printf("[VoiceChat] AI 回复: %s\n", response);

    /* TTS：文字 -> 16k/单声道/s16le。缓冲走堆，随任务释放。 */
    voice_task_show(task, "正在合成语音…");

    if (task->tts == NULL) {
        task->tts_cap = VOICE_TTS_BUF_BYTES;
        task->tts = malloc(task->tts_cap);
    }
    if (task->tts == NULL) {
        printf("[VoiceChat] TTS 缓冲分配失败\n");
        voice_task_show(task, "内存不足\n请重试");
        touch_ui_voice_chat_round_done();
        return;
    }

    ret = voice_tts_speak(response, task->tts, task->tts_cap, &tts_len);
    if (ret < 0 || tts_len == 0) {
        printf("[VoiceChat] 语音合成失败: ret=%d tts_len=%zu\n", ret, tts_len);

        if (ret < 0) {
            snprintf(status, sizeof(status), "语音合成失败 (%d)\n请重试", ret);
            /* 网络类失败额外弹一页把原因说清楚（本函数在工作线程里，
             * 所以走 ui_post_error 投递） */
            voice_show_net_error("语音合成", ret);
        } else {
            snprintf(status, sizeof(status), "语音合成返回空音响\n请重试");
        }

        voice_task_show(task, status);
        touch_ui_voice_chat_round_done();
        return;
    }

    if (!voice_task_alive(task)) {   /* 合成也要几秒，中途可能被关窗 */
        return;
    }

    /* audio_play_start() 会先把 PCM 拷进自己的播放缓冲，所以 task 随后释放
     * 不影响播放。半双工设备此刻已经在录音结束时就腾出来了（见提交那一步）。 */
    g_voice_playing_gen = task->gen;
    ret = audio_play_start(&g_audio_ctx, (const int16_t *)task->tts,
                           tts_len / 2, voice_play_complete_callback, NULL);
    if (ret < 0) {
        printf("[VoiceChat] 播放失败: %d\n", ret);
        snprintf(status, sizeof(status), "播放失败 (%d)\n请重试", ret);
        voice_task_show(task, status);
        ui_post(ROBOT_STATUS_IDLE, ROBOT_FACE_HAPPY, NULL);   /* 工作线程：投递 */
        touch_ui_voice_chat_round_done();
    } else {
        /* 到这里才真的在出声，之前一直显示的是"正在合成语音…"。
         * 状态栏跟着改成"说话中"（扬声器在响，和"聆听中"是两回事）。
         * ⚠️ 本函数跑在语音工作线程里，改界面只能投递。 */
        voice_task_show(task, "正在播放…");
        ui_post(ROBOT_STATUS_SPEAKING, ROBOT_FACE_HAPPY, NULL);
    }
    /* 播放成功则由 voice_play_complete_callback 收尾 */
}

/* 工作线程：ASR -> mimo_chat -> TTS -> 播放。
 * 这一段全是阻塞的网络/音频调用，绝不能放 UI 线程里。 */
static void *voice_worker(void *arg)
{
    voice_task_t *task = (voice_task_t *)arg;
    char text[VOICE_ASR_TEXT_MAX] = {0};
    char reply[VOICE_REPLY_TEXT_MAX] = {0};
    int ret;

    if (!voice_task_alive(task)) {   /* 还没开工就被关窗了 */
        goto out;
    }

    printf("[VoiceChat] 开始识别 (%zu 字节)\n", task->pcm_len);
    ret = voice_asr_recognize(task->pcm, task->pcm_len, text, sizeof(text));

    if (!voice_task_alive(task)) {
        goto out;
    }

    if (ret < 0) {
        printf("[VoiceChat] 识别失败: %d\n", ret);
        voice_task_show(task, "识别失败\n请重试");
        /* 网络/配置类失败：额外弹一页把原因和怎么修写清楚（本函数在工作线程，
         * 走 ui_post_error 投递） */
        voice_show_net_error("语音识别", ret);
        touch_ui_voice_chat_round_done();
        goto out;
    }

    if (text[0] == '\0') {
        printf("[VoiceChat] 识别结果为空\n");
        voice_task_show(task, "没听清\n请大声一点说");
        touch_ui_voice_chat_round_done();
        goto out;
    }

    printf("[VoiceChat] 识别结果: %s\n", text);

    /* 对话区先显示"我说："，再等 AI 的回复 */
    snprintf(task->reply, sizeof(task->reply), "我说：%s", text);
    voice_task_show(task, "正在思考…");

    /* ASR 用过的 PCM 不再需要，早点还给堆 */
    free(task->pcm);
    task->pcm = NULL;
    task->pcm_len = 0;

    /* 直接同步发一次 HTTPS 对话请求。不走 ai_agent 的 llm_send_text() /
     * velaclaw_*：那条路会把消息投进 ai_agent 进程独有的总线队列，别的 app
     * 调用时队列锁没初始化，pthread_mutex_lock 直接撞 NXSEM_IS_MUTEX 断言，
     * 把整个 app 打死。mimo_chat() 内部跑 TLS + 云端推理、会阻塞几十秒，而
     * 本函数就是那个专门的工作线程，阻塞在这里是预期的。 */
    ret = mimo_chat(text, reply, sizeof(reply));

    if (!voice_task_alive(task)) {   /* 等回复期间可能被关窗 */
        goto out;
    }

    if (ret < 0 || reply[0] == '\0') {
        printf("[VoiceChat] AI 请求失败: %d\n", ret);
        voice_task_show(task, "AI 请求失败\n请稍后再试");
        voice_show_net_error("AI 回复", ret);
        touch_ui_voice_chat_round_done();
        goto out;
    }

    /* 拿到回复：合成 + 播放（播放成功由 voice_play_complete_callback 收尾） */
    voice_speak_reply(task, reply);

out:
    /* 工作线程的**所有**出口都汇到这里（识别失败/空/AI 失败/被关窗/正常走完），
     * 所以常态听音的 resume 放这一处就够 —— 这一轮不再需要独占麦克风了。
     * 播放还没结束时 audio_is_playing() 仍是 true，听音模块的 busy 回调会拦住它。 */
    ambient_listen_resume();
    voice_task_unref(task);
    return NULL;
}

/* 「提交」：结束录音，把这一轮 PCM 交给工作线程（在 LVGL 线程里被调用） */
static void voice_submit_handler(void *user_data)
{
    voice_task_t *task;
    unsigned char *pcm;
    size_t pcm_len;
    pthread_attr_t attr;
    pthread_t tid;

    (void)user_data;

    /* 用户按了「提交」= 这一轮不再要麦克风了：令牌 +1，让可能还在开麦路上的那条
     * 线程自己作废（否则它会在这一轮之后又把麦开起来，而那时面板上已经
     * 没有「提交」也没有人在等，谁也不会去停它）。 */
    g_voice_open_token++;

    /* 半双工设备：要播 TTS 就得先把麦克风让出来 */
    if (audio_is_recording(&g_audio_ctx)) {
        audio_record_stop(&g_audio_ctx);
    }

    /* 麦克风已经关了：状态栏不该再显示「聆听中」
     * （后面这段是识别/思考/播放，由弹窗里的状态行如实显示进度） */
    voice_ui_idle();

    /* 录音缓冲的所有权转移给任务（不做第二次 320 KB 拷贝；
     * 下一轮录音时再按需 malloc） */
    pthread_mutex_lock(&g_voice_lock);
    pcm = g_voice_pcm;
    pcm_len = g_voice_pcm_len;
    g_voice_pcm = NULL;
    g_voice_pcm_len = 0;
    g_voice_pcm_full = false;
    pthread_mutex_unlock(&g_voice_lock);

    if (pcm == NULL || pcm_len < VOICE_PCM_MIN_BYTES) {
        printf("[VoiceChat] 录音太短 (%zu 字节)，不提交\n", pcm_len);
        free(pcm);
        touch_ui_set_voice_status("没有录到声音\n请再说一次");
        touch_ui_voice_chat_round_done();
        return;
    }

    printf("[VoiceChat] 提交 %zu 字节 PCM (~%u 秒)\n",
           pcm_len, (unsigned int)(pcm_len / (16000 * 2)));

    /* 录音体检：ASR 只回"嗯。"这类极短结果时，先看这段音频本身有没有声音。
     * peak 接近 0 = 麦克风/设备没采到东西（不是模型的问题）——
     * 这条日志是排查"识别成嗯"时唯一的一手依据（对比 hw_test audio 的 peak）。 */
    {
        const int16_t *p = (const int16_t *)pcm;
        size_t n = pcm_len / sizeof(int16_t);
        int32_t peak = 0;
        int64_t sum = 0;
        size_t i;

        for (i = 0; i < n; i++) {
            int32_t v = p[i];

            if (v < 0) {
                v = -v;
            }
            if (v > peak) {
                peak = v;
            }
            sum += v;
        }

        printf("[VoiceChat] PCM 体检: %zu 帧 peak=%d avg=%ld\n",
               n, (int)peak, (long)((n > 0) ? (sum / (int64_t)n) : 0));
    }

    task = voice_task_new(pcm, pcm_len);
    if (task == NULL) {
        free(pcm);
        touch_ui_set_voice_status("内存不足\n请重试");
        touch_ui_voice_chat_round_done();
        return;
    }

    /* 线程必须 detach：用户随时可能关窗，没人会去 join 它；任务自带引用
     * 计数会自释放，所以工作线程跑多久都拖不住界面 */
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    /* ASR 那层要跑 TLS + HTTPS，栈给足（默认 16 KB 偏紧：ai_companion 里
     * 这条路是跑在 32 KB 的主线程上） */
    pthread_attr_setstacksize(&attr, 32768);

    if (pthread_create(&tid, &attr, voice_worker, task) != 0) {
        pthread_attr_destroy(&attr);
        voice_task_unref(task);
        touch_ui_set_voice_status("识别线程启动失败");
        touch_ui_voice_chat_round_done();
        return;
    }
    pthread_attr_destroy(&attr);

    touch_ui_set_voice_status("正在识别…");
}

/* 「×」：用户放弃这一轮。停录音、丢缓冲；在途的工作线程靠世代号自动作废。 */
static void voice_cancel_handler(void *user_data)
{
    (void)user_data;

    /* 用户按了「×」= 这一轮放弃：令牌 +1，让可能还在开麦路上的那条线程自己
     * 作废（那样它就不会在用户已经走了之后把麦开起来 —— 开了没人会去停）。 */
    g_voice_open_token++;

    if (audio_is_recording(&g_audio_ctx)) {
        audio_record_stop(&g_audio_ctx);
    }

    /* 麦克风关了、这一轮也丢了：状态栏回待机（否则会一直显示「聆听中」） */
    voice_ui_idle();

    /* 缓冲还给堆（下次录音会重新分配）。audio_record_stop() 已经 join 过
     * 录音线程，这里不会和 voice_record_callback 抢缓冲，锁只是兜底。 */
    pthread_mutex_lock(&g_voice_lock);
    if (g_voice_pcm != NULL) {
        free(g_voice_pcm);
        g_voice_pcm = NULL;
    }
    g_voice_pcm_len = 0;
    g_voice_pcm_full = false;
    pthread_mutex_unlock(&g_voice_lock);

    /* 正在播的 TTS 不在这里打断：audio_play_stop() 要 join 播放线程，
     * 在 LVGL 线程里等会把界面卡住。让它把这句放完。 */

    printf("[VoiceChat] 用户取消，这一轮丢弃\n");

    /* 这一轮到此为止：放常态听音回来（关的是"手动占用"那一位，
     * 喇叭还在响时听音模块自己的 busy 回调依然会拦住它开麦） */
    ambient_listen_resume();
}

/* 镜像面板底部「提交」：请 hello_app 的 ai_companion 立刻收尾当前这一段录音
 * （用户原话：「下面是关闭按钮，我希望换成提交按钮」）。
 *
 * 语义：老人说完话不用再干等 VAD 那 3 秒静音超时，点一下就把这一段送去识别。
 * 常开麦 + VAD 自动断句照旧工作，「提交」只是多一个"提前收尾"的入口。
 *
 * 两条纪律（都在 app/hello_app/ai_companion_req.h 里写死了）：
 *   1) **非阻塞**：ai_companion_voice_submit() 只读一次 hello_app 的状态快照、
 *      登记一个请求就返回；真正"结束这一段"的动作由 hello_app 自己那条录音线程
 *      下一帧做（VAD 判"说完了"本来就在那条线程上）。所以本回调里不许等、不许
 *      碰音频设备 —— 跨 app 同步动设备出过整组死掉的事故；
 *   2) 本回调在 **LVGL 线程**里被调：这里只有"记一行日志 + 投一句界面文字"，
 *      touch_ui_set_voice_status() 内部是 lv_async_call，安全（不能直接碰控件）。
 */
static void voice_mirror_submit_handler(void *user_data)
{
    int ret;

    (void)user_data;

    ret = ai_companion_voice_submit();

    if (ret == AI_COMPANION_SUBMIT_ACCEPTED) {
        /* 请求登记上了：给一句立刻的反馈。后面 VAD 那一套状态（正在想…/正在
         * 说话…）会盖掉它，不会留着这句不动的。 */
        printf("[VoiceChat] 「提交」已登记：等 ai_companion 收尾这一段\n");
        touch_ui_set_voice_status("好，这就去识别…");
        return;
    }

    if (ret == AI_COMPANION_SUBMIT_BUSY) {
        /* 它正忙（送识别 / 等大模型 / 出声 / 追问流程）：这一下插一脚没有意义，
         * 界面也**不能**提示"没听到" —— 面板上本来就写着"正在想…/正在说话…"，
         * 再插一句只会让人以为它坏了。什么都不写，只留日志。 */
        printf("[VoiceChat] 「提交」不适用：ai_companion 正在处理上一句\n");
        return;
    }

    /* NONE：当前没在累积语音（还没听到人说话），请求根本没登记 —— 这时候
     * 界面要如实说一句，否则老人会以为按钮坏了。不刷屏、不弹窗，就改状态行。 */
    printf("[VoiceChat] 「提交」：当前没有听到人说话，只在状态行提示\n");
    touch_ui_set_voice_status("没听到你说什么\n直接说话，说完再点「提交」");
}

/* ==================== 镜像面板自动弹出 ==================== */
/*
 * 用户原话：「ai 说话界面交互性太差了，又看不到回复又看不到自己说了什么」。
 * 病根在「镜像面板」要用户自己去主菜单点开 —— 老人根本不知道该去点，
 * 于是从开口到播报整段对话他什么都看不见。所以语音链路一有动静
 * （收到 user_said，或 voice_state=listening/speaking）就自动把面板弹出来，
 * 之后识别文字、AI 回复都往这个面板里写。
 *
 * 骚扰抑制（这一段唯一需要动脑的地方）：面板是浮层，老人可能按「关闭」把它
 * 收起来看脸/看时间。touch_ui 那边**没有"面板被关闭"的回调**，能跨线程读的
 * 只有 touch_ui_voice_chat_active()（"面板在不在"）。于是这里靠状态轮询推断：
 *   ① 有语音信号时看到面板开着 -> 记下"它开过"（g_voice_mirror_seen_open）；
 *   ② 之后再收到语音信号、面板却不在了 -> 说明它被关掉了，置
 *      g_voice_mirror_user_closed，**本次开机内不再自动弹**。
 * 这样"手动关掉 -> 下一条消息又把它怼回屏幕中间"的骚扰被彻底掐掉；
 * 老人想看还能从主菜单点开（那条路不经过这里，永远有效）。
 *
 * ⚠️ 轮询只能读到"面板在不在"，区分不出"被谁关的"：用户按关闭、进主菜单
 * （touch_ui_show_menu 会顺手关掉面板）、报警/提醒弹层抢占，看到的效果都是
 * 面板没了 —— 这三种一律按"别再弹了"处理，并在串口打一条日志便于现场排查。
 * 宁可少弹一次，也不要在老人明确关掉之后反复把它弹回来。
 */
static bool g_voice_mirror_seen_open = false;    /* 自动弹之后确实看到面板开起来过 */
static bool g_voice_mirror_user_closed = false;  /* 本次开机用户关过面板：不再自动弹 */

/**
 * 语音链路有动静时要不要把镜像面板弹出来。
 *
 * 只读标志位 + 读 touch_ui_voice_chat_active()（后者的注释写明可跨线程读，
 * 只是个指针判空，不碰 LVGL）。真正的建控件在 touch_ui_show_voice_mirror()
 * 内部的 lv_async_call 里 —— 本函数在 network_task 线程里被调，绝不能在这里
 * 直接建控件（跨线程碰 LVGL 会踩 rendering_in_progress 断言，见上面 ui_post 段）。
 */
static void voice_mirror_autoshow(void)
{
    bool active;

    if (g_voice_mirror_user_closed) {
        /* 本次开机已经关过了：静默不弹（日志在上面那条分支里打过一次） */
        return;
    }

    active = touch_ui_voice_chat_active();

    if (active) {
        /* 面板开着：可能是我们弹的，也可能是用户从主菜单点开的。
         * 记这一笔是为了它消失时能判断出"是被关掉的"。 */
        g_voice_mirror_seen_open = true;
        return;
    }

    if (g_voice_mirror_seen_open) {
        /* 开过、现在又没了 = 被关掉了。本次开机不再自动弹。 */
        g_voice_mirror_user_closed = true;
        printf("[AI] 镜像面板已被关掉，本次开机不再自动弹出（主菜单仍可手动打开）\n");
        return;
    }

    touch_ui_show_voice_mirror();
    printf("[AI] 语音链路有动静，自动弹出镜像面板\n");
}

/* ==================== 给 robot_ui_bridge.c 的三个门面 ==================== */
/*
 * robot_ui_bridge.c（编在同一个 app 里）要把 ai_companion 推来的文字/状态刷到
 * 界面上，但它够不到本文件下面这三个东西（都是 static）：
 *   sanitize_for_display()   —— 字库字形清洗，显示前必须过
 *   ui_post()                —— 状态栏/表情/对话区的跨线程投递
 *   voice_mirror_autoshow()  —— "用户关过面板就别再弹"的抑制逻辑
 *
 * 把 static 去掉也能编，但这三个名字在本文件里有二十多个调用点，动一处就得
 * 跟着改一片；加三个薄门面代价最小，而且保证**清洗规则和抑制规则只有一份**
 * —— 直调和 MQTT 两条路走的是同一段逻辑，不会各自漂移。
 *
 * 门面本身不做任何加工，也不加锁：ui_post / voice_mirror_autoshow 内部已经
 * 是"投递到 LVGL 线程"，sanitize_for_display 是纯计算（只读输入、只写调用方
 * 给的缓冲），三者都能被任何线程调。
 */

size_t robot_ui_bridge_sanitize_text(const char *in, char *out, size_t out_cap)
{
    return sanitize_for_display(in, out, out_cap);
}

void robot_ui_bridge_post_status(int status, int face, const char *reply)
{
    ui_post(status, face, reply);
}

void robot_ui_bridge_panel_autoshow(void)
{
    voice_mirror_autoshow();
}

/* ==================== 诊断：把内部状态交给 network_comm 报出去 ==================== */
/*
 * 串口线丢了，MQTT 是**唯一**的观测通道。心跳（30 秒一条）和 {"action":"diag"}
 * 都要报"robot_ui 自己知道的"那几个状态，但它们都是本文件的 static
 * （g_ai_initialized / g_audio_ctx / 镜像面板），network_comm 够不着，
 * 所以这里注册一个只读提供者回调（和 network_set_mqtt_callback 那套一样，
 * 注册一次，之后由它来问）。
 *
 * ⚠️ 这个回调跑在 **network_task（MQTT 收包）线程**里：只读几个全局量就返回，
 *    不许碰 LVGL、不许等设备、不许做网络请求 —— 它一慢，心跳和重连就一起停摆，
 *    那正是"连唯一的观测通道也没了"。
 *    这里只有三类读操作，都符合上面的要求：
 *      g_ai_initialized            —— 本文件的 bool（初始化完成后不再改）；
 *      audio_is_recording/playing  —— ai_audio 里的纯状态查询（别的线程早就在读它，
 *                                     见 ambient_busy()）；
 *      touch_ui_voice_chat_active()—— 头文件里写明可跨线程读（只是个指针判空）。
 *    取不到/不确定就留 -1：心跳里"不知道"和"没有"是两回事，不能混。
 */
static void local_state_probe(local_state_t *out)
{
    out->ai_init   = g_ai_initialized ? 1 : 0;
    out->recording = audio_is_recording(&g_audio_ctx) ? 1 : 0;
    out->playing   = audio_is_playing(&g_audio_ctx) ? 1 : 0;
    out->panel     = touch_ui_voice_chat_active() ? 1 : 0;
}

/* ==================== AI 命令回调处理 ==================== */

/* 摔倒链的"回答"入口（实现在文件后半的「疑似摔倒事件链」一节）。
 * 提前声明在这里的原因：MQTT 自测动作 fall_answer 要用它，而那个动作的分发
 * （on_ai_command_received）在文件前半。answer: 0 = 没有，1 = 有。 */
static void fall_set_answer(int answer, const char *from);

/**
 * 处理来自手机端或云端的 AI 命令
 * 成员二的 AI 模块可以通过此接口接收控制命令
 *
 * ⚠️ 本回调跑在 **network_task（MQTT 收包）线程**里，不是 LVGL 线程：
 * 所有改界面的动作都必须走 ui_post_xxx（内部 lv_async_call）——
 * 直接调 robot_ui 的 set/show 系列会踩 LVGL 的 rendering_in_progress 断言把 app
 * 打死（2026-09-14 的崩溃就是这一类）。
 */
static void on_ai_command_received(const char *action, const char *param)
{
    printf("AI command: action=%s, param=%s\n", action, param);

    if (strcmp(action, "start_voice") == 0) {
        /* 开始语音监听 */
        ui_post(ROBOT_STATUS_LISTENING, ROBOT_FACE_THINKING,
                "聆听中...\n请说话。");

        /* 调用成员二的 AI 模块开始录音 */
        if (g_ai_initialized) {
            /* 常态听音常驻占着麦克风：先按停（同步，返回时麦克风一定已经放了，
             * 紧接着 audio_record_start 不会 -EBUSY） */
            ambient_listen_pause();
            audio_record_start(&g_audio_ctx, NULL);
            sm_handle_event(&g_sm_ctx, SM_EVENT_WAKEUP);
        }
    }
    else if (strcmp(action, "stop_voice") == 0) {
        /* 停止语音监听 */
        ui_post(ROBOT_STATUS_IDLE, ROBOT_FACE_HAPPY, NULL);

        /* 调用成员二的 AI 模块停止录音 */
        if (g_ai_initialized) {
            audio_record_stop(&g_audio_ctx);
            sm_handle_event(&g_sm_ctx, SM_EVENT_VOICE_COMPLETE);
        }
    }
    else if (strcmp(action, "ai_reply") == 0) {
        /* 显示 AI 回复。param 是云端/MQTT 下发的原文，可能带 emoji 或 Markdown，
         * 显示前先清洗（字库渲染不了的码点会变成方块）；只洗显示这份。 */
        char shown[256];

        sanitize_for_display(param, shown, sizeof(shown));
        ui_post(ROBOT_STATUS_SPEAKING, ROBOT_FACE_HAPPY, shown);

        /* 镜像面板开着时，同一段回复也写进面板的对话区（面板没开就是空操作）。
         * 面板状态行不在这里改 —— 那是 voice_state 的事，保持"一个来源"。 */
        touch_ui_set_voice_reply(shown);

        /* 播放语音回复 */
        if (g_ai_initialized) {
            audio_play_start(&g_audio_ctx, NULL, 0, NULL, NULL);
        }
    }
    else if (strcmp(action, "user_said") == 0) {
        /* 框架侧 ai_companion 的 ASR 结果：用户刚说的那句原话。
         * 以前它只落在串口日志里，界面上根本看不到 —— 用户原话「又看不到回复又看
         * 不到自己说了什么」，就是这句话。让老人看见"板子听清了什么"，他才敢接着聊。
         *
         * 和 ai_reply 一样，param 是 ASR/云端来的原文，可能带 emoji 或 Markdown，
         * 显示前先清洗（只洗显示这份）。 */
        char shown[256] = "";

        /* 先弹面板、再写文字：两者都是 lv_async_call，投递顺序就是执行顺序。
         * 反过来先写后弹，这句识别文字会落在"面板还没建好"的空档里被丢掉。 */
        voice_mirror_autoshow();

        sanitize_for_display(param, shown, sizeof(shown));

        if (shown[0] == '\0') {
            /* 空串、或者整句都被清洗掉了（比如只订了 emoji）：没什么可显示的，
             * 只记一条日志，**不要**拿空串去刷对话区 —— 那会把上一句擦掉，
             * 看着像面板坏了。 */
            printf("[AI] user_said 清洗后为空，界面不动\n");
        } else {
            touch_ui_set_voice_user_text(shown);
        }

        /* 面板状态行和状态栏都不在这里改：那是 voice_state(listening/thinking) 的事，
         * 保持"一个来源"，免得两个动作抢同一行字。 */
    }
    else if (strcmp(action, "voice_state") == 0) {
        /* 框架侧 ai_companion（常开麦克风跑 VAD -> ASR -> 大模型 -> TTS）
         * 报来的实时状态：既更新镜像面板的状态行，也顺手把主界面那行语音状态小字
         * 和表情跟上，老人不开面板也能看见"它在听/在想/在说"。
         *
         * 四个取值 -> 界面（"状态栏"那一档 2026-09-14 已经删了，现在说的是主界面
         * 「AI 回复区」里那一行小字，见 robot_ui_set_status）：
         *   "listening" 听   面板「检测到声音」蓝 + 小字「在听…」蓝（顺手自动弹面板）
         *   "thinking"  想   面板「正在想…」橙 + 小字「在想…」橙 + 表情思考
         *   "speaking"  说   面板「正在说话…」绿 + 小字「在说…」绿（顺手自动弹面板）
         *   "idle"      空闲 面板「录制中」灰 + 小字「空闲」灰
         *
         * thinking 原来借的是 ROBOT_STATUS_LISTENING（那时小字还没显示，靠表情区分
         * 就够）；2026-09-16 加了 ROBOT_STATUS_THINKING 这一档，主界面才能把
         * "在听"和"在想"分开 —— 这正是用户要的那四档。表情仍然是 THINKING，
         * 一个字都没变。
         *
         * ⚠️ 本回调跑在 network_task（MQTT 收包）线程里：ui_post() 和
         * touch_ui_set_voice_state() 内部都是 lv_async_call，不能在这里直接碰控件
         * （跨线程改 LVGL 会撞 rendering_in_progress 断言把 app 打死）。
         * 面板没开着时 touch_ui_set_voice_state() 自己是空操作，不用先判断。 */
        if (strcmp(param, "speaking") == 0) {
            /* ⚠️ **不要**把 "listening" 放进来：hello_app 是开机自启的，它一启动就
             * 进入 listening 状态并推一条 listening —— 那样开机第一眼看到的就是语音
             * 面板，而不是主菜单（用户实测反馈："为什么一开机就是语音聊天页面？
             * 我希望看到主菜单"）。
             *
             * 现在的策略：只在"确实有语音活动"时才弹 ——
             *   - 用户说的话到了（user_said 分支里那处 autoshow）→ 弹出并显示"你说：…"
             *   - 机器人正在回话（speaking）→ 弹出并显示回复
             * "thinking" 也不必触发（识别一出来就会先走 user_said 那处）。
             * 抑制规则见 voice_mirror_autoshow() 上面的说明。 */
            voice_mirror_autoshow();
        }

        if (strcmp(param, "listening") == 0) {
            ui_post(ROBOT_STATUS_LISTENING, ROBOT_FACE_HAPPY, NULL);
            touch_ui_set_voice_state(TOUCH_VOICE_STATE_LISTENING);
        } else if (strcmp(param, "thinking") == 0) {
            /* 表情仍然是"思考"，只有主界面那行小字从原来的「在听…」换成「在想…」
             * （以前借 LISTENING 是因为那行字还没显示出来，借一下就够）。 */
            ui_post(ROBOT_STATUS_THINKING, ROBOT_FACE_THINKING, NULL);
            touch_ui_set_voice_state(TOUCH_VOICE_STATE_THINKING);
        } else if (strcmp(param, "speaking") == 0) {
            ui_post(ROBOT_STATUS_SPEAKING, ROBOT_FACE_HAPPY, NULL);
            touch_ui_set_voice_state(TOUCH_VOICE_STATE_SPEAKING);
        } else if (strcmp(param, "idle") == 0) {
            ui_post(ROBOT_STATUS_IDLE, ROBOT_FACE_HAPPY, NULL);
            touch_ui_set_voice_state(TOUCH_VOICE_STATE_IDLE);
        } else {
            /* 不认识的状态不改界面：宁可少刷一次，也不要把面板停在一个错的字上 */
            printf("[AI] voice_state 不认识的状态: %s\n", param);
        }
    }
    else if (strcmp(action, "diag") == 0 || strcmp(action, "voice_diag") == 0) {
        /* 诊断入口：从 PC 往 zhi_ai/<client_id>/command 发
         *   {"action":"diag"}        （voice_diag 是等价别名，两个都认）
         * 板子立刻回一条完整快照到 zhi_ai/<client_id>/status（QoS0、不 retain）：
         *   {"type":"diag","device_id":"zhi_ai_001","timestamp":...,"uptime":...,
         *    "robot":{"ai_init":1,"rec":0,"play":0,"panel":0,"tasks":27,
         *             "mqtt":true,"broker":"broker.emqx.io","mqtt_fails":0,
         *             "broker_switches":0},
         *    "hello":{"..."}}          <- hello_app 那一份，原样内嵌
         *    "hello":"unavailable"     <- hello_app 没在跑（这就是最有价值的证据）
         *
         * 为什么值一个动作：串口线丢了之后这是唯一一条"能问板子内部"的路。
         * hello_app 的语音链路哑掉时它自己那条 MQTT 上报是死的，光看心跳只能
         * 从 ha 位看出"它没了"，问 diag 才能把它自己报的状态整份拿出来。
         *
         * 组装和发布都在 network_comm 的 report_diag() 里（那边才看得见 broker、
         * 连续失败计数、任务数；hello_app 的快照也在那边取，心跳要用同一份判断）。
         * 本回调跑在 network_task 线程里，而 mqtt_publish 用的正是这个任务自己的
         * socket —— 同任务调用，不涉及跨任务抢 fd（见 network_comm.c 里
         * mqtt_publish 那段说明：跨任务 close/send 会把整机打复位）。 */
        int ret = report_diag();

        printf("[Diag] 收到诊断请求（action=%s）：报告%s发出 ret=%d\n",
               action, (ret < 0) ? "未能" : "已", ret);
    }
    else if (strcmp(action, "start_remind") == 0) {
        /* 开始提醒 */
        ui_post(ROBOT_STATUS_REMINDING, ROBOT_FACE_WORRIED, NULL);
        ui_post_reminder("提醒", param);
    }
    else if (strcmp(action, "start_alarm") == 0) {
        /* 开始报警 */
        ui_post_alarm(param);

        /* 触发报警 */
        if (g_ai_initialized) {
            sm_handle_event(&g_sm_ctx, SM_EVENT_ALARM_DETECTED);
            report_alarm("sound_abnormal", param);
        }
    }
    else if (strcmp(action, "stop_alarm") == 0) {
        /* 停止报警 */
        ui_post_close_alarm();

        if (g_ai_initialized) {
            sm_handle_event(&g_sm_ctx, SM_EVENT_ALARM_CLEARED);
        }
    }
    else if (strcmp(action, "fall_test") == 0) {
        /* ★ 摔倒链的**上板自测入口**（不动硬件、不烧录就能把整条链走一遍）：
         * 从 PC/手机往 zhi_ai/<client_id>/command 发
         *   {"action":"fall_test"}
         * 就等于"检测器报了疑似摔倒" —— 后面弹窗、语音询问、听回答、
         * 取消或正式报警全部自己跑。将来检测器接进来调的就是同一个函数。
         *
         * 本回调在 network_task 线程里，而 fall_alarm_trigger() 只做
         * 判重 + 起一条工作线程，立刻返回，不会把 MQTT 那条线程拖住。
         * 幂等也在那里：链条没跑完时再发一次只会多一行"忽略本次触发"日志。 */
        int fret = fall_alarm_trigger("mqtt_test");
        printf("[Fall] 自测触发（action=fall_test）ret=%d\n", fret);
    }
    else if (strcmp(action, "fall_answer") == 0) {
        /* ★ 摔倒链的自测入口 2：**代替用户回答**，用于不动屏幕就能验证
         * "取消 / 正式报警"两条分支。param 取：
         *   "no" / "没有" / "0"   -> 按「没有」处理（取消警报）
         *   "yes" / "有" / "1"    -> 按「有」处理（正式报警）
         * 真机演示时这条不用 —— 用户点弹窗按钮或直接说话即可。
         *
         * 只认这几个取值：别的（打错字、空参数）一律**忽略**并打日志。
         * 这里不能"非 no 即 yes" —— 一条参数写错的测试命令不该变成一次正式报警。 */
        int no   = (param != NULL) &&
                   (strcmp(param, "no") == 0 || strcmp(param, "没有") == 0 ||
                    strcmp(param, "0") == 0);
        int yes  = (param != NULL) &&
                   (strcmp(param, "yes") == 0 || strcmp(param, "有") == 0 ||
                    strcmp(param, "1") == 0);

        if (!no && !yes) {
            printf("[Fall] 自测回答参数不认识（param=%s），忽略"
                   "（只认 no/没有/0 和 yes/有/1）\n",
                   (param != NULL) ? param : "-");
        } else {
            printf("[Fall] 自测回答（action=fall_answer param=%s）-> %s\n",
                   param, no ? "没有" : "有");
            fall_set_answer(no ? 0 : 1, "MQTT 自测");
        }
    }
    else if (strcmp(action, "confirm_alarm") == 0) {
        /* ★ 询问页的**第二条确认路**（屏幕可能坏了/被拆下来，所以不能只靠屏幕）：
         * 从 PC/手机往 zhi_ai/<client_id>/command 发
         *   {"action":"confirm_alarm","confirm":true}   等价于点「是的，报警」
         *   {"action":"confirm_alarm","confirm":false}  等价于点「不用了」
         * 两种写法的归一（布尔 / 数字 / 字符串）在 on_mqtt_message_received() 里做，
         * 到这里 param 通常是 "yes" / "no" / ""（没给 confirm 字段）。下面这一组
         * 白名单是给"别的调用点直接调本函数"留的余地（同一条 chain 里也判一次），
         * **不是**"非 no 即 yes"：认不出来的参数一律忽略并打日志，一条写错参数的
         * 测试命令不该变成一次正式报警（和上面 fall_answer 同一条纪律）。
         *
         * 本回调跑在 network_task（MQTT 收包）线程里，所以回答也要投递到 LVGL 线程，
         * 和屏幕上那两个按钮走同一个收口（ask_finish）——"点按钮"和"手机确认"
         * 的结果逐字一致。 */
        if (strcmp(param, "yes") == 0 || strcmp(param, "true") == 0 ||
            strcmp(param, "1") == 0) {
            printf("[Ask] MQTT 下行确认：是的，报警\n");
            ui_post_ask_answer(1, "MQTT 下行");
        } else if (strcmp(param, "no") == 0 || strcmp(param, "false") == 0 ||
                   strcmp(param, "0") == 0) {
            printf("[Ask] MQTT 下行否认：不用了\n");
            ui_post_ask_answer(0, "MQTT 下行");
        } else {
            printf("[Ask] confirm_alarm 的 confirm 取值不认识（param=%s），忽略"
                   "（只认 true/false、1/0、\"yes\"/\"no\"）\n",
                   (param != NULL && param[0] != '\0') ? param : "-");
        }
    }
    else if (strcmp(action, "ask_test") == 0) {
        /* ★ 询问链的**上板自测入口**（和 fall_test 一个路子）：
         *   {"action":"ask_test"}                    -> 用默认原因弹询问页
         *   {"action":"ask_test","param":"玻璃碎声"}  -> 用 param 当原因
         * 走的就是检测器将来调的那**同一个** ui_post_ask_alarm()，链路一模一样。
         * 本回调在 network_task 线程里，而 ui_post_ask_alarm() 只记账 + 投递 + 起
         * 一条 4KB 栈的看门狗线程，立刻返回，不会把 MQTT 那条线程拖住。 */
        const char *why = (param != NULL && param[0] != '\0') ? param : "异常声响";

        printf("[Ask] 自测触发（action=ask_test，原因=%s）\n", why);
        ui_post_ask_alarm(why);
    }
    else if (strcmp(action, "light_on") == 0 || strcmp(action, "light_off") == 0) {
        /* 手动自检入口：不开语音、不接大模型也能把"说话 -> 发指令 -> 灯回执 ->
         * 界面显示"这条链路走一遍。从 PC 往 zhi_ai/<client_id>/command 发
         *   {"action":"light_on"}  /  {"action":"light_off"}
         * 板子就替你说出这句命令（send_device_command 发到 .../device_cmd，
         * 队友的智能灯模拟器收下后执行）。
         *
         * 这里只报"指令发出去没有"，**不写"客厅灯：已开"** —— 灯到底开没开
         * 以模拟器回来的 device_state 为准（见 on_device_state_received），
         * 先写死一个"已开"就是假信息。
         *
         * 本回调在 network_task 线程里，mqtt_publish 用的正是这个任务自己的
         * socket，同任务调用不涉及跨任务抢 fd（见 network_comm.c 的
         * mqtt_publish 注释：那个函数只许改标志、不许碰 socket）。 */
        bool want_on = (strcmp(action, "light_on") == 0);
        int ret = send_device_command("living_room_light", want_on ? "on" : "off");

        printf("[DeviceCmd] 自检：%s客厅灯，ret=%d\n", want_on ? "开" : "关", ret);

        if (ret < 0) {
            ui_post(-1, -1, want_on ? "客厅灯：开灯指令没发出去，检查网络"
                                    : "客厅灯：关灯指令没发出去，检查网络");
        } else {
            ui_post(-1, -1, want_on ? "客厅灯：开灯指令已发出，等灯回执..."
                                    : "客厅灯：关灯指令已发出，等灯回执...");
        }
    }
    else if (strcmp(action, "set_face") == 0) {
        /* 设置表情（-1 = 这次不改状态栏） */
        if (strcmp(param, "happy") == 0) {
            ui_post(-1, ROBOT_FACE_HAPPY, NULL);
        } else if (strcmp(param, "thinking") == 0) {
            ui_post(-1, ROBOT_FACE_THINKING, NULL);
        } else if (strcmp(param, "sleepy") == 0) {
            ui_post(-1, ROBOT_FACE_SLEEPY, NULL);
        } else if (strcmp(param, "surprised") == 0) {
            ui_post(-1, ROBOT_FACE_SURPRISED, NULL);
        } else if (strcmp(param, "worried") == 0) {
            ui_post(-1, ROBOT_FACE_WORRIED, NULL);
        }
    }
    else if (strcmp(action, "send_text") == 0) {
        /* 云端/MQTT 下发的纯文本对话：暂时只打印，不调大模型。
         *
         * 原来这里调 llm_send_text()（走 ai_agent 的 velaclaw 客户端），但那条路
         * 依赖 ai_agent 进程自己初始化过的消息总线队列，别的 app 调用时队列锁
         * 还是 .bss 的全 0，pthread_mutex_lock 直接撞 NXSEM_IS_MUTEX 断言把整个
         * app 打死（和语音聊天那次崩溃同一个根因）。
         * 这里也不能直接换 mimo_chat()：本回调跑在 network_task（MQTT 收包）
         * 线程里，mimo_chat() 是阻塞的 TLS + 云端推理（可能几十秒），会把
         * MQTT 心跳/重连一起冻住。要真支持就在这里起一个 detached 工作线程去跑
         * （做法同 voice_submit_handler -> voice_worker）。 */
        printf("[AI] send_text 动作暂不支持（原因见源码注释）: %s\n", param);
    }
}

/* ==================== AI 模块回调函数 ==================== */

/**
 * VAD 回调 - 语音活动检测
 */
static void vad_callback(bool speech_detected, void *user_data)
{
    if (speech_detected) {
        printf("[VAD] Speech detected\n");
        sm_handle_event(&g_sm_ctx, SM_EVENT_WAKEUP);
    } else {
        printf("[VAD] Speech ended\n");
        sm_handle_event(&g_sm_ctx, SM_EVENT_VOICE_COMPLETE);
    }
}

/**
 * 声音检测回调 - 异常声音
 *
 * ⚠️ 跑在声音检测线程里：界面动作必须投递（ui_post_alarm）
 */
static void sound_alarm_callback(sound_type_t type, float confidence, void *user_data)
{
    const char *type_name = sound_detect_get_type_name(type);
    printf("[SoundDetect] Alarm: %s (confidence: %.2f)\n", type_name, confidence);

    /* 触发报警（投递到 LVGL 线程建报警页） */
    ui_post_alarm(type_name);
    report_alarm_queued(type_name, "Abnormal sound detected");

    /* 通知状态机 */
    sm_handle_event(&g_sm_ctx, SM_EVENT_ALARM_DETECTED);
}

/**
 * 关怀确认按钮回调 - 用户点击"我没事"或"需要帮助"后触发
 * 由 UI 线程调用，通过 ai_checkin_respond 提交给状态机
 */
static void checkin_btn_callback(uint64_t checkin_id, bool needs_help, void *user_data)
{
    uint64_t now_ms = lv_tick_get();
    printf("[Checkin] Respond: id=%lu needs_help=%d\n",
           (unsigned long)checkin_id, needs_help);

#if 0 /* [缺文件临时隔离] ai_checkin_respond() */
    int ret = ai_checkin_respond(checkin_id, needs_help, now_ms);
    if (ret != 0) {
        printf("[Checkin] respond failed: %d\n", ret);
    }
#else
    printf("[Checkin] (ai_checkin 未提交，这里只打印)\n");
#endif
}

/* 用于 lv_async_call 的 show_checkin 参数 */
typedef struct {
    uint64_t checkin_id;
    uint32_t timeout_ms;
    checkin_btn_cb_t cb;
    void *user_data;
} show_checkin_arg_t;

/* lv_async_call 回调：在 LVGL 线程中显示 checkin 面板 */
static void show_checkin_async(void *arg_ptr)
{
    show_checkin_arg_t *arg = (show_checkin_arg_t *)arg_ptr;
    touch_ui_show_checkin(arg->checkin_id, arg->timeout_ms,
                          arg->cb, arg->user_data);
    free(arg);
}

/**
 * 主动关怀回调 - 从 care 模块线程调用
 * ⚠️ 这里**不是** LVGL 线程：界面动作一律 ui_post_*（内部 lv_async_call）。
 *    （原注释写着"robot_ui_* 内部已有线程安全机制"，其实没有 —— robot_ui.c 里
 *      这些函数都是直接 lv_label_set_text，跨线程调会踩 LVGL 断言把 app 打死。）
 */
static void care_remind_callback(care_type_t type, const char *message, void *user_data)
{
    const char *type_name = care_get_type_name(type);
    printf("[Care] Reminder: %s - %s\n", type_name, message);

    /* 显示提醒（投递到 LVGL 线程） */
    ui_post(ROBOT_STATUS_REMINDING, ROBOT_FACE_WORRIED, NULL);
    ui_post_reminder(type_name, message);

    /* 发送推送 */
    push_send_health_reminder(type_name, message);

    /* 启动关怀确认：分配 30 秒超时 */
#if 0 /* [缺文件临时隔离] ai_checkin_begin() + 确认面板投递 */
    uint64_t checkin_id = 0;
    uint64_t now_ms = lv_tick_get();
    if (ai_checkin_begin(now_ms, 30000, &checkin_id) == 0) {
        printf("[Checkin] Started: id=%lu\n", (unsigned long)checkin_id);

        /* 通过 lv_async_call 投递到 LVGL 线程显示 checkin 面板 */
        show_checkin_arg_t *arg = malloc(sizeof(show_checkin_arg_t));
        if (arg) {
            arg->checkin_id = checkin_id;
            arg->timeout_ms = 30000;
            arg->cb = checkin_btn_callback;
            arg->user_data = NULL;
            /* 这段在 #if 0 里（等队友补 ai_checkin.{c,h}），但照样走加锁的
             * 投递口：删掉 #if 0 复活它的时候就是对的，免得将来又漏一个。 */
            ui_async_call(show_checkin_async, arg);
        }
    } else {
        printf("[Checkin] Failed to start\n");
    }
#endif
}

/**
 * 语音聊天回调 - 由语音聊天弹窗（touch_ui_show_voice_chat / 「再说一次」）调用
 *
 * ⚠️ 本回调在 **LVGL 线程**里被调（touch_ui 的开启回调），所以这里只做两件事：
 * 给用户一个"已经在准备"的即时反馈，然后开一条工作线程去要麦克风。
 * 停掉别人的录音 + audio_record_start()（都要碰设备）都在那条线程里做，
 * 理由见 voice_open_thread() 头上的说明 —— 原来这两步就压在这个线程里，界面会僵，
 * 而且拿不到麦时是静默失败（用户报的"点了说话没反应"）。
 */
static void voice_chat_handler(void *user_data)
{
    uint32_t *token;
    pthread_attr_t attr;
    pthread_t tid;

    /* 音频上下文用全局的 g_audio_ctx（和录音回调、播放那条路同一份），
     * 不再从回调参数取 —— 工作线程里也读同一个全局，少一条传递路径。 */
    (void)user_data;

    printf("[VoiceChat] 用户点了语音聊天：开麦交给工作线程（停别人的录音 + 开自己这一路都在那边）\n");

    /* 即时反馈：状态栏/表情先亮起来，面板状态行稍后由工作线程改成
     * "正在录音…"或"麦克风正忙"。 */
    robot_ui_set_status(ROBOT_STATUS_LISTENING);
    robot_ui_set_face(ROBOT_FACE_THINKING);

    /* 新的一轮：丢掉上一轮没交出去的 PCM（缓冲本身留着复用，不反复 malloc） */
    pthread_mutex_lock(&g_voice_lock);
    g_voice_pcm_len = 0;
    g_voice_pcm_full = false;
    pthread_mutex_unlock(&g_voice_lock);

    if (!g_ai_initialized) {
        touch_ui_set_voice_status("AI 模块未就绪");
        touch_ui_voice_chat_round_done();
        return;
    }

    /* 上一条开麦线程还在准备中：不再起第二条（两条一起开麦必然自伤，
     * 见 voice_open_claim），这一轮沿用那一次 —— 它开成之后会把状态行刷成
     * "正在录音…"，和用户这次点的是同一件事。令牌不动，那一条继续有效。 */
    if (!voice_open_claim()) {
        printf("[VoiceChat] 上一次开麦还在准备中，沿用那一次\n");
        touch_ui_set_voice_status("正在准备麦克风…\n请稍等");
        return;
    }

    /* 这一轮的开麦令牌 +1：上一条开麦线程（如果有）就此作废，它会在动设备之前
     * 自己退出。用户关窗/提交/进菜单时也会 +1（见 voice_cancel_handler /
     * voice_submit_handler），保证"用户不要了这一轮"这件事一定传得到那条线程。 */
    g_voice_open_token++;

    touch_ui_set_voice_status("正在准备麦克风…\n请稍等");

    /* 令牌按值交给工作线程：用堆上一格，免得做"整数转指针"这种转换。
     * 线程负责 free；它起不来就在下面 free 掉。 */
    token = malloc(sizeof(*token));
    if (token == NULL) {
        printf("[VoiceChat] 开麦令牌分配失败\n");
        voice_open_finish();
        ui_post(ROBOT_STATUS_IDLE, ROBOT_FACE_HAPPY, "语音功能启动失败\n请重试");
        touch_ui_set_voice_status("语音功能启动失败\n请关掉重开");
        touch_ui_voice_chat_round_done();
        return;
    }
    *token = g_voice_open_token;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    /* 这条线程只做"停别人的录音 + 开自己这一路"：没有 TLS、没有 HTTPS，不需要 voice_worker
     * 那 32 KB（常驻的录音线程是 audio_record_start 内部自己起的那条）。 */
    pthread_attr_setstacksize(&attr, 16384);

    if (pthread_create(&tid, &attr, voice_open_thread, token) != 0) {
        pthread_attr_destroy(&attr);
        free(token);
        printf("[VoiceChat] 开麦线程起不来\n");
        voice_open_finish();
        ui_post(ROBOT_STATUS_IDLE, ROBOT_FACE_HAPPY, "语音功能启动失败\n请重试");
        touch_ui_set_voice_status("语音功能启动失败\n请关掉重开");
        touch_ui_voice_chat_round_done();
        return;
    }

    pthread_attr_destroy(&attr);
}

/**
 * 紧急呼叫回调 - 由触摸菜单「紧急联系家人」确认后触发（touch_ui.c 的按钮回调，
 * 所以本函数跑在 **LVGL 线程**：可以直接碰界面，但一个阻塞动作都不能做 ——
 * 不收尾录音、不做阻塞式 HTTPS）。
 *
 * 走的是"检测到异常声响 → 用户确认报警"那**同一条**收口 alarm_escalate()：
 * 报警页 + 警音 + 状态机 + MQTT + 手机 Bark 全在里面，本函数不另写报警动作。
 * 出门之前只是把菜单那条死路（原来只发一次 MQTT + 一次推送，报警页和警音都不响）
 * 接上这条真报警链路。
 *
 * new_event = true：老人按下这个按钮本身就是**一次全新事件**（和"检测到一声异常
 * 声响"同级），所以事件序号要 +1 —— 否则上一次异常留下的"已报警"记号会把这次
 * 呼叫判成"已经报过了"而挡掉。去重闸仍然生效：语音追问那条路如果已经报过警，
 * 这里就只记一行日志（报警页/铃声/上报都不重复做）。
 */
static void emergency_call_handler(void *user_data)
{
    (void)user_data;

    printf("[Emergency] 紧急呼叫已确认 → 走既有报警入口（报警页 + 响铃 + 上报）\n");

    if (!alarm_escalate("紧急呼叫", true, "emergency",
                        "紧急呼叫：正在联系家人")) {
        /* 没领到票：别路已经把这一次报过了，本路不再报第二遍（只记日志，见收口）。
         * 主界面的表情/文案也不改：报警页马上就要上来，改了也看不见。 */
        return;
    }

    /* 主界面的表情和回复区文案（报警页撤下之后老人还能看见这一句）。
     * 手机推送已经在收口里的 report_alarm_queued() 里发过了（一次就够），
     * 这里**不再**单独 push_send_alarm —— 那样家人的手机会收到两条一样的报警。 */
    robot_ui_set_face(ROBOT_FACE_WORRIED);
    robot_ui_set_ai_reply("已通知家人\n请保持镇静");
}

/* 设置里拖「音量」滑块：界面那层只存数字，真正下到音频硬件由这里做。
 * 说明：音量是音频设备的一个 feature unit，必须在**已经打开的播放设备**上
 * 生效；ai_audio 的 audio_set_volume() 内部自己 open/configure/close 一次
 * （playback 方向 → DAC 音量），所以这里直接调就行，不用关心当前在放什么。 */
static void volume_set_handler(int volume, void *user_data)
{
    int ret;

    (void)user_data;

    ret = audio_set_volume(&g_audio_ctx, (uint8_t)volume);
    if (ret != 0) {
        printf("[Settings] 音量下发失败: %d（值 %d）\n", ret, volume);
    } else {
        printf("[Settings] 音量已设为 %d\n", volume);
    }
}

/* ==================== 设备（灯）状态回执 ==================== */

/* topic 是不是「设备状态回执」（zhi_ai/<client_id>/device_state）。
 * 只比后缀：client_id 是配置出来的（换板子、换 key 都会变），把整条 topic
 * 写死会在换板子时静默失配 —— 那时表现是"界面永远不显示灯的状态"，很难查。 */
static bool is_device_state_topic(const char *topic)
{
    static const char suffix[] = "/device_state";
    size_t tlen;
    size_t slen;

    if (topic == NULL) {
        return false;
    }

    tlen = strlen(topic);
    slen = sizeof(suffix) - 1;

    return tlen >= slen && strcmp(topic + tlen - slen, suffix) == 0;
}

/* device_id -> 屏幕上给人看的名字。只认已知的这一盏灯，不认识的设备原样
 * 显示 id：队友后面接新设备时，宁可屏幕上出现一个英文 id，
 * 也不要猜错名字张冠李戴。 */
static const char *device_display_name(const char *device_id)
{
    if (strcmp(device_id, "living_room_light") == 0) {
        return "客厅灯";
    }

    return device_id;
}

/* 灯的执行回执：板子发指令（send_device_command）-> 队友的智能灯模拟器执行
 * -> 在 zhi_ai/<client_id>/device_state 上回一条，payload 形如
 *   {"type":"device_state","device_id":"living_room_light","state":"on",
 *    "success":true,"message":"command executed","timestamp":1757830000}
 *
 * 只把结果写进对话区（ui_post 内部是 lv_async_call）。**本函数跑在
 * network_task（MQTT 收包）线程里**：直接碰 LVGL 会踩
 * rendering_in_progress 断言把整个 app 打死（见本文件 ui_post 那一段注释）。
 *
 * 解析失败 / 字段缺失时只打串口日志、不动界面：一次回执刷不上屏是小事，
 * 把界面刷成一句错话（别人的 payload 显示成"客厅灯：已开"）才是大事。
 */
static void on_device_state_received(const char *payload)
{
    cJSON *root;
    cJSON *item;
    const char *device_id;
    const char *state;
    const char *message;
    const char *name;
    bool success;
    char shown[160];
    char clean[160];

    if (payload == NULL || payload[0] == '\0') {
        printf("[DeviceState] payload 为空，忽略\n");
        return;
    }

    root = cJSON_Parse(payload);
    if (root == NULL) {
        printf("[DeviceState] JSON 解析失败，忽略\n");
        return;
    }

    item = cJSON_GetObjectItem(root, "device_id");
    device_id = (cJSON_IsString(item) && item->valuestring != NULL)
                    ? item->valuestring : NULL;
    item = cJSON_GetObjectItem(root, "state");
    state = (cJSON_IsString(item) && item->valuestring != NULL)
                ? item->valuestring : NULL;
    item = cJSON_GetObjectItem(root, "message");
    message = (cJSON_IsString(item) && item->valuestring != NULL)
                  ? item->valuestring : "";

    /* success 缺失（没这个字段）按成功算：正常的回执一定带它，缺了多半是
     * 别的东西误发到这条 topic 上，这时凭空报一句"执行失败"更吓人。 */
    item = cJSON_GetObjectItem(root, "success");
    success = (item == NULL) ? true : !cJSON_IsFalse(item);

    if (device_id == NULL || state == NULL) {
        printf("[DeviceState] 缺 device_id/state，忽略: %.80s\n", payload);
        cJSON_Delete(root);
        return;
    }

    name = device_display_name(device_id);

    if (!success) {
        snprintf(shown, sizeof(shown), "%s：执行失败", name);
    } else if (strcmp(state, "on") == 0) {
        snprintf(shown, sizeof(shown), "%s：已开", name);
    } else if (strcmp(state, "off") == 0) {
        snprintf(shown, sizeof(shown), "%s：已关", name);
    } else {
        /* 不认识的 state 原样显示：不要替它猜成"已开/已关"，那是假信息 */
        snprintf(shown, sizeof(shown), "%s：%s", name, state);
    }

    printf("[DeviceState] %s state=%s success=%d message=%s\n",
           device_id, state, (int)success, message);

    /* 过一遍清洗：device_id/state 都是网络上来的字节，长度或编码意外时
     * 不能让它们把对话区刷成乱码（sanitize_for_display 只留字库里有字形的码点）。 */
    sanitize_for_display(shown, clean, sizeof(clean));
    ui_post(-1, -1, clean);     /* 只改对话区：状态栏/表情归 voice_state 管，不抢 */

    cJSON_Delete(root);
}

/* ==================== MQTT 消息回调处理 ==================== */

/* 从入站 JSON 里取 "confirm" 并归一成 "yes" / "no"（取不到或认不出来返回 NULL）。
 *
 * 为什么要这一步：询问页的确认用的是**布尔字段**
 *   {"action":"confirm_alarm","confirm":true}
 * 而 cJSON 里布尔项的 valuestring 是 NULL —— 下面按 "param" 取字符串的老写法
 * 永远只能拿到空串，这条确认路就是死的。归一成 "yes"/"no" 之后，分发函数
 * （on_ai_command_received）仍然只认 (action, param) 两个字符串，一行都不用改。
 *
 * 三种写法都认，手工用 mosquitto_pub 测试时方便：
 *   true/false（JSON 布尔）、1/0（数字）、"yes"/"no"/"true"/"false"（字符串）。
 * 认不出来 -> NULL，调用方按"参数不认识"忽略并打日志：一条写错参数的测试命令
 * 不该变成一次正式报警（和 fall_answer 那条纪律一样）。 */
static const char *confirm_field_to_yesno(const cJSON *root)
{
    const cJSON *c = cJSON_GetObjectItem(root, "confirm");

    if (c == NULL) {
        return NULL;
    }

    if (cJSON_IsBool(c)) {
        return cJSON_IsTrue(c) ? "yes" : "no";
    }

    if (cJSON_IsNumber(c)) {
        return (c->valuedouble != 0) ? "yes" : "no";
    }

    if (cJSON_IsString(c) && c->valuestring != NULL) {
        if (strcmp(c->valuestring, "yes") == 0 || strcmp(c->valuestring, "true") == 0 ||
            strcmp(c->valuestring, "1") == 0) {
            return "yes";
        }
        if (strcmp(c->valuestring, "no") == 0 || strcmp(c->valuestring, "false") == 0 ||
            strcmp(c->valuestring, "0") == 0) {
            return "no";
        }
    }

    return NULL;
}

/**
 * 处理来自 MQTT 的消息
 * 解析命令并转发给 AI 命令处理函数
 */
static void on_mqtt_message_received(const char *topic, const char *payload)
{
    printf("MQTT received: topic=%s\n", topic);

    /* 设备回执（.../device_state）的字段是 device_id/state/success，**没有 action**：
     * 按 topic 先分流再解析。不然它会被下面那条按 action 解析的分支当成"未知命令"，
     * 白白丢掉一次状态显示。 */
    if (is_device_state_topic(topic)) {
        on_device_state_received(payload);
        return;
    }

    /* 解析 JSON 命令 */
    cJSON *root = cJSON_Parse(payload);
    if (!root) {
        printf("JSON parse failed\n");
        return;
    }

    cJSON *action = cJSON_GetObjectItem(root, "action");
    cJSON *param = cJSON_GetObjectItem(root, "param");

    if (action && action->valuestring) {
        const char *param_str = (param && param->valuestring) ? param->valuestring : "";
        char confirm_buf[8] = "";

        /* confirm_alarm 的答案在 "confirm" 字段上（不是 param）：先归一成
         * "yes"/"no" 再往下走，分发函数那边一行都不用改。 */
        if (strcmp(action->valuestring, "confirm_alarm") == 0) {
            const char *yesno = confirm_field_to_yesno(root);

            if (yesno != NULL) {
                snprintf(confirm_buf, sizeof(confirm_buf), "%s", yesno);
                param_str = confirm_buf;
            }
        }

        on_ai_command_received(action->valuestring, param_str);
    }

    cJSON_Delete(root);
}

/* ==================== 定时提醒：到点响应 ==================== */
/*
 * 整条链路：
 *   用户在「提醒」里加/删 → touch_ui.c 调 reminder_sched_reload()
 *     → reminder_sched 把最近的一条挂到板级 rtc_alarm_at_daily()（日循环闹钟，
 *       硬件只有 1 个槽，多条提醒在软件层排队）
 *     → 到点：板级模块在**它的工作线程**里调 reminder_on_fire()
 *     → 这里只把参数拷一份、用 lv_async_call 投给 LVGL 线程，自己立刻返回
 *     → reminder_fire_async() 在 LVGL 线程里弹窗、起工作线程（**先响本地提示音**、
 *       响完再试云端合成补念一句）、推送到手机 —— 见下面 reminder_announce_thread
 *       头上那段"为什么顺序是这样"
 *     → reminder_sched 在同一轮回调里顺手把"下一条"挂上，如此往复。
 *
 * 为什么不能直接在 reminder_on_fire() 里弹窗：LVGL 不是线程安全的，
 * 而回调跑在 rtcalarm 工作线程里（docs/rtc_alarm_usage.md 第 3/7 节）。
 */

typedef struct {
    int   hour;
    int   min;
    char *titles;    /* 堆上的拷贝，reminder_fire_async() 负责 free */
} reminder_fire_msg_t;

/* 提醒提示音：本地生成的方波"嘀"，不依赖任何素材文件、也**不依赖网络**
 * （2026-09-14：这声铃原来是排在云端合成后面的，实测那次 TLS 拿了垃圾 -0x7200、
 * 40 秒后才失败，"到点"就成了"晚很久"。现在它是到点要做的第一件事）。
 *
 * 形态：两轮、每轮三声"嘀"（200 ms 声 + 150 ms 静音），两轮之间静音 1 秒，
 * 全长约 3.1 秒 —— 像闹钟那样"再来一次"，但**不是**不点不停的循环：无限循环会
 * 一直占着半双工设备，老人说话时 hello_app 的常开麦就听不见了。
 * 整段一次生成 = 49600 个采样（约 97 KB 堆，播完立刻 free）。为什么不做成"放完
 * 一轮、再放一轮"：那样中间要重新开一次播放设备（一次 open/START/STOP），反而更
 * 不像一声连续的闹钟。
 *
 * 只在 robot_ui 自己"没在录音、也没在放音"时响（那两种情况下抢设备会打断
 * 用户正在进行的对话）。这一条只管 robot_ui 自己的 g_audio_ctx。 */
#define REMINDER_BEEP_RATE     16000
#define REMINDER_BEEP_HZ       1000
#define REMINDER_BEEP_MS       200     /* 每声多长 */
#define REMINDER_BEEP_GAP_MS   150     /* 同一轮里声与声之间的静音 */
#define REMINDER_BEEP_TIMES    3       /* 每轮几声 */
#define REMINDER_BEEP_ROUNDS   2       /* 响几轮 */
#define REMINDER_BEEP_ROUND_GAP_MS 1000 /* 两轮之间的静音 */
#define REMINDER_BEEP_AMPLITUDE 5000   /* 约 15% 满量程，够听见又不刺耳 */

/* 没响成时的补放：这段声音再走一遍，最多补 REMINDER_BEEP_RETRY_TIMES 次。
 *
 * 为什么只补一次：用户的底线是"到点先来铃声"，而第一次没响成多是"设备刚被判
 * 占用"这种瞬时原因，隔 400 ms 再走一遍成功的概率不低。再往上补就变成"一直在响
 * 一个响不成的铃"了 —— 本函数跑在播报线程里，它后面还排着云端人声，补得越多，
 * "该…了"这句话来得越晚。
 * 为什么是 400 ms：给设备把上一次会话收干净留时间（起播本身会停录，正常几毫秒
 * 到几百毫秒），也不至于让"到点"整体晚太多。
 * 上限：补放本身最坏 = 声音 3.1 秒，加第一次一共约 6.2 秒 —— 与后面那次云端
 * TTS（几百 ms 到几十秒）同量级，不会把链路拖死。 */
#define REMINDER_BEEP_RETRY_TIMES 1
#define REMINDER_BEEP_RETRY_GAP_MS 400

/* 播放完成回调：只置一个标志，别的什么都不做（在 ai_audio 的播放线程里被调）。
 * ai_audio 的约定：被 audio_play_stop() 打断的播放**不**回调 —— 所以它置位
 * 意味着"播放线程收尾了"。注意它**不代表有声音**：设备打不开 / START 被
 * -EBUSY 拒掉 / write 失败时照样回调（好让上层状态机不等一个不来的事件）。
 * "到底出没出声"由 reminder_play_exclusive() 事后读 audio_play_last_result()
 * 判定 —— 原来少了这一步，于是"一声没响"被判成了"放完了"。 */
static void reminder_play_done(void *user_data)
{
    *(bool *)user_data = true;
}

/**
 * @brief  把播放层的错误码翻成一句人话（串口上直接看出"为什么没响"）
 *
 * 码都来自 ai_audio.c 的播放线程（写在 ctx->play_last_result 里，由
 * audio_play_last_result() 读出来），以及 audio_play_start() 自己的返回值：
 *   -EBUSY      START 被驱动拒掉（半双工，另一方向的会话正占着通路）—— 一声都没出
 *   -EIO        write() 返回 <= 0 / 没写满就退出 —— 只出了一部分或一声没出
 *   -ENOSPC     这段数据超过播放缓冲（AUDIO_PLAY_BUFFER_MS），播放层根本没接
 *   -ECANCELED  中途被 audio_play_stop() 停了
 *   -EINPROGRESS 播放线程还没给出结果（读得太早）
 * 只用于日志，不参与任何判断。
 */
static const char *reminder_play_reason(int rc)
{
    switch (rc) {
    case 0:             return "放完了";
    case -EBUSY:        return "设备被另一方向的会话占着，START 被驱动拒掉，一声都没出";
    case -EIO:          return "写设备失败，只出了一部分或一声没出";
    case -ENOSPC:       return "这段声音超过播放缓冲，播放层没接";
    case -EINVAL:       return "播放层参数非法";
    case -ENODEV:       return "音频设备节点打不开";
    case -ENOENT:       return "音频设备节点不存在";
    case -ECANCELED:    return "播放中途被停掉了";
    case -EINPROGRESS:  return "播放线程还没给出结果";
    default:            return "播放层报错";
    }
}

/**
 * @brief  放一段 PCM 并等它真的放完（半双工由 audio_play_start 内部处理）
 *
 * @param  ctx     robot_ui 自己的音频上下文
 * @param  data    PCM（16k/单声道/s16le）
 * @param  frames  帧数
 * @param  who     日志里的名字（"提示音" / "语音播报"）
 * @return 0  = 这次**真的出声了**（整段写完，播放层 audio_play_last_result() 报 0）；
 *         负值 = 没响成，负值本身就是原因（audio_play_start() 的错误码，或者播放层
 *                记下的失败码，例如 -EBUSY = START 被驱动拒掉、-EIO = 写设备失败、
 *                -ETIMEDOUT = 等整段放完超时）。
 *         起播成功**不等于**出声 —— audio_play_start() 只是 memcpy +
 *         pthread_create，真正 open/START 在播放线程里，设备打不开或 START 被
 *         -EBUSY 拒掉时它照样返回 0。调用方（reminder_play_beep / 播报那次）就靠
 *         这个返回值判"这声铃到底响没响"，别拿"返回 0"当"放完了"。
 *
 * 半双工（本板只有一条通路，录和放必须串行）：
 *   audio_play_start() 内部就会先停录音、播完按原配置恢复 —— 见 ai_audio.c 的
 *   audio_prepare_output() / audio_resume_record()。所以这里**不需要**先跟谁申请，
 *   直接出声就行。
 *
 * ★ 2026-09-21：这套包装原来是"独占音频"的（先请 hello_app 的常开麦让路、
 *   轮询等它让开、放完再还）。那套跨 app 的让路握手**整套删掉了**（用户拍板：
 *   「关闭让路…不需要做麦克风转让，因为我们的麦是常开的」；真机故障形状见
 *   app/hello_app/ai_companion_req.h 头上）。这里保留包装的另一个理由始终成立：
 *   **等整段真的放完**并把结果如实报回去。
 *
 * 为什么必须等"真的放完"才返回：
 *   audio_play_start() 只是把数据 memcpy 进播放缓冲、起播放线程就返回，真正出声
 *   在它自己的线程里。调用方（reminder_play_beep 的补放、播报后的收尾）需要知道
 *   "这一段到底出没出声"，而只有播放完成回调置位之后 audio_play_last_result()
 *   才是最终值。
 *
 * ⚠️ 整段必须待在**工作线程**里，绝不能在 LVGL 线程上调：
 *   等整段播完（播报要念两遍，十几秒）会把界面冻住 —— 本文件已经因为"在 LVGL
 *   线程里等设备"把界面冻过。两个调用点都在同一条工作线程
 *   reminder_announce_thread() 里（提示音、人声各一次，串行）：
 *     - reminder_play_beep()：到点后第一件事，先响本地提示音；
 *     - 播报那次：TTS 合成 + 念两遍。
 *   reminder_fire_async()（LVGL 线程）只负责把这条线程起起来，自己不调这个函数。
 *   这里再挡一道 up_interrupt_context()：万一以后有人在中断里调进来，宁可
 *   这次不出声，也不能在中断上下文里等。
 *
 * 日志判据（串口上直接可读）：
 *   成功  [Reminder] 提示音：放完了（真出声）
 *   没响成 [Reminder] 提示音：没响成（设备被另一方向的会话占着，START 被驱动拒掉，
 *                  一声都没出，rc=-16）
 *   绝不允许再出现"没响成、日志却说放完了"。
 */
static int reminder_play_exclusive(audio_context_t *ctx,
                                   const int16_t *data, size_t frames,
                                   const char *who)
{
    bool done = false;
    long rate_hz;
    long wait_ms;
    const char *why = NULL;     /* 非 NULL 时用它替代 reminder_play_reason() 的说法 */
    int rc;                     /* 0 = 真出声；负值 = 没响成的原因 */
    int ret;

    if (ctx == NULL || data == NULL || frames == 0) {
        return -EINVAL;
    }

    if (up_interrupt_context()) {
        printf("[Reminder] %s：在中断上下文里，不播\n", who);
        return -EBUSY;
    }

    printf("[Reminder] %s：直接出声（半双工由 audio_play_start 内部处理）\n", who);

    ret = audio_play_start(ctx, data, frames, reminder_play_done, &done);

    if (ret < 0) {
        /* 起播就没成（参数非法 / 装不下 / 已经在播）：播放层根本没跑，结果就是它报的码 */
        rc = ret;
    } else {
        /* 时长按**实际配置的采样率**折算（而不是写死 16000）：播报和提示音
         * 两条路的 ctx 配置不保证一样，写死会算错等待上限。再加 3 秒余量
         * 兜住 DMA/调度抖动；真超时说明播放线程卡住了，那也得如实报"没放完" ——
         * 宁可这次声音不完整，也不能让调用方以为它放完了。 */
        rate_hz = (long)ctx->config.sample_rate;
        if (rate_hz <= 0) {
            rate_hz = REMINDER_BEEP_RATE;   /* ctx 配置异常时按本板固定采样率兜底 */
        }

        wait_ms = (long)(frames * 1000 / (size_t)rate_hz) + 3000;

        while (!done && wait_ms > 0) {
            usleep(20000);                  /* 20 ms 一探：够细，也不占 CPU */
            wait_ms -= 20;
        }

        /* 回调被调用（done）之后读播放层的结果才是最终值：播放线程先把
         * play_last_result 写定，再调 reminder_play_done()。
         * done 为真但结果是负的 = "播放线程收尾了、可一声没出"（设备打不开 /
         * START 被 -EBUSY 拒掉 / 写失败）—— 这正是原来被打成"放完了"的那种情况。 */
        rc = audio_play_last_result(ctx);

        if (!done) {
            if (audio_is_playing(ctx)) {
                rc = -ETIMEDOUT;
                why = "等播放超时，播放线程卡住了（这段没放完）";
            } else if (rc == 0) {
                /* 线程都没了、结果却说放完了：自相矛盾，不能报成功 */
                rc = -EIO;
                why = "播放线程提前退出，没等到完成回调";
            } else {
                why = "播放被提前打断";
            }
        }
    }

    if (rc == 0) {
        printf("[Reminder] %s：放完了（真出声）\n", who);
    } else {
        printf("[Reminder] %s：没响成（%s，rc=%d）\n",
               who, why ? why : reminder_play_reason(rc), rc);
    }

    return rc;
}

static void reminder_play_beep(audio_context_t *ctx)
{
    const size_t half  = (size_t)(REMINDER_BEEP_RATE / (REMINDER_BEEP_HZ * 2));
    const size_t unit  = (size_t)(REMINDER_BEEP_RATE * REMINDER_BEEP_MS / 1000);
    const size_t gap   = (size_t)(REMINDER_BEEP_RATE * REMINDER_BEEP_GAP_MS / 1000);
    const size_t round_len = (unit + gap) * REMINDER_BEEP_TIMES;      /* 一轮（含轮内静音） */
    const size_t round_gap = (size_t)REMINDER_BEEP_RATE * REMINDER_BEEP_ROUND_GAP_MS / 1000;
    /* 最后一轮后面不再挂静音：那只会白占设备一秒才还麦克风 */
    const size_t total = round_len * REMINDER_BEEP_ROUNDS +
                         round_gap * (REMINDER_BEEP_ROUNDS - 1);
    int16_t *buf;
    int attempt;
    int rc = 0;

    if (ctx == NULL || half == 0) {
        return;
    }

    if (audio_is_recording(ctx) || audio_is_playing(ctx)) {
        printf("[Reminder] 正在录音/放音，这次只弹窗不出提示音\n");
        return;
    }

    buf = malloc(total * sizeof(int16_t));
    if (buf == NULL) {
        printf("[Reminder] 提示音缓冲分配失败\n");
        return;
    }

    for (size_t i = 0; i < total; i++) {
        size_t in_round = i % (round_len + round_gap);   /* 换算到"当前这一轮"里的位置 */
        size_t t = in_round % (unit + gap);

        /* in_round >= round_len 的就是两轮之间的那段静音 */
        if (in_round < round_len && t < unit) {
            buf[i] = ((i / half) & 1) ? REMINDER_BEEP_AMPLITUDE
                                      : -REMINDER_BEEP_AMPLITUDE;
        } else {
            buf[i] = 0;
        }
    }

    /* 走 reminder_play_exclusive()：直接出声（半双工由 audio_play_start 内部
     * 处理），等这段放完才返回 —— 理由见那个函数头上。它返回时数据早就拷完了，
     * 所以这里照样能立刻 free。
     * 本函数是"到点"时第一条要出声的路径（见 reminder_announce_thread），所以它
     * 全程不等网络：最坏就是这段声音本身。
     *
     * ⚠️ 返回值是"到底有没有真出声"（不是"起播成没成"）：非 0 就补一次。
     * 用户要的是"到点先听见铃声"，而第一次没响成多半是瞬时原因（设备刚被判
     * 占用），隔 REMINDER_BEEP_RETRY_GAP_MS 再走一遍值得试 —— 这就是那条
     * **一次性有界重试**（只补一次，别把后面那句云端人声一直往后推）。 */

    for (attempt = 0; attempt <= REMINDER_BEEP_RETRY_TIMES; attempt++) {
        if (attempt > 0) {
            printf("[Reminder] 提示音：上一遍没响成，隔 %d ms 补一次\n",
                   REMINDER_BEEP_RETRY_GAP_MS);
            usleep(REMINDER_BEEP_RETRY_GAP_MS * 1000);
        }

        rc = reminder_play_exclusive(ctx, buf, total,
                                     (attempt == 0) ? "提示音" : "提示音(补)");
        if (rc == 0) {
            break;
        }
    }

    free(buf);

    if (rc != 0) {
        /* 上面那几行已经把"为什么没响"（播放层错误码）打在串口上了，
         * 这里只留一句结论，方便一眼看到这次到点是真的没用铃声。 */
        printf("[Reminder] 提示音：补放一次也没响成（%s，rc=%d），这次到点没有铃声\n",
               reminder_play_reason(rc), rc);
    }
}

/* 到点（LVGL 线程）：弹窗 + 起工作线程（先响铃，再试云端播报） + 推到手机 */
/* ============ 提醒到点：本地提示音优先 + AI 语音播报（音量渐进 + 念两遍） ============ */
/*
 * 这里要同时满足用户的两句话：
 *   ①「提示音做成本地时钟直接触发吧，像闹钟那样」→ 到点必须**立刻**响，不许等网络。
 *     2026-09-14 之前是先 voice_tts_speak() 才退回提示音，实测那次 TLS 握手拿了
 *     垃圾 -0x7200、**40 秒后**才失败 —— 老人最需要的那声"叮"排在一次可能拖 40 秒的
 *     云端请求后面，网络不好时提醒就成了"晚了很久"。所以本线程第一步就是本地提示音
 *     （reminder_play_beep 纯本地生成方波，一个字节网络都不需要）。
 *   ② 云端合成的那句人声是"能合成就补一句"，它成不成功不再决定这声铃响不响。
 *
 * 为什么整段要单开线程（这条是底线，不是风格）：voice_tts_speak() 是阻塞的 HTTPS
 * （TLS + 云端合成，几百 ms 到几十秒），而 reminder_fire_async() 跑在 LVGL 线程里 ——
 * 直接调会把界面冻住；出声这一步还要等整段放完，同样只能待在工作线程里
 * （见 reminder_play_exclusive 头上那段约束）。LVGL 线程那边只做两件不阻塞的事：
 * 弹窗、pthread_create。
 *
 * 播报做成什么样：
 *   1. 文本用「该<标题>了」（标题就是提醒列表里那条，例如 吃药 → "该吃药了"）；
 *   2. 合成出来的 PCM **复制两份首尾拼起来**，一次播放里念两遍（省掉二次调度）；
 *   3. 整段乘一条**软件增益斜坡**：第 1 遍 30% → 100%，第 2 遍保持 100%。
 *      为什么用软件增益而不是中途改硬件音量：播放中改 DAC 音量要走"另开 fd 的
 *      ioctl"（ai_audio 的 audio_set_volume），在半双工设备上会打断正在播的这段，
 *      反而更突兀；乘系数是最稳的。
 *   4. 合成失败 / 超时 / 没网 → **什么都不补**，只留已经响过的那声铃，串口一行 INFO
 *      说明这次没念出来（不刷 ERROR：提醒本身已经送到了）。
 *   5. 正在录音（用户在跟机器人说话）时根本不进这里 —— 由调用方拦掉，
 *      否则会把麦克风抢走。
 *   6. 出声走 reminder_play_exclusive()：直接 audio_play_start()，等整段放完返回
 *      （半双工由它内部处理：起播先停录、播完按原配置恢复）。它的返回值 =
 *      "这段到底有没有真出声"，没出声时下面按失败收工。
 *      提示音和人声是**两次**独立的出声，各走一次完整的「起播 → 放完」。
 */

#define REMINDER_TTS_BUF_BYTES  (16000 * 2 * 6)   /* 6 秒，够念一句话 */
#define REMINDER_RATE_HZ        16000
#define REMINDER_GAIN_START     30                /* 起始增益（百分比） */
#define REMINDER_FADE_MS        1800              /* 从起始增益升到 100% 用多久 */

static void reminder_play_beep(audio_context_t *ctx);

/* 到点后的工作线程：① 本地提示音（不等网络）→ ② 云端合成 → ③ 念那句话。
 * 选"复用这一条线程"而不是再起一条"先响铃"的小线程，是为了让出声有**严格顺序**：
 * 两条线程各自调 reminder_play_exclusive() 会互相抢半双工设备，人声可能插到提示音
 * 前面；一条线程串行做，时序可预测，也让"什么时候还麦克风"只有一个地方要想。 */
static void *reminder_announce_thread(void *arg)
{
    char *title = (char *)arg;
    char text[96];
    unsigned char *pcm;
    int16_t *twice;
    size_t pcm_len = 0;
    size_t frames;
    size_t fade_frames;
    size_t i;
    int ret;

    /* 标题最长 20 字（提醒模块限制），"该…了"再占两个字，留足余量 */
    snprintf(text, sizeof(text), "该%s了", title);

    /* ===== ① 先响本地提示音：到点就能听见，**不等网络** =====
     * 这一行日志是验收判据：它必须出现在任何 TLS / 合成日志之前。
     * 最坏阻塞 = 这段声音约 3.1 秒；万一没响成还会再补放一遍（只补一次，见
     * REMINDER_BEEP_RETRY_TIMES）。等整段放完在 reminder_play_exclusive() 里，
     * 单出口，见它的说明。
     * 没响成的话（正在录音/放音、缓冲分配失败、设备被占）它会自己打一串说明，
     * 这里不需要补什么 —— 但**不会**再出现"没响成却打了放完了"。 */
    printf("[Reminder] 响铃：先放本地提示音（不等云端，网络失败也不影响它）\n");
    reminder_play_beep(&g_audio_ctx);

    /* ===== ② 再谈"补一句"：合成要过 TLS，几百 ms 到几十秒都可能 =====
     * 走到这里铃已经响过了，所以下面每条失败路径都只留一行 INFO 收工。
     *
     * ★ 先查预缓存（2026-09-19 加）：到点提醒的句子就是「该<标题>了」，开机默认装的
     *   那三条（吃药/喝水/散步）的 PCM 已经随固件打进 ROMFS 了（app/hello_app/
     *   tts_cache.c 的查表规则）。命中就 open + read + 直接用，**一个字节的网络都不走**，
     *   于是"网络慢/断 → 这句念不出来"这件事在默认提醒上直接消失。
     *   用户现场用语音加的标题没法预生成，照旧落回下面那次 live TTS。 */
    if (tts_cache_load(text, &pcm, &pcm_len) == 0) {
        printf("[Reminder] 命中预缓存: %zu 字节（约 %zu ms），不走网络\n",
               pcm_len, pcm_len * 1000 / (REMINDER_RATE_HZ * 2));
    } else {
        pcm = malloc(REMINDER_TTS_BUF_BYTES);
        if (pcm == NULL) {
            printf("[Reminder] 这次没念出来（合成缓冲分配失败），只响铃\n");
            free(title);
            return NULL;
        }

        ret = voice_tts_speak(text, pcm, REMINDER_TTS_BUF_BYTES, &pcm_len);
        if (ret < 0 || pcm_len == 0) {
            printf("[Reminder] 这次没念出来（合成失败 %d），只响铃\n", ret);
            free(pcm);
            free(title);
            return NULL;
        }
    }

    /* 念两遍：同一段 PCM 拼两份 */
    twice = malloc(pcm_len * 2);
    if (twice == NULL) {
        printf("[Reminder] 这次没念出来（拼接缓冲分配失败），只响铃\n");
        free(pcm);
        free(title);
        return NULL;
    }

    memcpy(twice, pcm, pcm_len);
    memcpy((char *)twice + pcm_len, pcm, pcm_len);
    free(pcm);

    frames = (pcm_len * 2) / sizeof(int16_t);

    /* 增益斜坡：第 1 遍从 REMINDER_GAIN_START% 线性升到 100%，第 2 遍原样 */
    fade_frames = (size_t)((long)REMINDER_RATE_HZ * REMINDER_FADE_MS / 1000);
    if (fade_frames > frames) {
        fade_frames = frames;
    }

    for (i = 0; i < fade_frames; i++) {
        int32_t g = REMINDER_GAIN_START +
                    (100 - REMINDER_GAIN_START) * (int32_t)i / (int32_t)fade_frames;

        twice[i] = (int16_t)((int32_t)twice[i] * g / 100);
    }

    printf("[Reminder] 语音播报: \"%s\"（%u ms ×2，音量 %d%%→100%%）\n",
           text, (unsigned)(pcm_len * 1000 / (REMINDER_RATE_HZ * 2)),
           REMINDER_GAIN_START);

    /* ===== ③ 念这句话（能合成才走到这里；合成失败上面已经收工了） =====
     * 只看 robot_ui 自己"现在没在录音"（用户在跟机器人说话，插进去会把录音
     * 打断）。本调用会一直等到整段念完（十几秒）才返回，但本函数本来就跑在
     * 单独的工作线程里（reminder_start_announce 起的），界面不受影响。
     * 返回值 = "这段有没有真出声"（不是"起播成没成"，见 reminder_play_exclusive
     * 的 @return），所以这里的判据连"起播成功但一声没出"也一起抓住了。
     * 失败也**不再**退回去响铃：铃在①里已经响过了，补响一遍等于同一件事响两次，
     * 还会白占设备 —— 一行 INFO 说清楚就够了（具体原因上面那行已经打过）。 */
    if (audio_is_recording(&g_audio_ctx)) {
        printf("[Reminder] 正在录音，这句人声不抢麦克风（提示音已响过）\n");
    } else if (reminder_play_exclusive(&g_audio_ctx, twice, frames,
                                       "语音播报") < 0) {
        printf("[Reminder] 这句人声没放出来（提示音已响过，不再补）\n");
    }

    free(twice);
    free(title);
    return NULL;
}

/* 起一个 detached 线程去"先响铃、再合成播报"（调用方在 LVGL 线程里，只做
 * pthread_create，不阻塞）。栈给 32 KB 是因为这一步里要跑 voice_tts_speak()
 * 的 TLS 握手，跟"先响铃"无关 —— 响铃只用几百字节和几 KB 堆。 */
static void reminder_start_announce(const char *title)
{
    pthread_attr_t attr;
    pthread_t tid;
    char *copy;

    if (title == NULL || title[0] == '\0') {
        return;
    }

    copy = ui_strdup(title);
    if (copy == NULL) {
        return;
    }

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 32768);   /* TTS 要跑完整 TLS 握手（getaddrinfo + x509
                                                * 解析都在这一层），16 KB 实测太紧：
                                                * 同一份代码在别处的 TTS 线程给的就是 32 KB。
                                                * 栈小到刚好"跑得动但压着边界"时，
                                                * 症状是 TLS 随机报 -0x7200（读到垃圾记录），
                                                * 很难查——所以这里给足。 */

    if (pthread_create(&tid, &attr, reminder_announce_thread, copy) != 0) {
        printf("[Reminder] 播报线程起不来\n");
        free(copy);
    }

    pthread_attr_destroy(&attr);
}

static void reminder_fire_async(void *arg)
{
    reminder_fire_msg_t *msg = (reminder_fire_msg_t *)arg;
    char content[160];

    /* 弹窗和语音说同一句话（老人听到的和看到的对得上）：
     *   标题「吃药」+ 08:00 → "该吃药了  08:00" */
    snprintf(content, sizeof(content), "该%s了  %02d:%02d",
             msg->titles, msg->hour, msg->min);

    printf("[Reminder] 到点提醒: %s\n", content);

    /* 弹窗（robot_ui_show_reminder 是现成的提醒弹窗，顶部图层，盖住整屏） */
    robot_ui_show_reminder("提醒", content);

    /* 到点后出声的全部活都交给下面这条工作线程：**第一步先响本地提示音**（不等
     * 网络），响完再试云端合成、成功才补念那句话（顺序和理由见
     * reminder_announce_thread 头上）。这里只 pthread_create，不做任何阻塞操作 ——
     * 没响成时补放一次、以及等整段放完，全都在那条线程里，绝不在 LVGL 线程里等。
     * 正在录音时不进这条线程 —— 用户正在跟机器人说话，插进去会把录音打断，这次
     * 就只留弹窗+推送（g_ai_initialized 同时是"音频已经初始化好"的判据，
     * 没它就连 audio_play_start 都用不了）。 */
    if (g_ai_initialized) {
        if (audio_is_recording(&g_audio_ctx)) {
            printf("[Reminder] 正在录音，只弹窗不响铃不播报\n");
        } else {
            reminder_start_announce(msg->titles);
        }
    }

    /* 手机端也收到一条：push_send_* 只是把请求丢进队列，由推送任务去发，
     * 不会卡住界面（见 network_comm.c 的 push_task）。没配推送 key 就
     * 打印一行"push not enabled"直接返回。 */
    if (push_send_health_reminder("提醒", content) < 0) {
        printf("[Reminder] 手机推送没发出去（推送没开或没配 key）\n");
    }

    free(msg->titles);
    free(msg);
}

/* 到点（RTC 模块工作线程）：只做拷贝 + 投递，别阻塞、别碰 LVGL */
static void reminder_on_fire(int hour, int min, const char *titles, void *arg)
{
    reminder_fire_msg_t *msg;
    char *copy;

    (void)arg;

    msg  = malloc(sizeof(reminder_fire_msg_t));
    copy = malloc(strlen(titles) + 1);
    if (msg == NULL || copy == NULL) {
        free(msg);
        free(copy);
        printf("[Reminder] 到点但内存不够，这次不弹了\n");
        return;
    }

    strcpy(copy, titles);
    msg->hour = hour;
    msg->min = min;
    msg->titles = copy;

    if (ui_async_call(reminder_fire_async, msg) != LV_RESULT_OK) {
        free(copy);
        free(msg);
    }
}

/* ==================== 语音建提醒：mimo_voice 的落地回调 ==================== */
/*
 * 语音说"八点提醒我吃药"时，模型会调 add_reminder 工具，mimo_voice.c 把
 * "HH:MM + 标题"交给这里落地（接口见 app/hello_app/mimo_voice.h 的
 * mimo_set_reminder_hook）。两边分属不同的 app，用一个函数指针解耦：
 * 提醒列表（reminder_sched）是 robot_ui 的，工具是 hello_app 的。
 *
 * 这个回调跑在**语音工作线程**里，不能碰 LVGL 控件 —— 界面刷新交给
 * touch_ui_notify_reminders_changed()（内部 lv_async_call 投到 LVGL 线程）。
 *
 * 时间格式 mimo_voice.c 已经校验并规范化成两位数了，这里再解一遍纯属防守。
 * 返回 0 = 建好了；负 errno 会被 mimo_voice 当作工具结果讲给用户听。
 */
static int voice_add_reminder(const char *hhmm, const char *title)
{
    int hour = 0;
    int min  = 0;
    int ret;

    if (hhmm == NULL || title == NULL || title[0] == '\0') {
        return -EINVAL;
    }

    if (sscanf(hhmm, "%d:%d", &hour, &min) != 2 ||
        hour < 0 || hour > 23 || min < 0 || min > 59) {
        printf("[Reminder] 语音建提醒：时间格式不对 %s\n", hhmm);
        return -EINVAL;
    }

    ret = reminder_sched_add(title, hour, min);
    if (ret < 0) {
        printf("[Reminder] 语音建提醒失败: %d（列表最多 %d 条）\n",
               ret, REMINDER_MAX_ITEMS);
        return ret;
    }

    /* 新加的这条可能比原来那条更近，重挂一次 RTC 闹钟（和界面新增同一条路） */
    reminder_sched_reload();

    printf("[Reminder] 语音新增: %s %02d:%02d (index=%d)\n",
           title, hour, min, ret);

    /* 用户要是正停在提醒列表上，就地重画（走 lv_async_call，本线程不碰 LVGL） */
    touch_ui_notify_reminders_changed();

    return 0;
}

/* ==================== 疑似摔倒事件链 ==================== */
/*
 * 用户要的时序（原话整理）：
 *   检测到异常 -> 判断疑似摔倒
 *     -> ① 手机收到「疑似摔倒」               （复用现成的 MQTT + BARK 通路）
 *     -> ② 板子弹窗「您摔到了吗？」有 / 没有   （touch_ui 的摔倒询问面板）
 *     -> ③ 同时语音问「您摔到了吗？」          （复用语音聊天那条 TTS 播放通路）
 *     -> ④ 等回答：点按钮 或 说话回答（先到的算数）
 *     -> ⑤ 答「没有」：撤弹窗 + 停响铃 + 手机收到「已取消」
 *         答「有」/ 超时没回应：手机收到「机主摔倒！」+ 板子响铃 + 红色报警页
 *
 * 唯一入口：fall_alarm_trigger(const char *source)（声明在 fall_alarm.h）。
 * **本文件不做摔倒判定**——判定方式还没定（本地模型 / 启发式 / 云端，由队友给），
 * 检测器将来不论在哪一侧，报一次就行；重复报在链条没跑完时会被忽略（幂等）。
 *
 * 为什么整条链在一条**工作线程**里跑（而不是在触发它的那个线程、或 LVGL 线程里）：
 *   - TTS 合成（云端 HTTPS）几百 ms 到几秒、ASR 识别同样、等回答最长 25 秒 ——
 *     这些全在一个函数里串起来，压在触发者（可能是
 *     MQTT 收包线程、将来是检测线程）或 LVGL 线程上都不可接受；
 *   - 界面动作一个都不在这里直接做：全部走 touch_ui_* / ui_post_*，它们内部是
 *     lv_async_call（见 ui_async.h）。
 *
 * 麦克风（半双工，这一步最容易漏）：
 *   本板音频**半双工**、驱动状态整机一份。这一节自己拿 g_audio_ctx 开录音：
 *   开之前先把"别的路径可能占着的录音"停掉（audio_record_stop + 常态听音
 *   pause），听回答那段结束之后（fall_listen_close）再放常态听音回来。
 *   ★ 2026-09-21：原来这里还要"请 hello_app 让开麦克风"（ai_companion_audio_yield
 *     + 轮询 + 一个 fall_mic_release()）—— 那套让路协议整套删掉了（见
 *     ai_companion_req.h 头上）。现在 hello_app 那边的麦克风是纯常开，我们这一轮
 *     开麦会像任何一次半双工切换一样把它挤掉；开不起来就是 -EBUSY，按下面那条
 *     "听不到回答"走（按钮和超时那两条路照常生效）。
 *
 * 网络不通也必须能用：弹窗、响铃、等回答、报警页全部是本地动作，网络那几步
 * （MQTT / 手机推送）失败只打日志。摔倒报警是安全功能，不能挂在一个公网来回上。
 */

/* 缓冲区（都走堆：320 KB 级的静态数组会把内核 SRAM 顶满，见 VOICE_PCM_MAX_BYTES 那段） */
#define FALL_TTS_BUF_BYTES      (16000 * 2 * 20)  /* 一句话 20 秒封顶，绰绰有余 */
#define FALL_PCM_MAX_BYTES      (16000 * 2 * 10)  /* 语音回答最多攒 10 秒 */
#define FALL_PCM_MIN_BYTES      (16000 * 2 / 2)   /* 少于 0.5 秒不值得发一次 ASR */
#define FALL_RATE_HZ            16000

/* 听回答时的断句：老人答"没有"两个字，说完 1.5 秒静音就够判一句完了。
 * 用 ai_audio 默认的 3 秒也不是不行，但那会让"答完到报警/取消"白等 1.5 秒；
 * 也不敢再短（1.2 秒会在老人句中小停顿时把话截断，ASR 容易听成"没…"）。
 * 最小语音长度 300ms：和默认一致（比这短的当噪声，不启动一句）。 */
#define FALL_LISTEN_SILENCE_MS     1500
#define FALL_LISTEN_MIN_SPEECH_MS   300

/* 判定结果（fall_classify 的返回） */
#define FALL_VERDICT_NO       0   /* 否定：取消警报 */
#define FALL_VERDICT_YES      1   /* 肯定：正式报警 */
#define FALL_VERDICT_UNCLEAR  2   /* 没听清 / 不认识：按"超时没回应"走 */

/* 这条链的共享状态。写者是两条线程（链条工作线程 + LVGL 线程里的按钮回调）
 * 和 MQTT 收包线程（自测动作），读法约定：
 *   - 这几个标志都是"一置就不再改"的单向位，32 位对齐读写在这颗 Cortex-M 上
 *     是原子的，所以不逐位加锁（和 g_voice_pcm_full 一个路子）；
 *   - 只有"抢链条"这一件事必须原子，那一段用 g_fall_lock。 */
static pthread_mutex_t g_fall_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile bool   g_fall_active = false;         /* 链条在跑（幂等闸门） */
static volatile int    g_fall_state = FALL_ALARM_IDLE;/* fall_alarm_state_t 快照 */
static volatile bool   g_fall_answer_ready = false;   /* 回答已定（按钮或语音） */
static volatile int    g_fall_answer = -1;            /* -1 未定 / 0 没有 / 1 有 */
static char            g_fall_answer_src[16];         /* 谁定的（日志用） */
static volatile bool   g_fall_speech_started = false; /* VAD：听到有人开始说话 */
static volatile bool   g_fall_speech_ended = false;   /* VAD：这一句说完了 */
static volatile bool   g_fall_pcm_full = false;       /* 攒到上限，后面丢掉 */
static unsigned char  *g_fall_pcm = NULL;             /* 语音回答累积缓冲（堆，首次用时分配） */
static size_t          g_fall_pcm_len = 0;

/* 毫秒时钟（自开机起，单调） */
static uint32_t fall_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint32_t)ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000));
}

/* 回答的判定表（用户要求"识别到什么算否定、什么算肯定"写成一张小表）
 *
 *   否定 -> 取消警报 ：没有 / 没摔 / 没跌 / 没事 / 不用 / 不需要 / 还好 /
 *                     不疼 / 不痛 / 挺好 / 没关 / 别管 / 不碍事 / 没啥
 *   肯定 -> 正式报警 ：摔倒 / 摔了 / 摔到 / 跌 / 起不来 / 站不起来 / 不能动 /
 *                     动不了 / 救命 / 救 / 帮我 / 需要 / 疼 / 痛 / 难受 /
 *                     不舒服 / 骨折 / 流血 / 头晕 / 恶心
 *   其余（含空串、ASR 失败）-> 没听清，按"超时没回应"走正式报警（安全侧）
 *
 * ⚠️ 判定顺序**必须先否定再肯定**：肯定词表里的短词本身是很多否定回答的**子串**
 *    ——「有」在「没有」里、「疼」在「不疼」里、「需要」在「不需要」里。反过来先
 *    匹配肯定，就会把"没有"（含"有"）判成摔倒、"不疼"判成受伤，也就是把
 *    "我没事"变成一次正式报警 —— 这条链最怕的方向。
 * ⚠️ 表里都是整词/短句，**不用单字**（"救"是唯一例外，它是"救命/救我"的词根，
 *    单独出现也确实是求救）：宁可判成没听清（会报警），不要猜。
 * ⚠️ 判成"没听清"的后果是发正式报警，所以这张表只影响"会不会多报一次"，
 *    不会影响"该报的时候报不报"。
 */
static const char *const g_fall_neg_keywords[] = {
    "没有", "没摔", "没跌", "没事", "不用", "不需要", "还好", "不疼", "不痛",
    "挺好", "没关", "别管", "不碍事", "没啥"
};

static const char *const g_fall_yes_keywords[] = {
    "摔倒", "摔了", "摔到", "跌", "起不来", "站不起来", "不能动", "动不了",
    "救命", "救", "帮我", "需要", "疼", "痛", "难受", "不舒服",
    "骨折", "流血", "头晕", "恶心"
};

/* 按上面那张表判一句 ASR 原文。先否定、再肯定、最后留给"没听清"。 */
static int fall_classify(const char *text)
{
    size_t i;

    if (text == NULL || text[0] == '\0') {
        return FALL_VERDICT_UNCLEAR;
    }

    for (i = 0; i < sizeof(g_fall_neg_keywords) / sizeof(g_fall_neg_keywords[0]); i++) {
        if (strstr(text, g_fall_neg_keywords[i]) != NULL) {
            printf("[Fall] ④ 判定：否定（命中「%s」）\n", g_fall_neg_keywords[i]);
            return FALL_VERDICT_NO;
        }
    }

    for (i = 0; i < sizeof(g_fall_yes_keywords) / sizeof(g_fall_yes_keywords[0]); i++) {
        if (strstr(text, g_fall_yes_keywords[i]) != NULL) {
            printf("[Fall] ④ 判定：肯定（命中「%s」）\n", g_fall_yes_keywords[i]);
            return FALL_VERDICT_YES;
        }
    }

    /* 兜最朴素的回答："有"。它落在所有否定词之外（"没有"上面已经命中），
     * 但不在肯定表里（表里不用单字），所以单独认一下 —— 「您摔到了吗？」
     * 对着问的最短回答就是它。 */
    if (strstr(text, "有") != NULL && strstr(text, "没") == NULL) {
        printf("[Fall] ④ 判定：肯定（整句里就一个「有」）\n");
        return FALL_VERDICT_YES;
    }

    printf("[Fall] ④ 判定：没听清（表里没有能认的词）\n");
    return FALL_VERDICT_UNCLEAR;
}

/* 定回答：**先到的算数**（按钮 / 语音 / 自测三条路都可能先到）。
 * 任何线程可调，只写几个标志，不碰设备、不碰控件。 */
static void fall_set_answer(int answer, const char *from)
{
    if (!g_fall_active) {
        /* 链条没在跑（已经收完尾 / 或者根本没触发过）：这条回答没有归属，
         * 丢掉。手机端误发一条 fall_answer 自测命令不会在几秒钟之后
         * 莫名其妙地冒出一句"回答已定"。 */
        printf("[Fall] 链条没在跑，这次回答（%s）丢掉\n", (from != NULL) ? from : "?");
        return;
    }

    if (g_fall_answer_ready) {
        printf("[Fall] 回答已经定过了，这次（%s）忽略\n", (from != NULL) ? from : "?");
        return;
    }

    g_fall_answer = answer;
    snprintf(g_fall_answer_src, sizeof(g_fall_answer_src), "%s",
             (from != NULL) ? from : "?");

    /* 先把来源写好再置"已定"：读的人看到 ready 时 src/answer 一定已经是最新的 */
    g_fall_answer_ready = true;

    printf("[Fall] 回答已定：%s（来自%s）\n",
           (answer == 1) ? "有" : "没有", (from != NULL) ? from : "?");
}

/* 弹窗上的两个按钮（**在 LVGL 线程里被调**，见 touch_ui.h）：
 * 只登记回答，立刻返回；后面的动作在链条工作线程里做。 */
static void fall_answer_btn_cb(touch_fall_answer_t answer, void *user_data)
{
    (void)user_data;

    fall_set_answer((answer == TOUCH_FALL_ANSWER_YES) ? 1 : 0, "弹窗按钮");
}

/* 录音数据回调（在 ai_audio 的**录音线程**里跑，每帧一次，不许阻塞） */
static void fall_record_cb(const int16_t *data, size_t frames, void *user_data)
{
    size_t bytes = frames * sizeof(int16_t);

    (void)user_data;

    if (data == NULL || bytes == 0 || g_fall_pcm == NULL || g_fall_pcm_full) {
        return;
    }

    if (g_fall_pcm_len + bytes > FALL_PCM_MAX_BYTES) {
        if (!g_fall_speech_started) {
            /* 还没听到人说话：这是干等时的静音，把缓冲滚掉重来 ——
             * 不能在这里停手，老人可能再过几秒才开口。 */
            g_fall_pcm_len = 0;
        } else {
            g_fall_pcm_full = true;
            printf("[Fall] ④ 语音回答攒到 %u 秒上限，后面的丢掉\n",
                   (unsigned)(FALL_PCM_MAX_BYTES / (FALL_RATE_HZ * 2)));
            return;
        }
    }

    memcpy(g_fall_pcm + g_fall_pcm_len, data, bytes);
    g_fall_pcm_len += bytes;
}

/* VAD 回调（同样在录音线程里跑）：只置标志，**不许在这里调 ASR**
 * （voice_asr_recognize 是阻塞 HTTPS，在这里调就把录音线程钉住了）。 */
static void fall_vad_cb(bool speech_detected, void *user_data)
{
    (void)user_data;

    if (speech_detected) {
        if (!g_fall_speech_started) {
            g_fall_speech_started = true;
            printf("[Fall] ④ 听到有人说话（开始累积语音回答）\n");
        }
    } else {
        if (!g_fall_speech_ended) {
            g_fall_speech_ended = true;
            printf("[Fall] ④ 用户说完了（静音 %u ms），准备送去识别\n",
                   (unsigned)FALL_LISTEN_SILENCE_MS);
        }
    }
}

/* 播放完成回调（在 ai_audio 的播放线程里跑）：只置标志 */
static void fall_play_done(void *user_data)
{
    *(bool *)user_data = true;
}

/* 语音问一句（工作线程里跑）。
 *
 * 顺序：先合成（这时不占播放设备）-> 出声 -> 等它放完。
 * 说话没成也照常往下走：老人还能点按钮，点不动还有超时那条路。
 */
static int fall_speak_question(void)
{
    unsigned char *pcm;
    size_t pcm_len = 0;
    bool done = false;
    long rate_hz;
    long wait_ms;
    int rc;
    int ret;

    /* 先查预缓存（2026-09-19 加）：这句问句是写死的（fall_alarm.h 的
     * FALL_ALARM_QUESTION），PCM 已经随固件打进 ROMFS 了 —— 命中就不走网络，
     * "网络慢/断 → 摔倒了却问不出来"这条直接消失。**这条尤其值当**：跌倒这条链
     * 是"问一句 → 听回答 → 决定报不报警"，问句卡在云端 TLS 里等于整条链一起等。
     * 没预生成的话照旧落回下面那次 live TTS。 */
    if (tts_cache_load(FALL_ALARM_QUESTION, &pcm, &pcm_len) == 0) {
        printf("[Fall] ③ 命中预缓存: %zu 字节（约 %zu ms），不走网络\n",
               pcm_len, pcm_len * 1000 / (FALL_RATE_HZ * 2));
    } else {
        pcm = malloc(FALL_TTS_BUF_BYTES);
        if (pcm == NULL) {
            printf("[Fall] ③ 合成缓冲分配失败，这次不念了（弹窗和按钮照常）\n");
            return -ENOMEM;
        }

        ret = voice_tts_speak(FALL_ALARM_QUESTION, pcm, FALL_TTS_BUF_BYTES, &pcm_len);
        if (ret < 0 || pcm_len == 0) {
            printf("[Fall] ③ 语音合成失败（%d），这次不念了（弹窗和按钮照常）\n", ret);
            free(pcm);
            return (ret < 0) ? ret : -EIO;
        }
    }

    ret = audio_play_start(&g_audio_ctx, (const int16_t *)pcm, pcm_len / 2,
                           fall_play_done, &done);
    free(pcm);      /* audio_play_start 已经把 PCM 拷进自己的播放缓冲 */

    if (ret < 0) {
        rc = ret;
    } else {
        rate_hz = (long)g_audio_ctx.config.sample_rate;
        if (rate_hz <= 0) {
            rate_hz = FALL_RATE_HZ;
        }

        wait_ms = (long)((pcm_len / 2) * 1000 / (size_t)rate_hz) + 3000;

        while (!done && wait_ms > 0) {
            usleep(20000);
            wait_ms -= 20;
        }

        rc = audio_play_last_result(&g_audio_ctx);

        if (!done) {
            rc = audio_is_playing(&g_audio_ctx) ? -ETIMEDOUT :
                 ((rc == 0) ? -EIO : rc);
        }
    }

    if (rc == 0) {
        printf("[Fall] ③ 询问：放完了（麦克风先不还，接着听回答）\n");
    } else {
        printf("[Fall] ③ 询问：没响成（rc=%d），继续等回答（按钮 / 超时仍然生效）\n", rc);
    }

    return rc;
}

/* 开麦听回答（工作线程里跑）。返回 false 只表示"这一路没开起来"，
 * 按钮和超时那两条路照常 —— 调用方不需要为此改变流程。 */
static bool fall_listen_open(void)
{
    audio_record_config_t rec;
    int ret;

    if (audio_is_recording(&g_audio_ctx)) {
        printf("[Fall] ④ 麦克风被别的路径占着，先停掉\n");
        audio_record_stop(&g_audio_ctx);
    }

    /* 常态听音常驻占着麦克风（默认关，开了才有）：先按停再用设备
     * （和 voice_open_thread 一样，pause() 返回即保证设备已放）。
     * 恢复在链条收尾处（ambient_listen_resume），和语音聊天那条路一样成对。 */
    ambient_listen_pause();

    /* 语音回答缓冲：分配一次就留着（本链可能被触发很多次，而 320 KB 的
     * malloc/free 每轮来一次只会多一处失败点）。**不 free 还有一个更硬的理由**：
     * audio_record_stop() 回收录音线程是"有界等待 + 放弃"，返回时线程不一定
     * 真死透了（见 ai_audio.h 里 record_exited 的说明），这时候 free 掉缓冲，
     * 那条线程下一帧就会写进已释放的内存。 */
    if (g_fall_pcm == NULL) {
        g_fall_pcm = malloc(FALL_PCM_MAX_BYTES);
        if (g_fall_pcm == NULL) {
            printf("[Fall] ④ 语音缓冲分配失败，这次听不成（按钮 / 超时仍然生效）\n");
            return false;
        }
    }

    g_fall_pcm_len = 0;
    g_fall_pcm_full = false;
    g_fall_speech_started = false;
    g_fall_speech_ended = false;

    /* VAD 的回调是**整机一份**的槽位（ai_audio 里只有 ctx->vad_callback 一个）：
     * 这一窗口换成我们自己的，收尾时（fall_listen_close）还原成 robot_ui 原来
     * 那个 vad_callback。不还原的话，之后别处的 VAD 事件会打进这条已经结束的链。 */
    audio_vad_enable(&g_audio_ctx, fall_vad_cb, NULL);

    memset(&rec, 0, sizeof(rec));
    rec.enable_vad = true;
    rec.silence_timeout_ms = FALL_LISTEN_SILENCE_MS;
    rec.min_speech_ms = FALL_LISTEN_MIN_SPEECH_MS;
    rec.data_callback = fall_record_cb;
    rec.user_data = NULL;

    ret = audio_record_start(&g_audio_ctx, &rec);
    if (ret != 0) {
        printf("[Fall] ④ 开麦失败: %d，这次听不成语音回答（按钮 / 超时仍然生效）\n", ret);
        audio_vad_enable(&g_audio_ctx, vad_callback, NULL);   /* 槽位还原 */
        return false;
    }

    printf("[Fall] ④ 开始限时听回答（最多 %u 秒；说完静音 %u ms 就送去识别）\n",
           (unsigned)(FALL_ALARM_ANSWER_TIMEOUT_MS / 1000),
           (unsigned)FALL_LISTEN_SILENCE_MS);
    return true;
}

/* 收麦（工作线程里跑）。
 * （单出口，见本节头上那段）；这里只把设备和 VAD 槽位收干净。 */
static void fall_listen_close(void)
{
    if (audio_is_recording(&g_audio_ctx)) {
        audio_record_stop(&g_audio_ctx);
    }

    audio_vad_enable(&g_audio_ctx, vad_callback, NULL);       /* VAD 槽位还原 */
}

/* ⑤ 正式报警：手机收到「机主摔倒！」+ 板子响铃（+ 红色报警页）。
 * 顺序按既有教训排：**先切画面、再响铃、最后上网** —— 网络（TLS/推送）慢或卡住
 * 时，用户至少已经能看到报警页、也已经听到铃声。 */
static void fall_escalate(const char *why)
{
    int ret;

    printf("[Fall] ⑤ 正式报警：机主摔倒！（判定依据：%s）\n", why);

    /* ① 报警页（投递到 LVGL 线程；它内部自己会切 scr_alarm 并响铃，见
     *    robot_ui_show_alarm —— 那里还顺手报了一次 report_alarm_queued("ui")）。 */
    touch_ui_hide_fall_ask();
    ui_post_alarm("机主摔倒！");

    /* ② 板子响铃：板级报警模块（非阻塞，自己开 /dev/audio/audio0）。
     *    这里**再显式报一次**，是因为它是唯一不依赖界面线程的一段 ——
     *    界面那一路万一卡住，报警声也必须响。同级重复触发只更新 reason/text
     *    （见 alarm_trigger 的说明），不会响两遍。reason 用 "fall"：
     *    docs/alarm_usage.md 里给摔倒留的就是这个标识。 */
    ret = alarm_trigger(ALARM_LEVEL_EMERGENCY, "fall", "机主摔倒！");
    if (ret != OK) {
        printf("[Fall] ⑤ alarm_trigger 失败: %d（报警声可能没响）\n", ret);
    }

    /* ③ MQTT 上报 + 手机推送。排队发布：本函数在工作线程里，不是 network_task
     *    的 task group，直发必然失败（见 mqtt_publish_queued 的说明）。
     *    report_alarm_queued() 内部已经带了一次 push_send_alarm。 */
    ret = report_alarm_queued("fall", "机主摔倒！");
    if (ret < 0) {
        printf("[Fall] ⑤ MQTT 上报没入队: %d（报警页和铃声不受影响）\n", ret);
    }

    /* 手机上最醒目的那一条：标题就是这句话（report_alarm_queued 那条推送的标题
     * 是 "[ALARM] fall"，正文里才有"机主摔倒！"，对家属不够直白）。 */
    if (push_send_notification("机主摔倒！", "检测到机主摔倒，请立即查看！", "alarm") < 0) {
        printf("[Fall] ⑤ 手机推送没发出去（推送没开或没配 key）\n");
    }

    /* 状态机：和"声音检测到异常"走同一个事件（本工程的状态机没有"摔倒确认"
     * 这个态，也不为此新增一个 —— 报警就是报警，多一个态只会多一处要维护）。 */
    sm_handle_event(&g_sm_ctx, SM_EVENT_ALARM_DETECTED);

    printf("[Fall] ⑤ 报警已完成：报警页 + 响铃 + MQTT(alarm) + 手机推送「机主摔倒！」\n");
}

/* ⑤ 取消警报：撤下弹窗 + 停止响铃 + 手机收到「已取消」 */
static void fall_cancel(const char *why)
{
    printf("[Fall] ⑤ 取消警报：撤弹窗 + 停响铃 + 通知手机「已取消」（判定依据：%s）\n", why);

    touch_ui_hide_fall_ask();

    /* 没在响时是安全空操作。这里会连同**别的来源**正在响的报警一起停掉
     * （报警模块只有一个状态），这是刻意的：机主刚亲口说"没有摔倒"，
     * 现场不该继续响铃。 */
    if (alarm_clear() != OK) {
        printf("[Fall] ⑤ alarm_clear 返回非 OK（本来就没在响也是正常的）\n");
    }

    /* MQTT 侧留一条痕（复用的 sound_alarm 通道，sound_type 带 _cancelled 后缀，
     * 一眼能看出这不是新报警） */
    if (report_abnormal_sound_queued("fall_cancelled", 0) < 0) {
        printf("[Fall] ⑤ MQTT「已取消」没入队（网络不通不影响取消动作本身）\n");
    }

    if (push_send_notification("已取消", "机主已确认没有摔倒，报警已取消。", "fall") < 0) {
        printf("[Fall] ⑤ 手机推送没发出去（推送没开或没配 key）\n");
    }

    sm_handle_event(&g_sm_ctx, SM_EVENT_ALARM_CLEARED);

    printf("[Fall] ⑤ 已取消：弹窗已撤下，铃声已停，手机收到「已取消」\n");
}

/* 链条工作线程：①②③④⑤ 全在这里串行做（单出口，任何一步失败都往下走） */
static void *fall_chain_thread(void *arg)
{
    char *source = (char *)arg;
    char why[80];
    char text[256];
    uint32_t t0;
    int answer = -1;

    printf("[Fall] ===== 链条开始（source=%s）=====\n",
           (source != NULL) ? source : "-");

    /* ① 弹窗。先弹它、再上网（这个工程踩过"先上网后弹窗"的坑：网络一慢，
     *    用户什么都看不到）。弹窗是投递出去的，LVGL 线程下一拍就画出来。
     *    闸门：界面还没起来（robot_ui 没在跑 / 还在初始化）时不投 ——
     *    那时候投进去的 lv_async_call 没人消费（见 robot_ui_bridge.h 那张闸门
     *    的说明）。报警和响铃不受这个闸门影响，照常走。 */
    if (robot_ui_bridge_is_ready()) {
        touch_ui_show_fall_ask();
        printf("[Fall] ① 弹窗（已投递）：%s —— 有 / 没有\n", FALL_ALARM_QUESTION);
    } else {
        printf("[Fall] ① 界面还没就绪，这次不弹窗（报警/响铃/推送照常）\n");
    }

    /* ② 手机 + MQTT：一条「疑似摔倒」。
     *    MQTT 走 sound_alarm 这条**既有**通道（sound_type 取文档里已有的 "fall"，
     *    语义正好是"异常声音/疑似事件"），手机通知走现成的 BARK 推送。
     *    两条都是非阻塞排队，网络不通只打日志。 */
    if (report_abnormal_sound_queued("fall", 100) < 0) {
        printf("[Fall] ② MQTT「疑似摔倒」没入队（网络不通不影响弹窗和问话）\n");
    }

    if (push_send_notification("疑似摔倒", "检测到疑似摔倒，正在询问机主是否安全…", "fall") < 0) {
        printf("[Fall] ② 手机推送没发出去（推送没开或没配 key）\n");
    }

    printf("[Fall] ② 已上报「疑似摔倒」：MQTT(sound_alarm=fall) + 手机推送\n");

    /* ③ 语音问一句（同时把麦克风留在我们手里，紧接着要听回答） */
    printf("[Fall] ③ 语音询问：%s\n", FALL_ALARM_QUESTION);
    fall_speak_question();

    /* ④ 限时听回答：点按钮 / 说话，先到的算数；都没有就等超时。
     * 状态行如实反映"能不能听到你说话"—— 开不了麦时别骗用户对着板子说。 */
    if (fall_listen_open()) {
        touch_ui_set_fall_status("我在听，请说「有」或「没有」");
    } else {
        touch_ui_set_fall_status("请点「有」或「没有」");
    }

    t0 = fall_now_ms();

    while (1) {
        if (g_fall_answer_ready) {
            break;                       /* 按钮（或自测）已经回答 */
        }

        if (g_fall_speech_ended) {
            break;                       /* 说完了，下面去识别 */
        }

        if ((uint32_t)(fall_now_ms() - t0) >= (uint32_t)FALL_ALARM_ANSWER_TIMEOUT_MS) {
            printf("[Fall] ④ 等回答超时（%u 秒内没有任何回应），按最坏情况处理\n",
                   (unsigned)(FALL_ALARM_ANSWER_TIMEOUT_MS / 1000));
            break;
        }

        usleep(50 * 1000);
    }

    /* 收麦（还麦克风在后面那一处单出口） */
    fall_listen_close();

    /* 语音那条路：说完了、而按钮还没点过 -> 把攒下来的 PCM 送去识别。
     * ASR 是阻塞 HTTPS（秒级），在本工作线程里做，别处都不合适。 */
    if (!g_fall_answer_ready && g_fall_speech_ended) {
        if (g_fall_pcm_len < FALL_PCM_MIN_BYTES) {
            printf("[Fall] ④ 语音只有 %u 字节（不足 %u），当成没听清\n",
                   (unsigned)g_fall_pcm_len, (unsigned)FALL_PCM_MIN_BYTES);
        } else {
            int ret = voice_asr_recognize(g_fall_pcm, g_fall_pcm_len,
                                          text, sizeof(text));

            if (ret < 0) {
                printf("[Fall] ④ ASR 失败: %d（按没听清处理 -> 正式报警）\n", ret);
            } else {
                printf("[Fall] ④ ASR 结果：「%s」\n", text);

                int verdict = fall_classify(text);

                if (verdict == FALL_VERDICT_NO) {
                    fall_set_answer(0, "语音回答");
                } else if (verdict == FALL_VERDICT_YES) {
                    fall_set_answer(1, "语音回答");
                } else {
                    printf("[Fall] ④ 没听清（按超时没回应处理 -> 正式报警）\n");
                }
            }
        }
    }

    /* 把常态听音放回来（默认关，开了才有；和 fall_listen_open 里的 pause() 成对）。 */
    ambient_listen_resume();

    /* ⑤ 判定：只有"明确答没有"才取消，其余（有 / 没听清 / 超时没回应）一律正式报警。
     *    这是安全功能的取舍：宁可多报一次（家属白跑一趟），也不漏报一次。 */
    if (g_fall_answer_ready) {
        answer = g_fall_answer;
    }

    if (answer == 0) {
        snprintf(why, sizeof(why), "机主答「没有」，来自%s",
                 g_fall_answer_src[0] ? g_fall_answer_src : "?");
        fall_cancel(why);
    } else if (answer == 1) {
        snprintf(why, sizeof(why), "机主答「有」，来自%s",
                 g_fall_answer_src[0] ? g_fall_answer_src : "?");
        fall_escalate(why);
    } else {
        fall_escalate("超时没回应（也含没听清 / 说不出来）");
    }

    /* 收尾：状态落定 + 放行下一次触发（下一个 fall_alarm_trigger 才能进来） */
    pthread_mutex_lock(&g_fall_lock);
    g_fall_state  = (answer == 0) ? FALL_ALARM_CANCELLED : FALL_ALARM_ALARMED;
    g_fall_active = false;
    pthread_mutex_unlock(&g_fall_lock);

    printf("[Fall] ===== 链条结束：%s =====\n",
           (answer == 0) ? "已取消警报" : "已正式报警（机主摔倒！）");

    free(source);
    return NULL;
}

/* ==================== 上面的链条的公开入口（fall_alarm.h） ==================== */

int fall_alarm_trigger(const char *source)
{
    pthread_attr_t attr;
    pthread_t tid;
    char *copy;

    /* 中断里不能做：要 malloc、要起线程（和 robot_ui_bridge 那道
     * up_interrupt_context() 检查同一类原因） */
    if (up_interrupt_context()) {
        printf("[Fall] 在中断上下文里，不触发（要起线程、要 malloc）\n");
        return FALL_ALARM_TRIGGER_ERROR;
    }

    /* 幂等闸门：链条没跑完时重复触发一律忽略（不叠弹窗、不发第二条报警） */
    pthread_mutex_lock(&g_fall_lock);

    if (g_fall_active) {
        pthread_mutex_unlock(&g_fall_lock);
        printf("[Fall] 已在处理中，忽略本次触发（source=%s）\n",
               (source != NULL) ? source : "-");
        return FALL_ALARM_TRIGGER_BUSY;
    }

    g_fall_active = true;
    g_fall_state  = FALL_ALARM_ASKING;

    /* 上一轮的残留在这里清（上一轮线程已经收完尾才会放开 active，见线程末尾） */
    g_fall_answer_ready = false;
    g_fall_answer       = -1;
    g_fall_answer_src[0] = '\0';
    g_fall_speech_started = false;
    g_fall_speech_ended   = false;
    g_fall_pcm_full       = false;

    pthread_mutex_unlock(&g_fall_lock);

    copy = ui_strdup((source != NULL) ? source : "-");
    if (copy == NULL) {
        pthread_mutex_lock(&g_fall_lock);
        g_fall_active = false;
        g_fall_state  = FALL_ALARM_IDLE;
        pthread_mutex_unlock(&g_fall_lock);
        printf("[Fall] 内存不够，这次触发没跑起来\n");
        return FALL_ALARM_TRIGGER_ERROR;
    }

    printf("[Fall] 收到「疑似摔倒」上报：source=%s（开始跑事件链）\n", copy);

    /* detached + 32 KB 栈：里面要跑 TTS 合成和 ASR 识别（都含完整的 TLS 握手，
     * 栈小到压着边界时症状是 TLS 随机报 -0x7200，见 reminder_start_announce）。
     * 起失败也要把闸门放开，否则这条链就再也进不来了。 */
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 32768);

    if (pthread_create(&tid, &attr, fall_chain_thread, copy) != 0) {
        pthread_attr_destroy(&attr);
        free(copy);

        pthread_mutex_lock(&g_fall_lock);
        g_fall_active = false;
        g_fall_state  = FALL_ALARM_IDLE;
        pthread_mutex_unlock(&g_fall_lock);

        printf("[Fall] 工作线程起不来，这次不跑（等下一次触发）\n");
        return FALL_ALARM_TRIGGER_ERROR;
    }

    pthread_attr_destroy(&attr);
    return FALL_ALARM_TRIGGER_OK;
}

fall_alarm_state_t fall_alarm_get_state(void)
{
    return (fall_alarm_state_t)g_fall_state;
}

/* 主函数 */
/* 后台对时线程：等网络通了再取时间。
 *
 * 不能放在 LVGL 主循环里做：HEAD 请求要 TLS 握手，秒级，会把界面卡住。
 * 板子断电后时间是 2000-01-01，而"提醒"是按真实日期排的 RTC 日循环闹钟，
 * 所以这一步不做，提醒到点就不会响（或者响在错误的日子）。 */
static void *time_sync_thread(void *arg)
{
  int i;

  (void)arg;

  /* 对时一直重试到成功：板子刚开机时 RNDIS 往往还没枚举好（要等 PC 侧认到设备
   * 并开了网络共享），只试几次就放弃的话，晚插一会儿 USB 就永远对不上时。
   * 每 60 秒一次，成功后线程自己退出；失败日志只在第 1 次和每 10 次打一条。 */
  for (i = 1;; i++)
    {
      if (time_sync_once() == 0)
        {
          printf("[TimeSync] 完成（第 %d 次尝试）\n", i);
          return NULL;
        }

      if (i == 1 || (i % 10) == 0)
        {
          printf("[TimeSync] 还没成功（第 %d 次），60 秒后重试\n", i);
        }

      sleep(60);
    }
}

int main(int argc, char *argv[])
{
    printf("ZhiAi Companion starting...\n");

    /* ===== 初始化 LVGL 与显示设备（必须在 UI 创建之前） ===== */
    lv_init();
    lv_nuttx_dsc_t lv_info = {0};
    lv_nuttx_result_t lv_result = {0};
    lv_nuttx_dsc_init(&lv_info);
    lv_nuttx_init(&lv_info, &lv_result);
    if (lv_result.disp == NULL)
    {
        printf("LVGL display init failed!\n");
        return 1;
    }

#if UI_PERF_LOG
    /* 刷屏计时仪表（ui_perf.c）：挂上后只在"慢"的时候打日志。 */
    ui_perf_attach_display(lv_result.disp);
#endif

    /* 屏幕镜像（真机屏幕坏了，拿电脑当显示器）：挂 flush 钩子 + 起镜像任务。
     * 开机自动起，目标默认 192.168.137.1:5600；控制命令 `hw_test lcdmirror ...`。
     * 必须在 lv_nuttx_init() 成功之后、建界面之前调 —— 影子帧缓冲靠"开机头几秒
     * 整屏重绘"填满，挂晚了也没事，客户端连上会再整屏重绘一次。 */
    lcd_mirror_attach_lvgl_display(lv_result.disp);

    /* 触摸采样周期:默认跟随 LV_DEF_REFR_PERIOD（33ms ~ 30Hz），手感偏迟钝。
     * 这里只把输入设备读取定时器提到 10ms，屏幕刷新节奏不变。 */
    if (lv_result.indev != NULL)
    {
        lv_timer_set_period(lv_indev_get_read_timer(lv_result.indev), 10);
    }

    /* ===== 初始化网络通信 ===== */
    network_comm_init();

    /* ===== WiFi 连接 ===== */
    /* NOTE: 板子通过 USB RNDIS 上网，WiFi 代码未实际使用。
     * network_comm.c 中的 wifi_connect() 只是设置 wifi_config.connected = true，
     * 让 MQTT 能启动连接。不要删除此调用，否则 MQTT 不会连接。 */
    wifi_connect("RNDIS", "");

    /* ===== 启动网络后台任务 =====
     * 负责 MQTT 连接 broker.emqx.io:1883、收消息、发心跳。
     * network_task() 一直存在但从来没被创建过，所以 MQTT 一次都没连上过。
     * 它自己会轮询 wifi_config.connected，所以放在 wifi_connect() 之后创建。
     */
    if (task_create("net_task", 100, 12288, (main_t)network_task, NULL) < 0) {
        printf("net_task create failed\n");
    }

    /* ===== 初始化手机推送服务 ===== */
    /* key 传 NULL：由 network_comm.c 统一从 /etc/assets/push_key.txt
     * 或那里的 PUSH_KEY_DEFAULT* 取，源码里不再硬编码密钥。 */
    /* PushPlus (Android 微信推送) */
    push_init(PUSH_SERVICE_PUSHPLUS, NULL);
    /* Bark (iPad iOS 推送) */
    push_init(PUSH_SERVICE_BARK, NULL);

    /* ===== 注册回调函数 ===== */
    network_set_mqtt_callback(on_mqtt_message_received);
    network_set_ai_command_callback(on_ai_command_received);
    /* 心跳/诊断要报 robot_ui 自己的状态（AI 就绪、录音、放音、镜像面板），
     * 而那几个量只有本文件看得见 —— 注册一个只读回调给 network_comm 来问
     * （跑在 network_task 线程里，只读全局量，见 local_state_probe 的说明）。 */
    network_set_local_state_provider(local_state_probe);

    /* ===== 初始化 AI 模块 (成员二) ===== */
    printf("Initializing AI modules...\n");

    /* 初始化状态机 */
    if (sm_init(&g_sm_ctx) == 0) {
        printf("State machine initialized\n");
    }

    /* 初始化音频模块 */
    audio_config_t audio_config = {
        .sample_rate = AUDIO_RATE_16K,
        .channels = AUDIO_CH_MONO,
        .format = AUDIO_FORMAT_S16_LE,
        /* frame_ms 不填是 0，audio_init() 的校验会直接返回 -EINVAL，
         * 于是"语音聊天"按钮一直录不到音（audio_record_start 失败）。 */
        .frame_ms = AUDIO_DEFAULT_FRAME_MS
    };
    if (audio_init(&g_audio_ctx, &audio_config) == 0) {
        printf("Audio module initialized\n");
        /* 启用 VAD 检测 */
        audio_vad_enable(&g_audio_ctx, vad_callback, NULL);
    }

    /* 不再初始化 ai_llm：对话改走 mimo_voice.c 的 mimo_chat()（见文件头注释）。
     * 原来这里的 llm_init() 只是给 llm_send_text() 用的，而后者会把消息投进
     * ai_agent 进程独享的消息总线，别的 app 调必崩。 */

    /* 初始化声音检测 */
    sound_detect_config_t detect_cfg = {
        .mode = DETECT_MODE_REALTIME,
        .threshold = SOUND_DETECT_THRESHOLD_DEFAULT,
        .sample_rate = SOUND_DETECT_SAMPLE_RATE,
        .frame_ms = SOUND_DETECT_FRAME_MS,
        .enable_vad = true,
        .enable_feedback = true,
        .callback = sound_alarm_callback,
        .user_data = NULL
    };
    if (sound_detect_init(&g_sound_ctx, &detect_cfg) == 0) {
        printf("Sound detect initialized\n");
    }

    /* 初始化主动关怀 */
    care_config_t care_cfg = {
        .enable_greeting = true,
        .enable_health = true,
        .enable_life = true,
        .enable_exercise = true,
        .callback = care_remind_callback,
        .user_data = NULL
    };
    if (care_init(&g_care_ctx, &care_cfg) == 0) {
        printf("Care module initialized\n");
    }

    /* 注册语音 ASR/TTS 后端并选中 MiMo。
     * 凭据由 ai_agent 从 /data/ai_agent/config/config.json 读（开机自动装好），
     * 这里不碰任何密钥。没配好时后面 voice_asr_recognize()/voice_tts_speak()
     * 会返回负 errno，弹窗里会如实报"识别失败/语音合成失败"，不会静默。 */
    mimo_asr_register();
    mimo_tts_register();
    if (voice_asr_set_backend("mimo") == 0 &&
        voice_tts_set_backend("mimo") == 0) {
        printf("Voice backend: mimo\n");
    } else {
        printf("Voice backend: mimo unavailable (config check failed)\n");
    }

    g_ai_initialized = true;
    printf("AI modules initialization done\n");

    /* ===== 常态听音（工作流 C）：登记回调，默认关 =====
     * 配置键 enable_ambient_listen 没开就只是登记，不起线程、不占麦克风；
     * 开了才会常驻听 + 常驻约 283 KiB 堆。开关见 ambient_listen_init()。
     *
     * ⚠️ 2026-09-14 语音入口统一交给 openvela 框架（hello_app 的 ai_companion）
     * 之后：**麦克风归 ai_companion 的纯常开麦**。robot_ui 默认不再开麦。
     * 如果哪天要打开这个常态听音，它就会跟 ai_companion 抢麦（半双工只有一个麦）——
     * 要用就得先想清楚谁先谁后。 */
    ambient_listen_init(ambient_on_event, NULL);
    ambient_listen_set_busy_cb(ambient_busy, NULL);
    printf("[Ambient] 常态听音: %s\n",
           ambient_listen_is_enabled() ? "已开启" : "关闭（配置键 enable_ambient_listen）");



    /* ===== 后台对时（不阻塞 UI） ===== */
    pthread_t ts_tid;
    if (pthread_create(&ts_tid, NULL, time_sync_thread, NULL) == 0)
      {
        pthread_detach(ts_tid);
      }

    /* ===== 初始化机器人 UI（先创建主屏并 lv_scr_load，成为活动屏） ===== */
    robot_ui_init();

    /* ===== 初始化触摸交互 UI（须在活动屏 = 主屏之后, 菜单才可见） ===== */
    touch_ui_init();

    /* ===== 注册触摸菜单功能回调 ===== */
    touch_ui_set_voice_chat_cb(voice_chat_handler, &g_audio_ctx);
    touch_ui_set_emergency_cb(emergency_call_handler, NULL);
    /* 设置里的音量滑块 -> 音频硬件。注册时它会把当前设置值立刻下发一次
     * （设置是从 /data 读回来的），所以开机就是用户上次调好的音量。 */
    touch_ui_set_volume_cb(volume_set_handler, NULL);
    /* 语音聊天弹窗的两个出口：提交 -> 工作线程跑 ASR→LLM→TTS；× -> 停录音丢缓冲 */
    touch_ui_set_voice_submit_cb(voice_submit_handler, NULL);
    touch_ui_set_voice_cancel_cb(voice_cancel_handler, NULL);
    /* 镜像面板（常开麦那一套的显示器）底部的「提交」：只转发一次"请立刻收尾
     * 这一段"给 hello_app，不起线程、不碰音频设备（见 voice_mirror_submit_handler）。 */
    touch_ui_set_voice_mirror_submit_cb(voice_mirror_submit_handler, NULL);

    /* ===== 摔倒询问面板的两个按钮（有 / 没有）=====
     * 面板本身是 touch_ui 的（touch_ui_show_fall_ask），点下去只回调一个
     * "回答是哪个"过来；真正的动作在摔倒链的工作线程里做（本回调跑在 LVGL
     * 线程里，里面只置标志，见 fall_answer_btn_cb）。 */
    touch_ui_set_fall_answer_cb(fall_answer_btn_cb, NULL);

    /* ===== 异常声响「询问是否报警」页的回答（是的，报警 / 不用了）=====
     * 面板本身是 robot_ui 的（robot_ui_show_ask_alarm），点下去只回调"回答是哪个"
     * 过来；报不报警、以及"同一原因 60 秒内不再问"的记账都在 ask_finish() 里。
     * 回调跑在 LVGL 线程里，里面不做耗时的事（只投递），见 ask_btn_result_cb。 */
    robot_ui_set_ask_result_cb(ask_btn_result_cb, NULL);

    /* ===== 注册提醒的到点回调 ===== */
    /* 必须在添加提醒之前注册：注册完下面 touch_ui_add_reminder() 会立刻
     * 把"下一条"挂到 RTC 闹钟上，到点时就会走 reminder_on_fire()。 */
    reminder_sched_set_fire_cb(reminder_on_fire, NULL);

    /* ===== 把"语音建提醒"接到提醒列表上 ===== */
    /* 注册了这个，mimo_chat() 的 add_reminder 工具才会下发给模型（没注册时
     * 工具不出现在 tools 表里，见 mimo_voice.c 的 chat_reminder_tool_enabled）。
     * 这一步不注册就等于"语音不认提醒"，界面上手动新增仍然照常。 */
    mimo_set_reminder_hook(voice_add_reminder);

    /* ===== 添加默认提醒 ===== */
    /* 只是开机示例，用户可以自己加/删（「提醒」→ 列表）。
     * 注意：提醒只存在内存里，重启就没了（/data 是 tmpfs，见 reminder_sched.h）；
     * 每加一条都会重挂一次 RTC 闹钟，所以加完就是生效的。 */
    touch_ui_add_reminder("吃药", "08:00");
    touch_ui_add_reminder("喝水", "10:00");
    touch_ui_add_reminder("散步", "16:00");

    /* ===== 设置初始状态 ===== */
    robot_ui_set_status(ROBOT_STATUS_IDLE);
    robot_ui_set_face(ROBOT_FACE_HAPPY);
    robot_ui_set_ai_reply("你好！我是智爱陪伴\n有什么可以帮你的吗？");

    /* ===== 显示主菜单 ===== */
    touch_ui_show_menu(MENU_TYPE_MAIN);

    /* ===== 打开"语音链路直调界面"这道闸门 =====
     * 放在这里而不是更早：到这一行 LVGL 已经初始化、主屏和触摸 UI 都建好了，
     * 之后 lv_async_call 投进来的东西一定会被 LVGL 线程消费掉。
     * 之前（比如 lv_init() 还没跑）就放行的话，ai_companion 那边先跑起来时
     * 推过来的会攒在队列里没人处理 —— 那正是我们要避免的"看着没反应"。 */
    robot_ui_bridge_set_ready(true);

    printf("ZhiAi Companion started!\n");

    /* 主循环 */
    while (1) {
        static int  net_tick = 0;
        static bool net_ok   = false;
        static int  time_tick = 0;
        static int  reminder_tick = 0;

        /* 面板内容对不上（开机丢帧 / 面板被复位过）⇒ 把整屏判脏，下面这一帧
         * 就会重画一整屏。必须在 LVGL 线程里调（本循环就是），函数内部自己限速。 */
        robot_ui_bridge_lcd_check();

        /* 计时仪表：量这一轮 lv_timer_handler 的耗时（一帧的"渲染 + 刷屏"都在里面）。
         * 正常时它什么都不打，只有"慢"才由 ui_perf_frame_end() 打一行。 */
#if UI_PERF_LOG
        ui_perf_frame_begin();
#endif
        lvgl_timer_handler();

#if UI_PERF_LOG
        ui_perf_frame_end();
#endif

        /* 每 ~200ms 刷新一次状态栏上的网络状态。
         * LVGL 不是线程安全的，所以只在这个任务里改控件；network_task 那边
         * 只维护 mqtt_config.connected，由这里轮询。
         */
        if (++net_tick >= 40) {
            static int  net_down_ticks = 0;
            static bool net_warned     = false;
            bool ok = mqtt_is_connected();

            net_tick = 0;
            if (ok != net_ok) {
                net_ok = ok;
                robot_ui_set_net_status(ok ? "NET OK" : "NET --");
            }

            /* 开机一直连不上网：弹一页把原因说清楚，别让用户对着"没反应"猜。
             * 只在**从没连上过**且满 ~2 分钟时弹一次；连上过就不再弹（中途掉线
             * 由 MQTT 自己重连，弹页反而打扰）。 */
            if (ok) {
                net_warned = true;
            } else if (!net_warned) {
                if (++net_down_ticks >= 600) {   /* 600 × ~200ms ≈ 2 分钟 */
                    net_down_ticks = 0;
                    net_warned = true;
                    ui_post_error("网络没连上",
                                  "这台设备要连手机热点/电脑共享的网络。\n"
                                  "请检查：\n"
                                  "1. 板子的 USB 线插好没有；\n"
                                  "2. 电脑上的「网络共享」还开着没有。");
                }
            }
        }

        /* 每 ~1s 刷新状态栏时钟 */
        if (++time_tick >= 200) {
            time_tick = 0;
            robot_ui_update_time();

            /* 每 ~30 秒看一眼提醒的闹钟要不要补挂。
             * 板子开机时 RTC 常常还没对时（没有备份电池，读到 2000 年附近），
             * 那时 reminder_sched 不会挂闹钟；等网络对时或 NSH 里 date -s 之后，
             * 靠这里补上。平时是空操作。 */
            if (++reminder_tick >= 30) {
                reminder_tick = 0;
                reminder_sched_tick();
            }
        }

        /* 录音攒满 10 秒（VOICE_PCM_MAX_BYTES）：在 LVGL 线程里收尾。
         * 录音线程只负责打标记和丢数据，停设备由这里做（它不能自己拆自己）。 */
        if (g_voice_pcm_full && touch_ui_voice_chat_active() &&
            audio_is_recording(&g_audio_ctx)) {
            audio_record_stop(&g_audio_ctx);
            touch_ui_voice_chat_stop_timer();
            touch_ui_set_voice_status("已录满 10 秒\n请点「提交」");
            printf("[VoiceChat] 录音到 10 秒上限，已自动停录音\n");

            /* 麦克风已经关了，状态栏别再显示「聆听中」 */
            voice_ui_idle();
        }

        /* 运行 AI 模块 */
        if (g_ai_initialized) {
            sm_run(&g_sm_ctx);

#if 0 /* [缺文件临时隔离] ai_checkin_tick() / ai_checkin_snapshot() 轮询 */
            /* 运行关怀确认状态机 */
            ai_checkin_tick(lv_tick_get());

            /* 轮询 checkin 状态并更新 UI */
            checkin_snapshot_t snap = ai_checkin_snapshot();
            static checkin_state_t last_checkin_state = CHECKIN_IDLE;
            if (snap.state != last_checkin_state) {
                last_checkin_state = snap.state;
                switch (snap.state) {
                    case CHECKIN_SENDING:
                        touch_ui_update_checkin_state(TOUCH_CHECKIN_SENDING);
                        break;
                    case CHECKIN_SENT:
                        touch_ui_update_checkin_state(TOUCH_CHECKIN_SENT);
                        /* 通知成功后 2 秒自动关闭面板 */
                        /* TODO: 用 lv_timer 延迟关闭 */
                        break;
                    case CHECKIN_FAILED:
                        touch_ui_update_checkin_state(TOUCH_CHECKIN_FAILED);
                        break;
                    case CHECKIN_IDLE:
                        /* 确认流程结束，隐藏面板 */
                        touch_ui_hide_checkin();
                        break;
                    default:
                        break;
                }
            }
#endif
        }

        usleep(5000); // 5ms 刷新周期
    }

    return 0;
}
