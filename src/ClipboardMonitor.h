#pragma once
// 5e 剪贴板监听（Go MoqiLaunch PipeServer 的 WM_CLIPBOARDUPDATE 等价物）。
// daemon 建隐藏窗口 + AddClipboardFormatListener，收到变化后 500ms 防抖
// 取 CF_UNICODETEXT → CloudClipboardSync::uploadFromRequest。
// 线程模型：消息循环跑在独立线程（不占 dispatch 锁）。
#include <windows.h>

namespace Moqi {

// 启动剪贴板监听线程（幂等）。配置未启用时监听挂空（无上传动作）。
void startClipboardMonitor();

// 停止监听（进程退出时自动）。
void stopClipboardMonitor();

}  // namespace Moqi
