/****************************************************************************
 * app/drvtest/netstub.c
 *
 * 给「开着 CONFIG_NET 核心、但一块网卡都没有」的配置补一个空的
 * arm_netinitialize()。
 *
 * 为什么需要它（两条都对着源码核过）：
 *   1. nuttx/arch/arm/src/common/arm_initialize.c 里 arm_netinitialize()
 *      是**无条件调用**的（那行外面没有 #ifdef CONFIG_NET），所以这个符号
 *      必须有人定义，否则链接直接报
 *          undefined reference to `arm_netinitialize'
 *   2. 本固件把 CONFIG_NET 留着，只是为了绕开 usrsock.h 那个头文件问题
 *      （原因写在 board/contest_board/configs/drvtest/defconfig 里），
 *      一片网卡驱动都没开，所以没有任何东西会去定义它。
 *
 * 上游现成的同款做法就在 packages/ai_agent/src/stubs.c（注释原文是
 * "Stubs to fix link errors in a broken defconfig"），那里也是一个空的
 * weak arm_netinitialize()。这里照抄同样的写法，**只在 drvtest 这个 app 里
 * 定义**，不碰共享的板级/内核源码；而且它是 weak，以后真加回网卡驱动时，
 * 驱动里那个强符号会盖掉它。
 *
 * 它什么都不做：不注册 netdev、不发包、不参与任何初始化。
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_NET

__attribute__((weak)) void arm_netinitialize(void)
{
}

#endif /* CONFIG_NET */
