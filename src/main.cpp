//
//	moqi-daemon main: stdin/stdout JSON-frame loop replacing the Go backend
//	for the RPC fallback path (phase 5). Protocol = 4-byte LE length prefix
//	+ protojson-compatible JSON, identical to what the launcher speaks to
//	MoqiIME.exe today. Only the key path is implemented; cold methods
//	(menus/AI/cloud clipboard) return success-empty by design (user
//	2026-11-01: 用不到的接口不写，Go 代码保留可随时切回).
//
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
  // 冷方法（菜单/AI/云剪贴板等）：success 空实现——按定案不写，
  // 走 Go 后端才有完整行为。
  if (method == "METHOD_ON_COMMAND" || method == "METHOD_ON_MENU" ||
      method == "METHOD_ON_PRESERVED_KEY" ||
      method == "METHOD_ON_COMPARTMENT_CHANGED" ||
      method == "METHOD_ON_KEYBOARD_STATUS_CHANGED" ||
      method == "METHOD_CLOUD_CLIPBOARD_UPLOAD") {
    return;
  }
}

} // namespace

int main() {
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);

  resolveLayout();
  setCoreLogSink(&daemonLog);
  daemonLog(L"moqi-daemon start");

  Proto::FrameBuffer frames;
  char buf[8192];
  while (true) {
    size_t got = fread(buf, 1, sizeof(buf), stdin);
    if (got == 0) {
      break; // launcher closed stdin -> exit
    }
    frames.append(buf, got);
    string payload;
    while (frames.nextFrame(payload)) {
      Json::Value req;
      if (!Proto::parseJsonPayload(payload, req)) {
        daemonLog(L"bad request frame, exiting");
        return 1;
      }
      Json::Value resp;
      dispatch(req, resp);
      // 启动器按 clientId 路由响应，必须回填。
      resp["clientId"] = req.get("clientId", "");
      string framed;
      if (Proto::serializeJsonMessage(resp, framed)) {
        fwrite(framed.data(), 1, framed.size(), stdout);
        fflush(stdout);
      }
    }
  }
  daemonLog(L"moqi-daemon exit");
  return 0;
}
