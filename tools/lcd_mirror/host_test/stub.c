/* 主机侧桩实现（说明见 stubs/nuttx/config.h） */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>

#include <nuttx/clock.h>
#include <nuttx/sched.h>
#include <syslog.h>

uint32_t clock_systime_ticks(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)((uint64_t)ts.tv_sec * 100 + (uint64_t)ts.tv_nsec / 10000000ull);
}

struct task_arg_s
{
  main_t entry;
};

static void *task_trampoline(void *p)
{
  struct task_arg_s *a = (struct task_arg_s *)p;
  main_t entry = a->entry;

  free(a);
  entry(0, NULL);
  return NULL;
}

int task_create(const char *name, int priority, size_t stack_size,
                main_t entry, char * const argv[])
{
  pthread_t        tid;
  pthread_attr_t   attr;
  struct task_arg_s *a;

  (void)name;
  (void)priority;
  (void)argv;

  a = malloc(sizeof(*a));
  if (a == NULL)
    {
      return -1;
    }

  a->entry = entry;

  if (pthread_attr_init(&attr) != 0)
    {
      free(a);
      return -1;
    }

  if (stack_size > 0)
    {
      pthread_attr_setstacksize(&attr, stack_size < 64 * 1024 ? 64 * 1024 : stack_size);
    }

  if (pthread_create(&tid, &attr, task_trampoline, a) != 0)
    {
      pthread_attr_destroy(&attr);
      free(a);
      return -1;
    }

  pthread_attr_destroy(&attr);
  pthread_detach(tid);
  return 1;
}

void syslog(int priority, const char *fmt, ...)
{
  va_list ap;
  const char *tag;

  switch (priority)
    {
      case LOG_ERR:     tag = "ERR "; break;
      case LOG_WARNING: tag = "WARN"; break;
      default:          tag = "INFO"; break;
    }

  fprintf(stderr, "[%s] ", tag);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}
