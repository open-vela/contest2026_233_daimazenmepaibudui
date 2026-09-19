import serial, time, sys

secs = int(sys.argv[1]) if len(sys.argv) > 1 else 12
s = serial.Serial('COM4', 1000000, timeout=0.2)
s.write(b'\r\n')
time.sleep(0.3)
s.write(b'\r\n')          # 敲个回车，看 NSH 是否活着
end = time.time() + secs
buf = b''
while time.time() < end:
    n = s.in_waiting
    if n:
        buf += s.read(n)
    else:
        time.sleep(0.05)
s.close()

txt = buf.decode('utf-8', 'replace').replace('\r', '')
open('raw_capture.txt', 'w', encoding='utf-8', errors='replace').write(txt)
lines = [l.rstrip() for l in txt.split('\n')]
# 只过滤最常见的 RNDIS 噪声，其它全打印，便于看崩溃/断言
out = [l for l in lines if l.strip() and not l.lstrip().startswith(('usbclass_', 'rndis_'))]
print('\n'.join(out[-90:]))
