#ifndef __LCD_MIRROR_HOST_STUB_MUTEX_H
#define __LCD_MIRROR_HOST_STUB_MUTEX_H

#include <pthread.h>

/* NuttX 的 nxmutex 就是带优先级继承的互斥量；主机上直接拿 pthread 互斥量顶。 */
typedef pthread_mutex_t mutex_t;

#define NXMUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER

#define nxmutex_lock(m)    pthread_mutex_lock(m)
#define nxmutex_unlock(m)  pthread_mutex_unlock(m)

#endif
