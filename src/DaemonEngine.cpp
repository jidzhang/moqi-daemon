//
//	DaemonEngine.cpp: per-client session logic ported verbatim from
//	LocalEngine.cpp (phase 5) with the TSF coupling removed.
//
#include "DaemonEngine.h"

#include <algorithm>
#include <fstream>

namespace Moqi {
// main.cpp 共用的轻量日志（临时探针用；见 processKey 内 [remap] 标记）。
void daemonLogExtern(const std::wstring& msg);
}

using namespace std;

namespace Moqi {

// main.cpp 注入的安装布局（engine init 需要）。
std::wstring g_daemonProgramDir;
std::wstring g_daemonBackendDir;

void DaemonEngine::ensureEngine() {
  RimeRuntime& runtime = RimeRuntime::instance();
  if (!runtime.supported() || runtime.failed() || runtime.ready()) {
    return;
  }
  // rime.dll 用 daemon 自己的目录（x86 版，放安装根；moqi-ime\...\rime.dll
  // 是 x64，归 Go 后端/x64 TSF DLL）。MOQI_RIME_DLL_DIR 可覆盖（测试用）。
  const wchar_t* dllDirEnv = _wgetenv(L"MOQI_RIME_DLL_DIR");
  const std::wstring dllDir =
      dllDirEnv && *dllDirEnv ? std::wstring(dllDirEnv) : g_daemonProgramDir;
  runtime.requestInit(g_daemonProgramDir, g_daemonBackendDir, dllDir);
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
}

// ---------------------------------------------------------------- AI ------

void DaemonEngine::resetAIState() {
  std::lock_guard<std::mutex> lock(aiResultLock_);
  aiState_.active = false;
  aiState_.candidates.clear();
  aiState_.cursor = 0;
  aiState_.prompt.clear();
}

// 热键触发：读配置 → 匹配 action → 后台 HTTP。返回 true = 键被消费。
bool DaemonEngine::maybeTriggerAI(const KeyEventData &e,
                                  std::function<void(Json::Value)> push) {
  if (!aiState_.client.load()) {
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

  bool engineOk = false;
  const EngineState state = currentState(&engineOk);
  std::vector<std::string> inputCandidates;
  for (const auto &cand : state.candidates) {
    if (inputCandidates.size() >= 3) break;
    inputCandidates.push_back(cand.first);
  }
  if (state.composition.empty() && inputCandidates.empty()) {
    return false; // 无组字也无候选：不消费（键交还宿主）
  }

  const long long seq = ++aiState_.requestSeq;
  aiState_.pending.store(true);
  aiState_.active = false;
  {
    std::lock_guard<std::mutex> lock(aiResultLock_);
    aiState_.prompt = state.composition;
    aiState_.candidates.clear();
    aiState_.cursor = 0;
    aiState_.error.clear();
  }

  const AIAction actionCopy = *action;
  const std::string composition = state.composition;
  const std::string previousCommit = aiState_.previousCommit;
  if (aiWorker_.joinable()) {
    aiWorker_.join(); // 上一轮已结束（pending 保证同一时刻只有一个）
  }
  aiWorker_ = std::thread([this, actionCopy, composition, inputCandidates,
                           previousCommit, seq, push]() {
    std::vector<std::string> result;
    std::string err;
    const bool ok = aiState_.client.generate(actionCopy, composition,
                                             inputCandidates, previousCommit,
                                             &result, &err);
    Json::Value update;
    {
      std::lock_guard<std::mutex> lock(aiResultLock_);
      if (seq != aiState_.requestSeq.load()) {
        aiState_.pending.store(false);
        return; // 过期结果丢弃
      }
      aiState_.pending.store(false);
      if (!ok) {
        aiState_.error = err;
        aiState_.active = false;
        return;
      }
      aiState_.candidates = std::move(result);
      aiState_.cursor = 0;
      aiState_.active = !aiState_.candidates.empty();
      if (!aiState_.active) return;
      // 主动推一条候选更新（等价 Go 的 asyncResponseSender）：DLL 收到后
      // 立即刷新候选窗，无需等下一次按键。
      update = Json::Value(Json::objectValue);
      update["success"] = true;
      update["seqNum"] = 0; // 异步更新（不与任何同步请求配对）
      update["returnValue"] = 1;
      Json::Value entries(Json::arrayValue);
      for (const auto &text : aiState_.candidates) {
        Json::Value item(Json::objectValue);
        item["text"] = text;
        entries.append(item);
      }
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
    } else {
      daemonLogExtern(L"[ai] no push sink (result only in overlay)");
    }
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
  if (sessionId_ == 0) {
    return;
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  std::lock_guard<std::mutex> lock(runtime.mutex());
  runtime.rimeDestroySession(sessionId_);
  sessionId_ = 0;
  secondSelectConsumeKeyUpCode_ = 0;
  keyComposing_ = false;
}

bool DaemonEngine::ensureSession() {
  if (sessionId_ != 0) {
    return true;
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  sessionId_ = runtime.rimeCreateSession();
  return sessionId_ != 0;
}

// Sets menu/page_size to the configured candidate count (backend
// createSession(): SetCandidatePageSize when count != 9).
bool DaemonEngine::refreshPageSize() {
  if (sessionId_ == 0 || candidateCount_ == 9) {
    return true;
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  char schemaId[256] = {};
  if (!runtime.rimeGetCurrentSchema(sessionId_, schemaId, sizeof(schemaId))) {
    return false;
  }
  if (schemaId[0] == '\0') {
    return true;
  }
  rimeapi::Config config = {};
  if (!runtime.rimeSchemaOpen(schemaId, &config)) {
    return false;
  }
  const bool ok =
      runtime.rimeConfigSetInt(&config, "menu/page_size", candidateCount_) != 0;
  runtime.rimeConfigClose(&config);
  return ok;
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
    daemonLogExtern(L"[remap] composing key=" + to_wstring(e.keyCode) +
                    L" permSize=" + to_wstring(displayPerm_.size()));
    int target = -1;
    if (effKeyCode == vkSpace) {
      target = 0;
    } else if (effKeyCode >= '1' && effKeyCode <= '9') {
      target = static_cast<int>(effKeyCode) - '1';
    }
    if (target >= 0 && target < static_cast<int>(displayPerm_.size())) {
      const int engineIdx = displayPerm_[target];
      daemonLogExtern(L"[remap] target=" + to_wstring(target) +
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

  const wstring& iconDir = RimeRuntime::instance().iconDir();
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
    // recordTypoMemory：raw 优先取当前跟踪值，回退提交按键上的快照。
    string raw =
        rawInputTracked_.empty() ? rawInputAtCommit_ : rawInputTracked_;
    rawInputAtCommit_.clear();
    if (!raw.empty()) {
      TypoMemoryStore::instance().record(raw, state.commit);
    }
    contextualCommits_.push(state.commit, rerankCfg.windowSeconds);
    rawInputTracked_.clear();
  }
  Json::Value emptyList(Json::arrayValue);
  if (state.composition.empty()) {
    rawInputTracked_.clear();
    displayPerm_.clear();
    resp["compositionString"] = "";
    resp["candidateList"] = emptyList;
    resp["candidateEntries"] = emptyList;
    resp["showCandidates"] = false;
    return;
  }
  resp["compositionString"] = state.composition;
  const int caret =
      compositionCaretRuneIndex(state.composition, state.cursorPos);
  resp["cursorPos"] = caret;
  resp["compositionCursor"] = caret;
  resp["selStart"] = state.selStart;
  resp["selEnd"] = state.selEnd;

  size_t visible = state.candidates.size();
  if (candidateCount_ > 0 && visible > static_cast<size_t>(candidateCount_)) {
    visible = static_cast<size_t>(candidateCount_);
  }
  if (visible == 0) {
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
  // 阶段5c AI 覆盖层：AI 候选置顶（与 Go fillAIResponse 的叠加顺序一致）。
  if (aiState_.active) {
    std::lock_guard<std::mutex> aiLock(aiResultLock_);
    for (const auto &text : aiState_.candidates) {
      Json::Value item(Json::objectValue);
      item["text"] = text;
      entries.append(item);
    }
  }
  for (const auto& cand : cands) {
    Json::Value item;
    item["text"] = cand.first;
    if (!cand.second.empty()) {
      item["comment"] = cand.second;
    }
    entries.append(item);
  }
  resp["candidateEntries"] = entries;
  if (aiState_.active) {
    resp["candidateCursor"] = aiState_.cursor;
  }
  if (cursor < 0) {
    cursor = 0;
  } else if (cursor >= static_cast<int>(cands.size())) {
    cursor = static_cast<int>(cands.size()) - 1;
  }
  resp["candidateCursor"] = cursor;
  resp["showCandidates"] = true;

  // Select keys follow the backend contract: only announce on change.
  string selectKeys =
      state.selectKeys.empty() ? "1234567890" : state.selectKeys;
  if (selectKeys.size() > cands.size()) {
    selectKeys.resize(cands.size());
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
  std::lock_guard<std::mutex> lock(runtime.mutex());

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
  // Same-key dedup (TSF duplicate events), with the bare-Shift exemption:
  // a lost keyup must never break the Shift toggle.
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
  std::lock_guard<std::mutex> lock(runtime.mutex());

  // 阶段5c AI 覆盖层：数字/空格/回车提交 AI 候选；上下移动光标；ESC 退出。
  if (aiState_.active) {
    int target = -1;
    if (e.keyCode == vkSpace || e.keyCode == vkReturn) {
      target = aiState_.cursor;
    } else if (e.keyCode >= '1' && e.keyCode <= '9') {
      target = static_cast<int>(e.keyCode) - '1';
    }
    if (target >= 0) {
      if (commitAICandidate(target, resp)) {
        return true;
      }
      resp["returnValue"] = 1;
      return true;
    }
    if (e.keyCode == vkUp || e.keyCode == vkDown) {
      std::lock_guard<std::mutex> aiLock(aiResultLock_);
      const int n = static_cast<int>(aiState_.candidates.size());
      if (e.keyCode == vkUp && aiState_.cursor > 0) {
        --aiState_.cursor;
      } else if (e.keyCode == vkDown && aiState_.cursor + 1 < n) {
        ++aiState_.cursor;
      }
      bool engineOk = false;
      const EngineState state = currentState(&engineOk);
      buildResponse(state, false, resp);
      resp["returnValue"] = 1;
      return true;
    }
    if (e.keyCode == vkEscape) {
      resetAIState();
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
  resp["returnValue"] = 1;
  return true;
}

bool DaemonEngine::filterKeyUp(const KeyEventData& e, Json::Value& resp) {
  RimeRuntime& runtime = RimeRuntime::instance();
  std::lock_guard<std::mutex> lock(runtime.mutex());

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
  std::lock_guard<std::mutex> lock(runtime.mutex());

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
  if (sessionId_ == 0 || index < 0 || index >= 9) {
    return false;
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  lock_guard<mutex> lock(runtime.mutex());
  // Optional export in some rime.dll builds (upstream defines it static);
  // missing behaves like the Go procAvailable guard: not supported.
  if (runtime.rimeHighlightCandidateOnCurrentPage == nullptr ||
      !runtime.rimeHighlightCandidateOnCurrentPage(sessionId_, index)) {
    return false;
  }
  bool engineOk = false;
  const EngineState state = currentState(&engineOk);
  buildResponse(state, false, resp);
  return true;
}

bool DaemonEngine::selectCandidate(int index, Json::Value& resp) {
  if (sessionId_ == 0 || index < 0 || index >= 9) {
    return false;
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  std::lock_guard<std::mutex> lock(runtime.mutex());
  secondSelectConsumeKeyUpCode_ = 0;
  if (!runtime.rimeSelectCandidateOnCurrentPage(sessionId_, index)) {
    return false;
  }
  bool engineOk = false;
  const EngineState state = currentState(&engineOk);
  buildResponse(state, true, resp);
  appendLangStatusChange(resp, state.asciiMode, state.fullShape);
  return true;
}

bool DaemonEngine::changePage(bool backward, Json::Value& resp) {
  if (sessionId_ == 0) {
    return false;
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  lock_guard<mutex> lock(runtime.mutex());
  if (runtime.rimeChangePage == nullptr ||
      !runtime.rimeChangePage(sessionId_, backward ? 1 : 0)) {
    return false;
  }
  bool engineOk = false;
  const EngineState state = currentState(&engineOk);
  buildResponse(state, false, resp);
  return true;
}

void DaemonEngine::onCompositionTerminated(bool forced) {
  secondSelectConsumeKeyUpCode_ = 0;
  keyComposing_ = false;
  rawInputTracked_.clear();
  displayPerm_.clear();
  if (sessionId_ == 0) {
    return;
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  std::lock_guard<std::mutex> lock(runtime.mutex());
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
    ifstream file(root + L"\appearance_config.json", ifstream::binary);
    if (file) {
      file >> cfg;
    }
  }
  candidateCount_ = 7;
  if (cfg["candidate_count"].isInt() && cfg["candidate_count"].asInt() > 0) {
    candidateCount_ = cfg["candidate_count"].asInt();
  }
  semicolonSelectSecond_ = cfg.get("semicolon_select_second", false).asBool();

  std::lock_guard<std::mutex> lock(runtime.mutex());
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
  rawInputTracked_.clear();
  rawInputAtCommit_.clear();
  displayPerm_.clear();
  RerankData::instance().ensureLoaded(runtime.sharedDataDirUtf8(),
                                      runtime.appDataRootUtf8());
  TypoMemoryStore::instance().ensureLoaded(runtime.appDataRootUtf8());
}

} // namespace Moqi
