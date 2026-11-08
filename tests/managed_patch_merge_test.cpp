// managed_patch_merge_test —— buildInProcCustomPatch 合并守卫回归
// （2026-11-08 档位升级事故的回归锚）。
// 事故：单引擎架构下 backendUserDir 与写入目标同目录，旧实现把本工具自己
// 的旧输出（带 managed marker 的 17 块堆积文件）合并回新块——每次 daemon
// 启动追加一块，且 YAML 重复键后者生效，lite→std 升级后旧档
// grammar/language: zh-moqi-lite 永远压住新档，std 每会话报 gram 缺失
// ERROR（15 套 glog 三件套）。修复：marker 文件不回并；无 marker 的用户
// custom yaml（方案集/雾凇）仍合并。
// include 纪律：jsoncpp 先于裸 STL（MSVC <cmath> 崩，见 ChildProxy.h）。
#include <json/json.h>

#include "RimeCore.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using Moqi::buildInProcCustomPatch;

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

static void writeFile(const std::filesystem::path& p, const std::string& text) {
  std::filesystem::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << text;
}

int main() {
  const auto tmp = std::filesystem::temp_directory_path() /
                   "moqi-merge-test";
  std::filesystem::remove_all(tmp);
  std::filesystem::create_directories(tmp);

  // 1) 旧托管输出（带 marker + 事故特征键）不得被合并回去。
  writeFile(tmp / "rime_frost.custom.yaml",
            "# managed by moqi local engine\n"
            "patch:\n"
            "  grammar/language: zh-moqi-lite\n"
            "  x_canary_stale: 1\n");
  const std::string managed = buildInProcCustomPatch(tmp.wstring(), L"");
  check(!contains(managed, "x_canary_stale"),
        "stale managed block NOT merged back");
  check(!contains(managed, "zh-moqi-lite"),
        "stale tier grammar NOT resurrected");
  check(contains(managed, "patch:"), "fresh patch body present");

  // 2) 无 marker 的用户 custom yaml（方案集目录语义）仍要合并。
  writeFile(tmp / "rime_frost.custom.yaml",
            "# my own scheme set\n"
            "patch:\n"
            "  my_custom_key: 1\n");
  const std::string userYaml = buildInProcCustomPatch(tmp.wstring(), L"");
  check(contains(userYaml, "my_custom_key: 1"),
        "user-owned custom yaml still merged");

  // 3) 幂等：同一输入二次调用结果一致（不再自我膨胀）。
  const std::string again = buildInProcCustomPatch(tmp.wstring(), L"");
  check(userYaml == again, "idempotent across calls");

  std::filesystem::remove_all(tmp);
  std::printf("\n%d passed, %d failed\n", g_pass, g_failed);
  return g_failed == 0 ? 0 : 1;
}
