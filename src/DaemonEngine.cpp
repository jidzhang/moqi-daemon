//
//	DaemonEngine.cpp: per-client session logic ported verbatim from
//	LocalEngine.cpp (phase 5) with the TSF coupling removed.
//
#include "DaemonEngine.h"

#include "DaemonCloudConfig.h"
#include "DaemonWebDAV.h"
#include "DigitPassthrough.h"

#include <atomic>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <thread>

namespace Moqi {
// main.cpp 共用的轻量日志（临时探针用；见 processKey 内 [remap] 标记）。
void daemonLogExtern(const std::wstring& msg);
void daemonLogDebugExtern(const std::wstring& msg);
}

using namespace std;

namespace Moqi {

// main.cpp 注入的安装布局（engine init 需要）。
std::wstring g_daemonProgramDir;
std::wstring g_daemonBackendDir;

void DaemonEngine::ensureEngine() {
  RimeRuntime& runtime = RimeRuntime::instance();
  if (!runtime.supported() || runtime.ready()) {
    return;
  }
  // Failed 也必须能重试：RimeRuntime::requestInit 会把 Failed 复位成 Idle。
  // 旧代码在这里直接 return，失败一次就永久直落英文（只能重启进程）。
  // 5 秒退避避免每个按键都重走 30 秒初始化。
  const ULONGLONG now = ::GetTickCount64();
  if (runtime.failed() && now - lastInitAttempt_ < 5000) {
    return;
  }
  lastInitAttempt_ = now;
  // rime.dll 用 daemon 自己的目录（x86 版，放安装根；moqi-ime\...\rime.dll
  // 是 x64，归 Go 后端/x64 TSF DLL）。MOQI_RIME_DLL_DIR 可覆盖（测试用）。
  const wchar_t* dllDirEnv = _wgetenv(L"MOQI_RIME_DLL_DIR");
  const std::wstring dllDir =
      dllDirEnv && *dllDirEnv ? std::wstring(dllDirEnv) : g_daemonProgramDir;
  // 2026-11-04 单引擎架构：daemon 直接使用标准 Rime 用户目录
  // %APPDATA%\Moqi\<方案集>（默认 Rime）。雾凇等整包按官方方式解压到
  // 这里即可，用户词典/学习/部署全部落在标准位置。
  // MOQI_USER_DATA_DIR：单测把用户目录重定向到临时目录——真用户词典绝不
  // 让测试进程碰（rime 的 LevelDB userdb 非并发安全；2026-11-05 实证过
  // 测试进程对活用户词典跑过 RepairDB）。生产不设此变量，行为不变。
  const wchar_t* userDataDirEnv = _wgetenv(L"MOQI_USER_DATA_DIR");
  const std::wstring masterDir =
      userDataDirEnv && *userDataDirEnv ? std::wstring(userDataDirEnv)
                                        : sharedUserConfigDir();
  runtime.requestInit(g_daemonProgramDir, g_daemonBackendDir, dllDir, masterDir);
  const ULONGLONG deadline = ::GetTickCount64() + 30000;
  while (!runtime.ready() && !runtime.failed() && ::GetTickCount64() < deadline) {
    ::Sleep(50);
  }
}

bool DaemonEngine::engineReady() const {
  return RimeRuntime::instance().ready();
}


DaemonEngine::DaemonEngine() {}
DaemonEngine::~DaemonEngine() {
  if (aiWorker_.joinable()) {
    aiWorker_.join();
  }
  // 列表加载线程捕获 this；析构前必须 join，避免 g_clients.clear() 后
  // 后台线程写已释放的 cloudClip_（UAF）。线程自身有 8 秒总超时。
  if (cloudClipWorker_.joinable()) {
    cloudClipWorker_.join();
  }
}

// ---------------------------------------------------------------- AI ------

void DaemonEngine::resetAIState() {
  std::lock_guard<std::mutex> lock(aiResultLock_);
  // Go resetAIState 会 ++aiRequestSeq：退出覆盖层/被 Esc 取消后，在途结果
  // 必须作废。只清 active 不递增 seq，几秒后旧结果仍会把覆盖层"复活"。
  ++aiState_.requestSeq;
  aiState_.pending.store(false);
  aiState_.active = false;
  aiState_.candidates.clear();
  aiState_.cursor = 0;
  aiState_.prompt.clear();
  aiState_.error.clear();
}

// 热键触发：读配置 → 匹配 action → 后台 HTTP（异步，绝不阻塞打字）。
// 返回 true = 键被消费；结果经 push 回推（DLL 候选窗 WM_TIMER 轮询取回）。
bool DaemonEngine::maybeTriggerAI(const KeyEventData &e,
                                  std::function<void(Json::Value)> push,
                                  const Json::Value *localState) {
  // 配置节流：每键重读 ai_config.json 会给按键加文件 IO 延迟（实测体感
  // 会变钝）。2 秒内复用上次结果；用户改文件后最多 2 秒生效。
  const ULONGLONG now = ::GetTickCount64();
  if (now - aiConfigLoadedTick_ >= 2000) {
    aiConfigLoadedTick_ = now;
    aiEnabledCached_ = aiState_.client.load();
  }
  if (!aiEnabledCached_) {
    return false;
  }
  // 修饰键位来自 keyStates（VK_CONTROL/VK_SHIFT/VK_MENU）。
  const BYTE *states = e.keyStates;
  const bool ctrl = states && ((states[VK_CONTROL] & 0x80) != 0);
  const bool alt = states && ((states[VK_MENU] & 0x80) != 0);
  const bool shift = states && ((states[VK_SHIFT] & 0x80) != 0);
  const AIAction *action = aiState_.client.match(e.keyCode, ctrl, alt, shift);
  if (action == nullptr) {
    return false;
  }
  if (aiState_.pending.load()) {
    return true; // 在途去重（与 Go 一致：连按不重复计费）
  }

  // 组字/候选来源：DLL 进程内引擎随请求携带（64 位），否则读本引擎会话。
  const bool remoteOverlay = localState != nullptr;
  std::string composition;
  std::vector<std::string> inputCandidates;
  if (remoteOverlay) {
    composition = localState->get("localComposition", "").asString();
    const Json::Value &cands = (*localState)["localCandidates"];
    for (Json::ArrayIndex i = 0; cands.isArray() && i < cands.size() &&
                                 inputCandidates.size() < 3; ++i) {
      inputCandidates.push_back(cands[i].asString());
    }
  } else {
    bool engineOk = false;
    const EngineState state = currentState(&engineOk);
    composition = state.composition;
    for (const auto &cand : state.candidates) {
      if (inputCandidates.size() >= 3) break;
      inputCandidates.push_back(cand.first);
    }
  }
  if (composition.empty() && inputCandidates.empty()) {
    return false; // 无组字也无候选：不消费（键交还宿主）
  }

  const AIAction actionCopy = *action;
  const std::string previousCommit = aiState_.previousCommit;
  const long long seq = ++aiState_.requestSeq;
  aiState_.pending.store(true);
  {
    std::lock_guard<std::mutex> lock(aiResultLock_);
    aiState_.prompt = composition;
    aiState_.candidates.clear();
    aiState_.cursor = 0;
    aiState_.error.clear();
    aiState_.active = false;
  }
  if (aiWorker_.joinable()) {
    aiWorker_.join();
  }
  // 异步（与 Go 一致）：慢网络绝不阻塞打字；结果经 push 回推，DLL 由候选窗
  // 的 WM_TIMER 轮询取回并渲染（见 MoqiCandidateWindow::wndProc）。
  aiWorker_ = std::thread([this, actionCopy, composition, inputCandidates,
                           previousCommit, seq, push, remoteOverlay]() {
    std::vector<std::string> result;
    std::string err;
    bool ok = false;
    try {
      ok = aiState_.client.generate(actionCopy, composition, inputCandidates,
                                    previousCommit, &result, &err);
    } catch (const std::exception& e) {
      err = e.what();
    } catch (...) {
      err = "unknown ai exception";
    }
    Json::Value update;
    {
      std::lock_guard<std::mutex> lock(aiResultLock_);
      if (seq != aiState_.requestSeq.load()) {
        aiState_.pending.store(false);
        return; // 过期结果丢弃（Go 的 seq 检查）
      }
      if (!ok) {
        aiState_.pending.store(false);
        aiState_.error = err;
        aiState_.active = false;
        daemonLogExtern(L"[ai] generate failed: " +
                        std::wstring(err.begin(), err.end()));
        return;
      }
      if (result.empty()) {
        aiState_.pending.store(false);
        return;
      }
      aiState_.candidates = std::move(result);
      aiState_.cursor = 0;
      // 远端覆盖层：DLL 自己叠加显示与选字，本会话不进入 AI 态
      //（否则下次转发的热键会被当成覆盖期间按键误处理）。
      aiState_.active = !remoteOverlay;
      update = Json::Value(Json::objectValue);
      update["success"] = true;
      update["seqNum"] = 0;
      update["returnValue"] = 1;
      update["aiOverlay"] = true;
      Json::Value entries(Json::arrayValue);
      Json::Value item(Json::objectValue);
      item["text"] = aiState_.candidates[0];
      entries.append(item);
      update["candidateEntries"] = entries;
      update["candidateList"] = Json::Value(Json::arrayValue);
      update["showCandidates"] = true;
      update["candidateCursor"] = 0;
      update["compositionString"] = aiState_.prompt;
      update["compositionCursor"] =
          static_cast<Json::Int>(aiState_.prompt.size());
    }
    if (push) {
      daemonLogExtern(L"[ai] pushing candidate update n=" +
                      std::to_wstring(update["candidateEntries"].size()));
      push(update);
    }
    // pending 必须覆盖整个推送过程：推送阻塞时，下一次热键看到 pending
    // 直接去重返回，不会 join 到阻塞的 worker（旧顺序会让按键线程卡在
    // 管道写上）。
    aiState_.pending.store(false);
  });
  return true;
}

// 选中 AI 覆盖层第 index 个候选并上屏（清空引擎组字）。
bool DaemonEngine::commitAICandidate(int index, Json::Value &resp) {
  std::lock_guard<std::mutex> lock(aiResultLock_);
  if (!aiState_.active || index < 0 ||
      index >= static_cast<int>(aiState_.candidates.size())) {
    return false;
  }
  const std::string commit = aiState_.candidates[index];
  resp["success"] = true;
  resp["returnValue"] = 1;
  resp["commitString"] = commit;
  resp["compositionString"] = "";
  resp["candidateList"] = Json::Value(Json::arrayValue);
  resp["candidateEntries"] = Json::Value(Json::arrayValue);
  resp["showCandidates"] = false;
  aiState_.active = false;
  aiState_.candidates.clear();
  RimeRuntime &runtime = RimeRuntime::instance();
  if (sessionId_ != 0 && runtime.rimeClearComposition != nullptr) {
    runtime.rimeClearComposition(sessionId_);
  }
  aiState_.previousCommit = commit;
  return true;
}

void DaemonEngine::deactivate() {
  // 列表激活状态必须随会话一起清：否则切走再回来，DLL 还停在 RPC 转发
  // 模式而 daemon 侧已经 reset，按键语义两边错位。
  resetCloudClipState();
  if (sessionId_ == 0) {
    return;
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  std::lock_guard<std::recursive_mutex> lock(runtime.mutex());
  runtime.rimeDestroySession(sessionId_);
  sessionId_ = 0;
  secondSelectConsumeKeyUpCode_ = 0;
  keyComposing_ = false;
}

bool DaemonEngine::ensureSession() {
  RimeRuntime& runtime = RimeRuntime::instance();
  if (sessionId_ != 0) {
    // RimeSyncUserData 内部会 CleanupAllSessions；若调用方忘了 deactivate，
    // 死句柄会让后续每个键静默失效。Go 每次 ensure 都 FindSession。
    if (runtime.rimeFindSession == nullptr ||
        runtime.rimeFindSession(sessionId_) != 0) {
      return true;
    }
    sessionId_ = 0;
  }
  sessionId_ = runtime.rimeCreateSession();
  if (sessionId_ != 0) {
    // Go createSession()：新会话套用输入状态共享（未启用时为空操作）。
    applySharedInputState(sessionId_);
  }
  return sessionId_ != 0;
}

// Sets menu/page_size to the configured candidate count (backend
// createSession(): SetCandidatePageSize). 9 也要显式写：从 5/7 改回 9 时
// 若跳过，schema 里残留的旧值会让 9 永远不生效。
bool DaemonEngine::refreshPageSize() {
  if (sessionId_ == 0) {
    return true;
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  char schemaId[256] = {};
  if (!runtime.rimeGetCurrentSchema(sessionId_, schemaId, sizeof(schemaId))) {
    return false;
  }
  // 刚创建的会话停在 ".default"：优先用保存的方案，但必须确认它还在当前
  // 方案列表里——用户换成雾凇等整包后旧的 rime_frost 已不存在，此时回退到
  // default.yaml 的首个方案（整包兼容）。
  std::string target;
  if (schemaId[0] != '\0' && schemaId[0] != '.') {
    target = schemaId;
  } else {
    const std::string saved = loadSyncedSchemaId();
    std::string first;
    bool savedFound = false;
    if (runtime.rimeGetSchemaList != nullptr &&
        runtime.rimeFreeSchemaList != nullptr) {
      rimeapi::RimeSchemaList list = {};
      if (runtime.rimeGetSchemaList(&list)) {
        for (size_t i = 0; i < list.size; ++i) {
          const char* id = list.list[i].schema_id;
          if (id == nullptr || id[0] == '\0') continue;
          if (first.empty()) first = id;
          if (!saved.empty() && saved == id) savedFound = true;
        }
        runtime.rimeFreeSchemaList(&list);
      }
    }
    target = savedFound ? saved : first;
    if (target.empty()) target = saved.empty() ? "rime_frost" : saved;
  }
  if (runtime.rimeSelectSchema != nullptr) {
    runtime.rimeSelectSchema(sessionId_, target.c_str());
  }
  rimeapi::Config config = {};
  if (!runtime.rimeSchemaOpen(target.c_str(), &config)) {
    return false;
  }
  const bool ok =
      runtime.rimeConfigSetInt(&config, "menu/page_size", candidateCount_) != 0;
  runtime.rimeConfigClose(&config);
  // Schema 在构造时缓存 page_size；改完 config 必须重建 Schema 才生效。
  if (ok && runtime.rimeSelectSchema != nullptr) {
    runtime.rimeSelectSchema(sessionId_, target.c_str());
    // ApplySchema 清瞬态选项，重放共享输入状态。
    applySharedInputState(sessionId_);
  }
  return ok;
}

// 菜单改"总候选数"后的即时生效（Go applyCandidateCountConfig）：更新本引擎
// 计数并写 page size，正在输入的会话下一个组字即按新页长出候选。
void DaemonEngine::applyCandidateCount(int count) {
  if (count <= 0 || count == candidateCount_) {
    return;
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  std::lock_guard<std::recursive_mutex> lock(runtime.mutex());
  candidateCount_ = count;
  // refreshPageSize 会 rimeSelectSchema 重建 Schema，librime 的 ApplySchema
  // 会清空 context。别的应用正在组字时不要动它；新值会在下次会话重建/
  // 组字结束后生效。
  if (sessionId_ != 0) {
    bool ok = false;
    const EngineState state = currentState(&ok);
    if (ok && !state.composition.empty()) return;
  }
  refreshPageSize();
}

DaemonEngine::EngineState DaemonEngine::currentState(bool* engineOk) {
  RimeRuntime& runtime = RimeRuntime::instance();
  EngineState state;
  *engineOk = sessionId_ != 0 && runtime.rimeFindSession(sessionId_) != 0;
  if (!*engineOk) {
    return state;
  }
  rimeapi::Commit commit = {};
  commit.data_size =
      static_cast<int>(sizeof(rimeapi::Commit) - sizeof(commit.data_size));
  if (runtime.rimeGetCommit(sessionId_, &commit)) {
    state.commit = commit.text != nullptr ? commit.text : "";
    runtime.rimeFreeCommit(&commit);
  }
  rimeapi::Context context = {};
  context.data_size = static_cast<int>(sizeof(rimeapi::Context) -
                                       sizeof(context.data_size));
  if (runtime.rimeGetContext(sessionId_, &context)) {
    state.composition = context.composition.preedit != nullptr
                            ? context.composition.preedit
                            : "";
    state.cursorPos = context.composition.cursor_pos;
    state.selStart = context.composition.sel_start;
    state.selEnd = context.composition.sel_end;
    state.highlightedIndex = context.menu.highlighted_candidate_index;
    state.pageNo = context.menu.page_no;
    state.selectKeys =
        context.menu.select_keys != nullptr ? context.menu.select_keys : "";
    const int count = context.menu.num_candidates;
    if (count > 0 && context.menu.candidates != nullptr) {
      state.candidates.reserve(count);
      for (int i = 0; i < count; ++i) {
        const rimeapi::Candidate& candidate = context.menu.candidates[i];
        state.candidates.emplace_back(
            candidate.text != nullptr ? candidate.text : "",
            candidate.comment != nullptr ? candidate.comment : "");
      }
    }
    runtime.rimeFreeContext(&context);
  }
  state.asciiMode = runtime.rimeGetOption(sessionId_, "ascii_mode") != 0;
  state.fullShape = runtime.rimeGetOption(sessionId_, "full_shape") != 0;
  return state;
}

// Port of snapshotRawInputForCommit: 提交类按键前保存 raw 快照（供
// typo_memory 记录；空格提交时跟踪值即将清零）。
void DaemonEngine::snapshotRawInputForCommit() {
  if (!rawInputTracked_.empty()) {
    rawInputAtCommit_ = rawInputTracked_;
  }
  rawInputTracked_.clear();
}

// Port of updateTrackedRawInput：跟踪用户实际敲的字母串（a-z 与 '），
// 供错字记忆在提交时配对；keyCode/charCode 是重排换算后的有效键值。
void DaemonEngine::updateTrackedRawInput(const KeyEventData& e, UINT keyCode,
                                        UINT charCode, bool backendRet) {
  if (!backendRet) {
    return;
  }
  UINT ch = charCode;
  if (ch == 0 && keyCode >= 'A' && keyCode <= 'Z') {
    ch = keyCode + 32;
  }
  switch (keyCode) {
  case vkBack:
    if (!rawInputTracked_.empty()) {
      rawInputTracked_.pop_back(); // 跟踪串只含 ASCII
    }
    return;
  case vkSpace:
  case vkReturn:
    snapshotRawInputForCommit();
    return;
  case vkEscape:
    rawInputAtCommit_.clear();
    rawInputTracked_.clear();
    return;
  default:
    break;
  }
  const bool digit =
      (keyCode >= '1' && keyCode <= '9') || (ch >= '1' && ch <= '9');
  if (digit || (semicolonSelectSecond_ && isSecondSelectionShortcut(e))) {
    snapshotRawInputForCommit();
    return;
  }
  if (ch >= 'a' && ch <= 'z') {
    rawInputTracked_.push_back(static_cast<char>(ch));
    return;
  }
  if (ch == '\'' && !rawInputTracked_.empty() &&
      rawInputTracked_.back() != '\'') {
    rawInputTracked_.push_back('\'');
    return;
  }
  if (!rawInputTracked_.empty() && ch >= 0x20 && ch != '\'') {
    rawInputTracked_.clear();
  }
}

// Port of processKey(): translate, send to the engine, then the backend's
// special cases (arrow fallback, Return/Shift/CapsLock handling).
bool DaemonEngine::processKey(const KeyEventData& e, bool isUp, bool* ret) {
  RimeRuntime& runtime = RimeRuntime::instance();
  if (!ensureSession()) {
    *ret = false;
    return false;
  }
  if (!isUp) {
    bool engineOk = false;
    const EngineState state = currentState(&engineOk);
    keyComposing_ =
        engineOk && (!state.composition.empty() || !state.candidates.empty());
  }

  const bool isArrow = !isUp && (e.keyCode == vkUp || e.keyCode == vkDown);
  EngineState beforeState;
  if (isArrow) {
    bool engineOk = false;
    beforeState = currentState(&engineOk);
  }

  const int keycode0 = translateKeyCode(e);
  const int mask = translateModifiers(e, isUp);
  int keycode = keycode0;
  // 重排后的索引换算：显示序与引擎序不一致时，空格/数字指向显示位次，
  // 必须换算成引擎索引再发给引擎（否则按 2 选错词——历史实报教训）。
  UINT effKeyCode = e.keyCode;
  UINT effCharCode = e.charCode;
  if (!isUp && mask == 0 && keyComposing_) {
    daemonLogDebugExtern(L"[remap] composing key=" + to_wstring(e.keyCode) +
                         L" permSize=" + to_wstring(displayPerm_.size()));
    int target = -1;
    if (effKeyCode == vkSpace) {
      target = 0;
    } else if (effKeyCode >= '1' && effKeyCode <= '9') {
      target = static_cast<int>(effKeyCode) - '1';
    }
    if (target >= 0 && overlayCustomCount_ > 0) {
      target -= overlayCustomCount_;
    }
    if (target >= 0 && overlayCustomCount_ > 0 && displayPerm_.empty()) {
      effKeyCode = static_cast<UINT>('1') + target;
      effCharCode = effKeyCode;
      keycode = static_cast<int>(effCharCode);
    } else if (target >= 0 &&
               target < static_cast<int>(displayPerm_.size())) {
      const int engineIdx = displayPerm_[target];
      daemonLogDebugExtern(L"[remap] target=" + to_wstring(target) +
                           L" engineIdx=" + to_wstring(engineIdx));
      if (engineIdx != target && engineIdx >= 0 &&
          engineIdx < candidateCount_) {
        effKeyCode = static_cast<UINT>('1') + engineIdx;
        effCharCode = effKeyCode;
        keycode = static_cast<int>(effCharCode); // digit keysym = ASCII
      }
    }
  }
  bool backendRet = runtime.rimeProcessKey(sessionId_, keycode, mask) != 0;

  if (isArrow && mask == 0 && !beforeState.candidates.empty()) {
    // Arrow navigation fallback when the engine ignored Up/Down: move the
    // highlight ourselves (backend applyArrowNavigationFallback).
    bool engineOk = false;
    const EngineState afterState = currentState(&engineOk);
    if (!afterState.candidates.empty() &&
        afterState.highlightedIndex == beforeState.highlightedIndex) {
      int target =
          beforeState.highlightedIndex < 0 ? 0 : beforeState.highlightedIndex;
      if (e.keyCode == vkUp && target > 0) {
        --target;
      } else if (e.keyCode == vkDown &&
                 target < static_cast<int>(afterState.candidates.size()) - 1) {
        ++target;
      }
      if (target != afterState.highlightedIndex &&
          runtime.rimeHighlightCandidateOnCurrentPage != nullptr &&
          runtime.rimeHighlightCandidateOnCurrentPage(sessionId_, target)) {
        backendRet = true;
      }
    }
  }

  if (!isUp) {
    updateTrackedRawInput(e, effKeyCode, effCharCode, backendRet);
  }

  // 输入状态共享：Shift 抬起 / Caps 按下后捕获四选项变化并落盘
  //（Go shouldSyncSharedInputStateAfterProcessKey；未启用时只付一次 stat）。
  syncSharedAfterKey(sessionId_, e.keyCode, isUp);

  *ret = backendRet;
  if (!backendRet) {
    if (keyComposing_ && e.keyCode == vkReturn) {
      *ret = true;
    } else if ((e.keyCode == vkShift || e.keyCode == vkCapital) &&
               (mask == 0 || mask == kReleaseMask)) {
      *ret = true;
    }
  }
  return true;
}

bool DaemonEngine::readInputModes(bool* asciiMode, bool* fullShape) {
  RimeRuntime& runtime = RimeRuntime::instance();
  if (sessionId_ == 0 || !runtime.rimeFindSession(sessionId_)) {
    return false;
  }
  *asciiMode = runtime.rimeGetOption(sessionId_, "ascii_mode") != 0;
  *fullShape = runtime.rimeGetOption(sessionId_, "full_shape") != 0;
  return true;
}

// 数字/句点原生直通（bug#20 结构性修法）。返回 true = 已作答，调用方直接
// return。规则：
//   * 只在 filterKeyDown 阶段判（本函数由 filterKeyDown 调用），onKeyDown
//     靠 nativeDigitPassthroughPending_ 复用同一结论，两阶段必须一致。
//   * 组字中一律不接管：数字是选候选、句点是标点顶屏。
//   * 带 Ctrl/Alt/Shift 的组合键不接管。
//   * T9 类方案（alphabet 含数字）不接管：那里数字是输入。
//   * 数字键（0-9）无组字时直通；句点在"上一键是直通数字"时直通。
bool DaemonEngine::tryNativeDigitPassthrough(const KeyEventData& e,
                                             Json::Value& resp) {
  const bool isDigit = e.keyCode >= '0' && e.keyCode <= '9';
  const bool isPeriod =
      e.keyCode == VK_OEM_PERIOD || e.keyCode == VK_DECIMAL;
  if (!isDigit && !isPeriod) {
    lastKeyWasNativeDigit_ = false;
    // 其它键进了 filter 阶段：上一个直通键的 onKeyDown 不会再来了，
    // 清掉 pending，防止滞留标记误放行后续组字中的数字/句点。
    nativeDigitPassthroughPending_ = false;
    return false;
  }
  const BYTE* st = e.keyStates;
  const bool hasModifier =
      st != nullptr && (((st[VK_CONTROL] & 0x80) != 0) ||
                        ((st[VK_MENU] & 0x80) != 0) ||
                        ((st[VK_SHIFT] & 0x80) != 0));
  // 组字中：数字选候选、句点顶屏标点，全部交引擎。keyComposing_ 不能用——
  // 它是 processKey 在处理"上一键"之前采的样，落后一拍：空格刚上屏、组字
  // 已清时它仍是 true，紧跟的数字会被误判成组字态→走 IME 代插→Word 丢
  // 句点的场景在"先打中文再起列表"时复活（2026-11-05 晚自查发现）。这里
  // 现查引擎状态。数字/句点不是热路径，多一次查询可接受。
  const bool overlayActive = overlayCustomCount_ > 0 ||
                             !overlaySuperText_.empty() || aiState_.active ||
                             cloudClip_.active;
  if (hasModifier || overlayActive || schemaUsesDigitInput()) {
    lastKeyWasNativeDigit_ = false;
    return false;
  }
  {
    bool engineOk = false;
    const EngineState state = currentState(&engineOk);
    if (!engineOk || !state.composition.empty() || !state.candidates.empty()) {
      lastKeyWasNativeDigit_ = false;
      return false;
    }
  }
  if (isDigit) {
    lastKeyWasNativeDigit_ = true;
    nativeDigitPassthroughPending_ = true;
    lastKeyDownCode_ = static_cast<int>(e.keyCode);
    lastKeySkip_ = 0;
    lastKeyDownRet_ = false;
    lastKeyUpCode_ = 0;
    daemonLogDebugExtern(L"[native-digit] passthrough vk=" +
                         to_wstring(e.keyCode));
    resp["returnValue"] = 0;
    return true;
  }
  // 句点：只有紧跟原生直通数字时才直通，其它情况走 punctuator。
  const bool followDigit = lastKeyWasNativeDigit_;
  lastKeyWasNativeDigit_ = false;
  if (!followDigit) {
    return false;
  }
  nativeDigitPassthroughPending_ = true;
  lastKeyDownCode_ = static_cast<int>(e.keyCode);
  lastKeySkip_ = 0;
  lastKeyDownRet_ = false;
  lastKeyUpCode_ = 0;
  daemonLogDebugExtern(L"[native-digit] period passthrough vk=" +
                       to_wstring(e.keyCode));
  resp["returnValue"] = 0;
  return true;
}

// 当前方案的数字是否属于输入（T9 九宫格）：与 moqi_plain_number.lua 同口径
// ——方案 id 含 t9，或 speller/alphabet 里出现数字。结果缓存一次（方案切换
// 会重置，见 ensureSession）。纯判定逻辑抽到 DigitPassthrough.h（可单测）。
bool DaemonEngine::schemaUsesDigitInput() {
  if (schemaDigitChecked_) {
    return schemaUsesDigitInput_;
  }
  schemaDigitChecked_ = true;
  schemaUsesDigitInput_ = false;
  RimeRuntime& runtime = RimeRuntime::instance();
  if (sessionId_ == 0 || runtime.rimeGetCurrentSchema == nullptr) {
    return schemaUsesDigitInput_;
  }
  char schemaId[256] = {0};
  if (!runtime.rimeGetCurrentSchema(sessionId_, schemaId, sizeof(schemaId))) {
    return schemaUsesDigitInput_;
  }
  if (Moqi::schemaIdLooksT9(schemaId)) {
    schemaUsesDigitInput_ = true;
    return schemaUsesDigitInput_;
  }
  // 方案文件在共享数据目录（<rime>\data）与用户目录都可能存在；读 schema
  // 原文找 speller/alphabet 行（T9 方案把它写成含数字的字符集）。
  const std::wstring shared = utf8ToWide(runtime.sharedDataDirUtf8().c_str());
  const std::wstring userRoot = moqiAppDataDir();
  const std::wstring fileName = utf8ToWide(schemaId) + L".schema.yaml";
  const std::wstring candidates[] = {userRoot + L"\\" + fileName,
                                     shared + L"\\" + fileName};
  for (const auto& path : candidates) {
    std::ifstream in(path, std::ios::binary);
    if (!in) continue;
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    if (Moqi::schemaYamlUsesDigitAlphabet(text)) {
      schemaUsesDigitInput_ = true;
    }
    break; // 第一个存在的方案文件即权威
  }
  return schemaUsesDigitInput_;
}

// Port of shouldPassThroughModifierOnKey().
bool DaemonEngine::shouldPassThroughModifierOnKey(const KeyEventData& e,
                                                 bool filterRet) {
  if (filterRet) {
    return false;
  }
  if (e.keyCode == vkControl || e.keyCode == vkMenu) {
    return true;
  }
  if (e.charCode > 0 && e.charCode < 0x20) {
    return true;
  }
  const BYTE* states = e.keyStates;
  return (states[vkControl] & 0x80) != 0 || (states[vkMenu] & 0x80) != 0;
}
// Port of updateLangStatus(): refresh mode button icons when the engine
// flipped ascii/full-shape (Shift toggle, schema switches).
// 命令路径包装：读当前模式并按需附 changeButton（开关类 ON_COMMAND 用）。
void DaemonEngine::appendLangStatus(Json::Value& resp) {
  bool ascii = false;
  bool full = false;
  if (readInputModes(&ascii, &full)) {
    appendLangStatusChange(resp, ascii, full);
  }
}

void DaemonEngine::appendLangStatusChange(Json::Value& resp, bool ascii,
                                         bool full) {
  if (!asciiKnown_) {
    asciiBefore_ = ascii;
    fullBefore_ = full;
    asciiKnown_ = true;
    return;
  }
  if (ascii == asciiBefore_ && full == fullBefore_) {
    return;
  }
  asciiBefore_ = ascii;
  fullBefore_ = full;

  RimeRuntime& runtime = RimeRuntime::instance();
  wstring iconDir = runtime.iconDir();
  auto hasIcons = [](const wstring& dir) {
    return !dir.empty() &&
           ::GetFileAttributesW((dir + L"\\eng.ico").c_str()) !=
               INVALID_FILE_ATTRIBUTES;
  };
  if (!hasIcons(iconDir)) {
    // daemon 的 iconDir_ 是 <install>\icons（rime.dll 在安装根），实际图标
    // 在 <install>\moqi-ime\input_methods\rime\icons。安装布局探测同
    // buildActivatePayload；否则 Shift/Caps 后语言栏图标永不刷新。
    const wstring shared = utf8ToWide(runtime.sharedDataDirUtf8().c_str());
    const size_t slash = shared.find_last_of(L"\\/");
    const wstring rimeRoot =
        slash == wstring::npos ? wstring() : shared.substr(0, slash);
    const wstring candidates[] = {shared + L"\\icons", rimeRoot + L"\\icons",
                                 g_daemonProgramDir +
                                     L"\\moqi-ime\\input_methods\\rime\\icons"};
    for (const auto& c : candidates) {
      if (hasIcons(c)) {
        iconDir = c;
        break;
      }
    }
  }
  if (iconDir.empty()) {
    return;
  }
  const bool capsOn = (::GetKeyState(VK_CAPITAL) & 1) != 0;
  auto iconPath = [&iconDir](const wchar_t* name) {
    return wideToUtf8((iconDir + L"\\" + name).c_str());
  };

  Json::Value changes(Json::arrayValue);
  const wstring mode = ascii ? L"eng" : L"chi";
  const wstring shape = full ? L"full" : L"half";
  const wstring caps = capsOn ? L"on" : L"off";
  Json::Value modeIcon;
  modeIcon["id"] = "windows-mode-icon";
  modeIcon["icon"] =
      iconPath((mode + L"_" + shape + L"_caps" + caps + L".ico").c_str());
  changes.append(modeIcon);
  Json::Value langIcon;
  langIcon["id"] = "switch-lang";
  langIcon["icon"] = iconPath(ascii ? L"eng.ico" : L"chi.ico");
  changes.append(langIcon);
  Json::Value shapeIcon;
  shapeIcon["id"] = "switch-shape";
  shapeIcon["icon"] = iconPath(full ? L"full.ico" : L"half.ico");
  changes.append(shapeIcon);
  resp["changeButton"] = changes;
}

// Port of fillResponseFromBackendState(): commit bookkeeping + rerank
// pipeline (contextual lift → bigram → mixed pair) + displayPerm + typo
// hoist, then composition/candidate fields.
void DaemonEngine::buildResponse(const EngineState& state, bool allowCommit,
                                Json::Value& resp) {
  const RerankConfig& rerankCfg = RerankData::instance().config();
  if (allowCommit && !state.commit.empty()) {
    resp["commitString"] = state.commit;
    // 数字直通标记（bug#20）：单字符 0-9 置位，其他提交清零；供
    // filterKeyDown 的句点直通判定（Word 2016 自动编号窗口期丢代插）。
    lastCommitSingleDigit_ = state.commit.size() == 1 &&
                             state.commit[0] >= '0' &&
                             state.commit[0] <= '9';
    userDictDirty_ = true; // 有提交=主 userdb 有新学习，反激活时备份快照
    // recordTypoMemory：raw 优先取当前跟踪值，回退提交按键上的快照。
    string raw =
        rawInputTracked_.empty() ? rawInputAtCommit_ : rawInputTracked_;
    rawInputAtCommit_.clear();
    if (!raw.empty()) {
      TypoMemoryStore::instance().record(raw, state.commit);
    }
    // AI prompt 的 {{previous_commit}} 来源：Go rememberAICommit 在每次上屏
    // 时记录；旧实现只有 AI 自己的提交会写，普通打字永远是空。
    {
      std::lock_guard<std::mutex> aiLock(aiResultLock_);
      aiState_.previousCommit = state.commit;
    }
    contextualCommits_.push(state.commit, rerankCfg.windowSeconds);
    rawInputTracked_.clear();
  }
  Json::Value emptyList(Json::arrayValue);
  if (state.composition.empty()) {
    rawInputTracked_.clear();
    displayPerm_.clear();
    overlayCustomCount_ = 0;
    overlayCustomTexts_.clear();
    overlaySuperText_.clear();
    resp["compositionString"] = "";
    resp["candidateList"] = emptyList;
    resp["candidateEntries"] = emptyList;
    resp["showCandidates"] = false;
    return;
  }
  // 组字出现即清数字直通标记：组字中的句点必须交回引擎（标点顶屏/取字），
  // 不走应用原生插入。
  lastCommitSingleDigit_ = false;
  resp["compositionString"] = state.composition;
  const int caret =
      compositionCaretRuneIndex(state.composition, state.cursorPos);
  resp["cursorPos"] = caret;
  resp["compositionCursor"] = caret;
  resp["selStart"] = state.selStart;
  resp["selEnd"] = state.selEnd;

  // 置顶短语 / 超级简拼 overlay（Go custom_phrase/super_abbrev）。AI 覆盖
  // 期间不叠加（Go currentCustomPhraseOverlay 同）。
  InputOverlay overlay;
  if (!aiState_.active) {
    overlay = computeInputOverlay(rawInputTracked_, state.composition,
                                  state.pageNo, candidateCount_);
  }
  overlayCustomCount_ = static_cast<int>(overlay.custom.size());
  overlayCustomTexts_.clear();
  for (const auto& c : overlay.custom) overlayCustomTexts_.push_back(c.text);
  overlaySuperText_ = overlay.superText;
  const bool superSynthetic = overlay.custom.empty() &&
                              !overlay.superText.empty() &&
                              state.candidates.empty();

  size_t visible = state.candidates.size();
  if (candidateCount_ > 0 && visible > static_cast<size_t>(candidateCount_)) {
    visible = static_cast<size_t>(candidateCount_);
  }
  if (visible == 0 && overlay.custom.empty() && !superSynthetic) {
    displayPerm_.clear();
    resp["candidateList"] = emptyList;
    resp["candidateEntries"] = emptyList;
    resp["showCandidates"] = false;
    return;
  }

  // 截断后的候选进重排流水线（与 Go currentVisibleBackendState 一致）。
  vector<Cand> cands(state.candidates.begin(),
                     state.candidates.begin() + static_cast<long>(visible));
  int cursor = state.highlightedIndex;
  const vector<Cand> engineOrder = cands;
  string tail;
  if (contextualCommits_.activeTail(rerankCfg.windowSeconds, &tail)) {
    applyContextualLift(tail, cands, cursor);
    applyBigramRerank(tail, cands, cursor);
  }
  applyMixedPairRerank(state.composition, cands, cursor);
  displayPerm_ = buildDisplayPerm(engineOrder, cands);

  // 错字记忆提顶：同一 raw 上次选过的词已在候选里则提到第 1 位（仅改
  // 显示，displayPerm 已构建——与 Go 行为逐位一致）。
  if (!rawInputTracked_.empty() && cands.size() > 1) {
    const string remembered =
        TypoMemoryStore::instance().lookup(rawInputTracked_);
    if (!remembered.empty() && cands[0].first != remembered) {
      for (size_t i = 1; i < cands.size(); ++i) {
        if (cands[i].first == remembered) {
          const Cand item = cands[i];
          cands.erase(cands.begin() + static_cast<long>(i));
          cands.insert(cands.begin(), item);
          // 2026-11-02 修复：提顶改了显示顺序，displayPerm 必须同步旋转
          //（与 LocalEngine 同修复；否则空格/数字按提顶前映射选错词）。
          // 注意 buildDisplayPerm 在"同序"时返回空（=恒等映射）——提顶
          // 恰恰打破恒等，必须显式生成旋转后的映射，而不是对空表旋转。
          if (displayPerm_.empty()) {
            displayPerm_.resize(cands.size());
            for (size_t k = 0; k < cands.size(); ++k) {
              displayPerm_[k] = static_cast<int>(k);
            }
          }
          if (i < displayPerm_.size()) {
            const int picked = displayPerm_[i];
            displayPerm_.erase(displayPerm_.begin() + static_cast<long>(i));
            displayPerm_.insert(displayPerm_.begin(), picked);
          }
          break;
        }
      }
    }
  }

  Json::Value entries(Json::arrayValue);
  int aiCursor = -1;
  // 阶段5c AI 覆盖层：Go 只把第一条 AI 结果置顶（aiCandidates=1），其余
  // 数字键落到后端候选。旧实现把 3 条 AI 全塞进候选窗，按 4-7 会落到
  // AI 数组越界被吞，底层候选选不到。
  if (aiState_.active) {
    std::lock_guard<std::mutex> aiLock(aiResultLock_);
    if (!aiState_.candidates.empty()) {
      Json::Value item(Json::objectValue);
      item["text"] = aiState_.candidates[0];
      entries.append(item);
    }
    aiCursor = aiState_.cursor;
  }
  // 置顶短语在前；super abbrev 优先标在置顶短语第一项（Go
  // applySuperAbbrevOverlayToCandidates），否则标在引擎第一项。
  std::set<std::string> entryTexts;
  for (size_t i = 0; i < overlay.custom.size(); ++i) {
    Json::Value item(Json::objectValue);
    item["text"] = overlay.custom[i].text;
    if (i == 0 && !overlay.superText.empty()) {
      item["comment"] = overlay.superText;
    }
    entries.append(item);
    entryTexts.insert(overlay.custom[i].text);
  }
  if (overlay.custom.empty() && !overlay.superText.empty() &&
      !cands.empty()) {
    cands[0].second = overlay.superText;
  }
  const bool superOnly = overlay.custom.empty() && superSynthetic;
  if (superOnly) {
    Json::Value item(Json::objectValue);
    item["text"] = overlay.superText;
    item["comment"] = "?";
    entries.append(item);
    entryTexts.insert(overlay.superText);
  }
  for (const auto& cand : cands) {
    if (!entryTexts.insert(cand.first).second) continue;
    if (candidateCount_ > 0 &&
        static_cast<int>(entries.size()) >= candidateCount_) {
      break;
    }
    Json::Value item;
    item["text"] = cand.first;
    if (!cand.second.empty()) {
      item["comment"] = cand.second;
    }
    entries.append(item);
  }
  resp["candidateEntries"] = entries;
  if (aiState_.active && aiCursor >= 0) {
    // 覆盖层光标是"AI 第几条"，不是引擎高亮；之前被下面的引擎 cursor
    // 无条件覆盖，上下箭头看着不动、空格却提交另一条。
    resp["candidateCursor"] =
        aiCursor < static_cast<int>(entries.size())
            ? aiCursor
            : (entries.empty() ? 0 : static_cast<int>(entries.size()) - 1);
  } else if (!overlay.custom.empty() || superOnly) {
    // 置顶短语/合成候选默认选中第 1 项；数字键可直接选。
    resp["candidateCursor"] = 0;
  } else {
    if (cursor < 0) {
      cursor = 0;
    } else if (cursor >= static_cast<int>(entries.size())) {
      cursor = entries.empty() ? 0 : static_cast<int>(entries.size()) - 1;
    }
    resp["candidateCursor"] = cursor;
  }
  resp["showCandidates"] = true;

  // Select keys follow the backend contract: only announce on change.
  string selectKeys =
      state.selectKeys.empty() ? "1234567890" : state.selectKeys;
  if (selectKeys.size() > entries.size()) {
    selectKeys.resize(entries.size());
  }
  if (selectKeys != selectKeysCache_) {
    resp["setSelKeys"] = selectKeys;
    selectKeysCache_ = selectKeys;
  }
}

bool DaemonEngine::handleSecondSelectionFilterDown(const KeyEventData& e,
                                                  Json::Value& resp) {
  if (!semicolonSelectSecond_ || !isSecondSelectionShortcut(e)) {
    return false;
  }
  bool engineOk = false;
  const EngineState state = currentState(&engineOk);
  if (!engineOk || state.composition.empty() || state.candidates.size() < 2) {
    return false;
  }
  secondSelectConsumeKeyUpCode_ = selectionShortcutConsumeCode(e);
  resp["returnValue"] = 1;
  return true;
}

bool DaemonEngine::handleSecondSelectionDown(const KeyEventData& e,
                                            Json::Value& resp) {
  if (secondSelectConsumeKeyUpCode_ == 0 ||
      selectionShortcutConsumeCode(e) != secondSelectConsumeKeyUpCode_) {
    return false;
  }
  bool engineOk = false;
  const EngineState state = currentState(&engineOk);
  if (!engineOk || state.composition.empty() || state.candidates.size() < 2) {
    buildResponse(state, false, resp);
    resp["returnValue"] = 1;
    return true;
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  if (runtime.rimeSelectCandidateOnCurrentPage(sessionId_, 1)) {
    bool ok = false;
    const EngineState after = currentState(&ok);
    buildResponse(after, true, resp);
    appendLangStatusChange(resp, after.asciiMode, after.fullShape);
    resp["returnValue"] = 1;
    return true;
  }
  buildResponse(state, false, resp);
  resp["returnValue"] = 1;
  return true;
}

bool DaemonEngine::handleSecondSelectionFilterUp(const KeyEventData& e,
                                                Json::Value& resp) {
  if (secondSelectConsumeKeyUpCode_ == 0 ||
      selectionShortcutConsumeCode(e) != secondSelectConsumeKeyUpCode_) {
    return false;
  }
  resp["returnValue"] = 1;
  return true;
}

bool DaemonEngine::handleSecondSelectionUp(const KeyEventData& e,
                                          Json::Value& resp) {
  if (secondSelectConsumeKeyUpCode_ == 0 ||
      selectionShortcutConsumeCode(e) != secondSelectConsumeKeyUpCode_) {
    return false;
  }
  secondSelectConsumeKeyUpCode_ = 0;
  bool engineOk = false;
  const EngineState state = currentState(&engineOk);
  buildResponse(state, false, resp);
  resp["returnValue"] = 1;
  return true;
}

bool DaemonEngine::filterKeyDown(const KeyEventData& e, Json::Value& resp,
                                 std::function<void(Json::Value)> push) {
  RimeRuntime& runtime = RimeRuntime::instance();
  std::lock_guard<std::recursive_mutex> lock(runtime.mutex());

  // 5e 云剪贴板列表：激活期间的按键由选择器消费。
  if (cloudClip_.active) {
    if (handleCloudClipFilterDown(e, resp)) {
      return true;
    }
  } else if (maybeShowCloudClipList(e, push)) {
    lastKeyDownRet_ = true;
    resp["returnValue"] = 1;
    return true;
  }

  // 置顶短语 / 超级简拼：数字/空格选置顶候选；Tab/句点提交超级简拼。
  if (!aiState_.active &&
      (overlayCustomCount_ > 0 || !overlaySuperText_.empty())) {
    const bool superKey = e.keyCode == vkTab || e.keyCode == VK_OEM_PERIOD ||
                          e.charCode == '.';
    if (superKey && !overlaySuperText_.empty()) {
      overlayCommitText_ = overlaySuperText_;
      overlayConsumeKeyUpCode_ = superKey && e.keyCode == vkTab ? vkTab : '.';
      lastKeyDownRet_ = true;
      lastKeyUpCode_ = 0;
      resp["returnValue"] = 1;
      return true;
    }
    if (e.keyCode == vkSpace && overlayCustomCount_ > 0) {
      overlayCommitText_ = overlayCustomTexts_[0];
      overlayConsumeKeyUpCode_ = vkSpace;
      lastKeyDownRet_ = true;
      resp["returnValue"] = 1;
      return true;
    }
    if (e.keyCode >= '1' && e.keyCode <= '9') {
      const int index = static_cast<int>(e.keyCode) - '1';
      if (index < overlayCustomCount_) {
        overlayCommitText_ = overlayCustomTexts_[index];
        overlayConsumeKeyUpCode_ = static_cast<int>(e.keyCode);
        lastKeyDownRet_ = true;
        resp["returnValue"] = 1;
        return true;
      }
    }
  }

  // 阶段5c AI：结果覆盖期间，选择键/空格/回车/ESC/上下箭头由 AI 消费；
  // 其他键退出覆盖并正常走引擎（与 Go handleAIKeyDownFilter 一致）。
  if (aiState_.active) {
    const bool isSel = e.keyCode >= '1' && e.keyCode <= '9';
    const bool handled = isSel || e.keyCode == vkSpace ||
                         e.keyCode == vkReturn || e.keyCode == vkEscape ||
                         e.keyCode == vkUp || e.keyCode == vkDown;
    if (handled) {
      lastKeyDownRet_ = true;
      resp["returnValue"] = 1;
      return true;
    }
    resetAIState();
  }
  // 热键触发（Ctrl+Shift+X 等）；无组字/候选时不消费，键交还宿主。
  if (maybeTriggerAI(e, push)) {
    lastKeyDownRet_ = true;
    lastKeyUpCode_ = 0;
    resp["returnValue"] = 1;
    return true;
  }

  if (handleSecondSelectionFilterDown(e, resp)) {
    return true;
  }
  // 数字/句点原生直通（bug#20 结构性修法，2026-11-05）：无组字、无修饰键、
  // 非 T9 类方案时，数字键与"紧跟上一次直通数字"的句点键不进引擎，直接
  // returnValue=0 交应用原生插入——与微软/谷歌同一条路。IME 代插走
  // ITfInsertAtSelection 编辑会话，Word 自动编号/格式转换窗口期会把它静默
  // 丢掉（真机实测快打 "1. 2. 3." 只剩 ". . "）；应用原生插入没有这个问题。
  if (tryNativeDigitPassthrough(e, resp)) {
    return true;
  }
  // Same-key dedup (TSF duplicate events), with the bare-Shift exemption:
  // a lost keyup must never break the Shift toggle.
  // 数字后句点直通（bug#20）：无修饰键、非组字、引擎刚提交单个数字时，
  // 句点不进引擎（避免 rime 代插 "." 被 Word 自动编号窗口期丢弃），直接
  // 放行给应用原生插入；onKeyDown 阶段对同一键同样回"未吃"。
  {
    const bool isPeriodKey = e.keyCode == VK_OEM_PERIOD ||
                             e.keyCode == VK_DECIMAL;
    const BYTE* st = e.keyStates;
    const bool hasModifier =
        st != nullptr && (((st[VK_CONTROL] & 0x80) != 0) ||
                          ((st[VK_MENU] & 0x80) != 0) ||
                          ((st[VK_SHIFT] & 0x80) != 0));
    // keyComposing_ 落后一拍（processKey 处理上一键前采样）：引擎刚提交单个
    // 数字后它仍为 true，这里必须现查，否则兜底直通永远不触发。
    bool composingNow = true;
    if (isPeriodKey && lastCommitSingleDigit_ && !hasModifier) {
      bool engineOk = false;
      const EngineState state = currentState(&engineOk);
      composingNow = !engineOk || !state.composition.empty() ||
                     !state.candidates.empty();
    }
    if (isPeriodKey && lastCommitSingleDigit_ && !hasModifier &&
        !composingNow) {
      lastCommitSingleDigit_ = false;
      periodPassthroughPending_ = true;
      lastKeyDownCode_ = static_cast<int>(e.keyCode);
      lastKeySkip_ = 0;
      lastKeyDownRet_ = false;
      lastKeyUpCode_ = 0;
      daemonLogDebugExtern(
          L"[digit-period] passthrough (engine-side) vk=" +
          to_wstring(e.keyCode));
      resp["returnValue"] = 0;
      return true;
    }
    periodPassthroughPending_ = false;
  }
  if (!isBareShiftKeyCode(e.keyCode) &&
      static_cast<int>(e.keyCode) == lastKeyDownCode_) {
    ++lastKeySkip_;
    if (lastKeySkip_ >= 2) {
      lastKeyDownCode_ = 0;
      lastKeySkip_ = 0;
    }
  } else {
    lastKeyDownCode_ = static_cast<int>(e.keyCode);
    lastKeySkip_ = 0;
    if (!processKey(e, false, &lastKeyDownRet_)) {
      return false;
    }
    bool ascii = false;
    bool full = false;
    if (readInputModes(&ascii, &full)) {
      appendLangStatusChange(resp, ascii, full);
    }
  }
  lastKeyUpCode_ = 0;
  resp["returnValue"] = lastKeyDownRet_ ? 1 : 0;
  return true;
}

bool DaemonEngine::onKeyDown(const KeyEventData& e, Json::Value& resp) {
  RimeRuntime& runtime = RimeRuntime::instance();
  std::lock_guard<std::recursive_mutex> lock(runtime.mutex());

  // 数字/句点原生直通的下半程（bug#20）：filterKeyDown 已回"未吃"，
  // 这里对同一键也回"未吃"，避免应用原生插入后又被吃回。
  if (nativeDigitPassthroughPending_) {
    nativeDigitPassthroughPending_ = false;
    const bool isDigit = e.keyCode >= '0' && e.keyCode <= '9';
    const bool isPeriod =
        e.keyCode == VK_OEM_PERIOD || e.keyCode == VK_DECIMAL;
    if (isDigit || isPeriod) {
      lastKeyDownRet_ = false;
      resp["returnValue"] = 0;
      return true;
    }
  }

  // 数字后句点直通的下半程（bug#20）：filterKeyDown 已放行给应用，
  // 这里对同一键也回"未吃"，避免应用原生插入后又被吃回。
  if (periodPassthroughPending_) {
    periodPassthroughPending_ = false;
    if (e.keyCode == VK_OEM_PERIOD || e.keyCode == VK_DECIMAL) {
      lastKeyDownRet_ = false;
      resp["returnValue"] = 0;
      return true;
    }
  }

  // 置顶短语/超级简拼的提交（filterKeyDown 已消费按键）。
  if (!overlayCommitText_.empty()) {
    const std::string text = overlayCommitText_;
    overlayCommitText_.clear();
    overlayConsumeKeyUpCode_ = 0;
    if (sessionId_ != 0 && runtime.rimeClearComposition != nullptr) {
      runtime.rimeClearComposition(sessionId_);
    }
    keyComposing_ = false;
    rawInputTracked_.clear();
    rawInputAtCommit_.clear();
    displayPerm_.clear();
    overlayCustomCount_ = 0;
    overlayCustomTexts_.clear();
    overlaySuperText_.clear();
    resp["returnValue"] = 1;
    resp["commitString"] = text;
    resp["compositionString"] = "";
    resp["candidateList"] = Json::Value(Json::arrayValue);
    resp["candidateEntries"] = Json::Value(Json::arrayValue);
    resp["showCandidates"] = false;
    return true;
  }

  // 5e 云剪贴板列表：选字/翻页/删除/退出。
  if (cloudClip_.active) {
    if (handleCloudClipKeyDown(e, resp, nullptr)) {
      return true;
    }
  }

  // 阶段5c AI 覆盖层：数字/空格/回车提交 AI 候选；上下移动光标；ESC 退出。
  if (aiState_.active) {
    int target = -1;
    if (e.keyCode == vkSpace || e.keyCode == vkReturn) {
      target = aiState_.cursor;
    } else if (e.keyCode >= '1' && e.keyCode <= '9') {
      target = static_cast<int>(e.keyCode) - '1';
    }
    if (target >= 0) {
      if (target == 0) {
        if (commitAICandidate(0, resp)) return true;
        resp["returnValue"] = 1;
        return true;
      }
      // 显示序 = [AI 第 1 条, 引擎候选 0..n]。数字 2 起落到引擎第
      // target-1 位（Go commitBackendOverlayCandidate 同义）；旧实现
      // 只在 AI 数组里找，越界后吞键。
      bool engineOk = false;
      const EngineState before = currentState(&engineOk);
      if (engineOk && target - 1 < static_cast<int>(before.candidates.size())) {
        resetAIState();
        if (runtime.rimeSelectCandidateOnCurrentPage != nullptr &&
            runtime.rimeSelectCandidateOnCurrentPage(sessionId_, target - 1)) {
          bool ok = false;
          const EngineState after = currentState(&ok);
          buildResponse(after, true, resp);
          appendLangStatusChange(resp, after.asciiMode, after.fullShape);
          resp["returnValue"] = 1;
          return true;
        }
      }
      resp["returnValue"] = 1;
      return true;
    }
    if (e.keyCode == vkUp || e.keyCode == vkDown) {
      bool engineOk = false;
      const EngineState state = currentState(&engineOk);
      const int engineCount =
          engineOk ? static_cast<int>(state.candidates.size()) : 0;
      std::lock_guard<std::mutex> aiLock(aiResultLock_);
      const int n = 1 + engineCount;  // AI 第 1 条 + 引擎候选
      if (e.keyCode == vkUp && aiState_.cursor > 0) {
        --aiState_.cursor;
      } else if (e.keyCode == vkDown && aiState_.cursor + 1 < n) {
        ++aiState_.cursor;
      }
      buildResponse(state, false, resp);
      resp["returnValue"] = 1;
      return true;
    }
    if (e.keyCode == vkEscape) {
      resetAIState();
      // Go 把 Esc 继续交给引擎（onKey），由引擎清组字。只清响应会让
      // 覆盖层消失但组字还留在引擎里，下一个字母续接出怪候选。
      RimeRuntime& runtime = RimeRuntime::instance();
      std::lock_guard<std::recursive_mutex> lock(runtime.mutex());
      if (sessionId_ != 0 && runtime.rimeClearComposition != nullptr) {
        runtime.rimeClearComposition(sessionId_);
      }
      keyComposing_ = false;
      rawInputTracked_.clear();
      displayPerm_.clear();
      resp["returnValue"] = 1;
      resp["compositionString"] = "";
      resp["candidateList"] = Json::Value(Json::arrayValue);
      resp["candidateEntries"] = Json::Value(Json::arrayValue);
      resp["showCandidates"] = false;
      return true;
    }
    resetAIState();
  }

  if (handleSecondSelectionDown(e, resp)) {
    return true;
  }
  if (shouldPassThroughModifierOnKey(e, lastKeyDownRet_)) {
    resp["returnValue"] = 0;
    return true;
  }
  bool engineOk = false;
  const EngineState state = currentState(&engineOk);
  if (!engineOk) {
    resp["returnValue"] = 0;
    return true;
  }
  buildResponse(state, true, resp);
  appendLangStatusChange(resp, state.asciiMode, state.fullShape);
  // Go 每次 onKey 后比对四键并落盘；方案内快捷键改 ascii_punct/简繁时
  // 菜单命令路径覆盖不到，这里补捕获。
  syncSharedAfterOptionToggle(sessionId_);
  resp["returnValue"] = 1;
  return true;
}

bool DaemonEngine::filterKeyUp(const KeyEventData& e, Json::Value& resp) {
  RimeRuntime& runtime = RimeRuntime::instance();
  std::lock_guard<std::recursive_mutex> lock(runtime.mutex());

  // 置顶短语/超级简拼消费过的键，键松也要吞掉，否则宿主会收到残留。
  if (overlayConsumeKeyUpCode_ != 0 &&
      (static_cast<int>(e.keyCode) == overlayConsumeKeyUpCode_ ||
       (overlayConsumeKeyUpCode_ == '.' &&
        (e.keyCode == VK_OEM_PERIOD || e.charCode == '.')))) {
    overlayConsumeKeyUpCode_ = 0;
    resp["returnValue"] = 1;
    return true;
  }

  // 5e 云剪贴板列表：键松也消费。
  if (cloudClip_.active) {
    if (handleCloudClipKeyUp(e, resp)) {
      return true;
    }
  }

  if (handleSecondSelectionFilterUp(e, resp)) {
    return true;
  }
  if (!isBareShiftKeyCode(e.keyCode) &&
      static_cast<int>(e.keyCode) == lastKeyUpCode_) {
    lastKeyUpCode_ = 0;
  } else if (isKeyReleaseRelevantForBackend(e.keyCode)) {
    lastKeyUpCode_ = static_cast<int>(e.keyCode);
    if (!processKey(e, true, &lastKeyUpRet_)) {
      return false;
    }
    bool ascii = false;
    bool full = false;
    if (readInputModes(&ascii, &full)) {
      appendLangStatusChange(resp, ascii, full);
    }
  } else {
    // Ordinary key releases never reach the engine (sentence-context rule).
    lastKeyUpCode_ = static_cast<int>(e.keyCode);
    lastKeyUpRet_ = false;
  }
  lastKeyDownCode_ = 0;
  lastKeySkip_ = 0;
  resp["returnValue"] = lastKeyUpRet_ ? 1 : 0;
  return true;
}

bool DaemonEngine::onKeyUp(const KeyEventData& e, Json::Value& resp) {
  RimeRuntime& runtime = RimeRuntime::instance();
  std::lock_guard<std::recursive_mutex> lock(runtime.mutex());

  if (handleSecondSelectionUp(e, resp)) {
    return true;
  }
  if (shouldPassThroughModifierOnKey(e, lastKeyUpRet_)) {
    resp["returnValue"] = 0;
    return true;
  }
  bool engineOk = false;
  const EngineState state = currentState(&engineOk);
  if (!engineOk) {
    resp["returnValue"] = 0;
    return true;
  }
  buildResponse(state, true, resp);
  appendLangStatusChange(resp, state.asciiMode, state.fullShape);
  resp["returnValue"] = 1;
  return true;
}

bool DaemonEngine::highlightCandidate(int index, Json::Value& resp) {
  if (cloudClip_.active) {
    if (index < 0 || index >= kCloudClipPageSize) return false;
    const int target = cloudClip_.page * kCloudClipPageSize + index;
    if (target >= 0 && target < static_cast<int>(cloudClip_.entries.size())) {
      cloudClip_.cursor = target;
      fillCloudClipResponse(resp);
      resp["returnValue"] = 1;
      return true;
    }
    return false;
  }
  if (sessionId_ == 0 || index < 0 || index >= 9) {
    return false;
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  lock_guard<recursive_mutex> lock(runtime.mutex());
  // Optional export in some rime.dll builds (upstream defines it static);
  // missing behaves like the Go procAvailable guard: not supported.
  if (runtime.rimeHighlightCandidateOnCurrentPage == nullptr ||
      !runtime.rimeHighlightCandidateOnCurrentPage(sessionId_, index)) {
    return false;
  }
  bool engineOk = false;
  const EngineState state = currentState(&engineOk);
  buildResponse(state, false, resp);
  resp["returnValue"] = 1;
  return true;
}

bool DaemonEngine::selectCandidate(int index, Json::Value& resp) {
  if (!aiState_.active && overlayCustomCount_ > 0 && index >= 0 &&
      index < overlayCustomCount_) {
    const std::string text = overlayCustomTexts_[index];
    RimeRuntime& rt = RimeRuntime::instance();
    std::lock_guard<std::recursive_mutex> lock(rt.mutex());
    if (sessionId_ != 0 && rt.rimeClearComposition != nullptr) {
      rt.rimeClearComposition(sessionId_);
    }
    keyComposing_ = false;
    rawInputTracked_.clear();
    displayPerm_.clear();
    overlayCustomCount_ = 0;
    overlayCustomTexts_.clear();
    overlaySuperText_.clear();
    resp["returnValue"] = 1;
    resp["commitString"] = text;
    resp["compositionString"] = "";
    resp["candidateList"] = Json::Value(Json::arrayValue);
    resp["candidateEntries"] = Json::Value(Json::arrayValue);
    resp["showCandidates"] = false;
    return true;
  }
  if (!aiState_.active && overlayCustomCount_ > 0 && index >= 0) {
    index -= overlayCustomCount_;
  }
  if (cloudClip_.active) {
    if (index < 0 || index >= kCloudClipPageSize) return false;
    const int target = cloudClip_.page * kCloudClipPageSize + index;
    if (target < 0 || target >= static_cast<int>(cloudClip_.entries.size())) {
      return false;
    }
    commitCloudClipEntry(resp, cloudClip_.entries[target]);
    return true;
  }
  if (sessionId_ == 0 || index < 0 || index >= 9) {
    return false;
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  std::lock_guard<std::recursive_mutex> lock(runtime.mutex());
  secondSelectConsumeKeyUpCode_ = 0;
  if (!runtime.rimeSelectCandidateOnCurrentPage(sessionId_, index)) {
    return false;
  }
  bool engineOk = false;
  const EngineState state = currentState(&engineOk);
  buildResponse(state, true, resp);
  appendLangStatusChange(resp, state.asciiMode, state.fullShape);
  resp["returnValue"] = 1;
  return true;
}

bool DaemonEngine::changePage(bool backward, Json::Value& resp) {
  if (cloudClip_.active) {
    const int total = static_cast<int>(cloudClip_.entries.size());
    const int pageCount = (total + kCloudClipPageSize - 1) / kCloudClipPageSize;
    if (backward) {
      if (cloudClip_.page > 0) --cloudClip_.page;
    } else if (cloudClip_.page + 1 < pageCount) {
      ++cloudClip_.page;
    }
    cloudClip_.cursor = cloudClip_.page * kCloudClipPageSize;
    fillCloudClipResponse(resp);
    resp["returnValue"] = 1;
    return true;
  }
  if (sessionId_ == 0) {
    return false;
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  lock_guard<recursive_mutex> lock(runtime.mutex());
  // 定制 rime.dll 把 RimeChangePage 标成 static（只在 RimeApi 表里），
  // 平铺导出不存在。退化为发送 Page_Up/Page_Down keysym，由 navigator
  // processor 翻页——与用户按 -/= 键走的是同一条路径。
  int changed = 0;
  if (runtime.rimeChangePage != nullptr) {
    changed = runtime.rimeChangePage(sessionId_, backward ? 1 : 0);
  }
  if (!changed && runtime.rimeProcessKey != nullptr) {
    changed = runtime.rimeProcessKey(sessionId_, backward ? kRimePrior : kRimeNext, 0);
  }
  daemonLogExtern(L"[page] change backward=" + to_wstring(backward ? 1 : 0) +
                  L" ret=" + to_wstring(changed));
  if (!changed) {
    return false;
  }
  bool engineOk = false;
  const EngineState state = currentState(&engineOk);
  buildResponse(state, false, resp);
  resp["returnValue"] = 1;
  return true;
}

void DaemonEngine::onCompositionTerminated(bool forced) {
  // 组字被外部终止：AI 覆盖层/在途结果一起作废（Go 同）。
  resetAIState();
  secondSelectConsumeKeyUpCode_ = 0;
  keyComposing_ = false;
  rawInputTracked_.clear();
  displayPerm_.clear();
  if (forced) resetCloudClipState();
  if (sessionId_ == 0) {
    return;
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  std::lock_guard<std::recursive_mutex> lock(runtime.mutex());
  if (forced) {
    runtime.rimeDestroySession(sessionId_);
    sessionId_ = 0;
  } else {
    runtime.rimeClearComposition(sessionId_);
  }
}

// ------------------------------------------------ wire method wrappers --
// onActivate：建会话（tryEngage 的无 TSF 版——helper 永远"已接管"）。
void DaemonEngine::onActivate() {
  RimeRuntime& runtime = RimeRuntime::instance();
  if (!runtime.ready() || sessionId_ != 0) {
    return;
  }
  Json::Value cfg;
  const wstring root = moqiAppDataDir();
  if (!root.empty()) {
    ifstream file(root + L"\\appearance_config.json", ifstream::binary);
    if (file) {
      Json::CharReaderBuilder builder;
      std::string errs;
      if (!Json::parseFromStream(builder, file, &cfg, &errs)) {
        cfg = Json::Value();
      }
    }
  }
  candidateCount_ = 7;
  if (cfg["candidate_count"].isInt()) {
    const int n = cfg["candidate_count"].asInt();
    if (n == 3 || n == 5 || n == 7 || n == 9) candidateCount_ = n;
  }
  semicolonSelectSecond_ = cfg.get("semicolon_select_second", false).asBool();

  std::lock_guard<std::recursive_mutex> lock(runtime.mutex());
  sessionId_ = runtime.rimeCreateSession();
  if (sessionId_ == 0) {
    return;
  }
  if (cfg.get("input_state_shared", false).asBool()) {
    const Json::Value& shared = cfg["shared_options"];
    if (shared.isObject()) {
      if (shared.isMember("ascii_mode")) {
        runtime.rimeSetOption(sessionId_, "ascii_mode",
                              shared.get("ascii_mode", false).asBool() ? 1 : 0);
      }
      if (shared.isMember("full_shape")) {
        runtime.rimeSetOption(sessionId_, "full_shape",
                              shared.get("full_shape", false).asBool() ? 1 : 0);
      }
    }
  }
  refreshPageSize();
  asciiBefore_ = runtime.rimeGetOption(sessionId_, "ascii_mode") != 0;
  fullBefore_ = runtime.rimeGetOption(sessionId_, "full_shape") != 0;
  asciiKnown_ = true;
  selectKeysCache_.clear();
  lastKeyDownCode_ = 0;
  lastKeySkip_ = 0;
  lastKeyUpCode_ = 0;
  keyComposing_ = false;
  secondSelectConsumeKeyUpCode_ = 0;
  // 方案可能已换：数字直通方案判定与"上一键是数字"标记都要重置。
  schemaDigitChecked_ = false;
  lastKeyWasNativeDigit_ = false;
  nativeDigitPassthroughPending_ = false;
  rawInputTracked_.clear();
  rawInputAtCommit_.clear();
  displayPerm_.clear();
  RerankData::instance().ensureLoaded(runtime.sharedDataDirUtf8(),
                                      runtime.appDataRootUtf8());
  TypoMemoryStore::instance().ensureLoaded(runtime.appDataRootUtf8());
}

// ------------------------------------------- 5e 云剪贴板列表选择器 ----
// Go cloud_clipboard.go 编排层的对应实现。候选窗承载列表（每页 9 条），
// 热键 Ctrl+0 唤起，数字/空格/回车上屏，-/= 翻页，↑↓ 移动，Delete 删远端
// 条目，Esc/其他键退出。上屏后 applyingRemote 抑制 300ms 防自回声。

namespace {
struct ParsedHotkey {
  unsigned keyCode = '0';
  bool ctrl = true;
  bool alt = false;
  bool shift = false;
};

ParsedHotkey parseListHotkey(const std::string& text) {
  ParsedHotkey out;
  bool sawKey = false;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t plus = text.find('+', start);
    std::string token = text.substr(
        start, plus == std::string::npos ? std::string::npos : plus - start);
    size_t b = token.find_first_not_of(" \t");
    size_t e = token.find_last_not_of(" \t");
    token = b == std::string::npos ? "" : token.substr(b, e - b + 1);
    for (char& c : token) c = (char)std::toupper((unsigned char)c);
    if (token == "CTRL" || token == "CONTROL") {
      out.ctrl = true;
    } else if (token == "ALT" || token == "MENU") {
      out.alt = true;
    } else if (token == "SHIFT") {
      out.shift = true;
    } else if (token.size() == 1 &&
               ((token[0] >= 'A' && token[0] <= 'Z') ||
                (token[0] >= '0' && token[0] <= '9'))) {
      out.keyCode = (unsigned char)token[0];
      sawKey = true;
    } else if (!token.empty()) {
      return ParsedHotkey();  // 不支持的主键：回落 Ctrl+0
    }
    if (plus == std::string::npos) break;
    start = plus + 1;
  }
  if (!sawKey) return ParsedHotkey();
  return out;
}

ParsedHotkey cachedListHotkey() {
  static ULONGLONG loadedAt = 0;
  static ParsedHotkey cached;
  const ULONGLONG now = ::GetTickCount64();
  if (loadedAt == 0 || now - loadedAt >= 2000) {
    loadedAt = now;
    cached = parseListHotkey(loadCloudClipboardHotkey());
  }
  return cached;
}
}  // namespace

bool DaemonEngine::isCloudClipHotkey(const KeyEventData& e) const {
  // Go loadCloudClipboardListHotkey：默认 Ctrl+0，用户可在 json 里改。
  // 匹配规则：主键相同 + 修饰键精确匹配（Go modifiersMatch）。
  const BYTE* s = e.keyStates;
  if (s == nullptr) return false;
  const ParsedHotkey hk = cachedListHotkey();
  const bool ctrl = (s[VK_CONTROL] & 0x80) != 0 ||
                    (s[VK_LCONTROL] & 0x80) != 0 ||
                    (s[VK_RCONTROL] & 0x80) != 0 ||
                    e.keyCode == VK_CONTROL || e.keyCode == VK_LCONTROL ||
                    e.keyCode == VK_RCONTROL;
  const bool shift = (s[VK_SHIFT] & 0x80) != 0 ||
                     (s[VK_LSHIFT] & 0x80) != 0 ||
                     (s[VK_RSHIFT] & 0x80) != 0 ||
                     e.keyCode == VK_SHIFT || e.keyCode == VK_LSHIFT ||
                     e.keyCode == VK_RSHIFT;
  const bool alt = (s[VK_MENU] & 0x80) != 0 ||
                   (s[VK_LMENU] & 0x80) != 0 ||
                   (s[VK_RMENU] & 0x80) != 0 || e.keyCode == VK_MENU ||
                   e.keyCode == VK_LMENU || e.keyCode == VK_RMENU;
  return e.keyCode == hk.keyCode && ctrl == hk.ctrl && alt == hk.alt &&
         shift == hk.shift;
}

bool DaemonEngine::maybeShowCloudClipList(
    const KeyEventData& e, std::function<void(Json::Value)> push) {
  if (cloudClip_.active) return false;
  if (!isCloudClipHotkey(e)) return false;
  return showCloudClipList(push);
}

// 列表加载入口（热键与 preserved key 共用）：配置检查 + 后台拉取 + 推送。
bool DaemonEngine::showCloudClipList(
    std::function<void(Json::Value)> push) {
  if (cloudClip_.active) return false;
  if (cloudClipLoading_.exchange(true)) return true;  // 已在加载：去重
  CloudClipboardConfig cfg = loadCloudClipboardConfig();
  auto pushHint = [&push](const char* text, int durationMs) {
    if (!push) return;
    Json::Value m(Json::objectValue);
    m["success"] = true;
    m["seqNum"] = 0;
    m["returnValue"] = 1;
    m["cloudClip"] = false;
    Json::Value msg(Json::objectValue);
    msg["message"] = text;
    msg["duration"] = durationMs;
    m["showMessage"] = msg;
    push(m);
  };
  if (!cfg.enabled) {
    // Go ShowMessage "请先在设置中开启云剪贴板"。旧实现只写日志，
    // 用户按 Ctrl+0 看不到任何反应（键还被静默吞掉）。
    daemonLogExtern(L"[cloudclip] disabled; hint");
    pushHint("请先在设置中开启云剪贴板", 2500);
    cloudClipLoading_.store(false);
    return true; // 消费键但不弹列表
  }
  if (!cfg.isComplete() || !isAllowedBaseUrl(cfg.baseUrl)) {
    daemonLogExtern(L"[cloudclip] incomplete; hint");
    pushHint("请先完成 WebDAV 配置", 2500);
    cloudClipLoading_.store(false);
    return true;
  }
  // 后台拉列表 → push 推送候选窗
  resetCloudClipState();
  daemonLogExtern(L"[cloudclip] loading list...");
  if (cloudClipWorker_.joinable()) cloudClipWorker_.join();
  cloudClipWorker_ = std::thread([this, cfg, push]() {
    CloudClipboardSync sync(cfg);
    std::vector<CloudClipboardSync::DisplayItem> items;
    std::string err;
    auto pushMessage = [&push](const std::string& text) {
      if (!push) return;
      Json::Value m(Json::objectValue);
      m["success"] = true;
      m["seqNum"] = 0;
      m["returnValue"] = 1;
      m["cloudClip"] = false;
      Json::Value msg(Json::objectValue);
      msg["message"] = text;
      msg["duration"] = 3000;
      m["showMessage"] = msg;
      push(m);
    };
    if (!sync.listClipsForDisplay(&items, &err)) {
      daemonLogExtern(L"[cloudclip] load failed: " +
                      std::wstring(err.begin(), err.end()));
      pushMessage("加载失败: " + err);
      cloudClipLoading_.store(false);
      return;
    }
    if (items.empty()) {
      daemonLogExtern(L"[cloudclip] empty");
      pushMessage("云剪贴板为空");
      cloudClipLoading_.store(false);
      return;
    }
    // 先在本地构建完整列表，最后原子赋值（避免与 dispatch 线程的竞态：
    // push_back 扩容时数据指针失效会拿到垃圾）。
    std::vector<CloudClipEntry> loaded;
    for (const auto& it : items) {
      CloudClipEntry entry;
      entry.name = it.name;
      entry.preview = formatPreview(it.fullText, 60);  // Go 60 字符
      char buf[32] = {};
      if (it.lastModifiedMs > 0) {
        SYSTEMTIME st = {};
        FILETIME ft = {}, lft = {};
        ULARGE_INTEGER u{};
        u.QuadPart = (ULONGLONG)(it.lastModifiedMs * 10000 + 116444736000000000ULL);
        ft = *reinterpret_cast<FILETIME*>(&u);
        ::FileTimeToLocalFileTime(&ft, &lft);
        ::FileTimeToSystemTime(&lft, &st);
        snprintf(buf, sizeof(buf), "%02d-%02d %02d:%02d", st.wMonth,
                 st.wDay, st.wHour, st.wMinute);
        entry.comment = buf;
      }
      entry.fullText = it.fullText;
      loaded.push_back(std::move(entry));
    }
    Json::Value update(Json::objectValue);
    {
      // 与 dispatch 键盘路径共用 runtime 锁：列表落地/读取不能交错。
      std::lock_guard<std::recursive_mutex> lock(
          RimeRuntime::instance().mutex());
      cloudClip_.entries = std::move(loaded);
      cloudClip_.cursor = 0;
      cloudClip_.page = 0;
      cloudClip_.active = true;
      update["success"] = true;
      update["seqNum"] = 0;
      update["returnValue"] = 1;
      fillCloudClipResponse(update);
    }
    for (size_t k = 0; k < cloudClip_.entries.size() && k < 3; ++k) {
      daemonLogExtern(L"[cloudclip] entry[" + to_wstring(k) + L"] name=" +
                      std::wstring(cloudClip_.entries[k].name.begin(),
                                   cloudClip_.entries[k].name.end()) +
                      L" fullTextLen=" +
                      to_wstring(cloudClip_.entries[k].fullText.size()));
    }
    daemonLogExtern(L"[cloudclip] loaded n=" +
                    to_wstring(cloudClip_.entries.size()));
    // 推送候选窗更新
    if (push) push(update);
    cloudClipLoading_.store(false);
  });
  return true;
}

void DaemonEngine::fillCloudClipResponse(Json::Value& resp) {
  // DLL 据此进入/退出云剪贴板转发模式（所有键走 RPC）。
  resp["cloudClip"] = cloudClip_.active;
  const int total = static_cast<int>(cloudClip_.entries.size());
  if (total == 0) {
    resp["showCandidates"] = false;
    return;
  }
  const int pageCount = (total + kCloudClipPageSize - 1) / kCloudClipPageSize;
  if (cloudClip_.page < 0) cloudClip_.page = 0;
  if (cloudClip_.page >= pageCount) cloudClip_.page = pageCount - 1;
  const int start = cloudClip_.page * kCloudClipPageSize;
  const int end = min(start + kCloudClipPageSize, total);
  if (cloudClip_.cursor < start || cloudClip_.cursor >= end) {
    cloudClip_.cursor = start;
  }
  // 组字串显示页码（Go fmt.Sprintf("云剪贴板 %d/%d", page+1, pageCount)）
  char comp[32] = {};
  snprintf(comp, sizeof(comp), "\xe4\xba\x91\xe5\x89\xaa\xe8\xb4\xb4\xe6"
              "\x9d\xbf %d/%d",
           cloudClip_.page + 1, pageCount);
  resp["compositionString"] = comp;
  const int compCaret =
      compositionCaretRuneIndex(comp, static_cast<int>(strlen(comp)));
  resp["cursorPos"] = compCaret;
  resp["compositionCursor"] = compCaret;
  Json::Value entries(Json::arrayValue);
  for (int i = start; i < end; ++i) {
    Json::Value item;
    item["text"] = cloudClip_.entries[i].preview;
    if (!cloudClip_.entries[i].comment.empty()) {
      item["comment"] = cloudClip_.entries[i].comment;
    }
    entries.append(item);
  }
  resp["candidateEntries"] = entries;
  resp["candidateList"] = Json::Value(Json::arrayValue);
  resp["candidateCursor"] = cloudClip_.cursor - start;
  resp["showCandidates"] = true;
  resp["setSelKeys"] =
      std::string("123456789").substr(0, std::min<int>(9, end - start));
}

void DaemonEngine::commitCloudClipEntry(Json::Value& resp,
                                          const CloudClipEntry& entry) {
  // 必须先拷贝再 reset——resetCloudClipState 清空 entries vector，
  // entry 引用立即悬空（fullText 读到空串的根因）。
  const std::string text = entry.fullText;
  resetCloudClipState();
  resp["cloudClip"] = false;  // DLL 退出转发模式
  resp["commitString"] = text;
  resp["compositionString"] = "";
  resp["candidateList"] = Json::Value(Json::arrayValue);
  resp["candidateEntries"] = Json::Value(Json::arrayValue);
  resp["showCandidates"] = false;
  resp["returnValue"] = 1;
  keyComposing_ = false;
}

bool DaemonEngine::isCloudClipHandledKey(const KeyEventData& e) const {
  return e.keyCode == vkEscape || e.keyCode == vkReturn ||
         e.keyCode == vkSpace || e.keyCode == vkDelete ||
         e.keyCode == vkUp || e.keyCode == vkDown ||
         (e.keyCode >= '1' && e.keyCode <= '9') ||
         e.keyCode == VK_OEM_MINUS || e.keyCode == VK_OEM_PLUS ||
         e.charCode == '-' || e.charCode == '=' ||
         e.keyCode == VK_CONTROL || e.keyCode == VK_SHIFT ||
         e.keyCode == VK_MENU || e.keyCode == '0';
}

bool DaemonEngine::handleCloudClipFilterDown(const KeyEventData& e,
                                               Json::Value& resp) {
  if (!cloudClip_.active) return false;
  if (isCloudClipHotkey(e)) {
    // 再按热键 = 刷新（Go：fill + returnValue 1）
    fillCloudClipResponse(resp);
    resp["returnValue"] = 1;
    return true;
  }
  if (isCloudClipHandledKey(e)) {
    resp["returnValue"] = 1;
    return true;
  }
  // 其他键退出覆盖层（Go resetCloudClipboardState）。必须带 cloudClip:false，
  // 否则 DLL 的转发模式闩锁永远不会复位（按键从此全走 RPC）。
  resetCloudClipState();
  resp["cloudClip"] = false;
  return false;
}

bool DaemonEngine::handleCloudClipKeyDown(
    const KeyEventData& e, Json::Value& resp,
    std::function<void(Json::Value)> push) {
  if (!cloudClip_.active) return false;
  const int total = static_cast<int>(cloudClip_.entries.size());
  daemonLogExtern(L"[cloudclip] handleKeyDown vk=" + to_wstring(e.keyCode) +
                  L" total=" + to_wstring(total));

  if (isCloudClipHotkey(e)) {
    fillCloudClipResponse(resp);
    resp["returnValue"] = 1;
    return true;
  }
  // 翻页：- 前一页，= 后一页
  if (e.keyCode == VK_OEM_MINUS || e.charCode == '-') {
    if (cloudClip_.page > 0) --cloudClip_.page;
    cloudClip_.cursor = cloudClip_.page * kCloudClipPageSize;
    fillCloudClipResponse(resp);
    resp["returnValue"] = 1;
    return true;
  }
  if (e.keyCode == VK_OEM_PLUS || e.charCode == '=') {
    const int pageCount =
        (total + kCloudClipPageSize - 1) / kCloudClipPageSize;
    if (cloudClip_.page + 1 < pageCount) ++cloudClip_.page;
    cloudClip_.cursor = cloudClip_.page * kCloudClipPageSize;
    fillCloudClipResponse(resp);
    resp["returnValue"] = 1;
    return true;
  }
  if (e.keyCode == vkEscape) {
    resetCloudClipState();
    resp["cloudClip"] = false;  // DLL 退出转发模式
    resp["returnValue"] = 1;
    resp["compositionString"] = "";
    resp["candidateList"] = Json::Value(Json::arrayValue);
    resp["candidateEntries"] = Json::Value(Json::arrayValue);
    resp["showCandidates"] = false;
    keyComposing_ = false;
    return true;
  }
  if (e.keyCode == vkReturn || e.keyCode == vkSpace) {
    if (cloudClip_.cursor >= 0 && cloudClip_.cursor < total) {
      commitCloudClipEntry(resp, cloudClip_.entries[cloudClip_.cursor]);
      return true;
    }
    resp["returnValue"] = 1;
    return true;
  }
  if (e.keyCode == vkDelete) {
    // 删远端条目（Go deleteCurrentCloudClipboardEntry）。Go 同步等 DELETE
    // 结果，失败保留条目并提示；旧实现先本地删再异步请求，失败也看不出来。
    if (cloudClip_.cursor >= 0 && cloudClip_.cursor < total) {
      const std::string name = cloudClip_.entries[cloudClip_.cursor].name;
      CloudClipboardConfig cfg = loadCloudClipboardConfig();
      CloudClipboardSync sync(cfg);
      std::string err;
      if (!sync.deleteClip(name, &err)) {
        daemonLogExtern(L"[cloudclip] delete failed: " +
                        std::wstring(err.begin(), err.end()));
        Json::Value msg(Json::objectValue);
        msg["message"] = "删除失败: " + (err.empty() ? "未知错误" : err);
        msg["duration"] = 3000;
        resp["showMessage"] = msg;
        fillCloudClipResponse(resp);
        resp["returnValue"] = 1;
        return true;
      }
      cloudClip_.entries.erase(cloudClip_.entries.begin() + cloudClip_.cursor);
      if (cloudClip_.entries.empty()) {
        resetCloudClipState();
        resp["cloudClip"] = false;
        resp["returnValue"] = 1;
        resp["compositionString"] = "";
        resp["candidateList"] = Json::Value(Json::arrayValue);
        resp["candidateEntries"] = Json::Value(Json::arrayValue);
        resp["showCandidates"] = false;
        keyComposing_ = false;
        return true;
      }
      if (cloudClip_.cursor >= static_cast<int>(cloudClip_.entries.size())) {
        cloudClip_.cursor = static_cast<int>(cloudClip_.entries.size()) - 1;
      }
      cloudClip_.page = cloudClip_.cursor / kCloudClipPageSize;
    }
    fillCloudClipResponse(resp);
    resp["returnValue"] = 1;
    return true;
  }
  if (e.keyCode == vkUp) {
    if (cloudClip_.cursor > 0) --cloudClip_.cursor;
    cloudClip_.page = cloudClip_.cursor / kCloudClipPageSize;
    fillCloudClipResponse(resp);
    resp["returnValue"] = 1;
    return true;
  }
  if (e.keyCode == vkDown) {
    if (cloudClip_.cursor + 1 < total) ++cloudClip_.cursor;
    cloudClip_.page = cloudClip_.cursor / kCloudClipPageSize;
    fillCloudClipResponse(resp);
    resp["returnValue"] = 1;
    return true;
  }
  // 数字选条目
  if (e.keyCode >= '1' && e.keyCode <= '9') {
    daemonLogExtern(L"[cloudclip] digit key=" + to_wstring(e.keyCode) +
                    L" total=" + to_wstring(total) +
                    L" cursor=" + to_wstring(cloudClip_.cursor) +
                    L" page=" + to_wstring(cloudClip_.page));
    const int idx = static_cast<int>(e.keyCode) - '1';
    const int start = cloudClip_.page * kCloudClipPageSize;
    const int end = min(start + kCloudClipPageSize, total);
    const int absolute = start + idx;
    if (absolute >= start && absolute < end) {
      commitCloudClipEntry(resp, cloudClip_.entries[absolute]);
      return true;
    }
  }
  // 修饰键：消费但不动作
  if (e.keyCode == VK_CONTROL || e.keyCode == VK_SHIFT ||
      e.keyCode == VK_MENU || e.keyCode == '0') {
    fillCloudClipResponse(resp);
    resp["returnValue"] = 1;
    return true;
  }
  // 其他键退出
  resetCloudClipState();
  resp["cloudClip"] = false;
  return false;
}

bool DaemonEngine::handleCloudClipKeyUp(const KeyEventData& e,
                                          Json::Value& resp) {
  if (!cloudClip_.active) return false;
  if (isCloudClipHandledKey(e) || e.keyCode == '0') {
    fillCloudClipResponse(resp);
    resp["returnValue"] = 1;
    return true;
  }
  return false;
}

} // namespace Moqi
