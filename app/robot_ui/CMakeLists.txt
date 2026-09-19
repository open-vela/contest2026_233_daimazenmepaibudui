# app/robot_ui/CMakeLists.txt

# 条件编译：只有启用了这个配置才编译
if(CONFIG_LVX_USE_CONTEST2026_233_ZHI_AI)

  # 添加应用程序
  #
  # INCLUDE_DIRECTORIES 里三个路径的用途：
  #
  # ① `${CMAKE_CURRENT_SOURCE_DIR}/../hello_app`：robot_ui 的 main.c 会调成员二
  #    hello_app 里的 AI 模块（ai_state_machine / ai_audio / ai_sound_detect /
  #    ai_care / mimo_voice），需要它们的头文件路径。
  #    注意 ai_llm.h 已经不用了：main.c 的对话改走 mimo_voice.c 的 mimo_chat()，
  #    不再碰 llm_send_text()（那条路会撞 ai_agent 消息总线的未初始化队列锁）。
  #    注意这里写 `../hello_app` 是**对的**：`contest2026_233_robot_ui` 本身是符号链接
  #    指向我们仓库的 `app/robot_ui`，所以 `${CMAKE_CURRENT_SOURCE_DIR}/..` 解析到的是
  #    仓库的 `app/` 目录，而不是构建树里的 `apps/packages/demos/`。
  #    （反过来写 `../contest2026_233_hello_app` 会指到 `app/contest2026_233_hello_app`，不存在。）
  #    只要头文件、**不要**把那些 .c 也编进来：它们已经属于 hello_app 那个 app，
  #    重复编译会让同一批符号被编两遍；跨 app 的符号在最终链接时能解析（同一个固件里）。
  #
  # ② `${NUTTX_BOARD_ABS_DIR}/src`：板级模块（sf32lb52_alarm.h 等）的头文件目录，
  #    与 app/hello_app、app/hw_test 的 CMakeLists.txt 是同一写法。
  #    robot_ui 的报警按钮要调 alarm_trigger()/alarm_clear()，需要它。
  #
  # ③ `${NUTTX_APPS_DIR}/packages/ai_agent/include`：ai_agent 包的
  #    voice/voice_asr.h、voice/voice_tts.h（语音后端分发层）。robot_ui 的
  #    "语音聊天"要自己调 voice_asr_recognize()/voice_tts_speak()，
  #    与 app/hello_app/CMakeLists.txt 里那两条 -I 是同一写法。
  #    符号本身在 ai_agent 那个 app 里（voice_asr.c / voice_tts.c），最终链接时解析。
  #
  # SRCS 里的 lv_font_ui_16/20/24.c：用 lv_font_conv + simhei.ttf 生成的三档中文字库，
  # 变量名 lv_font_ui_16 / lv_font_ui_20 / lv_font_ui_24（16/4bpp、20/4bpp、24/4bpp，
  # 与改动前 montserrat 的字号分档一一对应）。
  # 字符集是**常用汉字全集**：GB2312 一级+二级 6763 字 + ASCII 0x20-0x7E +
  # CJK 标点 U+3000-U+303F + 全角 U+FF00-U+FFEF + 少量常用符号，共 7178 个码点
  # （字库里实际 7010 个字形，simhei 没有的会跳过）。
  # 以前是从 app/ 源码里抽字符生成的（971 个），AI 回复里源码没出现过的字全是方块，
  # 所以换成了全集。生成脚本和命令见 patches/README.md「中文字库覆盖常用汉字全集」。
  #
  # SRCS 里的 reminder_sched.c：提醒的软调度（硬件只有一个 RTC 日循环 alarm 槽，
  # 多个提醒要在软件层排队）。提醒列表的数据也放在那里，touch_ui.c 只通过
  # reminder_sched_* 增删查；到点回调在工作线程里，main.c 用 lv_async_call 弹窗。
  # 环境听音（工作流 C）：常态听音 + 突发大音量检测 -> ASR -> 大模型判断。
  # 实现在 ambient_listen.c，默认**关**（配置键 enable_ambient_listen），
  # 打开后才起线程占麦克风、才分配那约 250 KiB 堆缓冲。
  nuttx_add_application(
    NAME robot_ui
    SRCS main.c robot_ui.c touch_ui.c reminder_sched.c network_comm.c time_sync.c
         ambient_listen.c
         # 跨线程"投到 LVGL 线程"的唯一投递口：内部把 lv_async_call 用一把
         # 互斥锁包住（LVGL 9.1 的 timer 链表插入本身没有锁，多线程同时插会
         # 插坏链表 -> LVGL 线程跳到垃圾地址 -> 整机硬故障）。原因和残余风险
         # 写在 ui_async.h 里，调用点见 main.c / touch_ui.c。
         ui_async.c
         # 语音链路推界面的跨 app 直调入口（ai_companion 直接函数调用，不再只靠
         # MQTT 绕公网）。头文件是自给自足的，hello_app 那边只 include 它。
         robot_ui_bridge.c
         # 刷屏/渲染耗时仪表（工作用）：只把慢 flush 和慢帧打到串口。总开关在
         # ui_perf.h 的 UI_PERF_LOG，置 0 时这个文件编译成空翻译单元，main.c 里
         # 那几处调用也被 #if 掉。不想要了就删这一行 + 删两个文件。
         ui_perf.c
         # 屏幕镜像的 LVGL 侧（真机屏幕坏了，拿电脑当显示器）：
         # 只挂一个 LV_EVENT_FLUSH_START 把它们交给驱动的那块脏区拷进板级
         # 影子帧缓冲，socket/协议/任务/nsh 全在 board/.../lcd_mirror.c。
         # 拿掉这个功能：删这一行 + 删两个文件 + 删 main.c 里那一句调用。
         lcd_mirror_glue.c
         # TTS 预缓存（app/hello_app/tts_cache.c）**不在这一行里**：main.c 只是
         # include 它的头文件、调 tts_cache_load()，实现已经属于 hello_app 那个 app
         # （和上面 robot_ui_bridge.c / ai_companion_req.h 同一套跨 app 处理：
         #  头文件路径由下面 `../hello_app` 提供，符号在最终链接时解析，
         #  重复编译会让同一批符号被编两遍）。
         lv_font_ui_16.c lv_font_ui_20.c lv_font_ui_24.c
    STACKSIZE 32768
    PRIORITY 100
    INCLUDE_DIRECTORIES
      ${CMAKE_CURRENT_SOURCE_DIR}/../hello_app
      ${NUTTX_BOARD_ABS_DIR}/src
      ${NUTTX_APPS_DIR}/packages/ai_agent/include
      ${NUTTX_APPS_DIR}/packages/ai_agent/src
  )

  # 板子网络连通性测试（与 zhi_ai 无关，纯测网络）
  # NSH 里执行：net_test
  nuttx_add_application(
    NAME net_test
    SRCS net_test.c
    STACKSIZE 8192
    PRIORITY 100
  )

endif()
