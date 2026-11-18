// local_rerank_test —— 重排层纯逻辑回归（UTF-8 工具 + 短窗连贯）。
// 这两个是完全无依赖的纯函数，直接影响候选排序正确性。
// include 纪律：jsoncpp 先于裸 STL。
#include <json/json.h>

#include "LocalRerank.h"

#include <cstdio>
#include <string>
#include <vector>

using Moqi::Cand;
using Moqi::ContextualCommits;
using Moqi::RerankConfig;
using Moqi::RerankData;
using Moqi::utf8FirstRune;
using Moqi::utf8RuneCount;
using Moqi::utf8RuneSuffix;

static int g_pass = 0;
static int g_failed = 0;

static void check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (ok) {
    ++g_pass;
  } else {
    ++g_failed;
  }
}

int main() {
  // --- utf8RuneCount ---
  check(utf8RuneCount("") == 0, "runeCount empty");
  check(utf8RuneCount("abc") == 3, "runeCount ascii");
  check(utf8RuneCount("\xe4\xbd\xa0\xe5\xa5\xbd") == 2, "runeCount 你好=2");
  check(utf8RuneCount("a\xe4\xbd\xa0" "b") == 3, "runeCount mixed a你b=3");

  // --- utf8FirstRune ---
  check(utf8FirstRune("abc") == "a", "firstRune ascii");
  check(utf8FirstRune("\xe4\xbd\xa0\xe5\xa5\xbd") == "\xe4\xbd\xa0",
        "firstRune 你好=你");
  check(utf8FirstRune("") == "", "firstRune empty");

  // --- utf8RuneSuffix ---
  check(utf8RuneSuffix("\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c", 2) ==
            "\xe4\xb8\x96\xe7\x95\x8c",
        "suffix 你好世界 last2=世界");
  check(utf8RuneSuffix("abcdef", 3) == "def", "suffix abcdef last3=def");
  check(utf8RuneSuffix("ab", 5) == "ab", "suffix overflow returns all");
  check(utf8RuneSuffix("", 1) == "", "suffix empty");
  check(utf8RuneSuffix("abc", 0) == "", "suffix 0 runes = empty");

  // --- ContextualCommits（短窗连贯）---
  {
    ContextualCommits cc;
    std::string tail;
    check(!cc.activeTail(8, &tail), "no commits -> no active tail");

    cc.push("你好世界", 8);
    check(cc.activeTail(8, &tail) && tail == "你好世界",
          "fresh commit is active tail");

    cc.push("新句子", 8);
    check(cc.activeTail(8, &tail) && tail == "新句子",
          "latest commit wins");
  }

  {
    // 窗口过期（秒级时间戳——用极小窗口测试过期逻辑）
    ContextualCommits cc;
    std::string tail;
    cc.push("第一条", 0);  // 窗口 0 秒 = 立即过期
    check(!cc.activeTail(0, &tail), "0s window expires immediately");
  }

  // --- applyMixedPairRerank 覆盖闸门（2026-11-18 混拼首候选事故）---
  {
    // zaiwokl 生产实况：字级高频对（在|我 9708）过 floor，旧代码把丢弃
    // 简拼尾巴的短前缀候选「在我」(2字) 轮转到完整候选「在我看来」(4字)
    // 之上；闸门后更短的候选不得被提升。
    RerankConfig cfg;
    cfg.mixedPair = true;
    cfg.pairRatio = 999.0;
    cfg.pairFloor = 300;
    cfg.pairMaxLen = 4;
    cfg.pairWordRatio = 1.0;
    std::unordered_map<std::string, std::unordered_map<std::string, int>> pairs;
    pairs["在"]["我"] = 9708;
    pairs["是"]["的"] = 29010;
    RerankData::instance().injectForTest(cfg, pairs, {});
    {
      std::vector<Cand> cands = {{"在我看来", ""}, {"在我看", ""},
                                 {"在我", ""}, {"在握", ""}};
      int cursor = 0;
      const bool changed =
          Moqi::applyMixedPairRerank("zai wo k l", cands, cursor);
      check(!changed && cands[0].first == "在我看来",
            "mixedPair: shorter prefix (在我) must not overtake 在我看来");
    }
    {
      // 原设计场景（shid 同长度候选）必须保持提升。
      std::vector<Cand> cands = {{"时代", ""}, {"是的", ""}, {"使得", ""}};
      int cursor = 0;
      const bool changed = Moqi::applyMixedPairRerank("shid", cands, cursor);
      check(changed && cands[0].first == "是的",
            "mixedPair: equal-length promotion (是的) must stay");
    }
  }

  std::printf("\n%d passed, %d failed\n", g_pass, g_failed);
  return g_failed == 0 ? 0 : 1;
}