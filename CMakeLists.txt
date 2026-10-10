cmake_minimum_required(VERSION 3.20)
project(LustedExternal LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
  message(FATAL_ERROR "Lusted external is Windows-only (RobloxPlayerBeta.exe + WinHTTP + NTAPI).")
endif()

include(FetchContent)

# --- nlohmann/json (offsets.json / fflags.json parsing) ---
FetchContent_Declare(
  json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP true
)
FetchContent_MakeAvailable(json)

# --- Luau (VM + Compiler). Features are written in Luau. ---
set(LUAU_BUILD_TESTS OFF CACHE INTERNAL "" FORCE)
set(LUAU_BUILD_BENCHMARKS OFF CACHE INTERNAL "" FORCE)
set(LUAU_BUILD_EXAMPLES OFF CACHE INTERNAL "" FORCE)
set(BUILD_TESTING OFF CACHE INTERNAL "" FORCE)
FetchContent_Declare(
  luau
  GIT_REPOSITORY https://github.com/luau-lang/luau.git
  GIT_TAG 0.681
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(luau)

# --- Dear ImGui (Win32 + Direct3D 11 UI) ---
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.9b
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

add_executable(lusted
  src/main.cpp
  src/roblox_finder.cpp
  src/offsets_fetcher.cpp
  src/memory.cpp
  src/instance.cpp
  src/ui.cpp
  src/explorer.cpp
  src/drawing.cpp
  src/luau_manager.cpp
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)

target_include_directories(lusted PRIVATE src ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)

target_link_libraries(lusted PRIVATE
  nlohmann_json::nlohmann_json
  Luau.VM
  Luau.Compiler
  winhttp
  version
  psapi
  gdi32
  user32
  comctl32
  d3d11
  dxgi
  d3dcompiler
  dwmapi
  imm32
)

# Luau headers come from its targets; ensure C++20 + MSVC compat
if(MSVC)
  target_compile_options(lusted PRIVATE /W3 /EHsc /O2 /Ot /GL /arch:AVX2 /fp:fast /Oi /DNDEBUG)
  target_link_options(lusted PRIVATE /LTCG /OPT:REF /OPT:ICF)
  set_target_properties(lusted PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
else()
  target_compile_options(lusted PRIVATE -O3 -march=native -DNDEBUG)
  set_target_properties(lusted PROPERTIES INTERPROCEDURAL_OPTIMIZATION FALSE)
endif()

# Runtime offsets are cached under C:/LUSTED/offsets/.
# User Luau scripts are auto-executed from C:/LUSTED/Luas/.
