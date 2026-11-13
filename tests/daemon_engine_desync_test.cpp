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
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace Moqi;

// DaemonEngine::ensureEngine 依赖 main.cpp 注入的布局变量（定义在
// DaemonEngine.cpp），测试里 extern 声明后赋值即可。
namespace Moqi {
extern std::wstring g_daemonProgramDir;
extern std::wstring g_daemonBackendDir;
// Synthetic rows exercise final display mapping without changing dictionaries.
struct DaemonEngineContractProbe {
  static void enableSyntheticAI(DaemonEngine& engine) {
    engine.aiState_.active = true;
    engine.aiState_.cursor = 0;
    engine.aiState_.candidates = {"SyntheticAIContract"};
    bool ok = false;
    const auto state = engine.currentState(&ok);
    Json::Value resp;
    engine.buildResponse(state, false, resp);
  }
  static std::string displayedBackend(DaemonEngine& engine, int index) {
    bool ok = false;
    const auto state = engine.currentState(&ok);
    Json::Value resp;
    engine.buildResponse(state, false, resp);
    return resp["candidateEntries"][index + 1]["text"].asString();
  }
  static void explicitPolicy(DaemonEngine& engine, bool enabled) {
    engine.explicitMemoryOnly_ = enabled;
  }
  static bool explicitPolicyRejectsPartial(DaemonEngine& engine) {
    engine.explicitMemoryOnly_ = true;
    const std::string raw = "zzpartialpolicy";
    engine.explicitMemoryRaw_ = raw;
    engine.explicitMemoryText_ = "PartialTarget";
    DaemonEngine::EngineState state;
    state.commit = "PartialTarget";
    state.composition = "remaining";
    Json::Value resp;
    engine.buildResponse(state, true, resp);
    const bool rejected = TypoMemoryStore::instance().lookup(raw).empty();
    engine.explicitMemoryOnly_ = false;
    return rejected;
  }
  static bool explicitPolicyRejectsDefault(DaemonEngine& engine) {
    engine.explicitMemoryOnly_ = true;
    const std::string raw = "zzexplicitpolicy";
    TypoMemoryStore::instance().record(raw, "ExplicitTarget");
    engine.rawInputTracked_ = raw;
    DaemonEngine::EngineState state;
    state.commit = "DefaultTarget";
    Json::Value resp;
    engine.buildResponse(state, true, resp);
    const bool kept = TypoMemoryStore::instance().lookup(raw) == "ExplicitTarget";
    engine.explicitMemoryOnly_ = false;
    return kept;
  }
  static bool ordinaryCommitOverwritesMemory(DaemonEngine& engine) {
    const std::string raw = "zzcontractmemory";
    TypoMemoryStore::instance().record(raw, "ChosenTarget");
    engine.rawInputTracked_ = raw;
    DaemonEngine::EngineState state;
    state.commit = "DefaultAcceptance";
    Json::Value resp;
    engine.buildResponse(state, true, resp);
    return TypoMemoryStore::instance().lookup(raw) == "DefaultAcceptance";
  }
  static bool dedupMapsDisplayedRows(DaemonEngine& engine) {
    DaemonEngine::EngineState state;
    state.composition = "aa";
    state.highlightedIndex = 2;
    state.candidates = {{"Alpha", ""}, {"Alpha", ""}, {"Beta", ""}};
    Json::Value resp;
    engine.buildResponse(state, false, resp);
    const bool uniqueHighlight = resp["candidateEntries"].size() == 2 &&
           engine.displayPerm_ == std::vector<int>({0, 2}) &&
           resp["candidateCursor"].asInt() == 1;
    state.candidates = {{"Alpha", ""}, {"Beta", ""}, {"Beta", ""}};
    engine.buildResponse(state, false, resp);
    return uniqueHighlight && engine.displayPerm_ == std::vector<int>({0, 1}) &&
           resp["candidateCursor"].asInt() == 1;
  }
};
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

bool confirmsDisplayed(const Json::Value& response, const std::string& text) {
  if (text.empty()) return false;
  const std::string commit = response.get("commitString", "").asString();
  const std::string composition = response.get("compositionString", "").asString();
  // Rime's partial-syllable rows confirm a segment, not an entire input.
  return commit == text || (commit.empty() && composition.rfind(text, 0) == 0);
}

std::string candAt(const Json::Value& cands, int idx) {
  if (!cands.isArray() || idx >= static_cast<int>(cands.size())) return "";
  return cands[idx].get("text", "").asString();
}

} // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--verify-memory-reload") {
    const wchar_t* isolated = _wgetenv(L"MOQI_USER_DATA_DIR");
    if (!isolated || !*isolated) return 1;
    const std::string root = wideToUtf8(moqiAppDataDir().c_str());
    TypoMemoryStore::instance().ensureLoaded(root);
    const bool reloaded = TypoMemoryStore::instance().lookup("zzboundarypersist") ==
                          "BoundaryTarget";
    check(reloaded, "persistence: fresh process reloads boundary target");
    return reloaded ? 0 : 1;
  }
  const wchar_t* program = _wgetenv(L"MOQI_PROGRAM_DIR");
  const wchar_t* dllDir = _wgetenv(L"MOQI_RIME_DLL_DIR");
  if (!program || !dllDir) {
    std::wcout << L"SKIP: MOQI_PROGRAM_DIR / MOQI_RIME_DLL_DIR not set"
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
  // rime.dll 也必须在（x86 版在安装根）：只有数据树没有 dll 的机器上，
  // engine init 必失败——那不是回归，应跳过而不是红。
  const std::wstring dllPath = std::wstring(dllDir) + L"\\rime.dll";
  if (::GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
    std::wcout << L"SKIP: rime.dll not present: " << dllPath << std::endl;
    return 0;
  }
  // 用户目录必须重定向（MOQI_USER_DATA_DIR 由 ctest 注入）：真用户词典
  // 绝不让测试进程碰——学习写入会污染真人候选顺序，且 rime 的 LevelDB
  // userdb 非并发安全（2026-11-05 实证测试进程曾对活词典跑 RepairDB）。
  const wchar_t* userDataDir = _wgetenv(L"MOQI_USER_DATA_DIR");
  if (!userDataDir || !*userDataDir) {
    std::wcout << L"FAIL: MOQI_USER_DATA_DIR not set "
                  L"(refusing to touch the real user dictionary)"
               << std::endl;
    return 1;  // ctest 无条件注入此变量；走到这里=测试装配坏了，要红不能绿
  }
  ::CreateDirectoryW(userDataDir, nullptr);  // 首次运行时目录可能不存在
  const std::filesystem::path isolatedConfig =
      std::filesystem::path(userDataDir) / L"Moqi";
  check(std::filesystem::path(moqiAppDataDir()) == isolatedConfig,
        "isolation: shared configuration is under test root");
  if (failures != 0) return 1;
  std::filesystem::create_directories(isolatedConfig);
  // Reset only this test's fixture, so repeated runs start without overlays.
  std::ofstream(isolatedConfig / L"moqi_custom_phrase.txt", std::ios::binary);

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

  // Mouse selection must resolve the same display permutation as digit keys.
  Json::Value clickDisplay = typeDisplay(engine, "shima");
  const std::string clickTop = candAt(clickDisplay, 0);
  Json::Value invalidResp;
  check(!engine.selectCandidate(-1, invalidResp), "mouse: reject negative index");
  check(!engine.selectCandidate(static_cast<int>(clickDisplay.size()), invalidResp),
        "mouse: reject first hidden index");
  check(clickTop == learned, "mouse: memory-hoisted candidate is first");
  Json::Value clickResp;
  check(engine.selectCandidate(0, clickResp), "mouse: select displayed first");
  check(clickResp.get("commitString", "").asString() == clickTop,
        "mouse: click commits displayed first");
  engine.onCompositionTerminated(false);

  Json::Value highlightedDisplay = typeDisplay(engine, "shima");
  const std::string highlightedSecond = candAt(highlightedDisplay, 1);
  Json::Value highlightedResp;
  check(engine.highlightCandidate(1, highlightedResp),
        "highlight: move to displayed second");
  check(highlightedResp.get("candidateCursor", -1).asInt() == 1,
        "highlight: cursor corresponds to displayed second");
  check(pressSpace(engine) == highlightedSecond,
        "highlight: space commits highlighted displayed second");

  Json::Value arrowDisplay = typeDisplay(engine, "shima");
  KeyEventData arrow{};
  std::vector<BYTE> arrowStates = keyStates();
  arrow.keyCode = VK_DOWN;
  arrow.keyStates = arrowStates.data();
  Json::Value arrowFilter, arrowResp;
  engine.filterKeyDown(arrow, arrowFilter);
  check(arrowFilter.get("returnValue", 0).asInt() == 1,
        "arrow: filter consumes Down before onKeyDown");
  engine.onKeyDown(arrow, arrowResp);
  check(arrowResp.get("candidateCursor", -1).asInt() == 1,
        "arrow: Down moves to displayed second");
  Json::Value arrowUpFilter, arrowUpResp;
  engine.filterKeyUp(arrow, arrowUpFilter);
  engine.onKeyUp(arrow, arrowUpResp);
  check(arrowUpResp.get("candidateCursor", -1).asInt() == 1,
        "arrow: key-up preserves displayed second");
  check(pressSpace(engine) == candAt(arrowDisplay, 1),
        "arrow: Down then Space commits displayed second");

  // 3) 数字一致性：显示第 2 位 → 数字 2 提交第 2 位
  Json::Value disp3 = typeDisplay(engine, "shima");
  const std::string second3 = candAt(disp3, 1);
  const std::string digitCommit = pressDigit(engine, '2');
  check(digitCommit == second3,
        "step3: digit-2 commits displayed 2nd (got " + digitCommit + ")");

  // Restore a known non-identity ordering before testing the second shortcut.
  typeDisplay(engine, "shima");
  pressDigit(engine, '5');
  Json::Value shortcutDisplay = typeDisplay(engine, "shima");
  const std::string shortcutSecond = candAt(shortcutDisplay, 1);
  engine.setSemicolonSelectSecond(true);
  KeyEventData shortcut{};
  std::vector<BYTE> shortcutStates = keyStates();
  shortcut.keyCode = 0xBA; // VK_OEM_1
  shortcut.charCode = ';';
  shortcut.keyStates = shortcutStates.data();
  Json::Value shortcutFilter, shortcutResp;
  engine.filterKeyDown(shortcut, shortcutFilter);
  engine.onKeyDown(shortcut, shortcutResp);
  check(shortcutResp.get("commitString", "").asString() == shortcutSecond,
        "semicolon: commits displayed second");
  Json::Value shortcutUpFilter, shortcutUpResp;
  engine.filterKeyUp(shortcut, shortcutUpFilter);
  engine.onKeyUp(shortcut, shortcutUpResp);
  check(shortcutUpResp.get("returnValue", 0).asInt() == 1,
        "semicolon: key-up remains consumed");
  engine.onCompositionTerminated(false);
  check(DaemonEngineContractProbe::dedupMapsDisplayedRows(engine),
        "dedup: displayed Alpha/Beta map to engine 0/2 and cursor 1");

  // Fixtures live only in the mandatory isolated root; never user settings.
  const std::filesystem::path configRoot = moqiAppDataDir();
  std::filesystem::create_directories(configRoot);
  auto writePhrase = [&configRoot](const std::string& text) {
    std::ofstream out(configRoot / L"moqi_custom_phrase.txt", std::ios::binary);
    out << text << "\tshima\t100\n";
    out.close();
    check(!out.fail(), "overlay: write isolated phrase fixture");
  };
  writePhrase("UniqueOverlayFixture");
  Json::Value overlayDisplay = typeDisplay(engine, "shima");
  check(candAt(overlayDisplay, 0) == "UniqueOverlayFixture",
        "overlay: custom phrase occupies first slot");
  check(pressDigit(engine, '2') == candAt(overlayDisplay, 1),
        "overlay: digit 2 commits displayed backend row");
  Json::Value overlayClickDisplay = typeDisplay(engine, "shima");
  Json::Value overlayClickResp;
  check(engine.selectCandidate(1, overlayClickResp),
        "overlay: click backend row after custom prefix");
  check(overlayClickResp.get("commitString", "").asString() ==
            candAt(overlayClickDisplay, 1),
        "overlay: click commits displayed backend row");
  engine.onCompositionTerminated(false);

  // Duplicate a real backend candidate: removing it must retain engine index.
  writePhrase(candAt(overlayClickDisplay, 1));
  Json::Value duplicateDisplay = typeDisplay(engine, "shima");
  check(pressDigit(engine, '2') == candAt(duplicateDisplay, 1),
        "overlay duplicate: digit 2 skips removed backend duplicate");
  Json::Value customDisplay = typeDisplay(engine, "shima");
  check(pressSpace(engine) == candAt(customDisplay, 0),
        "overlay: space commits custom first row");

  // No network request: inject a result to exercise AI cursor lock scope.
  typeDisplay(engine, "shima");
  DaemonEngineContractProbe::enableSyntheticAI(engine);
  Json::Value aiFilter, aiResp;
  engine.filterKeyDown(arrow, aiFilter);
  engine.onKeyDown(arrow, aiResp);
  check(aiResp.get("candidateCursor", -1).asInt() == 1,
        "AI: Down returns without recursive-lock deadlock");
  const std::string aiBackend =
      DaemonEngineContractProbe::displayedBackend(engine, 0);
  check(pressDigit(engine, '2') == aiBackend,
        "AI: digit 2 commits displayed backend row");
  typeDisplay(engine, "shima");
  DaemonEngineContractProbe::enableSyntheticAI(engine);
  const std::string aiClickBackend =
      DaemonEngineContractProbe::displayedBackend(engine, 0);
  Json::Value aiClickResp;
  check(engine.selectCandidate(1, aiClickResp), "AI: click backend row");
  check(aiClickResp.get("commitString", "").asString() == aiClickBackend,
        "AI: click commits displayed backend row");
  engine.resetAIState();
  engine.onCompositionTerminated(false);

  writePhrase(""); // No custom prefix during page-size and pagination tests.
  for (int count : {3, 5, 6, 7, 8, 9}) {
    engine.applyCandidateCount(count);
    Json::Value countDisplay = typeDisplay(engine, "shima");
    check(countDisplay.size() <= static_cast<Json::ArrayIndex>(count),
          "page size: displayed rows do not exceed configured count");
    const int last = static_cast<int>(countDisplay.size()) - 1;
    Json::Value countResp;
    check(last >= 0 && engine.selectCandidate(last, countResp),
          "page size: last visible row is selectable");
    std::cout << "COUNT=" << count << " expected=" << candAt(countDisplay, last)
              << " commit=" << countResp.get("commitString", "").asString()
              << " composing=" << countResp.get("compositionString", "").asString()
              << std::endl;
    check(confirmsDisplayed(countResp, candAt(countDisplay, last)),
          "page size: last visible click commits or confirms displayed segment");
    engine.onCompositionTerminated(false);

    typeDisplay(engine, "shima");
    Json::Value pageResp;
    const bool paged = engine.changePage(false, pageResp);
    check(paged, "pagination: next page is reachable");
    if (paged) {
      const auto pageDisplay = pageResp["candidateEntries"];
      check(pageResp.get("candidateCursor", -1).asInt() == 0,
            "pagination: next page starts at first visible row");
      Json::Value pageCommit;
      check(engine.selectCandidate(0, pageCommit),
            "pagination: first row of next page is selectable");
      std::cout << "PAGE count=" << count << " expected=" << candAt(pageDisplay, 0)
                << " commit=" << pageCommit.get("commitString", "").asString()
                << " composing=" << pageCommit.get("compositionString", "").asString()
                << std::endl;
      check(confirmsDisplayed(pageCommit, candAt(pageDisplay, 0)),
            "pagination: click commits or confirms next-page displayed segment");
    }
    engine.onCompositionTerminated(false);
  }
  engine.applyCandidateCount(5);

  // Diagnostic baseline, not a desired learning-policy contract. No gold is
  // inferred: this proves the current extra memory treats default commits as
  // authoritative replacements of a prior explicit target.
  check(DaemonEngineContractProbe::ordinaryCommitOverwritesMemory(engine),
        "learning diagnostic: ordinary commit replaces prior extra-memory target");

  check(DaemonEngineContractProbe::explicitPolicyRejectsDefault(engine),
        "learning experiment: default acceptance cannot overwrite explicit target");

  check(DaemonEngineContractProbe::explicitPolicyRejectsPartial(engine),
        "learning experiment: partial commit does not learn full raw");
  engine.onCompositionTerminated(false);
  DaemonEngineContractProbe::explicitPolicy(engine, true);
  const auto explicitDisplay = typeDisplay(engine, "shima");
  const auto explicitTarget = candAt(explicitDisplay, 1);
  check(pressDigit(engine, '2') == explicitTarget,
        "learning experiment: numeric explicit selection commits target");
  check(TypoMemoryStore::instance().lookup("shima") == explicitTarget,
        "learning experiment: numeric explicit selection is recorded");
  const auto explicitClickDisplay = typeDisplay(engine, "shima");
  Json::Value explicitClickResp;
  check(engine.selectCandidate(1, explicitClickResp),
        "learning experiment: mouse explicit selection succeeds");
  check(TypoMemoryStore::instance().lookup("shima") == candAt(explicitClickDisplay, 1),
        "learning experiment: mouse explicit selection is recorded");
  engine.onCompositionTerminated(false);
  const auto explicitSemicolonDisplay = typeDisplay(engine, "shima");
  Json::Value explicitSemicolonFilter, explicitSemicolonResp;
  engine.filterKeyDown(shortcut, explicitSemicolonFilter);
  engine.onKeyDown(shortcut, explicitSemicolonResp);
  check(explicitSemicolonResp.get("commitString", "").asString() ==
            candAt(explicitSemicolonDisplay, 1),
        "learning experiment: semicolon commits explicit target");
  check(TypoMemoryStore::instance().lookup("shima") ==
            candAt(explicitSemicolonDisplay, 1),
        "learning experiment: semicolon explicit selection is recorded");
  engine.filterKeyUp(shortcut, shortcutUpFilter);
  engine.onKeyUp(shortcut, shortcutUpResp);
  engine.onCompositionTerminated(false);
  DaemonEngineContractProbe::explicitPolicy(engine, false);

  // One update below the eight-update threshold must survive deactivation.
  // Read the file, not the singleton cache, to verify actual persistence.
  TypoMemoryStore::instance().flush();
  const std::string persistentRaw = "zzboundarypersist";
  TypoMemoryStore::instance().record(persistentRaw, "BeforeBoundary");
  TypoMemoryStore::instance().flush();
  TypoMemoryStore::instance().record(persistentRaw, "BoundaryTarget");
  {
    std::ifstream before(configRoot / L"typo_memory.tsv", std::ios::binary);
    const std::string oldDisk((std::istreambuf_iterator<char>(before)),
                             std::istreambuf_iterator<char>());
    check(oldDisk.find("\t" + persistentRaw + "\tBoundaryTarget\n") ==
              std::string::npos,
          "persistence: one update is not saved by the batch threshold");
  }
  engine.deactivate();
  std::ifstream persisted(configRoot / L"typo_memory.tsv", std::ios::binary);
  const std::string disk((std::istreambuf_iterator<char>(persisted)),
                         std::istreambuf_iterator<char>());
  check(disk.find("\t" + persistentRaw + "\tBoundaryTarget\n") != std::string::npos,
        "persistence: one pending update survives deactivate on disk");

  std::cout << (failures == 0 ? "ENGINE-TEST PASS" : "ENGINE-TEST FAIL")
            << std::endl;
  return failures == 0 ? 0 : 1;
}
