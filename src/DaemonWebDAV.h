#pragma once
// 5e 云剪贴板/WebDAV：WinHTTP 客户端 + 守卫/命名规则。
// 契约 = Go cloudclipboard/{client,guard,paths,config}.go，规格见
// docs/5e-cloud-clipboard-port-spec.md。错误文案与 Go 逐字一致（用户已见的
// 提示不换说法）。
#include <windows.h>
#include <string>
#include <vector>

namespace Moqi {

// = cloudclipboard::Config（密码在内存中，落盘走 DPAPI，见 DaemonCloudConfig）
struct WebDavConfig {
  bool enabled = false;
  std::string baseUrl;
  std::string username;
  std::string password;
  std::string settingsRoot = "/moqi-input-method/";
  int minIntervalSec = 10;
  std::string listHotkey = "Ctrl+0";

  bool isComplete() const {
    return !baseUrl.empty() && !username.empty() && !password.empty();
  }
};

struct ClipEntry {
  std::string name;
  long long lastModifiedMs = 0;  // Unix 毫秒（PROPFIND getlastmodified）
};

// 用户词库快照（Go cloudclipboard.UserDictSnapshotEntry）。
struct UserDictSnapshotEntry {
  std::string schemeSet;
  std::string deviceId;
  std::string name;
};

// http/https 前缀门禁（Go IsAllowedBaseURL / RejectionMessage）。
bool isAllowedBaseUrl(const std::string& url);
const char* webDavRejectionMessage();

class WebDavClient {
 public:
  explicit WebDavClient(const WebDavConfig& cfg);

  // 全部返回 false 时 *err 带用户可见中文文案。
  bool testConnection(std::string* err);
  bool listClips(std::vector<ClipEntry>* out, std::string* err);
  bool uploadClip(const std::string& filename, const std::string& text,
                  std::string* err);
  bool downloadClip(const std::string& name, std::string* body,
                    std::string* err);
  bool deleteClip(const std::string& name, std::string* err);

  // 用户词库快照同步（Go client.ListUserDictSnapshots /
  // DownloadUserDictSnapshot / UploadUserDictSnapshot）。
  bool listUserDictSnapshots(const std::string& schemeSet,
                             std::vector<UserDictSnapshotEntry>* out,
                             std::string* err);
  bool downloadUserDictSnapshot(const UserDictSnapshotEntry& entry,
                                std::string* data, std::string* err);
  bool uploadUserDictSnapshot(const UserDictSnapshotEntry& entry,
                              const std::string& data, std::string* err);

 private:
  // 单次请求：verb 任意（PROPFIND/MKCOL…），返回 HTTP 状态码，-1=传输错误。
  int request(const wchar_t* verb, const std::string& url,
              const std::string* body, const char* contentType, int depth,
              std::string* respBody, std::string* err);
  bool propfindOk(const std::string& url, int depth, std::string* body,
                  int* status, std::string* err);
  bool directoryExists(const std::string& url);
  bool mkcol(const std::string& url);
  bool ensureRemoteDirectory(std::string* err);

  WebDavConfig cfg_;
};

// ---- 守卫/命名（guard.go）----
std::string contentMD5Hex(const std::string& text);
std::string buildClipFilename(const std::string& text);  // md5 + ".txt"
bool isClipFileName(const std::string& name);
bool isUserDictSnapshotFileName(const std::string& name);
bool isSyncDeviceDirName(const std::string& name);
bool isSchemeSetDirName(const std::string& name);
bool isUploadableText(const std::string& utf8);
std::string normalizePlainText(const std::string& utf8);
// 空白折叠单行 + 截断加 …（maxLength 按 UTF-8 字符数）。
std::string formatPreview(const std::string& utf8, size_t maxChars);

// SettingsRoot 归一（空→默认、补前导/尾随 /）。
std::string normalizeWebDavDir(const std::string& raw);

}  // namespace Moqi
