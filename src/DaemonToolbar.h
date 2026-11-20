#pragma once
// 输入工具栏（2026-11-19）：固定在屏幕右下角的 4 按钮浮层。
//
// 为什么在 daemon 而不是 DLL：
//   DLL 在每个宿主进程里各跑一份。工具栏若做在 DLL 且常驻显示，记事本、
//   浏览器、IDE 会各自叠出一个窗口，全部落在右下角同一位置——用户只看到
//   一个（最上层那个），点击只改到它所属进程的状态，其他应用纹丝不动。
//   daemon 是单实例，做这里才能保证"一个工具栏、一次切换全局生效"。
//
// 显示策略（用户 2026-11-19 定）：
//   - 常驻显示，不隐藏（不做"打字才浮出"）。
//   - Win7 默认开；Win10 默认关（有语言栏，避免重复入口）。
//   - 与托盘图标互斥：开了工具栏就不创建托盘。
//
// 四个按钮：菜单 / 中英 / 标点 / 全半角。点击通过回调交给 main.cpp，
// 由它走 MoqiServeDispatch 这条与语言栏完全相同的命令路径。
#include <functional>
#include <string>

#include <json/json.h>

namespace Moqi {

// main.cpp 提供：读三项开关的当前值（从菜单 gather 拿，状态永远实时）。
struct ToolbarState {
  bool ascii = false;
  bool fullShape = false;
  bool asciiPunct = false;
};

using ToolbarStateProvider = std::function<ToolbarState()>;
// 执行一个命令 id。
// 注意（2026-11-19 实测踩坑）：不能走 MoqiServeDispatch + 虚拟 clientId——
// 那只会翻转"daemon-tray"这个虚拟客户端的引擎，用户真正在用的宿主引擎
// 毫发无损，表现就是"点了切英文，按键盘还是汉字"。
// 必须由 main.cpp 遍历所有活跃客户端引擎逐个 setOption。
using ToolbarCommandExecutor = std::function<void(int)>;
// 拉完整菜单树（复用 buildMenu，与语言栏/托盘同一棵）。
using ToolbarMenuTreeProvider = std::function<Json::Value()>;

struct ToolbarCallbacks {
  ToolbarStateProvider readState;
  ToolbarCommandExecutor executeCommand;
  ToolbarMenuTreeProvider buildMenuTree;
  // toolbar_enabled 从 false 变 true 时通知（main.cpp 用它撤掉托盘，
  // 保证"开工具栏就不挂托盘"这条互斥在配置热改时也成立）。
  std::function<void(bool enabled)> onEnabledChanged;
  // 前台窗口的进程是否由我们服务（daemon 侧查 Pipeline 的 clientPids）。
  // 这是工具栏显隐的**权威依据**：切换输入法时 TSF 不一定调 Deactivate
  //（不同宿主行为不一），但前台窗口一定换人。返回 true = 前台是我们的。
  std::function<bool()> isForegroundServed;
};

// 启动常驻工具栏线程（幂等）。失败只记日志，不影响其他功能。
void startToolbar(ToolbarCallbacks callbacks);

// 移除工具栏（进程退出前调用）。
void stopToolbar();

// 活跃客户端计数变化时调用（客户端的 onActivate / onDeactivate 驱动）。
// 有客户端激活就显示（用户切到墨奇输入法），全部失活才隐藏——与候选窗
// 无关，候选窗是"打字时"出现，工具栏是"输入法是当前输入法时"出现。
void setToolbarActiveCount(int count);

// 配置改动后调用：重读 toolbar_enabled 并决定是否显示。
void refreshToolbarVisibility();

// 设置配置里的 toolbar_enabled（Win7 默认 true、Win8+ 默认 false，
// main.cpp 读配置后传进来）。立即生效，无需重启 daemon。
void setToolbarEnabled(bool enabled);

// 状态可能变化时调用（命令执行后），立即重绘按钮而不等轮询节拍。
void pokeToolbarState();

// 引擎侧直接推状态进来（2026-11-20）。
//
// 之前是工具栏线程每 250ms 主动调 rimeGetOption 读三项开关。那条路要在
// 界面线程/轮询线程拿 rime 的锁，而 rime 正在重建会话（用户词典合并、
// 部署）时会阻塞数秒——用户看到的就是"鼠标变等待光标、点一下无响应、
// 过好多秒才能点"。
//
// 现在改成：引擎每次产生状态（按键回包、命令回包、激活、工具栏点击）都
// 调这里一次，工具栏只存快照并重绘，**永不主动读引擎**。
void updateToolbarState(bool ascii, bool fullShape, bool asciiPunct);

} // namespace Moqi