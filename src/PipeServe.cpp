// 阶段 5c：daemon 管道服务实现。语义对齐 MoqiLaunch 的 PipeServer：
// - 管道名 \\.\pipe\<用户>\MoqiIM\Launcher（用户名入路径防多用户冲突）
// - 多实例顺序接受，每客户端一读线程
// - 每连接分配 clientId（UUID），注入每个上行帧
// - 回包按帧内 clientId 路由；单实例互斥体沿用工名 MoqiLaunchMutex
//   （TSF DLL 的 isLauncherProcessRunning 探测即靠它，零改动兼容）
// - 帧协议 = 4 字节小端长度前缀 + JSON，字节流读法（见全局管道陷阱 memory）
#include "RimeCore.h"

#include <lmcons.h>

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

bool writeAll(HANDLE h, const void* data, size_t len) {
  const char* p = static_cast<const char*>(data);
  while (len > 0) {
    DWORD written = 0;
    if (!::WriteFile(h, p, static_cast<DWORD>(len), &written, nullptr) ||
        written == 0) {
      return false;
    }
    p += written;
    len -= written;
  }
  return true;
}

} // namespace

struct PipeServe::Impl {
  CRITICAL_SECTION clientsLock;
  std::map<std::string, HANDLE> clients; // clientId -> 客户端管道写端
  volatile LONG stopRequested = 0;
  std::wstring pipeName;
  HANDLE singleInstance = nullptr;

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
};

DWORD WINAPI clientThread(LPVOID self) {
  std::unique_ptr<ClientConn> conn(static_cast<ClientConn*>(self));
  const HANDLE pipe = conn->pipe;
  const std::string clientId = conn->clientId;
  PipeServe::Impl* serve = conn->serve;

  Proto::FrameBuffer frames;
  char buf[8192];
  for (;;) {
    DWORD got = 0;
    if (!::ReadFile(pipe, buf, sizeof(buf), &got, nullptr) || got == 0) {
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
        MoqiServeDispatch(withId, [pipe](const std::string& framed) {
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
  return 0;
}

} // namespace

PipeServe::PipeServe() : impl_(new Impl) {}

PipeServe::~PipeServe() { delete impl_; }

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
  impl_->singleInstance =
      ::CreateMutexW(nullptr, FALSE, L"MoqiLaunchMutex");
  const bool alreadyRunning =
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
        PIPE_ACCESS_DUPLEX | firstFlag,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      if (firstInstance && alreadyRunning) {
        daemonLogExtern(L"[serve] another instance owns the pipe; exiting");
        break;
      }
      ::Sleep(500);
      continue;
    }
    firstInstance = false;
    if (!::ConnectNamedPipe(pipe, nullptr) &&
        ::GetLastError() != ERROR_PIPE_CONNECTED) {
      ::CloseHandle(pipe);
      if (impl_->stopRequested) break;
      continue;
    }
    ClientConn* conn = new ClientConn;
    conn->pipe = pipe;
    conn->clientId = newClientId();
    conn->serve = impl_;
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
