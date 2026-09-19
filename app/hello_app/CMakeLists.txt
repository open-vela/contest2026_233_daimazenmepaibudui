if(CONFIG_LVX_USE_CONTEST2026_233_HELLO_APP)
  set(HELLO_APP_SRCS
    ai_companion_main.c
    ai_state_machine.c
    ai_audio.c
    ai_llm.c
    ai_sound_detect.c
    ai_care.c
    ai_network.c
    # 小米 MiMo 云端语音后端（ASR + TTS，普通 HTTPS POST）。
    # 只依赖 ai_agent 的 voice_asr/voice_tts 分发接口 + vela_tls，见文件头注释。
    mimo_voice.c
    # IP 定位（免 key 的 HTTPS 接口，结果缓存），给 mimo_chat 的 system prompt
    # 提供"用户所在城市"；只用 vela_tls，不依赖别的东西。
    mimo_location.c
    # 唤醒词「你好，openvela」/「Hello，openvela」的命令词识别（MFCC + DTW，
    # 纯本地纯计算）。不开麦、不起线程、不碰 LVGL：父 agent 把麦克风 PCM
    # 喂给 kws_feed()，返回 1 就是命中。没有模板时永远返回 0（见 .h 文件头）。
    kws_dtw.c
    # 把我们的工具（set_light 等）注册进 openvela 框架的工具表，让框架里的
    # 大模型也能调用。get_tools/execute 两个回调跑在 ai_agent 那个 app 的任务里，
    # 所以只有 ai_agent 在跑时模型才看得见（见 .h 的「运行时机」）。
    ai_tools_provider.c
    # 跨 app 请求的薄壳（robot_ui 调它给 hello_app 递两个非阻塞请求：「提交」
    # 和"报警后让语音追问收摊"，见 ai_companion_req.h）：只登记标志位，真动作在
    # ai_companion_main.c（那边的 static 才够得着）。
    # robot_ui 也 include 这个头文件，所以它必须自给自足（不引 LVGL）。
    # ★ 2026-09-21：原来的「让路 / 麦克风转让」握手（ai_companion_yield.c/.h）
    #   整套删掉了，本文件由它改名而来，只留下和让路无关的那两个请求。
    ai_companion_req.c
    # 只读诊断快照的薄壳（robot_ui 的 network_task 靠它把 hello_app 的内部状态
    # 塞进心跳的 ha 位和 {"action":"diag"} 的报文里，见 ai_companion_diag.h）：
    # 本文件只认 "key=value" 那一行文本、负责拼 JSON，真读取在
    # ai_companion_main.c 的 ai_companion_state_snapshot_impl()（那边的 static
    # 才够得着）。它同样要自给自足 —— robot_ui 也 include 那个头文件。
    ai_companion_diag.c
    # 固定文案的 TTS 预缓存（查表 + 文件名规则）：关怀/追问/提醒这些写死的句子
    # 在 PC 上预合成成 16k/mono/s16le 裸 PCM 随固件打进 ROMFS，板端算个 FNV-1a
    # 哈希就能找到、直接播，不走网络也不阻塞。文件名规则必须和 PC 侧脚本
    # （_flash/gen_tts_cache.py）逐字节一致，对拍见 _tts_cache_host_test/。
    # 同样自给自足：robot_ui 也 include 这个头文件（跨 app 的符号最终链接时解析）。
    tts_cache.c
  )

  if(CONFIG_HELLO_APP_SOUND_EVENT)
    # 本地声音事件识别（摔倒/敲击/尖叫）：mel 前端 + DSCNN-lite + 3/4 投票，
    # 纯 C 零依赖（自写 512 点 FFT，不用 CMSIS-DSP / tflite），权重在
    # sound_event_model.h 里。谁喂 PCM、命中后干什么，由调用方决定（见 sound_event.h）。
    list(APPEND HELLO_APP_SRCS sound_event.c)
  endif()

  set(HELLO_APP_COMPILE_FLAGS "")

  if(CONFIG_HELLO_APP_LLM_AI_AGENT)
    list(APPEND HELLO_APP_COMPILE_FLAGS
      -I${NUTTX_APPS_DIR}/packages/ai_agent/include
      -I${NUTTX_APPS_DIR}/packages/ai_agent/src
      -I${NUTTX_APPS_DIR}/packages/ai_agent/src/voice
    )
  endif()

  set(HELLO_APP_DEPENDS "")

  if(CONFIG_HELLO_APP_LLM_AI_AGENT)
    # Link against ai_agent for velaclaw client functions
    list(APPEND HELLO_APP_DEPENDS ai_agent)
  endif()

  nuttx_add_application(
    NAME ${CONFIG_HELLO_APP_PROGNAME}
    PRIORITY ${CONFIG_HELLO_APP_PRIORITY}
    STACKSIZE ${CONFIG_HELLO_APP_STACKSIZE}
    MODULE ${CONFIG_LVX_USE_CONTEST2026_233_HELLO_APP}
    COMPILE_FLAGS ${HELLO_APP_COMPILE_FLAGS}
    DEPENDS ${HELLO_APP_DEPENDS}
    # ai_audio.c 用板级录音封装 sf32lb52_audio_in.h（audio_in_start/read/stop），
    # 写法与 app/hw_test、app/robot_ui 一致。
    # ../robot_ui：语音链路推界面的直调入口 robot_ui_bridge.h 在那里
    # （实现在 robot_ui 那个 app 里，跨 app 的符号最终链接时解析）。
    # 那个头文件是自给自足的（不引 LVGL），所以只加这一个 -I 就够了。
    INCLUDE_DIRECTORIES ${NUTTX_BOARD_ABS_DIR}/src
                        ${CMAKE_CURRENT_SOURCE_DIR}/../robot_ui
    SRCS ${HELLO_APP_SRCS}
  )
endif()
