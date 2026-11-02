//
//	moqi-daemon main: stdin/stdout JSON-frame loop replacing the Go backend
//	for the RPC fallback path (phase 5). Protocol = 4-byte LE length prefix
//	+ protojson-compatible JSON, identical to what the launcher speaks to
//	MoqiIME.exe today. Only the key path is implemented; cold methods
//	(menus/AI/cloud clipboard) return success-empty by design (user
//	2026-11-01: 用不到的接口不写，Go 代码保留可随时切回).
//
#include "ChildProxy.h"
#include "DaemonEngine.h"
#include "ProtoFraming.h"
#include "RimeCore.h"

#include <ShlObj.h>
#include <fcntl.h>
#include <io.h>
#include <fstream>
#include <iostream>
#include <map>
#include <string>

using namespace std;
using namespace Moqi;

namespace Moqi {
extern std::wstring g_daemonProgramDir;
extern std::wstring g_daemonBackendDir;
}

namespace {

wstring exeDir() {
  wchar_t path[MAX_PATH] = {};
  ::GetModuleFileNameW(nullptr, path, MAX_PATH);
  wstring dir(path);
  const size_t slash = dir.find_last_of(L'\\');
  return slash == wstring::npos ? wstring() : dir.substr(0, slash);
}

// 安装布局推导：<install>\moqi-ime\input_methods\rime —— helper exe 放在
// 安装根（与 MoqiLaunch.exe 同级）时成立；开发调试可用环境变量覆盖。
void resolveLayout() {
  const wchar_t* overrideDir = _wgetenv(L"MOQI_PROGRAM_DIR");
  wstring programDir = overrideDir && *overrideDir ? wstring(overrideDir) : exeDir();
  g_daemonProgramDir = programDir;
  g_daemonBackendDir = L"moqi-ime";
}

void daemonLog(const wstring& message) {
  // 轻量日志：%LOCALAPPDATA%\MoqiIM\Log\moqi-daemon.log（追加）。
  wchar_t* local = nullptr;
  if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
    wstring dir(local);
    ::CoTaskMemFree(local);
    dir += L"\\MoqiIM\\Log";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    wofstream out(dir + L"\\moqi-daemon.log", ios::app);
    if (out.is_open()) {
      out << message << L"\n";
    }
  }
}

// keyEvent JSON -> KeyEventData（keyStates 拷进调用方提供的缓冲）。
bool keyDataFromJson(const Json::Value& j, KeyEventData* e, vector<BYTE>& states) {
  states.assign(256, 0);
  if (j.isObject() && j["keyStates"].isArray()) {
    int i = 0;
    for (const auto& v : j["keyStates"]) {
      if (i >= 256) break;
      states[i++] = static_cast<BYTE>(v.asInt());
    }
  }
  e->keyCode = j.get("keyCode", 0).asUInt();
  e->charCode = j.get("charCode", 0).asUInt();
  e->scanCode = j.get("scanCode", 0).asUInt();
  e->isExtended = j.get("isExtended", false).asBool();
  e->keyStates = states.data();
  return true;
}

struct ClientEntry {
  DaemonEngine engine;
};

map<string, ClientEntry> g_clients;

// 本进程 stdout 由主线程与子进程读线程并发写，必须串行化。
CRITICAL_SECTION g_stdoutLock;
ChildProxy* g_proxy = nullptr;

void writeFramed(const std::string& framed) {
  ::EnterCriticalSection(&g_stdoutLock);
  fwrite(framed.data(), 1, framed.size(), stdout);
  fflush(stdout);
  ::LeaveCriticalSection(&g_stdoutLock);
}

// 分发动作：热方法本地答；冷方法原样转 Go；状态方法本地答 + 镜像给 Go
// （镜像帧 clientId 加 dm-drop: 前缀——启动器按 clientId 路由回包，找不到
// 客户端即丢弃，Go 侧状态得以建立而陈旧响应不会污染 DLL）。
// init/onActivate 走 ProxyWait：addButton/selKeys/customizeUi 由 Go 生成，
// 必须把 Go 的响应原样透传给 DLL（本地引擎状态照常建立，本地响应丢弃）。
enum class Action { Local, Proxy, LocalAndMirror, ProxyWait };

bool isKeyMethod(const string& m) {
  return m == "METHOD_FILTER_KEY_DOWN" || m == "METHOD_ON_KEY_DOWN" ||
         m == "METHOD_FILTER_KEY_UP" || m == "METHOD_ON_KEY_UP" ||
         m == "METHOD_HIGHLIGHT_CANDIDATE" ||
         m == "METHOD_SELECT_CANDIDATE" || m == "METHOD_CHANGE_PAGE";
}

Action actionFor(const string& m) {
  if (isKeyMethod(m)) {
    return Action::Local;
  }
  if (m == "METHOD_INIT" || m == "METHOD_ON_ACTIVATE") {
    return Action::ProxyWait;
  }
  if (m == "METHOD_ON_DEACTIVATE" || m == "METHOD_CLOSE" ||
      m == "METHOD_ON_COMPOSITION_TERMINATED") {
    return Action::LocalAndMirror;
  }
  // ON_COMMAND/ON_MENU/PRESERVED_KEY/COMPARTMENT/KEYBOARD_STATUS/
  // CLOUD_CLIPBOARD_UPLOAD 及未知方法：Go 实现（菜单/AI/云剪贴板）。
  return Action::Proxy;
}

void mirrorToGo(const Json::Value& req) {
  if (g_proxy == nullptr || !g_proxy->available()) {
    return;
  }
  Json::Value copy = req;
  copy["clientId"] = "dm-drop:" + req.get("clientId", "").asString();
  Json::StreamWriterBuilder builder;
  builder["commentStyle"] = "None";
  builder["indentation"] = "";
  const string payload = Json::writeString(builder, copy);
  if (!g_proxy->forward(payload)) {
    daemonLog(L"mirror to Go backend failed");
  }
}

ClientEntry& clientFor(const Json::Value& req) {
  return g_clients[req.get("clientId", "").asString()];
}

Json::Value makeResponse(const Json::Value& req) {
  Json::Value resp(Json::objectValue);
  resp["success"] = true;
  resp["seqNum"] = req.get("seqNum", 0);
  resp["returnValue"] = 0;
  return resp;
}

void dispatch(const Json::Value& req, Json::Value& resp) {
  const string method = req.get("method", "").asString();
  const Json::Value& keyEvent = req["keyEvent"];
  vector<BYTE> states;
  KeyEventData e;
  if (keyEvent.isObject()) {
    keyDataFromJson(keyEvent, &e, states);
  }
  ClientEntry& entry = clientFor(req);
  DaemonEngine& engine = entry.engine;
  engine.ensureEngine();
  resp = makeResponse(req);

  if (method == "METHOD_INIT") {
    engine.onActivate();
    return; // init 时已建会话即可
  }
  if (method == "METHOD_ON_ACTIVATE") {
    engine.onActivate();
    return;
  }
  if (method == "METHOD_ON_DEACTIVATE" || method == "METHOD_CLOSE") {
    engine.deactivate();
    return;
  }
  if (!engine.engineReady()) {
    resp["success"] = false;
    resp["error"] = "engine not ready";
    return;
  }
  if (method == "METHOD_FILTER_KEY_DOWN") {
    Json::Value payload;
    if (engine.filterKeyDown(e, payload)) {
      payload["success"] = true;
      payload["seqNum"] = resp["seqNum"];
      resp = payload;
    }
    return;
  }
  if (method == "METHOD_ON_KEY_DOWN") {
    Json::Value payload;
    if (engine.onKeyDown(e, payload)) {
      payload["success"] = true;
      payload["seqNum"] = resp["seqNum"];
      resp = payload;
    }
    return;
  }
  if (method == "METHOD_FILTER_KEY_UP") {
    Json::Value payload;
    if (engine.filterKeyUp(e, payload)) {
      payload["success"] = true;
      payload["seqNum"] = resp["seqNum"];
      resp = payload;
    }
    return;
  }
  if (method == "METHOD_ON_KEY_UP") {
    Json::Value payload;
    if (engine.onKeyUp(e, payload)) {
      payload["success"] = true;
      payload["seqNum"] = resp["seqNum"];
      resp = payload;
    }
    return;
  }
  if (method == "METHOD_HIGHLIGHT_CANDIDATE") {
    engine.highlightCandidate(req.get("candidateIndex", -1).asInt(), resp);
    return;
  }
  if (method == "METHOD_SELECT_CANDIDATE") {
    engine.selectCandidate(req.get("candidateIndex", -1).asInt(), resp);
    return;
  }
  if (method == "METHOD_CHANGE_PAGE") {
    engine.changePage(req.get("pageBackward", false).asBool(), resp);
    return;
  }
  if (method == "METHOD_ON_COMPOSITION_TERMINATED") {
    engine.onCompositionTerminated(req.get("forced", false).asBool());
    resp["returnValue"] = 1;
    return;
  }
  // 其余（菜单/命令/AI/云剪贴板/未知）：不走本地，由主循环转 Go 子进程。
}

} // namespace

int main() {
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);

  resolveLayout();
  setCoreLogSink(&daemonLog);
  ::InitializeCriticalSection(&g_stdoutLock);

  // 阶段 5a：Go 子进程承接冷方法（菜单/命令/AI/云剪贴板），启动即预热，
  // 对齐启动器行为（菜单首次点击不等冷启动）。exe 缺席时优雅降级。
  ChildProxy proxy(g_daemonProgramDir + L"\\moqi-ime\\MoqiIME.exe",
                   g_daemonProgramDir + L"\\moqi-ime", &writeFramed);
  g_proxy = &proxy;
  if (proxy.available()) {
    proxy.ensureStarted();
    daemonLog(L"moqi-daemon start (Go child warm)");
  } else {
    daemonLog(L"moqi-daemon start (no Go child, cold methods degraded)");
  }

  Proto::FrameBuffer frames;
  char buf[8192];
  while (true) {
    // _read 返回当前可读字节（fread 会等满 8KB 才返回，活管道场景永远等不齐）。
    int got = _read(_fileno(stdin), buf, sizeof(buf));
    if (got <= 0) {
      break; // launcher closed stdin -> exit
    }
    frames.append(buf, static_cast<size_t>(got));
    string payload;
    while (frames.nextFrame(payload)) {
      Json::Value req;
      if (!Proto::parseJsonPayload(payload, req)) {
        daemonLog(L"bad request frame, exiting");
        return 1;
      }
      const string method = req.get("method", "").asString();
      const Action action = actionFor(method);

      // init/onActivate：本地引擎状态照常建立，但响应透传 Go 的
      // （addButton/selKeys/customizeUi 只有 Go 有）。
      if (action == Action::ProxyWait) {
        Json::Value localResp;
        dispatch(req, localResp);
        string goResp;
        if (g_proxy != nullptr &&
            g_proxy->forwardAndWait(payload, req.get("seqNum", -1).asInt(),
                                    15000, &goResp)) {
          writeFramed(Proto::framePayload(goResp));
          continue;
        }
        daemonLog(L"proxy-wait failed, falling back to local response");
        localResp["clientId"] = req.get("clientId", "");
        string framed;
        if (Proto::serializeJsonMessage(localResp, framed)) {
          writeFramed(framed);
        }
        continue;
      }

      if (action == Action::Proxy) {
        // 原样转发（帧内已带启动器注入的 clientId），响应由读者线程回写。
        if (g_proxy != nullptr && g_proxy->forward(payload)) {
          continue;
        }
        // Go 缺席/转发失败：回 success-empty，菜单表现为空（可降级运行）。
        daemonLog(L"proxy forward failed, replying empty");
      }

      Json::Value resp;
      dispatch(req, resp);
      // 启动器按 clientId 路由响应，必须回填。
      resp["clientId"] = req.get("clientId", "");
      string framed;
      if (Proto::serializeJsonMessage(resp, framed)) {
        writeFramed(framed);
      }
      if (action == Action::LocalAndMirror) {
        mirrorToGo(req);
      }
    }
  }
  proxy.shutdown();
  daemonLog(L"moqi-daemon exit");
  return 0;
}
