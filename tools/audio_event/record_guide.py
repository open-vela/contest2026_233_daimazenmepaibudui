#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/audio_event/record_guide.py —— 板端自录声音事件数据向导（跑在 Windows）

一句话：提示你出声 -> 板子录 2 秒 -> 自动把 PCM 取回 PC -> 转成 16 kHz 单声道 wav。

用法（在仓库根目录）：

    py -3.10 tools/audio_event/record_guide.py                     # 向导：四类按推荐顺序录
    py -3.10 tools/audio_event/record_guide.py --class knock --count 25
    py -3.10 tools/audio_event/record_guide.py --list              # 只看已录进度，不碰串口
    py -3.10 tools/audio_event/record_guide.py --dry-run --count 2 # 不连板子跑通全流程（自测）

为什么长这样（设计取舍，改之前先读）：

  1. 串口层不是从零写的 —— 读循环照抄 tools/pc_env/cmd_cap.py 里那套"先读到安静再发命令、
     发完一直读、绝不带着未读输出关端口"的规矩。cmd_cap.py 顶部那段注释解释了为什么：
     板子还在写的时候把 COM 口关掉，CH343 就不取数据，板侧 nsh 的 write() 会阻塞，
     之后发的命令全排在积压后面，现象是"命令好像没生效"，其实是几分钟前的输出在重播。
     所以本脚本：发命令前先 drain 到安静；收完一轮一定再 drain；退出前一定 drain。

  2. 裸 PCM 不走 `cat`。板端没有 base64/xxd 命令行（写了 mbedtls 的 base64 只在 mimo_voice.c
     内部用，没开放成 NSH 命令），而 `cat 裸 PCM` 的字节会被控制台/日志吃掉、也没法知道丢在哪。
     板上真正可用的是 NSH 自带的 **hexdump**（`CONFIG_NSH_CMDOPT_HEXDUMP=y`，
     apps/nshlib/nsh_dbgcmds.c 的 cmd_hexdump）：
         <file> at 00000000:      <- 这个 08x 是**文件绝对偏移**（无 skip/count 时 position 就是它）
         0000: 41 42 43 ...  ABC   <- 这个 4 位是**块内偏移**，每 16 字节一行
     于是每一行都能独立定位到文件偏移，日志插入/截断只会丢那一行，不会让后面所有数据错位。
     取回策略：整文件 hexdump 逐轮抓，按偏移合并，直到 [0, N) 连续无洞为止
     （默认最多 3 轮；丢一行就靠下一轮补，比单轮死等可靠）。
     注意 nsh 的 skip=/count= 有 bug（skip 跨块时 position 已经加上去了，头部偏移会指错），
     所以本脚本**只用整文件 hexdump**，绝不带 skip。

  3. 文件大小以板子自己打印的 `SAVED <n> bytes -> <file>` 为准（audio_test/main.c 里
     do_record()），拿不到就问 `ls -l`；两者都没有就按"连续前缀"处理并告警。

  4. /data 在本板是 **tmpfs（RAM）**（board/contest_board/src/sifli_ap.c 里 nx_mount("/data",
     "tmpfs")），重启就空，而且录一条 2 秒就是 64 KB 内存。所以默认**取回成功就 rm 板上的文件**，
     不要在板上囤几十条。

依赖：pyserial（3.5，本机已有）+ stdlib（wave/array/re/struct）。不装新包、不用 numpy。
"""

import argparse
import os
import re
import sys
import time
import wave
from array import array

try:
    import serial
except ImportError:              # --list / --dry-run 不需要串口
    serial = None

# ---------------------------------------------------------------------------
# 常量（契约固定值，改之前先跟训练侧对齐）
# ---------------------------------------------------------------------------

PORT_DEFAULT = 'COM4'            # 板子串口（CH343）
BAUD = 1000000                   # 1 Mbaud，不是 115200
BOARD_DIR = '/data/rec'          # 板上的临时目录（/data 是 tmpfs）
OUT_DEFAULT = 'D:/apply/claw/_audio_event/selfrecord'
SAMPLE_RATE = 16000

QUIET_AFTER = float(os.environ.get('GUIDE_QUIET_AFTER', '1.2'))
QUIET_BEFORE = float(os.environ.get('GUIDE_QUIET_BEFORE', '0.8'))

# 类别 -> (中文名, 怎么制造声音, 建议条数)
CLASS_INFO = {
    'fall':   ('跌倒撞击声',
               '把一本厚书或靠垫从桌面高度摔到地上（别摔板子和手机），一条里摔 1~2 次',
               20),
    'knock':  ('敲击声',
               '指节敲桌面三下（咚、咚、咚，间隔约 0.3 秒），板子在桌上或桌边',
               20),
    'scream': ('尖叫 / 呼救',
               '自己喊一声"救命"或"啊——"，离板子 1~3 米；也可以让家人喊',
               15),
    'other':  ('其它（负样本）',
               '正常说话、放 TTS 播报、关门、走路、搬东西、翻书……只要是"不该报警"的声音',
               20),
}
CLASS_ORDER = ['fall', 'knock', 'scream', 'other']

CAN_HDR_FMT = '{path} at {off:08x}:'
RE_DUMP = re.compile(r'^([0-9a-fA-F]{2,8}): ((?:[0-9a-fA-F]{2} ){1,16})')
RE_DUMP_ANY = re.compile(r'([0-9a-fA-F]{2,8}): ((?:[0-9a-fA-F]{2} ){8,16})')
RE_SAVED = re.compile(r'SAVED (\d+) bytes -> (\S+)')
RE_PEAK = re.compile(r'RECORD peak=(-?\d+) avg=(-?\d+)')
RE_READ = re.compile(r'READ done: (-?\d+) of (\d+) bytes')


def say(msg=''):
    print(msg, flush=True)


# ---------------------------------------------------------------------------
# hexdump 文本 -> 字节（纯函数，可单独测）
# ---------------------------------------------------------------------------

def contiguous(data):
    """从偏移 0 开始连续拿到了多少字节（= 可信前缀长度）。"""
    n = 0
    while n in data:
        n += 1
    return n


def parse_hexdump(text, path):
    """从一段 hexdump 文本里捡回 <path> 的字节。

    返回 (data: {偏移: 字节值}, stat: dict)：
      stat['lines']      成功解析的数据行数
      stat['lost']       因为日志污染而整行丢掉的行数（下一轮能补）
      stat['recovered']  靠"偏移连续"从被日志前缀污染的行里救回来的行数
    """
    hdr = re.compile(r'^' + re.escape(path) + r' at ([0-9a-fA-F]{8}):\s*$')
    hdr_any = re.compile(re.escape(path) + r' at ([0-9a-fA-F]{8}):\s*$')
    data = {}
    st = {'lines': 0, 'lost': 0, 'recovered': 0}
    base = None
    last_off = None
    first_in_chunk = False

    for raw in text.split('\n'):
        line = raw.rstrip('\r')
        m = hdr.match(line)
        if m is None and path in line:
            # 头部行前面被日志糊住了（行尾还是完整的），按"行尾锚定"再认一次
            m = hdr_any.search(line)
        if m:
            base = int(m.group(1), 16)
            last_off = None
            first_in_chunk = True
            continue
        if base is None:
            continue

        m = RE_DUMP.match(line)
        if m is None:
            # 行首被日志污染了：只在"偏移刚好接得上"时才认它 ——
            # 要么接在上一行后面，要么是这一块的第一行（内部偏移 0）。
            # 免得把日志里碰巧像 hexdump 的文本塞进错误的位置。
            if last_off is not None or first_in_chunk:
                for m2 in RE_DUMP_ANY.finditer(line):
                    off2 = base + int(m2.group(1), 16)
                    if off2 == last_off or (last_off is None and off2 == base):
                        m = m2
                        st['recovered'] += 1
                        break
            if m is None:
                # 纯日志行（没有像 hex 的东西）不算丢；像"半行"的才记：
                # 要么是丢掉了行首的数据行，要么是被切断后剩下的十六进制尾巴
                if re.match(r'^[0-9a-fA-F]{2,8}: ', line) or \
                        re.match(r'^[0-9a-fA-F]{2} [0-9a-fA-F]{2} ', line):
                    st['lost'] += 1
                continue

        off = base + int(m.group(1), 16)
        for i, tok in enumerate(m.group(2).split()):
            data.setdefault(off + i, int(tok, 16))
        last_off = off + 16
        first_in_chunk = False
        st['lines'] += 1

    return data, st


def _merge(dst, src, st):
    for k, v in src.items():
        old = dst.get(k)
        if old is None:
            dst[k] = v
        elif old != v:
            st['conflict'] += 1
    return dst


# ---------------------------------------------------------------------------
# 真板子：串口读写
# ---------------------------------------------------------------------------

class Board:
    """1 Mbaud 串口上的 NSH 会话。读循环规矩见文件头第 1 条。"""

    name = 'board'

    def __init__(self, port, baud=BAUD):
        if serial is None:
            raise RuntimeError('没有 pyserial，装一个：py -3.10 -m pip install pyserial')
        self.port = port
        self.baud = baud
        self.ser = serial.Serial(port, baud, timeout=0.05)
        try:
            self.ser.set_buffer_size(rx_size=1 << 18, tx_size=1 << 12)
        except Exception:
            pass
        self.buf = b''
        self.t0 = time.time()

    # --- 基础 ---
    def text(self):
        return self.buf.decode('utf-8', 'replace').replace('\r', '')

    def _wait(self, max_wait, stop_when=None, quiet_exit=True, min_quiet=None):
        t0 = time.time()
        last = time.time()
        got = False
        while True:
            d = self.ser.read(65536)
            now = time.time()
            if d:
                self.buf += d
                got = True
                last = now
                if stop_when is not None and stop_when(self.text()):
                    break
            else:
                time.sleep(0.02)
                now = time.time()
            if now - t0 >= max_wait:
                break
            if quiet_exit and got and (now - last) >= (min_quiet or QUIET_AFTER):
                if stop_when is None or (now - last) >= max(min_quiet or 0.0, 2.0):
                    break
            if now - last >= max_wait:
                break
        return self.text()

    def drain(self, max_wait=4.0):
        """一直读到安静。发命令前、退出前都要来一次。"""
        self._wait(max_wait, None, True)
        self.buf = b''

    def flush_pending(self, max_wait=0.15):
        """把上一轮留下的尾巴（提示符之类）读掉，别让新命令排在积压后面。"""
        t0 = time.time()
        while time.time() - t0 < max_wait:
            if not self.ser.read(65536):
                break

    def send(self, cmd):
        self.flush_pending()
        self.ser.write(b'\r\n')
        time.sleep(0.15)
        self.ser.write(cmd.encode() + b'\r\n')

    def run(self, cmd, wait_s=3.0, stop_when=None, quiet_exit=True):
        self.buf = b''
        self.send(cmd)
        return self._wait(wait_s, stop_when, quiet_exit)

    def close(self):
        try:
            self.drain(3.0)
        finally:
            try:
                self.ser.close()
            except Exception:
                pass

    # --- 业务 ---
    def probe(self):
        """探一下 nsh 活着没、hexdump 在不在。返回 (alive, has_hexdump, 文本)。"""
        txt = self.run('help', wait_s=6.0, quiet_exit=True)
        alive = len(txt.strip()) > 40 or 'nsh>' in txt
        return (alive, 'hexdump' in txt, txt)


# ---------------------------------------------------------------------------
# 假板子：--dry-run 用，不碰 COM4，但把 hexdump 文本真造出来喂给解析器
# ---------------------------------------------------------------------------

class DryBoard:
    """模拟板子的 NSH：audio_test record / hexdump / rm，并故意往 hexdump 文本里
    插日志行、偶尔把一行截断 —— 用来证明解析器在有污染时也拼得回来。"""

    name = 'dry-run'

    def __init__(self, pollute=True, chunk=256):
        self.files = {}
        self.pollute = pollute
        self.chunk = chunk
        self.rng = 12345
        self.n = 0

    def _rand(self):
        self.rng = (self.rng * 1103515245 + 12345) & 0x7fffffff
        return self.rng

    def _fake_pcm(self, nsamples, peak=2600):
        out = array('h')
        for i in range(nsamples):
            s = 0.6 * peak * (1 if (i // 40) % 2 else -1) + (self._rand() % 64 - 32)
            s += 0.3 * peak * ((i % 160) / 160.0)
            out.append(max(-32768, min(32767, int(s))))
        return out.tobytes()

    def _dump_text(self, data, path, split_prob):
        lines = []
        if self.pollute:
            lines.append('nsh> ')
        for off in range(0, len(data), self.chunk):
            blk = data[off:off + self.chunk]
            lines.append(CAN_HDR_FMT.format(path=path, off=off))
            for i in range(0, len(blk), 16):
                grp = blk[i:i + 16]
                txt = '%04x: ' % i
                txt += ''.join('%02x ' % b for b in grp)
                txt += '   ' * (16 - len(grp))
                txt += ''.join(chr(b) if 0x20 <= b <= 0x7e else '.' for b in grp)
                if self.pollute:
                    # 真污染主要是"整行日志插在中间"（syslog 一次写一行）
                    if self._rand() % 7 == 0:
                        lines.append('rndis_rx: usbclass_ep0 3bytes evt=1 len=%d' % (self._rand() % 900))
                    if self._rand() % 100 < split_prob:
                        cut = 12 + self._rand() % 30
                        lines.append(txt[:cut])
                        lines.append(txt[cut:])
                        continue
                lines.append(txt)
            if self.pollute:
                lines.append('[AI] 陪伴任务心跳 seq=%d' % (self.n + 1))
        lines.append('nsh> ')
        return '\r\n'.join(lines) + '\r\n'

    def probe(self):
        time.sleep(0.05)
        return True, True, 'nsh> help\n  hexdump  <file>\n  cat  <file>\nnsh> '

    def drain(self, max_wait=4.0):
        return ''

    def close(self):
        pass

    def run(self, cmd, wait_s=3.0, stop_when=None, quiet_exit=True):
        self.n += 1
        time.sleep(0.01)
        parts = cmd.split()
        if not parts:
            return ''
        if parts[0] == 'mkdir':
            return ''
        if parts[0] == 'rm':
            self.files.pop(parts[1], None)
            return ''
        if parts[0] == 'audio_test' and len(parts) > 1 and parts[1] == 'record':
            ms = int(parts[2])
            path = parts[3]
            ns = SAMPLE_RATE * ms // 1000
            pcm = self._fake_pcm(ns)
            self.files[path] = pcm
            return ('audio_test: record %dms -> %s\n'
                    'READ done: %d of %d bytes\n'
                    'RECORD peak=2600 avg=310 (16k mono 16bit)\n'
                    'RECORD OK: 检测到声音\n'
                    'STOP done\n'
                    'SAVED %d bytes -> %s\n'
                    % (ms, path, len(pcm), len(pcm), len(pcm), path))
        if parts[0] == 'hexdump':
            path = parts[1]
            if path not in self.files:
                return 'hexdump: open failed: 2\n'
            return self._dump_text(self.files[path], path, split_prob=2)
        return ''


# ---------------------------------------------------------------------------
# 取回 / 存盘
# ---------------------------------------------------------------------------

def fetch_pcm(board, path, expected, passes=3, verbose=True):
    """hexdump 逐轮抓 -> 按偏移合并 -> 返回 (pcm_bytes, info)。

    info = {got, expected, exact, rounds, lost, conflict}
    """
    st = {'conflict': 0}
    merged = {}
    info = {'got': 0, 'expected': expected, 'exact': False, 'rounds': 0,
            'lost': 0, 'conflict': 0}

    for rnd in range(1, passes + 1):
        info['rounds'] = rnd
        prog = {'seen': 0}

        def enough(txt, prog=prog):
            if len(txt) - prog['seen'] < 8192:
                return False
            prog['seen'] = len(txt)
            d, _ = parse_hexdump(txt, path)
            _merge(merged, d, st)
            return contiguous(merged) >= expected if expected else False

        nbytes = expected if expected else 200000
        # 1 Mbaud 实测 ~64 KB/s；hex 文本约 4.4 倍膨胀，留足余量
        wait_s = 4.0 + nbytes * 4.6 / 6000.0
        txt = board.run('hexdump ' + path, wait_s=wait_s,
                        stop_when=(enough if expected else None),
                        quiet_exit=True)
        d, dst = parse_hexdump(txt, path)
        _merge(merged, d, st)
        info['lost'] += dst['lost']
        got = contiguous(merged)
        holes = len(merged) - got
        if verbose:
            pct = (100.0 * got / expected) if expected else 0.0
            extra = ('%d/%d 字节 (%.0f%%)' % (got, expected, pct)) if expected \
                else ('%d 字节' % got)
            note = ''
            if dst['lost']:
                note += '，被日志吃掉 %d 行' % dst['lost']
            if dst['recovered']:
                note += '，救回 %d 行' % dst['recovered']
            say('    [取回] 第 %d 轮：%s%s' % (rnd, extra, note))
        if got == 0:
            return b'', info
        if expected and got >= expected:
            info['exact'] = True
            break
        if not expected and not holes:
            break
        if got and not holes and rnd >= 2:
            break

    got = contiguous(merged)
    info['got'] = got
    info['conflict'] = st['conflict']
    if st['conflict']:
        say('    [!] 有 %d 个字节两轮取值不一致（当次传输有问题，建议重录）' % st['conflict'])
    pcm = bytes(merged[i] for i in range(got))
    return pcm, info


def pcm_stats(pcm):
    """峰值 / RMS（int16）。"""
    a = array('h')
    a.frombytes(pcm[:len(pcm) // 2 * 2])
    if not a:
        return 0, 0.0
    peak = 0
    s = 0.0
    for v in a:
        if v < 0:
            v = -v
        if v > peak:
            peak = v
        s += float(v) * v
    return peak, (s / len(a)) ** 0.5


def write_wav(path, pcm):
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(pcm)


def class_files(out_root, cls):
    d = os.path.join(out_root, cls)
    if not os.path.isdir(d):
        return []
    return sorted(f for f in os.listdir(d) if f.lower().endswith('.wav'))


def next_index(out_root, cls):
    mx = 0
    for f in class_files(out_root, cls):
        m = re.search(r'_(\d+)\.wav$', f)
        if m:
            mx = max(mx, int(m.group(1)))
    return mx + 1


def show_progress(out_root):
    say('')
    say('  PC 上的自录数据（%s）：' % out_root)
    total = 0
    for cls in CLASS_ORDER:
        n = len(class_files(out_root, cls))
        total += n
        bar = '#' * min(n, 20) + '-' * max(0, 20 - n)
        say('    %-7s [%s] %3d 条   （%s，建议 %d）'
            % (cls, bar, n, CLASS_INFO[cls][0], CLASS_INFO[cls][2]))
    if os.path.isdir(out_root):
        others = [d for d in os.listdir(out_root)
                  if os.path.isdir(os.path.join(out_root, d)) and d not in CLASS_ORDER]
        for d in others:
            n = len([f for f in os.listdir(os.path.join(out_root, d))
                     if f.lower().endswith('.wav')])
            total += n
            say('    %-7s %3d 条（非本向导类别）' % (d, n))
    say('    合计 %d 条 wav' % total)
    say('')


def append_index(out_root, row):
    p = os.path.join(out_root, 'index.csv')
    new = not os.path.exists(p)
    with open(p, 'a', encoding='utf-8', newline='') as fp:
        if new:
            fp.write('class,file,index,seconds,bytes,peak_pc,rms_pc,peak_board,'
                     'board_path,ts\n')
        fp.write(','.join(str(x) for x in row) + '\n')


# ---------------------------------------------------------------------------
# 一条录音的完整流程
# ---------------------------------------------------------------------------

def one_take(board, args, cls, idx):
    """录一条 -> 取回 -> 转 wav。返回 (wav路径, 峰值) 或 (None, 原因)。"""
    bpath = '%s/%s_%02d.pcm' % (BOARD_DIR, cls, idx)
    ms = int(args.seconds * 1000)

    say('  >>> 现在做动作！（录 %.2f 秒，动作做在开头就行）' % args.seconds)
    txt = board.run('audio_test record %d %s' % (ms, bpath),
                    wait_s=args.seconds + 3.0,
                    stop_when=lambda t: 'SAVED' in t,
                    quiet_exit=False)

    m = RE_SAVED.search(txt)
    peak_b = None
    mp = RE_PEAK.search(txt)
    if mp:
        peak_b = int(mp.group(1))
    mr = RE_READ.search(txt)
    expect = None
    if m:
        expect = int(m.group(1))
    elif mr:
        expect = max(0, int(mr.group(1)))
    if m is None:
        if 'RECORD 静音' in txt or 'peak=0' in txt:
            return None, '板子说没录到声音（麦克风通路或增益问题）'
        tail = '\n'.join(l for l in txt.split('\n') if l.strip())[-300:]
        return None, ('没等到 SAVED 行 —— 板子可能没写成功（麦克风被占用？唤醒/播放中？）\n'
                      '      板子最后几行：\n        ' + tail.replace('\n', '\n        '))
    if expect is None:
        expect = int(args.seconds * SAMPLE_RATE) * 2
    say('    板子存了 %d 字节（%.2f 秒），板端 peak=%s'
        % (expect, expect / 2.0 / SAMPLE_RATE, peak_b))

    pcm, info = fetch_pcm(board, bpath, expect, passes=args.passes)
    if not pcm:
        return None, ('一个字节都没取回来 —— hexdump 不可用/文件不存在/串口被占'
                      '（先试 --list 和 `help` 里有没有 hexdump）')

    if not args.no_delete:
        board.run('rm ' + bpath, wait_s=1.5, quiet_exit=True)

    if info['got'] < expect:
        say('    [!] 只拼回 %d/%d 字节（%.0f%%）—— 训练时这条会短 %.2f 秒，'
            '要么重录要么留着当短样本'
            % (info['got'], expect, 100.0 * info['got'] / expect,
               (expect - info['got']) / 2.0 / SAMPLE_RATE))

    peak, rms = pcm_stats(pcm)
    outdir = os.path.join(args.out, cls)
    os.makedirs(outdir, exist_ok=True)
    wav = os.path.join(outdir, '%s_%02d.wav' % (cls, idx))
    write_wav(wav, pcm)

    if peak < args.min_peak:
        os.remove(wav)
        return None, ('这一条峰值只有 %d（< %d），基本是静音 —— 已丢弃，重来一次'
                      '（麦克风离远/增益小/动作没做上）' % (peak, args.min_peak))

    warn = ''
    if peak >= 32700:
        warn = '（削顶了，麦克风增益偏大，考虑 audio_test vol 调小）'
    say('    [OK] %s  %.2f 秒  峰值 %d  RMS %.0f  %s'
        % (os.path.basename(wav), len(pcm) / 2.0 / SAMPLE_RATE, peak, rms, warn))
    say('    [文件] %s' % os.path.abspath(wav))

    append_index(args.out,
                 [cls, os.path.basename(wav), idx,
                  '%.2f' % (len(pcm) / 2.0 / SAMPLE_RATE), len(pcm), peak,
                  '%.0f' % rms, peak_b if peak_b is not None else '',
                  bpath, time.strftime('%Y-%m-%d %H:%M:%S')])
    return wav, None


def do_class(board, args, cls):
    name, howto, suggest = CLASS_INFO[cls]
    target = args.count
    say('')
    say('=' * 66)
    say('  类别：%s —— %s' % (cls, name))
    say('  做法：%s' % howto)
    say('  目标：%d 条（每条 %.2f 秒）；参考建议 %d 条' % (target, args.seconds, suggest))
    have = len(class_files(args.out, cls))
    say('  PC 上已有 %d 条' % have)
    say('=' * 66)

    done = 0
    while done < target:
        left = target - done
        if not args.auto:
            try:
                key = input('\n[%s %d/%d] 准备好就按回车开始录（q=换下一类，'
                            'l=看进度）> ' % (cls, done + 1, target))
            except EOFError:
                key = 'q'
            k = key.strip().lower()
            if k == 'q':
                break
            if k == 'l':
                show_progress(args.out)
                continue
        idx = next_index(args.out, cls)
        try:
            got, why = one_take(board, args, cls, idx)
        except Exception as e:                      # 单条失败不该把整轮带崩
            say('    [!] 这一条出错：%s（跳过，重来一条）' % e)
            continue
        if why:
            say('    [!] %s' % why)
            if args.fail_fast:
                break
            continue
        done += 1
        n = len(class_files(args.out, cls))
        bar = '#' * min(n, 20) + '-' * max(0, 20 - n)
        say('    进度 %s [%s] 本类 %d/%d，PC 上共 %d 条'
            % (cls, bar, done, target, n))
    say('  —— %s 本次录了 %d 条' % (cls, done))
    return done


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def build_parser():
    p = argparse.ArgumentParser(
        description='板端自录声音事件数据向导（录 -> 串口取回 -> 转 16k 单声道 wav）',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='例：py -3.10 tools/audio_event/record_guide.py --class knock --count 25\n'
               '    py -3.10 tools/audio_event/record_guide.py --dry-run --count 2 --auto')
    p.add_argument('--class', dest='classes', action='append', default=None,
                   help='要录的类别，可多次给或用逗号分隔：fall,knock,scream,other'
                        '（默认按推荐顺序全录）')
    p.add_argument('--count', type=int, default=20, help='每类录几条（默认 20）')
    p.add_argument('--seconds', type=float, default=2.0, help='每条录多久（默认 2 秒）')
    p.add_argument('--port', default=PORT_DEFAULT, help='串口（默认 %s）' % PORT_DEFAULT)
    p.add_argument('--baud', type=int, default=BAUD, help='波特率（默认 1000000）')
    p.add_argument('--out', default=OUT_DEFAULT,
                   help='PC 上的输出根目录（默认 %s）' % OUT_DEFAULT)
    p.add_argument('--gain', type=int, default=None,
                   help='先用 audio_test vol <0..1000> 设录音增益（不设就不动板子）')
    p.add_argument('--passes', type=int, default=3, help='每个文件最多抓几轮（默认 3）')
    p.add_argument('--min-peak', type=int, default=150,
                   help='峰值低于此值就当静音丢弃（默认 150）')
    p.add_argument('--no-delete', action='store_true',
                   help='取回后不删板上的 pcm（默认删；/data 是 tmpfs，别囤）')
    p.add_argument('--auto', action='store_true', help='不等人按回车（配合 --dry-run）')
    p.add_argument('--fail-fast', action='store_true', help='一条失败就停本类')
    p.add_argument('--list', action='store_true', help='只看 PC 上已录进度，不碰串口')
    p.add_argument('--dry-run', action='store_true', help='不连板子，跑通全流程（自测）')
    return p


def parse_classes(args):
    classes = []
    for v in (args.classes or CLASS_ORDER):
        for c in v.split(','):
            c = c.strip().lower()
            if not c:
                continue
            if c not in CLASS_INFO:
                raise SystemExit('不认识的类别 %r（可选：%s）' % (c, '/'.join(CLASS_ORDER)))
            if c not in classes:
                classes.append(c)
    return classes


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors='replace')   # 中文打不出来时退化成 ?，别为编码崩掉
        except Exception:
            pass

    args = build_parser().parse_args(argv)
    classes = parse_classes(args)
    try:
        os.makedirs(args.out, exist_ok=True)
    except Exception as e:
        say('[错误] 建不了输出目录 %s：%s' % (args.out, e))
        say('       --out 要用 Windows 路径（D:/... 或 D:\\...），别用 /d/... 这种。')
        return 2

    say('=' * 66)
    say('  声音事件自录向导   ——   录 -> 取回 -> 16k 单声道 wav')
    say('  串口 %s @ %d    板上目录 %s' % (args.port, args.baud, BOARD_DIR))
    say('  PC 输出 %s' % args.out)
    say('=' * 66)

    if args.list:
        show_progress(args.out)
        return 0

    try:
        board = DryBoard() if args.dry_run else Board(args.port, args.baud)
    except Exception as e:
        say('[错误] 打不开串口 %s：%s' % (args.port, e))
        say('       COM 口被占用时先关掉 serial_term.py / lcd_mirror.py --serial COM4 /'
            ' cmd_cap.py 这些工具；')
        say('       板子没插上就换一个口：--port COMx；只想看进度加 --list，'
            '不连板子自测加 --dry-run。')
        return 2

    saved = 0
    try:
        if not args.dry_run:
            say('[1/3] 打开串口 %s @ %d ...' % (args.port, args.baud))
            say('      （COM 口被别的工具占着会直接报错，先把串口工具关掉）')
            alive, has_hexdump, txt = board.probe()
            say('      nsh %s；hexdump %s'
                % ('活着' if alive else '没回话', '可用' if has_hexdump else '没找到'))
            if not alive:
                say('      [!] 串口打开成功但 nsh 没回话 —— 检查板子是不是在跑、'
                    '波特率是不是 1000000')
            if not has_hexdump:
                say('      [!] help 里没看到 hexdump。取回数据要靠它'
                    '（CONFIG_NSH_CMDOPT_HEXDUMP=y）；继续跑但很可能取不回来。')
            say('[2/3] 准备板上目录 %s' % BOARD_DIR)
            board.run('mkdir %s' % BOARD_DIR, wait_s=1.5, quiet_exit=True)
            if args.gain is not None:
                say('      设录音增益 %d/1000' % args.gain)
                say('      ' + board.run('audio_test vol %d' % args.gain, 1.5).strip())
            say('[3/3] 开始录（每条提示后按回车；q 换下一类）')
        else:
            say('[dry-run] 不碰串口：用假板子造 hexdump 文本（含日志污染/截断）验证全流程')

        for cls in classes:
            saved += do_class(board, args, cls)
    except KeyboardInterrupt:
        say('\n[中断] 收尾中……')
    except Exception as e:
        say('\n[错误] %s' % e)
    finally:
        try:
            board.close()
        except Exception:
            pass

    show_progress(args.out)
    say('本次共落盘 %d 条 wav。' % saved)
    if not args.dry_run:
        say('下一步：把 %s\\<class>\\*.wav 喂给训练脚本；'
            '这些数据是"我们自己的麦克风域"，和公开数据集混着用。' % args.out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
