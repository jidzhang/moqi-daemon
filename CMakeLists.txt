cmake_minimum_required(VERSION 3.20)
project(MoqiDaemon CXX)

# 新系统不一定装了 VC++ x86 运行库；MoqiDaemon 是整包里唯一动态链接 CRT
# 的组件（TSF DLL/SetupHelper 都是静态）。这里统一静态链接，避免 daemon
# 起不来导致 32 位应用直接上屏、语言栏按钮也不出现。
if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Release 优化：/Gy 函数级链接 + /OPT:REF,ICF 死代码消除/相同 COMDAT 折叠（尺寸）。
# LTCG 全程序优化默认开：同源 A/B n=4 实测 wall/CPU 约 −8%（大代码量收益实证，
# 代价体积 +~86KB）；小项目（TSF）无此正收益结论，不启用。PGO 已证每键慢
# 7-10%（56-交接），不启用。
option(MOQI_LTCG "LTCG whole-program opt: lower CPU, larger size" ON)
if(MOQI_LTCG)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Gy")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /Gy")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /OPT:REF /OPT:ICF")

# Version resource: parse the outer repo's version.txt (moqi-im-windows root,
# written by release packaging / CI from the release tag) into MAJOR/MINOR/PATCH
# and configure src/MoqiVersion.rc.in. Path derived from this source dir, not
# MOQI_WIN_DIR (declared further below). Missing or malformed file falls back
# to 0.0.0 rather than failing standalone daemon test builds.
set(MOQI_VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../version.txt")
set(MOQI_VERSION_MAJOR 0)
set(MOQI_VERSION_MINOR 0)
set(MOQI_VERSION_PATCH 0)
if(EXISTS "${MOQI_VERSION_FILE}")
  file(READ "${MOQI_VERSION_FILE}" MOQI_VERSION_STRING)
  string(STRIP "${MOQI_VERSION_STRING}" MOQI_VERSION_STRING)
  if(MOQI_VERSION_STRING MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(MOQI_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(MOQI_VERSION_MINOR "${CMAKE_MATCH_2}")
    set(MOQI_VERSION_PATCH "${CMAKE_MATCH_3}")
  endif()
endif()
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/MoqiVersion.rc.in"
  "${CMAKE_CURRENT_BINARY_DIR}/MoqiVersion.rc"
  @ONLY
)

set(CMAKE_CXX_STANDARD 20)
add_definitions(/D_UNICODE=1 /DUNICODE=1 /utf-8 /DMOQI_ALLOW_X86_ENGINE)

# Reuse verified sources from moqi-im-windows (coupling documented in README).
# Submodule layout (2026-11-05): daemon lives at <moqi-im-windows>/moqi-daemon,
# so the shared-engine tree is exactly one level up.
set(MOQI_WIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(TS_DIR "${MOQI_WIN_DIR}/MoqiTextService")

# JSONCPP_WITH_TESTS 才是 jsoncpp 1.9.6 的真选项名（此前误写 JSONCPP_TESTS，
# 形同没关：自带的 3 个测试被注册进 ctest 但 exe 又不在 ALL 里，跑一次挂一次）。
set(JSONCPP_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${MOQI_WIN_DIR}/jsoncpp" jsoncpp EXCLUDE_FROM_ALL)

# add_test 生效前提（此前漏了，ctest 报 No tests were found）。
enable_testing()

# Lite 版编译期裁剪（2026-11-06 用户定稿）：剔除 AI/云剪贴板/WebDAV/剪贴板
# 监视四个模块——配置文件存在也没有任何代码消费它。全量版默认 OFF 零变化。
option(MOQI_LITE "Lite edition: compile out AI/cloud-clipboard/WebDAV" OFF)
if(MOQI_LITE)
  add_compile_definitions(MOQI_LITE)
endif()

set(DAEMON_MAIN_SOURCES
  src/main.cpp
  src/DaemonEngine.cpp
  src/DaemonEngine.h
  src/DaemonMaintenance.cpp
  src/DaemonMaintenance.h
  src/DaemonMenuCore.cpp
  src/DaemonMenuCore.h
  src/DaemonMenuService.cpp
  src/DaemonMenuService.h
  src/PipeServe.cpp
  src/PipeServe.h
  src/MoqiDaemon.rc
  "${CMAKE_CURRENT_BINARY_DIR}/MoqiVersion.rc"
  "${TS_DIR}/RimeCore.cpp"
  "${TS_DIR}/RimeCore.h"
  "${TS_DIR}/LocalRerank.cpp"
  "${TS_DIR}/LocalRerank.h"
  "${MOQI_WIN_DIR}/libIME2/src/DebugLogConfig.cpp"
)
if(NOT MOQI_LITE)
  list(APPEND DAEMON_MAIN_SOURCES
    src/ClipboardMonitor.cpp
    src/ClipboardMonitor.h
    src/DaemonAI.cpp
    src/DaemonAI.h
    src/DaemonCloudConfig.cpp
    src/DaemonCloudConfig.h
    src/DaemonWebDAV.cpp
    src/DaemonWebDAV.h
  )
else()
  list(APPEND DAEMON_MAIN_SOURCES src/DaemonLiteStubs.cpp)
endif()

add_executable(MoqiDaemon ${DAEMON_MAIN_SOURCES})

# 5e WebDAV 纯函数回归（守卫/命名/MD5/解析，不碰网络）。Lite 构建无该源。
# Lite 菜单裁剪契约测试（仅 MOQI_LITE 构建；全量金样测试只在全量构建）。
if(MOQI_LITE)
add_executable(lite_menu_test tests/lite_menu_test.cpp src/DaemonMenuCore.cpp)
target_include_directories(lite_menu_test PRIVATE src "${TS_DIR}"
  "${MOQI_WIN_DIR}" "${MOQI_WIN_DIR}/jsoncpp/include")
target_link_libraries(lite_menu_test jsoncpp_static)
add_test(NAME lite_menu_test COMMAND lite_menu_test)
endif()

if(NOT MOQI_LITE)
add_executable(webdav_guard_test tests/webdav_guard_test.cpp src/DaemonWebDAV.cpp)
target_include_directories(webdav_guard_test PRIVATE src "${TS_DIR}"
  "${MOQI_WIN_DIR}" "${MOQI_WIN_DIR}/jsoncpp/include")
target_link_libraries(webdav_guard_test winhttp advapi32)
add_test(NAME webdav_guard_test COMMAND webdav_guard_test)
endif()

# 后台常驻进程：GUI 子系统，绝不弹控制台窗口（2026-11-02 用户实报黑框）。
# stdio 模式仍可用：启动器/Daemon 由父进程显式传入管道句柄，无需控制台。
set_target_properties(MoqiDaemon PROPERTIES WIN32_EXECUTABLE TRUE)
# WIN32 子系统但仍用标准 main()：指定 CRT 控制台入口，避免 WinMain 缺失。
set_target_properties(MoqiDaemon PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup")

# 5b 菜单纯核心回归测试：黄金树 = tests/fixtures/menu_golden.json
#（从 Go 后端实链抓取），行为先行锁定再实现。
if(NOT MOQI_LITE)
add_executable(daemon_menu_test tests/daemon_menu_test.cpp src/DaemonMenuCore.cpp)
target_include_directories(daemon_menu_test PRIVATE src)
target_compile_definitions(daemon_menu_test PRIVATE
  TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
target_link_libraries(daemon_menu_test jsoncpp_static)
add_test(NAME daemon_menu_test COMMAND daemon_menu_test)

# 命令分发边界回归（与 daemon_menu_test 互补）：模式开关/越界号/主题段
# 上界/竖排回横排兜底/外观表全命中/自定义颜色切 custom/222 冲突。
add_executable(daemon_menu_dispatch_test tests/daemon_menu_dispatch_test.cpp
  src/DaemonMenuCore.cpp)
target_include_directories(daemon_menu_dispatch_test PRIVATE src)
target_link_libraries(daemon_menu_dispatch_test jsoncpp_static)
add_test(NAME daemon_menu_dispatch_test COMMAND daemon_menu_dispatch_test)
endif()

# 数字/句点原生直通（bug#20）纯逻辑回归：T9 方案识别 + alphabet 数字识别。
# 判错后果：T9 打不了字 / Word 丢句点复活。头文件即被测物，无需链接库。
add_executable(digit_passthrough_test tests/digit_passthrough_test.cpp)
target_include_directories(digit_passthrough_test PRIVATE src)
add_test(NAME digit_passthrough_test COMMAND digit_passthrough_test)

# 管道帧协议回归（历史事故：>4096B 帧被静默丢块）+ 健壮性边界
#（损坏长度前缀不得让接收方无限等待/无限分配）。
add_executable(proto_framing_test tests/proto_framing_test.cpp)
target_include_directories(proto_framing_test PRIVATE
  "${MOQI_WIN_DIR}/proto" "${MOQI_WIN_DIR}/jsoncpp/include")
target_link_libraries(proto_framing_test jsoncpp_static)
add_test(NAME proto_framing_test COMMAND proto_framing_test)

# 产品行为补丁体回归（10-26 配置脱节事故锚：空配置绝不产生毒键；
# 开关行逐字节锁定）。被测物 = MoqiTextService/LocalBehaviorPatch.h。
add_executable(local_behavior_patch_test tests/local_behavior_patch_test.cpp)
target_include_directories(local_behavior_patch_test PRIVATE
  "${TS_DIR}" "${MOQI_WIN_DIR}/jsoncpp/include")
target_link_libraries(local_behavior_patch_test jsoncpp_static)
add_test(NAME local_behavior_patch_test COMMAND local_behavior_patch_test)

# 托管补丁合并守卫回归（2026-11-08 档位升级事故锚：带 marker 的旧输出
# 不回并——否则每次 daemon 启动追加一块，旧档 grammar/language 压住新档）。
add_executable(managed_patch_merge_test tests/managed_patch_merge_test.cpp
  "${TS_DIR}/RimeCore.cpp" "${TS_DIR}/RimeCore.h")
target_include_directories(managed_patch_merge_test PRIVATE
  "${TS_DIR}" "${MOQI_WIN_DIR}" "${MOQI_WIN_DIR}/jsoncpp/include")
target_link_libraries(managed_patch_merge_test jsoncpp_static)
add_test(NAME managed_patch_merge_test COMMAND managed_patch_merge_test)

# 重排层纯逻辑回归（UTF-8 工具 + 短窗连贯）。直接影响候选排序正确性。
# RimeCore.cpp 提供 utf8ToWide/fileMtime（LocalRerank.cpp 的传递依赖）。
add_executable(local_rerank_test tests/local_rerank_test.cpp
  "${TS_DIR}/LocalRerank.cpp" "${TS_DIR}/LocalRerank.h"
  "${TS_DIR}/RimeCore.cpp" "${TS_DIR}/RimeCore.h")
target_include_directories(local_rerank_test PRIVATE
  "${TS_DIR}" "${MOQI_WIN_DIR}" "${MOQI_WIN_DIR}/jsoncpp/include")
target_link_libraries(local_rerank_test jsoncpp_static)
add_test(NAME local_rerank_test COMMAND local_rerank_test)

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

# 云剪贴板配置层回归：JSON 解析/默认值兜底、DPAPI 密码回环、明文密码
# 过渡迁移、出站总闸文件语义、同步引擎零网络校验路径。
# rootOverride 把读写重定向到测试目录，不碰真实 %APPDATA%\Moqi。
add_executable(cloud_config_test tests/cloud_config_test.cpp
  src/DaemonCloudConfig.cpp
  src/DaemonCloudConfig.h
  src/DaemonWebDAV.cpp
  src/DaemonWebDAV.h
  "${TS_DIR}/RimeCore.cpp"
  "${TS_DIR}/LocalRerank.cpp")
target_include_directories(cloud_config_test PRIVATE
  src "${TS_DIR}" "${MOQI_WIN_DIR}" "${MOQI_WIN_DIR}/jsoncpp/include")
target_link_libraries(cloud_config_test jsoncpp_static crypt32 winhttp advapi32)
add_test(NAME cloud_config_test COMMAND cloud_config_test)

# 引擎级回归：shima 错字提顶后的显示/提交一致性（需 x86 rime.dll 布局，
# 环境变量由 ctest 注入；布局不在/无环境时测试自跳过）。
# 布局 = 安装根形态（根下 x86 rime.dll + moqi-ime\input_methods\rime\data；
# backendDir 由测试自己补成 moqi-ime）。用户目录重定向到构建目录的
# 临时目录——真用户词典绝不让测试进程碰（LevelDB 非并发安全）。
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
# desync 测试要装好的产品树（moqi-ime 数据布局 + 与测试进程同架构的
# rime.dll）。产品树只带 x86 rime.dll，所以 x86 构建默认注入安装目录约定
# （AGENTS 规范 14 允许的例外）真跑；x64 构建默认留空 → 测试内 SKIP
#（树缺失不是回归）。曾硬编码老机器 C:/work/moqi-daemon-full（内含 x64
# dll），换机重配后 desync 常红。
set(MOQI_DESYNC_PROGRAM_DIR "C:/Program Files (x86)/MoqiIM"
    CACHE PATH "Installed product tree for daemon_engine_desync_test")
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  set(MOQI_DESYNC_RIME_DLL_DIR_DEFAULT "${MOQI_DESYNC_PROGRAM_DIR}")
else()
  set(MOQI_DESYNC_RIME_DLL_DIR_DEFAULT "")
endif()
set(MOQI_DESYNC_RIME_DLL_DIR "${MOQI_DESYNC_RIME_DLL_DIR_DEFAULT}"
    CACHE PATH "rime.dll dir matching test arch (empty = skip)")
if(MOQI_DESYNC_RIME_DLL_DIR)
  set_tests_properties(daemon_engine_desync_test PROPERTIES
    ENVIRONMENT "MOQI_PROGRAM_DIR=${MOQI_DESYNC_PROGRAM_DIR};MOQI_RIME_DLL_DIR=${MOQI_DESYNC_RIME_DLL_DIR};MOQI_USER_DATA_DIR=${CMAKE_CURRENT_BINARY_DIR}/desync-user-data")
endif()

target_include_directories(MoqiDaemon PRIVATE
  src
  "${TS_DIR}"
  "${MOQI_WIN_DIR}"
  "${MOQI_WIN_DIR}/proto"
  "${MOQI_WIN_DIR}/jsoncpp/include"
)

target_link_libraries(MoqiDaemon jsoncpp_static comdlg32)
