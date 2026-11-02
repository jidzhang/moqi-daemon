//
//	moqi-daemon main: 两种运行形态共用一套分发核心。
//	形态一（stdin/stdout，阶段 5a/5b）：由 MoqiLaunch 作管道壳拉起。
//	形态二（--serve，阶段 5c）：直接绑定 \\.\pipe\<用户>\MoqiIM\Launcher，
//	MoqiLaunch 退休。协议 = 4 字节小端长度前缀 + protojson JSON。
//
#include "ChildProxy.h"
#include "DaemonEngine.h"
#include "DaemonMenuCore.h"
#include "DaemonMenuService.h"
#include "PipeServe.h"
#include "ProtoFraming.h"
#include "RimeCore.h"

#include <ShlObj.h>
#include <fcntl.h>
#include <functional>
#include <io.h>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
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

// 回包出口：stdin 模式写本进程 stdout；serve 模式按帧内 clientId 路由到
// 对应客户端管道（见 PipeServe）。同步响应直接走请求方的 reply。
CRITICAL_SECTION g_stdoutLock;
CRITICAL_SECTION g_dispatchLock;
ChildProxy* g_proxy = nullptr;
DaemonMenuService* g_menu = nullptr;
PipeServe* g_serve = nullptr; // 非 nullptr = serve 模式

void writeFramedToStdout(const string& framed) {
  ::EnterCriticalSection(&g_stdoutLock);
  fwrite(framed.data(), 1, framed.size(), stdout);
  fflush(stdout);
  ::LeaveCriticalSection(&g_stdoutLock);
}

// ChildProxy 读者线程的出口：serve 模式按 clientId 路由，找不到了（镜像
// dm-drop: 前缀 / 已断开客户端）丢弃；stdin 模式全部写 stdout。
void routeGoFrame(const string& framed) {
  if (g_serve != nullptr) {
    g_serve->routeFrame(framed);
    return;
  }
  writeFramedToStdout(framed);
}

// 分发动作：热方法本地答；冷方法原样转 Go；状态方法本地答 + 镜像给 Go
// （镜像帧 clientId 加 dm-drop: 前缀——壳按 clientId 路由回包，找不到
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

// launcher 级维护命令（30/32/33）：serve 模式下没有 MoqiLaunch 替我们执行，
// daemon 就地执行。stdin 模式下 MoqiLaunch 会在代理路径上拦截执行，这里
// 不动（与 Go 后端只回执的行为一致）。
void executeLauncherCommands(int cmdId) {
  if (g_serve == nullptr) {
    return;
  }
  switch (cmdId) {
    case Menu::ID_LAUNCHER_DEBUG_LOG: {
      wchar_t* local = nullptr;
      if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr,
                                           &local))) {
        wstring dir(local);
        ::CoTaskMemFree(local);
        dir += L"\\MoqiIM";
        ::CreateDirectoryW(dir.c_str(), nullptr);
        const wstring path = dir + L"\\MoqiLauncher.json";
        // 翻转 trace/error（DLL 读同一文件决定 tsf-debug 开关）
        Json::Value cfg;
        std::ifstream in(path, std::ios::binary);
        if (in) {
          std::string errs;
          Json::CharReaderBuilder rb;
          Json::parseFromStream(rb, in, &cfg, &errs);
        }
        if (!cfg.isObject()) cfg = Json::Value(Json::objectValue);
        std::string level = cfg.get("logLevel", "error").asString();
        cfg["logLevel"] =
            (level == "debug" || level == "trace") ? "error" : "trace";
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (out) {
          Json::StreamWriterBuilder wb;
          wb["commentStyle"] = "None";
          wb["indentation"] = "\t";
          std::unique_ptr<Json::StreamWriter> w(wb.newStreamWriter());
          w->write(cfg, &out);
          out << "\n";
        }
        daemonLog(L"[serve] debug log toggled");
      }
      break;
    }
    case Menu::ID_LAUNCHER_RESTART_BACKENDS:
      daemonLog(L"[serve] restart backends: dropping engines + Go child");
      g_clients.clear(); // 各客户端引擎会话销毁，下次按键重建
      if (g_proxy != nullptr) {
        g_proxy->shutdown(); // 下一次 forward 惰性重拉 Go
      }
      break;
    case Menu::ID_LAUNCHER_QUIT:
      daemonLog(L"[serve] quit requested (exiting; TSF will relaunch)");
      if (g_serve != nullptr) {
        g_serve->requestStop();
      }
      break;
    default:
      break;
  }
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
  // 其余（菜单/命令/AI/云剪贴板/未知）：不走本地，由 dispatchRequest 分流。
}

// 单帧请求的完整处理（两种运行形态共用）。reply = 请求方连接的写回出口。
// serve 模式多客户端线程并发进入，g_dispatchLock 串行化（单发单收，
// AI/云这类长请求由 Go 子进程异步回推，不占分发锁）。
void dispatchRequest(const string& payload,
                     const std::function<void(const string&)>& reply) {
  ::EnterCriticalSection(&g_dispatchLock);
  Json::Value req;
  if (!Proto::parseJsonPayload(payload, req)) {
    daemonLog(L"bad request frame");
    ::LeaveCriticalSection(&g_dispatchLock);
    return;
  }
  const string method = req.get("method", "").asString();

  // 5b：菜单本地作答（树=黄金）；命令本地集合就地执行，其余代理 Go。
  if (method == "METHOD_ON_MENU") {
    ClientEntry& entry = clientFor(req);
    entry.engine.ensureEngine();
    Json::Value resp(Json::objectValue);
    resp["success"] = true;
    resp["seqNum"] = req.get("seqNum", 0);
    resp["returnValue"] = 1;
    if (g_menu != nullptr) {
      resp["menuItems"] = Menu::buildMenu(g_menu->gather(entry.engine));
    }
    resp["clientId"] = req.get("clientId", "");
    string framed;
    if (Proto::serializeJsonMessage(resp, framed)) {
      reply(framed);
    }
    ::LeaveCriticalSection(&g_dispatchLock);
    return;
  }
  if (method == "METHOD_ON_COMMAND") {
    const int cmdId = req.get("commandId", 0).asInt();
    executeLauncherCommands(cmdId); // serve 模式就地执行维护命令
    if (g_menu != nullptr && Menu::isLocalCommandId(cmdId) && cmdId != 0) {
      ClientEntry& entry = clientFor(req);
      entry.engine.ensureEngine();
      Menu::MenuState st = g_menu->gather(entry.engine);
      Menu::CommandResult cr = Menu::handleCommand(cmdId, st);
      if (cr.action == Menu::CmdAction::Local && cr.ok) {
        g_menu->applyEngine(entry.engine, cr);
        if (cmdId >= Menu::ID_APPEARANCE_THEME_BASE) {
          g_menu->applyThemeColors(st);
        }
        g_menu->persistAppearance(st);
        g_menu->openCommandTarget(cmdId);
        Json::Value resp(Json::objectValue);
        resp["success"] = true;
        resp["seqNum"] = req.get("seqNum", 0);
        resp["returnValue"] = 1;
        if (cr.customizeUi.isObject() && !cr.customizeUi.empty()) {
          resp["customizeUi"] = cr.customizeUi;
        }
        resp["clientId"] = req.get("clientId", "");
        string framed;
        if (Proto::serializeJsonMessage(resp, framed)) {
          reply(framed);
        }
        ::LeaveCriticalSection(&g_dispatchLock);
        return;
      }
    }
    // 未接管（网络/方案集/未知）→ 落到代理路径
  }

  const Action action = actionFor(method);

  // init/onActivate：本地引擎状态照常建立，但响应透传 Go 的
  //（addButton/selKeys/customizeUi 只有 Go 有）。
  if (action == Action::ProxyWait) {
    Json::Value localResp;
    dispatch(req, localResp);
    string goResp;
    if (g_proxy != nullptr &&
        g_proxy->forwardAndWait(payload, req.get("seqNum", -1).asInt(),
                                15000, &goResp)) {
      reply(Proto::framePayload(goResp));
      ::LeaveCriticalSection(&g_dispatchLock);
      return;
    }
    daemonLog(L"proxy-wait failed, falling back to local response");
    localResp["clientId"] = req.get("clientId", "");
    string framed;
    if (Proto::serializeJsonMessage(localResp, framed)) {
      reply(framed);
    }
    ::LeaveCriticalSection(&g_dispatchLock);
    return;
  }

  if (action == Action::Proxy) {
    // 原样转发（帧内已带壳注入的 clientId），响应由读者线程路由。
    if (g_proxy != nullptr && g_proxy->forward(payload)) {
      ::LeaveCriticalSection(&g_dispatchLock);
      return;
    }
    // Go 缺席/转发失败：回 success-empty（可降级运行）。
    daemonLog(L"proxy forward failed, replying empty");
  }

  Json::Value resp;
  dispatch(req, resp);
  resp["clientId"] = req.get("clientId", "");
  string framed;
  if (Proto::serializeJsonMessage(resp, framed)) {
    reply(framed);
  }
  if (action == Action::LocalAndMirror) {
    mirrorToGo(req);
  }
  ::LeaveCriticalSection(&g_dispatchLock);
}

} // namespace

// PipeServe.cpp 共用的轻量日志（匿名命名空间外链接，须在 Moqi 命名空间）。
namespace Moqi {
void daemonLogExtern(const wstring& message) { daemonLog(message); }
} // namespace Moqi

// PipeServe 线程入口的桥（头文件不掺匿名命名空间细节）。
namespace Moqi {
void MoqiServeDispatch(const std::string& payload,
                       const std::function<void(const std::string&)>& reply) {
  try {
    dispatchRequest(payload, reply);
  } catch (const std::exception& e) {
    daemonLog(L"[serve] dispatch exception: " +
              wstring(e.what(), e.what() + strlen(e.what())));
  } catch (...) {
    daemonLog(L"[serve] dispatch unknown exception");
  }
}
} // namespace Moqi

int main(int argc, char** argv) {
  bool serveMode = false;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--serve") == 0) {
      serveMode = true;
    }
  }

  resolveLayout();
  setCoreLogSink(&daemonLog);
  ::InitializeCriticalSection(&g_stdoutLock);
  ::InitializeCriticalSection(&g_dispatchLock);

  // 阶段 5a：Go 子进程承接冷方法（AI/云剪贴板/方案集），启动即预热。
  static ChildProxy proxy(g_daemonProgramDir + L"\\moqi-ime\\MoqiIME.exe",
                          g_daemonProgramDir + L"\\moqi-ime", &routeGoFrame);
  g_proxy = &proxy;
  // 阶段 5b：菜单树与本地命令由 daemon 自建（黄金锁定，见 tests/）。
  static DaemonMenuService menu(
      g_daemonProgramDir,
      g_daemonProgramDir + L"\\moqi-ime\\input_methods\\rime\\data", L"");
  g_menu = &menu;
  if (proxy.available()) {
    proxy.ensureStarted();
  }

  if (serveMode) {
    // 预热自身引擎（对齐壳行为：首键不等冷启动）。
    {
      ClientEntry& warm = g_clients["serve-warmup"];
      warm.engine.ensureEngine();
      warm.engine.onActivate();
    }
    static PipeServe serve;
    g_serve = &serve;
    daemonLog(L"moqi-daemon serve start (pipe server, launcher retired)");
    const int code = serve.run();
    daemonLog(L"moqi-daemon serve exit");
    proxy.shutdown();
    return code;
  }

  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
  daemonLog(proxy.available() ? L"moqi-daemon start (Go child warm)"
                              : L"moqi-daemon start (no Go child, degraded)");

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
      dispatchRequest(payload, &writeFramedToStdout);
    }
  }
  proxy.shutdown();
  daemonLog(L"moqi-daemon exit");
  return 0;
}
