# app/robot_ui/CMakeLists.txt

# 条件编译：只有启用了这个配置才编译
if(CONFIG_LVX_USE_CONTEST2026_233_ZHI_AI)

  # 添加应用程序
  #
  # INCLUDE_DIRECTORIES 里两个路径的用途：
  #
  # ① `${CMAKE_CURRENT_SOURCE_DIR}/../hello_app`：robot_ui 的 main.c 会调成员二
  #    hello_app 里的 AI 模块（ai_state_machine / ai_audio / ai_llm / ai_sound_detect /
  #    ai_care），需要它们的头文件路径。
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
  # SRCS 里的 lv_font_ui_16/20/24.c：用 lv_font_conv + simhei.ttf 从 app/ 全部源码抽字符
  # （971 个字符）生成的三档中文字库，变量名 lv_font_ui_16 / lv_font_ui_20 / lv_font_ui_24。
  # LVGL 自带的 16px 中文点阵字体（simsun）只有约 1436 个字形，界面用到的汉字一半是方块；
  # 这三档才是界面上真正用的字体。
  nuttx_add_application(
    NAME robot_ui
    SRCS main.c robot_ui.c touch_ui.c network_comm.c
         lv_font_ui_16.c lv_font_ui_20.c lv_font_ui_24.c
    STACKSIZE 32768
    PRIORITY 100
    INCLUDE_DIRECTORIES
      ${CMAKE_CURRENT_SOURCE_DIR}/../hello_app
      ${NUTTX_BOARD_ABS_DIR}/src
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
