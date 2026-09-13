# app/hw_test/CMakeLists.txt
#
# 注意：这里故意**不**用独立的 Kconfig 符号（如 CONFIG_EXAMPLES_HW_TEST）。
#
# 原因：本工作区的 apps Kconfig 是构建时自动生成的，而
# nuttx/cmake/nuttx_kconfig.cmake 的 nuttx_generate_kconfig() 开头有这样一段：
#     if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt"
#        OR EXISTS "${NUTTX_APPS_BINDIR}/Kconfig")
#       return()
# 一旦 <build>/apps/Kconfig 已经存在（第一次 configure 就会生成），
# 之后**永远不会重新生成**，于是新增的 app 目录（比如本目录）不会被收录进
# Kconfig，符号在 .config 里也就永远是 "not set"，nuttx_add_application 永远不执行。
# 实测：加了 CONFIG_EXAMPLES_HW_TEST=y 也编不进固件（System.map 里没有 hw_test_main）。
#
# 所以这里跟 app/robot_ui、app/zhi_ai 保持一致，挂在团队 app 的总开关上。
# 改动本文件后需要触发一次 cmake reconfigure（ninja 会因本文件变新自动重跑）。
if(CONFIG_LVX_USE_CONTEST2026_233_ZHI_AI)

  # tts/asr 两个子命令要用 hello_app 的 MiMo 后端 + ai_agent 的
  # voice_asr/voice_tts 分发层：
  #   - INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/../hello_app
  #     拿 mimo_voice.h。写法照 app/robot_ui/CMakeLists.txt：hw_test 在构建树里是
  #     符号链接（apps/packages/demos/contest2026_233_hw_test -> 本仓库 app/hw_test），
  #     所以 `..` 解析到仓库的 app/。只拿头文件，**不**把那些 .c 也编进来
  #     （它们属于 hello_app 那个 app，重复编译会让同一批符号被编两遍；
  #     跨 app 的符号在最终链接时能解析，同一个固件里 —— robot_ui 就是这么调
  #     hello_app 的）。
  #   - COMPILE_FLAGS 里的三个 -I 是 ai_agent 的头文件路径，和 hello_app 里的写法
  #     完全一致（voice_asr.h / voice_tts.h 在 include/voice 下）。
  #   - DEPENDS ai_agent 只是保证构建顺序。
  #
  # STACKSIZE 从 8192 提到 32768：tts/asr 要在本任务里跑一次完整 HTTPS
  # （mbedtls 握手 + 几十万字节的 base64 收发），8 KB 栈不够。
  # 任务栈是从堆上分配的，改大不影响任何现有子命令。

  set(HW_TEST_COMPILE_FLAGS "")
  set(HW_TEST_DEPENDS "")

  if(CONFIG_HELLO_APP_LLM_AI_AGENT AND CONFIG_LVX_USE_CONTEST2026_233_HELLO_APP)
    list(APPEND HW_TEST_COMPILE_FLAGS
      -I${NUTTX_APPS_DIR}/packages/ai_agent/include
      -I${NUTTX_APPS_DIR}/packages/ai_agent/src
      -I${NUTTX_APPS_DIR}/packages/ai_agent/src/voice
    )
    list(APPEND HW_TEST_DEPENDS ai_agent)
  endif()

  nuttx_add_application(
    NAME hw_test
    SRCS main.c
    STACKSIZE 32768
    PRIORITY 100
    COMPILE_FLAGS ${HW_TEST_COMPILE_FLAGS}
    DEPENDS ${HW_TEST_DEPENDS}
    INCLUDE_DIRECTORIES
      ${CMAKE_CURRENT_SOURCE_DIR}/../hello_app
      ${NUTTX_BOARD_ABS_DIR}/src)
endif()
