// 维护命令回归：小狼毫皮肤 YAML / 墨奇外观 JSON 的导入解析与落盘。
// 落盘目录用 rootOverride 重定向到测试目录（不碰真实 %APPDATA%\Moqi）。
#include <Windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "DaemonMaintenance.h"
#include "json/json.h"

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const char* name) {
  if (ok) {
    ++g_pass;
    std::printf("PASS %s\n", name);
  } else {
    ++g_fail;
    std::printf("FAIL %s\n", name);
  }
}

std::string readAll(const std::wstring& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return "";
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void writeAll(const std::wstring& path, const std::string& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(data.data(), (std::streamsize)data.size());
}

Json::Value readJson(const std::wstring& path) {
  Json::Value v;
  std::string data = readAll(path);
  if (data.empty()) return v;
  Json::CharReaderBuilder builder;
  std::string errs;
  std::istringstream in(data);
  Json::parseFromStream(builder, in, &v, &errs);
  return v;
}

std::wstring exeDir() {
  wchar_t path[MAX_PATH] = {};
  ::GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring dir(path);
  const size_t slash = dir.find_last_of(L'\\');
  return slash == std::wstring::npos ? dir : dir.substr(0, slash);
}

} // namespace

int main() {
  const std::wstring root = exeDir() + L"\\maint-test-root";
  ::CreateDirectoryW(root.c_str(), nullptr);
  ::DeleteFileW((root + L"\\appearance_config.json").c_str());
  ::DeleteFileW((root + L"\\appearance_themes.json").c_str());

  // ---- 1. 小狼毫皮肤 YAML（含 patch 覆盖、嵌套 layout）----
  const std::string yaml =
      "style:\n"
      "  color_scheme: dark_temple\n"
      "  font_face: \"Microsoft YaHei\"\n"
      "  font_point: 18\n"
      "  comment_font_face: Consolas\n"
      "  comment_font_point: 12\n"
      "  inline_preedit: true\n"
      "  horizontal: true\n"
      "  candidate_spacing: 8\n"
      "  layout:\n"
      "    candidate_spacing: 8\n"
      "preset_color_schemes:\n"
      "  dark_temple:\n"
      "    name: \xe6\x9a\x97\xe5\xa0\x82\n"
      "    back_color: 0x2E2E2E\n"
      "    hilited_candidate_back_color: 0xC6DDF9\n"
      "    candidate_text_color: 0xFFFFFF\n"
      "    hilited_candidate_text_color: 0x000000\n"
      "    comment_text_color: 0x888888\n"
      "patch:\n"
      "  style/font_face: \"SimSun\"\n"
      "  preset_color_schemes/light_temple:\n"
      "    name: \xe6\x98\x8e\xe5\xa0\x82\n"
      "    back_color: 0xFFFFFF\n";
  const std::wstring yamlPath = root + L"\\skin-test.yaml";
  writeAll(yamlPath, yaml);

  std::string message;
  const bool yamlOk = Moqi::importAppearanceSkinFile(yamlPath, &message, root);
  check(yamlOk, "yaml: import ok");
  check(message.find("\xe6\x9a\x97\xe5\xa0\x82") != std::string::npos,
        "yaml: message names active theme");

  Json::Value cfg = readJson(root + L"\\appearance_config.json");
  check(cfg.get("font_face", "").asString() == "SimSun",
        "yaml: patch style/ overlay applied");
  check(cfg.get("font_point", 0).asInt() == 18, "yaml: font_point");
  check(cfg.get("inline_preedit", false).asBool(), "yaml: inline_preedit");
  check(cfg.get("candidate_per_row", 0).asInt() == 9,
        "yaml: horizontal -> per_row 9");
  check(cfg.get("candidate_spacing", -1).asInt() == 8,
        "yaml: candidate_spacing");
  check(cfg.get("candidate_background_color", "").asString() == "#2e2e2e",
        "yaml: abgr color converted");
  check(cfg.get("candidate_text_color", "").asString() == "#ffffff",
        "yaml: text color");
  // 0x00C6DDF9 ABGR -> r=0xF9 g=0xDD b=0xC6
  check(cfg.get("candidate_highlight_color", "").asString() == "#f9ddc6",
        "yaml: highlight color");
  check(cfg.get("candidate_theme", "").asString() == "dark_temple",
        "yaml: active theme id");

  Json::Value themes = readJson(root + L"\\appearance_themes.json");
  check(themes.get("version", 0).asInt() == 1, "themes: version");
  check(themes["themes"].isArray() && themes["themes"].size() == 2,
        "themes: two imported schemes");
  bool darkFound = false;
  bool lightFound = false;
  for (const auto& t : themes["themes"]) {
    const std::string id = t.get("id", "").asString();
    if (id == "dark_temple") {
      darkFound = t.get("label", "").asString() == "\xe6\x9a\x97\xe5\xa0\x82" &&
                  t.get("source", "").asString() == "imported" &&
                  t.get("source_file", "").asString() == "skin-test.yaml";
    }
    if (id == "light_temple") {
      lightFound = t.get("label", "").asString() == "\xe6\x98\x8e\xe5\xa0\x82";
    }
  }
  check(darkFound, "themes: dark_temple registered");
  check(lightFound, "themes: patch scheme registered");

  // ---- 2. 墨奇外观 JSON ----
  const std::wstring jsonPath = root + L"\\skin-test.json";
  writeAll(jsonPath,
           "{\"font_point\": 22, \"candidate_background_color\": \"#101010\"}\n");
  message.clear();
  const bool jsonOk = Moqi::importAppearanceSkinFile(jsonPath, &message, root);
  check(jsonOk, "json: import ok");
  cfg = readJson(root + L"\\appearance_config.json");
  check(cfg.get("font_point", 0).asInt() == 22, "json: font_point applied");
  check(cfg.get("candidate_background_color", "").asString() == "#101010",
        "json: background applied");
  check(cfg.get("font_face", "").asString() == "SimSun",
        "json: unrelated keys untouched");

  // ---- 3. 无配色方案的 YAML 必须失败 ----
  const std::wstring badPath = root + L"\\skin-bad.yaml";
  writeAll(badPath, "style:\n  font_face: Arial\n");
  message.clear();
  check(!Moqi::importAppearanceSkinFile(badPath, &message, root),
        "yaml: rejects file without schemes");

  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
