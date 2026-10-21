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

# 规则核心：零依赖静态库。
# 同一份代码会被三处链接：md_sim（模拟器）、md_server（联机服务器）、
# 以及 godot/addons/md_core（GDExtension，单机进程内）。
add_library(md_core STATIC
  core/src/core.cpp
  core/src/ai.cpp
)
target_include_directories(md_core PUBLIC core/include)

# 批量对局模拟器
add_executable(md_sim apps/md_sim/main.cpp)
target_link_libraries(md_sim PRIVATE md_core)
