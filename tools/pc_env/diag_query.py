"""向板子发 {"action":"diag"}，并把回执里的 hello_app 内部状态打出来。

用法： py -3.10 diag_query.py [broker ...]

topic / 字段名是照着固件核过的（app/robot_ui/network_comm.c）：
  请求  发到 zhi_ai/<client_id>/command    {"action":"diag"}
        （分发在 main.c 的 on_ai_command_received()，只认 action 字段；
          别名 voice_diag 等价）
  回执  回到 zhi_ai/<client_id>/status     {"type":"diag","device_id":...,
        "robot":{...},"hello":{...}}
        （report_diag() 拼的，QoS0、不 retain —— QoS0 丢了不会再发，
          没收到就重发一次请求，别改去订别的 topic）
  本机 client_id 是 zhi_ai_001（network_comm_init() 里写死的，换板子要跟着改）。

判据只认 JSON 里的 "type":"diag"：自己发出去的那条请求也会被 broker 回灌给
自己（MQTT 3.1.1 没有 no-local），拿 '"diag"' 当子串匹配的话，自己的回声会被
当成"收到回执" —— 于是"板子根本没回"这件事永远看不出来（2026-09-15 踩过）。
"""

import json
import socket
import sys
import time

BROKERS = sys.argv[1:] or ["broker.emqx.io", "test.mosquitto.org"]
CID = "zhi_ai_001"
CMD = "zhi_ai/%s/command" % CID
REPLY = "zhi_ai/%s/status" % CID
REQUEST = json.dumps({"action": "diag"})


def rl(n):
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        out.append(b | 0x80 if n > 0 else b)
        if n <= 0:
            return bytes(out)


def connect(broker, cid):
    s = socket.create_connection((broker, 1883), 8)
    s.settimeout(3)
    c = cid.encode()
    s.sendall(bytes([0x10]) + rl(10 + 2 + len(c)) + bytes([0, 4]) + b"MQTT" +
              bytes([4, 2, 0, 60]) + len(c).to_bytes(2, "big") + c)
    if not s.recv(4):
        raise RuntimeError("no CONNACK")
    return s


def subscribe(s, topic):
    t = topic.encode()
    s.sendall(bytes([0x82]) + rl(2 + 2 + len(t) + 1) + b"\x00\x01" +
              len(t).to_bytes(2, "big") + t + b"\x01")


def publish(s, topic, payload):
    t = topic.encode()
    p = payload.encode()
    body = len(t).to_bytes(2, "big") + t + p
    s.sendall(bytes([0x30]) + rl(len(body)) + body)


def is_diag(topic, payload):
    """是不是 report_diag() 真正发出来的那条（认 topic + type 字段，不认子串）。"""
    if topic != REPLY:
        return False
    try:
        obj = json.loads(payload)
    except Exception:
        return False
    return isinstance(obj, dict) and obj.get("type") == "diag"


for broker in BROKERS:
    print("=== %s ===" % broker, flush=True)
    try:
        s = connect(broker, "pc_diag")
    except Exception as e:
        print("  连接失败: %s" % e, flush=True)
        continue
    subscribe(s, "zhi_ai/#")
    time.sleep(0.4)
    print("  -> %s %s" % (CMD, REQUEST), flush=True)
    publish(s, CMD, REQUEST)
    end = time.time() + 12
    buf = bytearray()
    got = False
    others = []
    while time.time() < end:
        try:
            c = s.recv(4096)
        except socket.timeout:
            continue
        if not c:
            print("  broker 把连接关了", flush=True)
            break
        buf += c
        while len(buf) >= 2:
            hdr = buf[0]
            if hdr >> 4 != 3:
                # 非 PUBLISH：按剩余长度跳过
                mult, val, i = 1, 0, 1
                while i < len(buf) and buf[i] & 0x80:
                    val += (buf[i] & 0x7F) * mult
                    mult *= 128
                    i += 1
                if i >= len(buf):
                    break
                val += (buf[i] & 0x7F) * mult
                total = i + 1 + val
                if len(buf) < total:
                    break
                del buf[:total]
                continue
            mult, val, i = 1, 0, 1
            while i < len(buf):
                b = buf[i]
                val += (b & 0x7F) * mult
                mult *= 128
                i += 1
                if not b & 0x80:
                    break
            else:
                break
            total = i + val
            if len(buf) < total:
                break
            body = bytes(buf[i:total])
            del buf[:total]
            qos = (hdr >> 1) & 0x03
            tlen = int.from_bytes(body[0:2], "big")
            topic = body[2:2 + tlen].decode("utf-8", "replace")
            off = 2 + tlen + (2 if qos > 0 else 0)
            payload = body[off:].decode("utf-8", "replace")
            if topic == CMD and payload == REQUEST:
                continue                       # 自己发出去那条的回声
            if is_diag(topic, payload):
                got = True
                print("  [DIAG] %s" % payload, flush=True)
            else:
                others.append(topic)
    if not got:
        print("  （12 秒内没收到 diag 回执）", flush=True)
    if others:
        # 这条比"没收到回执"有用得多：板子在不在这一台上，看它有没有别的动作
        # 就知道 —— 一台都看不到 = 板子挂在别的 broker 上，别在这台上找原因。
        uniq = sorted(set(others))
        print("  同期还看到 %d 条别的发布，topic 前 3 个: %s"
              % (len(others), ", ".join(uniq[:3])), flush=True)
    try:
        s.close()
    except Exception:
        pass
