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
#include "DaemonToolbar.h"
#include "DaemonTray.h"
#include "OsVersion.h"
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
#include <set>
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
// 当前处于激活态的客户端 id 集合（工具栏显隐驱动，见 METHOD_ON_ACTIVATE）。
std::set<std::string> g_activeClientIds;
// 最近一次有按键/激活活动的客户端 id。工具栏读状态必须读它——读
// g_clients.begin() 拿到的是 map 首元素，未必是用户正在打字的那个，
// 表现就是"按 Shift 切了英文，工具栏还显示中文"（2026-11-19 用户实报）。
// 空 = 还没有任何活动，回退到 begin()。
std::string g_lastActiveClientId;
// 菜单树缓存（2026-11-19）：首次点"墨"要等引擎冷启动（ensureEngine 最长
// 30 秒），用户实报"卡好几秒"。daemon 启动预热时就把菜单树抓一份存这里，
// 首次点击直接用，不阻塞 UI。之后每次点击仍实时 gather（引擎已热，
// 毫秒级），保证勾选状态是最新的。
Json::Value g_cachedMenuTree(Json::arrayValue);
std::mutex g_menuTreeMutex;          // 保护 g_cachedMenuTree 的读写
std::atomic<bool> g_menuTreeReady{false};
std::atomic<bool> g_menuTreeRefreshing{false};

// 后台刷新菜单树缓存。点击"墨"时只发这条路、不等它，所以点击零延迟。
// gather() 每次都重读 default.yaml / schema yaml / themes json，文件 IO 就
// 是那"一秒钟"的来源——它不该出现在点击路径上（2026-11-19 用户实报
// "即使每次创建菜单也用不了一秒"）。
void refreshMenuTreeCacheAsync(DaemonEngine* engineOverride) {
  if (g_menuTreeRefreshing.exchange(true)) return;  // 已有在跑
  std::thread([engineOverride]() {
    Json::Value built(Json::arrayValue);
    // **引擎未就绪（重启后部署中）时绝不进入本函数的任何持锁段**
    //（2026-11-21 用户实报"每次重启完，点击墨字，卡好多秒"）：
    // 下面的 g_dispatchLock 段里 eng->ensureEngine() 会原地等部署完成
    // （可达数十秒），持有分发锁期间所有宿主打字 RPC、所有菜单命令全部
    // 冻结——外发版绝不能有这个体感。部署完成后的下一次刷新（菜单命令/
    // 点击）自然补上缓存；在那之前菜单显示"（引擎未就绪）"，秒开不卡。
    if (!RimeRuntime::instance().ready()) {
      g_menuTreeRefreshing.store(false);
      return;
    }
    if (g_menu != nullptr) {
      DaemonEngine* eng = engineOverride;
      DaemonEngine fallback;
      // g_clients 的增删在分发线程，这里必须持分发锁读（2026-11-20 review
      // 抓出：原来先无锁挑引擎再进锁，map 并发插入/删除是 UB）。
      // 锁内 ensureEngine 的冷启动只发生在"还没有任何客户端"的启动预热
      // 路径（此时没有请求会被卡住）；有客户端时引擎已热，毫秒级。
      ::EnterCriticalSection(&g_dispatchLock);
      if (eng == nullptr) {
        if (g_clients.empty()) {
          // 还没有客户端：用临时引擎抓一份结构（勾选状态可能不准，但
          // 菜单项是齐的，足够让用户先看到菜单）。
          fallback.ensureEngine();
          fallback.onActivate();
          eng = &fallback;
        } else {
          auto it = g_clients.find(g_lastActiveClientId);
          eng = &(it != g_clients.end() ? it : g_clients.begin())
                     ->second.engine;
          eng->ensureEngine();
          eng->onActivate();
        }
      }
      built = Menu::buildMenu(g_menu->gather(*eng));
      ::LeaveCriticalSection(&g_dispatchLock);
      if (eng == &fallback) fallback.deactivate();
    }
    if (built.isArray() && !built.empty()) {
      {
        std::lock_guard<std::mutex> lock(g_menuTreeMutex);
        g_cachedMenuTree = built;
      }
      g_menuTreeReady.store(true);
    }
    g_menuTreeRefreshing.store(false);
  }).detach();
}
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

  // 维护"最近活跃客户端"（工具栏读状态的依据）。
  //
  // 之前只在 METHOD_ON_ACTIVATE 里更新，而用户按 Shift 走的是按键方法
  //（filterKeyDown/onKeyDown），那条路径从不更新它 —— 多客户端时它一直
  // 指向旧的那个引擎，工具栏读到的永远是旧状态。表现就是
  // "按了 Shift 打字和标点都变英文了，工具栏界面却看不到变化"
  //（2026-11-19 用户实报）。这里任何非失活请求都刷新它。
  if (method != "METHOD_ON_DEACTIVATE" && method != "METHOD_CLOSE") {
    const std::string reqCid = req.get("clientId", "").asString();
    if (!reqCid.empty()) {
      g_lastActiveClientId = reqCid;
    }
  }

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
    // 工具栏显隐的唯一驱动：只要有客户端激活就显示（用户切到了墨奇），
    // 全部失活才隐藏。同一 clientId 重复 onActivate 不重复计数——用 clientId
    // 集合而不是裸计数器，避免切焦点时计数虚高导致永不隐藏。
    const std::string cid = req.get("clientId", "").asString();
    if (!cid.empty()) {
      g_lastActiveClientId = cid;
      if (g_activeClientIds.insert(cid).second) {
        Moqi::setToolbarActiveCount(static_cast<int>(g_activeClientIds.size()));
      }
    }
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
    const std::string cidOff = req.get("clientId", "").asString();
    if (!cidOff.empty() && g_activeClientIds.erase(cidOff) > 0) {
      Moqi::setToolbarActiveCount(static_cast<int>(g_activeClientIds.size()));
    }
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

// Win7 没有语言栏，工具栏一旦关掉就再没有入口（用户会锁死在"无法打开菜单"
// 的状态）。所以"工具栏关 + Win7"时退回托盘图标，菜单从托盘右键出。
//
// 启动时和"运行时把工具栏关掉"两条路径共用本函数（2026-11-19 用户要求把
// 开关挂进外观菜单——之前只有启动分支处理，运行中关掉就再也打不开了）。
// startTrayIcon 自身幂等（g_running 原子交换守卫）。
void startTrayFallbackIfWin7() {
  if (Moqi::osIsWin8Plus()) return;  // Win8+ 有语言栏，不需要托盘
  Moqi::TrayCallbacks trayCb;
  trayCb.buildMenuTree = []() -> Json::Value {
    // 与工具栏同款：只拷缓存、后台异步刷新（2026-11-20）。旧实现直接
    // ensureEngine + gather——右键托盘要冷启动引擎（秒级），而且不持
    // 分发锁就遍历 g_clients 是数据竞争。
    Json::Value snapshot(Json::arrayValue);
    {
      std::lock_guard<std::mutex> lock(g_menuTreeMutex);
      snapshot = g_cachedMenuTree;
    }
    refreshMenuTreeCacheAsync(nullptr);
    return snapshot;
  };
  trayCb.executeCommand = [](int commandId) {
    Json::Value req(Json::objectValue);
    req["method"] = "METHOD_ON_COMMAND";
    req["commandId"] = commandId;
    req["seqNum"] = 0;
    Json::StreamWriterBuilder wb;
    wb["commentStyle"] = "None";
    wb["indentation"] = "";
    const std::string body = Json::writeString(wb, req);
    Moqi::MoqiServeDispatch(body, "daemon-tray", [](const std::string&) {});
  };
  Moqi::startTrayIcon(std::move(trayCb));
  daemonLog(L"[tray] enabled (toolbar disabled, Win7 fallback)");
}

// 把当前中英/全半角/标点状态推给所有已连接的宿主（2026-11-20 用户实报：
// "点击中字之后语言栏的中没有跟着变成英，只有点击进入编辑框的时候才变"）。
//
// 根因是架构性的：语言栏按钮活在各宿主进程的 DLL 里，daemon 直接 rimeSetOption
// 只改了自己那份会话——DLL 不主动问就永远不知道。Shift 是按键路径，DLL 自己
// 处理并立刻刷图标；工具栏点击是 daemon 侧动作，没有回包。
//
// 所以这里给每个客户端构造一个"只带按钮补丁"的异步帧，由 PipeServe 按
// clientId 路由到对应管道。DLL 侧看到 langStatusOnly 只刷语言栏，不碰组字/
// 候选（否则会把用户正在打的字清掉）。
void broadcastLangBarPatch(bool ascii, bool fullShape, bool asciiPunct) {
  if (g_serve == nullptr) return;
  // 补丁按"目标状态"构造，与任何客户端会话无关；发给**每一条活着的连接**。
  // 之前的做法是按 g_clients 逐个客户端构造（要读它的引擎状态），实测会
  // 因为"会话没建好/连接已过期"漏掉真正在打字的宿主（frames=1 但日志
  // 显示多个客户端在线）。
  Json::Value patch = DaemonEngine::makeLangStatusPatch(ascii, fullShape,
                                                       asciiPunct);
  if (!patch.isMember("changeButton")) return;
  patch["success"] = true;
  patch["seqNum"] = -1;  // 异步帧：DLL 侧 seq 对不上会走异步队列
  patch["langStatusOnly"] = true;
  int sent = 0;
  g_serve->forEachClient([&](const std::string& id) {
    Json::Value frame = patch;
    frame["clientId"] = id;
    std::string framed;
    if (Proto::serializeJsonMessage(frame, framed)) {
      g_serve->routeFrame(framed);
      ++sent;
    }
  });
  daemonLog(L"[toolbar] langbar patch broadcast to " + std::to_wstring(sent) +
            L" client(s) ascii=" + std::to_wstring(ascii ? 1 : 0));
}

void dispatchRequest(const string& payload, const string& clientId,
                     const std::function<void(const string&)>& reply) {
  DispatchLockGuard lockGuard(&g_dispatchLock);
  Json::Value req;
  if (!Proto::parseJsonPayload(payload, req) || !req.isObject()) {
    daemonLog(L"bad request frame");
    return;
  }
  // 连接级 id 注入解析后的对象（刀一：旧实现在 PipeServe 注入后整帧重
  // 序列化、这里再解析第二遍——每帧两次解析一次多余序列化，2026-11-08
  // 性能挖掘 daemon 89.9% JSON 的主要来源之一）。
  if (!clientId.empty()) {
    req["clientId"] = clientId;
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
        // 总候选数命令（190-193 与新增 150/151 档）：立即改本 daemon 所有
        // 引擎会话的 page size（Go applyCandidateCountConfig 的
        // SetCandidatePageSize 分支）。64 位应用的进程内引擎由 DLL 侧按
        // 响应里的 candidateCount 跟随。
        if (Menu::isCandidateCountCommand(cmdId)) {
          for (auto& kv : g_clients) {
            kv.second.engine.applyCandidateCount(st.candCount);
          }
        }
        // 开关类命令（中英/全半角/简繁/方案集内开关）必须随响应刷新语言栏
        // 图标——Go 的 toggleOption 响应带 changeButton；此前 daemon 不带，
        // 图标要等下一次按键才更新（用户实报 Notepad4 图标左键"无效"）。
        entry.engine.appendLangStatus(resp);
        // 同一个开关从某个宿主的菜单切换时，其它宿主的语言栏图标也要跟着变
        // （广播补丁；请求方自己已经拿到回包，重复应用是无害的幂等操作）。
        {
          // 广播要写管道：放在后台线程，绝不占着分发锁写（某个宿主不读
          // 管道就能把整个 daemon 的所有请求拖死）。
          const Json::Value st = resp["langStatus"];
          const bool a = st.get("ascii", false).asBool();
          const bool f = st.get("fullShape", false).asBool();
          const bool pt = st.get("asciiPunct", false).asBool();
          std::thread([a, f, pt]() {
            broadcastLangBarPatch(a, f, pt);
          }).detach();
        }
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
void MoqiServeDispatch(const std::string& payload, const std::string& clientId,
                       const std::function<void(const std::string&)>& reply) {
  try {
    dispatchRequest(payload, clientId, reply);
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

  // ---- 输入工具栏 / 托盘图标（2026-11-19）----
  //
  // 两者的定位差异（用户实测定下的口径）：
  //   工具栏 = "输入法被切到时显示"，与候选窗无关；常驻浮层，固定在屏幕
  //            右下角；做在 daemon 里保证单实例（DLL 在每个宿主进程各跑
  //            一份，做在 DLL 会叠出 N 个重叠窗口）。
  //   托盘   = 纯功能入口的兜底，只在没有工具栏时才有意义。
  // 二者互斥：工具栏开了就不挂托盘，避免同一台机器两个入口。
  //
  // 默认值：Win7 开工具栏（没有语言栏，没有别的入口）；Win8+ 关工具栏
  // 且不挂托盘（有语言栏就够）。用户都能在 appearance_config.json 的
  // "toolbar_enabled" 里覆盖——Win10 想用工具栏也一样能开。
  //
  // 版本检测用 RtlGetVersion 而不是 IsWindows8OrGreater：后者来自
  // VersionHelpers.h，在 _WIN32_WINNT=0x0601 编译目标下不会被定义。
  {
    const bool isWin8Plus = Moqi::osIsWin8Plus();

    bool toolbarWanted = !isWin8Plus;
    const std::wstring cfgRoot = moqiAppDataDir();
    if (!cfgRoot.empty()) {
      std::ifstream in(cfgRoot + L"\appearance_config.json", std::ios::binary);
      if (in) {
        Json::Value cfg;
        std::string errs;
        Json::CharReaderBuilder rb;
        if (Json::parseFromStream(rb, in, &cfg, &errs) && cfg.isObject() &&
            cfg["toolbar_enabled"].isBool()) {
          toolbarWanted = cfg["toolbar_enabled"].asBool();
        }
      }
    }

    // 不管 toolbarWanted 是什么都启动线程：配置可能随时改成"要工具栏"
    //（用户 2026-11-19 实报 Win10 改了配置不出工具栏——那时只在开关为真时
    // 才起线程，改配置自然无效）。线程空转只是 250ms 一次 stat + 读三个
    // option，代价可忽略；不可见时窗口不显示、不占焦点。
    {
      Moqi::ToolbarCallbacks tbCb;
      // 读三项开关当前值。
      //
      // 这里**绝对不能用 gather()**：gather 会重读 default.yaml、当前
      // schema yaml、themes json 和 appearance_config.json，是几百微秒到
      // 几毫秒的文件 IO；而本函数被工具栏定时器每 250ms 调一次，等于让
      // daemon 常年背着一份文件扫描，菜单弹出时抢锁抢 IO，用户感受到的就是
      // "点墨卡好几秒"（2026-11-19 实报两次）。
      //
      // 直接读三个 rime option（纯内存查表，微秒级）就够了。
      tbCb.readState = []() -> Moqi::ToolbarState {
        Moqi::ToolbarState st;
        // g_clients 在分发线程里会被插入/删除，后台轮询线程读它必须持锁
        //（2026-11-20：轮询已经移出界面线程，这里持分发锁只阻塞轮询线程，
        // 界面线程不再受影响）。锁序与 dispatchRequest 一致：分发锁 -> 引擎锁。
        ::EnterCriticalSection(&g_dispatchLock);
        struct Unlock {
          ~Unlock() { ::LeaveCriticalSection(&g_dispatchLock); }
        } unlockGuard;
        if (g_clients.empty()) return st;
        // 读"最近活跃"的客户端引擎：用户可能在 A 应用里打字而 g_clients
        // 的首元素是 B 应用（2026-11-19 实报"按 Shift 切英文，工具栏还显示
        // 中文"）。
        //
        // 但 g_lastActiveClientId 可能是**已经断开的**宿主：进程退出后
        // g_clients 里的条目和这个 id 都还在，工具栏就会读一个死引擎
        // （2026-11-19 自测实况：探针连接抢走 lastActive 后，点中英按钮
        // 界面完全没反应）。所以优先在当前激活集合里找。
        std::string pick;
        if (!g_lastActiveClientId.empty() &&
            g_activeClientIds.count(g_lastActiveClientId) > 0) {
          pick = g_lastActiveClientId;
        } else if (!g_activeClientIds.empty()) {
          pick = *g_activeClientIds.begin();
        }
        auto it = g_clients.find(pick);
        auto& engine = (it != g_clients.end() ? it : g_clients.begin())
                           ->second.engine;
        const auto sid = engine.sessionId();
        RimeRuntime& rt = RimeRuntime::instance();
        if (sid == 0 || rt.rimeGetOption == nullptr) {
          return st;
        }
        st.ascii = rt.rimeGetOption(sid, "ascii_mode") != 0;
        st.fullShape = rt.rimeGetOption(sid, "full_shape") != 0;
        st.asciiPunct = rt.rimeGetOption(sid, "ascii_punct") != 0;
        return st;
      };
      tbCb.buildMenuTree = []() -> Json::Value {
        // 点击路径只做两件事：拷一份缓存返回、投一个后台刷新。
        // 绝不在这里 gather（那是文件 IO，就是用户感受到的"卡"）。
        Json::Value snapshot(Json::arrayValue);
        {
          std::lock_guard<std::mutex> lock(g_menuTreeMutex);
          snapshot = g_cachedMenuTree;
        }
        // 不在点击路径里触发刷新：后台 gather 会抢 dispatchLock 和引擎锁，
        // 菜单弹出瞬间正好需要它们，等于自己给自己制造卡顿。缓存在
        // "命令执行后"和"启动预热"两处刷新，够新鲜了。
        return snapshot;
      };
      tbCb.executeCommand = [](int commandId) {
        // 四个开关命令**直接作用到所有活跃客户端引擎**（2026-11-19 实测修）：
        // 此前走 MoqiServeDispatch + 虚拟 clientId "daemon-tray"，只翻了那个
        // 虚拟客户端的引擎，用户真正在用的宿主引擎毫发无损——表现就是
        // "点了切英文，按键盘还是汉字"，以及"跟 Shift 不联动"。
        //
        // 语义对齐语言栏：切中英/全半角/标点都是"全局一次生效"，所有宿主
        // 窗口一起变。遍历 g_clients 逐个 setOption 就是这件事。
        const char* optionName = nullptr;
        if (commandId == 1) optionName = "ascii_mode";
        else if (commandId == 3) optionName = "full_shape";
        else if (commandId == 4) optionName = "ascii_punct";

        if (optionName != nullptr) {
          // **必须放后台线程**（2026-11-20 用户实报三连："鼠标滑上去变等待
          // 光标""过了好多秒之后才可以点击""点一下弹无响应"）。下面的
          // ensureEngine/onActivate 会建 rime 会话、加载词典，秒级；跑在
          // 工具栏界面线程上就等于把窗口消息循环卡死，系统判定"程序未响应"。
          std::string name(optionName);
          // 每条命令一个短命线程：executeLang 会连着发两条（中英 + 标点），
          // 用"忙就丢弃"的闸门会把第二条吞掉（2026-11-20 实测 busy=1 丢帧）。
          // 线程们在 g_dispatchLock 上自然串行，代价可忽略。
          {
            std::thread([name]() {
              RimeRuntime& rt = RimeRuntime::instance();
              // **引擎未就绪（重启后部署中）时直接放弃**（2026-11-21 用户
              // 实报"重启完点墨卡几十秒"的同族冻结源）：下面的锁内段含
              // ensureEngine，部署期会原地等最多 30 秒并全程持有分发锁——
              // 所有宿主打字冻结。部署完成后下一个按键/点击自然生效。
              if (!rt.ready()) {
                daemonLog(L"[toolbar] toggle dropped (engine not ready)");
                return;
              }
              // 锁序：分发锁 -> 引擎锁（与 dispatchRequest 一致，避免循环等待）。
              ::EnterCriticalSection(&g_dispatchLock);
              {
                std::lock_guard<std::recursive_mutex> lock(rt.mutex());
                // 第一步：只把**已存在会话**的客户端的会话补齐；没有会话的
                // 客户端跳过（它的首次激活会从共享状态拿到当前值）。
                // 用 onActivate 会重载引擎，几十毫秒到几秒不等，不该每个
                // 客户端都做一遍。
                // **只给"最近活跃"的一个客户端建会话**（2026-11-20 用户
                // Win7/Win10 双机实报"点击后非常长的等待"）：ensureEngine
                // 会加载词典（秒级），而这里全程持有分发锁——多给一个没
                // 会话的客户端建会话，所有宿主的按键就多冻结几秒。explorer/
                // firefox/conhost 这些连过管道的进程都在客户端表里，群发
                // ensureEngine 等于每次点击都做 N 次冷启动。
                auto act = g_clients.find(g_lastActiveClientId);
                if (act != g_clients.end() && g_serve != nullptr &&
                    g_serve->hasClient(act->first)) {
                  act->second.engine.ensureEngine();
                }
                // 第二步：以"最近活跃客户端"的当前值为基准算目标值，再统一
                // 设到所有会话。
                const char* optionName = name.c_str();
                bool target = false;
                bool haveBase = false;
                auto base = g_clients.find(g_lastActiveClientId);
                if (base != g_clients.end() &&
                    g_serve != nullptr && g_serve->hasClient(base->first) &&
                    base->second.engine.sessionId() != 0 &&
                    rt.rimeGetOption != nullptr) {
                  target = rt.rimeGetOption(base->second.engine.sessionId(),
                                            optionName) == 0;
                  haveBase = true;
                }
                if (!haveBase && rt.rimeGetOption != nullptr) {
                  for (auto& kv : g_clients) {
                    if (g_serve == nullptr || !g_serve->hasClient(kv.first)) continue;
                    const auto sid = kv.second.engine.sessionId();
                    if (sid == 0) continue;
                    target = rt.rimeGetOption(sid, optionName) == 0;
                    haveBase = true;
                    break;
                  }
                }
                if (!haveBase) target = true;  // 全都没有会话：默认切到"开"

                if (rt.rimeSetOption != nullptr) {
                  for (auto& kv : g_clients) {
                    if (g_serve == nullptr || !g_serve->hasClient(kv.first)) continue;
                    const auto sid = kv.second.engine.sessionId();
                    if (sid == 0) continue;
                    rt.rimeSetOption(sid, optionName, target ? 1 : 0);
                  }
                  // 工具栏快照立即更新（不等下一帧回包）：点完立刻看到字变。
                  for (auto& kv : g_clients) {
                    if (g_serve == nullptr || !g_serve->hasClient(kv.first)) continue;
                    const auto sid = kv.second.engine.sessionId();
                    if (sid == 0 || rt.rimeGetOption == nullptr) continue;
                    Moqi::updateToolbarState(
                        rt.rimeGetOption(sid, "ascii_mode") != 0,
                        rt.rimeGetOption(sid, "full_shape") != 0,
                        rt.rimeGetOption(sid, "ascii_punct") != 0);
                    break;
                  }
                }
              }
              ::LeaveCriticalSection(&g_dispatchLock);
              daemonLog(std::wstring(L"[toolbar] toggled ") +
                        utf8ToWide(name.c_str()));
              // 通知所有宿主刷新语言栏图标：按钮在各宿主 DLL 里，daemon
              // 改选项它不会自己知道（用户实报"点了中字语言栏不变"）。
              {
                bool a = false, f = false, p = false;
                {
                  std::lock_guard<std::recursive_mutex> lock(rt.mutex());
                  for (auto& kv : g_clients) {
                    if (g_serve == nullptr || !g_serve->hasClient(kv.first)) continue;
                    const auto sid = kv.second.engine.sessionId();
                    if (sid == 0 || rt.rimeGetOption == nullptr) continue;
                    a = rt.rimeGetOption(sid, "ascii_mode") != 0;
                    f = rt.rimeGetOption(sid, "full_shape") != 0;
                    p = rt.rimeGetOption(sid, "ascii_punct") != 0;
                    break;
                  }
                }
                broadcastLangBarPatch(a, f, p);
              }
              Moqi::pokeToolbarState();
            }).detach();
          }
        } else {
          // 菜单里的其他命令（方案/主题/打开文件/维护等）仍走标准分发路径。
          // **必须丢后台线程**（2026-11-21）：本回调在工具栏 UI 线程里被
          // showFunctionMenu 同步调用，MoqiServeDispatch 要拿 g_dispatchLock
          // ——部署/引擎操作持锁期间（可达数十秒），UI 线程同步等锁=工具栏
          // "程序未响应"。后台执行，锁竞争自然串行。
          const int cmd = commandId;
          std::thread([cmd]() {
            Json::Value req(Json::objectValue);
            req["method"] = "METHOD_ON_COMMAND";
            req["commandId"] = cmd;
            req["seqNum"] = 0;
            Json::StreamWriterBuilder wb;
            wb["commentStyle"] = "None";
            wb["indentation"] = "";
            const std::string body = Json::writeString(wb, req);
            Moqi::MoqiServeDispatch(body, "daemon-tray", [](const std::string&) {});
          }).detach();
        }
        Moqi::pokeToolbarState();
        // 命令改动了状态（开关/方案/外观），缓存立即过期 -> 后台刷新一次，
        // 用户下次点"墨"看到的勾选就是新的。不在这里同步 gather（会卡）。
        refreshMenuTreeCacheAsync(nullptr);
      };
      // 工具栏开关的运行时联动（互斥规则在运行时也成立）：
      //   开 → 撤托盘（同一台机器不要两个入口）；
      //   关 → Win7 退回托盘，否则用户关掉工具栏后再没有菜单入口。
      tbCb.onEnabledChanged = [](bool enabled) {
        if (enabled) {
          Moqi::stopTrayIcon();
          daemonLog(L"[toolbar] tray icon removed (toolbar enabled at runtime)");
        } else {
          startTrayFallbackIfWin7();
        }
      };
      // 显隐权威判定：前台窗口所属进程是否由我们服务。
      // 切到别的输入法 / 别的应用后前台窗口换人，工具栏立即隐藏。
      tbCb.isForegroundServed = []() -> bool {
        if (g_serve == nullptr) return true;  // stdin 模式无管道，不判断
        HWND fg = ::GetForegroundWindow();
        if (fg == nullptr) return false;
        DWORD pid = 0;
        ::GetWindowThreadProcessId(fg, &pid);
        if (pid == 0) return false;
        // 前台是本进程窗口（工具栏/托盘/弹出菜单）时，**沿用上一个真实
        // 前台的判定**（2026-11-21 用户实报"点桌面后点墨字，菜单弹出即
        // 消失"）：
        //
        // 旧实现这里一律返回 false，配合 showFunctionMenu 里
        // SetForegroundWindow(owner)，250ms 显隐定时器会在菜单还开着时
        // 把工具栏 ShowWindow(SW_HIDE)——拥有者窗口一隐藏，TrackPopupMenu
        // 立即被系统取消，菜单就"弹出来立马消失"。记事本在前台时不复现，
        // 因为 SetForegroundWindow 抢不过它，前台仍是宿主。
        //
        // 缓存最近一次"非自身前台"的判定：点工具栏期间保持显示；真正切到
        // 别的应用/输入法时前台换人（不是本进程），缓存随之刷新→正常隐藏。
        static std::atomic<int> lastServed{-1};  // -1=未知，0=否，1=是
        if (pid == ::GetCurrentProcessId()) {
          const int cached = lastServed.load(std::memory_order_relaxed);
          // 未知时保守显示：工具栏此刻就在屏幕上、用户正在点它，
          // 隐藏只会造成"菜单闪一下就没了"。
          return cached < 0 ? true : cached == 1;
        }
        const bool served = g_serve->isProcessIdServed(pid);
        lastServed.store(served ? 1 : 0, std::memory_order_relaxed);
        return served;
      };
      Moqi::startToolbar(std::move(tbCb));
      Moqi::setToolbarEnabled(toolbarWanted);
      daemonLog(std::wstring(L"[toolbar] thread started, enabled=") +
                (toolbarWanted ? L"1" : L"0"));
    }
    // 工具栏在跑就不挂托盘（互斥）；工具栏没启用且是 Win7 才退回托盘。
    if (!toolbarWanted) {
      startTrayFallbackIfWin7();
    }
  }

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
    // 连接断开 -> 清理活跃集合（工具栏失活的兜底信号）。
    // DLL 在 Deactivate 时会发 onDeactivate 并断管，但进程退出/崩溃时那条
    // 通知发不出来，clientId 就永久残留 —— 表现是"切到其他输入法后工具栏
    // 不消失"（2026-11-19 用户实报）。连接断开是必然发生的，用它兜底。
    serve.setClientGoneCallback([](const std::string& goneId) {
      if (g_activeClientIds.erase(goneId) > 0) {
        daemonLog(L"[toolbar] client gone, active=" +
                  std::to_wstring(g_activeClientIds.size()));
        Moqi::setToolbarActiveCount(
            static_cast<int>(g_activeClientIds.size()));
      }
    });
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
      // 预热的同时抓一份菜单树缓存：用户第一次点工具栏"墨"字时直接用，
      // 不用再等 ensureEngine（2026-11-19 实报"第一次点击要卡好几秒"）。
      // 这个引擎只是临时实例，用完即弃；菜单里的勾选状态以点击时的实时
      // gather 为准，缓存只顶第一次。
      // 预热期抓第一份菜单缓存：用户第一次点"墨"时缓存已经在了。
      refreshMenuTreeCacheAsync(&warm);
      // 等它落位（最多 5 秒），保证首次点击就有菜单可显示。
      for (int i = 0; i < 50 && !g_menuTreeReady.load(); ++i) {
        ::Sleep(100);
      }
      daemonLog(L"[serve] menu tree cached ready=" +
                std::to_wstring(g_menuTreeReady.load() ? 1 : 0));
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
    Moqi::stopToolbar();
    Moqi::stopTrayIcon();
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
      dispatchRequest(payload, "", &writeFramedToStdout);
    }
  }
  daemonLog(L"moqi-daemon exit");
  Moqi::stopToolbar();
  Moqi::stopTrayIcon();
  Moqi::stopClipboardMonitor();
  Moqi::setGlobalCloudSync(nullptr);
  return 0;
}
