import os
import serial, time, sys

# 用法: py -3.10 cmd_cap.py <秒> "<命令>" [输出文件]
# 目的: 发一条命令并抓原始字节, 按 UTF-8 落盘 (避免 Windows 控制台 GBK 把中文吃掉)
#
# 2026-09-17 重写读循环, 治一个把现场搞得很乱的坑:
#   以前是"睡 0.5s → 读光 → 发命令 → 固定读 N 秒 → 关端口"。
#   板子还在写输出的时候把 COM 口关掉, CH343 就不再取数据, 板子侧 nsh 的
#   write() 会阻塞 —— 之后我发的命令全排在输入队列里, 现象是"命令好像没生效",
#   实际是几分钟前的输出在重播; 反复几次能积压几百 KB。
#   现在的规矩:
#     ① 发命令前先**读到安静**(连续 QUIET_BEFORE 秒没有新字节) 才发 —— 保证命令
#        一进去就被执行, 不会埋在旧积压后面;
#     ② 发完之后一直读, 直到"安静 + 已经收到过东西"或到时间上限 —— 绝不带着
#        未读输出关端口;
#     ③ 端口开着的时候 set_buffer_size 开大, 防止主机侧先丢字节。
QUIET_AFTER = float(os.environ.get('CAP_QUIET_AFTER', '1.2'))  # 收到过东西之后, 连续这么久没新字节就算说完了
QUIET_BEFORE = float(os.environ.get('CAP_QUIET_BEFORE', '0.8'))  # 发命令前要求的安静时长
# 注意: net_test 这类命令里有"DNS 最多等 30 秒"的静默段, 用默认的 1.2 秒会在
# 静默段提前退出、抓不到结果。要它用 CAP_QUIET_AFTER=8 这样放大。

secs = int(sys.argv[1]) if len(sys.argv) > 1 else 20
cmd = sys.argv[2] if len(sys.argv) > 2 else ''
out = sys.argv[3] if len(sys.argv) > 3 else 'cmd_capture.txt'

s = serial.Serial('COM4', 1000000, timeout=0.05)
try:
    s.set_buffer_size(rx_size=1 << 18, tx_size=1 << 12)
except Exception:
    pass


def read_until_quiet(max_wait, require_data):
    """读到安静为止, 返回 (新增字节, 是否收到过数据)。"""
    buf = b''
    got = False
    q = 0.0
    t0 = time.time()
    while time.time() - t0 < max_wait:
        d = s.read(65536)
        if d:
            buf += d
            got = True
            q = 0.0
        else:
            q += 0.05
            need = QUIET_AFTER if got else QUIET_BEFORE
            if q >= need and (got or not require_data):
                break
    return buf, got


# 1) 先排空旧输出（读到安静才动手）
pre, _ = read_until_quiet(6.0, False)

# 2) 发命令
s.write(b'\r\n')
time.sleep(0.2)
s.write(cmd.encode() + b'\r\n')

# 3) 读到"安静 + 有数据"，最多 secs 秒
body, got = read_until_quiet(float(secs), True)
s.close()

txt = (pre + body).decode('utf-8', 'replace').replace('\r', '')
with open(out, 'w', encoding='utf-8', errors='replace') as fp:
    fp.write(txt)
print('wrote %d bytes -> %s (pre=%d, body=%d)'
      % (len(txt), out, len(pre), len(body)))
