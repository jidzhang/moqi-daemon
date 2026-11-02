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

#include <string>
#include <vector>
#include <json/json.h>

namespace Moqi {

class DaemonEngine {
public:
  DaemonEngine();

  void ensureEngine();     // one-shot runtime init (blocking, first client)
  bool engineReady() const;

  // 菜单服务需要按会话读选项/方案；引擎所有权仍在 DaemonEngine。
  rimeapi::SessionId sessionId() const { return sessionId_; }

  // 会话生命周期（onActivate = tryEngage 的无 TSF 版）。
  void onActivate();
  void deactivate();
  // 按键/候选方法与 LocalEngine 同签名（KeyEventData 输入）。
  bool filterKeyDown(const KeyEventData& e, Json::Value& resp);
  bool onKeyDown(const KeyEventData& e, Json::Value& resp);
  bool filterKeyUp(const KeyEventData& e, Json::Value& resp);
  bool onKeyUp(const KeyEventData& e, Json::Value& resp);
  bool highlightCandidate(int index, Json::Value& resp);
  bool selectCandidate(int index, Json::Value& resp);
  bool changePage(bool backward, Json::Value& resp);
  void onCompositionTerminated(bool forced);

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
};

} // namespace Moqi

#endif // MOQI_HELPER_ENGINE_H_
