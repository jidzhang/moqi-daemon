#pragma once
// 前台权限转授（2026-11-19）。daemon 是后台进程，Windows 的 foreground lock
// 不让它把新窗口顶到前面：用户点"重新部署"或"配置 AI 助手"后，结果提示框 /
// 记事本被当前应用（ZCode、Total Commander 等）压住，看起来像没反应。
//
// 光 AllowSetForegroundWindow 不够：它要求**调用方**当前持有前台权限，daemon
// 从来没有，调用必然失败（实测）。可靠配方是 AttachThreadInput 把自己挂到
// 前台窗口的输入队列上继承前台身份，再 AllowSetForegroundWindow 把权限转授
// 给即将启动的子进程，最后 detach。
//
// 另一条互补路径在 DLL 侧（MoqiClient::onCommand）：宿主进程通常正处前台，
// 由它直接 AllowSetForegroundWindow(daemonPid) 更可靠。两条路径互不冲突。
#include <windows.h>

namespace Moqi {

// RAII：构造时 attach 到当前前台窗口的输入队列并转授前台权限，析构时 detach。
// 必须在启动子进程（ShellExecute/MessageBox）的同一线程作用域内使用。
class ForegroundGrant {
 public:
  ForegroundGrant() {
    HWND fg = ::GetForegroundWindow();
    if (fg == nullptr) return;
    const DWORD fgThread = ::GetWindowThreadProcessId(fg, nullptr);
    const DWORD myThread = ::GetCurrentThreadId();
    if (fgThread == 0 || fgThread == myThread) return;
    if (!::AttachThreadInput(myThread, fgThread, TRUE)) return;
    attachedThreadId_ = fgThread;
    ::AllowSetForegroundWindow(ASFW_ANY);
  }

  ~ForegroundGrant() {
    if (attachedThreadId_ != 0) {
      ::AttachThreadInput(::GetCurrentThreadId(), attachedThreadId_, FALSE);
    }
  }

  ForegroundGrant(const ForegroundGrant&) = delete;
  ForegroundGrant& operator=(const ForegroundGrant&) = delete;

 private:
  DWORD attachedThreadId_ = 0;
};

} // namespace Moqi
