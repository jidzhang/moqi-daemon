// OS 版本判定（2026-11-19）。
//
// 为什么不用 IsWindows8OrGreater：VersionHelpers.h 的实现依赖
// VerifyVersionInfo，而本项目用 MOQI_TARGET_WIN7 编译时 _WIN32_WINNT=0x0601，
// 那个头根本不提供它。RtlGetVersion 是 ntdll 的老 API，Win7 就有，且不受
// manifest 的兼容性声明影响（GetVersionEx 会撒谎，RtlGetVersion 不会）。
#pragma once

#include <windows.h>

namespace Moqi {

inline bool osIsWin8Plus() {
  static const bool cached = []() -> bool {
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) return true;  // 拿不到就按新系统处理（不挂托盘）
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlGetVersion")));
    if (rtlGetVersion == nullptr) return true;
    RTL_OSVERSIONINFOW vi = {};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (rtlGetVersion(&vi) != 0) return true;
    return vi.dwMajorVersion > 6 ||
           (vi.dwMajorVersion == 6 && vi.dwMinorVersion >= 2);
  }();
  return cached;
}

}  // namespace Moqi