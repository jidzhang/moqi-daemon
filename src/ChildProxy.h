#pragma once
// MoqiIME.exe 子进程代理（阶段 5a）：热路径由 DaemonEngine 本地承接，
// 冷方法（菜单/命令/AI/云剪贴板）原样转发给 Go 后端——菜单等能力零回退。
// 菜单/设置移植进 C++ 后，本类整体删除，Go 子进程退休。
//
// 协议：daemon 与 Go 子进程之间 = 启动器与后端之间同款
// 4 字节小端长度前缀 + protojson JSON，走子进程 stdin/stdout。
//
// 注意：本头文件不 include ProtoFraming.h/jsoncpp——jsoncpp 头与本工程
// 其余头的组合顺序会打断 MSVC STL（cmath/conditional_t 报错），帧序列化
// 由 .cpp 内部完成，头里只出现 std::string。
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace Moqi {

class ChildProxy {
public:
  // sink：把子进程产出的完整帧（含 4 字节前缀）写回本进程 stdout。
  // sink 必须线程安全（daemon 主线程与子进程读线程并发调用）。
  using Sink = void (*)(const std::string& framed);

  ChildProxy(std::wstring exePath, std::wstring workingDir, Sink sink);
  ~ChildProxy();

  bool available() const { return !exePath_.empty(); }

  // 预热：启动即拉起 Go 子进程（对齐启动器行为：菜单首次点击不等冷启动）。
  void ensureStarted();

  // 原样转发一帧（payload 不含 4 字节前缀，且必须已带 clientId——
  // 启动器已注入；镜像帧由调用方改写 clientId 后重序列化再转发）。
  bool forward(const std::string& payload);

  // 转发并同步等待子进程返回相同 seqNum 的响应（init/onActivate 的
  // addButton/selKeys/customizeUi 由 Go 生成，必须透传给 DLL）。
  // 等待期间收到的其他帧原样回写 stdout（异步推送不丢）。
  bool forwardAndWait(const std::string& payload, int seqNum, DWORD timeoutMs,
                      std::string* responsePayload);

  void shutdown();

private:
  bool startLocked();
  void stopLocked();
  static DWORD WINAPI readerMainStatic(LPVOID self);
  void readerMain();

  std::wstring exePath_;
  std::wstring workingDir_;
  Sink sink_;

  CRITICAL_SECTION lock_;
  PROCESS_INFORMATION proc_ = {};
  HANDLE stdinWrite_ = INVALID_HANDLE_VALUE;  // daemon -> 子进程 stdin
  HANDLE stdoutRead_ = INVALID_HANDLE_VALUE;  // 子进程 stdout -> daemon
  HANDLE readerThread_ = nullptr;
  bool started_ = false;

  // 同步等待：读者线程按 seqNum 配对（见 forwardAndWait）。
  CRITICAL_SECTION waitLock_;
  CONDITION_VARIABLE waitCv_;
  int waitSeqNum_ = -1;
  std::string waitResponse_;
  bool waitSatisfied_ = false;
};

} // namespace Moqi
