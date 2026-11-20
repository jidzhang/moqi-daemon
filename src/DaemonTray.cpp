// 托盘图标实现（2026-11-19）。设计要点见 DaemonTray.h。
//
// 关键约束（都踩过或写在别处的坑）：
//  1. 托盘窗口不能是 HWND_MESSAGE：Shell_NotifyIcon 的回调消息需要一个真实
//     的顶层窗口，message-only 窗口收不到。
//  2. TrackPopupMenu 之前必须 SetForegroundWindow + 之后 PostMessage(WM_NULL)，
//     否则菜单一闪而过/第一项被立刻选中（见 MoqiLangBarButton::OnClick 里
//     2026-11-01 的同款修复）。
//  3. 菜单命令交给 main.cpp 的 executeCommand 回调（内部走 MoqiServeDispatch），
//     执行路径与 DLL 触发的完全一致，不会出现两套实现漂移。
//  4. 菜单树在每次右键时实时 gather，不缓存——引擎选项/方案随时会变。
#include "DaemonTray.h"

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <string>
#include <thread>

namespace Moqi {

// main.cpp 提供（同一命名空间内的轻量日志）。
void daemonLogExtern(const std::wstring& msg);

namespace {

constexpr wchar_t kTrayClassName[] = L"MoqiTrayIconWindow";
constexpr UINT kTrayCallbackMessage = WM_APP + 0x51;
constexpr UINT kTrayIconId = 1;

std::atomic<bool> g_running{false};
std::atomic<HWND> g_trayHwnd{nullptr};
TrayCallbacks g_callbacks;

std::wstring utf8ToWs(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int need = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                         static_cast<int>(s.size()), nullptr, 0);
  if (need <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(need), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        &out[0], need);
  return out;
}

// 把 buildMenu 产出的 JSON 树递归转成 HMENU。id 直接用作菜单项命令 id；
// separator 项插 MF_SEPARATOR；checked -> MF_CHECKED；enabled=false ->
// MF_GRAYED；有 submenu 的建子菜单。
//
// 菜单项 id 必须保持原值（1000+/2000+/4000+），命令分发靠它。只有分隔符和
// 纯父节点用 0（不产生 WM_COMMAND）。
void appendJsonItems(HMENU menu, const Json::Value& items) {
  if (!items.isArray()) return;
  for (const auto& it : items) {
    if (!it.isObject()) continue;
    if (it.get("separator", false).asBool()) {
      ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      continue;
    }
    const std::wstring text = utf8ToWs(it.get("text", "").asString());
    const int id = it.get("id", 0).asInt();
    const bool checked = it.get("checked", false).asBool();
    const bool enabled = it.get("enabled", true).asBool();

    const Json::Value& sub = it["submenu"];
    if (sub.isArray() && !sub.empty()) {
      HMENU child = ::CreatePopupMenu();
      if (child == nullptr) continue;
      appendJsonItems(child, sub);
      UINT flags = MF_POPUP;
      if (!enabled) flags |= MF_GRAYED;
      ::AppendMenuW(menu, flags, reinterpret_cast<UINT_PTR>(child),
                    text.c_str());
      continue;
    }
    // 叶子项：id==0 且无子菜单 = 不可点（纯标题），禁用避免误发 WM_COMMAND。
    UINT flags = MF_STRING;
    if (checked) flags |= MF_CHECKED;
    if (!enabled || id == 0) flags |= MF_GRAYED;
    ::AppendMenuW(menu, flags, static_cast<UINT_PTR>(id), text.c_str());
  }
}

void showTrayMenu(HWND hwnd) {
  POINT pt = {};
  ::GetCursorPos(&pt);

  HMENU menu = ::CreatePopupMenu();
  if (menu == nullptr) return;
  if (g_callbacks.buildMenuTree) {
    appendJsonItems(menu, g_callbacks.buildMenuTree());
  }
  if (::GetMenuItemCount(menu) <= 0) {
    ::AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"（引擎未就绪）");
  }

  // 坑 2：必须置前台 + RIGHTBUTTON + 事后 WM_NULL，否则菜单一闪而过。
  ::SetForegroundWindow(hwnd);
  const UINT cmd = ::TrackPopupMenu(
      menu,
      TPM_NONOTIFY | TPM_RETURNCMD | TPM_LEFTALIGN | TPM_BOTTOMALIGN |
          TPM_RIGHTBUTTON,
      pt.x, pt.y, 0, hwnd, nullptr);
  ::PostMessage(hwnd, WM_NULL, 0, 0);
  ::DestroyMenu(menu);

  if (cmd != 0) {
    daemonLogExtern(L"[tray] command " + std::to_wstring(cmd));
    if (g_callbacks.executeCommand) {
      g_callbacks.executeCommand(static_cast<int>(cmd));
    }
  }
}

LRESULT CALLBACK trayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case kTrayCallbackMessage:
      switch (LOWORD(lp)) {
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
          showTrayMenu(hwnd);
          return 0;
        default:
          return 0;
      }
    case WM_CLOSE:
      ::DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      ::PostQuitMessage(0);
      return 0;
    default:
      return ::DefWindowProcW(hwnd, msg, wp, lp);
  }
}

void trayThreadProc() {
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = trayWndProc;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.lpszClassName = kTrayClassName;
  if (::RegisterClassExW(&wc) == 0) {
    daemonLogExtern(L"[tray] RegisterClassEx failed gle=" +
                    std::to_wstring(::GetLastError()));
    g_running.store(false);
    return;
  }

  // 真实顶层窗口（不能 HWND_MESSAGE）：托盘回调要靠它。
  HWND hwnd = ::CreateWindowExW(0, kTrayClassName, L"MoqiTray", 0, 0, 0, 0, 0,
                                nullptr, nullptr, wc.hInstance, nullptr);
  if (hwnd == nullptr) {
    daemonLogExtern(L"[tray] CreateWindowEx failed gle=" +
                    std::to_wstring(::GetLastError()));
    g_running.store(false);
    return;
  }
  g_trayHwnd.store(hwnd);

  NOTIFYICONDATAW nid = {};
  nid.cbSize = sizeof(nid);
  nid.hWnd = hwnd;
  nid.uID = kTrayIconId;
  nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  nid.uCallbackMessage = kTrayCallbackMessage;
  // 图标：直接用 exe 自己的图标资源（MoqiDaemon.rc 的 ID 1）。
  nid.hIcon = static_cast<HICON>(
      ::LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                   ::GetSystemMetrics(SM_CXSMICON),
                   ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
  if (nid.hIcon == nullptr) {
    nid.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
  }
  wcscpy_s(nid.szTip, L"墨奇输入法");

  if (!::Shell_NotifyIconW(NIM_ADD, &nid)) {
    daemonLogExtern(L"[tray] Shell_NotifyIcon NIM_ADD failed gle=" +
                    std::to_wstring(::GetLastError()));
    ::DestroyWindow(hwnd);
    g_trayHwnd.store(nullptr);
    g_running.store(false);
    return;
  }
  daemonLogExtern(L"[tray] icon added hwnd=" +
                  std::to_wstring(reinterpret_cast<uintptr_t>(hwnd)));

  MSG msg;
  while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
  }

  ::Shell_NotifyIconW(NIM_DELETE, &nid);
  if (nid.hIcon != nullptr) ::DestroyIcon(nid.hIcon);
  g_trayHwnd.store(nullptr);
  g_running.store(false);
}

}  // namespace

void startTrayIcon(TrayCallbacks callbacks) {
  if (g_running.exchange(true)) return;
  g_callbacks = std::move(callbacks);
  std::thread([]() { trayThreadProc(); }).detach();
}

void stopTrayIcon() {
  HWND hwnd = g_trayHwnd.load();
  if (hwnd != nullptr) {
    ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
  }
}

}  // namespace Moqi
