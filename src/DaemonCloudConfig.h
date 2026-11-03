#pragma once
// 5e 云剪贴板配置管理 + DPAPI 凭据 + 同步引擎。
// 契约 = Go cloud_clipboard_config.go + cloudclipboard/{sync,config}.go +
// cloud_clipboard_password_windows.go，规格见 docs/5e-cloud-clipboard-port-spec.md。
#include <windows.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "DaemonWebDAV.h"

namespace Moqi {

// = Go cloudclipboard::Config + 配置文件读写
struct CloudClipboardConfig {
  bool enabled = false;
  std::string baseUrl;
  std::string username;
  std::string settingsRoot = "/moqi-input-method/";
  int minIntervalSec = 10;
  std::string listHotkey = "Ctrl+0";
  // 密码不落 json，存 DPAPI 加密的 cloud_clipboard.pw（内存中持有）。
  std::string password;

  bool isComplete() const {
    return !baseUrl.empty() && !username.empty() && !password.empty();
  }
};

// 读 %APPDATA%\Moqi\cloud_clipboard.json + 解密 cloud_clipboard.pw。
CloudClipboardConfig loadCloudClipboardConfig();
// 只读热键字段（按键路径每 2 秒缓存一次，避免每次 DPAPI 解密密码）。
std::string loadCloudClipboardHotkey();
// 保存 json（密码单独走 saveCloudClipboardPassword）。
// newPassword 非空 → 写入密码文件；keepPassword=true 且新密码空 → 保留旧密码。
bool saveCloudClipboardConfig(const CloudClipboardConfig& cfg,
                               const std::string& newPassword,
                               bool keepPassword);

// DPAPI 密码（CryptProtectData/CryptUnprotectData，同 Go）。
std::string readCloudClipboardPassword();
bool saveCloudClipboardPassword(const std::string& password);

// 出站总闸（Go network_egress.go）：%APPDATA%\Moqi\network_egress.json
// {"allow_outbound": false} 时四个出站口（AI/云剪贴/词库同步/WebDAV）统一拒绝。
bool outboundAllowed();
extern const char kOutboundBlockedMessage[];

// = Go cloudclipboard::Sync（上传引擎）
class CloudClipboardSync {
 public:
  explicit CloudClipboardSync(const CloudClipboardConfig& cfg);

  void reloadConfig(const CloudClipboardConfig& cfg);

  // 上传入口（Go UploadFromRequest）：过滤+防抖+异步。
  void uploadFromRequest(const std::string& text);
  // 列表（Go ListClipsForDisplay）：取 top 40 条含全文。
  struct DisplayItem {
    std::string name;
    long long lastModifiedMs;
    std::string preview;
    std::string fullText;
  };
  bool listClipsForDisplay(std::vector<DisplayItem>* out, std::string* err);
  bool downloadClip(const std::string& name, std::string* body,
                    std::string* err);
  bool deleteClip(const std::string& name, std::string* err);
  bool testConnection(std::string* err);
  // 用户词库快照（Go syncUserDataWithWebDAV 的下载/上传两段）。
  bool downloadUserDictSnapshots(const std::wstring& localSyncRoot,
                                 const std::string& schemeSet,
                                 int* downloaded, std::string* err);
  bool uploadUserDictSnapshots(const std::wstring& localSyncRoot,
                               const std::string& schemeSet,
                               const std::string& deviceId, int* uploaded,
                               std::string* err);
  // 粘贴远端后 300ms 内抑制上传（防自回声）。
  void setApplyingRemote(bool active);

 private:
  void performUpload(const std::string& text);
  bool shouldUpload(const std::string& text) const;
  bool isApplyingRemote() const;
  WebDavClient* client();  // 懒建；未配置完整返回 null

  CloudClipboardConfig cfg_;
  std::mutex mutex_;
  std::unique_ptr<WebDavClient> client_;
  ULONGLONG lastUploadAt_ = 0;
  std::string lastUploadedHash_;
  bool isUploadInFlight_ = false;
  std::string pendingText_;
  bool applyingRemote_ = false;
  ULONGLONG applyingUntil_ = 0;
};

// main.cpp 持有全局同步引擎；监听线程经此 getter 取（避开跨编译单元
// 全局变量的链接坑）。
CloudClipboardSync* globalCloudSync();
void setGlobalCloudSync(CloudClipboardSync* sync);

}  // namespace Moqi
