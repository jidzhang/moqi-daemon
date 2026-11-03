#pragma once
// 5b 菜单应用层：从 rime 引擎与磁盘配置采集 MenuState，执行本地命令的
// 引擎/落盘/打开动作。纯逻辑（树构建/命令语义）在 DaemonMenuCore，
// 行为由 tests/daemon_menu_test 锁定；本层只做 I/O 与引擎对接。
#include <string>
#include <vector>

#include "DaemonEngine.h"
#include "DaemonMenuCore.h"

namespace Moqi {

// 用随包编辑器打开文本文件：{installRoot}\Notepad4\Notepad4.exe（zh-Hans
// Win32 单文件，BSD-3）→ 系统 notepad.exe → 系统关联。语言栏"打开 xx 配置"
// 类命令统一走这里（2026-11-03 用户定：裸记事本太傻，带语法高亮）。
void openTextInBundledEditor(const std::wstring& installRoot,
                             const std::wstring& file);

struct ThemeDefinition {
  std::string id;
  std::string label;
  // appearance_config 片段（内置主题只有颜色键；导入皮肤可带字体/排版）。
  Json::Value appearance;
};

class DaemonMenuService {
public:
  DaemonMenuService(std::wstring installRoot, std::wstring rimeDataDir,
                    std::wstring appDataDir);

  // 采集菜单全量输入（读引擎选项 + 各配置文件）。
  Menu::MenuState gather(DaemonEngine& engine);

  // 本地命令执行后的应用：引擎选项/方案切换 + appearance_config.json 落盘。
  void applyEngine(DaemonEngine& engine, const Menu::CommandResult& r);
  void persistAppearance(const Menu::MenuState& state);
  // 主题命令后：把所选主题的 4 色覆写到 state（持久化前调用）。
  void applyThemeColors(Menu::MenuState& state);

  // 文件夹/URL 命令的实际打开动作（本地命令里 id 对应的目标）。
  void openCommandTarget(int id);

  // Go 退休：ON_ACTIVATE 的本地应答（等价 Go addButtons+customizeUi+
  // removePreservedKey 三件套）。图标按当前 ascii/full 选 eng/chi、half/full。
  Json::Value buildActivatePayload(DaemonEngine& engine);
  // customizeUi 全量（GUI 变更随响应下发的部分；导入皮肤后实时刷新用）。
  Json::Value buildCustomizeUi(const Menu::MenuState& state);

private:
  std::vector<Menu::MenuSwitchDef> loadSwitchDefs(DaemonEngine& engine);
  std::vector<ThemeDefinition> loadThemes();
  Json::Value loadAppearanceConfig();

  std::wstring installRoot_;   // 安装根（moqi-ime 的父目录）
  std::wstring rimeDataDir_;   // <install>\moqi-ime\input_methods\rime\data
  std::wstring appDataDir_;    // %APPDATA%\Moqi
  std::vector<ThemeDefinition> themeDefs_; // gather 时刷新
};

} // namespace Moqi
