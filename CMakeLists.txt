cmake_minimum_required(VERSION 3.20)
project(MoqiDaemon CXX)

set(CMAKE_CXX_STANDARD 20)
add_definitions(/D_UNICODE=1 /DUNICODE=1 /utf-8 /DMOQI_ALLOW_X86_ENGINE)

# Reuse verified sources from moqi-im-windows (coupling documented in README).
set(MOQI_WIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../moqi-im-windows")
set(TS_DIR "${MOQI_WIN_DIR}/MoqiTextService")

set(JSONCPP_TESTS OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${MOQI_WIN_DIR}/jsoncpp" jsoncpp EXCLUDE_FROM_ALL)

add_executable(MoqiDaemon
  src/main.cpp
  src/ChildProxy.cpp
  src/ChildProxy.h
  src/DaemonAI.cpp
  src/DaemonAI.h
  src/DaemonEngine.cpp
  src/DaemonEngine.h
  src/DaemonMenuCore.cpp
  src/DaemonMenuCore.h
  src/DaemonMenuService.cpp
  src/DaemonMenuService.h
  src/PipeServe.cpp
  src/PipeServe.h
  src/MoqiDaemon.rc
  "${TS_DIR}/RimeCore.cpp"
  "${TS_DIR}/RimeCore.h"
  "${TS_DIR}/LocalRerank.cpp"
  "${TS_DIR}/LocalRerank.h"
)

# 5b 菜单纯核心回归测试：黄金树 = tests/fixtures/menu_golden.json
#（从 Go 后端实链抓取），行为先行锁定再实现。
add_executable(daemon_menu_test tests/daemon_menu_test.cpp src/DaemonMenuCore.cpp)
target_include_directories(daemon_menu_test PRIVATE src)
target_compile_definitions(daemon_menu_test PRIVATE
  TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
target_link_libraries(daemon_menu_test jsoncpp_static)
add_test(NAME daemon_menu_test COMMAND daemon_menu_test)

# 引擎级回归：shima 错字提顶后的显示/提交一致性（需 x86 rime.dll 布局，
# 环境变量由 ctest 注入；无环境时测试自跳过）。
add_executable(daemon_engine_desync_test
  tests/daemon_engine_desync_test.cpp
  src/DaemonAI.cpp
  src/DaemonAI.h
  src/DaemonEngine.cpp
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

target_link_libraries(MoqiDaemon jsoncpp_static)
