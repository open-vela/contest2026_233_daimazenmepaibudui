# Contest 2026 team 233 - AI companion app
if(CONFIG_LVX_USE_DEMO_CONTEST2026_000_HELLO_APP)
  nuttx_add_application(
    NAME hello_app
    SRCS ai_companion_main.c ai_state_machine.c ai_audio.c ai_llm.c ai_sound_detect.c ai_care.c
    STACKSIZE 16384)
endif()
