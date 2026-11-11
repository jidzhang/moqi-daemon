//
//	DaemonEngine: per-client engine session for the C++ helper process.
//	Ported from LocalEngine (TSF DLL) minus all TSF coupling — the helper
//	is always local and always engaged. Key semantics mirror the Go backend
//	hot path one-to-one (see docs/24-交接).
//
#ifndef MOQI_HELPER_ENGINE_H_
#define MOQI_HELPER_ENGINE_H_

#include "RimeCore.h"
#include "LocalRerank.h"
#include "DaemonAI.h"

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include <json/json.h>

namespace Moqi {

class DaemonEngine {
public:
  DaemonEngine();
  ~DaemonEngine();

  void ensureEngine();     // one-shot runtime init (blocking, first client)
  bool engineReady() const;

  // 菜单服务需要按会话读选项/方案；引擎所有权仍在 DaemonEngine。
  rimeapi::SessionId sessionId() const { return sessionId_; }

  // 会话生命周期（onActivate = tryEngage 的无 TSF 版）。
  void onActivate();
  void deactivate();
  // 命令响应附中英/全半角图标刷新（changeButton；开关类 ON_COMMAND 用，
  // 键路径在 filterKeyDown/Up 里已自带）。
  void appendLangStatus(Json::Value& resp);
  // 菜单"总候选数"变更的即时生效：更新计数 + 写 menu/page_size。
  void applyCandidateCount(int count);
  // 方案切换后重写 menu/page_size（新方案默认 9，会和界面候选数不一致）。
  void applySchemaPageSize() {
    refreshPageSize();
    pendingPageSize_ = false;
  }
  void setSemicolonSelectSecond(bool enabled) {
    semicolonSelectSecond_ = enabled;
  }
  // 自上次导出后是否有提交（= 主 userdb 有新学习）。客户端反激活时据此
  // 决定是否同步+备份快照，让 64 位进程能导入 32 位应用学到的词。
  bool consumeUserDictDirty() {
    bool d = userDictDirty_;
    userDictDirty_ = false;
    return d;
  }
  bool hasUserDictChanges() const { return userDictDirty_; }
  // 当前输入模式（ascii/full）——ON_ACTIVATE 本地应答选按钮图标用。
  bool uiModeState(bool* ascii, bool* full) {
    return readInputModes(ascii, full);
  }
  // 按键/候选方法与 LocalEngine 同签名（KeyEventData 输入）。
  bool filterKeyDown(const KeyEventData& e, Json::Value& resp,
                     std::function<void(Json::Value)> push = nullptr);
  bool onKeyDown(const KeyEventData& e, Json::Value& resp);
  bool filterKeyUp(const KeyEventData& e, Json::Value& resp);
  bool onKeyUp(const KeyEventData& e, Json::Value& resp);
  bool highlightCandidate(int index, Json::Value& resp);
  bool selectCandidate(int index, Json::Value& resp);
  bool changePage(bool backward, Json::Value& resp);
  void onCompositionTerminated(bool forced);

  // ---- 阶段5c：AI 评审（Go rime.go AI 分支的对应实现）----
  // 热键按下时触发后台请求；结果到齐后 aiActive=true，候选窗顶部显示
  // AI 结果，选中即上屏。返回 true = 该键已被 AI 消费。
  // push：结果到达时把候选更新响应主动推给客户端（Go 的 asyncResponseSender
  // 等价物；异步保证慢网络不阻塞打字）。
  // localState：64 位客户端（DLL 进程内引擎）转发热键时携带的
  // {localComposition, localCandidates}；非空时本引擎会话不参与，覆盖层
  // 由 DLL 本地保管，这里只生成并推送（aiActive 不置位）。
  bool maybeTriggerAI(const KeyEventData& e,
                      std::function<void(Json::Value)> push,
                      const Json::Value* localState = nullptr);
  // 选中 AI 候选（index 为 AI 覆盖层的位次）。
  bool commitAICandidate(int index, Json::Value& resp);
  void resetAIState();
  bool aiActive() const { return aiState_.active; }
  bool aiPending() const { return aiState_.pending.load(); }

  // ---- 5e 云剪贴板列表选择器 ----
  // 热键（Ctrl+0）触发 → 后台拉列表 → push 推送候选窗。返回 true=消费。
  // public：main.cpp 的 localComposition 早退路径也要查它（2026-11-03）。
  bool maybeShowCloudClipList(const KeyEventData& e,
                              std::function<void(Json::Value)> push);
  // 列表加载入口（preserved key 路径直接调，不经按键判定）。
  bool showCloudClipList(std::function<void(Json::Value)> push);
  bool cloudClipActive() const { return cloudClip_.active; }
  bool cloudClipLoading() const { return cloudClipLoading_.load(); }

private:
  struct EngineState {
    bool ok = false;
    std::string commit;
    std::string composition;
    int pageNo = 0;
    int cursorPos = 0;
    int selStart = 0;
    int selEnd = 0;
    int highlightedIndex = -1;
    std::string selectKeys;
    std::vector<std::pair<std::string, std::string>> candidates; // text, comment
    bool asciiMode = false;
    bool fullShape = false;
  };

  bool ensureSession();
  EngineState currentState(bool* engineOk);
  bool processKey(const KeyEventData& e, bool isUp, bool* ret);
  void buildResponse(const EngineState& state, bool allowCommit,
                     Json::Value& resp);
  bool shouldPassThroughModifierOnKey(const KeyEventData& e, bool filterRet);
  // 数字/句点原生直通（bug#20）：见 .cpp 注释。返回 true = 已作答。
  bool tryNativeDigitPassthrough(const KeyEventData& e, Json::Value& resp);
  bool schemaUsesDigitInput();
  void appendLangStatusChange(Json::Value& resp, bool ascii, bool full);
  void updateTrackedRawInput(const KeyEventData& e, UINT keyCode, UINT charCode, bool backendRet);
  void snapshotRawInputForCommit();
  bool handleSecondSelectionFilterDown(const KeyEventData& e, Json::Value& resp);
  bool handleSecondSelectionDown(const KeyEventData& e, Json::Value& resp);
  bool handleSecondSelectionFilterUp(const KeyEventData& e, Json::Value& resp);
  bool handleSecondSelectionUp(const KeyEventData& e, Json::Value& resp);
  bool readInputModes(bool* asciiMode, bool* fullShape);
  bool refreshPageSize();
  // 总候选数变更落在组字中时（候选窗正开着点菜单改数），refreshPageSize
  // 的 Schema 重建会清空上下文，只能跳过——置 pendingPageSize_，下一个
  // 按键入口（filterKeyDown/onKeyDown）见组字已空时补做。没有补做的话
  // "由小变大"永久失效（引擎页长停在旧值，显示侧钳制只帮得了变小）。
  void maybeFlushPendingPageSize();

  rimeapi::SessionId sessionId_ = 0;
  int candidateCount_ = 5;
  bool pendingPageSize_ = false;
  bool semicolonSelectSecond_ = false;
  ULONGLONG lastInitAttempt_ = 0;
  int lastKeyDownCode_ = 0;
  int lastKeySkip_ = 0;
  int lastKeyUpCode_ = 0;
  bool lastKeyDownRet_ = false;
  bool lastKeyUpRet_ = false;
  bool keyComposing_ = false;
  // Word 2016 丢句点修复（bug#20）：引擎刚直通提交单个数字时，紧跟的
  // 句点键不进引擎（rime 会代插 "."，Word 自动编号窗口期会丢掉这次代插），
  // 直接回 returnValue=0 让应用原生插入。DLL 侧有时序盲区（其标记要等
  // 提交落屏才置位，Word 恰恰压着不落），daemon 在引擎侧精确判定。
  bool lastCommitSingleDigit_ = false;
  bool periodPassthroughPending_ = false;
  // 数字/句点原生直通（bug#20 结构性修法，2026-11-05）：无组字时数字键
  // 干脆不进引擎，与微软/谷歌一致由应用原生插入。IME 代插走
  // ITfInsertAtSelection 编辑会话，Word 的自动编号/格式转换窗口期会静默
  // 丢掉这种插入（实测快打 "1. 2. 3." 只剩 ". . "）；原生插入无此问题。
  // 句点在"上一键是原生直通数字"时同样直通，"1." 全程不碰 IME。
  bool lastKeyWasNativeDigit_ = false;
  // filterKeyDown 已对某个数字/句点回了"未吃"：onKeyDown 必须对同一键回
  // 同样的结论（两阶段不一致会被宿主的 TSF 路径吃回，bug#20 首轮教训）。
  bool nativeDigitPassthroughPending_ = false;
  // 数字本身就是输入的方案（T9 九宫格）：整套直通关闭，数字照旧进引擎。
  bool schemaDigitChecked_ = false;
  bool schemaUsesDigitInput_ = false;
  int secondSelectConsumeKeyUpCode_ = 0;
  std::string selectKeysCache_;
  bool asciiKnown_ = false;
  bool asciiBefore_ = false;
  bool fullBefore_ = false;
  ContextualCommits contextualCommits_;
  std::string rawInputTracked_;
  std::string rawInputAtCommit_;
  std::vector<int> displayPerm_;
  std::vector<BYTE> keyStatesBuf_; // KeyEventData backing store per call
  std::atomic<bool> userDictDirty_{false};

  // ---- AI 状态（对齐 Go：seq 丢弃过期结果 / pending 去重 / active 覆盖）----
  struct AIState {
    std::atomic<bool> pending{false};
    std::atomic<long long> requestSeq{0};
    bool active = false;
    std::string prompt;                  // 触发时的组字串（比对是否已变化）
    std::vector<std::string> candidates; // AI 结果（显示在候选窗顶部）
    int cursor = 0;
    std::string error;
    std::string previousCommit; // 上文（{{previous_commit}}）
    DaemonAI client;
  } aiState_;
  std::mutex aiResultLock_;
  // 置顶短语 / 超级简拼 overlay（Go custom_phrase/super_abbrev 等价）
  int overlayCustomCount_ = 0;
  std::vector<std::string> overlayCustomTexts_;
  std::string overlaySuperText_;
  std::string overlayCommitText_;
  int overlayConsumeKeyUpCode_ = 0;

  std::thread aiWorker_;
  ULONGLONG aiConfigLoadedTick_ = 0; // 配置节流（见 maybeTriggerAI）
  bool aiEnabledCached_ = false;

  // ---- 5e 云剪贴板列表选择器（Go cloud_clipboard.go 编排层对应实现）----
  struct CloudClipEntry {
    std::string name;
    std::string preview;
    std::string comment;  // 时间戳 mm-dd HH:mm
    std::string fullText;
  };
  struct CloudClipState {
    bool active = false;
    int cursor = 0;  // 全局索引
    int page = 0;    // 0 起
    std::vector<CloudClipEntry> entries;
  } cloudClip_;
  static constexpr int kCloudClipPageSize = 9;
  std::atomic<bool> cloudClipLoading_{false};
  std::thread cloudClipWorker_;

  // ---- 5e 云剪贴板列表选择器（Go cloud_clipboard.go 编排层对应实现）----
  // 云剪贴板激活期间的按键处理（选字/翻页/删除/退出）。返回 true=消费。
  bool handleCloudClipKeyDown(const KeyEventData& e, Json::Value& resp,
                              std::function<void(Json::Value)> push);
  bool handleCloudClipFilterDown(const KeyEventData& e, Json::Value& resp);
  bool handleCloudClipKeyUp(const KeyEventData& e, Json::Value& resp);
  void resetCloudClipState() { cloudClip_ = CloudClipState{}; }
  void fillCloudClipResponse(Json::Value& resp);
  void commitCloudClipEntry(Json::Value& resp, const CloudClipEntry& entry);
  bool isCloudClipHandledKey(const KeyEventData& e) const;
  bool isCloudClipHotkey(const KeyEventData& e) const;
};

} // namespace Moqi

#endif // MOQI_HELPER_ENGINE_H_

