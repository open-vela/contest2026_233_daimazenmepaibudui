#!/bin/sh
set -eu
APP_DIR=${1:-app/hello_app}
APP_DIR=$(cd "$APP_DIR" && pwd)
TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT HUP INT TERM

mkdir -p "$TEST_DIR/nuttx/audio"
printf '#define OK 0\n' > "$TEST_DIR/nuttx/config.h"

cat > "$TEST_DIR/nuttx/audio/audio.h" <<'EOF'
#include <stdint.h>
/* Host-only mock: does not verify the real NuttX ABI. */
#define AUDIO_TYPE_INPUT 1
#define AUDIO_TYPE_OUTPUT 2
#define AUDIO_TYPE_FEATURE 4
#define AUDIO_FU_VOLUME 1
#define AUDIOIOC_CONFIGURE 100
#define AUDIOIOC_START 101
#define AUDIOIOC_STOP 102
struct audio_caps_s {
  uint8_t ac_len, ac_type, ac_channels;
  union { uint16_t hw; } ac_format;
  union { uint16_t hw[4]; uint8_t b[8]; } ac_controls;
};
struct audio_caps_desc_s { struct audio_caps_s caps; };
EOF

cat > "$TEST_DIR/test.c" <<'EOF'
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static size_t live_bytes, peak_bytes, written, received, idle_bytes;
static bool lazy_memory, fail_malloc;
static int failure, open_count, stop_count, complete_count, reads;
static unsigned char payload[640];
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static bool entered, released;

static void *mock_malloc(size_t n) {
  if (fail_malloc) return NULL;
  size_t *p = malloc(n + sizeof(max_align_t));
  if (!p) return NULL;
  *p = n;
  live_bytes += n;
  if (live_bytes > peak_bytes) peak_bytes = live_bytes;
  return (unsigned char *)p + sizeof(max_align_t);
}

static void mock_free(void *v) {
  if (!v) return;
  size_t *p = (size_t *)((unsigned char *)v - sizeof(max_align_t));
  live_bytes -= *p;
  free(p);
}

static void *mock_calloc(size_t n, size_t z) {
  void *p = mock_malloc(n * z);
  if (p) memset(p, 0, n * z);
  return p;
}

static int mock_open(const char *path, int flags, ...) {
  (void)flags;
  assert(strcmp(path, "/dev/audio/audio0") == 0);
  if (failure == 1) { errno = ENOENT; return -1; }
  ++open_count;
  return 123;
}

static int mock_close(int fd) {
  assert(fd == 123);
  --open_count;
  return 0;
}

static int mock_ioctl(int fd, unsigned long request, ...);

static ssize_t mock_write(int fd, const void *p, size_t n) {
  assert(fd == 123);
  if (failure == 4) { errno = EIO; return -1; }
  if (failure == 5) return 0;
  if (failure == 6) {
    failure = 0;
    errno = EINTR;
    return -1;
  }
  if (n > 13) n = 13;
  assert(written + n <= sizeof(payload));
  assert(memcmp(p, payload + written, n) == 0);
  written += n;
  return (ssize_t)n;
}

static ssize_t mock_read(int fd, void *p, size_t n) {
  assert(fd == 123);
  ++reads;
  if (failure == 8) {
    pthread_mutex_lock(&gate);
    entered = true;
    pthread_cond_signal(&condition);
    while (!released) pthread_cond_wait(&condition, &gate);
    pthread_mutex_unlock(&gate);
    memset(p, 0, n);
    return (ssize_t)n;
  }
  if (reads == 1) { errno = EINTR; return -1; }
  if (failure == 7) { errno = EIO; return -1; }
  if (n > 17) n = 17;
  assert(received + n <= sizeof(payload));
  memcpy(p, payload + received, n);
  received += n;
  return (ssize_t)n;
}

static int mock_create(pthread_t *t, const pthread_attr_t *a,
                       void *(*fn)(void *), void *v) {
  if (failure == 3) return EAGAIN;
  return pthread_create(t, a, fn, v);
}

#define malloc mock_malloc
#define calloc mock_calloc
#define free mock_free
#define open mock_open
#define close mock_close
#define ioctl mock_ioctl
#define read mock_read
#define write mock_write
#define pthread_create mock_create
#include "ai_audio.c"
#undef malloc
#undef calloc
#undef free
#undef open
#undef close
#undef ioctl
#undef read
#undef write
#undef pthread_create

static int mock_ioctl(int fd, unsigned long request, ...) {
  assert(fd == 123);
  if (request == AUDIOIOC_CONFIGURE) {
    va_list ap;
    va_start(ap, request);
    struct audio_caps_desc_s *d = (void *)va_arg(ap, unsigned long);
    va_end(ap);
    if (d->caps.ac_type == AUDIO_TYPE_FEATURE) {
      assert(d->caps.ac_controls.hw[0] == 700);
    } else {
      assert(d->caps.ac_channels == 1);
      assert(d->caps.ac_controls.hw[0] == 16000);
      assert(d->caps.ac_controls.b[2] == 16);
    }
    if (failure == 2) { errno = EINVAL; return -1; }
  }
  if (request == AUDIOIOC_STOP) ++stop_count;
  return 0;
}

static void done(void *v) {
  (void)v;
  ++complete_count;
}

static void record_cb(const int16_t *data, size_t frames, void *v) {
  audio_context_t *ctx = v;
  assert(frames == 320);
  assert(memcmp(data, payload, 640) == 0);
  __atomic_store_n(&ctx->record_stop, true, __ATOMIC_RELEASE);
}

static void join_play(audio_context_t *ctx) {
  assert(pthread_join(ctx->play_thread, NULL) == 0);
  ctx->play_thread_valid = false;
  assert(!audio_is_playing(ctx));
  assert(live_bytes == idle_bytes && open_count == 0);
  if (lazy_memory) assert(ctx->play_buf == NULL);
}

int main(void) {
  audio_context_t ctx;
  int16_t samples[320];

  for (unsigned i = 0; i < sizeof(payload); ++i)
    payload[i] = i % 251;
  memcpy(samples, payload, sizeof(samples));

  assert(audio_init(&ctx, NULL) == 0);
  idle_bytes = live_bytes;
  lazy_memory = ctx.play_buf == NULL;
  assert(idle_bytes == (lazy_memory ? 640u : 160640u));

  printf("Audio init allocations: %zu bytes; lazy playback: %s\n",
         idle_bytes,
         lazy_memory ? "YES" : "NO (memory optimization not applied)");

  for (failure = 1; failure <= 3; ++failure) {
    assert(audio_play_start(&ctx, samples, 320, done, NULL) < 0);
    assert(live_bytes == idle_bytes);
    assert(open_count == 0 && !audio_is_playing(&ctx));
  }

  failure = 0;
  if (lazy_memory) {
    fail_malloc = true;
    assert(audio_play_start(&ctx, samples, 320, done, NULL) == -ENOMEM);
    fail_malloc = false;
  }

  for (int f = 4; f <= 6; ++f) {
    failure = f;
    written = 0;
    complete_count = 0;
    assert(audio_play_start(&ctx, samples, 320, done, NULL) == 0);
    join_play(&ctx);
    assert(complete_count == (f == 6 ? 1 : 0));
    if (f == 6) assert(written == 640);
  }

  FILE *file = fopen("sample.pcm", "wb");
  assert(file);
  assert(fwrite(samples, 1, 640, file) == 640);
  fclose(file);

  failure = 0;
  written = 0;
  complete_count = 0;
  assert(audio_play_file(&ctx, "sample.pcm", done, NULL) == 0);
  join_play(&ctx);
  assert(written == 640 && complete_count == 1);
  assert(peak_bytes == (lazy_memory ? 1280u : idle_bytes));

  file = fopen("sample.pcm", "wb");
  assert(file);
  fputs("RIFF", file);
  fclose(file);
  assert(audio_play_file(&ctx, "sample.pcm", done, NULL) == -ENOTSUP);
  assert(live_bytes == idle_bytes);

  audio_record_config_t cfg = {
    .data_callback = record_cb,
    .user_data = &ctx
  };

  for (int f = 0; f <= 7; f += 7) {
    failure = f;
    reads = 0;
    received = 0;
    assert(audio_record_start(&ctx, &cfg) == 0);
    assert(pthread_join(ctx.record_thread, NULL) == 0);
    ctx.record_thread_valid = false;
    assert(!audio_is_recording(&ctx) && open_count == 0);
    assert(received == (f == 0 ? 640 : 0));
  }

  failure = 8;
  assert(audio_record_start(&ctx, &cfg) == 0);
  pthread_mutex_lock(&gate);
  while (!entered) pthread_cond_wait(&condition, &gate);

  assert(audio_play_start(&ctx, samples, 320, done, NULL) == -EBUSY);
  __atomic_store_n(&ctx.record_stop, true, __ATOMIC_RELEASE);
  released = true;
  pthread_cond_signal(&condition);
  pthread_mutex_unlock(&gate);

  audio_record_stop(&ctx);
  assert(!ctx.record_thread_valid && open_count == 0);

  audio_deinit(&ctx);
  assert(live_bytes == 0 && stop_count > 0);

  puts("audio I/O host tests: PASS "
       "(failures, PCM, short I/O, EINTR, normal stop)");
  return 0;
}
EOF

${CC:-cc} -std=gnu11 -Wall -Wextra -Werror \
  -I"$TEST_DIR" -I"$APP_DIR" \
  "$TEST_DIR/test.c" -pthread -lm -o "$TEST_DIR/test"

(cd "$TEST_DIR" && ./test)
