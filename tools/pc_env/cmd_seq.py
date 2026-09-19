import serial, time, sys

# 用法: py -3.10 cmd_seq.py <每条命令间隔秒> <输出文件> "命令1" "命令2" ...
# 逐条发送并等待间隔, 按 UTF-8 落盘 (避免 Windows 控制台 GBK 吃掉中文)

gap = float(sys.argv[1]) if len(sys.argv) > 1 else 8
out = sys.argv[2] if len(sys.argv) > 2 else 'cmd_seq.txt'
cmds = sys.argv[3:]
port = 'COM4'

s = serial.Serial(port, 1000000, timeout=0.2)
s.write(b'\r\n')
time.sleep(0.5)
while s.in_waiting:
    s.read(s.in_waiting)

buf = b''
for c in cmds:
    s.write(c.encode() + b'\r\n')
    end = time.time() + gap
    while time.time() < end:
        n = s.in_waiting
        if n:
            buf += s.read(n)
        else:
            time.sleep(0.05)
s.close()

txt = buf.decode('utf-8', 'replace').replace('\r', '')
with open(out, 'w', encoding='utf-8', errors='replace') as fp:
    fp.write(txt)
print('wrote %d bytes -> %s' % (len(txt), out))
