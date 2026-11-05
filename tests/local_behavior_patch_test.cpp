// local_behavior_patch_test —— 产品行为补丁体回归（10-26 事故的回归锚）。
// 事故：白盒考卷 yaml 与产品配置脱节（abbrev_sentences 残留），d3 假摔 8 点。
// 本测试锁定：空配置绝不产生任何"毒键"；每个开关的输出行精确匹配。
// include 纪律：jsoncpp 先于裸 STL（MSVC <cmath> 崩，见 ChildProxy.h）。
#include <json/json.h>

#include "LocalBehaviorPatch.h"

#include <cstdio>
#include <string>

using Moqi::buildLocalBehaviorPatchBody;

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

static bool contains(const std::string& hay, const char* needle) {
  return hay.find(needle) != std::string::npos;
}

static Json::Value obj(const char* json) {
  Json::Value v;
  Json::CharReaderBuilder b;
  std::string errs;
  Json::CharReaderBuilder rb;
  std::unique_ptr<Json::CharReader> r(rb.newCharReader());
  r->parse(json, json + std::strlen(json), &v, &errs);
  return v;
}

// 产品默认（无开关、grammar=essay、mixed_pair 默认开）的完整期望体。
static const char* kProductBody =
    "# managed by moqi local engine\n"
    "patch:\n"
    "  translator/disable_user_dict_for_patterns: [\".*[bcdfhjklmpqstvwxyz]$\"]\n"
    "  grammar/language: zh-hans-t-essay-bgw\n"
    "  translator/contextual_suggestions: true\n"
    "  grammar/non_collocation_penalty: -4\n"
    "  melt_eng/initial_quality: 0.01\n"
    "  reduce_english_filter/mode: all\n"
    "  translator/sentence_quality_boost: -2\n"
    "  translator/enable_transpose: true\n"
    "  translator/transpose_credibility: -8\n"
    "  translator/enable_deletion: true\n"
    "  translator/deletion_credibility: -8\n"
    "  translator/enable_substitution: true\n"
    "  translator/substitution_credibility: -8\n"
    "  translator/enable_insertion: true\n"
    "  translator/insertion_credibility: -8\n"
    "  translator/typo_second_pass: true\n"
    "  translator/typo_second_pass_max_input_length: 12\n"
    "  translator/typo_second_pass_insert_after: 5\n";

int main() {
  const Json::Value empty = obj("{}");
  const Json::Value emptyCtx = obj("{}");
  const std::string essay = "zh-hans-t-essay-bgw";

  // --- 事故回归锚：空配置 + 无 gram = 只有 mixed_pair 守卫一行（不是空体）---
  // 注意：mixed_pair 默认开，即使无 gram 也会 emit disable_user_dict_for_patterns
  // 空体只在 mixed_pair 显式关闭且无 gram 且无任何触发器时才出现。
  {
    const std::string body =
        buildLocalBehaviorPatchBody(empty, emptyCtx, "");
    check(contains(body, "disable_user_dict_for_patterns"),
          "empty cfg + no grammar -> only mixed_pair guard (1 line)");
    check(!contains(body, "abbrev"), "no abbrev_sentences ever (10-26 incident)");
    check(!contains(body, "grammar/language"),
          "no grammar/language line when lang empty");
    // 显式关 mixed_pair + 无 gram + 无触发器 = 真空体
    const Json::Value ctxOff = obj(
        "{\"bigram_rerank\":{\"mixed_pair\":false}}");
    const std::string empty2 =
        buildLocalBehaviorPatchBody(empty, ctxOff, "");
    check(empty2.empty(), "all off -> truly empty body");
  }

  // --- 产品默认体逐字节锁定 ---
  {
    const std::string body =
        buildLocalBehaviorPatchBody(empty, emptyCtx, essay);
    check(body == kProductBody, "product default body byte-exact");
  }

  // --- 触发器开关 ---
  {
    const Json::Value cfg = obj(
        "{\"disable_slash_punct\":true,\"disable_unicode_trigger\":true,"
        "\"disable_number_trigger\":true,\"disable_lunar_trigger\":true,"
        "\"disable_calculator\":true,\"disable_aux_lookup\":true}");
    const std::string body =
        buildLocalBehaviorPatchBody(cfg, emptyCtx, "");
    check(contains(body, "recognizer/patterns/punct: null"), "slash punct nulled");
    check(!contains(body, "^v([0-9]"), "v-only pattern absent when fully disabled");
    check(contains(body, "recognizer/patterns/unicode: null"), "unicode nulled");
    check(contains(body, "recognizer/patterns/number: null"), "number nulled");
    check(contains(body, "recognizer/patterns/gregorian_to_lunar: null"),
          "lunar nulled");
    check(contains(body, "recognizer/patterns/calculator: null"),
          "calculator nulled");
    check(contains(body, "engine/translators/-: table_translator@frost_aux"),
          "aux translator removed");
    check(contains(body, "engine/filters/-: lua_filter@*aux_lookup_filter"),
          "aux filter removed");
    check(contains(body, "translator/disable_user_dict_for_patterns"),
          "mixed-pair learning guard still on by default");
  }

  // --- slash_punct_v_only（/ 半角直通方案）---
  {
    const Json::Value cfg = obj("{\"slash_punct_v_only\":true}");
    const std::string body =
        buildLocalBehaviorPatchBody(cfg, emptyCtx, "");
    check(contains(body, "^v([0-9]|10|[A-Za-z]+)$"), "v-only punct pattern");
    check(!contains(body, "patterns/punct: null"), "not fully nulled");
  }

  // --- mixed_pair 关闭：不再禁用简拼尾字母学习 ---
  {
    const Json::Value ctx = obj(
        "{\"bigram_rerank\":{\"mixed_pair\":false}}");
    const std::string body =
        buildLocalBehaviorPatchBody(empty, ctx, "");
    check(!contains(body, "disable_user_dict_for_patterns"),
          "mixed_pair off -> no user dict guard");
  }

  // --- wanxiang 罚分为 0；显式手填优先且越界回退 -4 ---
  {
    const std::string body =
        buildLocalBehaviorPatchBody(empty, emptyCtx, "wanxiang-lts-zh-hans");
    check(contains(body, "grammar/non_collocation_penalty: 0"),
          "wanxiang penalty 0");
    const Json::Value cfg = obj("{\"grammar_penalty\":-9.5}");
    const std::string body2 =
        buildLocalBehaviorPatchBody(cfg, emptyCtx, essay);
    check(contains(body2, "grammar/non_collocation_penalty: -9.5"),
          "explicit penalty honored");
    const Json::Value bad = obj("{\"grammar_penalty\":-99}");
    const std::string body3 =
        buildLocalBehaviorPatchBody(bad, emptyCtx, essay);
    check(contains(body3, "grammar/non_collocation_penalty: -4"),
          "out-of-range penalty falls back to -4");
  }

  // --- 容器被手改成数组/标量：按全默认走，绝不抛 ---
  // （jsoncpp 的 operator[]/get 在非对象容器上抛 Json::LogicError；这段
  // 跑在 TSF DLL 里，手改配置不能崩宿主进程。）
  {
    const Json::Value arr(Json::arrayValue);
    const Json::Value num(5);
    const std::string bodyA = buildLocalBehaviorPatchBody(arr, num, "");
    check(contains(bodyA, "disable_user_dict_for_patterns"),
          "array cfg/contextual -> default body, no throw");
    check(!contains(bodyA, "patterns/punct: null"),
          "array cfg -> all triggers default off");
  }

  std::printf("\n%d passed, %d failed\n", g_pass, g_failed);
  return g_failed == 0 ? 0 : 1;
}