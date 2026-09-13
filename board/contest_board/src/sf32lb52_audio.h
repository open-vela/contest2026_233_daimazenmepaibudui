/****************************************************************************
 * boards/sf32lb52/sf32lb52_devkit_lcd/src/sf32lb52_audio.h
 *
 * SF32LB52 音频设备驱动（audio_lowerhalf 实现）
 * 注册 /dev/audio0，支持：
 *   - 播放（DAC + DMA）：喇叭/耳机输出
 *   - 录音（ADC + DMA）：板上 MEMS 麦克风输入
 *   - 功放（AW8155，PA10 = AU_PA_EN）控制
 *
 ****************************************************************************/

#ifndef __BOARDS_SF32LB52_SF32LB52_DEVTKIT_LCD_SRC_SF32LB52_AUDIO_H
#define __BOARDS_SF32LB52_SF32LB52_DEVTKIT_LCD_SRC_SF32LB52_AUDIO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/audio/audio.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: sf32lb52_audio_initialize
 *
 * Description:
 *   初始化 SF32LB52 音频设备（codec + 功放），并注册为 /dev/audio0
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int sf32lb52_audio_initialize(void);

#endif /* __BOARDS_SF32LB52_SF32LB52_DEVTKIT_LCD_SRC_SF32LB52_AUDIO_H */
