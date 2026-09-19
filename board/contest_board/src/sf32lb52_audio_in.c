/****************************************************************************
 * board/contest_board/src/sf32lb52_audio_in.c
 *
 * SF32LB52 录音通路封装（麦克风输入）
 *
 * 做四件事：start（open + CONFIGURE + START）、read、stop（STOP + close）、
 * abandon（只 close 自己的 fd，**不发 STOP** —— 用在"这次会话不是被本层停的"
 * 那类收尾路径上，见 sf32lb52_audio_in.h 里 abandon 与 stop 的分工）。
 * 内部用一个全局 fd 表示"当前这次录音会话"，用一把 nxmutex 保护 fd 与状态。
 *
 * 线程约定（重要）：
 *   - audio_in_read() 是阻塞的，**读的时候不持锁**，只短暂取锁拿 fd 快照。
 *     否则"一个任务在读、另一个任务调 audio_in_stop() 发 STOP 救场"的
 *     经典用法会直接死锁（stop 拿不到锁 -> 发不出 STOP -> read 永远不返回）。
 *   - 只有 fd 的创建/销毁（start/stop）和状态读写在锁里。
 *
 * 录音参数固定 16 kHz / 单声道 / 16bit：这是驱动与本板麦克风一路验证过的组合。
 * 详细说明与完整示例见 docs/audio_driver_usage.md 第 9 节。
 *
 * 残留会话自愈（重要）：
 *   单一大镜像意味着 g_audio_in_fd 是所有 app 共享的一份全局。某个 app 录音
 *   中途挂掉/退出时没人替它清这份状态，后面的 app 就会一直拿到 -16（EBUSY）。
 *   所以除了 fd 还记下"开它的线程 id + 它所在的任务组"，audio_in_start() 在 EBUSY
 *   时会先确认持有者线程是否还存在；线程没了还要再看那个任务组 —— 只有"是本组
 *   自己的残局"或"整个 app 都没了"才算残留，丢掉再重开一次。"线程死了但那个 app
 *   还活着"不算（那是对面自己把会话留在原地，抢过来会让同一台半双工设备上出现
 *   两个 read 客户端），详见 audio_in_start() 里那段注释。
 *   另一层在驱动里：AUDIOIOC_START 返回的 -EBUSY 现在只在"反方向那条通路的持有者
 *   线程确实还活着"时才出现（持有者已消失的残留由驱动自己收干净），板级遇到
 *   -EBUSY 只做一次有界重试、绝不去停别人的通路 —— 完整策略见
 *   sf32lb52_audio_in.h 的「遇 EBUSY 怎么办」。
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/audio/audio.h>
#include <nuttx/mutex.h>
#include <nuttx/sched.h>
#include <nuttx/clock.h>          /* clock_systime_ticks()/MSEC2TICK()：空转日志限频 */

#include "sf32lb52_audio_in.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 驱动 codec 时钟表里有的档位（sf32lb52_audio.c:167-186 的
 * codec_dac_clk_config / codec_adc_clk_config）。
 * 其余采样率驱动会在 CONFIGURE 时返回 -EINVAL，这里提前拦掉。 */

#define AUDIO_IN_RATE_8K      8000
#define AUDIO_IN_RATE_16K     16000
#define AUDIO_IN_RATE_44K1    44100
#define AUDIO_IN_RATE_48K     48000

/* "stop 空转"日志的限频窗口（见 audio_in_stop()）。stop() 是高频路径，而空转
 * 一旦发生就会每次调用都发生，不限频会把串口刷满、反而冲掉有用的上下文。 */

#define AUDIO_IN_STOP_SPIN_LOG_MS  3000

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 当前录音会话的 fd，-1 = 未打开 */

static int  g_audio_in_fd = -1;

/* 开这次会话的线程 id（nxsched_gettid()）。
 *
 * 为什么需要它：整机是单一大镜像，这份全局被**所有 app 共享**，某个 app
 * 录音中途挂掉/退出时既不会调 audio_in_stop()，也不会有人替它清 g_audio_in_fd，
 * 于是后面每个 app 都拿到 -16（EBUSY）、而 ps 里已经找不到那个 app 了。
 * 记下持有者之后，start() 就能分辨"别人真在录"和"死人留下的残局"。 */

static pid_t g_audio_in_owner = (pid_t)-1;

/* 开这次会话的**任务组** id（getpid()：NuttX 里 pthread 拿到的是本组组长的 pid）。
 *
 * 为什么还要记组号：fd 号是**每个 task group 各一套**的。整机是单一大镜像，这份
 * 全局被所有 app 共享，于是"另一个 app 的线程"完全可能读到这个 fd 号并拿去
 * ioctl/close —— 那个数字在它自己的组里指的是**另一个 file 对象**。实测真机事故
 * 就是这条路径：robot_ui 的播报线程替 hello_app 调 audio_in_stop()，结果
 * ioctl(AUDIOIOC_STOP) 问到了 robot_ui 组里另一个东西上，inode 上没有 ioctl
 * （VFS 返回 -ENOTTY=25），STOP 没生效、hello_app 的录音线程还阻塞在 read() 里；
 * 紧接着的 close(fd) 关掉的也是 robot_ui 自己的某个无关 fd。
 *
 * 注意判据不能停在"组号不同就拒绝"：task_create() 出来的子任务（hw_test 的读任务
 * 就是）会复制父任务的 fd 表，它的同号 fd 指向同一个 file 对象，对音频设备完全
 * 有效。所以"能不能动这个 fd"真正靠的是下面那个身份探测
 * （audio_in_fd_is_audio_device）。组号另外还有一个用途：判定"残留会话"时要看
 * 持有者那个组是否还在 —— 线程没了、组还在、又不是本组，说明对面 app 活着，
 * 不抢（见 audio_in_start() 里那段）。 */

static pid_t g_audio_in_owner_group = (pid_t)-1;

/* fd 与状态的保护锁。只保护 start/stop 的临界区，不覆盖阻塞的 read。 */

static mutex_t g_audio_in_lock = NXMUTEX_INITIALIZER;

/* "有线程正在收这一次会话"。收尾在锁外做（ioctl/close 都不能持锁），所以用一个
 * 标志防并发：两个线程同时看到同一个 fd 各自 close 一次，第二次关掉的可能是刚被
 * 别人复用的槽位。**stop() 与 abandon() 共用这一把收尾权** —— 谁先拿到标志谁负责
 * 关那一次 fd，后到的那个直接返回（abandon 返回 -ENOENT，stop 返回 OK），
 * 否则"一方发 STOP、另一方 close"就会各自关一次同一个 fd。
 * 配着全局一起看；标志带线程 id，好让"收到一半线程就没了"留下的残留能被下一次
 * 调用识别并清掉（和 start() 里那套"持有者还在不在"的自愈判据同一个套路）。 */

static bool  g_audio_in_stop_pending;
static pid_t g_audio_in_stop_owner = (pid_t)-1;

/* "stop 空转"日志的下一次可打点时刻（clock_systime_ticks()）。只在
 * audio_in_stop() 里"已有别的线程在收尾"那条分支用；0 = 还没打过，
 * 所以第一次空转必然留下一条日志。 */

static clock_t g_audio_in_stop_spin_log_next;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/**
 * @brief  采样率是否是驱动支持的档位
 */

static bool audio_in_rate_supported(int sample_rate)
{
  return sample_rate == AUDIO_IN_RATE_8K  ||
         sample_rate == AUDIO_IN_RATE_16K ||
         sample_rate == AUDIO_IN_RATE_44K1 ||
         sample_rate == AUDIO_IN_RATE_48K;
}

/**
 * @brief  fd 解析到的到底是不是本板音频设备
 *
 * 用 GETCAPS(QUERY) 探测：这条命令**只读不写**（驱动侧只填 caps 结构，不改任何
 * 状态、不动寄存器），问别的东西也不会造成副作用。音频设备一定会答"支持输入
 * （麦克风）"；普通文件、别的设备要么直接失败（-ENOTTY / -EBADF），要么答不上来。
 *
 * 为什么要这层探测而不是盲目发 STOP：拿一个不属于本组的 fd 号去发音频命令，
 * 命令号会落到**那个设备自己的 ioctl 处理函数**上（这正是本次事故的形态）。
 */

static bool audio_in_fd_is_audio_device(int fd)
{
  struct audio_caps_s caps;

  memset(&caps, 0, sizeof(caps));
  caps.ac_len     = sizeof(struct audio_caps_s);
  caps.ac_type    = AUDIO_TYPE_QUERY;
  caps.ac_subtype = AUDIO_TYPE_QUERY;

  if (ioctl(fd, AUDIOIOC_GETCAPS, (unsigned long)&caps) < 0)
    {
      return false;
    }

  return caps.ac_len >= sizeof(struct audio_caps_s) &&
         (caps.ac_controls.b[0] & AUDIO_TYPE_INPUT) != 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: audio_in_start
 ****************************************************************************/

int audio_in_start(int sample_rate, int channels, int bits)
{
  struct audio_caps_desc_s capdesc;
  FAR struct tcb_s *owner_tcb;
  pid_t self;
  int fd;

  /* 参数校验：不做隐式纠正，非法就直接拒绝，免得录出格式不对的数据 */

  if (!audio_in_rate_supported(sample_rate) ||
      channels != 1 || bits != AUDIO_IN_DEFAULT_BITS)
    {
      syslog(LOG_ERR,
             "AUDIO_IN: bad params (rate=%d ch=%d bits=%d), "
             "only 8k/16k/44.1k/48k, mono, 16bit\n",
             sample_rate, channels, bits);
      return -EINVAL;
    }

  if (nxmutex_lock(&g_audio_in_lock) < 0)
    {
      return -EINVAL;                 /* 锁坏掉了，属于不可能的状态 */
    }

  /* 已经有会话：先分辨是哪一种"忙"，不能一律 -EBUSY。
   *
   * 分四种：
   *   1) 持有者就是当前线程 —— 同一线程里真的在录，如实返回 -EBUSY；
   *   2) 持有者线程还在（nxsched_get_tcb() 拿得到 TCB）—— 别人真在录，
   *      返回 -EBUSY，不打断；
   *   3) 持有者线程已经不在了，**但持有者所在的任务组还活着、而且不是本组** ——
   *      那不是"死掉的 app 留下的残局"，是那个 app 自己把会话留在了原地
   *      （它的录音线程退场时走了"只退出、不 stop 不 close"那条收尾，见
   *      ai_audio.c 的 stale 分支）。这时候抢过来就是同一个半双工设备上开
   *      **两个 read 客户端**：驱动里 priv 是单实例，rx_sem / rx_busy / DMA 武装
   *      都只有一份，"一次 read 一次武装一次 post"的配对就此散掉，真机上落在
   *      sem_waitirq.c:137 那条断言上。所以一律拒绝，返回 -EBUSY
   *      （下面第 4 种里的"本组"例外就是冲着这种情况去的：同一个 app 自己回收
   *      自己的残局仍然允许，那是这份记录存在的原意）；
   *   4) 持有者线程不存在，且（是本组的残局，或持有者整个任务组都没了）——
   *      死掉的 app 留下的残留（单一大镜像里这份全局没人替它清），
   *      清掉残留后照常重开一次。
   */

  self = nxsched_gettid();

  if (g_audio_in_fd >= 0)
    {
      if (g_audio_in_owner == self)
        {
          nxmutex_unlock(&g_audio_in_lock);
          syslog(LOG_WARNING,
                 "AUDIO_IN: 本线程 %d 已在录音，拒绝重入（-EBUSY）\n",
                 (int)self);
          return -EBUSY;
        }

      owner_tcb = nxsched_get_tcb(g_audio_in_owner);

      if (owner_tcb != NULL)
        {
          /* nxsched_get_tcb() 会加 TCB 引用计数，必须配对释放 */

          nxsched_put_tcb(owner_tcb);
          nxmutex_unlock(&g_audio_in_lock);
          syslog(LOG_WARNING,
                 "AUDIO_IN: 录音设备仍被线程 %d 占用（-EBUSY）\n",
                 (int)g_audio_in_owner);
          return -EBUSY;
        }

      /* 持有线程没了。**先别急着判残留**：查一下它那个任务组还在不在 ——
       * 线程没了、组还在、又不是本组，说明是对面那个 app 还活着，只是它的录音
       * 线程把会话留在了原地。这时把设备交出去，就是两个 read 客户端同开一台
       * 半双工设备：驱动那边 priv 是单实例，rx_sem / rx_busy / RX 的 DMA 武装
       * 都只有一份，"一次 read 一次武装一次 post"的配对就此散掉，真机上落在
       * sem_waitirq.c:137 那条断言上（驱动自己记过这是这套等待方式的已知风险）。
       * 组号相等（本组自己的残局）不拦，留着自愈。
       *
       * 为什么"组还在"就够当判据：NuttX 里组组长一退，整组线程都会被带走，
       * 所以"组的 TCB 查得到"等价于"那个 app 确实还在跑"。 */

      if (g_audio_in_owner_group != getpid())
        {
          FAR struct tcb_s *owner_group_tcb =
            nxsched_get_tcb(g_audio_in_owner_group);

          if (owner_group_tcb != NULL)
            {
              nxsched_put_tcb(owner_group_tcb);
              nxmutex_unlock(&g_audio_in_lock);
              syslog(LOG_WARNING,
                     "AUDIO_IN: 持有线程 %d 已消失，但它的任务组 %d 还活着且不是"
                     "本组（group %d）—— 这是那个 app 自己把会话留在了原地，"
                     "不是死掉 app 的残留；拒绝抢占（-EBUSY）\n",
                     (int)g_audio_in_owner, (int)g_audio_in_owner_group,
                     (int)getpid());
              return -EBUSY;
            }
        }

      syslog(LOG_WARNING,
             "AUDIO_IN: 持有线程 %d 已消失，判定为残留录音会话（fd=%d），"
             "丢弃后重开一次\n",
             (int)g_audio_in_owner, g_audio_in_fd);

      /* **只清全局状态，不碰那个 fd 号**：fd 号是线程自己 fd 表里的槽位，
       * 线程死了它的 fd 早被内层回收，这个数字随时可能被当前任务里另一个
       * 打开的文件占着 —— 对它做 close()/ioctl() 会误伤别人，所以这里
       * 一个系统调用都不发，把收尾交给下面的完整 open + START：
       * 驱动侧 sf32lb52_audio_start() 会先做一次干净 stop 再真启动。 */

      g_audio_in_fd          = -1;
      g_audio_in_owner       = (pid_t)-1;
      g_audio_in_owner_group = (pid_t)-1;
    }

  /* 收尾权标志的残留自愈：它和上面那份会话记录是一对，会话记录被判成残留清掉
   * 之后，它可能还停在真值上 —— 典型是拿着收尾权的那个线程在阻塞的 ioctl/close
   * 里被删掉了/所属 app 直接退了，标志就成了没人能清的残留。
   * 残留着它的后果很重：之后每一次 audio_in_stop() 都会因为"已有线程在收尾"而
   * **空转返回 OK**（调用方也都不看返回值），于是会话永远停不下、fd 永不 close；
   * 而 tid 一旦被新线程复用，那次查活就会一直误判成"人还在"，连自愈都做不到。
   *
   * 判据就是文件里另外两处自愈（audio_in_stop() / audio_in_abandon() 开头的
   * "记下线程 id、用 nxsched_get_tcb() 查活"）那套，不另造。**只有查不到 TCB 才
   * 清**：查得到说明那人正拿着收尾权在动那个 fd，这个标志是"别重复关 fd"的唯一
   * 凭据，必须留着，本函数不去替它清 —— 那要么由它自己收尾时清掉，要么等它没了
   * 由下一次 stop()/abandon() 用同一套判据清。
   * 清掉是安全的：已经存在的线程才可能再走到 close(fd)，一个查不到 TCB 的线程
   * 既不会再关那个 fd，也不会把标志置回真值 ——
   * "谁先拿到收尾权谁负责关那一次 fd"的语义一点没动。 */

  if (g_audio_in_stop_pending)
    {
      FAR struct tcb_s *stopper = nxsched_get_tcb(g_audio_in_stop_owner);

      if (stopper != NULL)
        {
          nxsched_put_tcb(stopper);
        }
      else
        {
          syslog(LOG_WARNING,
                 "AUDIO_IN: 上一次收尾的线程 %d 已消失，清掉残留的 stop 标志"
                 "（否则之后的 stop() 会一直空转）\n",
                 (int)g_audio_in_stop_owner);
          g_audio_in_stop_pending = false;
          g_audio_in_stop_owner   = (pid_t)-1;
        }
    }

  /* 走到这里只有两种可能：本来没有会话，或刚把残留清掉。
   * 两种情况都完整走一遍 open + CONFIGURE + START。 */

  fd = open(AUDIO_IN_DEV, O_RDONLY);
  if (fd < 0)
    {
      int errcode = errno;

      nxmutex_unlock(&g_audio_in_lock);
      syslog(LOG_ERR, "AUDIO_IN: open %s failed: %d\n",
             AUDIO_IN_DEV, errcode);
      return -errcode;
    }

  memset(&capdesc, 0, sizeof(capdesc));
  capdesc.caps.ac_len            = sizeof(struct audio_caps_s);
  capdesc.caps.ac_type           = AUDIO_TYPE_INPUT;
  capdesc.caps.ac_channels       = (uint8_t)channels;
  capdesc.caps.ac_controls.hw[0] = (uint16_t)sample_rate;
  capdesc.caps.ac_controls.b[2]  = (uint8_t)bits;

  if (ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&capdesc) < 0)
    {
      int errcode = errno;

      close(fd);
      nxmutex_unlock(&g_audio_in_lock);
      syslog(LOG_ERR, "AUDIO_IN: AUDIOIOC_CONFIGURE failed: %d\n", errcode);
      return -errcode;
    }

  if (ioctl(fd, AUDIOIOC_START, 0) < 0)
    {
      int  errcode = errno;
      bool started = false;

      /* -EBUSY 的一次性兜底。策略与边界写在 sf32lb52_audio_in.h 的
       * 「遇 EBUSY 怎么办」那一段，这里的实现只做三件有界的事：看错误码是不是
       * EBUSY、是就重试一次、失败就如实返回 —— 不循环、不 sleep、不发 AUDIOIOC_STOP。
       *
       * 为什么板级**不**做"先停再开"：驱动侧现在自己会分辨"反方向那条通路到底是
       * 别人真在用，还是持有者早就死了的残局"（残留由驱动在里面收干净，这次 START
       * 会直接成功）。所以 -EBUSY 透到板级时，驱动判定对面有活人。而板级到这一步
       * **没有任何证据**能推翻它：本模块自己那份会话记录（g_audio_in_fd/
       * g_audio_in_owner）在"有会话"的两种情况下都在函数开头就返回或清掉了，
       * 能走到这里它一定是 -1。既然证不出来就绝不去停别人的通路 ——
       * AUDIOIOC_STOP 会同时停掉录和放，停在别人正在出声的播报上就是打断它，
       * 半双工的铁律不允许这么赌。
       *
       * 重试只有一次，接的是"上一拍的占用刚好在这一拍收尾"（对面的持有者在这两次
       * 调用之间死了，或者它的 STOP 刚到）。重试合法：START 失败时上层状态还是
       * PREPARED —— nuttx/audio/audio.c 的 audio_start() 只在成功时才
       * audio_setstate(RUNNING)，同一个 fd 可以再发一次 AUDIOIOC_START。 */

      if (errcode == EBUSY)
        {
          if (ioctl(fd, AUDIOIOC_START, 0) == 0)
            {
              started = true;
              syslog(LOG_WARNING,
                     "AUDIO_IN: AUDIOIOC_START 第一次 -EBUSY，重试一次成功"
                     "（对面的占用在这两拍之间收尾了）\n");
            }
          else
            {
              errcode = errno;
              syslog(LOG_ERR,
                     "AUDIO_IN: AUDIOIOC_START 连续两次失败（第一次 -EBUSY = 驱动"
                     "判定反方向通路仍有活着持有者；重试后 errno=%d）。设备确实被"
                     "另一方向的会话占着：本次不打断、不停止，如实返回 %d\n",
                     errcode, -errcode);
            }
        }
      else
        {
          syslog(LOG_ERR,
                 "AUDIO_IN: AUDIOIOC_START failed: %d（不是 EBUSY，不重试）\n",
                 errcode);
        }

      if (!started)
        {
          close(fd);
          nxmutex_unlock(&g_audio_in_lock);
          return -errcode;
        }
    }

  /* 最后一步才发布 fd：中途失败时全局状态始终是"未打开"，
   * 别人不会拿到一个半配置好的会话。持有者线程 + 持有者任务组 + fd 一起发布，
   * 保证"fd >= 0"时这两个 id 一定是有效值（公开函数全靠它们判断能不能动 fd）。 */

  g_audio_in_fd          = fd;
  g_audio_in_owner       = self;
  g_audio_in_owner_group = getpid();

  nxmutex_unlock(&g_audio_in_lock);

  syslog(LOG_INFO, "AUDIO_IN: started (%d Hz, %d ch, %d bit)\n",
         sample_rate, channels, bits);
  return OK;
}

/****************************************************************************
 * Name: audio_in_read
 ****************************************************************************/

ssize_t audio_in_read(FAR void *buf, size_t len)
{
  int fd;
  ssize_t n;

  if (buf == NULL || len == 0)
    {
      return -EINVAL;
    }

  /* 只拿一个 fd 快照，**不持锁**做阻塞读：否则别的任务没法调
   * audio_in_stop() 来发 STOP 唤醒这里的 read()。 */

  if (nxmutex_lock(&g_audio_in_lock) < 0)
    {
      return -EINVAL;
    }

  fd = g_audio_in_fd;

  nxmutex_unlock(&g_audio_in_lock);

  if (fd < 0)
    {
      return -EINVAL;                 /* 没 start，或者已经被 stop 了 */
    }

  /* 这里**不**做跨组校验：fd 号只在开它的那个组里有意义，而 task_create() 出来的
   * 子任务（比如 hw_test 的读任务）会复制父任务的 fd 表（group_setuptaskfiles ->
   * fdlist_copy），它的同一个 fd 号指向**同一个 file 对象**，读本板音频设备完全
   * 有效。能读到什么由那个 file 对象决定，不是由组号决定，所以这里不做判断。 */

  n = read(fd, buf, len);

  /* 这里是**原样转发**，一个字都不吞：上层的判据全靠这个返回值。
   *
   * read() 的语义（本板音频设备 dev/audio/audio0）：
   *   正数 = 真的读到了这么多字节；
   *   0    = EOF：被 AUDIOIOC_STOP 打断 / 设备没在跑 / 会话换代 ——
   *          也就是驱动 sf32lb52_audio_read 里"这一代会话真的结束了"那三个判据；
   *   负值 = 错误或超时。其中"这一次没等到数据"是驱动特意区分出来的
   *          （驱动返回 -ETIMEDOUT，也就是那一次 5 秒上界用光），
   *          别的负值是真错误（没 start、fd 失效等）。
   *
   * 注意这里调的是 NuttX 的 POSIX read()：驱动返回的负值会被 libc 那层翻成
   * -1 并把真正的错误号放进 errno（fs/vfs/fs_read.c 的 readv()：先
   * set_errno(-ret)、再 return -1）。所以调用方要分"超时"和"别的错误"，
   * 看的是 errno == ETIMEDOUT，不是返回值本身；而且调用前得先把 errno 清 0，
   * 否则会读到上一次调用留下的旧值。本函数自己返回 -EINVAL 的那几条路
   * （没 start / buf 为空 / len 为 0）不碰 errno。 */

  return n;
}

/****************************************************************************
 * Name: audio_in_stop
 ****************************************************************************/

int audio_in_stop(void)
{
  pid_t owner;
  pid_t owner_group;
  int fd;
  int errcode;

  if (nxmutex_lock(&g_audio_in_lock) < 0)
    {
      return -EINVAL;
    }

  fd = g_audio_in_fd;

  if (fd < 0)
    {
      nxmutex_unlock(&g_audio_in_lock);
      return OK;                      /* 没在录音，安全空操作 */
    }

  if (g_audio_in_stop_pending)
    {
      /* 已经有一个线程正在停这次会话（全局还没清就说明它还没停完）。
       * 放行会让两个线程拿到同一个 fd 各 close 一次 —— 第二次 close 关掉的
       * 可能是这个槽位刚被别人复用的 fd，所以这里直接空转返回。
       *
       * 但要先确认那个线程还在：停的动作在锁外做（ioctl 不能持锁），万一它在
       * 中途被删/被杀，标志就永远是残留、别人再也停不掉这个会话了。判据沿用
       * start() 里那套"持有者线程还在不在"的做法，自愈。 */

      FAR struct tcb_s *stopper = nxsched_get_tcb(g_audio_in_stop_owner);
      pid_t   stopper_tid       = g_audio_in_stop_owner;
      clock_t now;

      if (stopper != NULL)
        {
          nxsched_put_tcb(stopper);
          nxmutex_unlock(&g_audio_in_lock);
          now = clock_systime_ticks();

          /* 空转返回 OK 这件事，调用方**看不出来**（返回值与"真停掉了"完全一样），
           * 所以这里留一条限频日志：真有线程卡在收尾里，就会持续留痕，而不是
           * 一声不响地报成功。限频是必须的 —— 空转一旦发生就是每次调用都发生，
           * 不限频会把串口刷满、反而冲掉有用的上下文。
           * **只加日志、不动返回值**：调用方现在把 OK 当"设备已停"，
           * 改返回码会牵动它们的语义（见头文件里"第三种 OK"那段）。 */

          if ((int32_t)(now - g_audio_in_stop_spin_log_next) >= 0)
            {
              g_audio_in_stop_spin_log_next =
                now + MSEC2TICK(AUDIO_IN_STOP_SPIN_LOG_MS);
              syslog(LOG_WARNING,
                     "AUDIO_IN: 本次 stop **空转**：线程 %d 仍在收尾这一次会话"
                     "（fd=%d），会话没被停、fd 没关，这里照旧返回 OK。"
                     "反复出现即说明那个线程卡在 ioctl/close 里回不来了\n",
                     (int)stopper_tid, fd);
            }

          return OK;
        }

      syslog(LOG_WARNING,
             "AUDIO_IN: 上一次停会话的线程 %d 已消失，清掉残留的 stop 标志\n",
             (int)g_audio_in_stop_owner);
      g_audio_in_stop_pending = false;
    }

  owner       = g_audio_in_owner;
  owner_group = g_audio_in_owner_group;
  g_audio_in_stop_pending = true;
  g_audio_in_stop_owner   = nxsched_gettid();

  nxmutex_unlock(&g_audio_in_lock);

  /* 跨组调用要先确认这个 fd 到底通到哪个设备，再决定动不动它。
   *
   * 为什么不能只看组号就拒绝：task_create() 出来的子任务（hw_test 的读任务就是）
   * 会复制父任务的 fd 表，它的同号 fd 指向**同一个 file 对象**，对本板音频设备
   * 完全有效 —— 一刀切拒绝会把这种合法用法也挡掉。
   * 为什么不能不看就发 STOP：fd 号只在开它的那个组里保证有意义。别组的同号 fd
   * 是另一个 file 对象，音频命令号会落到**那个设备自己的 ioctl 处理函数**上 ——
   * 真机事故（AUDIOIOC_STOP failed: 25）正是这么来的：命令问到了 robot_ui 组里
   * 另一个东西上，音频驱动压根没收到。
   * 所以这里先做一次只读探测（GETCAPS(QUERY)，不改任何状态），探测不通过就
   * 一个系统调用都不发、全局也不动。 */

  if (owner_group != getpid() && !audio_in_fd_is_audio_device(fd))
    {
      syslog(LOG_ERR,
             "AUDIO_IN: stop 被别的 task group 调用，且 fd=%d 在本组里不是音频设备"
             "（会话属于 group %d / 线程 %d，调用者是 group %d 线程 %d）：已拒绝，"
             "STOP 没发出去、fd 没关、全局状态没动。\n"
             "  说明：fd 号在每个 task group 里各有一套含义，跨组拿别人的 fd 号"
             "做 ioctl/close 只会打到本组自己的文件上（这就是 AUDIOIOC_STOP "
             "failed: 25 / ENOTTY 的来源）。跨 app 让路请改成"
             "\"登记请求 + 持有者自己的线程执行\"。\n",
             fd, (int)owner_group, (int)owner, (int)getpid(),
             (int)nxsched_gettid());

      if (nxmutex_lock(&g_audio_in_lock) == 0)
        {
          g_audio_in_stop_pending = false;
          g_audio_in_stop_owner   = (pid_t)-1;
          nxmutex_unlock(&g_audio_in_lock);
        }

      return -EPERM;
    }

  /* 顺序很关键：先 STOP（把阻塞在 read() 里的任务唤醒），再 close。
   * close() 只在"最后一个 fd"时才走到驱动的 shutdown()；本模块只在文件里
   * 用到 STOP + close（见 docs/audio_driver_usage.md 第 8 节）。 */

  if (ioctl(fd, AUDIOIOC_STOP, 0) < 0)
    {
      errcode = errno;

      /* 关键：这一步失败时**什么都不动** —— 不清全局、不 close。
       *
       * 为什么不能在失败后照样 close + 清全局（原来就是这么写的）：
       * fd 号只在"开它的那个 task group"里有意义，别组的同号 fd 是另一个
       * file 对象，把它 close 掉就是误伤别人的 fd；而全局一旦被清成"已停"，
       * 真正的持有者就丢了唯一的句柄 —— 它的设备可能还在跑，却再也停不掉了
       * （真机现象：AUDIOIOC_STOP failed: 25 之后紧接着打印 "stopped"，
       *  而 hello_app 的录音线程仍阻塞在 read() 里，300ms 收不了尾）。
       *
       * 下面这条按 errno 分诊，直接告诉看日志的人"这个 fd 是不是音频设备"：
       *   ENOTTY(25) / EBADF(9) → 这个 fd 号在本组里根本不是音频设备（别人的
       *   fd 号），STOP 压根没送到驱动；其他 errno → 是本组自己的设备，但
       *   驱动当时拒绝了这次 STOP。 */

      syslog(LOG_ERR,
             "AUDIO_IN: AUDIOIOC_STOP failed: %d（fd=%d，会话属于 group %d / "
             "线程 %d，调用者 group %d 线程 %d）%s"
             "全局状态与 fd 都没动：设备可能仍在跑，持有者可以重试\n",
             errcode, fd, (int)owner_group, (int)owner,
             (int)getpid(), (int)nxsched_gettid(),
             (errcode == ENOTTY || errcode == EBADF) ?
               "—— 这个 fd 号在本组里不是音频设备（很可能是别的 app 的 fd 号），"
               "STOP 没有送到音频驱动，hw_stop() 没执行，阻塞在 read() 里的线程"
               "也等不到唤醒。" :
               "—— 音频设备在，但这次 STOP 被拒了。");

      if (nxmutex_lock(&g_audio_in_lock) == 0)
        {
          g_audio_in_stop_pending = false;
          g_audio_in_stop_owner   = (pid_t)-1;
          nxmutex_unlock(&g_audio_in_lock);
        }

      return -errcode;
    }

  /* STOP 成功才敢认定"这个 fd 就是音频设备"（也可能是 task_create 复制出来的
   * 同一个 file 对象，两者 close 掉都是安全的：file 是引用计数的）。
   * 这时候才清全局、才 close —— 而且只在"会话还是刚才那一个"时清，
   * 免得把期间新开的一次会话顶掉。 */

  if (nxmutex_lock(&g_audio_in_lock) == 0)
    {
      if (g_audio_in_fd == fd)
        {
          g_audio_in_fd          = -1;
          g_audio_in_owner       = (pid_t)-1;
          g_audio_in_owner_group = (pid_t)-1;
        }

      g_audio_in_stop_pending = false;
      g_audio_in_stop_owner   = (pid_t)-1;
      nxmutex_unlock(&g_audio_in_lock);
    }

  close(fd);

  if (owner_group != getpid())
    {
      /* 跨组的合法调用（task_create 复制的 fd 表）：事后必须能一眼看到，
       * 因为"谁在停别人的会话"正是本次要根治的那件事。 */

      syslog(LOG_WARNING,
             "AUDIO_IN: stopped（由别的 task group %d 代停：会话 group=%d / "
             "线程 %d）\n",
             (int)getpid(), (int)owner_group, (int)owner);
      return OK;
    }

  syslog(LOG_INFO, "AUDIO_IN: stopped\n");
  return OK;
}

/****************************************************************************
 * Name: audio_in_abandon
 *
 * Description:
 *   放弃当前录音会话：清掉本层的记录 + close 掉自己的 fd。除跨组时那一遍只读的
 *   身份探测（GETCAPS）之外，**一个 ioctl 都不发，尤其不发 AUDIOIOC_STOP**。
 *   契约、返回值和"什么时候该用 abandon、什么时候必须用 stop"见
 *   sf32lb52_audio_in.h 里 audio_in_abandon 那一段。
 *
 *   为什么不能直接复用 audio_in_stop()：它中间那句 ioctl(AUDIOIOC_STOP) 是
 *   **设备级**的（同时停播放和录音）。而本函数服务的是"这一次会话根本不是本层
 *   停的"那类收尾路径（audio_in_read() 返回 0/EOF），此时设备要么已经停了、
 *   要么已经被别人的会话接手 —— 再发一个设备级 STOP 就是把刚接手的那一方打死，
 *   单方向让路由此变成双向拆台。
 *
 *   为什么"只 close 自己的 fd"不会把设备锁死：见头文件里那一节（最后一个 file
 *   对象被关时 NuttX 音频上层会调驱动的 shutdown，那里会把 running 清掉）。
 ****************************************************************************/

int audio_in_abandon(void)
{
  pid_t owner;
  pid_t owner_group;
  int fd;

  if (nxmutex_lock(&g_audio_in_lock) < 0)
    {
      return -EINVAL;                 /* 锁坏掉了，属于不可能的状态 */
    }

  fd = g_audio_in_fd;

  if (fd < 0)
    {
      nxmutex_unlock(&g_audio_in_lock);

      /* 本来就没有会话。幂等：重复调、没 start 就调，都是这个返回值，
       * 不动任何东西（这正是调用方想要的"已经干净了"）。 */

      return -ENOENT;
    }

  if (g_audio_in_stop_pending)
    {
      /* 已经有线程拿到收尾权（stop() 或另一次 abandon()）在收这一次会话。
       * 放行会让两个线程各 close 一次同一个 fd，所以这里直接返回 ——
       * 对调用方来说"会话已经在被收走了"，语义上和本层没有会话是一回事。 */

      FAR struct tcb_s *stopper = nxsched_get_tcb(g_audio_in_stop_owner);

      if (stopper != NULL)
        {
          nxsched_put_tcb(stopper);
          nxmutex_unlock(&g_audio_in_lock);
          syslog(LOG_INFO,
                 "AUDIO_IN: abandon 时线程 %d 正在收这一次会话，本次不重复关 fd\n",
                 (int)g_audio_in_stop_owner);
          return -ENOENT;
        }

      syslog(LOG_WARNING,
             "AUDIO_IN: 上一次收会话的线程 %d 已消失，清掉残留的收尾标志\n",
             (int)g_audio_in_stop_owner);
      g_audio_in_stop_pending = false;
    }

  owner       = g_audio_in_owner;
  owner_group = g_audio_in_owner_group;
  g_audio_in_stop_pending = true;
  g_audio_in_stop_owner   = nxsched_gettid();

  nxmutex_unlock(&g_audio_in_lock);

  /* 跨组调用要先确认这个 fd 到底通到哪个设备，否则 close() 关掉的是别人组里的
   * 另一个 file 对象（fd 号只在开它的那个 task group 里保证有意义）。判据与
   * stop() 里那段完全一样，也同样是只读探测：过不了就一个系统调用都不发。
   *
   * 本组自己的调用不做这个探测：fd 号是自己开的，close 掉它就是收自己的尾。 */

  if (owner_group != getpid() && !audio_in_fd_is_audio_device(fd))
    {
      syslog(LOG_ERR,
             "AUDIO_IN: abandon 被别的 task group 调用，且 fd=%d 在本组里不是音频"
             "设备（会话属于 group %d / 线程 %d，调用者是 group %d 线程 %d）："
             "已拒绝，fd 没关、全局状态没动。\n"
             "  说明：fd 号在每个 task group 里各有一套含义，跨组拿别人的 fd 号做"
             "close/ioctl 只会打到本组自己的文件上。跨 app 让路请改成"
             "\"登记请求 + 持有者自己的线程执行\"。\n",
             fd, (int)owner_group, (int)owner, (int)getpid(),
             (int)nxsched_gettid());

      if (nxmutex_lock(&g_audio_in_lock) == 0)
        {
          g_audio_in_stop_pending = false;
          g_audio_in_stop_owner   = (pid_t)-1;
          nxmutex_unlock(&g_audio_in_lock);
        }

      return -EPERM;
    }

  /* 清全局再 close —— 顺序和 stop() 保持一致（那里是"STOP 成功 → 清全局 →
   * close"）。只在"会话还是刚才那一个"时清，免得把期间新开的一次会话顶掉：
   * 清完之后再有 audio_in_start()，它看到的就是"无会话"，可以正常开新的一次，
   * 而它新开的 fd 号不可能是我们手里这个（这个槽位还被我们占着，open 会拿别的
   * 号），所以下面的 close 关掉的一定是我们自己那一次会话的 fd。 */

  if (nxmutex_lock(&g_audio_in_lock) == 0)
    {
      if (g_audio_in_fd == fd)
        {
          g_audio_in_fd          = -1;
          g_audio_in_owner       = (pid_t)-1;
          g_audio_in_owner_group = (pid_t)-1;
        }

      g_audio_in_stop_pending = false;
      g_audio_in_stop_owner   = (pid_t)-1;
      nxmutex_unlock(&g_audio_in_lock);
    }

  /* 只关自己的 fd，不发 AUDIOIOC_STOP：这是本函数存在的全部意义。
   * 关闭最后一个 file 对象时上层会替我们调驱动的 shutdown（把 running 清掉），
   * 还有别人开着设备时则连 shutdown 都不会碰 —— 两种情况都不会锁住设备。 */

  close(fd);

  if (owner_group != getpid())
    {
      syslog(LOG_WARNING,
             "AUDIO_IN: abandoned（由别的 task group %d 代收：会话 group=%d / "
             "线程 %d，fd=%d 已关，**没发 AUDIOIOC_STOP**）\n",
             (int)getpid(), (int)owner_group, (int)owner, fd);
      return OK;
    }

  syslog(LOG_WARNING,
         "AUDIO_IN: abandoned（fd=%d 已关，**没发 AUDIOIOC_STOP**：这次会话不是"
         "本层停的，设备留给当前持有者）\n", fd);
  return OK;
}
