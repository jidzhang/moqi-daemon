// digit_passthrough_test —— 数字/句点原生直通（bug#20）纯逻辑回归。
// 锁定：T9 类方案识别 + speller/alphabet 含数字识别（这两个判定决定
// 数字键是否绕过引擎原生直通；判错=要么 T9 打不了字，要么 Word 丢句点复活）。
#include "DigitPassthrough.h"

#include <cstdio>
#include <string>

using Moqi::schemaIdLooksT9;
using Moqi::schemaYamlUsesDigitAlphabet;

static int g_failed = 0;

static void check(bool ok, const char* what) {
  if (!ok) {
    ++g_failed;
    std::printf("FAIL %s\n", what);
  } else {
    std::printf("PASS %s\n", what);
  }
}

int main() {
  // --- schemaIdLooksT9 ---
  check(schemaIdLooksT9("rime_frost_t9"), "t9 id lower");
  check(schemaIdLooksT9("rime_frost_T9"), "t9 id upper");
  check(schemaIdLooksT9("T9_abc"), "t9 prefix");
  check(!schemaIdLooksT9("rime_frost"), "plain frost id");
  check(!schemaIdLooksT9("rime_frost_double_pinyin_mspy"), "double pinyin id");
  check(!schemaIdLooksT9(""), "empty id");

  // --- schemaYamlUsesDigitAlphabet ---
  const std::string t9Yaml =
      "# rime_frost_t9\n"
      "speller:\n"
      "  alphabet: 23456789\n"
      "translator:\n"
      "  dictionary: rime_frost\n";
  check(schemaYamlUsesDigitAlphabet(t9Yaml), "t9 alphabet digits");

  const std::string pinyinYaml =
      "# rime_frost\n"
      "speller:\n"
      "  alphabet: zyxwvutsrqponmlkjihgfedcba\n"
      "  delimiter: \"'\"\n"
      "translator:\n"
      "  dictionary: rime_frost\n";
  check(!schemaYamlUsesDigitAlphabet(pinyinYaml), "pinyin alphabet no digit");

  // speller 段外的 alphabet 行不算
  const std::string outsideYaml =
      "speller:\n"
      "  alphabet: abcdefg\n"
      "custom:\n"
      "  alphabet: 123\n";
  check(!schemaYamlUsesDigitAlphabet(outsideYaml), "alphabet outside speller ignored");

  // 没有 speller 段
  check(!schemaYamlUsesDigitAlphabet("translator:\n  dictionary: x\n"),
        "no speller section");

  // 值后同行注释带数字 → 命中（保守方向：宁可禁用直通）
  const std::string commentYaml =
      "speller:\n"
      "  alphabet: abcdefg  # 9键\n";
  check(schemaYamlUsesDigitAlphabet(commentYaml),
        "digit in trailing comment counts (conservative)");

  // 多个 speller 段（第二个含数字）——正常方案只有一个，防御性用例
  const std::string twoSpellers =
      "speller:\n"
      "  alphabet: abc\n"
      "speller:\n"
      "  alphabet: 789\n";
  check(schemaYamlUsesDigitAlphabet(twoSpellers), "second speller digits caught");

  // 空文本
  check(!schemaYamlUsesDigitAlphabet(""), "empty yaml");

  if (g_failed == 0) {
    std::printf("14 passed, 0 failed\n");
    return 0;
  }
  std::printf("%d FAILED\n", g_failed);
  return 1;
}