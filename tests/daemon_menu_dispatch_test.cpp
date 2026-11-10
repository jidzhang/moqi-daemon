// daemon_menu_dispatch_test —— 菜单命令分发边界回归（与 daemon_menu_test
// 互补：那边锁黄金树与主干命令，这边锁分支边界与历史坑）：
//   - 模式开关 1-5（ascii_mode/full_shape/ascii_punct/traditionalization）
//   - 开关/方案/主题越界号：isLocalCommandId 说本地但 handleCommand 不认
//   - 主题段上界（5000+ 是云/WebDAV，绝不落进主题分支）
//   - 竖排→横排 perRowRaw 兜底（竖排落盘 0/1，切横排回落 9）
//   - 每行数/间距/候选数/字体族/注释字号各表全命中
//   - 档位 3/5/6/7/8/9（2026-11-10 扩档）+ 总数变更重算每行生效值
//   - 自定义颜色把主题切 custom + 注释色跟随
//   - 222 号历史冲突（打开成对符号设置，非注释字号 24）
//   - inlinePreedit / 分号选次 / 成对引号的 customizeUi 载荷
// include 纪律：jsoncpp 先于裸 STL。
#include <json/json.h>

#include <cstdio>
#include <string>

#include "DaemonMenuCore.h"

using namespace Moqi::Menu;

static int g_pass = 0;
static int g_fail = 0;

static void check(bool ok, const char* name) {
  if (ok) {
    ++g_pass;
    std::printf("PASS %s\n", name);
  } else {
    ++g_fail;
    std::printf("FAIL %s\n", name);
  }
}

static MenuState baseState() {
  MenuState s;
  s.options = {{"ascii_mode", false},
               {"full_shape", false}, {"ascii_punct", false},
               {"traditionalization", false}};
  s.schemas = {{"schema-a", "A", false}, {"schema-b", "B", true}};
  s.themes = {{"theme-0", "T0"}, {"theme-1", "T1"}};
  s.theme = "theme-1";
  return s;
}

int main() {
  // ---- 1. 模式开关 1-5 → 对应 rime 选项 ----
  {
    MenuState s = baseState();
    CommandResult r = handleCommand(ID_ASCII_MODE, s);
    check(r.action == CmdAction::Local && r.ok &&
              r.optionToggled == "ascii_mode" && s.options["ascii_mode"],
          "cmd: ID_ASCII_MODE toggles ascii_mode");
    r = handleCommand(ID_MODE_ICON, s);
    check(r.optionToggled == "ascii_mode" && s.options["ascii_mode"] == false,
          "cmd: ID_MODE_ICON toggles ascii_mode back");
    r = handleCommand(ID_FULL_SHAPE, s);
    check(r.optionToggled == "full_shape" && s.options["full_shape"],
          "cmd: ID_FULL_SHAPE toggles full_shape");
    r = handleCommand(ID_ASCII_PUNCT, s);
    check(r.optionToggled == "ascii_punct" && s.options["ascii_punct"],
          "cmd: ID_ASCII_PUNCT toggles ascii_punct");
    r = handleCommand(ID_TRADITIONALIZATION, s);
    check(r.optionToggled == "traditionalization" &&
              s.options["traditionalization"],
          "cmd: ID_TRADITIONALIZATION toggles traditionalization");
  }

  // ---- 2. 越界号：isLocal 认、handleCommand 不改状态不 Local ----
  {
    MenuState s = baseState();
    CommandResult r = handleCommand(ID_SWITCH_BASE + 999, s);
    check(r.action == CmdAction::Proxy && r.optionToggled.empty(),
          "cmd: out-of-range switch stays proxy, no state change");
    r = handleCommand(ID_SCHEMA_BASE + 999, s);
    check(r.action == CmdAction::Proxy && r.schemaSelected.empty(),
          "cmd: out-of-range schema stays proxy");
    r = handleCommand(ID_APPEARANCE_THEME_BASE + 999, s);
    check(r.action == CmdAction::Proxy && s.theme == "theme-1",
          "cmd: out-of-range theme stays proxy");
    // 100-225 段内未映射的号：isLocalCommandId=true（外观段整体本地），
    // handleCommand 落空回 Proxy——锁现状，防有人"顺手"扩段。
    check(isLocalCommandId(105), "range: unmapped 105 in local range");
    r = handleCommand(105, s);
    check(r.action == CmdAction::Proxy && !r.customizeUi.isObject(),
          "cmd: unmapped 105 is a no-op proxy");
  }

  // ---- 3. 主题段上界：5001+（云剪贴板/WebDAV）绝不进主题分支 ----
  // （5001-5004 全组 isLocalCommandId=false 由 daemon_menu_test C4c 锁定，
  // 这里只锁"落不进主题分支"这条。）
  {
    MenuState s = baseState();
    CommandResult r = handleCommand(ID_CLOUD_CLIPBOARD_SETTINGS, s);
    check(r.action == CmdAction::Proxy && s.theme == "theme-1" &&
              !r.customizeUi.isObject(),
          "cmd: 5001 does not fall into theme branch");
    // （主题号越界 = 4000+999，已由第 2 节的 out-of-range theme 检查锁定。）
  }

  // ---- 4. 竖排→横排 perRowRaw 兜底 ----
  {
    MenuState s = baseState();
    s.horizontal = false;
    s.perRowRaw = 1;  // 竖排落盘写 0/1，横排不可用
    s.perRow = 1;
    s.candCount = 7;
    CommandResult r = handleCommand(ID_APPEARANCE_LAYOUT_HORIZONTAL, s);
    check(r.action == CmdAction::Local && r.ok && s.horizontal &&
              s.perRowRaw == 9 && s.perRow == 7,
          "cmd: vertical->horizontal recovers perRowRaw 9");
    // 合法原值 3 保留：perRow = min(3, candCount=7) = 3
    MenuState s2 = baseState();
    s2.horizontal = false;
    s2.perRowRaw = 3;
    handleCommand(ID_APPEARANCE_LAYOUT_HORIZONTAL, s2);
    check(s2.perRowRaw == 3 && s2.perRow == 3,
          "cmd: horizontal keeps valid perRowRaw 3");
    // 横→竖只翻 horizontal；perRow 的落盘/显示值换算在 MenuService 层
    // （daemon_menu_test C5 已锁 vertical 分支主干）。
  }

  // ---- 5. 数值/字串外观表全命中 ----
  {
    MenuState s = baseState();
    CommandResult r = handleCommand(182, s);  // 每行 7
    check(r.action == CmdAction::Local && s.perRow == 7 && s.perRowRaw == 7 &&
              r.customizeUi["candPerRow"].asInt() == 7,
          "cmd: per-row 7 applies");
    r = handleCommand(185, s);  // 间距 10
    check(r.ok && s.spacing == 10 && r.customizeUi["candSpacing"].asInt() == 10,
          "cmd: spacing 10 applies");
    r = handleCommand(191, s);  // 候选数 5
    check(r.ok && s.candCount == 5, "cmd: cand count 5 applies");
    r = handleCommand(113, s);  // 字号 8
    check(r.ok && s.fontPoint == 8, "cmd: font 8 applies");
    r = handleCommand(213, s);  // 注释字号 8
    check(r.ok && s.commentFontPoint == 8 &&
              r.customizeUi["candCommentFontSize"].asInt() == 8,
          "cmd: comment font 8 applies");
    r = handleCommand(197, s);  // 字体族 宋体
    check(r.ok && s.fontFace == "SimSun" &&
              r.customizeUi["candFontName"].asString() == "SimSun",
          "cmd: font family SimSun applies");
    r = handleCommand(201, s);  // 注释字体族 宋体
    check(r.ok && s.commentFontFace == "SimSun" &&
              r.customizeUi["candCommentFontName"].asString() == "SimSun",
          "cmd: comment font family SimSun applies");
    // 2026-11-10：档位扩为 3/5/6/7/8/9——新增 id 148/149（每行 6/8）、
    // 150/151（总数 6/8）全命中。s 此时 perRowRaw=8（149 号已设）。
    r = handleCommand(148, s);  // 每行 6
    check(r.ok && s.perRow == 6 && s.perRowRaw == 6 &&
              r.customizeUi["candPerRow"].asInt() == 6,
          "cmd: per-row 6 applies");
    r = handleCommand(149, s);  // 每行 8
    check(r.ok && s.perRow == 8 && s.perRowRaw == 8,
          "cmd: per-row 8 applies");
    r = handleCommand(150, s);  // 总数 6 → 每行收敛 min(8,6)=6
    check(r.ok && s.candCount == 6 && s.perRow == 6,
          "cmd: cand count 6 applies");
    r = handleCommand(151, s);  // 总数 8 → 每行 min(8,8)=8
    check(r.ok && s.candCount == 8 && s.perRow == 8,
          "cmd: cand count 8 applies");
  }

  // ---- 5b. 总数变更必须重算每行生效值（2026-11-10 用户实报 bug）----
  {
    // 复现用户序列 7→5→9：改总数 9 时若不重算 perRow，命令响应下发的
    // candPerRow 是 stale 的 5，渲染每行 5 而重开菜单勾 9。
    MenuState s = baseState();
    s.candCount = 5;
    s.perRowRaw = 9;
    s.perRow = 5;
    CommandResult r = handleCommand(193, s);  // 总数 9
    check(r.ok && s.candCount == 9 && s.perRow == 9,
          "cmd: cand count 9 recomputes perRow");
    // 总数改小同样收敛：min(每行 9, 总数 3) = 3
    MenuState s2 = baseState();
    s2.candCount = 9;
    s2.perRowRaw = 9;
    s2.perRow = 9;
    handleCommand(190, s2);  // 总数 3
    check(s2.candCount == 3 && s2.perRow == 3,
          "cmd: cand count 3 recomputes perRow");
  }

  // ---- 5c. 总候选命令判定（main.cpp page-size 即时生效用）----
  {
    check(isCandidateCountCommand(190) && isCandidateCountCommand(193) &&
              isCandidateCountCommand(150) && isCandidateCountCommand(151),
          "cmd: cand-count ids recognized");
    check(!isCandidateCountCommand(148) && !isCandidateCountCommand(149) &&
              !isCandidateCountCommand(182) && !isCandidateCountCommand(999),
          "cmd: non cand-count ids rejected");
    check(isValidCandNum(3) && isValidCandNum(5) && isValidCandNum(6) &&
              isValidCandNum(7) && isValidCandNum(8) && isValidCandNum(9) &&
              !isValidCandNum(4) && !isValidCandNum(10),
          "cmd: cand tiers are 3/5/6/7/8/9");
    // 默认总数=5（2026-11-10 用户拍板，对齐微软/谷歌）
    MenuState s = baseState();
    check(s.candCount == 5, "state: default candCount is 5");
  }

  // ---- 6. 自定义颜色 → 主题切 custom + 注释色跟随 ----
  {
    MenuState s = baseState();
    s.commentColor = "#000000";
    CommandResult r = handleCommand(121, s);  // 背景 暖白
    check(r.ok && s.theme == "custom" && s.bgColor == "#fff7e8" &&
              r.customizeUi["candBackgroundColor"].asString() == "#fff7e8",
          "cmd: bg custom color switches theme");
    r = handleCommand(132, s);  // 高亮 浅绿
    check(r.ok && s.hlColor == "#d9f2e6", "cmd: highlight green applies");
    r = handleCommand(142, s);  // 正文 深蓝 → 注释色跟随
    check(r.ok && s.textColor == "#1d4ed8" && s.commentColor == "#1d4ed8" &&
              r.customizeUi["candCommentColor"].asString() == "#1d4ed8",
          "cmd: text color syncs comment color");
    // （高亮文字色→注释高亮跟随链路由 daemon_menu_test C4d 的 146 号锁定。）
  }

  // ---- 7. 222 历史冲突：打开成对符号设置（本地回执），不是注释字号 24 ----
  {
    MenuState s = baseState();
    const int before = s.commentFontPoint;
    CommandResult r = handleCommand(222, s);
    check(r.action == CmdAction::Local && r.ok &&
              s.commentFontPoint == before && !r.customizeUi.isObject(),
          "cmd: 222 is a bare local receipt, not comment font 24");
  }

  // ---- 8. 输入设置载荷 ----
  {
    MenuState s = baseState();
    CommandResult r = handleCommand(ID_APPEARANCE_INLINE_PREEDIT, s);
    check(r.ok && s.inlinePreedit &&
              r.customizeUi["inlinePreedit"].asBool(),
          "cmd: inline preedit toggles with ui payload");
    r = handleCommand(ID_INPUT_AUTO_PAIR_QUOTES, s);
    check(r.ok && s.autoPairQuotes &&
              r.customizeUi["autoPairQuotes"].asBool(),
          "cmd: auto pair quotes toggles with ui payload");
    r = handleCommand(ID_INPUT_SEMICOLON_SELECT_SECOND, s);
    check(r.ok && s.semicolonSecond && r.optionToggled.empty(),
          "cmd: semicolon second toggles locally");
    // （sharedInputState 主干由 daemon_menu_test C6 锁定。）
  }

  // ---- 9. ascii_mode 不在 options 快照里：不崩溃、仍上报选项名 ----
  {
    MenuState s = baseState();
    s.options.erase("ascii_mode");
    CommandResult r = handleCommand(ID_ASCII_MODE, s);
    check(r.action == CmdAction::Local && r.ok &&
              r.optionToggled == "ascii_mode" &&
              !s.options.count("ascii_mode"),
          "cmd: missing option snapshot survives toggle");
  }

  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
