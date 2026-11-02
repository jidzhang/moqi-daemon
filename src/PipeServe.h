#pragma once
// 阶段 5c：daemon 管道服务模式（MoqiLaunch 退休）。
// 绑定启动器同款管道名 \\.\pipe\<用户>\MoqiIM\Launcher，多客户端并发接入，
// 每连接注入 clientId、按 clientId 路由回包——对 TSF DLL 透明
//（DLL 零改动即可连上；拉起方由 MoqiLaunch.exe 换成 MoqiDaemon.exe）。
#include <functional>
#include <string>

#include <windows.h>

namespace Moqi {

// main.cpp 提供：单帧请求的完整处理（热路径/菜单/命令/代理 Go）。
void MoqiServeDispatch(const std::string& payload,
                       const std::function<void(const std::string&)>& reply);

class PipeServe {
public:
  PipeServe();
  ~PipeServe();

  // 阻塞运行接受循环（每个客户端一个线程）。返回 = requestStop 或致命错误。
  int run();

  // Go 子进程读者线程入口：按帧内 clientId 路由到对应客户端管道；
  // 找不到（dm-drop: 镜像回包 / 已断开）静默丢弃。
  void routeFrame(const std::string& framed);

  // 维护命令「退出引擎」用：请求接受循环退出（进程随后结束，
  // TSF 侧 ensureLauncherRunning 会在下一次按键把 daemon 拉起来）。
  void requestStop();

  struct Impl; // public：实现文件里的连接线程需要访问

private:
  Impl* impl_;
};

} // namespace Moqi
