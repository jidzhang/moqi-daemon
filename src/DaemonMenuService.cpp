// 5b 菜单应用层实现。采集/落盘/打开；纯逻辑见 DaemonMenuCore（单测锁定）。
// include 纪律：RimeCore.h 链开篇（见 ChildProxy.h 注释）。
#include "RimeCore.h"

#include <Shellapi.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

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

DaemonMenuService::DaemonMenuService(std::wstring installRoot,
                                     std::wstring rimeDataDir,
                                     std::wstring appDataDir)
    : installRoot_(std::move(installRoot)),
      rimeDataDir_(std::move(rimeDataDir)),
      appDataDir_(std::move(appDataDir)) {}

Json::Value DaemonMenuService::loadAppearanceConfig() {
  std::wstring root = appDataDir_.empty() ? appDataRoot() : appDataDir_;
  return readJsonFile(root + L"\\appearance_config.json");
}

void DaemonMenuService::persistAppearance(const Menu::MenuState& state) {
  std::wstring root = appDataDir_.empty() ? appDataRoot() : appDataDir_;
  Json::Value cfg = loadAppearanceConfig();
  if (!cfg.isObject()) cfg = Json::Value(Json::objectValue);
  cfg["candidate_theme"] = state.theme;
  cfg["inline_preedit"] = state.inlinePreedit;
  cfg["candidate_per_row"] = state.horizontal ? state.perRow : 0;
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
  cfg["input_state_shared"] = state.inputStateShared;
  cfg["auto_pair_quotes"] = state.autoPairQuotes;
  cfg["semicolon_select_second"] = state.semicolonSecond;
  writeJsonFile(root + L"\\appearance_config.json", cfg);
}

std::vector<ThemeDefinition> DaemonMenuService::loadThemes() {
  // 内置主题随包（appearance_themes.json），用户主题在 appdata；同 id 内置
  // 优先，最终按 label（UTF-8 字节序）再 id 排序 —— 与 Go 黄金一致。
  std::vector<ThemeDefinition> out;
  auto loadFrom = [&](const std::wstring& path, bool builtin) {
    Json::Value j = readJsonFile(path);
    const Json::Value& arr = j["themes"];
    if (!arr.isArray()) return;
    for (const auto& t : arr) {
      ThemeDefinition d;
      d.id = t.get("id", "").asString();
      d.label = t.get("label", "").asString();
      if (d.id.empty() || d.label.empty()) continue;
      const Json::Value& ap = t["appearance"];
      if (ap.isObject()) {
        d.bgColor = ap.get("candidate_background_color", "").asString();
        d.hlColor = ap.get("candidate_highlight_color", "").asString();
        d.textColor = ap.get("candidate_text_color", "").asString();
        d.hlTextColor =
            ap.get("candidate_highlight_text_color", "").asString();
      }
      // 同 id 去重：内置赢
      for (auto& e : out) {
        if (e.id == d.id) {
          if (builtin) e = d;
          goto next;
        }
      }
      out.push_back(d);
    next:;
    }
  };
  std::wstring root = appDataDir_.empty() ? appDataRoot() : appDataDir_;
  loadFrom(rimeDataDir_ + L"\\appearance_themes.json", true);
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
    std::lock_guard<std::mutex> lock(runtime.mutex());
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

  // 方案集：appdata 下非点目录（default "Rime" 恒在首位），当前值读
  // scheme_set.json。
  {
    std::vector<std::wstring> names;
    HANDLE hFind = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAW fd = {};
    hFind = ::FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
      do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        std::wstring n = fd.cFileName;
        if (n == L"Rime") continue;
        names.push_back(n);
      } while (::FindNextFileW(hFind, &fd));
      ::FindClose(hFind);
    }
    std::sort(names.begin(), names.end(), [](const std::wstring& a,
                                             const std::wstring& b) {
      std::string la = wsToUtf8(a), lb = wsToUtf8(b);
      std::transform(la.begin(), la.end(), la.begin(), ::tolower);
      std::transform(lb.begin(), lb.end(), lb.begin(), ::tolower);
      if (la != lb) return la < lb;
      return a < b;
    });
    Json::Value ssc = readJsonFile(root + L"\\scheme_set.json");
    std::string current = ssc.get("current", "Rime").asString();
    auto add = [&](const std::wstring& n) {
      Menu::SchemeSetEntry e;
      e.name = wsToUtf8(n);
      e.current = e.name == current;
      state.schemeSets.push_back(e);
    };
    add(L"Rime");
    for (auto& n : names) add(n);
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
  state.theme = cfg.get("candidate_theme", "").asString();
  state.inlinePreedit = cfg.get("inline_preedit", false).asBool();
  {
    // 镜像 Go effectiveCandidatePerRow()：竖排=1；横排=min(配置值, 总候选数)，
    // 总候选数只认 3/5/7/9，否则 7。
    int perRow = cfg.get("candidate_per_row", 9).asInt();
    int count = cfg.get("candidate_count", 7).asInt();
    if (count != 3 && count != 5 && count != 7 && count != 9) count = 7;
    state.candCount = count;
    if (perRow != 3 && perRow != 5 && perRow != 7 && perRow != 9) perRow = 9;
    state.horizontal = perRow != 0; // 竖排时配置存 0（持久化仍写原值）
    state.perRow = state.horizontal ? (perRow < count ? perRow : count) : 7;
  }
  state.spacing = cfg.get("candidate_spacing", 20).asInt();
  state.fontPoint = cfg.get("font_point", 14).asInt();
  state.commentFontPoint =
      cfg.get("candidate_comment_font_point", 12).asInt();
  state.fontFace = cfg.get("font_face", "Segoe UI").asString();
  state.commentFontFace =
      cfg.get("candidate_comment_font_face", "Consolas").asString();
  state.bgColor = cfg.get("candidate_background_color", "#ffffff").asString();
  state.hlColor =
      cfg.get("candidate_highlight_color", "#c6ddf9").asString();
  state.textColor = cfg.get("candidate_text_color", "#000000").asString();
  state.hlTextColor =
      cfg.get("candidate_highlight_text_color", "#000000").asString();
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
    std::lock_guard<std::mutex> lock(runtime.mutex());
    if (runtime.rimeSetOption != nullptr && engine.sessionId() != 0) {
      int cur = runtime.rimeGetOption(engine.sessionId(),
                                      r.optionToggled.c_str());
      runtime.rimeSetOption(engine.sessionId(), r.optionToggled.c_str(),
                            cur ? 0 : 1);
    }
  }
  if (!r.schemaSelected.empty()) {
    std::lock_guard<std::mutex> lock(runtime.mutex());
    if (runtime.rimeSelectSchema != nullptr && engine.sessionId() != 0) {
      runtime.rimeSelectSchema(engine.sessionId(), r.schemaSelected.c_str());
    }
  }
}

void DaemonMenuService::applyThemeColors(Menu::MenuState& state) {
  for (const auto& t : themeDefs_) {
    if (t.id == state.theme) {
      if (!t.bgColor.empty()) state.bgColor = t.bgColor;
      if (!t.hlColor.empty()) state.hlColor = t.hlColor;
      if (!t.textColor.empty()) state.textColor = t.textColor;
      if (!t.hlTextColor.empty()) state.hlTextColor = t.hlTextColor;
      return;
    }
  }
}

void DaemonMenuService::openCommandTarget(int id) {
  std::wstring root = appDataDir_.empty() ? appDataRoot() : appDataDir_;
  // 文件模板类命令：不存在则从安装 templates\ 拷模板（缺模板用回退头），再打开。
  auto openTemplated = [&](const wchar_t* fileName, const char* fallback) {
    std::wstring path = root + L"\\" + fileName;
    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
      std::wstring tmpl = rimeDataDir_ + L"\\templates\\" + fileName;
      std::string content;
      std::ifstream in(tmpl, std::ios::binary);
      if (in) {
        std::ostringstream ss;
        ss << in.rdbuf();
        content = ss.str();
      }
      if (content.empty() && fallback) content = fallback;
      if (!content.empty()) {
        std::ofstream out(path, std::ios::binary);
        out << content;
      }
    }
    shellOpen(path, false);
  };
  switch (id) {
    case 18: // 打开置顶短语
      openTemplated(L"moqi_custom_phrase.txt",
                    "# 置顶短语\n# 词汇\t编码\t权重\n");
      break;
    case 19: // 打开超级简拼
      openTemplated(L"moqi_super_abbrev.txt", nullptr);
      break;
    case 222: // 打开成对符号设置
      openTemplated(L"moqi_auto_pair_symbols.txt", nullptr);
      break;
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
      shellOpen(root + L"\\" + cur, false);
      break;
    }
    case Menu::ID_SHARED_DIR:
      shellOpen(rimeDataDir_, false);
      break;
    case Menu::ID_SYNC_DIR: {
      Json::Value ssc = readJsonFile(root + L"\\scheme_set.json");
      std::wstring cur =
          utf8ToWs(ssc.get("current", "Rime").asString());
      ::CreateDirectoryW((root + L"\\" + cur + L"\\sync").c_str(), nullptr);
      shellOpen(root + L"\\" + cur + L"\\sync", false);
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
