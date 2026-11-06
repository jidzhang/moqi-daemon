// 5b 菜单纯核心实现。行为黄金 = tests/fixtures/menu_golden.json；
// 规格与 id 表来源见 docs/5b-menu-port-spec.md 与 DaemonMenuCore.h。
#include "DaemonMenuCore.h"

#include <algorithm>
#include <cctype>

namespace Moqi {
namespace Menu {
namespace {

// ---- 黄金形状工具：每个菜单项都带全量六键（与 protojson 输出一致） ----

Json::Value makeItem(int id, const std::string& text) {
  Json::Value v(Json::objectValue);
  v["id"] = id;
  v["text"] = text;
  v["checked"] = false;
  v["enabled"] = true;
  v["submenu"] = Json::Value(Json::arrayValue);
  v["separator"] = false;
  return v;
}

Json::Value makeParent(const std::string& text) {
  return makeItem(0, text);
}

Json::Value makeSeparator() {
  Json::Value v = makeItem(0, "");
  v["separator"] = true;
  return v;
}

void setChecked(Json::Value& item, bool checked) { item["checked"] = checked; }

bool eqFold(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
      return false;
  }
  return true;
}

std::string toLower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

// ---- 外观子菜单：选项表驱动（id 序列与 Go 常量表一致） ----

struct NumOption {
  int id;
  int value;
  const char* label;
};

void appendNumGroup(Json::Value& parent, const char* title,
                    const NumOption* opts, size_t n, int current,
                    bool enabled = true) {
  Json::Value group = makeParent(title);
  for (size_t i = 0; i < n; ++i) {
    Json::Value it = makeItem(opts[i].id, opts[i].label);
    setChecked(it, current == opts[i].value);
    if (!enabled) it["enabled"] = false;
    group["submenu"].append(it);
  }
  if (!enabled) group["enabled"] = false;
  parent["submenu"].append(group);
}

struct StrOption {
  int id;
  const char* label;
  const char* value;
};

void appendStrGroup(Json::Value& parent, const char* title,
                    const StrOption* opts, size_t n,
                    const std::string& current) {
  Json::Value group = makeParent(title);
  for (size_t i = 0; i < n; ++i) {
    Json::Value it = makeItem(opts[i].id, opts[i].label);
    setChecked(it, eqFold(current, opts[i].value));
    group["submenu"].append(it);
  }
  parent["submenu"].append(group);
}

} // namespace

std::string switchMenuText(const MenuSwitchDef& sw, bool enabled) {
  if (sw.states.empty()) return sw.name;
  if (sw.states.size() == 1) return sw.states[0];
  // enabled: "states[1] → states[0]"（当前值在前、目标在后）
  if (enabled) return sw.states[1] + " → " + sw.states[0];
  return sw.states[0] + " → " + sw.states[1];
}

Json::Value buildMenu(const MenuState& state) {
  Json::Value items(Json::arrayValue);

  // 1) 开关区（save_options 顺序）
  int i = 0;
  for (const auto& sw : state.switches) {
    Json::Value it = makeItem(ID_SWITCH_BASE + i, switchMenuText(sw, state.options.count(sw.name) && state.options.at(sw.name)));
    setChecked(it, state.options.count(sw.name) && state.options.at(sw.name));
    // daemon 专属字段：选项名。64 位客户端的 DLL 据此在本地引擎上直接
    // 翻转（RPC 到这里切的是 daemon 会话，对进程内引擎无效）。
    it["option"] = sw.name;
    items.append(it);
    ++i;
  }
  if (!state.switches.empty()) items.append(makeSeparator());

  // 2) 输入方案
  if (!state.schemas.empty()) {
    Json::Value group = makeParent("输入方案(&I)");
    int n = 0;
    for (const auto& sc : state.schemas) {
      std::string text = sc.name.empty() ? sc.id : sc.name;
      if (text.empty()) { ++n; continue; }
      Json::Value it = makeItem(ID_SCHEMA_BASE + n, text);
      setChecked(it, sc.current && !sc.id.empty());
      group["submenu"].append(it);
      ++n;
    }
    items.append(group);
  }

  // 3) 置顶短语/简拼/更新/刷新
  items.append(makeItem(18, "打开置顶短语"));
  items.append(makeItem(19, "打开超级简拼"));
  items.append(makeItem(ID_UPDATE_CONFIG, "在线更新方案"));
  items.append(makeItem(ID_DEPLOY, "重新部署方案(&R)"));
  items.append(makeSeparator());

  // 5) 输入状态共享
  {
    Json::Value it = makeItem(ID_SHARED_INPUT_STATE, "输入状态固定");
    setChecked(it, state.inputStateShared);
    items.append(it);
  }

  // 6) 外观
  {
    Json::Value app = makeParent("外观(&A)");
    {
      Json::Value themes = makeParent("切换主题");
      int n = 0;
      for (const auto& th : state.themes) {
        Json::Value it = makeItem(ID_APPEARANCE_THEME_BASE + n, th.label);
        setChecked(it, !state.theme.empty() &&
                           toLower(state.theme) == toLower(th.id));
        themes["submenu"].append(it);
        ++n;
      }
      app["submenu"].append(themes);
    }
    {
      Json::Value it = makeItem(ID_APPEARANCE_INLINE_PREEDIT, "行内预编辑");
      setChecked(it, state.inlinePreedit);
      app["submenu"].append(it);
    }
    {
      Json::Value group = makeParent("候选排列");
      Json::Value v = makeItem(ID_APPEARANCE_LAYOUT_VERTICAL, "竖排");
      setChecked(v, !state.horizontal);
      Json::Value h = makeItem(ID_APPEARANCE_LAYOUT_HORIZONTAL, "横排");
      setChecked(h, state.horizontal);
      group["submenu"].append(v);
      group["submenu"].append(h);
      app["submenu"].append(group);
    }
    {
      static const NumOption perRow[] = {
          {ID_APPEARANCE_PER_ROW_3, 3, "3"}, {180 + 1, 5, "5"},
          {180 + 2, 7, "7"}, {180 + 3, 9, "9"}};
      appendNumGroup(app, "每行候选数", perRow, 4, state.perRow,
                     state.horizontal);
    }
    {
      static const NumOption spacing[] = {
          {184, 0, "0"}, {185, 10, "10"}, {186, 20, "20"}, {187, 30, "30"},
          {188, 40, "40"}, {189, 50, "50"}};
      appendNumGroup(app, "候选间距", spacing, 6, state.spacing);
    }
    {
      static const NumOption counts[] = {
          {190, 3, "3"}, {191, 5, "5"}, {192, 7, "7"}, {193, 9, "9"}};
      appendNumGroup(app, "总候选数量", counts, 4, state.candCount);
    }
    {
      static const NumOption fonts[] = {
          {113, 8, "8"}, {114, 10, "10"}, {115, 12, "12"}, {116, 14, "14"},
          {117, 16, "16"}, {118, 18, "18"}, {119, 20, "20"}, {123, 22, "22"},
          {124, 24, "24"}, {125, 26, "26"}, {126, 28, "28"}, {127, 30, "30"}};
      appendNumGroup(app, "字体大小", fonts, 12, state.fontPoint);
    }
    {
      static const StrOption faces[] = {
          {194, "Segoe UI", "Segoe UI"},
          {195, "微软雅黑 UI", "Microsoft YaHei UI"},
          {196, "等线", "DengXian"},
          {197, "宋体", "SimSun"}};
      appendStrGroup(app, "候选文字字体", faces, 4, state.fontFace);
    }
    {
      static const NumOption cfonts[] = {
          {213, 8, "8"}, {214, 10, "10"}, {216, 12, "12"}, {217, 14, "14"},
          {218, 16, "16"}, {219, 18, "18"}, {220, 20, "20"}, {221, 22, "22"},
          {222, 24, "24"}, {223, 26, "26"}, {224, 28, "28"}, {225, 30, "30"}};
      appendNumGroup(app, "注释文字大小", cfonts, 12, state.commentFontPoint);
    }
    {
      static const StrOption cfaces[] = {
          {198, "Consolas", "Consolas"},
          {199, "微软雅黑 UI", "Microsoft YaHei UI"},
          {200, "等线", "DengXian"},
          {201, "宋体", "SimSun"}};
      appendStrGroup(app, "注释文字字体", cfaces, 4, state.commentFontFace);
    }
    {
      static const StrOption bgs[] = {{120, "白色", "#ffffff"},
                                      {121, "暖白", "#fff7e8"},
                                      {122, "浅蓝", "#f3f8ff"}};
      appendStrGroup(app, "候选框背景", bgs, 3, state.bgColor);
    }
    {
      static const StrOption hls[] = {{130, "浅蓝", "#c6ddf9"},
                                      {131, "浅灰", "#e5e7eb"},
                                      {132, "浅绿", "#d9f2e6"}};
      appendStrGroup(app, "高亮颜色", hls, 3, state.hlColor);
    }
    {
      static const StrOption txts[] = {{140, "黑色", "#000000"},
                                       {141, "深灰", "#333333"},
                                       {142, "深蓝", "#1d4ed8"}};
      appendStrGroup(app, "字体颜色", txts, 3, state.textColor);
    }
    {
      static const StrOption htx[] = {{145, "黑色", "#000000"},
                                      {146, "白色", "#ffffff"},
                                      {147, "深蓝", "#1d4ed8"}};
      appendStrGroup(app, "高亮文字颜色", htx, 3, state.hlTextColor);
    }
    app["submenu"].append(makeSeparator());
    app["submenu"].append(makeItem(ID_APPEARANCE_IMPORT_SKIN, "导入皮肤"));
    items.append(app);
  }

  // 7) 输入设置
  {
    Json::Value group = makeParent("输入设置");
    Json::Value a = makeItem(ID_INPUT_AUTO_PAIR_QUOTES, "自动插入成对符号");
    setChecked(a, state.autoPairQuotes);
    group["submenu"].append(a);
    group["submenu"].append(makeItem(222, "打开成对符号设置"));
    Json::Value s = makeItem(ID_INPUT_SEMICOLON_SELECT_SECOND, "分号键次选");
    setChecked(s, state.semicolonSecond);
    group["submenu"].append(s);
    // daemon 专属：多数用户找不到 ai_config.json 在哪，就地用记事本打开。
    group["submenu"].append(makeItem(ID_AI_CONFIG, "配置AI助手"));
    items.append(group);
  }

  // 8) 同步设置（命令本地实现）
  {
    Json::Value group = makeParent("同步设置");
    // 云剪贴板与 WebDAV 共用 cloud_clipboard.json（base_url/账号/密码/开关/
    // 最短间隔/热键都在一个文件里），保留一个配置入口，避免两个菜单项
    // 打开同一文件（2026-11-03 Go 退休时合并）。
    group["submenu"].append(makeItem(ID_WEBDAV_SETTINGS, "WebDAV 配置..."));
    group["submenu"].append(makeItem(ID_CLOUD_CLIPBOARD_TEST, "测试 WebDAV"));
    Json::Value c =
        makeItem(ID_CLOUD_CLIPBOARD_ENABLED, "启用云剪贴板");
    setChecked(c, state.cloudEnabled);
    group["submenu"].append(c);
    group["submenu"].append(makeItem(ID_SYNC, "同步用户词库"));
    items.append(group);
  }

  // 9) 维护（launcher 执行，回执在本地）
  {
    Json::Value group = makeParent("维护");
    Json::Value d = makeItem(ID_LAUNCHER_DEBUG_LOG, "开启调试");
    setChecked(d, state.launcherDebug);
    group["submenu"].append(d);
    group["submenu"].append(makeItem(ID_LAUNCHER_RESTART_BACKENDS, "重启引擎"));
    group["submenu"].append(makeItem(ID_LAUNCHER_QUIT, "退出引擎"));
    items.append(group);
  }

  // 10) 打开文件夹
  {
    Json::Value group = makeParent("打开文件夹(&O)");
    group["submenu"].append(makeItem(ID_USER_DIR, "用户文件夹"));
    group["submenu"].append(makeItem(ID_SHARED_DIR, "共享文件夹"));
    group["submenu"].append(makeItem(ID_SYNC_DIR, "同步文件夹"));
    group["submenu"].append(makeItem(ID_LOG_DIR, "日志文件夹"));
    items.append(group);
  }

  items.append(makeSeparator());
  items.append(makeItem(ID_HELP_DOCS, "帮助文档"));
  items.append(makeItem(ID_DISCUSSIONS, "参与讨论"));
  return items;
}

namespace {

bool numCommand(const NumOption* opts, size_t n, int id, int& out) {
  for (size_t i = 0; i < n; ++i) {
    if (opts[i].id == id) {
      out = opts[i].value;
      return true;
    }
  }
  return false;
}

bool strCommand(const StrOption* opts, size_t n, int id, std::string& out) {
  for (size_t i = 0; i < n; ++i) {
    if (opts[i].id == id) {
      out = opts[i].value;
      return true;
    }
  }
  return false;
}

} // namespace

bool isLocalCommandId(int id) {
  if (id == 18 || id == 19 || id == 222 || // 置顶短语/超级简拼/成对符号设置
      id == ID_AI_CONFIG ||               // 打开 ai_config.json（daemon 专属）
      id == ID_HELP_DOCS || id == ID_DISCUSSIONS || id == ID_USER_DIR ||
      id == ID_SHARED_DIR || id == ID_SYNC_DIR || id == ID_LOG_DIR ||
      id == ID_LAUNCHER_DEBUG_LOG || id == ID_LAUNCHER_RESTART_BACKENDS ||
      id == ID_LAUNCHER_QUIT || id == ID_SHARED_INPUT_STATE ||
      id == ID_INPUT_AUTO_PAIR_QUOTES ||
      id == ID_INPUT_SEMICOLON_SELECT_SECOND || id == ID_MODE_ICON ||
      id == ID_ASCII_MODE || id == ID_FULL_SHAPE || id == ID_ASCII_PUNCT ||
      id == ID_TRADITIONALIZATION) {
    return true;
  }
  if (id >= ID_SWITCH_BASE && id < ID_SCHEME_SET_BASE) return true; // 开关
  if (id >= ID_SCHEMA_BASE && id < ID_SCHEME_SET_BASE) return true; // 方案
  // 主题号是 4000+序号。必须封上界：5001-5004（WebDAV/云剪贴板）也 >=4000，
  // 旧写法会把它们当主题命令，先走 ensureEngine/onActivate（冷启动最长 30s），
  // 再在 handleCommand 里越界落空。打开配置文件不该等引擎。
  if (id >= ID_APPEARANCE_THEME_BASE && id < ID_APPEARANCE_THEME_BASE + 1000) {
    return true;
  }
  // 外观命令全段 100-225（含 130-147 的颜色组——旧范围漏了 128-147，
  // Go 在世时靠代理兜底，退休后这三组颜色菜单会变成死项；2026-11-03
  // 全菜单扫描实测发现）。
  if (id >= 100 && id <= 225) return true;
  return false;
}

CommandResult handleCommand(int id, MenuState& state) {
  CommandResult r;
  r.action = CmdAction::Proxy;

  auto local = [&r]() {
    r.action = CmdAction::Local;
    r.ok = true;
  };
  // 开关：本地翻转 options 快照（菜单重建即见新状态），应用层按
  // optionToggled 驱动 rime 引擎 + 共享状态落盘。
  auto toggle = [&](const std::string& name) {
    r.optionToggled = name;
    auto it = state.options.find(name);
    if (it != state.options.end()) it->second = !it->second;
    local();
  };

  // 开关：本地引擎选项
  if (id == ID_MODE_ICON || id == ID_ASCII_MODE) {
    toggle("ascii_mode");
  } else if (id == ID_FULL_SHAPE) {
    toggle("full_shape");
  } else if (id == ID_ASCII_PUNCT) {
    toggle("ascii_punct");
  } else if (id == ID_TRADITIONALIZATION) {
    toggle("traditionalization");
  } else if (id >= ID_SWITCH_BASE && id < ID_SCHEME_SET_BASE) {
    size_t index = (size_t)(id - ID_SWITCH_BASE);
    if (index < state.switches.size()) {
      toggle(state.switches[index].name);
    }
    return r; // 2000-2999：越界交 Go 判 unknown，3000+ 不归这里
  }

  // 方案
  if (id >= ID_SCHEMA_BASE && id < ID_SCHEME_SET_BASE) {
    size_t index = (size_t)(id - ID_SCHEMA_BASE);
    if (index < state.schemas.size() && !state.schemas[index].id.empty()) {
      r.schemaSelected = state.schemas[index].id;
      local();
    }
    return r;
  }

  // 主题（4000-4999；5000+ 是云/WebDAV，不能落进这里）
  if (id >= ID_APPEARANCE_THEME_BASE &&
      id < ID_APPEARANCE_THEME_BASE + 1000) {
    size_t index = (size_t)(id - ID_APPEARANCE_THEME_BASE);
    if (index < state.themes.size()) {
      state.theme = state.themes[index].id;
      local();
      // 主题换全套外观——应用层落盘时读主题定义；这里至少广播当前值。
      r.customizeUi = Json::Value(Json::objectValue);
    }
    return r;
  }

  // 输入状态/输入设置
  if (id == ID_SHARED_INPUT_STATE) {
    state.inputStateShared = !state.inputStateShared;
    local();
  } else if (id == ID_INPUT_AUTO_PAIR_QUOTES) {
    state.autoPairQuotes = !state.autoPairQuotes;
    r.customizeUi = Json::Value(Json::objectValue);
    r.customizeUi["autoPairQuotes"] = state.autoPairQuotes;
    local();
  } else if (id == ID_INPUT_SEMICOLON_SELECT_SECOND) {
    state.semicolonSecond = !state.semicolonSecond;
    local();
  } else if (id == ID_APPEARANCE_INLINE_PREEDIT) {
    state.inlinePreedit = !state.inlinePreedit;
    r.customizeUi = Json::Value(Json::objectValue);
    r.customizeUi["inlinePreedit"] = state.inlinePreedit;
    local();
  } else if (id == ID_APPEARANCE_LAYOUT_VERTICAL ||
             id == ID_APPEARANCE_LAYOUT_HORIZONTAL) {
    state.horizontal = (id == ID_APPEARANCE_LAYOUT_HORIZONTAL);
    if (state.horizontal) {
      // 从竖排切回横排：原值可能已不可用（竖排落盘写 0/1），回落到 9。
      int raw = state.perRowRaw;
      if (raw != 3 && raw != 5 && raw != 7 && raw != 9) raw = 9;
      state.perRowRaw = raw;
      state.perRow = raw < state.candCount ? raw : state.candCount;
    }
    local();
  } else {
    // 数值/字串外观组
    int num = 0;
    std::string str;
    static const NumOption perRow[] = {
        {ID_APPEARANCE_PER_ROW_3, 3, "3"}, {180 + 1, 5, "5"},
        {180 + 2, 7, "7"}, {180 + 3, 9, "9"}};
    static const NumOption spacing[] = {
        {184, 0, "0"}, {185, 10, "10"}, {186, 20, "20"}, {187, 30, "30"},
        {188, 40, "40"}, {189, 50, "50"}};
    static const NumOption counts[] = {
        {190, 3, "3"}, {191, 5, "5"}, {192, 7, "7"}, {193, 9, "9"}};
    static const NumOption fonts[] = {
        {113, 8, "8"}, {114, 10, "10"}, {115, 12, "12"}, {116, 14, "14"},
        {117, 16, "16"}, {118, 18, "18"}, {119, 20, "20"}, {123, 22, "22"},
        {124, 24, "24"}, {125, 26, "26"}, {126, 28, "28"}, {127, 30, "30"}};
    static const NumOption cfonts[] = {
        {213, 8, "8"}, {214, 10, "10"}, {216, 12, "12"}, {217, 14, "14"},
        {218, 16, "16"}, {219, 18, "18"}, {220, 20, "20"}, {221, 22, "22"},
        {223, 26, "26"}, {224, 28, "28"}, {225, 30, "30"}};
    static const StrOption faces[] = {
        {194, "Segoe UI", "Segoe UI"},
        {195, "微软雅黑 UI", "Microsoft YaHei UI"},
        {196, "等线", "DengXian"},
        {197, "宋体", "SimSun"}};
    static const StrOption cfaces[] = {
        {198, "Consolas", "Consolas"},
        {199, "微软雅黑 UI", "Microsoft YaHei UI"},
        {200, "等线", "DengXian"},
        {201, "宋体", "SimSun"}};
    static const StrOption bgs[] = {{120, "白色", "#ffffff"},
                                    {121, "暖白", "#fff7e8"},
                                    {122, "浅蓝", "#f3f8ff"}};
    static const StrOption hls[] = {{130, "浅蓝", "#c6ddf9"},
                                    {131, "浅灰", "#e5e7eb"},
                                    {132, "浅绿", "#d9f2e6"}};
    static const StrOption txts[] = {{140, "黑色", "#000000"},
                                     {141, "深灰", "#333333"},
                                     {142, "深蓝", "#1d4ed8"}};
    static const StrOption htx[] = {{145, "黑色", "#000000"},
                                    {146, "白色", "#ffffff"},
                                    {147, "深蓝", "#1d4ed8"}};

    // 注意：222 = 打开成对符号设置（与注释字号 24 的 id 冲突是 Go 存量——
    // Go 的 onCommand 也是 222 先命中“打开成对符号设置”，字号 24 永远选不中，
    // 此处按同样语义走本地回执）。
    r.customizeUi = Json::Value(Json::objectValue);
    if (numCommand(perRow, 4, id, num)) {
      state.perRow = num;
      state.perRowRaw = num;
      r.customizeUi["candPerRow"] = num;
      local();
    } else if (numCommand(spacing, 6, id, num)) {
      state.spacing = num;
      r.customizeUi["candSpacing"] = num;
      local();
    } else if (numCommand(counts, 4, id, num)) {
      state.candCount = num;
      local();
    } else if (numCommand(fonts, 12, id, num)) {
      state.fontPoint = num;
      r.customizeUi["candFontSize"] = num;
      local();
    } else if (numCommand(cfonts, 11, id, num)) {
      state.commentFontPoint = num;
      r.customizeUi["candCommentFontSize"] = num;
      local();
    } else if (strCommand(faces, 4, id, str)) {
      state.fontFace = str;
      r.customizeUi["candFontName"] = str;
      local();
    } else if (strCommand(cfaces, 4, id, str)) {
      state.commentFontFace = str;
      r.customizeUi["candCommentFontName"] = str;
      local();
    } else if (strCommand(bgs, 3, id, str)) {
      // Go applyAppearanceCommand：任一自定义颜色命令都把主题切到 custom，
      // 否则菜单仍勾着原主题、实际颜色却已变。
      state.theme = "custom";
      state.bgColor = str;
      r.customizeUi["candBackgroundColor"] = str;
      local();
    } else if (strCommand(hls, 3, id, str)) {
      state.theme = "custom";
      state.hlColor = str;
      r.customizeUi["candHighlightColor"] = str;
      local();
    } else if (strCommand(txts, 3, id, str)) {
      state.theme = "custom";
      state.textColor = str;
      // Go 同步注释色：主题/自定义正文色变化时注释跟随，否则深色背景下
      // 注释仍是旧色，看不清。
      state.commentColor = str;
      r.customizeUi["candTextColor"] = str;
      r.customizeUi["candCommentColor"] = str;
      local();
    } else if (strCommand(htx, 3, id, str)) {
      state.theme = "custom";
      state.hlTextColor = str;
      state.commentHlColor = str;
      r.customizeUi["candHighlightTextColor"] = str;
      r.customizeUi["candCommentHighlightColor"] = str;
      local();
    } else {
      r.customizeUi = Json::Value(); // 未命中，清掉
    }
  }

  // 静态本地集（文件模板/文件夹/帮助/维护）：回执即可，动作在应用层
  if (r.action == CmdAction::Proxy &&
      (id == 18 || id == 19 || id == 222 || id == ID_AI_CONFIG ||
       id == ID_HELP_DOCS || id == ID_DISCUSSIONS || id == ID_USER_DIR ||
       id == ID_SHARED_DIR || id == ID_SYNC_DIR || id == ID_LOG_DIR ||
       id == ID_LAUNCHER_DEBUG_LOG || id == ID_LAUNCHER_RESTART_BACKENDS ||
       id == ID_LAUNCHER_QUIT)) {
    local();
  }
  return r;
}

} // namespace Menu
} // namespace Moqi
