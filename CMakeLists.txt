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
  src/DaemonEngine.cpp
  src/DaemonEngine.h
  "${TS_DIR}/RimeCore.cpp"
  "${TS_DIR}/RimeCore.h"
  "${TS_DIR}/LocalRerank.cpp"
  "${TS_DIR}/LocalRerank.h"
)

target_include_directories(MoqiDaemon PRIVATE
  src
  "${TS_DIR}"
  "${MOQI_WIN_DIR}"
  "${MOQI_WIN_DIR}/proto"
  "${MOQI_WIN_DIR}/jsoncpp/include"
)

target_link_libraries(MoqiDaemon jsoncpp_static)
