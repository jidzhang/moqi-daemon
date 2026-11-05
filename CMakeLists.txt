cmake_minimum_required(VERSION 3.20)
project(MoqiDaemon CXX)

# 新系统不一定装了 VC++ x86 运行库；MoqiDaemon 是整包里唯一动态链接 CRT
# 的组件（TSF DLL/SetupHelper 都是静态）。这里统一静态链接，避免 daemon
# 起不来导致 32 位应用直接上屏、语言栏按钮也不出现。
if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_CXX_STANDARD 20)
add_definitions(/D_UNICODE=1 /DUNICODE=1 /utf-8 /DMOQI_ALLOW_X86_ENGINE)

# Reuse verified sources from moqi-im-windows (coupling documented in README).
# Submodule layout (2026-11-05): daemon lives at <moqi-im-windows>/moqi-daemon,
# so the shared-engine tree is exactly one level up.
set(MOQI_WIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(TS_DIR "${MOQI_WIN_DIR}/MoqiTextService")

set(JSONCPP_TESTS OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${MOQI_WIN_DIR}/jsoncpp" jsoncpp EXCLUDE_FROM_ALL)

add_executable(MoqiDaemon
  src/main.cpp
  src/ClipboardMonitor.cpp
  src/ClipboardMonitor.h
  src/DaemonAI.cpp
  src/DaemonAI.h
  src/DaemonCloudConfig.cpp
  src/DaemonCloudConfig.h
  src/DaemonEngine.cpp
  src/DaemonEngine.h
  src/DaemonMaintenance.cpp
  src/DaemonMaintenance.h
  src/DaemonMenuCore.cpp
  src/DaemonMenuCore.h
  src/DaemonMenuService.cpp
  src/DaemonMenuService.h
  src/DaemonWebDAV.cpp
  src/DaemonWebDAV.h
  src/PipeServe.cpp
  src/PipeServe.h
  src/MoqiDaemon.rc
  "${TS_DIR}/RimeCore.cpp"
  "${TS_DIR}/RimeCore.h"
  "${TS_DIR}/LocalRerank.cpp"
  "${TS_DIR}/LocalRerank.h"
  "${MOQI_WIN_DIR}/libIME2/src/DebugLogConfig.cpp"
)

# 5e WebDAV 纯函数回归（守卫/命名/MD5/解析，不碰网络）。
add_executable(webdav_guard_test tests/webdav_guard_test.cpp src/DaemonWebDAV.cpp)
target_include_directories(webdav_guard_test PRIVATE src "${TS_DIR}"
  "${MOQI_WIN_DIR}" "${MOQI_WIN_DIR}/jsoncpp/include")
target_link_libraries(webdav_guard_test winhttp advapi32)
add_test(NAME webdav_guard_test COMMAND webdav_guard_test)

# 后台常驻进程：GUI 子系统，绝不弹控制台窗口（2026-11-02 用户实报黑框）。
# stdio 模式仍可用：启动器/Daemon 由父进程显式传入管道句柄，无需控制台。
set_target_properties(MoqiDaemon PROPERTIES WIN32_EXECUTABLE TRUE)
# WIN32 子系统但仍用标准 main()：指定 CRT 控制台入口，避免 WinMain 缺失。
set_target_properties(MoqiDaemon PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup")

# 5b 菜单纯核心回归测试：黄金树 = tests/fixtures/menu_golden.json
#（从 Go 后端实链抓取），行为先行锁定再实现。
add_executable(daemon_menu_test tests/daemon_menu_test.cpp src/DaemonMenuCore.cpp)
target_include_directories(daemon_menu_test PRIVATE src)
target_compile_definitions(daemon_menu_test PRIVATE
  TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
target_link_libraries(daemon_menu_test jsoncpp_static)
add_test(NAME daemon_menu_test COMMAND daemon_menu_test)

# 维护命令回归：皮肤 YAML/JSON 导入解析与落盘（rootOverride 重定向）。
add_executable(daemon_maintenance_test
  tests/daemon_maintenance_test.cpp
  src/DaemonMaintenance.cpp
  src/DaemonMaintenance.h
  "${TS_DIR}/RimeCore.cpp"
  "${TS_DIR}/LocalRerank.cpp")
target_include_directories(daemon_maintenance_test PRIVATE
  src "${TS_DIR}" "${MOQI_WIN_DIR}" "${MOQI_WIN_DIR}/jsoncpp/include")
target_link_libraries(daemon_maintenance_test jsoncpp_static comdlg32)
add_test(NAME daemon_maintenance_test COMMAND daemon_maintenance_test)

# 引擎级回归：shima 错字提顶后的显示/提交一致性（需 x86 rime.dll 布局，
# 环境变量由 ctest 注入；无环境时测试自跳过）。
add_executable(daemon_engine_desync_test
  tests/daemon_engine_desync_test.cpp
  src/DaemonAI.cpp
  src/DaemonAI.h
  src/DaemonCloudConfig.cpp
  src/DaemonCloudConfig.h
  src/DaemonEngine.cpp
  src/DaemonWebDAV.cpp
  src/DaemonWebDAV.h
  "${TS_DIR}/RimeCore.cpp"
  "${TS_DIR}/LocalRerank.cpp")
target_include_directories(daemon_engine_desync_test PRIVATE
  src "${TS_DIR}" "${MOQI_WIN_DIR}" "${MOQI_WIN_DIR}/proto"
  "${MOQI_WIN_DIR}/jsoncpp/include")
target_link_libraries(daemon_engine_desync_test jsoncpp_static)
add_test(NAME daemon_engine_desync_test COMMAND daemon_engine_desync_test)
set_tests_properties(daemon_engine_desync_test PROPERTIES
  ENVIRONMENT "MOQI_PROGRAM_DIR=C:/work/moqi-daemon-full;MOQI_RIME_DLL_DIR=C:/work/moqi-daemon-full")

target_include_directories(MoqiDaemon PRIVATE
  src
  "${TS_DIR}"
  "${MOQI_WIN_DIR}"
  "${MOQI_WIN_DIR}/proto"
  "${MOQI_WIN_DIR}/jsoncpp/include"
)

target_link_libraries(MoqiDaemon jsoncpp_static comdlg32)
