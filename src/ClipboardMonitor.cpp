// 5e 剪贴板监听实现。契约 = Go MoqiLaunch PipeServer.cpp 的
// WM_CLIPBOARDUPDATE → debounce → uploadCloudClipboardText 链。
// include 纪律：RimeCore.h 链开篇。
#include "RimeCore.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "ClipboardMonitor.h"
#include "DaemonCloudConfig.h"


namespace Moqi {

// main.cpp 的日志出口（在 Moqi 命名空间、非匿名——否则内部链接导致 LNK2019）。
void daemonLogExtern(const std::wstring& msg);
void daemonLogDebugExtern(const std::wstring& msg);

namespace {

constexpr UINT WM_FLUSH_UPLOAD = WM_APP + 0x401;
constexpr UINT kDebounceMs = 500;

struct MonitorState {
  std::atomic<bool> running{false};
  std::atomic<HWND> hwnd{nullptr};
  std::thread thread;
};
MonitorState g_monitor;

// 读剪贴板文本（CF_UNICODETEXT）。
std::string readClipboardText(HWND owner) {
  if (!::OpenClipboard(owner)) return "";
  HANDLE h = ::GetClipboardData(CF_UNICODETEXT);
  std::string utf8;
  if (h != nullptr) {
    const wchar_t* ws = (const wchar_t*)::GlobalLock(h);
    if (ws != nullptr) {
      const int len = ::GlobalSize(h) / sizeof(wchar_t);
      // 去掉尾零
      int actual = 0;
      while (actual < len && ws[actual] != L'\0') ++actual;
      const int n = ::WideCharToMultiByte(CP_UTF8, 0, ws, actual, nullptr, 0,
                                          nullptr, nullptr);
      utf8.resize(n);
      if (n > 0) {
        ::WideCharToMultiByte(CP_UTF8, 0, ws, actual, &utf8[0], n, nullptr,
                              nullptr);
      }
      ::GlobalUnlock(h);
    }
  }
  ::CloseClipboard();
  return utf8;
}

LRESULT CALLBACK monitorWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CLIPBOARDUPDATE: {
      // 常规剪贴板事件走 debug 档（error 档不落，用户反馈日志不受控）。
      daemonLogDebugExtern(
          L"[clipmon] WM_CLIPBOARDUPDATE received; arming timer");
      // 500ms 防抖（Go WM_FLUSH_CLIPBOARD_UPLOAD 模式：反复触发时只取最后一次）
      ::SetTimer(hwnd, 1, kDebounceMs, nullptr);
      return 0;
    }
    case WM_TIMER: {
      if (wp == 1) {
        ::KillTimer(hwnd, 1);
        const std::string text = readClipboardText(hwnd);
        daemonLogDebugExtern(L"[clipmon] timer fired; text len=" +
                             std::to_wstring(text.size()));
        if (!text.empty() && Moqi::globalCloudSync() != nullptr) {
          Moqi::globalCloudSync()->uploadFromRequest(text);
          daemonLogDebugExtern(L"[clipmon] uploadFromRequest called");
        }
      }
      return 0;
    }
    case WM_DESTROY: {
      ::RemoveClipboardFormatListener(hwnd);
      ::PostQuitMessage(0);
      return 0;
    }
  }
  return ::DefWindowProcW(hwnd, msg, wp, lp);
}

DWORD WINAPI monitorThread(LPVOID) {
  // 注册窗口类
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = monitorWndProc;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.lpszClassName = L"MoqiClipboardMonitor";
  ::RegisterClassExW(&wc);

  HWND hwnd = ::CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
  if (hwnd == nullptr) {
    return 1;
  }
  g_monitor.hwnd.store(hwnd);
  daemonLogExtern(L"[clipmon] window created hwnd=" + std::to_wstring((uintptr_t)hwnd));
  if (!::AddClipboardFormatListener(hwnd)) {
    daemonLogExtern(L"[clipmon] AddClipboardFormatListener FAILED gle=" + std::to_wstring(::GetLastError()));
    // 监听失败（权限/终端服务），不影响其他功能。
    // 但窗口留着——服务端还是能收 listClip 等命令的。
  } else {
    daemonLogExtern(L"[clipmon] listener registered");
  }

  MSG msg;
  while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
  }
  g_monitor.hwnd.store(nullptr);
  return 0;
}

}  // namespace

void startClipboardMonitor() {
  if (g_monitor.running.exchange(true)) {
    return;  // 已在跑
  }
  g_monitor.thread = std::thread([]() {
    monitorThread(nullptr);
    g_monitor.running.store(false);
  });
}

void stopClipboardMonitor() {
  HWND hwnd = g_monitor.hwnd.load();
  if (hwnd != nullptr) {
    ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
  }
  if (g_monitor.thread.joinable()) {
    g_monitor.thread.join();
  }
}

}  // namespace Moqi
