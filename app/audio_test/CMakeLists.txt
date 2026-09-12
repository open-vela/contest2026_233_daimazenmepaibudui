# app/audio_test/CMakeLists.txt
if(CONFIG_EXAMPLES_AUDIO_TEST)
  nuttx_add_application(
    NAME audio_test
    SRCS main.c
    STACKSIZE 4096)
endif()
