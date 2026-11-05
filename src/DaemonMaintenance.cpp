// 维护命令实现（Go 退休，2026-11-03）：刷新配置 / 更新配置 / 导入皮肤。
// 这些命令低频、非按键路径：允许阻塞（调用方放后台线程），但绝不弹控制台。
// include 纪律：RimeCore.h 链开篇（见 ChildProxy.h 注释）。
#include "RimeCore.h"

#include <Windows.h>
#include <commdlg.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#include "DaemonMaintenance.h"

namespace Moqi {
namespace {

std::string wsToUtf8(const std::wstring& ws) {
  if (ws.empty()) return std::string();
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(),
                                      nullptr, 0, nullptr, nullptr);
  std::string out(n, 0);
  ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), &out[0], n,
                        nullptr, nullptr);
  return out;
}

std::wstring utf8ToWs(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n =
      ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring out(n, 0);
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
  return out;
}

std::string trim(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  const size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  return s;
}

std::wstring appDataRoot() { return moqiAppDataDir(); }

// 当前方案集目录（%APPDATA%\Moqi\<scheme_set.json current>，默认 Rime）。
std::wstring currentSchemeDir() {
  const std::wstring root = appDataRoot();
  if (root.empty()) return L"";
  return root + L"\\" + currentSchemeSetDirName();
}

std::wstring quoteArg(const std::wstring& s) {
  return L"\"" + s + L"\"";
}

// ------------------------------------------------------------------ json --

Json::Value readJson(const std::wstring& path) {
  Json::Value v;
  std::ifstream in(path, std::ios::binary);
  if (!in) return v;
  std::string errs;
  Json::CharReaderBuilder builder;
  Json::parseFromStream(builder, in, &v, &errs);
  return v;
}

bool writeJson(const std::wstring& path, const Json::Value& v) {
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

// UTF-8 或 ANSI 字节流 → 宽串（git 输出编码不保证 UTF-8）。
std::wstring bytesToWide(const std::string& bytes) {
  if (bytes.empty()) return std::wstring();
  int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                (int)bytes.size(), nullptr, 0);
  UINT cp = CP_UTF8;
  if (n <= 0) {
    cp = CP_ACP;
    n = ::MultiByteToWideChar(cp, 0, bytes.data(), (int)bytes.size(), nullptr,
                              0);
    if (n <= 0) return std::wstring();
  }
  std::wstring out(n, 0);
  ::MultiByteToWideChar(cp, 0, bytes.data(), (int)bytes.size(), &out[0], n);
  return out;
}

// 截断到 maxLen 字节并保证不劈开 UTF-8 多字节序列。
std::string truncateUtf8(const std::string& s, size_t maxLen) {
  if (s.size() <= maxLen) return s;
  size_t cut = maxLen;
  while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) --cut;
  return s.substr(0, cut) + "...";
}

// ------------------------------------------------------- child processes --

struct ProcessRun {
  bool started = false;
  DWORD exitCode = 1;
  std::string output;
};

// 同步跑一个带输出捕获的子进程（无控制台窗口）。输出会等到进程退出后
// 继续排空，避免管道写满把子进程卡死。
ProcessRun runProcessCapture(const std::wstring& commandLine) {
  ProcessRun run;
  SECURITY_ATTRIBUTES sa = {};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE readPipe = nullptr;
  HANDLE writePipe = nullptr;
  if (!::CreatePipe(&readPipe, &writePipe, &sa, 0)) return run;
  ::SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  HANDLE nul = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &sa,
                             OPEN_EXISTING, 0, nullptr);
  si.hStdOutput = writePipe;
  si.hStdError = writePipe;
  si.hStdInput = nul;

  PROCESS_INFORMATION pi = {};
  std::wstring cmd = commandLine; // CreateProcessW 需要可写缓冲
  const BOOL ok = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  ::CloseHandle(writePipe);
  if (nul != INVALID_HANDLE_VALUE) ::CloseHandle(nul);
  if (!ok) {
    ::CloseHandle(readPipe);
    return run;
  }
  run.started = true;

  char buffer[4096];
  for (;;) {
    DWORD available = 0;
    if (::PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) &&
        available > 0) {
      DWORD got = 0;
      if (::ReadFile(readPipe, buffer, sizeof(buffer), &got, nullptr) &&
          got > 0) {
        run.output.append(buffer, got);
        continue;
      }
    }
    if (::WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
      // 进程退出后把管道里剩余的输出读干净。
      for (;;) {
        DWORD tail = 0;
        if (!::PeekNamedPipe(readPipe, nullptr, 0, nullptr, &tail, nullptr) ||
            tail == 0) {
          break;
        }
        DWORD got = 0;
        if (!::ReadFile(readPipe, buffer, sizeof(buffer), &got, nullptr) ||
            got == 0) {
          break;
        }
        run.output.append(buffer, got);
      }
      break;
    }
  }
  ::GetExitCodeProcess(pi.hProcess, &run.exitCode);
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);
  ::CloseHandle(readPipe);
  return run;
}

std::wstring findGitExe() {
  wchar_t buf[MAX_PATH] = {};
  const DWORD n =
      ::SearchPathW(nullptr, L"git.exe", nullptr, MAX_PATH, buf, nullptr);
  if (n == 0 || n >= MAX_PATH) return std::wstring();
  return std::wstring(buf);
}

bool samePath(const std::wstring& a, const std::wstring& b) {
  auto norm = [](std::wstring s) {
    std::replace(s.begin(), s.end(), L'/', L'\\');
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    return s;
  };
  return ::_wcsicmp(norm(a).c_str(), norm(b).c_str()) == 0;
}

std::vector<std::string> nonEmptyLines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    line = trim(line);
    if (!line.empty()) lines.push_back(line);
  }
  return lines;
}

bool gitIsRepo(const std::wstring& git, const std::wstring& dir) {
  const ProcessRun run = runProcessCapture(
      quoteArg(git) + L" -C " + quoteArg(dir) + L" rev-parse --show-toplevel");
  if (!run.started || run.exitCode != 0) return false;
  const std::wstring top = bytesToWide(trim(run.output));
  if (top.empty()) return false;
  return samePath(top, dir);
}

// ---------------------------------------------------------------- deploy --

bool runFullRedeployImpl(std::string* message) {
  RimeRuntime& runtime = RimeRuntime::instance();
  if (!runtime.ready() || runtime.inprocUserDir().empty()) {
    if (message) *message = "引擎尚未就绪";
    return false;
  }
  if (runtime.rimeStartMaintenance == nullptr ||
      runtime.rimeJoinMaintenanceThread == nullptr) {
    if (message) *message = "当前引擎不支持重新部署";
    return false;
  }
  // 1) 重新灌 in-proc patch（用户可能改过方案集目录里的 custom yaml）。
  const std::wstring schemeDir = currentSchemeDir();
  // 单引擎架构：daemon 的用户目录就是共享目录（%APPDATA%\Moqi\<方案集>），
  // 重部署直接读用户放进去的整包配置，不再需要同步/挂载步骤。
  if (!schemeDir.empty() &&
      !std::filesystem::exists(schemeDir + L"\\default.yaml")) {
    const std::string patch = buildInProcCustomPatch(
        schemeDir, utf8ToWs(runtime.sharedDataDirUtf8()));
    const std::wstring patchPath =
        runtime.inprocUserDir() + L"\\rime_frost.custom.yaml";
    if (readFileUtf8(patchPath) != patch) {
      writeFileUtf8(patchPath, patch);
    }
  }
  // 2) 全量 maintenance（full check）并等完成。
  bool started = false;
  {
    std::lock_guard<std::recursive_mutex> lock(runtime.mutex());
    started = runtime.rimeStartMaintenance(1) != 0;
    runtime.rimeJoinMaintenanceThread();
  }
  if (message) *message = started ? "重新部署成功" : "重新部署失败";
  return started;
}

// ------------------------------------------------------------------ skin --

// 极简 YAML 映射解析（只服务小狼毫皮肤：嵌套 map + 标量，忽略列表）。
Json::Value parseYamlMaps(const std::string& text) {
  Json::Value root(Json::objectValue);
  struct Level {
    int indent;
    Json::Value* node;
  };
  std::vector<Level> stack;
  stack.push_back({-1, &root});

  std::istringstream in(text);
  std::string line;
  bool first = true;
  bool inList = false;
  int listIndent = -1;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (first) {
      first = false;
      if (line.size() >= 3 && (unsigned char)line[0] == 0xEF &&
          (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) {
        line = line.substr(3);
      }
    }
    size_t i = 0;
    int indent = 0;
    for (; i < line.size(); ++i) {
      if (line[i] == ' ') {
        ++indent;
      } else if (line[i] == '\t') {
        indent += 2;
      } else {
        break;
      }
    }
    std::string rest = line.substr(i);
    if (rest.empty() || rest[0] == '#') continue;
    if (rest.rfind("---", 0) == 0 || rest.rfind("...", 0) == 0) continue;
    if (rest[0] == '-') {
      inList = true;
      listIndent = indent;
      continue;
    }
    if (inList) {
      if (indent > listIndent) continue;
      inList = false;
    }
    // 查找键分隔冒号（忽略引号内的冒号，冒号后必须是空白/行尾）。
    size_t colon = std::string::npos;
    bool inSingle = false;
    bool inDouble = false;
    for (size_t k = 0; k < rest.size(); ++k) {
      const char c = rest[k];
      if (c == '\'' && !inDouble) {
        inSingle = !inSingle;
      } else if (c == '"' && !inSingle) {
        inDouble = !inDouble;
      } else if (c == ':' && !inSingle && !inDouble &&
                 (k + 1 >= rest.size() || rest[k + 1] == ' ' ||
                  rest[k + 1] == '\t')) {
        colon = k;
        break;
      }
    }
    if (colon == std::string::npos) continue;
    std::string key = trim(rest.substr(0, colon));
    std::string value = trim(rest.substr(colon + 1));
    auto unquote = [](std::string v) {
      if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') ||
                            (v.front() == '\'' && v.back() == '\''))) {
        return v.substr(1, v.size() - 2);
      }
      return v;
    };
    key = unquote(key);
    // 去行尾注释（引号外、且 # 前有空白）。
    {
      bool s1 = false;
      bool d1 = false;
      for (size_t k = 0; k < value.size(); ++k) {
        const char c = value[k];
        if (c == '\'' && !d1) {
          s1 = !s1;
        } else if (c == '"' && !s1) {
          d1 = !d1;
        } else if (c == '#' && !s1 && !d1 &&
                   (k == 0 || value[k - 1] == ' ' || value[k - 1] == '\t')) {
          value = trim(value.substr(0, k));
          break;
        }
      }
    }
    value = unquote(value);
    while (stack.size() > 1 && stack.back().indent >= indent) {
      stack.pop_back();
    }
    Json::Value* parent = stack.back().node;
    if (value.empty()) {
      (*parent)[key] = Json::Value(Json::objectValue);
      stack.push_back({indent, &(*parent)[key]});
    } else {
      (*parent)[key] = value;
    }
  }
  return root;
}

std::string normalizeHexColor(std::string value) {
  value = toLower(trim(value));
  if (value.empty()) return "";
  if (value[0] != '#') value = "#" + value;
  if (value.size() != 7) return "";
  for (size_t i = 1; i < value.size(); ++i) {
    if (!std::isxdigit((unsigned char)value[i])) return "";
  }
  return value;
}

long long parseIntText(const std::string& raw) {
  const std::string v = trim(raw);
  if (v.empty()) return 0;
  try {
    if (v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
      return std::stoll(v.substr(2), nullptr, 16);
    }
    return std::stoll(v, nullptr, 10);
  } catch (...) {
    return 0;
  }
}

// 小狼毫颜色：默认 abgr（0xBBGGRR），可被 color_format 覆盖。
std::string weaselColorToHex(const Json::Value& value,
                             const std::string& colorFormat) {
  if (!value.isString()) return "";
  std::string raw = trim(value.asString());
  if (raw.empty()) return "";
  if (raw[0] == '#') return normalizeHexColor(raw);
  const bool hex = raw.size() > 2 && raw[0] == '0' &&
                   (raw[1] == 'x' || raw[1] == 'X');
  long long v = parseIntText(raw);
  if (!hex) {
    // 纯十进制颜色（少数皮肤）同规则处理。
  }
  if (v < 0) v &= 0xffffffffLL;
  // 全透明色（阴影等）跳过。
  if ((v & 0xffffffLL) == 0 && v > 0xffffffLL) return "";
  if (v == 0) return "#000000";
  int r = 0;
  int g = 0;
  int b = 0;
  const std::string fmt = toLower(colorFormat);
  if (fmt == "argb") {
    r = (int)((v >> 16) & 0xff);
    g = (int)((v >> 8) & 0xff);
    b = (int)(v & 0xff);
  } else if (fmt == "rgba") {
    r = (int)((v >> 24) & 0xff);
    g = (int)((v >> 16) & 0xff);
    b = (int)((v >> 8) & 0xff);
  } else { // abgr / bgr: 0xBBGGRR
    r = (int)(v & 0xff);
    g = (int)((v >> 8) & 0xff);
    b = (int)((v >> 16) & 0xff);
  }
  char buf[16] = {};
  std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
  return buf;
}

Json::Value firstExisting(const Json::Value& map,
                          std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    if (map.isObject() && map.isMember(key) && !map[key].isNull()) {
      return map[key];
    }
  }
  return Json::Value();
}

std::string firstText(const Json::Value& map,
                      std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    if (map.isObject() && map[key].isString()) {
      const std::string v = trim(map[key].asString());
      if (!v.empty()) return v;
    }
  }
  return "";
}

int intOf(const Json::Value& v) {
  if (v.isInt()) return v.asInt();
  if (v.isString()) return (int)parseIntText(v.asString());
  return 0;
}

bool boolOf(const Json::Value& v, bool* out) {
  if (v.isBool()) {
    *out = v.asBool();
    return true;
  }
  if (v.isString()) {
    const std::string s = toLower(trim(v.asString()));
    if (s == "true" || s == "yes" || s == "1") {
      *out = true;
      return true;
    }
    if (s == "false" || s == "no" || s == "0") {
      *out = false;
      return true;
    }
  }
  return false;
}

// style + 选中配色 → appearance_config 片段（键名与产品配置一致）。
Json::Value weaselStyleToAppearance(const Json::Value& style,
                                    const Json::Value& scheme) {
  Json::Value ap(Json::objectValue);
  std::string colorFormat = firstText(scheme, {"color_format"});
  if (colorFormat.empty()) colorFormat = firstText(style, {"color_format"});
  const Json::Value layout =
      style.isObject() ? style["layout"] : Json::Value();

  const std::string fontFace = firstText(style, {"font_face"});
  if (!fontFace.empty()) ap["font_face"] = fontFace;
  if (style.isObject() && style["font_point"].isString()) {
    const int v = intOf(style["font_point"]);
    if (v > 0) ap["font_point"] = v;
  }
  const std::string commentFontFace = firstText(style, {"comment_font_face"});
  if (!commentFontFace.empty()) ap["candidate_comment_font_face"] = commentFontFace;
  if (style.isObject() && style["comment_font_point"].isString()) {
    const int v = intOf(style["comment_font_point"]);
    if (v > 0) ap["candidate_comment_font_point"] = v;
  }
  bool flag = false;
  if (style.isObject() && boolOf(style["inline_preedit"], &flag)) {
    ap["inline_preedit"] = flag;
  }
  if (style.isObject() && boolOf(style["horizontal"], &flag)) {
    ap["candidate_per_row"] = flag ? 9 : 1;
  }
  {
    const Json::Value spacing = firstExisting(
        style, {"candidate_spacing", "spacing"});
    const Json::Value layoutSpacing =
        firstExisting(layout, {"candidate_spacing", "spacing"});
    const Json::Value chosen = spacing.isNull() ? layoutSpacing : spacing;
    if (!chosen.isNull()) {
      ap["candidate_spacing"] = intOf(chosen);
    }
  }

  auto color = [&](std::initializer_list<const char*> keys) {
    return weaselColorToHex(firstExisting(scheme, keys), colorFormat);
  };
  const std::string bg = color({"back_color", "candidate_back_color"});
  if (!bg.empty()) ap["candidate_background_color"] = bg;
  const std::string hl =
      color({"hilited_candidate_back_color", "hilited_back_color"});
  if (!hl.empty()) ap["candidate_highlight_color"] = hl;
  const std::string text = color({"candidate_text_color", "text_color"});
  if (!text.empty()) ap["candidate_text_color"] = text;
  const std::string hlText =
      color({"hilited_candidate_text_color", "hilited_text_color"});
  if (!hlText.empty()) ap["candidate_highlight_text_color"] = hlText;
  const std::string comment = color(
      {"comment_text_color", "candidate_text_color", "text_color"});
  if (!comment.empty()) ap["candidate_comment_color"] = comment;
  const std::string commentHl = color({"hilited_comment_text_color",
                                       "hilited_candidate_text_color",
                                       "hilited_text_color"});
  if (!commentHl.empty()) ap["candidate_comment_highlight_color"] = commentHl;
  return ap;
}

// 合并 patch 里的 style 覆盖（"style/xxx" 键 + patch.style 子映射）。
Json::Value mergeStylePatch(const Json::Value& style, const Json::Value& patch) {
  Json::Value merged = style.isObject() ? style : Json::Value(Json::objectValue);
  if (!patch.isObject()) return merged;
  if (patch["style"].isObject()) {
    for (auto it = patch["style"].begin(); it != patch["style"].end(); ++it) {
      merged[it.key().asString()] = *it;
    }
  }
  for (auto it = patch.begin(); it != patch.end(); ++it) {
    const std::string key = it.key().asString();
    if (key.rfind("style/", 0) == 0) {
      merged[key.substr(6)] = *it;
    }
  }
  return merged;
}

Json::Value mergeSchemePatches(const Json::Value& schemes,
                               const Json::Value& patch) {
  Json::Value merged =
      schemes.isObject() ? schemes : Json::Value(Json::objectValue);
  if (!patch.isObject()) return merged;
  if (patch["preset_color_schemes"].isObject()) {
    for (auto it = patch["preset_color_schemes"].begin();
         it != patch["preset_color_schemes"].end(); ++it) {
      merged[it.key().asString()] = *it;
    }
  }
  for (auto it = patch.begin(); it != patch.end(); ++it) {
    const std::string key = it.key().asString();
    const std::string prefix = "preset_color_schemes/";
    if (key.rfind(prefix, 0) != 0) continue;
    const std::string rest = key.substr(prefix.size());
    const size_t slash = rest.find('/');
    if (slash == std::string::npos) {
      merged[rest] = *it;
      continue;
    }
    const std::string name = rest.substr(0, slash);
    const std::string field = rest.substr(slash + 1);
    if (!merged[name].isObject()) merged[name] = Json::Value(Json::objectValue);
    merged[name][field] = *it;
  }
  return merged;
}

std::string normalizeThemeId(const std::string& id) { return toLower(trim(id)); }

bool mergeAppearanceConfig(const Json::Value& patch,
                           const std::wstring& root) {
  const std::wstring base = root.empty() ? appDataRoot() : root;
  const std::wstring path = base + L"\\appearance_config.json";
  Json::Value cfg = readJson(path);
  if (!cfg.isObject()) cfg = Json::Value(Json::objectValue);
  for (auto it = patch.begin(); it != patch.end(); ++it) {
    cfg[it.key().asString()] = *it;
  }
  return writeJson(path, cfg);
}

bool upsertImportedThemes(const Json::Value& newThemes,
                          const std::string& sourceFile,
                          const std::wstring& root) {
  const std::wstring base = root.empty() ? appDataRoot() : root;
  const std::wstring path = base + L"\\appearance_themes.json";
  Json::Value file = readJson(path);
  Json::Value list(Json::arrayValue);
  if (file.isObject() && file["themes"].isArray()) {
    list = file["themes"];
  }
  std::vector<std::string> ids(list.size());
  for (Json::ArrayIndex i = 0; i < list.size(); ++i) {
    // 存量文件里的 themes 元素可能被手改成非对象——安全取值不抛。
    ids[i] = normalizeThemeId(jsonStringOf(list[i], "id"));
  }
  for (Json::ArrayIndex i = 0; i < newThemes.size(); ++i) {
    Json::Value theme = newThemes[i];
    theme["id"] = normalizeThemeId(jsonStringOf(theme, "id"));
    theme["source"] = "imported";
    theme["source_file"] = sourceFile;
    bool replaced = false;
    for (size_t k = 0; k < list.size(); ++k) {
      const Json::ArrayIndex ai = static_cast<Json::ArrayIndex>(k);
      if (ids[k] != theme["id"].asString()) continue;
      // 同 id 去重：仅同一皮肤文件可刷新，不覆盖其它来源。
      if (jsonStringOf(list[ai], "source_file") == sourceFile &&
          jsonStringOf(list[ai], "source") == "imported") {
        list[ai] = theme;
      }
      replaced = true;
      break;
    }
    if (!replaced) {
      list.append(theme);
      ids.push_back(theme["id"].asString());
    }
  }
  // 与 Go 一致：按 label 再 id 排序。
  std::vector<Json::Value> items;
  items.reserve(list.size());
  for (const auto& item : list) items.push_back(item);
  std::sort(items.begin(), items.end(), [](const Json::Value& a,
                                           const Json::Value& b) {
    const std::string la = jsonStringOf(a, "label");
    const std::string lb = jsonStringOf(b, "label");
    if (la != lb) return la < lb;
    return jsonStringOf(a, "id") < jsonStringOf(b, "id");
  });
  Json::Value out(Json::objectValue);
  out["version"] = 1;
  out["themes"] = Json::arrayValue;
  for (const auto& item : items) out["themes"].append(item);
  return writeJson(path, out);
}

bool importSkinJson(const Json::Value& parsed, std::string* message,
                    const std::wstring& root) {
  if (!parsed.isObject()) {
    if (message) *message = "无法解析外观 JSON";
    return false;
  }
  static const char* kKeys[] = {
      "candidate_theme",          "font_face",
      "font_point",               "candidate_comment_font_face",
      "candidate_comment_font_point", "inline_preedit",
      "candidate_per_row",        "candidate_count",
      "candidate_spacing",        "candidate_background_color",
      "candidate_highlight_color", "candidate_text_color",
      "candidate_highlight_text_color", "candidate_comment_color",
      "candidate_comment_highlight_color"};
  Json::Value patch(Json::objectValue);
  for (const char* key : kKeys) {
    if (parsed.isMember(key) && !parsed[key].isNull()) patch[key] = parsed[key];
  }
  if (patch.empty()) {
    if (message) *message = "外观 JSON 中没有可导入的字段";
    return false;
  }
  if (!mergeAppearanceConfig(patch, root)) {
    if (message) *message = "写入外观配置失败";
    return false;
  }
  if (message) *message = "外观已导入";
  return true;
}

bool importSkinYaml(const std::string& data, const std::wstring& filePath,
                    std::string* message, const std::wstring& rootOverride) {
  const Json::Value root = parseYamlMaps(data);
  const Json::Value styleRaw = root["style"];
  const Json::Value patch = root["patch"];
  const Json::Value style = mergeStylePatch(styleRaw, patch);
  const Json::Value schemes = mergeSchemePatches(root["preset_color_schemes"],
                                                 patch);
  if (!schemes.isObject() || schemes.empty()) {
    if (message) *message = "皮肤 YAML 中没有 preset_color_schemes";
    return false;
  }
  // 文件名（UTF-8）用于同源刷新判定。
  const std::string sourceFile = wsToUtf8(filePath.substr(
      filePath.find_last_of(L"\\/") == std::wstring::npos
          ? 0
          : filePath.find_last_of(L"\\/") + 1));

  Json::Value themes(Json::arrayValue);
  Json::Value byId(Json::objectValue);
  Json::Value labelsById(Json::objectValue);
  for (auto it = schemes.begin(); it != schemes.end(); ++it) {
    const std::string name = it.key().asString();
    if (!it->isObject()) continue;
    Json::Value ap = weaselStyleToAppearance(style, *it);
    if (ap.empty()) continue;
    const std::string id = normalizeThemeId(name);
    if (id.empty()) continue;
    ap["candidate_theme"] = id;
    const std::string label =
        it->isMember("name") && it->operator[]("name").isString() &&
                !trim(it->operator[]("name").asString()).empty()
            ? trim(it->operator[]("name").asString())
            : name;
    Json::Value theme(Json::objectValue);
    theme["id"] = id;
    theme["label"] = label;
    theme["appearance"] = ap;
    themes.append(theme);
    byId[id] = ap;
    labelsById[id] = label;
  }
  if (themes.empty()) {
    if (message) *message = "皮肤文件中没有可导入的配色方案";
    return false;
  }
  if (!upsertImportedThemes(themes, sourceFile, rootOverride)) {
    if (message) *message = "写入主题列表失败";
    return false;
  }
  // 选中配色：style.color_scheme → color_scheme_dark → 唯一方案。
  std::string activeId = normalizeThemeId(firstText(style, {"color_scheme"}));
  if (activeId.empty()) {
    activeId = normalizeThemeId(firstText(style, {"color_scheme_dark"}));
  }
  if (activeId.empty()) {
    activeId = themes[0]["id"].asString();
  }
  if (!byId.isMember(activeId)) {
    // 选中方案不在本次导入集里（引用内置方案）→ 用第一个导入的方案。
    activeId = themes[0]["id"].asString();
  }
  Json::Value patchCfg = byId[activeId];
  if (!patchCfg.isObject()) patchCfg = Json::Value(Json::objectValue);
  patchCfg["candidate_theme"] = activeId;
  if (!mergeAppearanceConfig(patchCfg, rootOverride)) {
    if (message) *message = "写入外观配置失败";
    return false;
  }
  if (message) {
    *message = "皮肤已导入: " +
               labelsById.get(activeId, activeId).asString();
  }
  return true;
}

} // namespace

// ---------------------------------------------------------------- public --

bool runFullRedeploy(std::string* message) {
  return runFullRedeployImpl(message);
}

bool gitPullCurrentSchemeSet(std::string* message) {
  auto fail = [&](const std::string& m) {
    if (message) *message = m;
    return false;
  };
  const std::wstring git = findGitExe();
  if (git.empty()) return fail("未检测到 Git 命令");
  const std::wstring dir = currentSchemeDir();
  if (dir.empty()) return fail("未找到方案集目录");
  if (!gitIsRepo(git, dir)) return fail("当前方案集目录不是 Git 仓库");

  const ProcessRun run =
      runProcessCapture(quoteArg(git) + L" -C " + quoteArg(dir) + L" pull");
  if (!run.started) return fail("无法启动 Git 进程");
  const std::vector<std::string> lines = nonEmptyLines(run.output);
  if (run.exitCode != 0) {
    const std::string detail =
        lines.empty() ? std::string() : truncateUtf8(lines.back(), 96);
    return fail(detail.empty() ? "更新配置失败" : "更新配置失败: " + detail);
  }
  if (!lines.empty()) {
    const std::string last = toLower(lines.back());
    if (last.find("already up to date") != std::string::npos ||
        last.find("already up-to-date") != std::string::npos) {
      if (message) *message = "配置已是最新";
      return true;
    }
  }
  if (message) *message = "更新配置成功";
  return true;
}

bool importAppearanceSkinFromDialog(std::string* message) {
  if (message) message->clear();
  wchar_t file[MAX_PATH] = {};
  OPENFILENAMEW ofn = {};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = ::GetForegroundWindow();
  ofn.lpstrFilter =
      L"皮肤文件 (*.yaml;*.yml;*.json)\0*.yaml;*.yml;*.json\0所有文件 "
      L"(*.*)\0*.*\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
  ofn.lpstrTitle = L"导入皮肤";
  if (!::GetOpenFileNameW(&ofn)) {
    return false; // 取消：message 保持空
  }
  return importAppearanceSkinFile(file, message);
}

bool importAppearanceSkinFile(const std::wstring& path, std::string* message,
                              const std::wstring& rootOverride) {
  if (message) message->clear();
  std::string data = readFileUtf8(path);
  if (data.empty()) {
    if (message) *message = "无法读取皮肤文件";
    return false;
  }
  const size_t dot = path.find_last_of(L'.');
  const std::string ext =
      dot == std::wstring::npos ? "" : wsToUtf8(path.substr(dot));
  if (toLower(ext) == ".yaml" || toLower(ext) == ".yml") {
    return importSkinYaml(data, path, message, rootOverride);
  }
  if (toLower(ext) == ".json") {
    Json::Value parsed;
    std::string errs;
    Json::CharReaderBuilder builder;
    std::istringstream in(data);
    if (!Json::parseFromStream(builder, in, &parsed, &errs)) {
      if (message) *message = "无法解析外观 JSON";
      return false;
    }
    return importSkinJson(parsed, message, rootOverride);
  }
  // 未知扩展名：先按 JSON 试，失败再按 YAML。
  {
    Json::Value parsed;
    std::string errs;
    Json::CharReaderBuilder builder;
    std::istringstream in(data);
    if (Json::parseFromStream(builder, in, &parsed, &errs) &&
        importSkinJson(parsed, message, rootOverride)) {
      return true;
    }
  }
  return importSkinYaml(data, path, message, rootOverride);
}

void notifyUserMessage(const std::string& utf8Message, bool error) {
  if (utf8Message.empty()) return;
  std::thread([utf8Message, error]() {
    const std::wstring text = utf8ToWs(utf8Message);
    ::MessageBoxW(nullptr, text.c_str(), L"墨奇输入法",
                  MB_OK | (error ? MB_ICONWARNING : MB_ICONINFORMATION) |
                      MB_SETFOREGROUND | MB_TOPMOST);
  }).detach();
}

} // namespace Moqi
