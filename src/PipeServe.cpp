// 阶段 5c：daemon 管道服务实现。语义对齐 MoqiLaunch 的 PipeServer：
// - 管道名 \\.\pipe\<用户>\MoqiIM\Launcher（用户名入路径防多用户冲突）
// - 多实例顺序接受，每客户端一读线程
// - 每连接分配 clientId（UUID），注入每个上行帧
// - 回包按帧内 clientId 路由；单实例互斥体沿用工名 MoqiLaunchMutex
//   （TSF DLL 的 isLauncherProcessRunning 探测即靠它，零改动兼容）
// - 帧协议 = 4 字节小端长度前缀 + JSON，字节流读法（见全局管道陷阱 memory）
#include "RimeCore.h"

#include <lmcons.h>
#include <sddl.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "PipeServe.h"
#include "ProtoFraming.h"

namespace Moqi {

// main.cpp 共用的轻量日志。
void daemonLogExtern(const std::wstring& msg);

namespace {

std::string newClientId() {
  GUID g = {};
  wchar_t buf[64] = {};
  if (SUCCEEDED(::CoCreateGuid(&g)) && ::StringFromGUID2(g, buf, 64) > 0) {
    std::wstring ws(buf, wcslen(buf));
    // 去掉花括号得到裸 UUID
    if (ws.size() >= 2 && ws.front() == L'{' && ws.back() == L'}') {
      ws = ws.substr(1, ws.size() - 2);
    }
    std::string out;
    for (wchar_t c : ws) {
      out += static_cast<char>(c);
    }
    return "dm-" + out;
  }
  return "dm-????";
}

// 服务端句柄是 overlapped（见 run）。同步句柄下，读者线程挂着的 ReadFile
// 会让其他线程的 WriteFile 排队到它完成——AI 推送因此一直压到客户端发来
// 下一个请求才出管道（2026-11-02 19:17 实证：推送在下一次按键的 RPC 里才被
// 读到）。写走 overlapped + 自带事件，与挂起的读互不阻塞。
bool writeAll(HANDLE h, const void* data, size_t len) {
  HANDLE ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (ev == nullptr) return false;
  const char* p = static_cast<const char*>(data);
  bool ok = true;
  while (len > 0) {
    OVERLAPPED ov = {};
    ov.hEvent = ev;
    DWORD written = 0;
    if (!::WriteFile(h, p, static_cast<DWORD>(len), &written, &ov) &&
        ::GetLastError() != ERROR_IO_PENDING) {
      ok = false;
      break;
    }
    // 有界等待：客户端不读管道（窗口卡死/断连）时，不能让 AI 推送线程
    // 永远堵在这里——它会被下一次热键 join。
    const DWORD wait = ::WaitForSingleObject(ev, 5000);
    if (wait != WAIT_OBJECT_0 ||
        !::GetOverlappedResult(h, &ov, &written, FALSE) || written == 0) {
      ::CancelIoEx(h, &ov);
      ok = false;
      break;
    }
    p += written;
    len -= written;
  }
  ::CloseHandle(ev);
  return ok;
}

} // namespace

struct PipeServe::Impl {
  CRITICAL_SECTION clientsLock;
  std::map<std::string, HANDLE> clients; // clientId -> 客户端管道写端
  volatile LONG stopRequested = 0;
  std::wstring pipeName;
  HANDLE singleInstance = nullptr;
  std::atomic<bool> ownsPipe{false};

  Impl() { ::InitializeCriticalSection(&clientsLock); }
  ~Impl() {
    if (singleInstance != nullptr) {
      ::CloseHandle(singleInstance);
    }
    ::DeleteCriticalSection(&clientsLock);
  }

  void registerClient(const std::string& id, HANDLE h) {
    ::EnterCriticalSection(&clientsLock);
    clients[id] = h;
    ::LeaveCriticalSection(&clientsLock);
  }
  void unregisterClient(const std::string& id) {
    ::EnterCriticalSection(&clientsLock);
    clients.erase(id);
    ::LeaveCriticalSection(&clientsLock);
  }
  bool sendTo(const std::string& id, const std::string& framed) {
    HANDLE h = INVALID_HANDLE_VALUE;
    ::EnterCriticalSection(&clientsLock);
    auto it = clients.find(id);
    if (it != clients.end()) h = it->second;
    if (h == INVALID_HANDLE_VALUE) {
      std::wstring dump;
      for (const auto& kv : clients) {
        dump += L" [" + std::wstring(kv.first.begin(), kv.first.end()) + L"]";
      }
      daemonLogExtern(L"[serve] sendTo lookup '" +
                      std::wstring(id.begin(), id.end()) +
                      L"' registry size=" + std::to_wstring(clients.size()) +
                      L" keys:" + dump);
    }
    ::LeaveCriticalSection(&clientsLock);
    if (h == INVALID_HANDLE_VALUE) return false;
    return writeAll(h, framed.data(), framed.size());
  }
};

namespace {

struct ClientConn {
  HANDLE pipe;
  std::string clientId;
  PipeServe::Impl* serve;
  std::shared_ptr<std::mutex> writeLock; // 同连接多线程写（同步回包/异步推送）
};

DWORD WINAPI clientThread(LPVOID self) {
  std::unique_ptr<ClientConn> conn(static_cast<ClientConn*>(self));
  const HANDLE pipe = conn->pipe;
  const std::string clientId = conn->clientId;
  PipeServe::Impl* serve = conn->serve;
  auto writeLock = conn->writeLock;

  Proto::FrameBuffer frames;
  char buf[8192];
  HANDLE readEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  for (;;) {
    OVERLAPPED ov = {};
    ov.hEvent = readEvent;
    DWORD got = 0;
    if (readEvent == nullptr ||
        (!::ReadFile(pipe, buf, sizeof(buf), &got, &ov) &&
         ::GetLastError() != ERROR_IO_PENDING) ||
        !::GetOverlappedResult(pipe, &ov, &got, TRUE) || got == 0) {
      break; // 客户端断开
    }
    frames.append(buf, got);
    std::string payload;
    while (frames.nextFrame(payload)) {
      try {
        Json::Value req;
        if (!Proto::parseJsonPayload(payload, req) || !req.isObject()) {
          continue;
        }
        req["clientId"] = clientId; // 与壳一致：连接级 id 注入每个上行帧
        Json::StreamWriterBuilder wb;
        wb["commentStyle"] = "None";
        wb["indentation"] = "";
        const std::string withId = Json::writeString(wb, req);
        MoqiServeDispatch(withId, [pipe, writeLock](const std::string& framed) {
          std::lock_guard<std::mutex> wl(*writeLock);
          writeAll(pipe, framed.data(), framed.size());
        });
      } catch (const std::exception& e) {
        daemonLogExtern(L"[serve] client thread exception: " +
                        std::wstring(e.what(), e.what() + strlen(e.what())));
      } catch (...) {
        daemonLogExtern(L"[serve] client thread unknown exception");
      }
    }
  }
  serve->unregisterClient(clientId);
  ::DisconnectNamedPipe(pipe);
  ::CloseHandle(pipe);
  if (readEvent != nullptr) {
    ::CloseHandle(readEvent);
  }
  return 0;
}

} // namespace

PipeServe::PipeServe() : impl_(new Impl) {}

PipeServe::~PipeServe() { delete impl_; }

bool PipeServe::ownsPipe() const { return impl_->ownsPipe.load(); }

void PipeServe::routeFrame(const std::string& framed) {
  if (framed.size() < 4) return;
  // framed 含 4 字节前缀，剥掉再取 clientId。
  std::string payload(framed.data() + 4, framed.size() - 4);
  Json::Value msg;
  if (!Proto::parseJsonPayload(payload, msg) || !msg.isObject()) {
    daemonLogExtern(L"[serve] routeFrame: unparseable frame");
    return;
  }
  const std::string clientId = msg.get("clientId", "").asString();
  if (clientId.empty()) {
    daemonLogExtern(L"[serve] routeFrame: no clientId");
    return;
  }
  if (!impl_->sendTo(clientId, framed)) {
    daemonLogExtern(L"[serve] routeFrame: no client for " +
                    std::wstring(clientId.begin(), clientId.end()));
  }
}

void PipeServe::requestStop() {
  ::InterlockedExchange(&impl_->stopRequested, 1);
  // 连一次自己的管道让阻塞的 ConnectNamedPipe 尽快返回。
  if (!impl_->pipeName.empty()) {
    HANDLE h = ::CreateFileW(impl_->pipeName.c_str(),
                             GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      ::CloseHandle(h);
    }
  }
}

int PipeServe::run() {
  wchar_t user[UNLEN + 1] = {};
  DWORD userLen = UNLEN + 1;
  if (!::GetUserNameW(user, &userLen)) {
    return 1;
  }
  impl_->pipeName = std::wstring(L"\\\\.\\pipe\\") + user + L"\\MoqiIM\\Launcher";
  // 测试钩子：换名绑定，避开真链路上 DLL 自愈复活的 MoqiLaunch 抢管道。
  if (const wchar_t* overridePipe = _wgetenv(L"MOQI_SERVE_PIPE")) {
    impl_->pipeName = overridePipe;
  }

  // 单实例互斥体：沿用 MoqiLaunchMutex（TSF DLL 的存活探测名，零改动兼容）。
  // 安全描述符与管道一致；否则安装器以管理员拉起 daemon 时创建高完整性
  // 互斥体，普通 TSF/新 daemon 打不开，会误判“首实例”并卡在 CreateNamedPipe
  // 重试（2026-11-04 实测 3 个 daemon 并存）。
  // AC = ALL APPLICATION PACKAGES，标签降为 Low：UWP/AppContainer（便笺等）
  // 以低完整性运行，默认 Medium 标签的管道/互斥体对它是 no-write-up，
  // 连不上 daemon 就只能把按键放行成英文。管道名按用户名隔离，跨用户仍不可达。
  SECURITY_ATTRIBUTES sa = {};
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:(A;;GA;;;WD)(A;;GA;;;AC)S:(ML;;NW;;;LW)", SDDL_REVISION_1, &sd,
          nullptr)) {
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
  } else {
    daemonLogExtern(L"[serve] pipe SDDL parse failed; using default DACL");
  }
  // 测试钩子：换名互斥体，允许场景测试与真实 daemon 并行（默认生产名）。
  const wchar_t* overrideMutex = _wgetenv(L"MOQI_SERVE_MUTEX");
  LPCWSTR mutexName =
      (overrideMutex != nullptr && overrideMutex[0] != L'\0')
          ? overrideMutex
          : L"MoqiLaunchMutex";
  impl_->singleInstance = ::CreateMutexW(sd != nullptr ? &sa : nullptr,
                                         FALSE, mutexName);  const bool alreadyRunning =
      (impl_->singleInstance != nullptr &&
       ::GetLastError() == ERROR_ALREADY_EXISTS);

  bool firstInstance = true;
  for (;;) {
    if (impl_->stopRequested) break;
    // FIRST_PIPE_INSTANCE 只在首个实例上用：它要求名字尚不存在——
    // 同进程的后续实例带上它会创建失败（ERROR_PIPE_EXISTS 系）。
    const DWORD firstFlag =
        firstInstance ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0;
    HANDLE pipe = ::CreateNamedPipeW(
        impl_->pipeName.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | firstFlag,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0,
        sd != nullptr ? &sa : nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      if (firstInstance) {
        // 互斥体可能因完整性/权限差异看不到，直接探活管道兜底：能连上
        // 就说明已有实例在服务，必须退出，不能无限重试占着引擎不放。
        bool live = alreadyRunning;
        if (!live) {
          HANDLE probe = ::CreateFileW(
              impl_->pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
              nullptr, OPEN_EXISTING, 0, nullptr);
          if (probe != INVALID_HANDLE_VALUE) {
            ::CloseHandle(probe);
            live = true;
          }
        }
        if (live) {
          daemonLogExtern(L"[serve] another instance owns the pipe; exiting");
          break;
        }
      }
      ::Sleep(500);
      continue;
    }
    if (firstInstance) {
      impl_->ownsPipe.store(true);
      daemonLogExtern(L"[serve] pipe ready pid=" +
                      std::to_wstring(::GetCurrentProcessId()));
    }
    firstInstance = false;
    // overlapped 句柄的 ConnectNamedPipe 必须带 OVERLAPPED：正常返回
    // FALSE + ERROR_IO_PENDING，等事件；ERROR_PIPE_CONNECTED = 客户端已先到。
    OVERLAPPED connectOv = {};
    connectOv.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool connected = ::ConnectNamedPipe(pipe, &connectOv) != FALSE;
    if (!connected) {
      const DWORD err = ::GetLastError();
      if (err == ERROR_PIPE_CONNECTED) {
        connected = true;
      } else if (err == ERROR_IO_PENDING && connectOv.hEvent != nullptr) {
        DWORD ignored = 0;
        connected =
            ::GetOverlappedResult(pipe, &connectOv, &ignored, TRUE) != FALSE;
      }
    }
    if (connectOv.hEvent != nullptr) {
      ::CloseHandle(connectOv.hEvent);
    }
    if (!connected) {
      ::CloseHandle(pipe);
      if (impl_->stopRequested) break;
      continue;
    }
    ClientConn* conn = new ClientConn;
    conn->pipe = pipe;
    conn->clientId = newClientId();
    conn->serve = impl_;
    conn->writeLock = std::make_shared<std::mutex>();
    impl_->registerClient(conn->clientId, pipe);
    daemonLogExtern(L"[serve] client connected: " +
                    std::wstring(conn->clientId.begin(),
                                 conn->clientId.end()));
    HANDLE th = ::CreateThread(nullptr, 0, &clientThread, conn, 0, nullptr);
    if (th == nullptr) {
      impl_->unregisterClient(conn->clientId);
      ::DisconnectNamedPipe(pipe);
      ::CloseHandle(pipe);
      delete conn;
    } else {
      ::CloseHandle(th);
    }
  }
  return 0;
}

} // namespace Moqi
