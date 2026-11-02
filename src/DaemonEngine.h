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
  // 等价物；缺它候选窗要等下一次按键才刷新）。
  bool maybeTriggerAI(const KeyEventData& e,
                      std::function<void(Json::Value)> push);
  // 选中 AI 候选（index 为 AI 覆盖层的位次）。
  bool commitAICandidate(int index, Json::Value& resp);
  void resetAIState();
  bool aiActive() const { return aiState_.active; }
  bool aiPending() const { return aiState_.pending.load(); }

private:
  struct EngineState {
    bool ok = false;
    std::string commit;
    std::string composition;
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
  void appendLangStatusChange(Json::Value& resp, bool ascii, bool full);
  void updateTrackedRawInput(const KeyEventData& e, UINT keyCode, UINT charCode, bool backendRet);
  void snapshotRawInputForCommit();
  bool handleSecondSelectionFilterDown(const KeyEventData& e, Json::Value& resp);
  bool handleSecondSelectionDown(const KeyEventData& e, Json::Value& resp);
  bool handleSecondSelectionFilterUp(const KeyEventData& e, Json::Value& resp);
  bool handleSecondSelectionUp(const KeyEventData& e, Json::Value& resp);
  bool readInputModes(bool* asciiMode, bool* fullShape);
  bool refreshPageSize();

  rimeapi::SessionId sessionId_ = 0;
  int candidateCount_ = 7;
  bool semicolonSelectSecond_ = false;
  int lastKeyDownCode_ = 0;
  int lastKeySkip_ = 0;
  int lastKeyUpCode_ = 0;
  bool lastKeyDownRet_ = false;
  bool lastKeyUpRet_ = false;
  bool keyComposing_ = false;
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
  std::thread aiWorker_;
};

} // namespace Moqi

#endif // MOQI_HELPER_ENGINE_H_

