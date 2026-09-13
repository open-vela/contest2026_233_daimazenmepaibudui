# app/robot_ui/CMakeLists.txt

# 条件编译：启用 ZHI_AI 或 ROBOT_UI 时编译
if(CONFIG_LVX_USE_CONTEST2026_233_ZHI_AI OR CONFIG_LVX_USE_CONTEST2026_233_ROBOT_UI)

  # hello_app 路径（使用 symlink 名，避免 CMake 跟随 symlink 后路径错位）
  set(HELLO_APP_DIR ${NUTTX_APPS_DIR}/packages/demos/contest2026_233_hello_app)

  # hello_app 头文件路径 (AI 模块接口)
  set(ROBOT_UI_COMPILE_FLAGS
    -I${HELLO_APP_DIR}
  )

  # ai_agent TLS 客户端（用于 HTTPS 推送）
  if(CONFIG_HELLO_APP_LLM_AI_AGENT)
    list(APPEND ROBOT_UI_COMPILE_FLAGS
      -I${NUTTX_APPS_DIR}/packages/ai_agent/include
      -I${NUTTX_APPS_DIR}/packages/ai_agent/src
    )
  endif()

  # 包含 hello_app 源文件 (AI 模块实现)
  set(ROBOT_UI_SRCS
    main.c
    robot_ui.c
    touch_ui.c
    network_comm.c
    ${HELLO_APP_DIR}/ai_state_machine.c
    ${HELLO_APP_DIR}/ai_audio.c
    ${HELLO_APP_DIR}/ai_llm.c
    ${HELLO_APP_DIR}/ai_sound_detect.c
    ${HELLO_APP_DIR}/ai_care.c
  )

  # ai_agent 链接依赖
  set(ROBOT_UI_DEPENDS "")
  if(CONFIG_HELLO_APP_LLM_AI_AGENT)
    list(APPEND ROBOT_UI_DEPENDS ai_agent)
  endif()

  # 添加应用程序
  nuttx_add_application(
    NAME robot_ui
    SRCS ${ROBOT_UI_SRCS}
    STACKSIZE ${CONFIG_DEFAULT_TASK_STACKSIZE}
    PRIORITY 100
    COMPILE_FLAGS ${ROBOT_UI_COMPILE_FLAGS}
    DEPENDS ${ROBOT_UI_DEPENDS}
  )

endif()
