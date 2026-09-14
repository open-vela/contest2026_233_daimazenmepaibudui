/**
 * main.c - 智爱陪伴应用入口
 * 在 openvela 中运行
 */

#include <nuttx/config.h>
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
#include <pthread.h>

/* 语音后端（小米 MiMo，实现在 app/hello_app/mimo_voice.c）。
 * voice_asr/voice_tts 是 ai_agent 包的分发层，头文件路径由 CMakeLists.txt
 * 的 INCLUDE_DIRECTORIES 提供（与 app/hello_app/CMakeLists.txt 同一写法）。 */
#include "mimo_voice.h"
#include "voice/voice_asr.h"
#include "voice/voice_tts.h"

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
/* TTS 输出缓冲：与 ai_audio 的播放缓冲 AUDIO_PLAY_BUFFER_MS（8 秒 =
 * 256000 字节）对齐 —— 再大也没用，audio_play_start() 塞不下只会返回
 * -ENOSPC 一声不响。 */
#define VOICE_TTS_BUF_BYTES   (16000 * 2 * 8)
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

/* 播放完成回调（在 audio 的播放线程里跑）。
 * 播放要持续好几秒，期间用户可能关窗甚至又开了一轮；用放音时记下的世代号
 * 一比就知道这条"播放完成"该不该写进界面（同一个时刻只可能有一段在放，
 * audio_play_start() 对第二段会返回 -EBUSY，所以这个静态量不会串台）。 */
static void voice_play_complete_callback(void *user_data)
{
    (void)user_data;

    voice_ui_idle();

    if (g_voice_playing_gen != touch_ui_voice_chat_generation()) {
        return;
    }

    touch_ui_set_voice_status("回复完成\n点「再说一次」继续，或按 × 关闭");
    touch_ui_voice_chat_round_done();
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
        voice_ui_idle();
        touch_ui_voice_chat_round_done();
    } else {
        /* 到这里才真的在出声，之前一直显示的是"正在合成语音…"。
         * 状态栏跟着改成"说话中"（扬声器在响，和"聆听中"是两回事）。 */
        voice_task_show(task, "正在播放…");
        robot_ui_set_status(ROBOT_STATUS_SPEAKING);
        robot_ui_set_face(ROBOT_FACE_HAPPY);
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
        touch_ui_voice_chat_round_done();
        goto out;
    }

    /* 拿到回复：合成 + 播放（播放成功由 voice_play_complete_callback 收尾） */
    voice_speak_reply(task, reply);

out:
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
}

/* ==================== AI 命令回调处理 ==================== */

/**
 * 处理来自手机端或云端的 AI 命令
 * 成员二的 AI 模块可以通过此接口接收控制命令
 */
static void on_ai_command_received(const char *action, const char *param)
{
    printf("AI command: action=%s, param=%s\n", action, param);

    if (strcmp(action, "start_voice") == 0) {
        /* 开始语音监听 */
        robot_ui_set_status(ROBOT_STATUS_LISTENING);
        robot_ui_set_face(ROBOT_FACE_THINKING);
        robot_ui_set_ai_reply("聆听中...\n请说话。");

        /* 调用成员二的 AI 模块开始录音 */
        if (g_ai_initialized) {
            audio_record_start(&g_audio_ctx, NULL);
            sm_handle_event(&g_sm_ctx, SM_EVENT_WAKEUP);
        }
    }
    else if (strcmp(action, "stop_voice") == 0) {
        /* 停止语音监听 */
        robot_ui_set_status(ROBOT_STATUS_IDLE);
        robot_ui_set_face(ROBOT_FACE_HAPPY);

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

        robot_ui_set_status(ROBOT_STATUS_SPEAKING);
        robot_ui_set_face(ROBOT_FACE_HAPPY);
        sanitize_for_display(param, shown, sizeof(shown));
        robot_ui_set_ai_reply(shown);

        /* 播放语音回复 */
        if (g_ai_initialized) {
            audio_play_start(&g_audio_ctx, NULL, 0, NULL, NULL);
        }
    }
    else if (strcmp(action, "start_remind") == 0) {
        /* 开始提醒 */
        robot_ui_set_status(ROBOT_STATUS_REMINDING);
        robot_ui_set_face(ROBOT_FACE_WORRIED);
        robot_ui_show_reminder("提醒", param);
    }
    else if (strcmp(action, "start_alarm") == 0) {
        /* 开始报警 */
        robot_ui_show_alarm(param);

        /* 触发报警 */
        if (g_ai_initialized) {
            sm_handle_event(&g_sm_ctx, SM_EVENT_ALARM_DETECTED);
            report_alarm("sound_abnormal", param);
        }
    }
    else if (strcmp(action, "stop_alarm") == 0) {
        /* 停止报警 */
        robot_ui_close_alarm();

        if (g_ai_initialized) {
            sm_handle_event(&g_sm_ctx, SM_EVENT_ALARM_CLEARED);
        }
    }
    else if (strcmp(action, "set_face") == 0) {
        /* 设置表情 */
        if (strcmp(param, "happy") == 0) {
            robot_ui_set_face(ROBOT_FACE_HAPPY);
        } else if (strcmp(param, "thinking") == 0) {
            robot_ui_set_face(ROBOT_FACE_THINKING);
        } else if (strcmp(param, "sleepy") == 0) {
            robot_ui_set_face(ROBOT_FACE_SLEEPY);
        } else if (strcmp(param, "surprised") == 0) {
            robot_ui_set_face(ROBOT_FACE_SURPRISED);
        } else if (strcmp(param, "worried") == 0) {
            robot_ui_set_face(ROBOT_FACE_WORRIED);
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
 */
static void sound_alarm_callback(sound_type_t type, float confidence, void *user_data)
{
    const char *type_name = sound_detect_get_type_name(type);
    printf("[SoundDetect] Alarm: %s (confidence: %.2f)\n", type_name, confidence);

    /* 触发报警 */
    robot_ui_show_alarm(type_name);
    report_alarm(type_name, "Abnormal sound detected");

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
 * care_remind_callback 不直接操作 LVGL，通过 lv_async_call 投递到 UI 线程
 */
static void care_remind_callback(care_type_t type, const char *message, void *user_data)
{
    const char *type_name = care_get_type_name(type);
    printf("[Care] Reminder: %s - %s\n", type_name, message);

    /* 显示提醒（robot_ui_* 内部已有线程安全机制） */
    robot_ui_set_status(ROBOT_STATUS_REMINDING);
    robot_ui_set_face(ROBOT_FACE_WORRIED);
    robot_ui_show_reminder(type_name, message);

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
            lv_async_call(show_checkin_async, arg);
        }
    } else {
        printf("[Checkin] Failed to start\n");
    }
#endif
}

/**
 * 语音聊天回调 - 由语音聊天弹窗（touch_ui_show_voice_chat）调用
 * 在 LVGL 线程中执行：开始录音，音频由 voice_record_callback 攒进堆缓冲
 */
static void voice_chat_handler(void *user_data)
{
    audio_context_t *ctx = (audio_context_t *)user_data;
    audio_record_config_t rec_cfg;
    int ret;

    printf("[VoiceChat] 开始录音\n");

    robot_ui_set_status(ROBOT_STATUS_LISTENING);
    robot_ui_set_face(ROBOT_FACE_THINKING);
    robot_ui_set_ai_reply("聆听中...\n请说话。");

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

    /* 麦克风是独占的（半双工、单设备）：如果别处已经开着录音
     * （MQTT 下发的 start_voice、或别的手动路径），先把它停掉 ——
     * 用户是主动点开语音聊天窗的，这一轮该用麦克风。 */
    if (audio_is_recording(ctx)) {
        printf("[VoiceChat] 麦克风被别的路径占着，先停掉\n");
        audio_record_stop(ctx);
    }

    /* 录音数据全交给 voice_record_callback 攒起来。VAD 不在这里开：
     * 一问一答由用户按「提交」决定说完没有，不等静音超时。 */
    memset(&rec_cfg, 0, sizeof(rec_cfg));
    rec_cfg.enable_vad = false;
    rec_cfg.data_callback = voice_record_callback;
    rec_cfg.user_data = NULL;

    ret = audio_record_start(ctx, &rec_cfg);
    if (ret == 0) {
        sm_handle_event(&g_sm_ctx, SM_EVENT_WAKEUP);
        touch_ui_set_voice_status("正在录音…\n说完点「提交」");
    } else {
        printf("[VoiceChat] audio_record_start failed: %d\n", ret);
        robot_ui_set_status(ROBOT_STATUS_IDLE);
        robot_ui_set_face(ROBOT_FACE_HAPPY);
        robot_ui_set_ai_reply("录音启动失败\n请重试");
        touch_ui_set_voice_status("录音启动失败\n请关掉重开");
        touch_ui_voice_chat_round_done();
    }
}

/**
 * 紧急呼叫回调 - 由触摸菜单 "紧急呼叫" 确认后触发
 * 发送 MQTT 报警 + 手机推送通知
 */
static void emergency_call_handler(void *user_data)
{
    printf("[Emergency] Sending emergency alarm\n");

    /* MQTT 上报 */
    report_alarm("emergency", "老人按下紧急呼叫按钮");

    /* 手机推送 */
    push_send_alarm("emergency", "老人按下紧急呼叫按钮，请立即查看！");

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

/* ==================== MQTT 消息回调处理 ==================== */

/**
 * 处理来自 MQTT 的消息
 * 解析命令并转发给 AI 命令处理函数
 */
static void on_mqtt_message_received(const char *topic, const char *payload)
{
    printf("MQTT received: topic=%s\n", topic);

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
 *     → reminder_fire_async() 在 LVGL 线程里弹窗、响提示音、推送到手机
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

/* 提醒提示音：三声"嘀"（方波），不依赖任何素材文件。
 * 只在"没在录音、也没在放音"时响：audio_play_start() 内部会把半双工的
 * 麦克风切掉（播完再恢复），正在跟机器人说话时插一段提示音会打断录音。 */
#define REMINDER_BEEP_RATE     16000
#define REMINDER_BEEP_HZ       1000
#define REMINDER_BEEP_MS       200     /* 每声多长 */
#define REMINDER_BEEP_GAP_MS   150     /* 声与声之间的静音 */
#define REMINDER_BEEP_TIMES    3
#define REMINDER_BEEP_AMPLITUDE 5000   /* 约 15% 满量程，够听见又不刺耳 */

static void reminder_play_beep(audio_context_t *ctx)
{
    const size_t half  = (size_t)(REMINDER_BEEP_RATE / (REMINDER_BEEP_HZ * 2));
    const size_t unit  = (size_t)(REMINDER_BEEP_RATE * REMINDER_BEEP_MS / 1000);
    const size_t gap   = (size_t)(REMINDER_BEEP_RATE * REMINDER_BEEP_GAP_MS / 1000);
    const size_t total = (unit + gap) * REMINDER_BEEP_TIMES;
    int16_t *buf;
    int ret;

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
        size_t t = i % (unit + gap);

        if (t < unit) {
            buf[i] = ((i / half) & 1) ? REMINDER_BEEP_AMPLITUDE
                                      : -REMINDER_BEEP_AMPLITUDE;
        } else {
            buf[i] = 0;
        }
    }

    /* audio_play_start() 内部会先把数据 memcpy 进播放缓冲，所以这里
     * 返回后就能 free；实际出声由它的播放线程完成，本线程不等。 */
    ret = audio_play_start(ctx, buf, total, NULL, NULL);
    free(buf);

    if (ret != 0) {
        printf("[Reminder] 提示音播放失败: %d\n", ret);
    }
}

/* 到点（LVGL 线程）：弹窗 + 响一声 + 推到手机 */
static void reminder_fire_async(void *arg)
{
    reminder_fire_msg_t *msg = (reminder_fire_msg_t *)arg;
    char content[160];

    snprintf(content, sizeof(content), "%s  %02d:%02d",
             msg->titles, msg->hour, msg->min);

    printf("[Reminder] 到点提醒: %s\n", content);

    /* 弹窗（robot_ui_show_reminder 是现成的提醒弹窗，顶部图层，盖住整屏） */
    robot_ui_show_reminder("提醒", content);

    if (g_ai_initialized) {
        reminder_play_beep(&g_audio_ctx);
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

    if (lv_async_call(reminder_fire_async, msg) != LV_RESULT_OK) {
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

    printf("ZhiAi Companion started!\n");

    /* 主循环 */
    while (1) {
        static int  net_tick = 0;
        static bool net_ok   = false;
        static int  time_tick = 0;
        static int  reminder_tick = 0;

        lvgl_timer_handler();

        /* 每 ~200ms 刷新一次状态栏上的网络状态。
         * LVGL 不是线程安全的，所以只在这个任务里改控件；network_task 那边
         * 只维护 mqtt_config.connected，由这里轮询。
         */
        if (++net_tick >= 40) {
            bool ok = mqtt_is_connected();

            net_tick = 0;
            if (ok != net_ok) {
                net_ok = ok;
                robot_ui_set_net_status(ok ? "NET OK" : "NET --");
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
