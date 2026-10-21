cmake_minimum_required(VERSION 3.20)
project(MissingDogs LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W3 /utf-8)
else()
  add_compile_options(-Wall -Wextra)
endif()

# ---------------------------------------------------------------- 规则核心
# 零依赖静态库：不引用 Godot，不引用 socket。
# 三处链接：md_sim（模拟器）、md_server（联机服务器）、
#           godot/addons/md_core（GDExtension，单机进程内）。
add_library(md_core STATIC
  core/src/core.cpp
  core/src/ai.cpp
)
target_include_directories(md_core PUBLIC core/include)

# ---------------------------------------------------------------- 线协议
# Command 的解析 与 PlayerView 的编码。服务器和 GDExtension 共用，
# 这是"单机与联机走同一套协议"的落点。
add_library(md_net STATIC
  net/src/wire.cpp
  net/src/room.cpp
)
target_include_directories(md_net PUBLIC net/include)
target_link_libraries(md_net PUBLIC md_core)

# ---------------------------------------------------------------- 批量对局模拟器
add_executable(md_sim apps/md_sim/main.cpp)
target_link_libraries(md_sim PRIVATE md_core)

# ---------------------------------------------------------------- 无网络端到端自检
# 只链接 md_core + md_net，不碰 socket，因此任何环境都能跑。
add_executable(md_selftest apps/md_selftest/main.cpp)
target_link_libraries(md_selftest PRIVATE md_core md_net)

# ---------------------------------------------------------------- headless 服务器
add_executable(md_server apps/md_server/main.cpp)
target_link_libraries(md_server PRIVATE md_core md_net)
if(WIN32)
  target_link_libraries(md_server PRIVATE ws2_32)
endif()
