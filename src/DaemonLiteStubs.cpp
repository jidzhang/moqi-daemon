// Lite 版空桩（2026-11-06 用户定稿）：MOQI_LITE 构建剔除 DaemonAI /
// DaemonCloudConfig / DaemonWebDAV / ClipboardMonitor 四个模块的实现后，
// 保留文件（main/DaemonEngine/DaemonMenuService/DaemonMaintenance）对这些
// 入口的引用在本文件以“无操作/恒失败”桩满足链接。语义=功能不存在：
// 不读配置、不发网络请求、不启动监听线程——用户本地留有 cloud_clipboard
// .json / ai_config.json 也不会有任何执行路径（AI 热键永不匹配、云列表
// 热键永不匹配、上传/下载/测试恒失败返回错误文案）。
// 全量构建不编译本文件（CMakeLists 仅 MOQI_LITE 时加入源表）。
#include "DaemonAI.h"
#include "DaemonCloudConfig.h"
#include "ClipboardMonitor.h"

namespace Moqi {

// ---- ClipboardMonitor ----
void startClipboardMonitor() {}
void stopClipboardMonitor() {}

// ---- DaemonWebDAV 守卫/命名（保留代码引用的子集） ----
bool isAllowedBaseUrl(const std::string&) { return false; }
bool isSyncDeviceDirName(const std::string&) { return false; }
std::string formatPreview(const std::string& utf8, size_t) { return utf8; }

// ---- DaemonCloudConfig ----
CloudClipboardConfig loadCloudClipboardConfig(const std::wstring&) {
  return CloudClipboardConfig{};  // enabled=false：一切云路径关闭
}
std::string loadCloudClipboardHotkey(const std::wstring&) {
  return std::string();  // 空热键=parse 失败=Ctrl+0 等永不匹配
}
bool saveCloudClipboardConfig(const CloudClipboardConfig&, const std::string&,
                              bool, const std::wstring&) {
  return false;
}
CloudClipboardSync::CloudClipboardSync(const CloudClipboardConfig&,
                                       const std::wstring&) {}
void CloudClipboardSync::reloadConfig(const CloudClipboardConfig&) {}
bool CloudClipboardSync::listClipsForDisplay(std::vector<DisplayItem>*,
                                             std::string* err) {
  *err = "Lite 版不含云剪贴板功能";
  return false;
}
bool CloudClipboardSync::deleteClip(const std::string&, std::string* err) {
  *err = "Lite 版不含云剪贴板功能";
  return false;
}
bool CloudClipboardSync::testConnection(std::string* err) {
  *err = "Lite 版不含云剪贴板功能";
  return false;
}
bool CloudClipboardSync::downloadUserDictSnapshots(const std::wstring&,
                                                   const std::string&, int*,
                                                   std::string* err) {
  *err = "Lite 版不含云剪贴板功能";
  return false;
}
bool CloudClipboardSync::uploadUserDictSnapshots(const std::wstring&,
                                                 const std::string&,
                                                 const std::string&, int*,
                                                 std::string* err) {
  *err = "Lite 版不含云剪贴板功能";
  return false;
}
CloudClipboardSync* globalCloudSync() { return nullptr; }
void setGlobalCloudSync(CloudClipboardSync*) {}

// ---- DaemonAI ----
bool DaemonAI::load() { return false; }  // 恒禁用：aiState_ 短路
const AIAction* DaemonAI::match(unsigned, bool, bool, bool) const {
  return nullptr;  // Ctrl+Shift+E/G 等热键永不触发
}
bool DaemonAI::generate(const AIAction&, const std::string&,
                        const std::vector<std::string>&, const std::string&,
                        std::vector<std::string>*, std::string* err) {
  *err = "Lite 版不含 AI 功能";
  return false;
}

// ---- 菜单"配置 AI 助手"入口（菜单已裁，防御性兜底） ----
std::wstring AiConfigPathUserW() { return std::wstring(); }
std::wstring AiConfigBundledPathW() { return std::wstring(); }

}  // namespace Moqi
