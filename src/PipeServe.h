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
// clientId 走参数注入（2026-11-08 性能刀一）：旧实现 PipeServe 先 parse
// 注入 clientId 再整帧重新序列化、dispatchRequest 又 parse 第二遍——每帧
// 两次解析一次多余序列化。现在 PipeServe 零 JSON，dispatchRequest 只解析
// 一次并把 clientId 写入解析后的对象，下游 req.get("clientId") 不变。
void MoqiServeDispatch(const std::string& payload, const std::string& clientId,
                       const std::function<void(const std::string&)>& reply);

class PipeServe {
public:
  PipeServe();
  ~PipeServe();

  // 阻塞运行接受循环（每个客户端一个线程）。返回 = requestStop 或致命错误。
  int run();

  // 是否已抢到管道（第一个实例）。预热线程必须等它变 true，避免多实例
  // 竞争时每个进程都建一份引擎。
  bool ownsPipe() const;

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
