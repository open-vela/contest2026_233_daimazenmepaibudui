#ifndef __LCD_MIRROR_HOST_STUB_SCHED_H
#define __LCD_MIRROR_HOST_STUB_SCHED_H

#include <stddef.h>

#define FAR

typedef int (*main_t)(int argc, char *argv[]);

/* 主机桩：真起一条 pthread 跑 entry（这样镜像任务循环就是真的在跑）。 */
int task_create(const char *name, int priority, size_t stack_size,
                main_t entry, char * const argv[]);

#endif
