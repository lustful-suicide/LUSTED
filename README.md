# LUSTED

LUSTED is a Windows C++20 application with a Luau runtime, an ImGui control panel, and a drawing overlay. Luau features are loaded from `C:/LUSTED/Luas`.

## Build

Requirements: Windows, CMake 3.20+, and a C++20 toolchain. The first CMake configure fetches Luau, nlohmann/json, and Dear ImGui.

```powershell
cmake -S . -B build
cmake --build build --config Release --parallel 2
```

The executable is `build/lusted.exe`. `--help` shows command-line options; `--once` runs startup and auto-execution, then exits.

## Source map

| Path | Responsibility |
| --- | --- |
| `src/app` | Startup, shutdown, and top-level wiring |
| `src/platform/windows` | Windows process-memory adapter |
| `src/roblox` | Process discovery, version data, instance index, and properties |
| `src/runtime` | Luau VM, native bindings, and embedded standard library |
| `src/ui` | ImGui controls, instance explorer, and drawing overlay |
| `src/support` | Shared logging |

The matching CMake targets are `lusted_process`, `lusted_roblox`, `lusted_ui`, `lusted_runtime`, and the `lusted` executable. Read [docs/architecture.mdx](docs/architecture.mdx) before changing module dependencies.

## Scripts and runtime data

User scripts remain in `C:/LUSTED/Luas`; auto-run loads direct `.lua` and `.luau` children in sorted order. Offsets and configurations are stored under `C:/LUSTED`. See [the documentation](docs/index.mdx) for scripting, runtime paths, and troubleshooting.
