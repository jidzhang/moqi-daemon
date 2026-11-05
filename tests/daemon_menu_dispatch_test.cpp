// daemon_menu_dispatch_test —— 菜单命令分发边界回归（与 daemon_menu_test
// 互补：那边锁黄金树与主干命令，这边锁分支边界与历史坑）：
//   - 模式开关 1-5（ascii_mode/full_shape/ascii_punct/traditionalization）
//   - 开关/方案/主题越界号：isLocalCommandId 说本地但 handleCommand 不认
//   - 主题段上界（5000+ 是云/WebDAV，绝不落进主题分支）
//   - 竖排→横排 perRowRaw 兜底（竖排落盘 0/1，切横排回落 9）
//   - 每行数/间距/候选数/字体族/注释字号各表全命中
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
  s.switches = {{"sw0", {"a", "b"}}, {"sw1", {"c", "d"}}};
  s.options = {{"sw0", false}, {"sw1", true}, {"ascii_mode", false},
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
    check(r.action == CmdAction::Proxy && r.optionToggled.empty() &&
              s.options["sw0"] == false,
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
  {
    MenuState s = baseState();
    check(!isLocalCommandId(ID_CLOUD_CLIPBOARD_SETTINGS) &&
              !isLocalCommandId(ID_WEBDAV_SETTINGS),
          "range: 5001/5004 are not local");
    CommandResult r = handleCommand(ID_CLOUD_CLIPBOARD_SETTINGS, s);
    check(r.action == CmdAction::Proxy && s.theme == "theme-1" &&
              !r.customizeUi.isObject(),
          "cmd: 5001 does not fall into theme branch");
    // 4999 是最后一个主题号（4000+999，越界）
    r = handleCommand(4999, s);
    check(r.action == CmdAction::Proxy, "cmd: 4999 (beyond themes) proxy");
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
    // 横→竖：perRow 落 1
    MenuState s3 = baseState();
    handleCommand(ID_APPEARANCE_LAYOUT_VERTICAL, s3);
    check(!s3.horizontal, "cmd: horizontal->vertical");
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
    r = handleCommand(146, s);  // 高亮文字 白色 → 注释高亮跟随
    check(r.ok && s.hlTextColor == "#ffffff" && s.commentHlColor == "#ffffff",
          "cmd: highlight text color syncs comment highlight");
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
    r = handleCommand(ID_SHARED_INPUT_STATE, s);
    check(r.ok && s.inputStateShared, "cmd: shared input state toggles");
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
