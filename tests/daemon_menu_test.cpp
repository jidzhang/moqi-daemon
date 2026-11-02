// 5b 菜单纯核心回归测试。
//
// 黄金 = tests/fixtures/menu_golden.json（2026-11-02 从 Go 后端实链抓取的
// 26 项全树），输入 = menu_state.json（从黄金反推的 MenuState 快照）。
// 断言 buildMenu(state) 与黄金逐项一致（id/text/checked/enabled/separator/
// submenu 全对齐），命令表断言 handleCommand 的分发与状态变更。
// 行为变化的正当流程：改 Go 侧行为 -> 重新抓黄金 -> 这里改断言 -> 再改核心。
#include "DaemonMenuCore.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "tests/fixtures"
#endif

static int g_fail = 0;
static int g_pass = 0;

static void check(bool cond, const char* what) {
  if (cond) {
    ++g_pass;
    std::printf("PASS %s\n", what);
  } else {
    ++g_fail;
    std::printf("FAIL %s\n", what);
  }
}

static Json::Value parseFile(const std::string& path) {
  Json::CharReaderBuilder builder;
  Json::Value value;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::printf("CANNOT OPEN %s\n", path.c_str());
    return value;
  }
  std::string errs;
  Json::parseFromStream(builder, in, &value, &errs);
  if (!errs.empty()) {
    std::printf("PARSE ERROR %s: %s\n", path.c_str(), errs.c_str());
  }
  return value;
}

static std::string fixturePath(const char* name) {
  return std::string(TEST_FIXTURES_DIR) + "/" + name;
}

// 深比较，返回首个差异描述（空=一致）。
static std::string diffJson(const Json::Value& a, const Json::Value& b,
                            const std::string& path) {
  if (a.type() != b.type()) {
    return path + ": type " + std::to_string((int)a.type()) + " vs " +
           std::to_string((int)b.type());
  }
  if (a.isObject()) {
    std::vector<std::string> names = a.getMemberNames();
    std::vector<std::string> namesB = b.getMemberNames();
    if (names != namesB) {
      std::string x, y;
      for (auto& n : names) x += n + ",";
      for (auto& n : namesB) y += n + ",";
      return path + ": keys [" + x + "] vs [" + y + "]";
    }
    for (auto& n : names) {
      std::string d = diffJson(a[n], b[n], path + "." + n);
      if (!d.empty()) return d;
    }
    return std::string();
  }
  if (a.isArray()) {
    if (a.size() != b.size()) {
      return path + ": size " + std::to_string(a.size()) + " vs " +
             std::to_string(b.size());
    }
    for (Json::ArrayIndex i = 0; i < a.size(); ++i) {
      std::string d = diffJson(a[i], b[i],
                               path + "[" + std::to_string(i) + "]");
      if (!d.empty()) return d;
    }
    return std::string();
  }
  if (a != b) {
    return path + ": value mismatch";
  }
  return std::string();
}

using namespace Moqi::Menu;

static MenuState loadState(const Json::Value& j) {
  MenuState s;
  for (const auto& sw : j["switches"]) {
    MenuSwitchDef def;
    def.name = sw["name"].asString();
    for (const auto& st : sw["states"]) def.states.push_back(st.asString());
    s.switches.push_back(def);
    s.options[def.name] = j["options"].get(def.name, false).asBool();
  }
  for (const auto& sc : j["schemas"]) {
    SchemaEntry e;
    e.id = sc["id"].asString();
    e.name = sc["name"].asString();
    e.current = sc.get("current", false).asBool();
    s.schemas.push_back(e);
  }
  for (const auto& ss : j["schemeSets"]) {
    SchemeSetEntry e;
    e.name = ss["name"].asString();
    e.current = ss.get("current", false).asBool();
    s.schemeSets.push_back(e);
  }
  for (const auto& th : j["themes"]) {
    ThemeEntry e;
    e.id = th["id"].asString();
    e.label = th["label"].asString();
    s.themes.push_back(e);
  }
  s.theme = j.get("theme", "").asString();
  s.inlinePreedit = j.get("inlinePreedit", false).asBool();
  s.horizontal = j.get("horizontal", true).asBool();
  s.perRow = j.get("perRow", 7).asInt();
  s.spacing = j.get("spacing", 20).asInt();
  s.candCount = j.get("candCount", 7).asInt();
  s.fontPoint = j.get("fontPoint", 14).asInt();
  s.commentFontPoint = j.get("commentFontPoint", 12).asInt();
  s.fontFace = j.get("fontFace", "Segoe UI").asString();
  s.commentFontFace = j.get("commentFontFace", "Consolas").asString();
  s.bgColor = j.get("bgColor", "#ffffff").asString();
  s.hlColor = j.get("hlColor", "#c6ddf9").asString();
  s.textColor = j.get("textColor", "#000000").asString();
  s.hlTextColor = j.get("hlTextColor", "#000000").asString();
  s.inputStateShared = j.get("inputStateShared", false).asBool();
  s.autoPairQuotes = j.get("autoPairQuotes", false).asBool();
  s.semicolonSecond = j.get("semicolonSecond", false).asBool();
  s.cloudEnabled = j.get("cloudEnabled", false).asBool();
  s.launcherDebug = j.get("launcherDebug", false).asBool();
  return s;
}

static Json::Value item(const Json::Value& menu, const std::string& text) {
  for (const auto& it : menu) {
    if (it.get("text", "").asString() == text) return it;
  }
  return Json::Value();
}

int main() {
  Json::Value golden = parseFile(fixturePath("menu_golden.json"));
  Json::Value stateJson = parseFile(fixturePath("menu_state.json"));
  check(!golden.isNull() && golden.isArray(), "fixtures load");
  if (golden.isNull() || stateJson.isNull()) {
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return 1;
  }

  MenuState state = loadState(stateJson);
  check(state.switches.size() == 9, "state: 9 switches loaded");
  check(state.schemas.size() == 8, "state: 8 schemas loaded");
  check(state.themes.size() == 14, "state: 14 themes loaded");

  // A. 黄金树逐项一致
  Json::Value menu = buildMenu(state);
  std::string d = diffJson(menu, golden, "menu");
  if (d.empty()) {
    check(true, "menu: identical to Go golden (26 items)");
  } else {
    check(false, "menu: identical to Go golden (26 items)");
    std::printf("  FIRST DIFF: %s\n", d.c_str());
  }

  // B. 开关文案语义
  MenuSwitchDef two{"x", {"关", "开"}};
  check(switchMenuText(two, false) == "关 → 开", "text: off shows states0→1");
  check(switchMenuText(two, true) == "开 → 关", "text: on shows states1→0");

  // C1. 开关命令：本地 + 选项翻转 + 重建后文案翻转
  MenuState s2 = state;
  CommandResult r = handleCommand(ID_SWITCH_BASE + 3, s2);
  check(r.action == CmdAction::Local && r.ok, "cmd: switch is local");
  check(r.optionToggled == "opt-2003", "cmd: switch names option");
  check(s2.options["opt-2003"] != state.options.at("opt-2003"),
        "cmd: switch flips option");
  {
    Json::Value rebuilt = buildMenu(s2);
    Json::Value before = item(menu, "");
    // 找到 2003 项
    Json::Value after;
    for (const auto& it : rebuilt) {
      if (it.get("id", -1).asInt() == ID_SWITCH_BASE + 3) after = it;
    }
    check(!after.isNull() && after["checked"].asBool() !=
                                  golden[ID_SWITCH_BASE - ID_SWITCH_BASE + 3]
                                      ["checked"]
                                      .asBool(),
          "cmd: switch flips menu checked");
  }

  // C2. 方案命令
  MenuState s3 = state;
  r = handleCommand(ID_SCHEMA_BASE + 1, s3);
  check(r.action == CmdAction::Local && r.ok && r.schemaSelected == "schema-1",
        "cmd: schema select is local");

  // C3. 外观命令：字体大小 20（id 119）
  MenuState s4 = state;
  r = handleCommand(119, s4);
  check(r.action == CmdAction::Local && r.ok && s4.fontPoint == 20,
        "cmd: font size 20 applies");
  check(r.customizeUi.isObject() &&
        r.customizeUi["candFontSize"].asInt() == 20,
        "cmd: font size emits customizeUi");

  // C4. 主题命令
  MenuState s5 = state;
  r = handleCommand(ID_APPEARANCE_THEME_BASE + 0, s5);
  check(r.action == CmdAction::Local && r.ok && s5.theme == "theme-0",
        "cmd: theme select applies");

  // C5. 竖排切换
  MenuState s6 = state;
  r = handleCommand(ID_APPEARANCE_LAYOUT_VERTICAL, s6);
  check(r.action == CmdAction::Local && r.ok && s6.horizontal == false,
        "cmd: vertical layout applies");

  // C6. 输入状态共享 / 输入设置
  MenuState s7 = state;
  r = handleCommand(ID_SHARED_INPUT_STATE, s7);
  check(r.action == CmdAction::Local && r.ok &&
            s7.inputStateShared != state.inputStateShared,
        "cmd: input state shared toggles");
  r = handleCommand(ID_INPUT_AUTO_PAIR_QUOTES, s7);
  check(r.action == CmdAction::Local && r.ok &&
            s7.autoPairQuotes != state.autoPairQuotes &&
            r.customizeUi.isMember("autoPairQuotes"),
        "cmd: auto pair quotes toggles + ui");

  // C7. 代理集合：状态必须原样
  MenuState s8 = state;
  const int proxyIds[] = {ID_DEPLOY, ID_SYNC, ID_UPDATE_CONFIG,
                          ID_DOWNLOAD_SCHEME_SET, ID_APPEARANCE_IMPORT_SKIN,
                          ID_SCHEME_SET_BASE + 1, ID_WEBDAV_SETTINGS,
                          ID_CLOUD_CLIPBOARD_TEST, ID_CLOUD_CLIPBOARD_ENABLED,
                          ID_CLOUD_CLIPBOARD_SETTINGS, 999};
  bool allProxy = true;
  for (int id : proxyIds) {
    CommandResult pr = handleCommand(id, s8);
    if (pr.action != CmdAction::Proxy) allProxy = false;
  }
  check(allProxy, "cmd: network/scheme-set/unknown stay proxied");
  check(diffJson(buildMenu(s8), golden, "m").empty(),
        "cmd: proxied ids leave state untouched");

  // C8. 本地静态集合（文件模板/文件夹/帮助/维护回执）
  const int localIds[] = {18, 19, 222, ID_HELP_DOCS, ID_DISCUSSIONS,
                          ID_USER_DIR, ID_SHARED_DIR, ID_SYNC_DIR, ID_LOG_DIR,
                          ID_LAUNCHER_DEBUG_LOG, ID_LAUNCHER_RESTART_BACKENDS,
                          ID_LAUNCHER_QUIT};
  bool allLocal = true;
  for (int id : localIds) {
    if (!isLocalCommandId(id)) {
      std::printf("  C8 id %d: not in isLocalCommandId\n", id);
      allLocal = false;
    }
    CommandResult pr = handleCommand(id, s8);
    if (pr.action != CmdAction::Local || !pr.ok) {
      std::printf("  C8 id %d: action=%d ok=%d\n", id, (int)pr.action,
                  (int)pr.ok);
      allLocal = false;
    }
  }
  check(allLocal, "cmd: folders/help/launcher are local receipts");

  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
