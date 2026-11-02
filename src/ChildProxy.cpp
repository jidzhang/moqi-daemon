// MoqiIME.exe 子进程代理实现（协议与语义见 ChildProxy.h）。
// include 顺序纪律：RimeCore.h 的链（windows→cstdint→mutex→string→
// vector→jsoncpp）是本工程实测唯一稳定的开篇；直接以 jsoncpp/裸 STL 开篇
// 会让 MSVC STL 在 <cmath> 崩掉（conditional_t/is_same_v 报错，机理未明、
// 可稳定复现）。RimeCore.h 同时带来了 ProtoFraming 所需的全部依赖。
#include "RimeCore.h"

#include "ChildProxy.h"
#include "ProtoFraming.h"

namespace Moqi {

namespace {

// 阻塞写满整帧。管道默认缓冲 4KB，菜单/AI 请求远小于此；万一超出，
// WriteFile 阻塞到子进程消费，子进程卡死由上层超时兜底（daemon 退出）。
bool writeAll(HANDLE h, const void* data, size_t len) {
  const char* p = static_cast<const char*>(data);
  while (len > 0) {
    const DWORD chunk = static_cast<DWORD>(len);
    DWORD written = 0;
    if (!::WriteFile(h, p, chunk, &written, nullptr) || written == 0) {
      return false;
    }
    p += written;
    len -= written;
  }
  return true;
}

} // namespace

ChildProxy::ChildProxy(std::wstring exePath, std::wstring workingDir, Sink sink)
    : exePath_(std::move(exePath)),
      workingDir_(std::move(workingDir)),
      sink_(sink) {
  ::InitializeCriticalSection(&lock_);
  ::InitializeCriticalSection(&waitLock_);
  ::InitializeConditionVariable(&waitCv_);
  const DWORD attrs = ::GetFileAttributesW(exePath_.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
    // 安装里没有 Go 后端（终态布局）：冷方法由上层回 success-empty。
    exePath_.clear();
  }
}

ChildProxy::~ChildProxy() {
  shutdown();
  ::DeleteCriticalSection(&waitLock_);
  ::DeleteCriticalSection(&lock_);
}

bool ChildProxy::startLocked() {
  if (started_) {
    return stdinWrite_ != INVALID_HANDLE_VALUE;
  }

  SECURITY_ATTRIBUTES inheritable = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE stdinRead = INVALID_HANDLE_VALUE;   // 子进程 stdin 读端
  HANDLE stdoutWrite = INVALID_HANDLE_VALUE; // 子进程 stdout 写端
  if (!::CreatePipe(&stdinRead, &stdinWrite_, &inheritable, 0)) {
    return false;
  }
  if (!::CreatePipe(&stdoutRead_, &stdoutWrite, &inheritable, 0)) {
    ::CloseHandle(stdinRead);
    ::CloseHandle(stdinWrite_);
    stdinWrite_ = INVALID_HANDLE_VALUE;
    stdoutRead_ = INVALID_HANDLE_VALUE;
    return false;
  }
  // 本进程持有的两端不可被子进程继承。
  ::SetHandleInformation(stdinWrite_, HANDLE_FLAG_INHERIT, 0);
  ::SetHandleInformation(stdoutRead_, HANDLE_FLAG_INHERIT, 0);

  std::wstring cmd = L"\"" + exePath_ + L"\"";
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = stdinRead;
  si.hStdOutput = stdoutWrite;
  si.hStdError = ::GetStdHandle(STD_ERROR_HANDLE); // Go 走 glog 文件，stderr 基本空
  PROCESS_INFORMATION pi = {};
  const BOOL ok = ::CreateProcessW(exePath_.c_str(), cmd.data(), nullptr,
                                   nullptr, TRUE, CREATE_NO_WINDOW,
                                   nullptr,
                                   workingDir_.empty() ? nullptr : workingDir_.c_str(),
                                   &si, &pi);
  ::CloseHandle(stdinRead);   // 子进程已持有自己的端
  ::CloseHandle(stdoutWrite);
  if (!ok) {
    ::CloseHandle(stdinWrite_);
    stdinWrite_ = INVALID_HANDLE_VALUE;
    stdoutRead_ = INVALID_HANDLE_VALUE;
    return false;
  }
  proc_ = pi;
  started_ = true;
  // 读者线程句柄只在本函数管理：重启时先关旧句柄（不会杀线程），
  // 读者线程自己不碰句柄成员，避免竞态。
  if (readerThread_ != nullptr) {
    ::CloseHandle(readerThread_);
    readerThread_ = nullptr;
  }
  readerThread_ = ::CreateThread(nullptr, 0, readerMainStatic, this, 0, nullptr);
  if (readerThread_ == nullptr) {
    stopLocked();
    return false;
  }
  return true;
}

void ChildProxy::stopLocked() {
  if (!started_) {
    return;
  }
  started_ = false;
  if (proc_.hProcess != nullptr) {
    ::TerminateProcess(proc_.hProcess, 0);
    ::CloseHandle(proc_.hProcess);
    proc_.hProcess = nullptr;
  }
  if (proc_.hThread != nullptr) {
    ::CloseHandle(proc_.hThread);
    proc_.hThread = nullptr;
  }
  if (stdinWrite_ != INVALID_HANDLE_VALUE) {
    ::CloseHandle(stdinWrite_);
    stdinWrite_ = INVALID_HANDLE_VALUE;
  }
  // stdoutRead_ 由读者线程独占管理：子进程死亡 -> 写端关闭 ->
  // ReadFile 失败 -> 读者线程自己关闭并置空。这里不动它，杜绝双关。
}

void ChildProxy::ensureStarted() {
  if (!available()) {
    return;
  }
  ::EnterCriticalSection(&lock_);
  if (proc_.hProcess != nullptr) {
    DWORD code = 0;
    if (::GetExitCodeProcess(proc_.hProcess, &code) && code != STILL_ACTIVE) {
      stopLocked(); // 死了就重启（对齐启动器的惰性复活）
    }
  }
  startLocked();
  ::LeaveCriticalSection(&lock_);
}

bool ChildProxy::forward(const std::string& payload) {
  if (!available()) {
    return false;
  }
  std::string framed = Proto::framePayload(payload);
  ::EnterCriticalSection(&lock_);
  bool ok = false;
  for (int attempt = 0; attempt < 2 && !ok; ++attempt) {
    if (!started_ || stdinWrite_ == INVALID_HANDLE_VALUE) {
      stopLocked();
      if (!startLocked()) {
        break;
      }
    }
    ok = writeAll(stdinWrite_, framed.data(), framed.size());
    if (!ok) {
      stopLocked(); // 写失败：子进程已死，重拉一次再试
    }
  }
  ::LeaveCriticalSection(&lock_);
  return ok;
}

DWORD WINAPI ChildProxy::readerMainStatic(LPVOID self) {
  static_cast<ChildProxy*>(self)->readerMain();
  return 0;
}

void ChildProxy::readerMain() {
  // stdoutRead_ 在本线程生命周期内独占：别的代码不关它（见 stopLocked 注释）。
  Proto::FrameBuffer frames;
  char buf[8192];
  for (;;) {
    DWORD got = 0;
    const HANDLE h = stdoutRead_;
    if (h == INVALID_HANDLE_VALUE ||
        !::ReadFile(h, buf, sizeof(buf), &got, nullptr) || got == 0) {
      break; // 子进程退出/管道关闭：线程结束（下次 forward 会重拉）
    }
    frames.append(buf, got);
    std::string payload;
    while (frames.nextFrame(payload)) {
      // 等待者按 seqNum 配对；其余帧原样透传（异步推送/dm-drop 回包——
      // 后者启动器找不到客户端自然丢弃）。
      int seqNum = -1;
      {
        Json::Value msg;
        if (Proto::parseJsonPayload(payload, msg) && msg.isObject()) {
          seqNum = msg.get("seqNum", -1).asInt();
        }
      }
      bool delivered = false;
      ::EnterCriticalSection(&waitLock_);
      if (!waitSatisfied_ && waitSeqNum_ >= 0 && seqNum == waitSeqNum_) {
        waitResponse_ = payload;
        waitSatisfied_ = true;
        delivered = true;
      }
      ::LeaveCriticalSection(&waitLock_);
      if (delivered) {
        ::WakeConditionVariable(&waitCv_);
      } else if (sink_ != nullptr) {
        sink_(Proto::framePayload(payload));
      }
    }
  }
  if (stdoutRead_ != INVALID_HANDLE_VALUE) {
    ::CloseHandle(stdoutRead_);
    stdoutRead_ = INVALID_HANDLE_VALUE;
  }
}

bool ChildProxy::forwardAndWait(const std::string& payload, int seqNum,
                                DWORD timeoutMs, std::string* responsePayload) {
  if (!available()) {
    return false;
  }
  ::EnterCriticalSection(&waitLock_);
  waitSeqNum_ = seqNum;
  waitResponse_.clear();
  waitSatisfied_ = false;
  ::LeaveCriticalSection(&waitLock_);

  if (!forward(payload)) {
    ::EnterCriticalSection(&waitLock_);
    waitSeqNum_ = -1;
    ::LeaveCriticalSection(&waitLock_);
    return false;
  }

  bool ok = false;
  ::EnterCriticalSection(&waitLock_);
  const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
  while (!waitSatisfied_) {
    const ULONGLONG now = ::GetTickCount64();
    if (now >= deadline) {
      break;
    }
    if (!::SleepConditionVariableCS(&waitCv_, &waitLock_,
                                    static_cast<DWORD>(deadline - now))) {
      break;
    }
  }
  ok = waitSatisfied_;
  if (ok && responsePayload != nullptr) {
    *responsePayload = waitResponse_;
  }
  waitSeqNum_ = -1; // 等待结束，后续同 seqNum 帧按异步透传
  ::LeaveCriticalSection(&waitLock_);
  return ok;
}

void ChildProxy::shutdown() {
  ::EnterCriticalSection(&lock_);
  stopLocked();
  ::LeaveCriticalSection(&lock_);
}

} // namespace Moqi
