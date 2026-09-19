# app/drvtest/CMakeLists.txt
#
# 独立的驱动测试固件（NSH 命令 drvtest）。
#
# 只挂在 CONFIG_LVX_USE_CONTEST2026_233_DRVTEST 上（app/drvtest/Kconfig），
# 那个符号只有 board/contest_board/configs/drvtest 会置 y。这样主配置
# configs/sf32lb52_ai 编出来的固件里不会多出 drvtest。
#
# INCLUDE_DIRECTORIES 只给板级 src（driver test 要用的都是板级驱动封装头：
# sf32lb52_audio_in.h / sf32lb52_backlight.h / sf32lb52_rtc_alarm.h /
# sf32lb52_boardbtn.h），不引 robot_ui / hello_app 的任何东西。
#
# STACKSIZE 32768：drvtest 自己的主任务里要跑 LCD 整屏刷色和 i2c 扫描，
# 还要放 512 字节的参数拼接缓冲，8KB 太紧。
if(CONFIG_LVX_USE_CONTEST2026_233_DRVTEST)
  nuttx_add_application(
    NAME drvtest
    SRCS main.c netstub.c
    STACKSIZE 32768
    PRIORITY 100
    INCLUDE_DIRECTORIES ${NUTTX_BOARD_ABS_DIR}/src)
endif()
