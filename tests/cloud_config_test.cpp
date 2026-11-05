// cloud_config_test —— 云剪贴板配置层回归：JSON 解析/默认值兜底/trim、
// DPAPI 密码文件回环、明文密码过渡迁移、出站总闸文件语义、
// 同步引擎零网络校验路径。rootOverride 重定向到测试目录，
// 绝不碰真实 %APPDATA%\Moqi。
// include 纪律：jsoncpp 先于裸 STL。
#include <json/json.h>

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "DaemonCloudConfig.h"
#include "DaemonWebDAV.h"

using Moqi::CloudClipboardConfig;
using Moqi::CloudClipboardSync;

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

std::wstring exeDir() {
  wchar_t path[MAX_PATH] = {};
  ::GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring dir(path);
  const size_t slash = dir.find_last_of(L'\\');
  return slash == std::wstring::npos ? dir : dir.substr(0, slash);
}

bool fileExists(const std::wstring& path) {
  return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

Json::Value parseJson(const std::string& data) {
  Json::Value v;
  Json::CharReaderBuilder builder;
  std::string errs;
  std::istringstream in(data);
  Json::parseFromStream(builder, in, &v, &errs);
  return v;
}

// 错误文案逐字节锁（与 DaemonCloudConfig.cpp 内嵌 UTF-8 一致，用户已见
// 的提示不换说法）。
const char kErrIncomplete[] =
    "\xe4\xba\x91\xe5\x89\xaa\xe8\xb4\xb4\xe6\x9d\xbf\xe6\x9c\xaa\xe9\x85\x8d"
    "\xe7\xbd\xae\xe5\xae\x8c\xe6\x95\xb4";  // 云剪贴板未配置完整
const char kErrBadClipName[] =
    "\xe9\x9d\x9e\xe6\xb3\x95\xe4\xba\x91\xe5\x89\xaa\xe8\xb4\xb4\xe6\x9d\xbf"
    "\xe6\x96\x87\xe4\xbb\xb6\xe5\x90\x8d";  // 非法云剪贴板文件名
const char kErrBadDeviceId[] =
    "\xe6\x9c\xac\xe6\x9c\xba\xe5\x90\x8c\xe6\xad\xa5 ID "
    "\xe6\x97\xa0\xe6\x95\x88";  // 本机同步 ID 无效
const char kErrNeedUserPwd[] =
    "\xe8\xaf\xb7\xe5\xa1\xab\xe5\x86\x99\xe7\x94\xa8\xe6\x88\xb7\xe5\x90\x8d"
    "\xe5\x92\x8c\xe5\xaf\x86\xe7\xa0\x81";  // 请填写用户名和密码

}  // namespace

int main() {
  const std::wstring root = exeDir() + L"\\cloud-test-root";
  ::CreateDirectoryW(root.c_str(), nullptr);
  const std::wstring cfgPath = root + L"\\cloud_clipboard.json";
  const std::wstring pwPath = root + L"\\cloud_clipboard.pw";
  const std::wstring egressPath = root + L"\\network_egress.json";
  ::DeleteFileW(cfgPath.c_str());
  ::DeleteFileW(pwPath.c_str());
  ::DeleteFileW(egressPath.c_str());

  // ---- 1. 无任何文件：全默认值 ----
  {
    const CloudClipboardConfig cfg = Moqi::loadCloudClipboardConfig(root);
    check(!cfg.enabled, "default: enabled=false");
    check(cfg.baseUrl.empty(), "default: base_url empty");
    check(cfg.username.empty(), "default: username empty");
    check(cfg.settingsRoot == "/moqi-input-method/", "default: settings_root");
    check(cfg.minIntervalSec == 10, "default: min_interval 10s");
    check(cfg.listHotkey == "Ctrl+0", "default: hotkey Ctrl+0");
    check(cfg.password.empty(), "default: password empty");
    check(!cfg.isComplete(), "default: incomplete");
    check(Moqi::loadCloudClipboardHotkey(root) == "Ctrl+0",
          "default: hotkey-only load Ctrl+0");
    check(Moqi::outboundAllowed(root), "egress: no file -> allowed");
  }

  // ---- 2. 完整解析 + 首尾空白 trim + settings_root 归一 ----
  writeAll(cfgPath,
           "{\n"
           "  \"enabled\": true,\n"
           "  \"base_url\": \"  https://dav.example.com/dav  \",\n"
           "  \"username\": \" alice \",\n"
           "  \"settings_root\": \"moqi-input-method\",\n"
           "  \"min_interval_sec\": 30,\n"
           "  \"list_hotkey\": \" Ctrl+9 \"\n"
           "}\n");
  {
    const CloudClipboardConfig cfg = Moqi::loadCloudClipboardConfig(root);
    check(cfg.enabled, "parse: enabled");
    check(cfg.baseUrl == "https://dav.example.com/dav",
          "parse: base_url trimmed");
    check(cfg.username == "alice", "parse: username trimmed");
    check(cfg.settingsRoot == "/moqi-input-method/",
          "parse: settings_root normalized");
    check(cfg.minIntervalSec == 30, "parse: min_interval 30");
    check(cfg.listHotkey == "Ctrl+9", "parse: hotkey trimmed");
    check(Moqi::loadCloudClipboardHotkey(root) == "Ctrl+9",
          "parse: hotkey-only load");
  }

  // ---- 3. 兜底：非法/缺省值回落默认 ----
  writeAll(cfgPath, "{\"min_interval_sec\": 0}\n");
  check(Moqi::loadCloudClipboardConfig(root).minIntervalSec == 10,
        "guard: min_interval 0 -> 10");
  writeAll(cfgPath, "{\"min_interval_sec\": -5}\n");
  check(Moqi::loadCloudClipboardConfig(root).minIntervalSec == 10,
        "guard: min_interval -5 -> 10");
  writeAll(cfgPath, "{\"list_hotkey\": \"\"}\n");
  check(Moqi::loadCloudClipboardConfig(root).listHotkey == "Ctrl+0",
        "guard: empty hotkey -> Ctrl+0");
  writeAll(cfgPath, "{\"list_hotkey\": \"   \"}\n");
  check(Moqi::loadCloudClipboardConfig(root).listHotkey == "Ctrl+0",
        "guard: blank hotkey -> Ctrl+0");
  writeAll(cfgPath, "{\"list_hotkey\": 123}\n");
  check(Moqi::loadCloudClipboardHotkey(root) == "Ctrl+0",
        "guard: non-string hotkey -> Ctrl+0");
  writeAll(cfgPath, "{oops\n");
  {
    const CloudClipboardConfig cfg = Moqi::loadCloudClipboardConfig(root);
    check(!cfg.enabled && cfg.baseUrl.empty(), "guard: broken json -> defaults");
    check(Moqi::loadCloudClipboardHotkey(root) == "Ctrl+0",
          "guard: broken json hotkey -> Ctrl+0");
  }

  // ---- 4. DPAPI 密码回环 ----
  check(Moqi::saveCloudClipboardPassword("s3cret-pw", root), "pw: save ok");
  check(Moqi::readCloudClipboardPassword(root) == "s3cret-pw",
        "pw: roundtrip");
  {
    const std::string raw = readAll(pwPath);
    check(!raw.empty() && raw.find("s3cret-pw") == std::string::npos,
          "pw: file holds DPAPI ciphertext, not plaintext");
  }
  check(Moqi::saveCloudClipboardPassword("", root), "pw: clear ok");
  check(Moqi::readCloudClipboardPassword(root).empty(), "pw: cleared read");
  check(!fileExists(pwPath), "pw: file removed on clear");
  // 密码就位后 isComplete 翻真
  writeAll(cfgPath,
           "{\"enabled\": true, \"base_url\": \"https://dav.example.com\", "
           "\"username\": \"bob\"}\n");
  check(Moqi::saveCloudClipboardPassword("pw1", root), "pw: set pw1");
  {
    const CloudClipboardConfig cfg = Moqi::loadCloudClipboardConfig(root);
    check(cfg.password == "pw1" && cfg.isComplete(),
          "pw: config complete with dpapi password");
  }

  // ---- 5. JSON 明文密码过渡迁移（Go 退休后的手工配置入口）----
  ::DeleteFileW(pwPath.c_str());
  writeAll(cfgPath,
           "{\"base_url\": \"https://dav.example.com\", \"username\": \"bob\", "
           "\"password\": \"  plainpw  \"}\n");
  {
    const CloudClipboardConfig cfg = Moqi::loadCloudClipboardConfig(root);
    check(cfg.password == "plainpw", "migrate: json password taken (trimmed)");
    check(Moqi::readCloudClipboardPassword(root) == "plainpw",
          "migrate: dpapi file written");
    check(Moqi::loadCloudClipboardConfig(root).password == "plainpw",
          "migrate: reload prefers dpapi file");
  }
  ::DeleteFileW(pwPath.c_str());
  writeAll(cfgPath, "{\"password\": \"   \"}\n");
  check(Moqi::loadCloudClipboardConfig(root).password.empty() &&
            !fileExists(pwPath),
        "migrate: whitespace-only password not migrated");

  // ---- 6. 保存回环 + 密码三态 ----
  {
    CloudClipboardConfig cfg;
    cfg.enabled = true;
    cfg.baseUrl = " https://dav.example.com/dav ";
    cfg.username = " bob ";
    cfg.minIntervalSec = 0;  // 非法 → 落盘兜底 10
    cfg.listHotkey = "";     // 空 → 落盘 Ctrl+0
    check(Moqi::saveCloudClipboardConfig(cfg, "newpw", false, root),
          "save: ok");
    const CloudClipboardConfig back = Moqi::loadCloudClipboardConfig(root);
    check(back.enabled && back.baseUrl == "https://dav.example.com/dav" &&
              back.username == "bob",
          "save: trimmed fields roundtrip");
    check(back.minIntervalSec == 10, "save: min_interval floored to 10");
    check(back.listHotkey == "Ctrl+0", "save: hotkey defaulted");
    check(back.password == "newpw" && back.isComplete(),
          "save: new password written");
    const Json::Value saved = parseJson(readAll(cfgPath));
    check(!saved.isMember("password"), "save: password never in json");
    check(saved.get("min_interval_sec", 0).asInt() == 10,
          "save: json min_interval 10");
    check(saved.get("base_url", "").asString() ==
              "https://dav.example.com/dav",
          "save: json base_url trimmed");
    check(saved.get("list_hotkey", "").asString() == "Ctrl+0",
          "save: json hotkey default");
    // keepPassword=true 且新密码空 → 保留旧密码
    check(Moqi::saveCloudClipboardConfig(cfg, "", true, root),
          "save: keepPassword ok");
    check(Moqi::readCloudClipboardPassword(root) == "newpw",
          "save: keepPassword preserves");
    // keepPassword=false 且新密码空 → 清除
    check(Moqi::saveCloudClipboardConfig(cfg, "", false, root),
          "save: clearPassword ok");
    check(Moqi::readCloudClipboardPassword(root).empty(),
          "save: clearPassword empties");
    check(!Moqi::loadCloudClipboardConfig(root).isComplete(),
          "save: incomplete after password cleared");
  }

  // ---- 7. 出站总闸文件语义（只有显式 false 才拦）----
  writeAll(egressPath, "{\"allow_outbound\": false}\n");
  check(!Moqi::outboundAllowed(root), "egress: explicit false blocks");
  writeAll(egressPath, "{\"allow_outbound\": true}\n");
  check(Moqi::outboundAllowed(root), "egress: explicit true allows");
  writeAll(egressPath, "{oops\n");
  check(Moqi::outboundAllowed(root), "egress: broken json allows");
  writeAll(egressPath, "{\"allow_outbound\": \"false\"}\n");
  check(Moqi::outboundAllowed(root), "egress: non-bool value ignored");
  ::DeleteFileW(egressPath.c_str());

  // ---- 8. 同步引擎零网络校验路径 ----
  // deleteClip 不走总闸（纯本地校验先行）；其余方法内部读真机总闸文件，
  // 被本机拦时跳过，不让单测依赖机器状态。
  const bool egressOpenOnThisMachine = Moqi::outboundAllowed();
  {
    CloudClipboardConfig empty;
    CloudClipboardSync sync(empty);
    std::string err;
    check(!sync.deleteClip("../evil.txt", &err) && err == kErrBadClipName,
          "sync: deleteClip rejects bad name before config check");
    err.clear();
    check(!sync.deleteClip(std::string(32, 'a') + ".txt", &err) &&
              err == kErrIncomplete,
          "sync: deleteClip incomplete config message");
    if (egressOpenOnThisMachine) {
      err.clear();
      int uploaded = -1;
      check(!sync.uploadUserDictSnapshots(root, "Rime", "../bad id",
                                          &uploaded, &err) &&
                err == kErrBadDeviceId && uploaded == 0,
            "sync: upload rejects bad device id");
      err.clear();
      CloudClipboardConfig needPwd;
      needPwd.baseUrl = "https://dav.example.com";
      needPwd.username = "alice";
      CloudClipboardSync t1(needPwd);
      check(!t1.testConnection(&err) && err == kErrNeedUserPwd,
            "sync: testConnection missing password message");
      err.clear();
      CloudClipboardConfig badScheme = needPwd;
      badScheme.password = "pw";
      badScheme.baseUrl = "ftp://dav.example.com/";
      CloudClipboardSync t2(badScheme);
      check(!t2.testConnection(&err) && err == Moqi::webDavRejectionMessage(),
            "sync: testConnection rejects non-http scheme");
    } else {
      std::printf("SKIP sync egress-gated checks (machine blocks outbound)\n");
    }
  }

  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
