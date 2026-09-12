if(CONFIG_LVX_USE_CONTEST2026_233_HELLO_APP)
  set(HELLO_APP_SRCS
    ai_companion_main.c
    ai_state_machine.c
    ai_audio.c
    ai_llm.c
    ai_sound_detect.c
    ai_care.c
    ai_network.c
  )

  set(HELLO_APP_COMPILE_FLAGS "")

  if(CONFIG_HELLO_APP_LLM_AI_AGENT)
    # The ai_agent include is at ${NUTTX_APPS_DIR}/packages/ai_agent/include
    list(APPEND HELLO_APP_COMPILE_FLAGS
      -I${NUTTX_APPS_DIR}/packages/ai_agent/include
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
    SRCS ${HELLO_APP_SRCS}
  )
endif()
