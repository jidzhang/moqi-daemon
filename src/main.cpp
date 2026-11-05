//
//	moqi-daemon main: 两种运行形态共用一套分发核心。
//	形态一（stdin/stdout，阶段 5a/5b）：由 MoqiLaunch 作管道壳拉起。
//	形态二（--serve，阶段 5c）：直接绑定 \\.\pipe\<用户>\MoqiIM\Launcher，
//	MoqiLaunch 退休。协议 = 4 字节小端长度前缀 + protojson JSON。
//
#include "ClipboardMonitor.h"
#include "DaemonCloudConfig.h"
#include "DaemonEngine.h"
#include "DaemonMaintenance.h"
#include "DaemonMenuCore.h"
#include "DaemonMenuService.h"
#include "PipeServe.h"
#include "ProtoFraming.h"
#include "RimeCore.h"
#include "libIME2/src/DebugLogConfig.h"

#include <ShlObj.h>
#include <Shellapi.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>
#include <fcntl.h>
#include <functional>
#include <io.h>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
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

// 日志超过 8MB 就轮转，保留 30 天；检查降频到每 128 行一次。
void rotateDaemonLogIfNeeded(const wstring& dir) {
  static std::atomic<unsigned> ticks{0};
  if ((ticks.fetch_add(1) % 128) != 0) {
    return;
  }
  std::error_code ec;
  const std::filesystem::path current =
      std::filesystem::path(dir) / L"moqi-daemon.log";
  const auto size = std::filesystem::file_size(current, ec);
  if (ec || size < (8ull << 20)) {
    return;
  }
  SYSTEMTIME st = {};
  ::GetLocalTime(&st);
  wchar_t suffix[32] = {};
  swprintf_s(suffix, L".%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);
  const std::filesystem::path rotated =
      std::filesystem::path(dir) / (wstring(L"moqi-daemon") + suffix + L".log");
  std::filesystem::rename(current, rotated, ec);
  const auto now = std::filesystem::file_time_type::clock::now();
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    const wstring name = entry.path().filename().wstring();
    if (name == L"moqi-daemon.log" ||
        name.rfind(L"moqi-daemon.", 0) != 0 ||
        entry.path().extension() != L".log") {
      continue;
    }
    const auto written = std::filesystem::last_write_time(entry.path(), ec);
    if (!ec && now - written > std::chrono::hours(24 * 30)) {
      std::filesystem::remove(entry.path(), ec);
    }
  }
}

void daemonLog(const wstring& message) {
  // 多线程（分发/AI/云/同步泵）并发写同一个文件，必须串行化，否则
  // 行会互相穿插成 "[sync] [sync] [serve]..."，排障直接失效。
  static std::mutex logMutex;
  std::lock_guard<std::mutex> lock(logMutex);
  // 轻量日志：%LOCALAPPDATA%\MoqiIM\Log\moqi-daemon.log（追加）。
  // 统一格式：本地时间毫秒 + 线程号 + 原文；日志级别由调用方在消息里带
  // （[sync]/[ai]/[cloudclip] 等模块前缀）。
  SYSTEMTIME st = {};
  ::GetLocalTime(&st);
  wchar_t stamp[64] = {};
  swprintf_s(stamp, L"%04d-%02d-%02d %02d:%02d:%02d.%03d", st.wYear,
             st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             st.wMilliseconds);
  wchar_t* local = nullptr;
  if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
    wstring dir(local);
    ::CoTaskMemFree(local);
    dir += L"\\MoqiIM\\Log";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    rotateDaemonLogIfNeeded(dir);
    wofstream out(dir + L"\\moqi-daemon.log", ios::app);
    if (out.is_open()) {
      out << L"[" << stamp << L"][tid=" << ::GetCurrentThreadId() << L"] "
          << message << L"\n";
    }
  }
}

void daemonLogDebug(const wstring& message) {
  if (!Ime::isDebugLoggingEnabled()) {
    return;
  }
  daemonLog(message);
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

// 放 Moqi 命名空间给 ClipboardMonitor.cpp 引用。


// 回包出口：stdin 模式写本进程 stdout；serve 模式按帧内 clientId 路由到
// 对应客户端管道（见 PipeServe）。同步响应直接走请求方的 reply。
CRITICAL_SECTION g_stdoutLock;
CRITICAL_SECTION g_dispatchLock;
DaemonMenuService* g_menu = nullptr;
PipeServe* g_serve = nullptr; // 非 nullptr = serve 模式

void writeFramedToStdout(const string& framed) {
  ::EnterCriticalSection(&g_stdoutLock);
  fwrite(framed.data(), 1, framed.size(), stdout);
  fflush(stdout);
  ::LeaveCriticalSection(&g_stdoutLock);
}

// 分发动作（Go 退休 2026-11-03 后只剩一种：全部本地作答）。
enum class Action { Local };

Action actionFor(const string& m) {
  (void)m;
  // Go 退休（2026-11-03）：所有方法本地作答。菜单/命令/云剪贴板/AI 都已
  // 移植，未知方法回 success+returnValue 0（与 Go default 分支一致）。
  return Action::Local;
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
std::string readInstallationId(const std::wstring& userDir) {
  std::ifstream in(userDir + L"\\installation.yaml", std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos || line[first] == '#') continue;
    const std::string key = "installation_id:";
    if (line.compare(first, key.size(), key) != 0) continue;
    std::string value = line.substr(first + key.size());
    const size_t b = value.find_first_not_of(" \t");
    const size_t e = value.find_last_not_of(" \t");
    if (b == std::string::npos) return "";
    value = value.substr(b, e - b + 1);
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
      value = value.substr(1, value.size() - 2);
    }
    if (isSyncDeviceDirName(value)) return value;
    return "";
  }
  return "";
}

std::string currentSchemeSetNameForSync() {
  Json::Value cfg;
  std::ifstream in(moqiAppDataDir() + L"\\scheme_set.json", std::ios::binary);
  if (in) {
    Json::CharReaderBuilder builder;
    std::string errs;
    Json::parseFromStream(builder, in, &cfg, &errs);
  }
  // 安全取值：scheme_set.json 手改成数组/标量时裸 get 会抛。
  const std::string name = jsonStringOf(cfg, "current", "Rime");
  return name.empty() ? "Rime" : name;
}

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
        std::string level = jsonStringOf(cfg, "logLevel", "error");
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
      daemonLog(L"[serve] restart backends: dropping engines");
      g_clients.clear(); // 各客户端引擎会话销毁，下次按键重建
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

void dispatch(const Json::Value& req, Json::Value& resp,
              std::function<void(Json::Value)> push = nullptr) {
  const string method = req.get("method", "").asString();
  const Json::Value& keyEvent = req["keyEvent"];
  vector<BYTE> states;
  KeyEventData e;
  if (keyEvent.isObject()) {
    keyDataFromJson(keyEvent, &e, states);
  }
  ClientEntry& entry = clientFor(req);
  DaemonEngine& engine = entry.engine;
  resp = makeResponse(req);
  // 64 位客户端（DLL 进程内引擎）转发的热键：组字/候选随请求携带，本
  // 引擎会话不参与，也不等它初始化（进程内引擎用户可能永远不触发这里的
  // 冷启动，等 30s 会把 DLL 的 RPC 超时拖断）。
  // 2026-11-03：云剪贴板（Ctrl+0）无需组字也转发——先查云剪贴板（无组字
  // 依赖），再查 AI（需要组字）；两个都不匹配则 returnValue=0 交还宿主。
  if (method == "METHOD_FILTER_KEY_DOWN" && req.isMember("localComposition")) {
    if (engine.maybeShowCloudClipList(e, push)) {
      resp["returnValue"] = 1;
      return;
    }
    resp["returnValue"] = engine.maybeTriggerAI(e, push, &req) ? 1 : 0;
    return;
  }
  // 2026-11-02：INIT/ON_ACTIVATE 不再同步等引擎初始化（冷启动最长 30s），
  // 否则 DLL 的 RPC 超时先到 → 连接被拆 → 新建进程的按键全直落英文
  //（用户实报记事本里出来 "nihao" 原文）。引擎在首次按键时惰性初始化。
  // 2026-11-03：云剪贴板激活期间也跳过——64 位客户端的 daemon 侧引擎从未
  // 初始化（键全走本地引擎），ensureEngine 冷启动会把选字键的 RPC 拖超时。
  const bool initLike = (method == "METHOD_INIT" ||
                         method == "METHOD_ON_ACTIVATE");
  if (!initLike && !engine.cloudClipActive()) {
    engine.ensureEngine();
  }

  if (method == "METHOD_INIT") {
    // Go 抓包（F:\work\IME\go_init_golden.json）里 init 不带 addButton/
    // customizeUi（那些在 onActivate 回来）；这里保持一致，避免按钮重复添加。
    engine.onActivate();
    return;
  }
  if (method == "METHOD_ON_ACTIVATE") {
    engine.onActivate();
    if (g_menu != nullptr) {
      Json::Value act = g_menu->buildActivatePayload(engine);
      for (auto it = act.begin(); it != act.end(); ++it) {
        resp[it.name()] = *it;
      }
      resp["returnValue"] = 1;
    }
    return;
  }
  if (method == "METHOD_ON_PRESERVED_KEY") {
    // 云剪贴板列表热键（Ctrl+0 的 TSF preserved key）：本地触发列表；
    // 其他 GUID（Go 时代的 Shift_R 等）忽略回执。Go 退休后不再代理。
    const std::string guid = req.get("preservedKeyGuid", "").asString();
    if (guid == "{9d9f5a64-1a18-47f0-9a4f-7cbf2f5a44d9}") {
      // 直接走列表加载（无按键判定）。
      engine.showCloudClipList(push);
      resp["returnValue"] = 1;
    }
    return;
  }
  if (method == "METHOD_ON_DEACTIVATE" || method == "METHOD_CLOSE") {
    engine.deactivate();
    // 32 位应用（本引擎）学到的词在主 userdb 里；写回快照 64 位进程才看得到。
    // 反激活 = 用户切走，是安全窗口（RimeSyncUserData 会清全部会话）。
    if (engine.consumeUserDictDirty()) {
      static std::atomic<bool> masterSyncing{false};
      if (!masterSyncing.exchange(true)) {
        std::thread([]() {
          RimeRuntime& rt = RimeRuntime::instance();
          {
            std::lock_guard<std::recursive_mutex> lock(rt.mutex());
            if (rt.rimeSyncUserData() != 0) {
              rt.rimeJoinMaintenanceThread();
              // 每次提交都会跑：error 档下不落（用户反馈 daemon 日志不受控）。
  daemonLogDebug(L"[userdb] master snapshot refreshed after commit");
            }
          }
          ::EnterCriticalSection(&g_dispatchLock);
          for (auto& kv : g_clients) kv.second.engine.deactivate();
          ::LeaveCriticalSection(&g_dispatchLock);
          masterSyncing.store(false);
        }).detach();
      }
    }
    return;
  }
  // 云剪贴板列表激活时 daemon 侧引擎可能从未建会话（64 位应用的键全走
  // RPC）；列表的按键/鼠标由 DaemonEngine 的 cloudClip_ 分支处理，不需要
  // rime session。旧判断会让列表期间所有键返回 engine not ready。
  if (!engine.engineReady() && !engine.cloudClipActive()) {
    resp["success"] = false;
    resp["error"] = "engine not ready";
    return;
  }
  if (method == "METHOD_FILTER_KEY_DOWN") {
    Json::Value payload;
    if (engine.filterKeyDown(e, payload, push)) {
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
namespace {
// 异常安全的分发锁：任何时候抛出（典型：用户把 ai_config.json 字段类型
// 写错，jsoncpp 的 asString() 抛 LogicError）都必须释放临界区，否则整个
// daemon 的所有客户端永久冻结。RAII 比在每个 return 前手写 Leave 可靠。
struct DispatchLockGuard {
  explicit DispatchLockGuard(CRITICAL_SECTION* cs) : cs_(cs) {
    ::EnterCriticalSection(cs_);
  }
  ~DispatchLockGuard() { ::LeaveCriticalSection(cs_); }
  CRITICAL_SECTION* cs_;
};
}  // namespace

void dispatchRequest(const string& payload,
                     const std::function<void(const string&)>& reply) {
  DispatchLockGuard lockGuard(&g_dispatchLock);
  Json::Value req;
  if (!Proto::parseJsonPayload(payload, req)) {
    daemonLog(L"bad request frame");
    return;
  }
  const string method = req.get("method", "").asString();

  // 用户词典学习闭环（2026-11-02）：DLL 失焦导出的学习快照已拷入本引擎
  // 用户目录 sync/，就地合并入主 userdb。fire-and-forget——不回包（回包
  // 会成为迟到的零值帧，清掉重聚焦后的新组字）。
  if (method == "userdbChanged") {
    // 主词典合并 + 全量重建各客户端会话（RimeSyncUserData 内部
    // CleanupAllSessions 会销毁全部会话，不重建则后续按键直通——
    // 2026-11-02 DLL 侧同因实证过）。在途去重；fire-and-forget 不回包。
    static std::atomic<bool> syncing{false};
    if (!syncing.exchange(true)) {
      std::thread([]() {
        RimeRuntime& rt = RimeRuntime::instance();
        if (!rt.ready() || rt.rimeSyncUserData == nullptr) {
          daemonLog(L"[userdb] master sync skipped: engine not ready");
          syncing.store(false);
          return;
        }
        {
          std::lock_guard<std::recursive_mutex> lock(rt.mutex());
          if (rt.rimeSyncUserData() != 0) {
            rt.rimeJoinMaintenanceThread();
            daemonLog(L"[userdb] master merged");
          } else {
            daemonLog(L"[userdb] master sync failed");
          }
        }
        // 废死句柄：各引擎 deactivate（销毁会话+清零），下次按键惰性重建。
        ::EnterCriticalSection(&g_dispatchLock);
        for (auto& kv : g_clients) {
          kv.second.engine.deactivate();
        }
        ::LeaveCriticalSection(&g_dispatchLock);
        syncing.store(false);
      }).detach();
    }
    return;
  }

  // 5b：菜单本地作答（树=黄金）；命令本地集合就地执行，其余代理 Go。
  if (method == "METHOD_ON_MENU") {
    ClientEntry& entry = clientFor(req);
    entry.engine.ensureEngine();
    // 会话可能刚被主词典同步（CleanupAllSessions）销毁：菜单打开不需要
    // 按键/激活先行，若不重建会话，开关 9 项（中文/英文、简繁…）读不到
    // 方案 → 菜单只剩"输入方案"及以下（2026-11-03 用户实报 zcode/
    // notepad2/4 菜单不全）。onActivate 幂等：已有会话直接返回。
    entry.engine.onActivate();
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
    return;
  }
  if (method == "METHOD_ON_COMMAND") {
    const int cmdId = req.get("commandId", 0).asInt();
    executeLauncherCommands(cmdId); // serve 模式就地执行维护命令

    // ---- Go 退休（2026-11-03）：维护类命令由 daemon 本地实现 ----
    // 刷新配置(10)/更新配置(17)：先回执（菜单立即关闭），重部署/拉取放后台
    // 线程，结果用系统消息框提示（Go 时代是托盘气球，launcher 已退休）。
    if (cmdId == Menu::ID_DEPLOY || cmdId == Menu::ID_UPDATE_CONFIG) {
      {
        Json::Value resp(Json::objectValue);
        resp["success"] = true;
        resp["seqNum"] = req.get("seqNum", 0);
        resp["returnValue"] = 1;
        resp["clientId"] = req.get("clientId", "");
        string framed;
        if (Proto::serializeJsonMessage(resp, framed)) {
          reply(framed);
        }
      }
      std::thread([cmdId]() {
        std::string message;
        bool ok = false;
        if (cmdId == Menu::ID_UPDATE_CONFIG) {
          ok = Moqi::gitPullCurrentSchemeSet(&message);
          if (ok) {
            // 拉取成功后重新部署，让新配置生效（maintenance 前先清会话）。
            {
              ::EnterCriticalSection(&g_dispatchLock);
              for (auto& kv : g_clients) kv.second.engine.deactivate();
              ::LeaveCriticalSection(&g_dispatchLock);
            }
            std::string deployMessage;
            if (!Moqi::runFullRedeploy(&deployMessage)) {
              message += "（" + deployMessage + "）";
            }
          }
        } else {
          {
            ::EnterCriticalSection(&g_dispatchLock);
            for (auto& kv : g_clients) kv.second.engine.deactivate();
            ::LeaveCriticalSection(&g_dispatchLock);
          }
          ok = Moqi::runFullRedeploy(&message);
        }
        Moqi::notifyUserMessage(message, !ok);
        daemonLog(L"[maint] cmd=" + std::to_wstring(cmdId) + L" ok=" +
                  std::to_wstring(ok ? 1 : 0) + L" msg=" +
                  utf8ToWide(message.c_str()));
      }).detach();
      return;
    }
    if (cmdId == Menu::ID_APPEARANCE_IMPORT_SKIN) {
      // 文件对话框 + 解析同步执行（选完立刻回 customizeUi 实时换肤）。
      // 对话框期间用户不会打字，短暂占用分发锁可接受。
      ClientEntry& entry = clientFor(req);
      entry.engine.ensureEngine();
      // 会话可能刚被主词典同步销毁；gather 要读引擎状态，先幂等重建。
      entry.engine.onActivate();
      std::string message;
      const bool ok = Moqi::importAppearanceSkinFromDialog(&message);
      Json::Value resp(Json::objectValue);
      resp["success"] = true;
      resp["seqNum"] = req.get("seqNum", 0);
      resp["returnValue"] = ok ? 1 : 0;
      if (ok && g_menu != nullptr) {
        Menu::MenuState st = g_menu->gather(entry.engine);
        resp["customizeUi"] = g_menu->buildCustomizeUi(st);
      }
      resp["clientId"] = req.get("clientId", "");
      string framed;
      if (Proto::serializeJsonMessage(resp, framed)) {
        reply(framed);
      }
      if (!message.empty()) {
        Moqi::notifyUserMessage(message, !ok);
      }
      return;
    }

    if (g_menu != nullptr && Menu::isLocalCommandId(cmdId) && cmdId != 0) {
      ClientEntry& entry = clientFor(req);
      entry.engine.ensureEngine();
      // 会话可能刚被主词典同步销毁：命令要读/写引擎选项（开关勾选、
      // applyEngine 落选项），session=0 时全部失效。幂等重建。
      entry.engine.onActivate();
      Menu::MenuState st = g_menu->gather(entry.engine);
      Menu::CommandResult cr = Menu::handleCommand(cmdId, st);
      if (cr.action == Menu::CmdAction::Local && cr.ok) {
        g_menu->applyEngine(entry.engine, cr);
        if (cmdId >= Menu::ID_APPEARANCE_THEME_BASE) {
          g_menu->applyThemeColors(st);
        }
        if (cmdId == Menu::ID_SHARED_INPUT_STATE && st.inputStateShared) {
          // 打开共享时以当前会话为基线捕获（Go toggleInputStateShared 的
          // 开启分支——此时文件里 enabled 还是 false，须强制捕获并连同
          // enabled=true 落盘）。
          enableSharedFromSession(entry.engine.sessionId());
        }
        g_menu->persistAppearance(st);
        if (cmdId == Menu::ID_INPUT_SEMICOLON_SELECT_SECOND) {
          // Go 是命令即改内存字段；只落盘的话当前应用要切走再回来才生效。
          entry.engine.setSemicolonSelectSecond(st.semicolonSecond);
        }
        g_menu->openCommandTarget(cmdId);
        Json::Value resp(Json::objectValue);
        resp["success"] = true;
        resp["seqNum"] = req.get("seqNum", 0);
        resp["returnValue"] = 1;
        // 全量 customizeUi 随响应下发：主题/候选排列/总候选数等命令此前
        // 不带（或只带零星键），DLL 收不到就只能等重启——用户实报"很多
        // 选项都需要重启应用才行"。DLL 收到即实时应用。
        resp["customizeUi"] = g_menu->buildCustomizeUi(st);
        // 总候选数（190-193）：立即改本 daemon 所有引擎会话的 page size
        //（Go applyCandidateCountConfig 的 SetCandidatePageSize 分支）。
        // 64 位应用的进程内引擎由 DLL 侧按响应里的 candidateCount 跟随。
        if (cmdId >= 190 && cmdId <= 193) {
          for (auto& kv : g_clients) {
            kv.second.engine.applyCandidateCount(st.candCount);
          }
        }
        // 开关类命令（中英/全半角/简繁/方案集内开关）必须随响应刷新语言栏
        // 图标——Go 的 toggleOption 响应带 changeButton；此前 daemon 不带，
        // 图标要等下一次按键才更新（用户实报 Notepad4 图标左键"无效"）。
        entry.engine.appendLangStatus(resp);
        resp["clientId"] = req.get("clientId", "");
        string framed;
        if (Proto::serializeJsonMessage(resp, framed)) {
          reply(framed);
        }
        return;
      }
    }
    // 未接管（网络/方案集/未知）→ 落到代理路径
    // ---- 5e 云剪贴板命令（Go 云端已断，daemon 接管）----
    if (cmdId == 5002 && Moqi::globalCloudSync() != nullptr) {
      // 启用/禁用云剪贴板（Go toggleCloudClipboardEnabled）。
      CloudClipboardConfig cfg = loadCloudClipboardConfig();
      cfg.enabled = !cfg.enabled;
      saveCloudClipboardConfig(cfg, "", true);
      Moqi::globalCloudSync()->reloadConfig(cfg);
      Json::Value resp(Json::objectValue);
      resp["success"] = true;
      resp["seqNum"] = req.get("seqNum", 0);
      resp["returnValue"] = 1;
      resp["clientId"] = req.get("clientId", "");
      string framed;
      if (Proto::serializeJsonMessage(resp, framed)) {
        reply(framed);
      }
      return;
    }
    if (cmdId == 5003 && Moqi::globalCloudSync() != nullptr) {
      // 测试 WebDAV 连接（Go testCloudClipboardConnectionCommand）——
      // 后台执行（30s 超时），完成后异步推送 ShowMessage。同步回复回执。
      CloudClipboardConfig cfg = loadCloudClipboardConfig();
      std::thread([cfg, req, reply]() {
        CloudClipboardSync sync(cfg);
        std::string err;
        const bool ok = sync.testConnection(&err);
        // 结果经异步推送送达（Go 用 TrayNotification，daemon 无托盘改
        // ShowMessage 消息窗；此处简化：daemon 日志记录，后续列表选择器
        // 里程碑接入推送帧）。
        if (ok) {
          daemonLog(L"[cloud] WebDAV 连接成功");
          Moqi::notifyUserMessage("WebDAV 连接成功", false);
        } else {
          daemonLog(L"[cloud] 连接失败: " +
                    std::wstring(err.begin(), err.end()));
          Moqi::notifyUserMessage(
              "WebDAV 连接失败: " + (err.empty() ? "未知错误" : err), true);
        }
      }).detach();
      Json::Value resp(Json::objectValue);
      resp["success"] = true;
      resp["seqNum"] = req.get("seqNum", 0);
      resp["returnValue"] = 1;
      resp["clientId"] = req.get("clientId", "");
      string framed;
      if (Proto::serializeJsonMessage(resp, framed)) {
        reply(framed);
      }
      return;
    }
    if (cmdId == 11) {
      // 同步用户词库（Go user_dict_sync.go syncUserDataCommand 完整链路）：
      // 下载远端快照 → RimeSyncUserData 合并 → 上传本机快照。
      static std::atomic<bool> userDictSyncing{false};
      CloudClipboardConfig cfg = loadCloudClipboardConfig();
      const std::wstring userDir = moqiAppDataDir() + L"\\UserDictMaster";
      const std::string schemeSet = currentSchemeSetNameForSync();
      const std::string deviceId = readInstallationId(userDir);
      const bool webdav = cfg.isComplete() && isAllowedBaseUrl(cfg.baseUrl);
      if (!userDictSyncing.exchange(true)) {
        std::thread([cfg, userDir, schemeSet, deviceId, webdav]() {
          int downloaded = 0;
          int uploaded = 0;
          std::string err;
          auto finish = [&](const std::string& message, bool error) {
            daemonLog(L"[sync] " + utf8ToWide(message.c_str()));
            Moqi::notifyUserMessage(message, error);
            userDictSyncing.store(false);
          };
          daemonLog(L"[sync] 开始同步用户词库...");
          if (webdav) {
            CloudClipboardSync sync(cfg);
            if (!sync.downloadUserDictSnapshots(userDir + L"\\sync", schemeSet,
                                                &downloaded, &err)) {
              finish("用户词库同步失败: " + err, true);
              return;
            }
            daemonLog(L"[sync] 远端快照下载 " + std::to_wstring(downloaded));
          }
          {
            RimeRuntime& rt = RimeRuntime::instance();
            if (!rt.ready() || rt.rimeSyncUserData == nullptr) {
              finish("用户词库同步失败：Rime 后端不可用", true);
              return;
            }
            std::lock_guard<std::recursive_mutex> lock(rt.mutex());
            if (rt.rimeSyncUserData() == 0) {
              finish("用户词库同步失败: librime 合并用户词库失败", true);
              return;
            }
            rt.rimeJoinMaintenanceThread();
          }
          // RimeSyncUserData 会销毁全部会话；下次按键惰性重建。
          {
            ::EnterCriticalSection(&g_dispatchLock);
            for (auto& kv : g_clients) kv.second.engine.deactivate();
            ::LeaveCriticalSection(&g_dispatchLock);
          }
          if (webdav) {
            if (deviceId.empty()) {
              finish("用户词库同步失败: 无法读取本机 Rime installation_id",
                     true);
              return;
            }
            CloudClipboardSync sync(cfg);
            if (!sync.uploadUserDictSnapshots(userDir + L"\\sync", schemeSet,
                                              deviceId, &uploaded, &err)) {
              finish("用户词库同步失败: " + err, true);
              return;
            }
            daemonLog(L"[sync] 本机快照上传 " + std::to_wstring(uploaded));
          }
          if (!webdav) {
            finish("用户词库已在本地合并（未配置 WebDAV，未上传）", false);
          } else if (downloaded == 0 && uploaded == 0) {
            finish("用户资料同步完成", false);
          } else {
            finish("用户词库同步完成：下载 " + std::to_string(downloaded) +
                       "，上传 " + std::to_string(uploaded),
                   false);
          }
        }).detach();
      } else {
        Moqi::notifyUserMessage("用户词库同步已在进行中", false);
      }
      // 回执（fire-and-forget 不等结果）
      Json::Value resp(Json::objectValue);
      resp["success"] = true;
      resp["seqNum"] = req.get("seqNum", 0);
      resp["returnValue"] = 1;
      resp["clientId"] = req.get("clientId", "");
      string framed;
      if (Proto::serializeJsonMessage(resp, framed)) {
        reply(framed);
      }
      return;
    }
    if (cmdId == 5004 || cmdId == 5001) {
      // WebDAV/云剪贴板配置：打开 cloud_clipboard.json（多数用户不知道
      // %APPDATA% 在哪）。两个 id 都指向同一文件（Go 时代是两个对话框，
      // 2026-11-03 合并为一个配置入口）。编辑器=随包 Notepad4 优先。
      std::wstring path = moqiAppDataDir() + L"\\cloud_clipboard.json";
      if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // 不存在 → 写一份带注释的默认模板。
        const char* tmpl =
            "{\n"
            "  \"enabled\": false,\n"
            "  \"base_url\": \"\",\n"
            "  \"username\": \"\",\n"
            "  \"password\": \"\",\n"
            "  \"settings_root\": \"/moqi-input-method/\",\n"
            "  \"min_interval_sec\": 10,\n"
            "  \"list_hotkey\": \"Ctrl+0\"\n"
            "}\n";
        std::ofstream out(path, std::ios::binary);
        out.write(tmpl, strlen(tmpl));
      }
      openTextInBundledEditor(g_daemonProgramDir, path);
      Json::Value resp(Json::objectValue);
      resp["success"] = true;
      resp["seqNum"] = req.get("seqNum", 0);
      resp["returnValue"] = 1;
      resp["clientId"] = req.get("clientId", "");
      string framed;
      if (Proto::serializeJsonMessage(resp, framed)) {
        reply(framed);
      }
      return;
    }
  }

  const Action action = actionFor(method);
  (void)action;

  Json::Value resp;
  // AI 异步结果推动：把更新帧补上 clientId 后走同一回包出口
  //（serve 模式回写到该客户端管道；stdin 模式写 stdout）。
  const string pushClientId = req.get("clientId", "").asString();
  auto pushToClient = [reply, pushClientId](Json::Value update) {
    update["clientId"] = pushClientId;
    string framed;
    if (Proto::serializeJsonMessage(update, framed)) {
      reply(framed);
    }
  };
  try {
    dispatch(req, resp, pushToClient);
  } catch (const std::exception& e) {
    daemonLog(L"dispatch exception: " +
              std::wstring(e.what(), e.what() + strlen(e.what())));
    resp = makeResponse(req);
    resp["success"] = false;
    resp["error"] = "internal error";
  } catch (...) {
    daemonLog(L"dispatch unknown exception");
    resp = makeResponse(req);
    resp["success"] = false;
    resp["error"] = "internal error";
  }
  resp["clientId"] = req.get("clientId", "");
  string framed;
  if (Proto::serializeJsonMessage(resp, framed)) {
    reply(framed);
  }
}

} // namespace

// PipeServe.cpp 共用的轻量日志（匿名命名空间外链接，须在 Moqi 命名空间）。
namespace Moqi {
void daemonLogExtern(const wstring& message) { daemonLog(message); }
void daemonLogDebugExtern(const wstring& message) { daemonLogDebug(message); }
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

  // 阶段 5b：菜单树与本地命令由 daemon 自建（黄金锁定，见 tests/）。
  static DaemonMenuService menu(
      g_daemonProgramDir,
      g_daemonProgramDir + L"\\moqi-ime\\input_methods\\rime\\data", L"");
  g_menu = &menu;
  // 5e 云剪贴板同步引擎（Go globalCloudClipboardService 等价物）。
  static Moqi::CloudClipboardSync cloudSync(loadCloudClipboardConfig());
  Moqi::setGlobalCloudSync(&cloudSync);
  // 剪贴板监听线程（配置未启用时挂空，只读不传）。
  startClipboardMonitor();

  // 用户词典跨进程合并泵（2026-11-03）：DLL 的 userdbChanged 通知可能因管道
  // 陈旧/重连窗口丢失（实测导出成功但 daemon 无合并日志）。每 15 秒扫一次
  // 主词典 sync 目录 mtime，有新快照就合并并重建各客户端会话，保证"一个应用
  // 学会的词，其它应用/重启后都能用"。
  std::thread([]() {
    const wstring syncDir = moqiAppDataDir() + L"\\UserDictMaster\\sync";
    FILETIME last = {};
    for (;;) {
      ::Sleep(15000);
      // 只看 p-*（64 位进程导出的学习快照）：daemon 自己的 Backup 会每次
      // 刷新 sync/<uuid>/，若把它也算进来会形成"合并→Backup→又新→再合并"
      // 的 15 秒死循环。并且要比**文件**时间：同名快照反复覆盖时目录 mtime
      // 不变（2026-11-03 实测），用目录时间会漏掉后续学习。
      FILETIME newest = {};
      WIN32_FIND_DATAW fd = {};
      HANDLE hFind = ::FindFirstFileW((syncDir + L"\\p-*").c_str(), &fd);
      if (hFind != INVALID_HANDLE_VALUE) {
        do {
          if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
          WIN32_FIND_DATAW f2 = {};
          HANDLE hF2 = ::FindFirstFileW(
              (syncDir + L"\\" + fd.cFileName + L"\\*.userdb.txt").c_str(),
              &f2);
          if (hF2 == INVALID_HANDLE_VALUE) continue;
          do {
            if (CompareFileTime(&f2.ftLastWriteTime, &newest) > 0) {
              newest = f2.ftLastWriteTime;
            }
          } while (::FindNextFileW(hF2, &f2));
          ::FindClose(hF2);
        } while (::FindNextFileW(hFind, &fd));
        ::FindClose(hFind);
      }
      if (newest.dwLowDateTime == 0 && newest.dwHighDateTime == 0) {
        continue;
      }
      if (last.dwLowDateTime == 0 && last.dwHighDateTime == 0) {
        // 首次运行：已有快照就合并一次（干净启动时可能为空）。
        last = newest;
      } else if (CompareFileTime(&newest, &last) <= 0) {
        continue;
      } else {
        last = newest;
      }
      RimeRuntime& rt = RimeRuntime::instance();
      if (!rt.ready() || rt.rimeSyncUserData == nullptr) {
        continue;
      }
      {
        std::lock_guard<std::recursive_mutex> lock(rt.mutex());
        if (rt.rimeSyncUserData() != 0) {
          rt.rimeJoinMaintenanceThread();
          daemonLog(L"[userdb] pump merged");
        } else {
          daemonLog(L"[userdb] pump sync failed");
        }
      }
      ::EnterCriticalSection(&g_dispatchLock);
      for (auto& kv : g_clients) kv.second.engine.deactivate();
      ::LeaveCriticalSection(&g_dispatchLock);
    }
  }).detach();

  if (serveMode) {
    static PipeServe serve;
    g_serve = &serve;
    // 先绑管道再后台预热：旧顺序同步 ensureEngine（最长 30 秒）在
    // serve.run() 之前，冷启动时客户端根本连不上管道，首键直落英文。
    // 多实例竞争时只有抢到管道的实例才预热，否则输家也会白建一份引擎。
    std::thread([]() {
      while (!g_serve->ownsPipe()) {
        ::Sleep(50);
      }
      DaemonEngine warm;
      warm.ensureEngine();
      warm.onActivate();
      warm.deactivate();
      daemonLog(L"[serve] engine warmup done");
    }).detach();
    daemonLog(L"[serve] log=%LOCALAPPDATA%\\MoqiIM\\Log\\moqi-daemon.log debug=" +
              wstring(Ime::isDebugLoggingEnabled() ? L"1" : L"0"));
    daemonLog(L"moqi-daemon serve boot (pipe server, launcher retired)");
    const int code = serve.run();
    daemonLog(L"moqi-daemon serve exit");
    // 命名空间静态 std::thread 若仍 joinable，析构会 std::terminate：
    // 干净退出必须停监听并断开全局同步指针。
    Moqi::stopClipboardMonitor();
    Moqi::setGlobalCloudSync(nullptr);
    return code;
  }

  // stdin 模式 = 老 MoqiLaunch 的兼容形态。2.0 里 serve 守护才是正主：
  // 若已有 daemon 实例在跑（单实例互斥体被占），stdin 模式直接退出——
  // 两个进程抢同一个 UserDictMaster 会造成 LevelDB 锁互等、引擎卡死
  //（2026-11-03 验收实测：升级机上老 DLL 守护拉起 MoqiLaunch → 又拉一个
  // stdin daemon，与 serve daemon 抢锁，管道假死）。
  if (!serveMode) {
    DWORD sessionId = 0;
  ::ProcessIdToSessionId(::GetCurrentProcessId(), &sessionId);
  const std::wstring probeMutex =
      L"MoqiLaunchMutex." + std::to_wstring(sessionId);
  HANDLE probe = ::CreateMutexW(nullptr, FALSE, probeMutex.c_str());
    const bool already = probe != nullptr &&
                         ::GetLastError() == ERROR_ALREADY_EXISTS;
    if (probe != nullptr) {
      ::CloseHandle(probe);
    }
    if (already) {
      daemonLog(L"stdin mode refused: another daemon owns the engine");
      return 0;
    }
  }

  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
  daemonLog(L"moqi-daemon start (local engine, Go retired)");

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
  daemonLog(L"moqi-daemon exit");
  Moqi::stopClipboardMonitor();
  Moqi::setGlobalCloudSync(nullptr);
  return 0;
}
