cmake_minimum_required(VERSION 3.20)
project(LustedExternal LANGUAGES CXX C)

if(NOT WIN32)
  message(FATAL_ERROR "LUSTED requires Windows APIs and a Windows target.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
  json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP true
)
FetchContent_MakeAvailable(json)

set(LUAU_BUILD_TESTS OFF CACHE INTERNAL "" FORCE)
set(LUAU_BUILD_BENCHMARKS OFF CACHE INTERNAL "" FORCE)
set(LUAU_BUILD_EXAMPLES OFF CACHE INTERNAL "" FORCE)
set(LUAU_BUILD_CLI OFF CACHE BOOL "Build the Luau command-line tools" FORCE)
set(BUILD_TESTING OFF CACHE INTERNAL "" FORCE)
FetchContent_Declare(
  luau
  GIT_REPOSITORY https://github.com/luau-lang/luau.git
  GIT_TAG 0.681
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(luau)
set_target_properties(
  Luau.CLI.lib Luau.Config Luau.Analysis Luau.EqSat Luau.CodeGen
  Luau.Require Luau.RequireNavigator isocline
  PROPERTIES EXCLUDE_FROM_ALL TRUE
)

FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.9b
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

function(lusted_configure target)
  target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
  if(MSVC)
    target_compile_options(${target} PRIVATE /W3 /EHsc /O2 /Ot /GL /arch:AVX2 /fp:fast /Oi)
    set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  else()
    target_compile_options(${target} PRIVATE -O3)
  endif()
endfunction()

add_library(lusted_process STATIC src/platform/windows/memory.cpp)
lusted_configure(lusted_process)

add_library(lusted_roblox STATIC
  src/roblox/offsets_fetcher.cpp
  src/roblox/roblox_finder.cpp
  src/roblox/instance_offsets.cpp
  src/roblox/instance_index.cpp
  src/roblox/instance_properties.cpp
  src/roblox/instance_services.cpp
)
lusted_configure(lusted_roblox)
target_link_libraries(lusted_roblox PRIVATE lusted_process nlohmann_json::nlohmann_json winhttp version psapi)

add_library(lusted_ui STATIC
  src/ui/ui.cpp
  src/ui/explorer.cpp
  src/ui/drawing.cpp
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
lusted_configure(lusted_ui)
target_include_directories(lusted_ui PRIVATE ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(lusted_ui
  PRIVATE lusted_roblox nlohmann_json::nlohmann_json
          gdi32 user32 comctl32 d3d11 dxgi d3dcompiler dwmapi imm32
)

add_library(lusted_runtime STATIC src/runtime/luau_manager.cpp)
lusted_configure(lusted_runtime)
target_link_libraries(lusted_runtime PRIVATE lusted_roblox lusted_ui Luau.VM Luau.Compiler)

add_executable(lusted src/app/main.cpp)
lusted_configure(lusted)
target_link_libraries(lusted PRIVATE lusted_runtime lusted_ui lusted_roblox lusted_process)
if(MSVC)
  target_link_options(lusted PRIVATE /LTCG /OPT:REF /OPT:ICF)
endif()

# Runtime offsets are cached under C:/LUSTED/offsets/.
# User Luau scripts are auto-executed from C:/LUSTED/Luas/.
