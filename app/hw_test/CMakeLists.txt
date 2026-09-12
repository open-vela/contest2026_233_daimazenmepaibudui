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
  nuttx_add_application(
    NAME hw_test
    SRCS main.c
    STACKSIZE 8192
    PRIORITY 100)
endif()
