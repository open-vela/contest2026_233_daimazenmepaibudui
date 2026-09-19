#ifndef __LCD_MIRROR_HOST_STUB_CLOCK_H
#define __LCD_MIRROR_HOST_STUB_CLOCK_H

#include <stdint.h>

/* 对齐真机：CONFIG_USEC_PER_TICK=10000 -> 100 Hz，一 tick = 10 ms */
uint32_t clock_systime_ticks(void);

#define TICK2MSEC(t)  ((uint32_t)(t) * 10u)
#define MSEC2TICK(m)  ((uint32_t)(m) / 10u)

#endif
