# 把文件放进 ROM（只读固件资源）—— 接口说明

## 一句话

**把文件拷进仓库的 `board/contest_board/src/etc/` 目录，重新编译烧录，它就出现在板子的 `/etc/` 下，只读。**

放演示素材统一放在子目录 `src/etc/assets/` 里，上板后路径是 `/etc/assets/`。

## 为什么是这样

板级 CMakeLists 里有这一段（`board/contest_board/src/CMakeLists.txt`）：

```cmake
nuttx_add_romfs(
  NAME etc
  MOUNTPOINT etc
  RCSRCS etc/init.d/rcS etc/init.d/rc.sysinit
  RCRAWS etc/group etc/1.txt
  PATH ${CMAKE_CURRENT_LIST_DIR}/etc)
```

`PATH` 那一行会把**整个 `src/etc/` 目录**拷进 ROMFS 镜像，构建时走：

```
src/etc/**  →  genromfs  →  romfs.img  →  xxd -i  →  romfs_etc.c  →  链进 nuttx.bin
```

所以**不需要改任何 CMakeLists**：往 `src/etc/` 里丢文件就会被打包。`RCSRCS` / `RCRAWS` 只是把系统启动脚本另外再列一次（那些走预处理、支持 `#include`），普通素材不用管。

启动时 NuttX 通过 `CONFIG_ETC_ROMFS=y` 自动把这份镜像挂到 `/etc`。

## 用法

### 1. 放文件

```bash
# 例：放一个开机提示音
cp ding.wav  board/contest_board/src/etc/assets/ding.wav

# 例：放一个目录
mkdir -p board/contest_board/src/etc/assets/faces
cp happy.bin board/contest_board/src/etc/assets/faces/
```

### 2. 编译烧录

```bash
bash /home/youdian/build_full.sh
bash D:/apply/claw/_flash/do_flash.sh
```

### 3. 上板确认

```text
nsh> ls -l /etc/assets
/etc/assets:
 -r--r--r--        1234 ding.wav
 dr-xr-xr-x           0 faces/
```

### 4. 在代码里读（标准 POSIX，和读普通文件一样）

```c
#include <fcntl.h>
#include <unistd.h>

#define DING_PATH "/etc/assets/ding.wav"

int play_ding(void)
{
  char buf[512];
  int fd = open(DING_PATH, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    {
      /* ROM 里没有这个文件：说明忘了拷进 src/etc/assets/ 或者没重新烧录 */
      return -errno;
    }

  for (;;)
    {
      ssize_t n = read(fd, buf, sizeof(buf));
      if (n <= 0)
        {
          break;              /* 0 = 读完了 */
        }

      /* ... 把 buf[0..n) 送进播放通道 ... */
    }

  close(fd);
  return 0;
}
```

没有特殊接口、没有私有 ioctl —— ROMFS 就是一个**标准只读文件系统**，用 `open/read/lseek/close` 即可，`stat()`、`opendir/readdir` 也都能用。

## 接口一览

| 用途 | 接口 | 说明 |
|---|---|---|
| 读文件 | `open("/etc/assets/<名>", O_RDONLY)` + `read()` | 标准 POSIX |
| 看属性/大小 | `stat()` / `fstat()` | `st_size` 就是原始文件字节数 |
| 列目录 | `opendir()` / `readdir()` | |
| 定位 | `lseek()` | 支持随机访问，可以只读文件的一段 |

## 特性与限制（重要）

| 项 | 说明 |
|---|---|
| **只读** | 权限是 `-r--r--r--`。改内容只能改仓库里的文件 + 重新烧录。要可写请看 `/data`。 |
| **不占 RAM** | 镜像在 flash 里，在 **XIP** 区被直接读。写东西还是要 `read()` 到缓冲区再处理。 |
| **容量** | flash 共 16MB，当前固件用掉约 1.6MB，余量很大。 |
| **断电不丢** | 因为它就在固件里。 |
| **改了要重烧** | 只改素材也要 `build_full.sh` + 烧录。只改 `src/etc/` 下的文件时，重编很快（只会重新生成 romfs）。 |
| **文件名** | 建议纯 ASCII（中文名在 genromfs/终端上容易出问题）。 |

## 和 `/data` 的分工

| | `/etc`（ROMFS） | `/data`（tmpfs） |
|---|---|---|
| 内容 | 编译时打进固件的**只读素材** | 运行时产生的**临时文件** |
| 可写 | ❌ | ✅（`open(O_WRONLY)`、`mkdir` 都行） |
| 断电保留 | ✅（本来就在固件里） | ❌ **只存在内存里，复位就没了** |
| 空间 | flash，16MB 里还剩 14MB+ | 从堆动态增长（堆约 8MB，和别的 malloc 共享） |
| 典型用途 | 提示音、图片、模型、默认配置、字体 | 录音临时文件、日志、运行期缓存 |

一句话：**素材放 `/etc`（跟着固件走），运行期产生的文件放 `/data`（丢了也无所谓）。**

## 排查

| 现象 | 原因 |
|---|---|
| `/etc/assets` 不存在 | 目录里没有任何文件，genromfs 不会生成空目录——至少放一个文件 |
| **放了新文件，但上板后整个目录都不在** | **构建系统没有重新生成 romfs。** 这是最容易踩的坑：`nuttx_add_romfs` 那条自定义命令的 `DEPENDS` 只跟踪 `RCSRCS` / `RCRAWS` 里**逐个列出的文件**，而 `PATH` 目录里文件的增删**它感知不到**。解决：`touch board/contest_board/src/etc/1.txt` 再重编（`1.txt` 在 `RCRAWS` 里），或者直接删掉构建目录里的 `romfs_etc.c`。 |
| 上板后文件是旧的 | 只烧了旧的 `nuttx.bin`；确认烧录的是刚编出来的那个（对比 md5/时间） |
| `open()` 返回 `ENOENT` | 文件名拼错（注意 `/etc/assets/` 前缀），或者忘了重新烧录 |
| 想验证 ROM 里的内容 | `md5_test /etc/assets/<名>` 可以和 PC 上 `md5sum` 的结果对比 |
