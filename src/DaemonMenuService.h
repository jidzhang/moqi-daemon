#pragma once
// 5b 菜单应用层：从 rime 引擎与磁盘配置采集 MenuState，执行本地命令的
// 引擎/落盘/打开动作。纯逻辑（树构建/命令语义）在 DaemonMenuCore，
// 行为由 tests/daemon_menu_test 锁定；本层只做 I/O 与引擎对接。
#include <string>
#include <vector>

#include "DaemonEngine.h"
#include "DaemonMenuCore.h"

namespace Moqi {

struct ThemeDefinition {
  std::string id;
  std::string label;
  // appearance 值（当前内置主题只带 4 个颜色键）
  std::string bgColor, hlColor, textColor, hlTextColor;
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
