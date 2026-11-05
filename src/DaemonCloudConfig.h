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
// rootOverride 仅供单测把读写重定向到临时目录（同 DaemonMaintenance 先例）；
// 空串 = 真实 %APPDATA%\Moqi，生产调用点全部走默认值。
CloudClipboardConfig loadCloudClipboardConfig(
    const std::wstring& rootOverride = L"");
// 只读热键字段（按键路径每 2 秒缓存一次，避免每次 DPAPI 解密密码）。
std::string loadCloudClipboardHotkey(const std::wstring& rootOverride = L"");
// 保存 json（密码单独走 saveCloudClipboardPassword）。
// newPassword 非空 → 写入密码文件；keepPassword=true 且新密码空 → 保留旧密码。
bool saveCloudClipboardConfig(const CloudClipboardConfig& cfg,
                               const std::string& newPassword,
                               bool keepPassword,
                               const std::wstring& rootOverride = L"");

// DPAPI 密码（CryptProtectData/CryptUnprotectData，同 Go）。
std::string readCloudClipboardPassword(const std::wstring& rootOverride = L"");
bool saveCloudClipboardPassword(const std::string& password,
                                const std::wstring& rootOverride = L"");

// 出站总闸（Go network_egress.go）：%APPDATA%\Moqi\network_egress.json
// {"allow_outbound": false} 时四个出站口（AI/云剪贴/词库同步/WebDAV）统一拒绝。
bool outboundAllowed(const std::wstring& rootOverride = L"");
extern const char kOutboundBlockedMessage[];
// 用户可见错误文案（与 Go 契约逐字一致；曾内联在 .cpp 多处，导出成常量
// 防漂移，单测对常量做字节锁）。
extern const char kErrCloudIncomplete[];   // 云剪贴板未配置完整
extern const char kErrCloudBadClipName[];  // 非法云剪贴板文件名
extern const char kErrCloudBadDeviceId[];  // 本机同步 ID 无效
extern const char kErrCloudNeedUserPwd[];  // 请填写用户名和密码

// = Go cloudclipboard::Sync（上传引擎）
class CloudClipboardSync {
 public:
  // egressRootOverride 仅供单测把出站总闸文件重定向到测试目录（否则
  // 同步方法的校验路径覆盖度依赖真机 network_egress.json 状态）。
  explicit CloudClipboardSync(const CloudClipboardConfig& cfg,
                              const std::wstring& egressRootOverride = L"");

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
  std::wstring egressRoot_;  // 出站总闸文件根（默认空=真 %APPDATA%\Moqi）
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
