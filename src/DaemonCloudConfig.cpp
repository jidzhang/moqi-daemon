// 5e 云剪贴板配置管理 + DPAPI 凭据 + 同步引擎实现。
// 契约 = Go cloud_clipboard_config.go + cloudclipboard/{sync,config}.go +
// cloud_clipboard_password_windows.go。文案逐字对齐 Go。
// include 纪律：RimeCore.h 链开篇（见 ChildProxy.h 注释）。
#include "RimeCore.h"

#include <windows.h>
#include <dpapi.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

#include <json/json.h>

#include "DaemonCloudConfig.h"
#include "DaemonWebDAV.h"

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "advapi32.lib")

namespace Moqi {
namespace {

// 配置文件路径：rootOverride 非空（单测重定向）优先，否则 %APPDATA%\Moqi。
// 根目录取不到（空）时返回空串，调用方按“文件不存在”处理。
std::wstring filePathInRoot(const std::wstring& rootOverride,
                            const wchar_t* suffix) {
  const std::wstring root =
      rootOverride.empty() ? moqiAppDataDir() : rootOverride;
  return root.empty() ? L"" : root + suffix;
}

std::string trimStr(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && isspace((unsigned char)s[b])) ++b;
  while (e > b && isspace((unsigned char)s[e - 1])) --e;
  return s.substr(b, e - b);
}

// DPAPI（同 Go cloud_clipboard_password_windows.go，无 entropy）。
std::string protectData(const std::string& plain) {
  if (plain.empty()) return "";
  DATA_BLOB in{}, out{};
  in.cbData = (DWORD)plain.size();
  in.pbData = (BYTE*)plain.data();
  if (!::CryptProtectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) {
    return "";
  }
  std::string cipher((char*)out.pbData, out.cbData);
  ::LocalFree(out.pbData);
  return cipher;
}

std::string unprotectData(const std::string& cipher) {
  if (cipher.empty()) return "";
  DATA_BLOB in{}, out{};
  in.cbData = (DWORD)cipher.size();
  in.pbData = (BYTE*)cipher.data();
  if (!::CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0,
                            &out)) {
    return "";
  }
  std::string plain((char*)out.pbData, out.cbData);
  ::LocalFree(out.pbData);
  return plain;
}

std::string readFileBin(const std::wstring& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return "";
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool writeFileBin(const std::wstring& path, const std::string& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(data.data(), data.size());
  return out.good();
}

constexpr int kDebounceMs = 500;
constexpr int kMaxDisplayItems = 40;
constexpr size_t kPreviewMaxLength = 240;
constexpr ULONGLONG kRemoteClipClearMs = 300;
constexpr ULONGLONG kMinIntervalFloorMs = 10000;  // Go 默认 10s

// 读 network_egress.json：文件缺失/坏 JSON/非对象/键非布尔一律放行——
// 总闸只有显式 false 才拦（Go network_egress.go 同语义）。
// 注意非对象 JSON（如 "[]"）：jsoncpp 的 get/isMember/operator[] 在数组、
// 标量上会抛 Json::LogicError，必须先判 isObject（2026-11-06 实证，
// 配置文件手改成数组会让 daemon 启动即崩）。
bool readOutboundAllowedFile(const std::wstring& rootOverride) {
  std::ifstream in(filePathInRoot(rootOverride, L"\\network_egress.json"),
                   std::ios::binary);
  if (!in) return true;
  Json::Value rootJson;
  Json::CharReaderBuilder builder;
  std::string errs;
  if (!Json::parseFromStream(builder, in, &rootJson, &errs)) return true;
  if (!rootJson.isObject()) return true;
  if (rootJson.isMember("allow_outbound") &&
      rootJson["allow_outbound"].isBool()) {
    return rootJson["allow_outbound"].asBool();
  }
  return true;
}

}  // namespace

// ------------------------------------------------------------- 配置 ----

const char kOutboundBlockedMessage[] =
    "已按出站总闸设置拒绝网络请求（network_egress.json allow_outbound=false）";

// 用户可见错误文案（与 Go 契约逐字一致，单测字节锁）。
const char kErrCloudIncomplete[] =
    "\xe4\xba\x91\xe5\x89\xaa\xe8\xb4\xb4\xe6\x9d\xbf\xe6\x9c\xaa\xe9\x85\x8d"
    "\xe7\xbd\xae\xe5\xae\x8c\xe6\x95\xb4";  // 云剪贴板未配置完整
const char kErrCloudBadClipName[] =
    "\xe9\x9d\x9e\xe6\xb3\x95\xe4\xba\x91\xe5\x89\xaa\xe8\xb4\xb4\xe6\x9d\xbf"
    "\xe6\x96\x87\xe4\xbb\xb6\xe5\x90\x8d";  // 非法云剪贴板文件名
const char kErrCloudBadDeviceId[] =
    "\xe6\x9c\xac\xe6\x9c\xba\xe5\x90\x8c\xe6\xad\xa5 ID "
    "\xe6\x97\xa0\xe6\x95\x88";  // 本机同步 ID 无效
const char kErrCloudNeedUserPwd[] =
    "\xe8\xaf\xb7\xe5\xa1\xab\xe5\x86\x99\xe7\x94\xa8\xe6\x88\xb7\xe5\x90\x8d"
    "\xe5\x92\x8c\xe5\xaf\x86\xe7\xa0\x81";  // 请填写用户名和密码

bool outboundAllowed(const std::wstring& rootOverride) {
  // 单测重定向路径不进 2 秒缓存：测试要连续摆弄不同文件内容。
  if (!rootOverride.empty()) return readOutboundAllowedFile(rootOverride);
  static ULONGLONG loadedAt = 0;
  static bool allowed = true;
  const ULONGLONG now = ::GetTickCount64();
  if (loadedAt != 0 && now - loadedAt < 2000) return allowed;
  loadedAt = now;
  allowed = readOutboundAllowedFile(L"");
  return allowed;
}

std::string readCloudClipboardPassword(const std::wstring& rootOverride) {
  return unprotectData(readFileBin(
      filePathInRoot(rootOverride, L"\\cloud_clipboard.pw")));
}

bool saveCloudClipboardPassword(const std::string& password,
                                const std::wstring& rootOverride) {
  const std::wstring path =
      filePathInRoot(rootOverride, L"\\cloud_clipboard.pw");
  if (path.empty()) return false;
  if (password.empty()) {
    ::DeleteFileW(path.c_str());
    return true;
  }
  return writeFileBin(path, protectData(password));
}

CloudClipboardConfig loadCloudClipboardConfig(
    const std::wstring& rootOverride) {
  CloudClipboardConfig cfg;
  const std::wstring path =
      filePathInRoot(rootOverride, L"\\cloud_clipboard.json");
  if (path.empty()) return cfg;
  std::ifstream in(path, std::ios::binary);
  if (!in) return cfg;
  // 手工解析（daemon 已链 jsoncpp，但这里保持与 RimeCore 相同的轻量模式）
  Json::CharReaderBuilder builder;
  builder["collectComments"] = false;
  Json::Value root;
  std::string errs;
  if (!Json::parseFromStream(builder, in, &root, &errs)) {
    return cfg;
  }
  // 非对象 JSON（[]/5/"x"）会让后续 get/operator[] 抛 Json::LogicError，
  // 静态初始化路径上一抛即崩——按“没配置”处理。
  if (!root.isObject()) {
    return cfg;
  }
  cfg.enabled = root.get("enabled", false).asBool();
  cfg.baseUrl = trimStr(root.get("base_url", "").asString());
  cfg.username = trimStr(root.get("username", "").asString());
  cfg.settingsRoot = normalizeWebDavDir(
      root.get("settings_root", "").asString());
  int sec = root.get("min_interval_sec", 0).asInt();
  if (sec > 0) cfg.minIntervalSec = sec;
  std::string hk = trimStr(root.get("list_hotkey", "").asString());
  if (!hk.empty()) cfg.listHotkey = hk;
  cfg.password = readCloudClipboardPassword(rootOverride);
  // 过渡入口：JSON 里显式写 password 时转入 DPAPI 文件（Go 时代是对话框
  // 输入；退休 Go 后若不支持，新用户永远配不出密码）。
  const Json::Value& plainPwd = root["password"];
  if (cfg.password.empty() && plainPwd.isString() &&
      !trimStr(plainPwd.asString()).empty()) {
    cfg.password = trimStr(plainPwd.asString());
    saveCloudClipboardPassword(cfg.password, rootOverride);
    // 迁移完成立即重写 json 抹掉明文：save 不写 password 键、keepPassword
    // 保留刚生成的 DPAPI 文件。否则明文会一直躺在盘上，直到某次无关的
    // 保存才消失，违背“密码不落 json”。
    saveCloudClipboardConfig(cfg, "", true, rootOverride);
  }
  return cfg;
}

std::string loadCloudClipboardHotkey(const std::wstring& rootOverride) {
  const std::wstring path =
      filePathInRoot(rootOverride, L"\\cloud_clipboard.json");
  if (path.empty()) return "Ctrl+0";
  std::ifstream in(path, std::ios::binary);
  if (!in) return "Ctrl+0";
  Json::Value root;
  Json::CharReaderBuilder builder;
  std::string errs;
  if (!Json::parseFromStream(builder, in, &root, &errs)) return "Ctrl+0";
  // 同 loadCloudClipboardConfig：非对象 JSON 按“没配置”处理，不进
  // operator[]（会抛）。
  if (!root.isObject()) return "Ctrl+0";
  const Json::Value& hk = root["list_hotkey"];
  const std::string value = hk.isString() ? trimStr(hk.asString()) : "";
  return value.empty() ? "Ctrl+0" : value;
}

bool saveCloudClipboardConfig(const CloudClipboardConfig& cfg,
                              const std::string& newPassword,
                              bool keepPassword,
                              const std::wstring& rootOverride) {
  const std::wstring path =
      filePathInRoot(rootOverride, L"\\cloud_clipboard.json");
  if (path.empty()) return false;
  Json::Value root(Json::objectValue);
  root["enabled"] = cfg.enabled;
  root["base_url"] = trimStr(cfg.baseUrl);
  root["username"] = trimStr(cfg.username);
  root["settings_root"] = normalizeWebDavDir(cfg.settingsRoot);
  root["min_interval_sec"] = cfg.minIntervalSec > 0 ? cfg.minIntervalSec : 10;
  root["list_hotkey"] = cfg.listHotkey.empty() ? "Ctrl+0" : cfg.listHotkey;
  Json::StreamWriterBuilder writer;
  writer["commentStyle"] = "None";
  writer["indentation"] = "  ";
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out << Json::writeString(writer, root);
  if (!out.good()) return false;
  // 密码
  if (!newPassword.empty()) {
    return saveCloudClipboardPassword(newPassword, rootOverride);
  }
  if (!keepPassword) {
    return saveCloudClipboardPassword("", rootOverride);
  }
  return true;
}

// ------------------------------------------------------------- 同步 ----

CloudClipboardSync::CloudClipboardSync(const CloudClipboardConfig& cfg,
                                       const std::wstring& egressRootOverride)
    : cfg_(cfg), egressRoot_(egressRootOverride) {}

void CloudClipboardSync::reloadConfig(const CloudClipboardConfig& cfg) {
  std::lock_guard<std::mutex> lock(mutex_);
  cfg_ = cfg;
  client_.reset();
}

WebDavClient* CloudClipboardSync::client() {
  if (!cfg_.enabled || !cfg_.isComplete() ||
      !isAllowedBaseUrl(cfg_.baseUrl)) {
    return nullptr;
  }
  if (!client_) {
    WebDavConfig wc;
    wc.enabled = true;
    wc.baseUrl = cfg_.baseUrl;
    wc.username = cfg_.username;
    wc.password = cfg_.password;
    wc.settingsRoot = cfg_.settingsRoot;
    client_ = std::make_unique<WebDavClient>(wc);
  }
  return client_.get();
}

void CloudClipboardSync::setApplyingRemote(bool active) {
  std::lock_guard<std::mutex> lock(mutex_);
  applyingRemote_ = active;
  applyingUntil_ = active ? ::GetTickCount64() + kRemoteClipClearMs : 0;
}

bool CloudClipboardSync::isApplyingRemote() const {
  if (applyingRemote_ && ::GetTickCount64() < applyingUntil_) return true;
  return false;
}

bool CloudClipboardSync::shouldUpload(const std::string& text) const {
  if (!cfg_.enabled || !cfg_.isComplete()) return false;
  if (isApplyingRemote()) return false;
  if (!isUploadableText(text)) return false;
  if (contentMD5Hex(text) == lastUploadedHash_) return false;
  return true;
}

void CloudClipboardSync::uploadFromRequest(const std::string& raw) {
  if (!outboundAllowed(egressRoot_)) return;
  const std::string text = normalizePlainText(raw);
  if (text.empty()) return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!client() || !shouldUpload(text)) return;
    pendingText_ = text;
  }
  // 500ms 防抖：Go 用 time.AfterFunc，这里后台线程 sleep（简版，够用）。
  std::thread([this]() {
    ::Sleep(kDebounceMs);
    std::string toUpload;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      toUpload = pendingText_;
      pendingText_.clear();
    }
    if (!toUpload.empty()) performUpload(toUpload);
  }).detach();
}

void CloudClipboardSync::performUpload(const std::string& text) {
  CloudClipboardConfig cfg;
  ULONGLONG minInterval = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!cfg_.enabled || !cfg_.isComplete() ||
        !isAllowedBaseUrl(cfg_.baseUrl)) {
      return;
    }
    if (!shouldUpload(text)) return;
    if (isUploadInFlight_) {
      pendingText_ = text;  // 在途：合并到下一次
      return;
    }
    const ULONGLONG now = ::GetTickCount64();
    minInterval =
        (ULONGLONG)(cfg_.minIntervalSec > 0 ? cfg_.minIntervalSec : 10) *
        1000;
    if (lastUploadAt_ != 0 && now - lastUploadAt_ < minInterval) {
      // Go 用 time.AfterFunc 延迟重排；旧实现只挂 pending 就返回，之后
      // 没有定时器，这次复制永远传不上去。
      pendingText_ = text;
      const ULONGLONG delay = minInterval - (now - lastUploadAt_) + 50;
      std::thread([this, delay]() {
        ::Sleep((DWORD)delay);
        std::string pending;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          pending.swap(pendingText_);
        }
        if (!pending.empty()) performUpload(pending);
      }).detach();
      return;
    }
    cfg = cfg_;
    isUploadInFlight_ = true;
    pendingText_.clear();
  }

  // 网络上传不持锁：最长 30s，持锁会冻住列表/配置重载。
  WebDavConfig wc;
  wc.enabled = true;
  wc.baseUrl = cfg.baseUrl;
  wc.username = cfg.username;
  wc.password = cfg.password;
  wc.settingsRoot = cfg.settingsRoot;
  WebDavClient c(wc);
  std::string err;
  const bool ok = c.uploadClip(buildClipFilename(text), text, &err);

  std::string retry;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    isUploadInFlight_ = false;
    lastUploadAt_ = ::GetTickCount64();
    if (ok) {
      lastUploadedHash_ = contentMD5Hex(text);
    }
    retry.swap(pendingText_);
  }
  if (!retry.empty() && retry != text) {
    performUpload(retry);
  }
  // 同一内容上传失败不自动重试（Go 只重发不同的 pendingText）：断网时
  // 每 10s 重发旧剪贴板既无用，也会在网络恢复后把过期内容补传上去。
}

namespace {
bool makeClientFromConfig(const CloudClipboardConfig& cfg,
                          std::unique_ptr<WebDavClient>* out) {
  if (!cfg.enabled || !cfg.isComplete() || !isAllowedBaseUrl(cfg.baseUrl)) {
    return false;
  }
  WebDavConfig wc;
  wc.enabled = true;
  wc.baseUrl = cfg.baseUrl;
  wc.username = cfg.username;
  wc.password = cfg.password;
  wc.settingsRoot = cfg.settingsRoot;
  *out = std::make_unique<WebDavClient>(wc);
  return true;
}
}  // namespace

bool CloudClipboardSync::listClipsForDisplay(
    std::vector<CloudClipboardSync::DisplayItem>* out, std::string* err) {
  if (!outboundAllowed(egressRoot_)) {
    if (err) *err = kOutboundBlockedMessage;
    return false;
  }
  CloudClipboardConfig cfg;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    cfg = cfg_;
  }
  std::unique_ptr<WebDavClient> c;
  if (!makeClientFromConfig(cfg, &c)) {
    if (err) *err = kErrCloudIncomplete;
    return false;
  }
  std::vector<ClipEntry> entries;
  if (!c->listClips(&entries, err)) return false;
  if (entries.size() > (size_t)kMaxDisplayItems) {
    entries.resize(kMaxDisplayItems);
  }
  // Go ListClipsForDisplay 有 8 秒总超时；旧实现 40 条 × 30s 接收超时
  // 会让 NAS 假死时 Ctrl+0 十几分钟没反馈。
  const ULONGLONG deadline = ::GetTickCount64() + 8000;
  for (const auto& e : entries) {
    if (::GetTickCount64() >= deadline) break;
    std::string body;
    if (!c->downloadClip(e.name, &body, nullptr)) continue;
    body = normalizePlainText(body);
    DisplayItem item;
    item.name = e.name;
    item.lastModifiedMs = e.lastModifiedMs;
    item.preview = formatPreview(body, kPreviewMaxLength);
    item.fullText = body;
    out->push_back(std::move(item));
  }
  return true;
}

bool CloudClipboardSync::downloadClip(const std::string& name,
                                      std::string* body, std::string* err) {
  // 出站总闸（与其余出站口一致；此方法当前无生产调用方，护栏面向未来）。
  if (!outboundAllowed(egressRoot_)) {
    if (err) *err = kOutboundBlockedMessage;
    return false;
  }
  CloudClipboardConfig cfg;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    cfg = cfg_;
  }
  std::unique_ptr<WebDavClient> c;
  if (!makeClientFromConfig(cfg, &c)) {
    if (err) *err = kErrCloudIncomplete;
    return false;
  }
  return c->downloadClip(name, body, err);
}

bool CloudClipboardSync::deleteClip(const std::string& name,
                                    std::string* err) {
  // 出站总闸先于本地校验：列表开着时用户关总闸，删除也应立即拒绝
  // （2 秒缓存窗口内与其余出站口行为一致）。
  if (!outboundAllowed(egressRoot_)) {
    if (err) *err = kOutboundBlockedMessage;
    return false;
  }
  if (!isClipFileName(name)) {
    if (err) *err = kErrCloudBadClipName;
    return false;
  }
  CloudClipboardConfig cfg;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    cfg = cfg_;
  }
  std::unique_ptr<WebDavClient> c;
  if (!makeClientFromConfig(cfg, &c)) {
    if (err) *err = kErrCloudIncomplete;
    return false;
  }
  return c->deleteClip(name, err);
}

namespace {
bool writeSnapshotAtomic(const std::filesystem::path& path,
                         const std::string& data, std::string* err) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    if (err) *err = "创建本地同步目录失败";
    return false;
  }
  const std::filesystem::path tmp = path.wstring() + L".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (err) *err = "写入本地快照失败";
      return false;
    }
    out.write(data.data(), data.size());
    if (!out.good()) {
      if (err) *err = "写入本地快照失败";
      return false;
    }
  }
  if (!::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    ::DeleteFileW(tmp.c_str());
    if (err) *err = "替换本地快照失败";
    return false;
  }
  return true;
}
}  // namespace

bool CloudClipboardSync::downloadUserDictSnapshots(
    const std::wstring& localSyncRoot, const std::string& schemeSet,
    int* downloaded, std::string* err) {
  if (downloaded) *downloaded = 0;
  if (!outboundAllowed(egressRoot_)) {
    if (err) *err = kOutboundBlockedMessage;
    return false;
  }
  CloudClipboardConfig cfg;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    cfg = cfg_;
  }
  std::unique_ptr<WebDavClient> c;
  if (!makeClientFromConfig(cfg, &c)) {
    if (err) *err = "WebDAV 未配置完整";
    return false;
  }
  std::vector<UserDictSnapshotEntry> entries;
  if (!c->listUserDictSnapshots(schemeSet, &entries, err)) return false;
  int n = 0;
  for (const auto& e : entries) {
    std::string data;
    if (!c->downloadUserDictSnapshot(e, &data, err)) {
      if (downloaded) *downloaded = n;
      return false;
    }
    const std::filesystem::path target =
        std::filesystem::path(localSyncRoot) / std::filesystem::path(e.deviceId) /
        std::filesystem::path(e.name);
    if (!writeSnapshotAtomic(target, data, err)) {
      if (downloaded) *downloaded = n;
      return false;
    }
    ++n;
  }
  if (downloaded) *downloaded = n;
  return true;
}

bool CloudClipboardSync::uploadUserDictSnapshots(
    const std::wstring& localSyncRoot, const std::string& schemeSet,
    const std::string& deviceId, int* uploaded, std::string* err) {
  if (uploaded) *uploaded = 0;
  if (!outboundAllowed(egressRoot_)) {
    if (err) *err = kOutboundBlockedMessage;
    return false;
  }
  if (!isSyncDeviceDirName(deviceId)) {
    if (err) *err = kErrCloudBadDeviceId;
    return false;
  }
  CloudClipboardConfig cfg;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    cfg = cfg_;
  }
  std::unique_ptr<WebDavClient> c;
  if (!makeClientFromConfig(cfg, &c)) {
    if (err) *err = "WebDAV 未配置完整";
    return false;
  }
  const std::filesystem::path dir =
      std::filesystem::path(localSyncRoot) / std::filesystem::path(deviceId);
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec)) return true;
  int n = 0;
  for (const auto& de : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) break;
    if (!de.is_regular_file()) continue;
    const std::string name = de.path().filename().string();
    if (!isUserDictSnapshotFileName(name)) continue;
    std::ifstream in(de.path(), std::ios::binary);
    if (!in) {
      if (err) *err = "读取本地快照失败";
      if (uploaded) *uploaded = n;
      return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    UserDictSnapshotEntry e;
    e.schemeSet = schemeSet;
    e.deviceId = deviceId;
    e.name = name;
    if (!c->uploadUserDictSnapshot(e, ss.str(), err)) {
      if (uploaded) *uploaded = n;
      return false;
    }
    ++n;
  }
  if (uploaded) *uploaded = n;
  return true;
}

bool CloudClipboardSync::testConnection(std::string* err) {
  if (!outboundAllowed(egressRoot_)) {
    if (err) *err = kOutboundBlockedMessage;
    return false;
  }
  // Go testCloudClipboardConnection：强制 Enabled 建临时 client。
  CloudClipboardConfig test = cfg_;
  test.enabled = true;
  WebDavConfig wc;
  wc.enabled = true;
  wc.baseUrl = test.baseUrl;
  wc.username = test.username;
  wc.password = test.password;
  wc.settingsRoot = test.settingsRoot;
  if (!wc.isComplete() || !isAllowedBaseUrl(wc.baseUrl)) {
    if (err) {
      *err = isAllowedBaseUrl(wc.baseUrl)
                 ? kErrCloudNeedUserPwd
                 : webDavRejectionMessage();
    }
    return false;
  }
  WebDavClient c(wc);
  return c.testConnection(err);
}

// 全局同步引擎指针（main.cpp 设置；getter/setter 避开跨编译单元链接坑）。
static CloudClipboardSync* s_cloudSync = nullptr;
CloudClipboardSync* globalCloudSync() { return s_cloudSync; }
void setGlobalCloudSync(CloudClipboardSync* sync) { s_cloudSync = sync; }

}  // namespace Moqi
