// 5b 菜单应用层实现。采集/落盘/打开；纯逻辑见 DaemonMenuCore（单测锁定）。
// include 纪律：RimeCore.h 链开篇（见 ChildProxy.h 注释）。
#include "RimeCore.h"

#include <Shellapi.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include "DaemonAI.h"
#include "DaemonEngine.h"
#include "DaemonMenuCore.h"
#include "DaemonMenuService.h"

namespace Moqi {
namespace {

std::string wsToUtf8(const std::wstring& ws) {
  if (ws.empty()) return std::string();
  int n = ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(),
                                nullptr, 0, nullptr, nullptr);
  std::string out(n, 0);
  ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), &out[0], n,
                        nullptr, nullptr);
  return out;
}

std::wstring utf8ToWs(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring out(n, 0);
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
  return out;
}

std::wstring appDataRoot() { return moqiAppDataDir(); }

std::wstring localAppDataRoot() {
  const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
  if (local && *local) return std::wstring(local) + L"\\MoqiIM";
  return L"";
}

Json::Value readJsonFile(const std::wstring& path) {
  Json::Value v;
  std::ifstream in(path, std::ios::binary);
  if (!in) return v;
  std::string errs;
  Json::CharReaderBuilder builder;
  Json::parseFromStream(builder, in, &v, &errs);
  return v;
}

bool writeJsonFile(const std::wstring& path, const Json::Value& v) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  Json::StreamWriterBuilder builder;
  builder["commentStyle"] = "None";
  builder["indentation"] = "  ";
  std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
  writer->write(v, &out);
  out << "\n";
  return out.good();
}

std::string trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

std::string stripQuotes(const std::string& s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
    return s.substr(1, s.size() - 2);
  return s;
}

// 极简 yaml 列表扫描（只服务 rime-frost 的 save_options 与 switches 块，
// 不做通用 yaml）。save_options 在 default.yaml 里是嵌套块（带缩进+行尾
// 注释），按"首个 save_options: 行"定位再收列表。文件为 UTF-8。
std::vector<std::string> yamlTopLevelList(const std::wstring& path,
                                          const std::string& key) {
  std::vector<std::string> out;
  std::ifstream in(path, std::ios::binary);
  if (!in) return out;
  std::string line;
  bool inList = false;
  std::string listIndent; // 列表所属行的缩进
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!inList) {
      const std::string t = trim(line);
      if (t.rfind(key + ":", 0) == 0) {
        inList = true;
        listIndent = line.substr(0, line.find_first_not_of(" \t"));
      }
      continue;
    }
    // 列表项：缩进不浅于所属键行，且以 "- " 开头；否则列表结束。
    if (trim(line).empty()) continue;
    const size_t ind = line.find_first_not_of(" \t");
    const std::string t = trim(line);
    if (ind < listIndent.size() || t.empty() || t[0] != '-') {
      break;
    }
    std::string item = trim(t.substr(1));
    size_t hash = item.find('#');
    if (hash != std::string::npos) item = trim(item.substr(0, hash));
    item = stripQuotes(item);
    if (!item.empty()) out.push_back(item);
  }
  return out;
}

struct RawSwitch {
  std::string name;
  std::vector<std::string> states;
};

std::vector<RawSwitch> yamlSwitches(const std::wstring& path) {
  std::vector<RawSwitch> out;
  std::ifstream in(path, std::ios::binary);
  if (!in) return out;
  std::string line;
  bool inBlock = false;
  bool haveCurrent = false;
  RawSwitch cur;
  auto push = [&]() {
    if (haveCurrent && !cur.name.empty()) out.push_back(cur);
    cur = RawSwitch();
    haveCurrent = false;
  };
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!inBlock) {
      if (line == "switches:") {
        inBlock = true;
      }
      continue;
    }
    if (line.empty()) continue;
    if (line[0] != ' ') break; // 块结束
    if (line.size() > 2 && line[0] == ' ' && line[1] == '-' &&
        (line[2] == ' ' || line[2] == '-')) {
      // "- name: xxx" 或 "- - name: xxx"（列表套列表极罕见，按前者处理）
    }
    if (line.find("- name:") != std::string::npos) {
      push();
      std::string name = line.substr(line.find("- name:") + 7);
      size_t hash = name.find('#'); // 行内注释（如 "ascii_punct # 中英标点"）
      if (hash != std::string::npos) name = name.substr(0, hash);
      cur.name = trim(name);
      haveCurrent = true;
    } else if (line.find("states:") != std::string::npos && haveCurrent) {
      size_t lb = line.find('[');
      if (lb != std::string::npos) {
        size_t rb = line.find(']', lb);
        std::string inner =
            line.substr(lb + 1, rb == std::string::npos
                                    ? std::string::npos
                                    : rb - lb - 1);
        std::istringstream ss(inner);
        std::string part;
        while (std::getline(ss, part, ',')) {
          std::string s = stripQuotes(trim(part));
          if (!s.empty()) cur.states.push_back(s);
        }
      } else {
        // 块列表形态：后续 "  - 文案" 行
        while (std::getline(in, line)) {
          if (!line.empty() && line.back() == '\r') line.pop_back();
          if (line.find('-') == std::string::npos) break;
          size_t dash = line.find('-');
          std::string s = stripQuotes(trim(line.substr(dash + 1)));
          if (s.empty() || s.find(':') != std::string::npos) break;
          cur.states.push_back(s);
        }
      }
    }
  }
  push();
  return out;
}

std::string fileToString(const std::wstring& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return "";
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void shellOpen(const std::wstring& target, bool isUrl) {
  if (target.empty()) return;
  ::ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr,
                  SW_SHOWNORMAL);
}

} // namespace

void openTextInBundledEditor(const std::wstring& installRoot,
                             const std::wstring& file) {
  if (file.empty()) return;
  // 随包编辑器：Notepad4（BSD-3）以 notepad2.exe 名称分发（用户 2026-11-03
  // 定：4 在汉语文化里寓意不好）。旧安装的 Notepad4 目录作兼容回退。
  const std::wstring candidates[] = {
      installRoot + L"\\Notepad2\\notepad2.exe",
      installRoot + L"\\Notepad4\\Notepad4.exe",
  };
  wchar_t sysDir[MAX_PATH] = {};
  const UINT n = ::GetSystemDirectoryW(sysDir, MAX_PATH);
  const std::wstring notepad =
      n > 0 ? std::wstring(sysDir) + L"\\notepad.exe" : L"notepad.exe";
  std::wstring editor;
  for (const auto& candidate : candidates) {
    if (::GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
      editor = candidate;
      break;
    }
  }
  if (editor.empty() &&
      ::GetFileAttributesW(notepad.c_str()) != INVALID_FILE_ATTRIBUTES) {
    editor = notepad;
  }
  if (!editor.empty()) {
    const std::wstring args = L"\"" + file + L"\"";
    ::ShellExecuteW(nullptr, L"open", editor.c_str(), args.c_str(), nullptr,
                    SW_SHOWNORMAL);
    return;
  }
  shellOpen(file, false);
}


DaemonMenuService::DaemonMenuService(std::wstring installRoot,
                                     std::wstring rimeDataDir,
                                     std::wstring appDataDir)
    : installRoot_(std::move(installRoot)),
      rimeDataDir_(std::move(rimeDataDir)),
      appDataDir_(std::move(appDataDir)) {}

Json::Value DaemonMenuService::loadAppearanceConfig() {
  std::wstring root = appDataDir_.empty() ? appDataRoot() : appDataDir_;
  Json::Value cfg = readJsonFile(root + L"\\appearance_config.json");
  if (cfg.isObject()) return cfg;
  // Go loadAppearancePrefs 的 legacy 回退：旧版把配置写在 Moqi\Rime\。
  // 升级到 2.0 的用户如果只剩旧文件，不迁移就会丢主题/字号/开关。
  const std::wstring legacy = root + L"\\Rime\\appearance_config.json";
  Json::Value old = readJsonFile(legacy);
  if (old.isObject()) {
    writeJsonFile(root + L"\\appearance_config.json", old);
    return old;
  }
  return cfg;
}

void DaemonMenuService::persistAppearance(const Menu::MenuState& state) {
  std::wstring root = appDataDir_.empty() ? appDataRoot() : appDataDir_;
  Json::Value cfg = loadAppearanceConfig();
  if (!cfg.isObject()) cfg = Json::Value(Json::objectValue);
  cfg["candidate_theme"] = state.theme;
  cfg["inline_preedit"] = state.inlinePreedit;
  // 落盘写「配置原值」（3/5/7/9），不写 min(配置, 总候选) 的生效值——否则
  // 用户把总候选从 7 调到 9 后，每行候选数会永久卡在 7（Go 也写原值）。
  cfg["candidate_per_row"] = state.horizontal ? state.perRowRaw : 0;
  cfg["candidate_count"] = state.candCount;
  cfg["candidate_spacing"] = state.spacing;
  cfg["font_point"] = state.fontPoint;
  cfg["font_face"] = state.fontFace;
  cfg["candidate_comment_font_point"] = state.commentFontPoint;
  cfg["candidate_comment_font_face"] = state.commentFontFace;
  cfg["candidate_background_color"] = state.bgColor;
  cfg["candidate_highlight_color"] = state.hlColor;
  cfg["candidate_text_color"] = state.textColor;
  cfg["candidate_highlight_text_color"] = state.hlTextColor;
  cfg["candidate_comment_color"] = state.commentColor;
  cfg["candidate_comment_highlight_color"] = state.commentHlColor;
  cfg["input_state_shared"] = state.inputStateShared;
  cfg["auto_pair_quotes"] = state.autoPairQuotes;
  cfg["semicolon_select_second"] = state.semicolonSecond;
  // 一次性迁移标记：存量配置里的 "Segoe UI" 是 Go 时代默认值（无 CJK
  // 字形，GDI 回退宋体），gather 会改成雅黑；但菜单里仍允许手动选回
  // Segoe UI，没有标记的话用户选了也会被再次迁移掉。
  cfg["font_face_migrated"] = true;
  writeJsonFile(root + L"\\appearance_config.json", cfg);
}

std::vector<ThemeDefinition> DaemonMenuService::loadThemes() {
  // 内置主题随包（appearance_themes.json），用户主题在 appdata；同 id 内置
  // 优先，最终按 label（UTF-8 字节序）再 id 排序 —— 与 Go 黄金一致。
  std::vector<ThemeDefinition> out;
  std::vector<std::string> builtinIds;
  auto normalize = [](std::string s) {
    size_t b = 0, e = s.size();
    while (b < e && isspace((unsigned char)s[b])) ++b;
    while (e > b && isspace((unsigned char)s[e - 1])) --e;
    s = s.substr(b, e - b);
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
  };
  auto loadFrom = [&](const std::wstring& path, bool builtin) {
    Json::Value j = readJsonFile(path);
    const Json::Value& arr = j["themes"];
    if (!arr.isArray()) return;
    for (const auto& t : arr) {
      ThemeDefinition d;
      d.id = normalize(t.get("id", "").asString());
      if (d.id.empty()) continue;
      d.label = t.get("label", "").asString();
      // Go normalizeThemeDefinition：label 空回退 id。
      size_t lb = d.label.find_first_not_of(" \t");
      size_t le = d.label.find_last_not_of(" \t");
      d.label = lb == std::string::npos ? "" : d.label.substr(lb, le - lb + 1);
      if (d.label.empty()) d.label = d.id;
      std::string source = normalize(t.get("source", "").asString());
      if (source.empty()) source = "imported";
      if (builtin) {
        builtinIds.push_back(d.id);
      } else if (source == "imported" &&
                 std::find(builtinIds.begin(), builtinIds.end(), d.id) !=
                     builtinIds.end()) {
        // Go mergeThemeDefinitions：导入主题与内置同 id 时保留两者，
        // 导入项改名 "imported/<id>" 并加"(导入)"后缀。旧实现直接丢掉，
        // 用户导入皮肤后菜单里看不到。
        d.id = "imported/" + d.id;
        d.label += " (导入)";
      }
      const Json::Value& ap = t["appearance"];
      if (ap.isObject()) d.appearance = ap;
      bool exists = false;
      for (auto& e : out) {
        if (e.id == d.id) {
          exists = true;
          if (builtin) e = d;
          break;
        }
      }
      if (!exists) out.push_back(d);
    }
  };
  std::wstring root = appDataDir_.empty() ? appDataRoot() : appDataDir_;
  loadFrom(rimeDataDir_ + L"\\appearance_themes.json", true);
  // 内置表也可能有多份历史位置（rime\appearance_themes.json）。
  if (out.empty()) {
    std::wstring rimeRoot = rimeDataDir_;
    const size_t slash = rimeRoot.find_last_of(L"\\/");
    if (slash != std::wstring::npos) rimeRoot = rimeRoot.substr(0, slash);
    loadFrom(rimeRoot + L"\\appearance_themes.json", true);
  }
  loadFrom(root + L"\\appearance_themes.json", false);
  std::stable_sort(out.begin(), out.end(), [](const ThemeDefinition& a,
                                              const ThemeDefinition& b) {
    if (a.label != b.label) return a.label < b.label;
    return a.id < b.id;
  });
  return out;
}

std::vector<Menu::MenuSwitchDef> DaemonMenuService::loadSwitchDefs(
    DaemonEngine& engine) {
  // 顺序 = default.yaml 的 save_options；states 来自当前 schema yaml 的
  // switches 块。与 Go（SaveOptions 顺序 ∩ SchemaSwitches）同构。
  std::vector<Menu::MenuSwitchDef> defs;
  RimeRuntime& runtime = RimeRuntime::instance();
  std::vector<std::string> saved =
      yamlTopLevelList(rimeDataDir_ + L"\\default.yaml", "save_options");
  std::vector<RawSwitch> raw;
  {
    char schemaId[256] = {};
    if (runtime.rimeGetCurrentSchema != nullptr &&
        runtime.rimeGetCurrentSchema(engine.sessionId(), schemaId,
                                     sizeof(schemaId)) &&
        schemaId[0]) {
      std::wstring schemaWs = utf8ToWs(schemaId);
      raw = yamlSwitches(rimeDataDir_ + L"\\" + schemaWs + L".schema.yaml");
    }
  }
  for (const auto& name : saved) {
    for (const auto& sw : raw) {
      if (sw.name == name) {
        Menu::MenuSwitchDef d;
        d.name = sw.name;
        d.states = sw.states;
        defs.push_back(d);
        break;
      }
    }
  }
  return defs;
}

Menu::MenuState DaemonMenuService::gather(DaemonEngine& engine) {
  Menu::MenuState state;
  std::wstring root = appDataDir_.empty() ? appDataRoot() : appDataDir_;
  RimeRuntime& runtime = RimeRuntime::instance();

  // 开关 + 当前值
  state.switches = loadSwitchDefs(engine);
  {
    std::lock_guard<std::recursive_mutex> lock(runtime.mutex());
    for (const auto& sw : state.switches) {
      bool v = false;
      if (runtime.rimeGetOption != nullptr && engine.sessionId() != 0) {
        v = runtime.rimeGetOption(engine.sessionId(), sw.name.c_str()) != 0;
      }
      state.options[sw.name] = v;
    }
  }

  // 方案列表
  if (runtime.rimeGetSchemaList != nullptr && runtime.rimeFreeSchemaList !=
                                                  nullptr) {
    rimeapi::RimeSchemaList list = {};
    if (runtime.rimeGetSchemaList(&list)) {
      char current[256] = {};
      runtime.rimeGetCurrentSchema(engine.sessionId(), current,
                                   sizeof(current));
      for (size_t i = 0; i < list.size; ++i) {
        Menu::SchemaEntry e;
        e.id = list.list[i].schema_id ? list.list[i].schema_id : "";
        e.name = list.list[i].name ? list.list[i].name : "";
        e.current = !e.id.empty() && e.id == current;
        state.schemas.push_back(e);
      }
      runtime.rimeFreeSchemaList(&list);
    }
  }

  // 主题
  themeDefs_ = loadThemes();
  for (const auto& t : themeDefs_) {
    Menu::ThemeEntry e;
    e.id = t.id;
    e.label = t.label;
    state.themes.push_back(e);
  }

  // appearance_config.json
  Json::Value cfg = loadAppearanceConfig();
  state.theme = cfg.get("candidate_theme", "default").asString();
  // 默认行内预编辑（对齐 Go defaultStyle：InlinePreedit="composition"）。
  // 配置文件缺失/无此键时必须默认开，否则拼音会从光标处挪进候选窗
  //（2026-11-03 用户重装后实报"拼音和候选放一起了"）。
  state.inlinePreedit = cfg.get("inline_preedit", true).asBool();
  {
    // 镜像 Go effectiveCandidatePerRow()：竖排=1；横排=min(配置值, 总候选数)，
    // 总候选数只认 3/5/7/9，否则 7。竖排配置值 Go 写 1、本实现写 0，都认。
    int perRow = cfg.get("candidate_per_row", 9).asInt();
    int count = cfg.get("candidate_count", 7).asInt();
    if (count != 3 && count != 5 && count != 7 && count != 9) count = 7;
    state.candCount = count;
    state.horizontal = perRow > 1;
    if (state.horizontal &&
        perRow != 3 && perRow != 5 && perRow != 7 && perRow != 9) {
      perRow = 9;
    }
    state.perRowRaw = (perRow == 3 || perRow == 5 || perRow == 7 ||
                       perRow == 9)
                          ? perRow
                          : 9;
    state.perRow = state.horizontal ? (perRow < count ? perRow : count) : 7;
  }
  state.spacing = cfg.get("candidate_spacing", 20).asInt();
  state.fontPoint = cfg.get("font_point", 14).asInt();
  state.commentFontPoint =
      cfg.get("candidate_comment_font_point", 12).asInt();
  state.fontFace =
      cfg.get("font_face", "Microsoft YaHei UI").asString();
  // 存量配置里旧默认 "Segoe UI"（无 CJK 字形，GDI 回退成宋体）迁移为雅黑；
  // 菜单里仍可手动选回。2026-11-03 用户重启后实报"候选字都是宋体"。
  // 只在未打过迁移标记时做：否则用户主动选 Segoe UI 会在下次 gather 时
  // 被改回雅黑（选了不生效/重启就丢）。
  if (!cfg.get("font_face_migrated", false).asBool() &&
      state.fontFace == "Segoe UI") {
    state.fontFace = "Microsoft YaHei UI";
  }
  state.commentFontFace =
      cfg.get("candidate_comment_font_face", "Consolas").asString();
  state.bgColor = cfg.get("candidate_background_color", "#ffffff").asString();
  state.hlColor =
      cfg.get("candidate_highlight_color", "#c6ddf9").asString();
  state.textColor = cfg.get("candidate_text_color", "#000000").asString();
  state.hlTextColor =
      cfg.get("candidate_highlight_text_color", "#000000").asString();
  state.commentColor =
      cfg.get("candidate_comment_color", "#000000").asString();
  state.commentHlColor =
      cfg.get("candidate_comment_highlight_color", "#000000").asString();
  state.inputStateShared = cfg.get("input_state_shared", false).asBool();
  state.autoPairQuotes = cfg.get("auto_pair_quotes", false).asBool();
  state.semicolonSecond =
      cfg.get("semicolon_select_second", false).asBool();

  // 云剪贴板 / 启动器调试
  Json::Value cloud = readJsonFile(root + L"\\cloud_clipboard.json");
  state.cloudEnabled = cloud.get("enabled", false).asBool();
  std::wstring localRoot = localAppDataRoot();
  Json::Value launcher =
      readJsonFile(localRoot + L"\\MoqiLauncher.json");
  std::string level = launcher.get("logLevel", "error").asString();
  state.launcherDebug = (level == "debug" || level == "trace");
  return state;
}

void DaemonMenuService::applyEngine(DaemonEngine& engine,
                                    const Menu::CommandResult& r) {
  RimeRuntime& runtime = RimeRuntime::instance();
  if (!r.optionToggled.empty()) {
    {
      std::lock_guard<std::recursive_mutex> lock(runtime.mutex());
      if (runtime.rimeSetOption != nullptr && engine.sessionId() != 0) {
        int cur = runtime.rimeGetOption(engine.sessionId(),
                                        r.optionToggled.c_str());
        runtime.rimeSetOption(engine.sessionId(), r.optionToggled.c_str(),
                              cur ? 0 : 1);
      }
    }
    // Go toggleOption：共享四键之一变了 → 捕获当前值落盘，其他应用激活时
    // 跟随（未启用共享时为空操作）。
    syncSharedAfterOptionToggle(engine.sessionId());
  }
  if (!r.schemaSelected.empty()) {
    {
      std::lock_guard<std::recursive_mutex> lock(runtime.mutex());
      if (runtime.rimeSelectSchema != nullptr && engine.sessionId() != 0) {
        runtime.rimeSelectSchema(engine.sessionId(), r.schemaSelected.c_str());
      }
    }
    // Go selectSchemaByIDLocked：方案切换会重置选项 → 落盘 current_schema_id
    //（进程内引擎激活时跟随）+ 共享态重新套用。
    saveSyncedSchemaId(r.schemaSelected);
    applySharedInputState(engine.sessionId());
    syncSharedAfterOptionToggle(engine.sessionId());
    engine.applySchemaPageSize();
  }
}

void DaemonMenuService::applyThemeColors(Menu::MenuState& state) {
  for (const auto& t : themeDefs_) {
    if (t.id != state.theme) continue;
    const Json::Value& ap = t.appearance;
    if (!ap.isObject()) return;
    auto str = [&](const char* key, std::string& out) {
      if (ap[key].isString() && !ap[key].asString().empty()) {
        out = ap[key].asString();
      }
    };
    auto num = [&](const char* key, int& out) {
      if (ap[key].isInt() && ap[key].asInt() > 0) out = ap[key].asInt();
    };
    str("candidate_background_color", state.bgColor);
    str("candidate_highlight_color", state.hlColor);
    str("candidate_text_color", state.textColor);
    // Go applyThemeAppearanceToStyle：主题给正文色时注释色跟随（内置主题
    // 只定义四色）；主题显式给了注释色再用显式值覆盖。
    state.commentColor = state.textColor;
    str("candidate_highlight_text_color", state.hlTextColor);
    state.commentHlColor = state.hlTextColor;
    str("candidate_comment_color", state.commentColor);
    str("candidate_comment_highlight_color", state.commentHlColor);
    str("font_face", state.fontFace);
    str("candidate_comment_font_face", state.commentFontFace);
    num("font_point", state.fontPoint);
    num("candidate_comment_font_point", state.commentFontPoint);
    num("candidate_spacing", state.spacing);
    if (ap["inline_preedit"].isBool()) {
      state.inlinePreedit = ap["inline_preedit"].asBool();
    }
    if (ap["candidate_per_row"].isInt()) {
      const int perRow = ap["candidate_per_row"].asInt();
      state.horizontal = perRow > 1;
      int count = state.candCount;
      if (count != 3 && count != 5 && count != 7 && count != 9) count = 7;
      if (state.horizontal) {
        state.perRowRaw = (perRow == 3 || perRow == 5 || perRow == 7 ||
                           perRow == 9)
                              ? perRow
                              : 9;
        state.perRow = perRow < count ? perRow : count;
      } else {
        state.perRow = 7;
      }
    }
    return;
  }
}

// Go 退休：ON_ACTIVATE 本地应答。形状严格对齐 Go 实链抓包
//（F:\work\IME\go_init_golden.json）：4 按钮 + customizeUi + removePreservedKey。
Json::Value DaemonMenuService::buildActivatePayload(DaemonEngine& engine) {
  Json::Value payload(Json::objectValue);
  bool ascii = false;
  bool full = false;
  engine.uiModeState(&ascii, &full);
  // 图标目录：Go 用 <moqi-ime>\input_methods\rime\icons；安装布局另有
  // <moqi-ime>\icons 与 data\icons 两种历史位置。按存在性依次尝试，
  // 全不存在时回退 Go 路径（绝不发一个不存在的绝对路径——2026-11-03
  // 用户实报"中英切换小图标没有显示出来"，根因=这里取的是 data\icons）。
  std::wstring iconDir;
  if (!rimeDataDir_.empty()) {
    std::wstring rimeRoot = rimeDataDir_;
    const size_t slash = rimeRoot.find_last_of(L"\\/");
    if (slash != std::wstring::npos) rimeRoot = rimeRoot.substr(0, slash);
    const std::wstring candidates[] = {rimeDataDir_ + L"\\icons",
                                       rimeRoot + L"\\icons",
                                       installRoot_ + L"\\moqi-ime\\icons"};
    for (const auto& candidate : candidates) {
      if (::GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
        iconDir = candidate;
        break;
      }
    }
    if (iconDir.empty()) iconDir = rimeRoot + L"\\icons";
  }
  auto iconPath = [&iconDir](const wchar_t* name) {
    return wsToUtf8(iconDir + L"\\" + name);
  };
  const wchar_t* modeIcon =
      ascii ? (full ? L"eng_full_capsoff.ico" : L"eng_half_capsoff.ico")
            : (full ? L"chi_full_capsoff.ico" : L"chi_half_capsoff.ico");

  Json::Value buttons(Json::arrayValue);
  auto addButton = [&](const char* id, const wchar_t* icon, const char* text,
                       const char* tooltip, int commandId,
                       const char* type) {
    Json::Value b(Json::objectValue);
    b["id"] = id;
    b["icon"] = iconPath(icon);
    b["text"] = text;
    b["tooltip"] = tooltip;
    b["commandId"] = commandId;
    b["type"] = type;
    b["enable"] = true;
    b["toggled"] = false;
    buttons.append(b);
  };
  addButton("windows-mode-icon", modeIcon, "", "\xe4\xb8\xad\xe8\x8b\xb1\xe6"
            "\x96\x87\xe5\x88\x87\xe6\x8d\xa2", 1, "BUTTON_TYPE_UNSPECIFIED");
  addButton("switch-lang", ascii ? L"eng.ico" : L"chi.ico",
            "\xe4\xb8\xad\xe8\x8b\xb1\xe6\x96\x87\xe5\x88\x87\xe6\x8d\xa2",
            "\xe4\xb8\xad\xe8\x8b\xb1\xe6\x96\x87\xe5\x88\x87\xe6\x8d\xa2", 2,
            "BUTTON_TYPE_UNSPECIFIED");
  addButton("switch-shape", full ? L"full.ico" : L"half.ico",
            "\xe5\x85\xa8\xe5\x8d\x8a\xe8\xa7\x92\xe5\x88\x87\xe6\x8d\xa2",
            "\xe5\x85\xa8\xe8\xa7\x92\x2f\xe5\x8d\x8a\xe8\xa7\x92\xe5\x88\x87"
            "\xe6\x8d\xa2",
            3, "BUTTON_TYPE_UNSPECIFIED");
  addButton("settings", L"config.ico", "\xe8\xae\xbe\xe7\xbd\xae", "", 0,
            "BUTTON_TYPE_MENU");
  payload["addButton"] = buttons;

  // customizeUi（外观全量，同 Go customizeUIMap）。
  Menu::MenuState state = gather(engine);
  payload["customizeUi"] = buildCustomizeUi(state);

  // removePreservedKey：旧 Shift_R 保留键（Go 同款）。
  Json::Value removeKeys(Json::arrayValue);
  removeKeys.append("{9d9f5a64-1a18-47f0-9a4f-7cbf2f5a44d9}");
  payload["removePreservedKey"] = removeKeys;

  // Go onActivate 只 removePreservedKey，不 add：Ctrl+0 由 filterKeyDown
  // 的 handleCloudClipboardListHotkey 处理（支持用户在 json 里改热键）。
  // 这里若硬编码 add Ctrl+0，会抢占用户配置的其它热键（例如改成 Ctrl+9
  // 后按 Ctrl+0 仍被吞）。
  return payload;
}

namespace {
// Go loadAutoPairRules：%APPDATA%\Moqi\moqi_auto_pair_symbols.txt。
// 每行一对（空格/Tab 分隔，或恰好两个字符）；空/注释/重复跳过；
// 文件缺失或没有任何有效行时回退内置 8 对。
std::vector<std::pair<std::string, std::string>> loadAutoPairRules() {
  std::vector<std::pair<std::string, std::string>> rules;
  const std::wstring path = appDataRoot() + L"\\moqi_auto_pair_symbols.txt";
  std::ifstream in(path, std::ios::binary);
  if (in) {
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const std::string t = trim(line);
      if (t.empty() || t[0] == '#') continue;
      std::istringstream ss(t);
      std::string open, close;
      ss >> open >> close;
      if (close.empty()) {
        const std::wstring ws = utf8ToWs(t);
        if (ws.size() != 2) continue;
        open = wsToUtf8(std::wstring(1, ws[0]));
        close = wsToUtf8(std::wstring(1, ws[1]));
      }
      if (open.empty() || close.empty()) continue;
      bool dup = false;
      for (const auto& r : rules) {
        if (r.first == open && r.second == close) {
          dup = true;
          break;
        }
      }
      if (!dup) rules.emplace_back(open, close);
    }
  }
  if (rules.empty()) {
    rules = {{"\xe2\x80\x9c", "\xe2\x80\x9d"},
             {"\xe2\x80\x98", "\xe2\x80\x99"},
             {"\xe3\x80\x90", "\xe3\x80\x91"},
             {"\xe3\x80\x8a", "\xe3\x80\x8b"},
             {"<", ">"},
             {"(", ")"},
             {"\xef\xbc\x88", "\xef\xbc\x89"},
             {"\xe3\x80\x8c", "\xe3\x80\x8d"}};
  }
  return rules;
}
}  // namespace

Json::Value DaemonMenuService::buildCustomizeUi(const Menu::MenuState& state) {
  Json::Value ui(Json::objectValue);
  ui["candFontName"] = state.fontFace;
  ui["candFontSize"] = state.fontPoint;
  // 竖排必须发 1（Go effectiveCandidatePerRow：竖排=1），DLL 用 1 识别单列；
  // 2026-11-03 验收实测发现竖排时发了每行数值。
  ui["candPerRow"] = state.horizontal ? state.perRow : 1;
  ui["candUseCursor"] = true;
  ui["inlinePreedit"] = state.inlinePreedit;
  // 总候选数：DLL 侧进程内引擎据此即时改 page size（菜单命令的实时生效）。
  ui["candidateCount"] = state.candCount;
  ui["candBackgroundColor"] = state.bgColor;
  ui["candHighlightColor"] = state.hlColor;
  ui["candTextColor"] = state.textColor;
  ui["candHighlightTextColor"] = state.hlTextColor;
  ui["candCommentFontSize"] = state.commentFontPoint;
  ui["candCommentFontName"] = state.commentFontFace;
  ui["candCommentColor"] = state.commentColor;
  ui["candCommentHighlightColor"] = state.commentHlColor;
  ui["candSpacing"] = state.spacing;
  ui["autoPairQuotes"] = state.autoPairQuotes;
  ui["semicolonSelectSecond"] = state.semicolonSecond;
  // autoPairRules：读用户文件（菜单"打开成对符号设置"编辑的就是它）。
  // 旧实现固定 8 条，用户改文件后永远不生效——死配置。
  Json::Value rules(Json::arrayValue);
  for (const auto& pair : loadAutoPairRules()) {
    Json::Value r(Json::objectValue);
    r["open"] = pair.first;
    r["close"] = pair.second;
    rules.append(r);
  }
  ui["autoPairRules"] = rules;
  return ui;
}

void DaemonMenuService::openCommandTarget(int id) {
  std::wstring root = appDataDir_.empty() ? appDataRoot() : appDataDir_;
  // 文件模板类命令：不存在则从安装 templates\ 拷模板（缺模板用回退头），再打开。
  auto openTemplated = [&](const wchar_t* fileName, const char* fallback) {
    std::wstring path = root + L"\\" + fileName;
    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
      // 模板随包在 rime\templates（安装布局实测）；data\templates 是历史
      // 位置，两处都试。都没有时用 fallback 头生成——绝不留死菜单项
      //（2026-11-03 用户实报"打开超级简拼点了没反应"）。
      std::string content;
      std::wstring rimeRoot = rimeDataDir_;
      const size_t slash = rimeRoot.find_last_of(L"\\/");
      if (slash != std::wstring::npos) rimeRoot = rimeRoot.substr(0, slash);
      const std::wstring tmplDirs[] = {rimeDataDir_ + L"\\templates",
                                       rimeRoot + L"\\templates"};
      for (const auto& dir : tmplDirs) {
        std::ifstream in(dir + L"\\" + fileName, std::ios::binary);
        if (!in) continue;
        std::ostringstream ss;
        ss << in.rdbuf();
        content = ss.str();
        if (!content.empty()) break;
      }
      if (content.empty() && fallback) content = fallback;
      if (!content.empty()) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << content;
      }
    }
    // 随包 Notepad4 优先（语法高亮），见 openTextInBundledEditor。
    openTextInBundledEditor(installRoot_, path);
  };
  switch (id) {
    case 18: // 打开置顶短语
      openTemplated(L"moqi_custom_phrase.txt",
                    "# 置顶短语\n# 词汇\t编码\t权重\n");
      break;
    case 19: // 打开超级简拼
      openTemplated(L"moqi_super_abbrev.txt",
                    "# 超级简拼\n# 词汇\t简拼\n");
      break;
    case 222: // 打开成对符号设置（Go ensureAutoPairSymbolsFileExists 同款默认）
      openTemplated(L"moqi_auto_pair_symbols.txt",
                    "# 成对符号\n"
                    "# 每行一对符号\n"
                    "\xe2\x80\x9c\xe2\x80\x9d\n"
                    "\xe2\x80\x98\xe2\x80\x99\n"
                    "\xe3\x80\x90\xe3\x80\x91\n"
                    "\xe3\x80\x8a\xe3\x80\x8b\n"
                    "<>\n"
                    "()\n"
                    "\xef\xbc\x88\xef\xbc\x89\n"
                    "\xe3\x80\x8c\xe3\x80\x8d\n");
      break;
    case Menu::ID_AI_CONFIG: {
      // 打开引擎实际读取的 ai_config.json；不存在则先从安装目录拷一份随包
      // 模板（内含 MQ- key 与五动作，用户只差填 base_url）。
      std::wstring path = AiConfigPathUserW();
      if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::ifstream in(AiConfigBundledPathW(), std::ios::binary);
        if (in) {
          std::ostringstream ss;
          ss << in.rdbuf();
          std::ofstream out(path, std::ios::binary);
          out << ss.str();
        }
      }
      if (path.empty()) break;
      // 随包 Notepad4 优先（JSON 高亮），见 openTextInBundledEditor。
      openTextInBundledEditor(installRoot_, path);
      break;
    }
    case Menu::ID_HELP_DOCS:
      shellOpen(L"https://moqiyinxing.chunqiujinjing.com/index", true);
      break;
    case Menu::ID_DISCUSSIONS:
      shellOpen(
          L"https://github.com/gaboolic/moqi-im-windows/discussions", true);
      break;
    case Menu::ID_USER_DIR: {
      Json::Value ssc = readJsonFile(root + L"\\scheme_set.json");
      std::wstring cur =
          utf8ToWs(ssc.get("current", "Rime").asString());
      // 全新用户/清过配置的机器上方案集目录可能还不存在：先建再打开，
      // 否则菜单项是死的（2026-11-03 Go 退休后不再有后端替我们建目录）。
      const std::wstring dir = root + L"\\" + cur;
      ::CreateDirectoryW(dir.c_str(), nullptr);
      shellOpen(dir, false);
      break;
    }
    case Menu::ID_SHARED_DIR:
      shellOpen(rimeDataDir_, false);
      break;
    case Menu::ID_SYNC_DIR: {
      // 新架构真正的主词典在这里；方案集目录的 sync 是 Go 时代旧位置，
      // 打开它只会看到空/过期内容。
      const std::wstring dir = root + L"\\UserDictMaster\\sync";
      ::CreateDirectoryW(dir.c_str(), nullptr);
      shellOpen(dir, false);
      break;
    }
    case Menu::ID_LOG_DIR: {
      std::wstring logDir = localAppDataRoot() + L"\\Log";
      ::CreateDirectoryW(logDir.c_str(), nullptr);
      shellOpen(logDir, false);
      break;
    }
    default:
      break;
  }
}

} // namespace Moqi
