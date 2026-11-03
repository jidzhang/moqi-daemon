#pragma once
// 5b 菜单纯核心：给定 MenuState 产出黄金形状的菜单树、按 id 执行命令并改状态。
// 不 include windows.h / rime —— 状态的采集与落盘由 daemon 侧组装器负责，
// 本层只做可单测的纯逻辑（行为黄金 = tests/fixtures/menu_golden.json，
// 2026-11-02 从 Go 后端实链抓取）。规格见 docs/5b-menu-port-spec.md。
#include <map>
#include <string>
#include <vector>

#include <json/json.h>

namespace Moqi {
namespace Menu {

// 命令 id 常量（与 Go rime.go 25-118 一致，改前先跑 daemon_menu_test）。
enum CommandIds {
  ID_MODE_ICON = 1,
  ID_ASCII_MODE = 2,
  ID_FULL_SHAPE = 3,
  ID_ASCII_PUNCT = 4,
  ID_TRADITIONALIZATION = 5,
  ID_DEPLOY = 10,
  ID_SYNC = 11,
  ID_SYNC_DIR = 12,
  ID_SHARED_DIR = 13,
  ID_USER_DIR = 14,
  ID_LOG_DIR = 16,
  ID_UPDATE_CONFIG = 17,
  ID_HELP_DOCS = 21,
  ID_DISCUSSIONS = 22,
  ID_DOWNLOAD_SCHEME_SET = 23,
  ID_LAUNCHER_DEBUG_LOG = 30,
  ID_LAUNCHER_RESTART_BACKENDS = 32,
  ID_LAUNCHER_QUIT = 33,
  ID_SCHEMA_BASE = 1000,
  ID_SWITCH_BASE = 2000,
  ID_SCHEME_SET_BASE = 3000,
  ID_APPEARANCE_INLINE_PREEDIT = 100,
  ID_APPEARANCE_FONT_8 = 113,
  ID_APPEARANCE_FONT_30 = 127,
  ID_APPEARANCE_BG_WHITE = 120,
  ID_APPEARANCE_BG_BLUE = 122,
  ID_APPEARANCE_HL_BLUE = 130,
  ID_APPEARANCE_HL_GREEN = 132,
  ID_APPEARANCE_TEXT_BLACK = 140,
  ID_APPEARANCE_TEXT_BLUE = 142,
  ID_APPEARANCE_HLTEXT_BLACK = 145,
  ID_APPEARANCE_HLTEXT_BLUE = 147,
  ID_APPEARANCE_THEME_BASE = 4000,
  ID_APPEARANCE_FONT_FAMILY_SEGOE_UI = 194,
  ID_APPEARANCE_FONT_FAMILY_SIMSUN = 197,
  ID_APPEARANCE_COMMENT_FONT_FAMILY_CONSOLAS = 198,
  ID_APPEARANCE_COMMENT_FONT_FAMILY_SIMSUN = 201,
  ID_APPEARANCE_LAYOUT_VERTICAL = 170,
  ID_APPEARANCE_LAYOUT_HORIZONTAL = 171,
  ID_APPEARANCE_PER_ROW_3 = 180,
  ID_APPEARANCE_PER_ROW_9 = 183,
  ID_APPEARANCE_SPACING_0 = 184,
  ID_APPEARANCE_SPACING_50 = 189,
  ID_APPEARANCE_CAND_COUNT_3 = 190,
  ID_APPEARANCE_CAND_COUNT_9 = 193,
  ID_APPEARANCE_IMPORT_SKIN = 215,
  ID_SHARED_INPUT_STATE = 210,
  ID_INPUT_AUTO_PAIR_QUOTES = 220,
  ID_INPUT_SEMICOLON_SELECT_SECOND = 221,
  // daemon 专属新增（Go 无此命令）：打开 ai_config.json。外观 213-225 之外
  // 的独立号，须同时列入 isLocalCommandId 的显式集合。
  ID_AI_CONFIG = 226,
  ID_WEBDAV_SETTINGS = 5004,
  ID_CLOUD_CLIPBOARD_TEST = 5003,
  ID_CLOUD_CLIPBOARD_ENABLED = 5002,
  ID_CLOUD_CLIPBOARD_SETTINGS = 5001,
};

struct MenuSwitchDef {
  std::string name;
  std::vector<std::string> states; // 0/1 两个状态文案
};

struct SchemaEntry {
  std::string id;
  std::string name;
  bool current = false;
};

struct ThemeEntry {
  std::string id;   // 归一化小写
  std::string label;
};

// 全部菜单输入。采集器从 rime API / appearance_config.json / 文件扫描填充；
// 单测直接从 fixtures 构造。
struct MenuState {
  std::vector<MenuSwitchDef> switches;   // save_options 顺序（已过滤到 schema 开关）
  std::map<std::string, bool> options;   // rimeGetOption 快照
  std::vector<SchemaEntry> schemas;
  std::vector<ThemeEntry> themes;

  // appearance_config.json 的菜单可见字段
  std::string theme;                     // candidate_theme（小写归一比较）
  bool inlinePreedit = false;
  bool horizontal = true;                // 候选排列
  int perRow = 7;                        // 生效值（min(配置, 总候选)）
  int perRowRaw = 9;                     // 配置原值（3/5/7/9）——落盘写这个，
                                         // 否则总候选改大后每行数跟着缩水
  int spacing = 20;
  int candCount = 7;
  int fontPoint = 14;
  int commentFontPoint = 12;
  std::string fontFace = "Microsoft YaHei UI";
  std::string commentFontFace = "Consolas";
  std::string bgColor = "#ffffff";
  std::string hlColor = "#c6ddf9";
  std::string textColor = "#000000";
  std::string hlTextColor = "#000000";
  std::string commentColor = "#000000";
  std::string commentHlColor = "#000000";

  bool inputStateShared = false;
  bool autoPairQuotes = false;
  bool semicolonSecond = false;
  bool cloudEnabled = false;
  bool launcherDebug = false;
};

// 产出 menuItems 数组（形状/顺序与 Go buildMenu 一致；叶子也带 "submenu": []，
// 与黄金逐字节对齐）。
Json::Value buildMenu(const MenuState& state);

// 开关文案：states 两个时 enabled -> "states[1] → states[0]"，
// 否则 "states[0] → states[1]"（与 Go switchMenuText 一致）。
std::string switchMenuText(const MenuSwitchDef& sw, bool enabled);

// 命令分发。Local = daemon 已接管（状态已被修改，调用方负责落盘/广播）；
// Proxy = 仍走 Go 子进程（状态不动）。
enum class CmdAction { Local, Proxy };

struct CommandResult {
  CmdAction action = CmdAction::Proxy;
  bool ok = false;                    // Local 时命令是否生效
  Json::Value customizeUi;            // 需要随响应下发的 UI 更新（可空）
  std::string optionToggled;          // 本地开关命令改动的 rime 选项名（应用层执行）
  std::string schemaSelected;         // 本地方案命令选中的 schema id（应用层执行）
};

CommandResult handleCommand(int id, MenuState& state);

// main 循环用：该 id 是否本地接管（不查动态表，只看静态集合）。
bool isLocalCommandId(int id);

} // namespace Menu
} // namespace Moqi
