cmake_minimum_required(VERSION 3.20)
project(MissingDogs LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W3 /utf-8)
  # godot-cpp 在 Windows 上固定用静态 CRT（MT），Godot 本体也是。
  # 我们的静态库必须一致，否则链接 GDExtension 时会报 LNK2038 RuntimeLibrary 不匹配。
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  add_compile_options(-Wall -Wextra)
endif()

# ---------------------------------------------------------------- 规则核心
# 零依赖静态库：不引用 Godot，不引用 socket。
add_library(md_core STATIC
  core/src/core.cpp
  core/src/ai.cpp
)
target_include_directories(md_core PUBLIC core/include)

# ---------------------------------------------------------------- 线协议
# Command 的解析 与 PlayerView 的编码，外加不依赖 socket 的 Room。
# 服务器与 GDExtension 共用，这是"单机与联机走同一套协议"的落点。
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
add_executable(md_selftest apps/md_selftest/main.cpp)
target_link_libraries(md_selftest PRIVATE md_core md_net)

# ---------------------------------------------------------------- headless 服务器
add_executable(md_server apps/md_server/main.cpp)
target_link_libraries(md_server PRIVATE md_core md_net)
if(WIN32)
  target_link_libraries(md_server PRIVATE ws2_32)
endif()

# ---------------------------------------------------------------- stdio 服务器
# 不碰 socket、不碰 Godot：让 Godot 用管道把服务器当子进程跑。
# 用途是在"dll 与 socket 都被策略拦掉"的环境里仍能做进程级联调。
add_executable(md_stdio apps/md_stdio/main.cpp)
target_link_libraries(md_stdio PRIVATE md_core md_net)

# ---------------------------------------------------------------- GDExtension
# 需要先准备 third_party/godot-cpp（脚本见 tools/fetch-godot-cpp.ps1）。
#   cmake -B build-gdext -G "Visual Studio 18 2026" -A x64 \
#         -DGODOTCPP_TARGET=template_release -DMD_BUILD_GDEXTENSION=ON
#   cmake --build build-gdext --config Release
option(MD_BUILD_GDEXTENSION "Build the Godot 4 GDExtension (requires godot-cpp)" OFF)

if(MD_BUILD_GDEXTENSION)
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/godot-cpp/CMakeLists.txt)
    message(FATAL_ERROR "third_party/godot-cpp is missing. Run tools/fetch-godot-cpp.ps1 first.")
  endif()
  add_subdirectory(third_party/godot-cpp ${CMAKE_BINARY_DIR}/godot-cpp)

  add_library(md_gdext SHARED
    godot/addons/md_core/src/register_types.cpp
    godot/addons/md_core/src/md_local_server.cpp
  )
  target_include_directories(md_gdext PRIVATE godot/addons/md_core/src)
  target_link_libraries(md_gdext PRIVATE md_core md_net godot-cpp)

  set(GDEXT_OUT ${CMAKE_CURRENT_SOURCE_DIR}/godot/addons/md_core/bin)
  set_target_properties(md_gdext PROPERTIES
    PREFIX ""
    OUTPUT_NAME "md_core.windows.x86_64"
    RUNTIME_OUTPUT_DIRECTORY ${GDEXT_OUT}
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${GDEXT_OUT}
    LIBRARY_OUTPUT_DIRECTORY ${GDEXT_OUT}
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${GDEXT_OUT}
  )
endif()
