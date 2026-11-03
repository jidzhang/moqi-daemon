#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Cloud clipboard / WebDAV user-dict sync end-to-end (local fake server).

Requires F:\\work\\IME\\fake_webdav.py (Basic auth u1/p1, 127.0.0.1:18077).
Backs up/restores %APPDATA%\\Moqi like audit_scenarios.py.
"""
import base64
import ctypes
import http.client
import json
import os
import shutil
import struct
import subprocess
import sys
import time

sys.stdout.reconfigure(encoding="utf-8")
kernel32 = ctypes.windll.kernel32
APPDATA = os.environ["APPDATA"]
MOQI = os.path.join(APPDATA, "Moqi")
BACKUP = os.path.join(r"F:\work\IME", "audit-profile-backup-webdav")
PIPE = r"\\.\pipe\%s\MoqiIM\Launcher" % os.environ.get("USERNAME", "")
FAKE = r"F:\work\IME\fake_webdav.py"
BASE = "http://127.0.0.1:18077"
USER, PASS = "u1", "p1"
results = []


def check(name, cond, detail=""):
    results.append((name, bool(cond), detail))
    print(("PASS " if cond else "FAIL ") + name + ((" :: " + detail) if detail and not cond else ""))


def auth_header():
    return "Basic " + base64.b64encode((USER + ":" + PASS).encode()).decode()


def dav(method, path, body=None, depth=None):
    conn = http.client.HTTPConnection("127.0.0.1", 18077, timeout=10)
    headers = {"Authorization": auth_header()}
    if depth is not None:
        headers["Depth"] = str(depth)
    conn.request(method, path, body=body, headers=headers)
    r = conn.getresponse()
    data = r.read()
    conn.close()
    return r.status, data


class Pipe:
    def __init__(self):
        self.h = kernel32.CreateFileW(PIPE, 0x80000000 | 0x40000000, 0, None, 3, 0, None)
        if self.h in (-1, 0xFFFFFFFF):
            raise RuntimeError("pipe connect failed")
        self.seq = 300

    def close(self):
        if self.h not in (-1, 0xFFFFFFFF):
            kernel32.CloseHandle(self.h)
            self.h = -1

    def _read(self):
        hdr = b""
        while len(hdr) < 4:
            buf = ctypes.create_string_buffer(4 - len(hdr))
            got = ctypes.c_ulong(0)
            if not kernel32.ReadFile(self.h, buf, 4 - len(hdr), ctypes.byref(got), None) or got.value == 0:
                return None
            hdr += buf.raw[:got.value]
        n = struct.unpack("<I", hdr)[0]
        body = b""
        while len(body) < n:
            buf = ctypes.create_string_buffer(n - len(body))
            got = ctypes.c_ulong(0)
            if not kernel32.ReadFile(self.h, buf, n - len(body), ctypes.byref(got), None) or got.value == 0:
                return None
            body += buf.raw[:got.value]
        return json.loads(body.decode("utf-8"))

    def try_read(self):
        avail = ctypes.c_ulong(0)
        if not kernel32.PeekNamedPipe(self.h, None, 0, None, ctypes.byref(avail), None):
            return None
        if avail.value == 0:
            return None
        return self._read()

    def call(self, obj, timeout=20):
        self.seq += 1
        obj["seqNum"] = self.seq
        p = json.dumps(obj).encode()
        f = struct.pack("<I", len(p)) + p
        w = ctypes.c_ulong(0)
        kernel32.WriteFile(self.h, f, len(f), ctypes.byref(w), None)
        deadline = time.time() + timeout
        extra = []
        while time.time() < deadline:
            x = self._read()
            if x is None:
                return None, extra
            if x.get("seqNum") == self.seq:
                return x, extra
            extra.append(x)
        return None, extra


def key(vk, ch, ctrl=False):
    st = [0] * 256
    st[vk] = 0x80
    if ctrl:
        st[0x11] = 0x80
    return {"keyCode": vk, "charCode": ch, "scanCode": 0, "isExtended": False,
            "repeatCount": 1, "keyStates": st}


def stop_daemon():
    try:
        p = Pipe()
        try:
            p.call({"method": "METHOD_ON_COMMAND", "clientId": "stop", "commandId": 33}, timeout=2)
        finally:
            p.close()
    except Exception:
        pass
    time.sleep(1.2)
    subprocess.run(["taskkill", "/F", "/IM", "MoqiDaemon.exe"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.8)


def start_daemon(exe, program_dir):
    stop_daemon()
    env = os.environ.copy()
    env["MOQI_PROGRAM_DIR"] = program_dir
    env["MOQI_RIME_DLL_DIR"] = program_dir
    subprocess.Popen([exe, "--serve"], env=env, close_fds=True, creationflags=0x8)
    deadline = time.time() + 40
    while time.time() < deadline:
        h = kernel32.CreateFileW(PIPE, 0x80000000 | 0x40000000, 0, None, 3, 0, None)
        if h not in (-1, 0xFFFFFFFF):
            kernel32.CloseHandle(h)
            time.sleep(1.5)
            return
        time.sleep(0.5)
    raise RuntimeError("daemon pipe not ready")


def backup():
    stop_daemon()
    if os.path.exists(BACKUP):
        shutil.rmtree(BACKUP)
    if os.path.exists(MOQI):
        shutil.copytree(MOQI, BACKUP)


def restore():
    stop_daemon()
    if os.path.exists(MOQI):
        shutil.rmtree(MOQI, ignore_errors=True)
    if os.path.exists(BACKUP):
        shutil.copytree(BACKUP, MOQI)
    installed = r"C:\Program Files (x86)\MoqiIM\MoqiDaemon.exe"
    if os.path.exists(installed):
        try:
            start_daemon(installed, r"C:\Program Files (x86)\MoqiIM")
        except Exception as exc:
            print("warning: installed daemon restart failed:", exc)


def main():
    ap = __import__("argparse").ArgumentParser()
    ap.add_argument("--daemon", required=True)
    ap.add_argument("--program-dir", required=True)
    args = ap.parse_args()
    backup()
    server = None
    try:
        if os.path.exists(MOQI):
            shutil.rmtree(MOQI, ignore_errors=True)
        os.makedirs(MOQI, exist_ok=True)
        with open(os.path.join(MOQI, "cloud_clipboard.json"), "w", encoding="utf-8") as f:
            json.dump({"enabled": True, "base_url": BASE, "username": USER,
                       "password": PASS, "settings_root": "/moqi-input-method/",
                       "min_interval_sec": 10, "list_hotkey": "Ctrl+0"}, f)
        server_log = open(os.path.join(r"F:\work\IME", "fake-webdav.log"), "w",
                          encoding="utf-8")
        server = subprocess.Popen([sys.executable, FAKE], stdout=server_log,
                                  stderr=subprocess.STDOUT)
        time.sleep(1.0)
        # Seed one clip on the server.
        clip = "云剪贴板测试内容"
        import hashlib
        name = hashlib.md5(clip.encode()).hexdigest() + ".txt"
        st, _ = dav("MKCOL", "/moqi-input-method/")
        dav("MKCOL", "/moqi-input-method/clip/")
        st, _ = dav("PUT", "/moqi-input-method/clip/" + name, clip.encode())
        check("fake server accepts clip PUT", st in (200, 201, 204), "status=%s" % st)

        start_daemon(args.daemon, args.program_dir)
        p = Pipe()
        cid = "webdav"
        p.call({"method": "METHOD_INIT", "clientId": cid})
        p.call({"method": "METHOD_ON_ACTIVATE", "clientId": cid})
        r, _ = p.call({"method": "METHOD_ON_COMMAND", "clientId": cid, "commandId": 5003}, timeout=8)
        check("5003 returns", r is not None and r.get("returnValue") == 1)

        # Ctrl+0 opens the list; the list is pushed asynchronously.
        r, extra = p.call({"method": "METHOD_FILTER_KEY_DOWN", "clientId": cid,
                           "keyEvent": key(ord("0"), ord("0"), ctrl=True)}, timeout=15)
        # Async list push arrives after the synchronous response.
        deadline = time.time() + 6
        while time.time() < deadline:
            f = p.try_read()
            if f is not None:
                extra.append(f)
            else:
                time.sleep(0.1)
        entries = []
        for f in extra + ([r] if r else []):
            if f.get("candidateEntries"):
                entries = f["candidateEntries"]
        check("cloud list shows seeded clip",
              any("云剪贴板测试内容" in e.get("text", "") for e in entries),
              "entries=%r extra=%r" % (entries, extra))

        # Esc exits list; then user-dict sync (download+merge+upload).
        p.call({"method": "METHOD_ON_KEY_DOWN", "clientId": cid,
                "keyEvent": key(0x1B, 0)}, timeout=5)
        r, _ = p.call({"method": "METHOD_ON_COMMAND", "clientId": cid, "commandId": 11}, timeout=8)
        check("11 returns", r is not None and r.get("returnValue") == 1)
        time.sleep(8)

        # Remote dict/scheme/device snapshot should exist (the completion
        # notification is a Win32 message box, not machine-readable; the
        # uploaded snapshot is the real end-to-end assertion).
        install = os.path.join(MOQI, "UserDictMaster", "installation.yaml")
        device = ""
        if os.path.exists(install):
            for line in open(install, encoding="utf-8", errors="replace"):
                if line.strip().startswith("installation_id:"):
                    device = line.split(":", 1)[1].strip().strip('"\'')
                    break
        check("installation_id present", bool(device), "device=%r" % device)
        if device:
            st, body = dav("PROPFIND", "/moqi-input-method/dict/Rime/" + device + "/", depth=1)
            text = body.decode("utf-8", "replace")
            check("remote user-dict snapshot uploaded",
                  st in (207, 200) and "userdb.txt" in text,
                  "status=%s body=%s" % (st, text[:300]))
        p.close()
    finally:
        if server:
            server.terminate()
        restore()
        passed = sum(1 for _, ok, _ in results if ok)
        failed = len(results) - passed
        print("\n%d passed, %d failed" % (passed, failed))
        if failed:
            for n, ok, d in results:
                if not ok:
                    print("  FAILED: %s :: %s" % (n, d))
            sys.exit(1)


if __name__ == "__main__":
    main()
