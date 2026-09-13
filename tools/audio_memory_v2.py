from pathlib import Path
import re
import shutil
import tempfile

root = Path("app/hello_app")
p = root / "ai_audio.c"
m = root / "ai_companion_main.c"
s, main = p.read_text(), m.read_text()

if "AI_AUDIO_MEMORY_V2" in s:
    raise SystemExit("已经应用过内存优化，不要重复执行。")

def once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError("源码匹配失败：" + old[:90])
    return text.replace(old, new, 1)

def function(text, name, new):
    pattern = r"^int " + name + r"\([^;]*?\)\s*\n\{.*?^\}"
    text, count = re.subn(
        pattern, lambda _: new.strip(), text,
        flags=re.MULTILINE | re.DOTALL
    )
    if count != 1:
        raise RuntimeError("函数匹配失败：" + name)
    return text

# 初始化时只分配录音缓存。
a = s.index("  ctx->play_buf_size =\n")
b = s.index("  ctx->state = AUDIO_STATE_IDLE;", a)
s = s[:a] + """  /* AI_AUDIO_MEMORY_V2: allocate on playback only. */
  ctx->play_buf = NULL;
  ctx->play_buf_size = 0;

""" + s[b:]

helper = r'''
static void audio_release_play_buffer(audio_context_t *ctx)
{
  free(ctx->play_buf);
  ctx->play_buf = NULL;
  ctx->play_buf_size = 0;
  ctx->play_frames = 0;
}

/* One controlling thread per context; owns ctx->play_buf. */
static int audio_launch_play(audio_context_t *ctx,
                             audio_play_complete_cb_t callback,
                             void *user_data)
{
  int ret = audio_open_play_device(ctx);
  if (ret < 0)
    {
      audio_release_play_buffer(ctx);
      return ret;
    }

  ctx->play_cb = callback;
  ctx->play_user_data = user_data;
  ctx->play_frames = ctx->play_buf_size / sizeof(int16_t);
  __atomic_store_n(&ctx->play_stop, false, __ATOMIC_RELEASE);
  ctx->state = AUDIO_STATE_PLAYING;
  ctx->playing = true;

  ret = pthread_create(&ctx->play_thread, NULL, audio_play_thread, ctx);
  if (ret != 0)
    {
      audio_close_play_device(ctx);
      audio_release_play_buffer(ctx);
      ctx->state = AUDIO_STATE_IDLE;
      ctx->playing = false;
      return -ret;
    }

  ctx->play_thread_valid = true;
  return 0;
}
'''

s = once(
    s, "static const char *g_state_names[] =",
    helper + "\nstatic const char *g_state_names[] ="
)

# 播放结束、写入失败或停止后，释放播放数据。
s = once(
    s, "  /* Callback runs before the worker is marked inactive.",
    "  audio_release_play_buffer(ctx);\n\n"
    "  /* Callback runs before the worker is marked inactive."
)

s = function(s, "audio_play_start", r'''
int audio_play_start(audio_context_t *ctx, const int16_t *data,
                     size_t frames, audio_play_complete_cb_t callback,
                     void *user_data)
{
  if (ctx == NULL || !ctx->initialized || data == NULL || frames == 0)
    return -EINVAL;

  if (ctx->playing || ctx->recording)
    return -EBUSY;

  if (frames >
      (size_t)AUDIO_DEFAULT_SAMPLE_RATE * AUDIO_PLAY_BUFFER_MS / 1000)
    return -ENOSPC;

  if (ctx->play_thread_valid)
    {
      int ret = pthread_join(ctx->play_thread, NULL);
      if (ret != 0) return -ret;
      ctx->play_thread_valid = false;
    }

  size_t bytes = frames * sizeof(int16_t);
  int16_t *buffer = malloc(bytes);
  if (buffer == NULL) return -ENOMEM;

  memcpy(buffer, data, bytes);
  ctx->play_buf = buffer;
  ctx->play_buf_size = bytes;
  return audio_launch_play(ctx, callback, user_data);
}
''')

# 文件只申请一份实际大小的缓存，不再复制第二份。
s = function(s, "audio_play_file", r'''
int audio_play_file(audio_context_t *ctx, const char *filepath,
                    audio_play_complete_cb_t callback, void *user_data)
{
  if (ctx == NULL || !ctx->initialized || filepath == NULL)
    return -EINVAL;

  if (ctx->playing || ctx->recording)
    return -EBUSY;

  if (ctx->play_thread_valid)
    {
      int ret = pthread_join(ctx->play_thread, NULL);
      if (ret != 0) return -ret;
      ctx->play_thread_valid = false;
    }

  FILE *file = fopen(filepath, "rb");
  if (file == NULL) return -errno;

  int ret = -EIO;
  int16_t *buffer = NULL;

  if (fseek(file, 0, SEEK_END) != 0) goto done;
  long length = ftell(file);
  if (length < 0) goto done;

  if (length == 0 || length % sizeof(int16_t) != 0)
    {
      ret = -EINVAL;
      goto done;
    }

  if ((unsigned long)length >
      (unsigned long)AUDIO_DEFAULT_SAMPLE_RATE *
      AUDIO_PLAY_BUFFER_MS / 1000 * 2)
    {
      ret = -EFBIG;
      goto done;
    }

  if (fseek(file, 0, SEEK_SET) != 0) goto done;

  buffer = malloc((size_t)length);
  if (buffer == NULL)
    {
      ret = -ENOMEM;
      goto done;
    }

  if (fread(buffer, 1, (size_t)length, file) != (size_t)length)
    goto done;

  if (fgetc(file) != EOF || ferror(file)) goto done;

  if ((length >= 4 && memcmp(buffer, "RIFF", 4) == 0) ||
      (length >= 3 && memcmp(buffer, "ID3", 3) == 0))
    {
      ret = -ENOTSUP;
      goto done;
    }

  ret = 0;

done:
  fclose(file);

  if (ret != 0)
    {
      free(buffer);
      return ret;
    }

  ctx->play_buf = buffer;
  ctx->play_buf_size = (size_t)length;
  return audio_launch_play(ctx, callback, user_data);
}
''')

# 完成资源和状态更新后，才发布“线程不再工作”的标志。
for field in ("playing", "recording"):
    s = re.sub(
        r"  ctx->" + field + r" = false;\n(  ctx->state = [^;]+;)",
        r"\1\n  ctx->" + field + " = false;",
        s
    )

# 对这三个跨线程状态字段统一使用原子读写。
for field in ("playing", "recording", "state"):
    s = re.sub(
        r"ctx->" + field + r" = ([^;]+);",
        lambda match: (
            "__atomic_store_n(&ctx->" + field + ", " +
            match.group(1) + ", __ATOMIC_RELEASE);"
        ),
        s
    )
    s = re.sub(
        r"(?<!&)\bctx->" + field + r"\b",
        "__atomic_load_n(&ctx->" + field + ", __ATOMIC_ACQUIRE)",
        s
    )

# 32KB 声音自检数组改为运行自检时申请。
old = """      static int16_t test_audio[SOUND_DETECT_FRAMES_PER_WINDOW];
      memset(test_audio, 0, sizeof(test_audio));"""

new = """      int16_t *test_audio = calloc(SOUND_DETECT_FRAMES_PER_WINDOW,
                                   sizeof(*test_audio));
      if (test_audio == NULL)
        {
          printf("[自检] 内存不足，跳过测试\\n");
        }
      else
        {"""

main = once(main, old, new)
main = once(
    main,
    '             ret == OK ? "成功" : "失败", ret);',
    '             ret == OK ? "成功" : "失败", ret);\n'
    '          free(test_audio);\n        }'
)

# 所有匹配完成后，先备份再写文件。
backup = Path(tempfile.mkdtemp(
    prefix="member2-memory-backup-", dir=Path.home()
))

for path in (p, m):
    shutil.copy2(path, backup / path.name)

p.write_text(s)
m.write_text(main)

print("内存优化已应用。备份位置：", backup)
