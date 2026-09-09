# app/robot_ui/CMakeLists.txt

# 条件编译：只有启用了这个配置才编译
if(CONFIG_LVX_USE_CONTEST2026_233_ZHI_AI)

  # 添加应用程序
  nuttx_add_application(
    NAME robot_ui
    SRCS main.c robot_ui.c touch_ui.c network_comm.c
    STACKSIZE 32768
    PRIORITY 100
  )

endif()
