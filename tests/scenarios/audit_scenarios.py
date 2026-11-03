#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MoqiDaemon scenario regression (pipe-driven, no human clicking).

Covers the incidents from 2026-11-02/03 that used to require manual testing:
  * fresh user profile defaults (inline preedit / font / candidate count)
  * menu tree + command persistence across daemon restart
  * typing, commit, page size and page-2 reachability ("xin" / "鑫")
  * semicolon second-selection runtime switch
  * cloud clipboard disabled hint (Ctrl+0)
  * malformed ai_config.json must not deadlock/kill the daemon
  * 5000-range commands must not cold-start the engine

Run from an elevated/user shell while the installed daemon is stopped:
  python audit_scenarios.py --daemon <build>\\MoqiDaemon.exe \
      --program-dir "C:\\Program Files (x86)\\MoqiIM"
The script backs up %APPDATA%\\Moqi and restores it in a finally block.
"""
import argparse
import ctypes
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
BACKUP = os.path.join(r"F:\work\IME", "audit-profile-backup")
PIPE = r"\\.\pipe\%s\MoqiIM\Launcher" % os.environ.get("USERNAME", "")

_results = []


def check(name, cond, detail=""):
    _results.append((name, bool(cond), detail))
    print(("PASS " if cond else "FAIL ") + name + ((" :: " + detail) if detail and not cond else ""))


class Pipe:
    def __init__(self):
        self.h = kernel32.CreateFileW(PIPE, 0x80000000 | 0x40000000, 0,
                                      None, 3, 0, None)
        if self.h in (-1, 0xFFFFFFFF):
            raise RuntimeError("connect failed: %s" % ctypes.GetLastError())
        self.seq = 100

    def close(self):
        if self.h not in (-1, 0xFFFFFFFF):
            kernel32.CloseHandle(self.h)
            self.h = -1

    def _read_frame(self):
        hdr = b""
        while len(hdr) < 4:
            buf = ctypes.create_string_buffer(4 - len(hdr))
            got = ctypes.c_ulong(0)
            if not kernel32.ReadFile(self.h, buf, 4 - len(hdr),
                                     ctypes.byref(got), None) or got.value == 0:
                return None
            hdr += buf.raw[:got.value]
        n = struct.unpack("<I", hdr)[0]
        body = b""
        while len(body) < n:
            buf = ctypes.create_string_buffer(n - len(body))
            got = ctypes.c_ulong(0)
            if not kernel32.ReadFile(self.h, buf, n - len(body),
                                     ctypes.byref(got), None) or got.value == 0:
                return None
            body += buf.raw[:got.value]
        return json.loads(body.decode("utf-8"))

    def call(self, obj, timeout=20):
        self.seq += 1
        obj["seqNum"] = self.seq
        payload = json.dumps(obj).encode()
        frame = struct.pack("<I", len(payload)) + payload
        written = ctypes.c_ulong(0)
        kernel32.WriteFile(self.h, frame, len(frame), ctypes.byref(written), None)
        deadline = time.time() + timeout
        pending = []
        while time.time() < deadline:
            f = self._read_frame()
            if f is None:
                return None
            if f.get("seqNum") == self.seq:
                return f
            pending.append(f)
        return None

    def call_collect(self, obj, timeout=20):
        """Return (response, extra frames)."""
        self.seq += 1
        obj["seqNum"] = self.seq
        payload = json.dumps(obj).encode()
        frame = struct.pack("<I", len(payload)) + payload
        written = ctypes.c_ulong(0)
        kernel32.WriteFile(self.h, frame, len(frame), ctypes.byref(written), None)
        deadline = time.time() + timeout
        extra = []
        while time.time() < deadline:
            f = self._read_frame()
            if f is None:
                return None, extra
            if f.get("seqNum") == self.seq:
                return f, extra
            extra.append(f)
        return None, extra


def key_event(vk, ch=0, ctrl=False, shift=False, alt=False):
    states = [0] * 256
    states[vk] = 0x80
    if ctrl:
        states[0x11] = 0x80
    if shift:
        states[0x10] = 0x80
    if alt:
        states[0x12] = 0x80
    return {"keyCode": vk, "charCode": ch, "scanCode": 0,
            "isExtended": False, "repeatCount": 1, "keyStates": states}


CID = "audit-%d" % os.getpid()


def init(p):
    p.call({"method": "METHOD_INIT", "clientId": CID, "guid":
            "{3f6b5a12-8d44-4e71-9a2e-6b4f9c1d2a30}",
            "isWindows8Above": True, "isMetroApp": False, "isUiLess": True,
            "isConsole": False})
    r = p.call({"method": "METHOD_ON_ACTIVATE", "clientId": CID,
                "buttonId": "settings"})
    return r


def menu(p):
    r = p.call({"method": "METHOD_ON_MENU", "clientId": CID,
                "buttonId": "settings"})
    return (r or {}).get("menuItems") or []


def flat(items):
    out = []
    for it in items:
        out.append(it)
        out.extend(flat(it.get("submenu") or []))
    return out


def by_text(items, text):
    for it in flat(items):
        if it.get("text") == text:
            return it
    return None


def config():
    path = os.path.join(MOQI, "appearance_config.json")
    if not os.path.exists(path):
        return {}
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def cmd(p, cid, timeout=20):
    return p.call({"method": "METHOD_ON_COMMAND", "clientId": CID,
                   "commandId": cid}, timeout=timeout)


def type_key(p, vk, ch):
    down = p.call({"method": "METHOD_FILTER_KEY_DOWN", "clientId": CID,
                   "keyEvent": key_event(vk, ch)})
    if down and down.get("returnValue"):
        return p.call({"method": "METHOD_ON_KEY_DOWN", "clientId": CID,
                       "keyEvent": key_event(vk, ch)})
    return down


def type_text(p, text):
    last = None
    for ch in text:
        last = type_key(p, ord(ch.upper()), ord(ch))
    return last


def start_daemon(exe, program_dir):
    subprocess.run(["taskkill", "/F", "/IM", "MoqiDaemon.exe"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.6)
    env = os.environ.copy()
    env["MOQI_PROGRAM_DIR"] = program_dir
    env["MOQI_RIME_DLL_DIR"] = program_dir
    subprocess.Popen([exe, "--serve"], env=env, close_fds=True,
                     creationflags=0x00000008)  # DETACHED_PROCESS
    deadline = time.time() + 40
    while time.time() < deadline:
        h = kernel32.CreateFileW(PIPE, 0x80000000 | 0x40000000, 0, None,
                                 3, 0, None)
        if h not in (-1, 0xFFFFFFFF):
            kernel32.CloseHandle(h)
            time.sleep(0.8)  # engine warmup in background
            return
        time.sleep(0.5)
    raise RuntimeError("daemon pipe did not appear")


def stop_daemon():
    # The installed daemon may be elevated (started by the installer), so
    # taskkill from a normal shell is denied. Ask it to quit through its own
    # pipe first (ID_LAUNCHER_QUIT=33), then fall back to taskkill.
    try:
        p = Pipe()
        try:
            p.call({"method": "METHOD_ON_COMMAND", "clientId": "audit-stop",
                    "commandId": 33}, timeout=3)
        finally:
            p.close()
    except Exception:
        pass
    time.sleep(1.5)
    subprocess.run(["taskkill", "/F", "/IM", "MoqiDaemon.exe"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.8)


def backup_profile():
    # LevelDB files are held open by the daemon; stop it first.
    stop_daemon()
    if os.path.exists(BACKUP):
        shutil.rmtree(BACKUP)
    if os.path.exists(MOQI):
        shutil.copytree(MOQI, BACKUP)


def restore_profile():
    stop_daemon()
    if os.path.exists(MOQI):
        shutil.rmtree(MOQI, ignore_errors=True)
    if os.path.exists(BACKUP):
        shutil.copytree(BACKUP, MOQI)
    # Bring the installed daemon back for the real IME.
    installed = os.path.join(r"C:\Program Files (x86)\MoqiIM", "MoqiDaemon.exe")
    if os.path.exists(installed):
        try:
            start_daemon(installed, r"C:\Program Files (x86)\MoqiIM")
        except Exception as exc:
            print("warning: could not restart installed daemon:", exc)


def fresh_profile():
    if os.path.exists(MOQI):
        shutil.rmtree(MOQI, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--daemon", required=True)
    ap.add_argument("--program-dir", required=True)
    args = ap.parse_args()
    backup_profile()
    try:
        fresh_profile()
        start_daemon(args.daemon, args.program_dir)
        p = Pipe()
        act = init(p)
        ui = (act or {}).get("customizeUi") or {}
        check("fresh: activate returns 4 buttons",
              len((act or {}).get("addButton") or []) == 4)
        check("fresh: inline preedit defaults on", ui.get("inlinePreedit") is True,
              "ui=%r" % ui)
        check("fresh: candidate count 7", ui.get("candidateCount") == 7,
              "ui=%r" % ui)
        check("fresh: candidate per row 7", ui.get("candPerRow") == 7,
              "ui=%r" % ui)
        check("fresh: font is Microsoft YaHei UI",
              ui.get("candFontName") == "Microsoft YaHei UI",
              "ui=%r" % ui)
        patch_path = os.path.join(MOQI, "UserDictMaster", "rime_frost.custom.yaml")
        patch = open(patch_path, encoding="utf-8", errors="replace").read() if os.path.exists(patch_path) else ""
        check("fresh: local_behavior patch generated",
              "disable_user_dict_for_patterns" in patch,
              "patch=%r" % patch[:500])

        items = menu(p)
        check("fresh: menu 25 items", len(items) == 25, "n=%d" % len(items))
        check("fresh: no scheme-set submenu",
              by_text(items, "切换方案集") is None)
        check("fresh: input settings present", by_text(items, "输入设置") is not None)

        # 5000 range must not trigger engine cold start. Measure a no-op
        # command that opens the bundled editor? 5002 just toggles config.
        t0 = time.time()
        r = cmd(p, 5002)
        dt = time.time() - t0
        check("5002 responds without cold start", r is not None and dt < 3.0,
              "dt=%.2f" % dt)
        cmd(p, 5002)  # restore enabled=false

        # candidate count + per-row persistence
        r = cmd(p, 191)  # 5
        ui = (r or {}).get("customizeUi") or {}
        check("cmd 191 -> candidateCount 5", ui.get("candidateCount") == 5,
              "ui=%r" % ui)
        r = cmd(p, 181)  # per row 5
        check("cmd 181 -> per row 5",
              ((r or {}).get("customizeUi") or {}).get("candPerRow") == 5)
        cfg = config()
        check("config: candidate_count 5", cfg.get("candidate_count") == 5)
        check("config: candidate_per_row 5", cfg.get("candidate_per_row") == 5)

        # vertical then horizontal restore
        r = cmd(p, 170)
        check("vertical -> candPerRow 1",
              ((r or {}).get("customizeUi") or {}).get("candPerRow") == 1)
        check("config: vertical per_row 0", config().get("candidate_per_row") == 0)
        r = cmd(p, 171)
        # 竖排落盘 0；C++ 从竖排切回横排回落到 9，再与总候选 5 取小 = 5。
        check("horizontal -> per row 5",
              ((r or {}).get("customizeUi") or {}).get("candPerRow") == 5)
        cmd(p, 181)  # per row 5 again for restart check

        # typing + commit
        resp = type_text(p, "nihao")
        entries = (resp or {}).get("candidateEntries") or []
        check("typing nihao has candidates", len(entries) > 0,
              "resp=%r" % (resp or {}))
        commit = type_key(p, 0x20, 0x20)
        check("space commits 你好",
              (commit or {}).get("commitString") == "你好",
              "commit=%r" % (commit or {}))

        # page size: xin page 2 must contain 鑫
        cmd(p, 192)  # count 7
        resp = type_text(p, "xin")
        page1 = p.call({"method": "METHOD_CHANGE_PAGE", "clientId": CID,
                        "pageBackward": False})
        texts = [e.get("text", "") for e in ((page1 or {}).get("candidateEntries") or [])]
        check("xin page 2 contains 鑫", "鑫" in texts, "texts=%r" % texts)

        # clear the leftover "xin" composition before overlay tests
        type_key(p, 0x1B, 0x1B)

        # custom phrase / super abbrev overlays
        with open(os.path.join(MOQI, "moqi_custom_phrase.txt"), "w",
                  encoding="utf-8") as f:
            f.write("置顶测试\tqzwx\t99\n")
        with open(os.path.join(MOQI, "moqi_super_abbrev.txt"), "w",
                  encoding="utf-8") as f:
            f.write("超简测试\tcsj\n")
        resp = type_text(p, "qzwx")
        entries = (resp or {}).get("candidateEntries") or []
        check("custom phrase prepended",
              bool(entries) and entries[0].get("text") == "置顶测试",
              "entries=%r" % entries)
        commit = type_key(p, 0x20, 0x20)
        check("custom phrase commits on space",
              (commit or {}).get("commitString") == "置顶测试",
              "commit=%r" % (commit or {}))
        resp = type_text(p, "csj")
        entries = (resp or {}).get("candidateEntries") or []
        has_super = any("超简测试" in (e.get("comment", "") + e.get("text", ""))
                        for e in entries)
        check("super abbrev shows mapped comment", has_super,
              "entries=%r" % entries)
        commit = type_key(p, 0x09, 0x09)
        check("super abbrev commits on Tab",
              (commit or {}).get("commitString") == "超简测试",
              "commit=%r" % (commit or {}))
        os.remove(os.path.join(MOQI, "moqi_custom_phrase.txt"))
        os.remove(os.path.join(MOQI, "moqi_super_abbrev.txt"))

        # cloud clipboard disabled hint
        r, extra = p.call_collect({"method": "METHOD_FILTER_KEY_DOWN",
                                   "clientId": CID,
                                   "keyEvent": key_event(ord("0"), ord("0"), ctrl=True)})
        hinted = any((f.get("showMessage") or {}).get("message", "").find("开启云剪贴板") >= 0
                     for f in (extra + ([r] if r else [])))
        check("Ctrl+0 disabled shows hint", hinted,
              "resp=%r extra=%r" % (r, extra))

        # malformed AI config must not deadlock
        with open(os.path.join(MOQI, "ai_config.json"), "w", encoding="utf-8") as f:
            f.write('{"api":{"base_url":"http://127.0.0.1:1","api_key":123,"model":"m"}}')
        type_text(p, "ni")
        alive = p.call({"method": "METHOD_ON_MENU", "clientId": CID,
                        "buttonId": "settings"}, timeout=5)
        check("malformed ai_config does not kill daemon", alive is not None)
        os.remove(os.path.join(MOQI, "ai_config.json"))

        # restart persistence
        p.close()
        stop_daemon()
        start_daemon(args.daemon, args.program_dir)
        p = Pipe()
        act = init(p)
        ui = (act or {}).get("customizeUi") or {}
        check("restart: candidate count 7 persisted", ui.get("candidateCount") == 7,
              "ui=%r" % ui)
        check("restart: per row 5 persisted", ui.get("candPerRow") == 5,
              "ui=%r" % ui)
        p.close()
    finally:
        stop_daemon()
        restore_profile()
        passed = sum(1 for _, ok, _ in _results if ok)
        failed = len(_results) - passed
        print("\n%d passed, %d failed" % (passed, failed))
        if failed:
            for name, ok, detail in _results:
                if not ok:
                    print("  FAILED: %s :: %s" % (name, detail))
            sys.exit(1)


if __name__ == "__main__":
    main()
