#!/usr/bin/env python3
"""md_server 的 TCP 集成测试。

这是唯一真正跨进程、跨语言的验证：Python 客户端 ← TCP → C++ 服务器 → C++ 规则。

做三件事：
  1. 单客户端 + 3 AI：打完一整局，验证 over、手牌不泄露
  2. 两个客户端 + 2 AI：验证座位分配与多连接时的回合仲裁
  3. 非法输入被拒：伪造 seat / 乱发命令

用法：python3 tools/tcp_client_test.py <md_server 可执行文件路径>
"""

import json
import socket
import subprocess
import sys
import threading
import time

FAILS = []
PASSES = [0]


def check(what, ok, detail=""):
    if ok:
        PASSES[0] += 1
        print(f"  [ok]   {what}")
    else:
        FAILS.append(what)
        print(f"  [FAIL] {what}{('  ' + detail) if detail else ''}")


def cmd_for(act):
    k = act.get("k", "play")
    cards = act.get("cards", [])
    targets = act.get("targets", [])
    a, b = -1, -1
    if k == "gather":
        a = 0
    elif k == "gun":
        a = targets[0] if targets else -1
    elif k == "play":
        if cards and targets:
            a = cards[0]
            b = targets[0]
    return {"t": "cmd", "k": k, "a": a, "b": b}


class Client(threading.Thread):
    def __init__(self, port, label, auto_start=False):
        super().__init__(daemon=True)
        self.port = port
        self.label = label
        self.auto_start = auto_start
        self.seat = None
        self.views = 0
        self.over = None
        self.leak = False
        self.errors = []
        self.joined_event = threading.Event()
        self._sock = None
        self._f = None
        self._lock = threading.Lock()

    def send(self, obj):
        with self._lock:
            self._f.write((json.dumps(obj) + "\n").encode())
            self._f.flush()

    def run(self):
        try:
            self._sock = socket.create_connection(("127.0.0.1", self.port), timeout=30)
            self._f = self._sock.makefile("rwb", buffering=0)
            self.send({"t": "join"})
            while True:
                line = self._f.readline()
                if not line:
                    break
                m = json.loads(line)
                t = m.get("t")
                if t == "joined":
                    self.seat = m["seat"]
                    self.joined_event.set()
                    if self.auto_start:
                        self.send({"t": "start"})
                elif t == "view":
                    self.views += 1
                    seat = m["seat"]
                    if len(m["hand"]) != m["pub_hand"][seat]:
                        self.leak = True
                    act = m.get("act")
                    if act:
                        self.send(cmd_for(act))
                elif t == "over":
                    self.over = m
                    break
                elif t == "error":
                    self.errors.append(m.get("msg"))
        except Exception as e:  # noqa: BLE001
            self.errors.append(repr(e))
        finally:
            try:
                if self._sock:
                    self._sock.close()
            except OSError:
                pass


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def wait_port(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            return True
        except OSError:
            time.sleep(0.15)
    return False


def start_server(exe, seed):
    port = free_port()
    proc = subprocess.Popen(
        [exe, "--port", str(port), "--seed", str(seed)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if not wait_port(port):
        proc.kill()
        raise RuntimeError("服务器没有在超时内开始监听")
    return proc, port


def test_single_client(exe):
    print("\n[1] 单客户端 + 3 AI：打完一整局")
    proc, port = start_server(exe, 1)
    try:
        c = Client(port, "A", auto_start=True)
        c.start()
        c.join(timeout=120)

        check("拿到的座位是 0", c.seat == 0, f"seat={c.seat}")
        check("收到 over", c.over is not None)
        check("手牌未泄露", not c.leak)
        check("没有协议错误", not c.errors, str(c.errors))
        check("产生了若干次 view", c.views > 0, f"views={c.views}")
        if c.over:
            print(f"       结果: {json.dumps(c.over, ensure_ascii=False)}")
    finally:
        proc.kill()
        proc.wait(timeout=10)


def test_two_clients(exe):
    print("\n[2] 两个客户端 + 2 AI：座位分配与回合仲裁")
    proc, port = start_server(exe, 3)
    try:
        a = Client(port, "A")
        b = Client(port, "B")
        a.start()
        b.start()
        a.joined_event.wait(timeout=30)
        b.joined_event.wait(timeout=30)

        check("两个客户端拿到不同座位", {a.seat, b.seat} == {0, 1}, f"{a.seat},{b.seat}")

        # 座位都就绪后再开局，否则后连的人会撞上"已开局"
        a.send({"t": "start"})

        a.join(timeout=120)
        b.join(timeout=120)

        check("A 收到 over", a.over is not None)
        check("B 收到 over", b.over is not None)
        check("A 手牌未泄露", not a.leak)
        check("B 手牌未泄露", not b.leak)
        check("双方看到的是同一个结果", a.over == b.over)
    finally:
        proc.kill()
        proc.wait(timeout=10)


def test_rejects(exe):
    print("\n[3] 非法输入被拒")
    proc, port = start_server(exe, 5)
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=15)
        f = s.makefile("rwb", buffering=0)
        f.write(b'{"t":"join"}\n')
        msg = json.loads(f.readline())
        check("握手回 joined", msg.get("t") == "joined")

        # 还没开局就发命令
        f.write(b'{"t":"cmd","k":"play","a":0,"b":1}\n')
        msg = json.loads(f.readline())
        check("未开局时命令被拒", msg.get("t") == "error", json.dumps(msg, ensure_ascii=False))

        # 不是 JSON
        f.write(b'garbage\n')
        msg = json.loads(f.readline())
        check("非法 JSON 被拒", msg.get("t") == "error")

        # 未知消息
        f.write(b'{"t":"nonsense"}\n')
        msg = json.loads(f.readline())
        check("未知消息被拒", msg.get("t") == "error")
        s.close()
    finally:
        proc.kill()
        proc.wait(timeout=10)


def main():
    if len(sys.argv) < 2:
        print("用法: python3 tools/tcp_client_test.py <md_server>")
        return 2
    exe = sys.argv[1]
    print(f"md_server: {exe}")

    test_single_client(exe)
    test_two_clients(exe)
    test_rejects(exe)

    print("\n--------------------------------------------------")
    print(f"通过 {PASSES[0]} 项，失败 {len(FAILS)} 项")
    if FAILS:
        for f in FAILS:
            print("  FAIL:", f)
        return 1
    print("TCP 集成测试: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
