// 2026-11-02 用户实报 shima 错位的引擎级回归测试（需要 x86 rime.dll 与
// 真实词库布局，跑在 C:\work\moqi-daemon-full 上；ctest 设好环境变量）。
//
// 不变式：候选窗"显示第 k 位"与"数字 k/空格提交的词"必须一致——包括
// typo_memory 提顶之后的第二轮输入（提顶曾不改 displayPerm，导致显示
// "是嘛"上屏"事吗"）。
//
// 流程：
//   1) shima → 数字选 5 → 上屏（typo_memory 记住 shima→该词）
//   2) 再输 shima → 候选首位被提顶 → 空格 → 必须提交显示首位
//   3) 再输 shima → 数字 2 → 必须提交显示第 2 位
#include "DaemonEngine.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace Moqi;

// DaemonEngine::ensureEngine 依赖 main.cpp 注入的布局变量（定义在
// DaemonEngine.cpp），测试里 extern 声明后赋值即可。
namespace Moqi {
extern std::wstring g_daemonProgramDir;
extern std::wstring g_daemonBackendDir;
// DaemonEngine 里的临时探针写日志用（测试目标不链接 main.cpp，本地定义）。
void daemonLogExtern(const std::wstring& message) {
  std::wcout << message << std::endl << std::flush;
}
void daemonLogDebugExtern(const std::wstring& message) {
  std::wcout << message << std::endl << std::flush;
}
}

namespace {

int failures = 0;

void check(bool cond, const std::string& what) {
  std::cout << (cond ? "PASS " : "FAIL ") << what << std::endl;
  if (!cond) ++failures;
}

std::vector<BYTE> keyStates() { return std::vector<BYTE>(256, 0); }

// 输入若干字母，返回最后一个字母键之后的"显示候选"快照（未提交）。
Json::Value typeDisplay(DaemonEngine& engine, const std::string& letters) {
  KeyEventData e{};
  std::vector<BYTE> states = keyStates();
  Json::Value filterResp;
  Json::Value last;
  for (char c : letters) {
    Json::Value resp;
    e.keyCode = static_cast<UINT>(c);
    e.charCode = static_cast<UINT>(c);
    e.scanCode = 0;
    e.isExtended = false;
    e.keyStates = states.data();
    // 真链形态：filterKeyDown（决定吞/放）先走，再 onKeyDown 出候选。
    engine.filterKeyDown(e, filterResp);
    engine.onKeyDown(e, resp);
    last = std::move(resp);
  }
  return last.get("candidateEntries", Json::Value(Json::arrayValue));
}

// 空格提交，返回 commitString。
std::string pressSpace(DaemonEngine& engine) {
  KeyEventData e{};
  std::vector<BYTE> states = keyStates();
  e.keyCode = 32;
  e.charCode = 32;
  e.keyStates = states.data();
  Json::Value filterResp;
  engine.filterKeyDown(e, filterResp);
  Json::Value resp;
  engine.onKeyDown(e, resp);
  engine.onCompositionTerminated(false);
  return resp.get("commitString", "").asString();
}

// 数字选择提交（提交后同样补终止通知），返回 commitString。
std::string pressDigit(DaemonEngine& engine, char digit) {
  KeyEventData e{};
  std::vector<BYTE> states = keyStates();
  e.keyCode = static_cast<UINT>(digit);
  e.charCode = static_cast<UINT>(digit);
  e.keyStates = states.data();
  Json::Value filterResp;
  engine.filterKeyDown(e, filterResp);
  Json::Value resp;
  engine.onKeyDown(e, resp);
  engine.onCompositionTerminated(false);
  return resp.get("commitString", "").asString();
}

std::string candAt(const Json::Value& cands, int idx) {
  if (!cands.isArray() || idx >= static_cast<int>(cands.size())) return "";
  return cands[idx].get("text", "").asString();
}

} // namespace

int main() {
  const wchar_t* program = _wgetenv(L"MOQI_PROGRAM_DIR");
  const wchar_t* dllDir = _wgetenv(L"MOQI_RIME_DLL_DIR");
  if (!program || !dllDir) {
    std::cout << "SKIP: MOQI_PROGRAM_DIR / MOQI_RIME_DLL_DIR not set"
              << std::endl;
    return 0; // 环境未配（如纯单测机）不算失败
  }
  // 与 main.cpp 的注入一致：数据目录 = {program}\moqi-ime\input_methods\rime。
  // 此前漏设 backendDir（空串）导致解析成 {program}\input_methods\rime，
  // 布局不存在 → engine init 失败被当成断言失败。
  g_daemonProgramDir = program;
  g_daemonBackendDir = L"moqi-ime";
  const std::wstring dataDir =
      g_daemonProgramDir + L"\\" + g_daemonBackendDir +
      L"\\input_methods\\rime\\data";
  const DWORD attrs = ::GetFileAttributesW(dataDir.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
    std::wcout << L"SKIP: engine layout not present: " << dataDir
               << std::endl;
    return 0; // 布局不在（如纯单测机）不算失败
  }

  DaemonEngine engine;
  engine.ensureEngine();
  if (!engine.engineReady()) {
    std::cout << "FAIL engine init" << std::endl;
    return 1;
  }
  engine.onActivate();

  // 1) 学习：shima → 数字 5 上屏第 5 位 → typo_memory 记住 shima→该词
  Json::Value disp1 = typeDisplay(engine, "shima");
  const std::string learned = pressDigit(engine, '5');
  std::cout << "learned: " << learned << std::endl;
  check(!learned.empty(), "step1: committed something");

  // 2) 重输 shima：首位应提顶为 learned；空格 → 必须提交首位（THE FIX）
  Json::Value disp2 = typeDisplay(engine, "shima");
  const std::string top2 = candAt(disp2, 0);
  check(top2 == learned,
        "step2: hoisted first candidate == learned (" + top2 + ")");
  const std::string spaceCommit = pressSpace(engine);
  check(spaceCommit == top2,
        "step2: space commits displayed top (got " + spaceCommit + ")");

  // 3) 数字一致性：显示第 2 位 → 数字 2 提交第 2 位
  Json::Value disp3 = typeDisplay(engine, "shima");
  const std::string second3 = candAt(disp3, 1);
  const std::string digitCommit = pressDigit(engine, '2');
  check(digitCommit == second3,
        "step3: digit-2 commits displayed 2nd (got " + digitCommit + ")");

  std::cout << (failures == 0 ? "ENGINE-TEST PASS" : "ENGINE-TEST FAIL")
            << std::endl;
  return failures == 0 ? 0 : 1;
}
