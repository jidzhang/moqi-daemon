#pragma once
// 托盘图标入口（2026-11-19）：Windows 7 没有语言栏，TSF 的 ITfLangBarItemMgr
// 按钮在那台机器上没有显示位置，菜单/设置/AI 配置全部无法触达。本模块给
// daemon 加一个常驻托盘图标，右键弹出与语言栏同源的菜单树（buildMenu），
// 命令经回调交给 main.cpp 走与 DLL 相同的分发路径——业务逻辑零复制。
//
// 只在 Win7 启用：Win8+ 有语言栏，两处入口会让用户困惑。环境变量
// MOQI_FORCE_TRAY=1 可强制开启（调试/无语言栏的宿主用）。
#include <functional>
#include <string>

#include <json/json.h>

namespace Moqi {

// main.cpp 注册：产出当前菜单树（实时 gather，不缓存）。
using TrayMenuTreeProvider = std::function<Json::Value()>;
// main.cpp 注册：执行一个菜单命令 id（内部走 MoqiServeDispatch）。
using TrayCommandExecutor = std::function<void(int)>;

struct TrayCallbacks {
  TrayMenuTreeProvider buildMenuTree;
  TrayCommandExecutor executeCommand;
};

// 启动托盘线程（幂等）。callbacks 为空或启动失败只记日志，不影响其他功能。
void startTrayIcon(TrayCallbacks callbacks);

// 停止托盘并移除图标（进程退出前调用；不调用则图标会残留到鼠标划过）。
void stopTrayIcon();

} // namespace Moqi
