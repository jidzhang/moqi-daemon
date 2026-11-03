#pragma once
// 维护类菜单命令的 daemon 本地实现（Go 退休，2026-11-03）：
// 刷新配置(10) / 更新配置(17) / 导入皮肤(215)。
//
// 架构前提：引擎用户目录固定（daemon = UserDictMaster，DLL 每进程 p-*），
// 方案集目录只提供 in-proc patch 与 prebuilt 数据；因此"刷新配置"= 重新
// 灌 patch + full-check 重部署，调用方负责先销毁全部会话。
#include <string>

namespace Moqi {

// 从当前方案集目录重建 in-proc patch 写入引擎用户目录，再跑全量
// maintenance（阻塞到完成）。message = 面向用户的结果（UTF-8）。
bool runFullRedeploy(std::string* message);

// 对当前方案集目录执行 git pull（不改引擎）。调用方成功后自行重部署。
bool gitPullCurrentSchemeSet(std::string* message);

// 文件对话框选皮肤（.yaml/.yml/.json）→ 解析 → 落 appearance_themes.json
// 与 appearance_config.json。取消选择返回 false 且 message 为空。
bool importAppearanceSkinFromDialog(std::string* message);

// 同上但直接给路径（单测/无对话框场景）。rootOverride 仅供单测把落盘重定向
// 到临时目录（空 = %APPDATA%\Moqi）。
bool importAppearanceSkinFile(const std::wstring& path, std::string* message,
                              const std::wstring& rootOverride = L"");

// 维护命令结果提示（独立线程弹系统消息框，不占调用线程）。
void notifyUserMessage(const std::string& utf8Message, bool error);

} // namespace Moqi
